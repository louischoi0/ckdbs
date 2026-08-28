#!/usr/bin/env python3
"""RP8 cells B1-B3: what the two-phase commit costs, and whether the
one-owner path still costs what it did (`instructions/v2.4.0/2pc.md` §6).

D7 stated the prediction before anything was built: *"A cross-owner commit
costs **at least two durable syncs in sequence** (prepare, then decide),
where a single-core commit costs one... a two-owner transaction should cost
roughly twice a one-owner one, plus two ring round trips at ~20 µs each"*,
and *"the expected shape is two sync latencies deep, however many
participants wide, up to four"*.

Three arms, and each is a different question:

  **local**       one transaction, session on the core that owns the
                  relations it writes. No participants, so `HandleCommit`
                  never reaches `PrepareAcrossOwners` - D1's fast path, and
                  B3's unit of comparison.
  **xowner-N**    one transaction from a core-0 session writing N relations
                  owned by N different peers. N participants, both phases.
  **split-N**     the same N rows, written by N separate one-owner
                  transactions, each from a session on its own owner core.
                  B1's "the same work as two separate one-owner
                  transactions".

**Every arm writes the same rows.** That is what makes the ratios mean
something: `xowner-2` and `split-2` differ only in how many transactions the
N inserts are wrapped in, and `local` differs from `split-1` only in that
`local` writes N rows to one owner where `split-1` writes one.

**p50 and p99, never a single ratio** — §2's obligation on B1, because M3
found shipping's cost in the tail (+11% p50 against +76% p99), so a 2×
median alone would record D7 as confirmed while an unpredicted tail passed
unnoticed. The full table carries p0/p25/p50/p95/p99, which is
`.claude/agents/ck-tester.md` rule 1.

Method, per the order's §7: fresh server and data file per invocation, one
process per arm, per-rep spreads reported before any median, rows in = rows
out checked by `COUNT(*)` at the end of every arm.

    python3 bench/txn_2pc_cost_probe.py --arm xowner --participants 2 \
        --reps 5 --txns 200 --server build-release/kds_server --json out.json

One arm per invocation on purpose: the interleaving is the runner's job
(`bench/run_2pc_cost.sh`), because an arm that shares a process with another
arm shares its page cache, its trx-id lease state and its WAL segment.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from multicore_benchmark import Conn, field, wait_for_port  # noqa: E402


def percentiles(values):
    """p0/p25/p50/p95/p99 over one rep's latencies, in microseconds."""
    if not values:
        return {}
    s = sorted(values)

    def at(p):
        return s[min(len(s) - 1, int(len(s) * p))]

    return {'p0': round(s[0], 1), 'p25': round(at(0.25), 1),
            'p50': round(at(0.50), 1), 'p95': round(at(0.95), 1),
            'p99': round(at(0.99), 1), 'n': len(s),
            'mean': round(sum(s) / len(s), 1)}


def write_conf(workdir, port, cores, placement):
    conf = os.path.join(workdir, 's.conf')
    with open(conf, 'w') as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                f"placement = {placement}\n"
                f"peer_listeners = {'on' if placement == 'rotate' and cores > 1 else 'off'}\n"
                # **`group`, the default, and not `relaxed`.** D7's whole
                # claim is about durable syncs in sequence; measuring under a
                # class that does not wait for one would answer a different
                # question.
                f"durability = group\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = error\n")
    return conf


def session_on_core(port, core, spare, tries=192):
    for _ in range(tries):
        c = Conn(port)
        if field(c.cmd('SHOW META'), 'core') == core:
            return c
        spare.append(c)
    raise RuntimeError(f'no session landed on core {core}')


def run_txn(conn, statements, tries=40):
    """One transaction, retried as a unit and timed only on the attempt that
    committed. A retryable refusal inside an explicit transaction poisons it
    (the R6-8 review's rule), so a retry is a fresh BEGIN - and a timing that
    included the poisoned attempt would be measuring the refusal, not the
    protocol."""
    for _ in range(tries):
        start = time.perf_counter()
        r = conn.cmd('BEGIN')
        if r.startswith('ERR'):
            time.sleep(0.02)
            continue
        failed = None
        for s in statements:
            r = conn.cmd(s)
            if r.startswith('ERR'):
                failed = r
                break
        if failed is None:
            r = conn.cmd('COMMIT')
            elapsed_us = (time.perf_counter() - start) * 1e6
            if not r.startswith('ERR'):
                return elapsed_us, None
            failed = r
        conn.cmd('ROLLBACK')
        time.sleep(0.02)
    return None, failed


def build_relations(c0, count):
    """`count` relations, and where each landed. Under `rotate` none is on
    core 0 (`catalog/core_placement.hpp`), which is what leaves the client's
    core free to be a coordinator that owns nothing."""
    owners = {}
    for i in range(count):
        name = f't{i}'
        created = c0.cmd(f'CREATE TABLE {name} (id int64, tag varchar, n int64) BTREE')
        if created.startswith('ERR'):
            raise RuntimeError(f'CREATE {name}: {created[:200]}')
        owners[name] = field(c0.cmd(f'DESCRIBE {name}'), 'owner_core')
    return owners


def one_per_owner(owners, participants):
    """One relation per distinct owner core, `participants` of them - so a
    transaction over the result has exactly that many participants and not
    fewer through two relations sharing an owner."""
    by_core = {}
    for name, core in owners.items():
        by_core.setdefault(core, []).append(name)
    chosen = [names[0] for _, names in sorted(by_core.items())]
    if len(chosen) < participants:
        raise RuntimeError(f'only {len(chosen)} distinct owners: {owners}')
    return chosen[:participants]


def count_rows(conn, table):
    reply = conn.cmd(f'SELECT COUNT(*) FROM {table}')
    for part in reversed(reply.replace('\\n', '\n').split('\n')):
        if part.strip().isdigit():
            return int(part.strip())
    return None


def run_arm(args):
    workdir = os.path.join(args.workdir, f'{args.arm}-{args.participants}')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)

    # `local` needs every relation on the client's own core, which only
    # `creating` placement gives; the other two need them on peers.
    placement = 'creating' if args.arm == 'local' else 'rotate'
    cores = 1 if args.arm == 'local' else args.participants + 1
    conf = write_conf(workdir, args.port, cores, placement)
    out = {'arm': args.arm, 'participants': args.participants, 'cores': cores,
           'placement': placement, 'reps': args.reps, 'txns_per_rep': args.txns}

    err = open(os.path.join(workdir, 's.stderr'), 'a')
    proc = subprocess.Popen([args.server, '--config', conf], stdout=err,
                            stderr=subprocess.STDOUT)
    spare = []
    try:
        wait_for_port(args.port, os.path.join(workdir, 's.stderr'))
        c0 = session_on_core(args.port, 0, spare)
        n = args.participants
        owners = build_relations(c0, n if args.arm == 'local' else 4 * n)
        out['owners'] = owners

        if args.arm == 'local':
            tables = sorted(owners)[:n]
            writers = [(c0, tables)]
        else:
            tables = one_per_owner(owners, n)
            if args.arm == 'xowner':
                writers = [(c0, tables)]
            else:  # split: one session per owner core, each writing locally
                writers = [(session_on_core(args.port, owners[t], spare), [t])
                           for t in tables]
        out['tables'] = tables

        # Warm-up, outside every measurement: the first transactions on a
        # fresh instance pay a trx-id lease grant and the relations' first
        # page allocations, neither of which is what D7 is about.
        for _ in range(args.warmup):
            for conn, group in writers:
                run_txn(conn, [f"INSERT INTO {t} VALUES ('warm', 0)" for t in group])

        reps, committed, refusals = [], 0, 0
        for rep in range(args.reps):
            lats = []
            for i in range(args.txns):
                # One *unit of work* is one row per table, however many
                # transactions the arm wraps that in. `split` therefore times
                # the sum of its N transactions, which is what makes it
                # comparable to `xowner`'s one.
                total = 0.0
                ok = True
                for conn, group in writers:
                    us, err_reply = run_txn(
                        conn, [f"INSERT INTO {t} VALUES ('m{rep}', {i})" for t in group])
                    if us is None:
                        refusals += 1
                        ok = False
                        out.setdefault('last_refusal', err_reply)
                        break
                    total += us
                if ok:
                    lats.append(total)
                    committed += 1
            reps.append(percentiles(lats))
        out['per_rep'] = reps
        out['committed_units'] = committed
        out['refused_units'] = refusals

        # **Rows in = rows out.** Every table takes one row per committed
        # unit, plus the warm-ups. A mismatch means a transaction the driver
        # counted did not land, and no percentile below it would be honest.
        expected = committed + args.warmup
        actual = {t: count_rows(c0, t) for t in tables}
        out['rows_expected_each'] = expected
        out['rows_actual'] = actual
        out['rows_match'] = all(v == expected for v in actual.values())

        c0.cmd('STOP')
        try:
            proc.wait(timeout=120)
        except subprocess.TimeoutExpired:
            out['stop_timed_out'] = True
    finally:
        for c in spare:
            c.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arm', choices=('local', 'xowner', 'split'), required=True)
    ap.add_argument('--participants', type=int, default=2)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--txns', type=int, default=200)
    ap.add_argument('--warmup', type=int, default=20)
    ap.add_argument('--port', type=int, default=22900)
    ap.add_argument('--server', default=os.path.join(ROOT, 'build-release/kds_server'))
    ap.add_argument('--workdir', default=os.path.expanduser('~/kds-2pc-cost'))
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    result = run_arm(args)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(result, f, indent=2)
    print(json.dumps({k: v for k, v in result.items() if k != 'owners'}, indent=2))
    if not result.get('rows_match'):
        print('FAIL: rows in != rows out', file=sys.stderr)
        return 1
    if result.get('refused_units'):
        print(f"NOTE: {result['refused_units']} unit(s) refused", file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
