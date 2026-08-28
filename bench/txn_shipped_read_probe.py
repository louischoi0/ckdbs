#!/usr/bin/env python3
"""R6-R cells R1-R2: what a cross-owner *read* costs.

`instructions/v2.5.0/cross-owner-protocol-closing.md` §7 (RR2, R1/R2). RR1
widened the read site (`HandleSelect`, `command_dispatcher.cpp:6058`) to
test `MayShip(session) || MayEnrolShip(session)` - a foreign read inside an
explicit transaction now ships and enrols instead of being refused, so a
transaction that reads before it writes can finally reach the cross-owner
commit path (RP8's B5 blocker). This driver prices what that costs.

Three arms, one point-lookup shape (`SELECT * FROM rt WHERE id = <i>`, a
BTREE relation on a peer core):

  **autocommit**  no `BEGIN`. `MayEnrolShip` is false (not in an explicit
                  transaction), so `HandleSelect`'s single-step remote-read
                  pipeline answers it exactly as it did before RR1 - the
                  baseline HR1 is stated against.
  **rc**          `BEGIN` (session default, READ COMMITTED); `SELECT`;
                  `COMMIT`. The read now enrols and ships (RR1); D3's
                  ratified `[OPEN]` has RC carry no watermark, so the read
                  itself should cost the autocommit path's own cost plus
                  one branch (`MayEnrolShip`) - HR1's claim, CR1's question.
                  The `COMMIT` pays a cross-owner decide even though nothing
                  was written, because the read alone enrolled a participant
                  (`EnrolParticipant`, `command_dispatcher.cpp:3953`, and the
                  D1 fast path comment at `:7143-7148`: "a transaction that
                  ships every statement to one peer has one participant and
                  still runs the full protocol").
  **rr**          Identical to `rc` except `BEGIN ISOLATION LEVEL RR`. The
                  participant now carries and repeats a watermark
                  (RR0/D3) on every reply. R2's question: what that costs
                  against `rc`, same shape, same run.

Every leg is timed separately (`begin_us`/`select_us`/`commit_us`/
`total_us`) so CR1 can be answered from `select_us` alone - the read
statement's own cost - without the 2PC commit's durability wait folded in,
and `total_us` still reports the whole unit's cost for context (the "a read
transaction now pays a full participant's commit" fact `bench/docs/README.md`
should also be able to point to).

**Every reply is content-checked**, not merely absence-of-ERR: the returned
row's `id`/`n` are compared against what was seeded, so a wrong answer -
RR1's whole reason for existing was a wrong answer on an enrolled read -
fails the run rather than passing silently. `rows_match` in the JSON is the
fraction of reads that returned the exact seeded row.

**Rule 9's sweep.** `--rows` sets how many distinct pk rows are seeded and
cycled through; the point-lookup shape is not expected to scale with it
(a btree descent is the same depth at 200, 1K or 10K rows for the row
counts this table holds), and the run at three sizes is the evidence for
that rather than an assumption of it.

    python3 bench/txn_shipped_read_probe.py --arm rc --rows 1000 \
        --reps 5 --txns 200 --server build-release/kds_server --json out.json

One arm per invocation, for the same reason `txn_2pc_cost_probe.py` gives:
an arm sharing a process with another shares its page cache, its trx-id
lease state and its WAL segment.
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
                # Off, deliberately (per RP8's B5 finding on this same
                # shape): every connection, DDL and probe alike, then lands
                # on core 0, which `rotate` never assigns table ownership
                # to - so a single client connection is coordinator for
                # every read without a peer-listener retry loop.
                f"peer_listeners = off\n"
                f"durability = group\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = error\n")
    return conf


def build_relation(c0):
    created = c0.cmd('CREATE TABLE rt (id int64, tag varchar, n int64) BTREE')
    if created.startswith('ERR'):
        raise RuntimeError(f'CREATE rt: {created[:200]}')
    return field(c0.cmd('DESCRIBE rt'), 'owner_core')


def seed_rows(c0, rows):
    """The id is **omitted**, not caller-supplied: a peer-owned relation
    refuses a caller-supplied pk from a shipped write
    (`workplan-peer-writer.md` §7a - admitting one writes the relation's
    catalog row, the system core's page), so the engine issues it and the
    reply names it (`field(reply, 'id')`). Returns the issued ids in
    seed order, which is what every later point lookup indexes by - `rt`'s
    ids are **not** 1..rows, and nothing below may assume they are."""
    ids = []
    for i in range(1, rows + 1):
        r = None
        for _ in range(200):
            r = c0.cmd(f"INSERT INTO rt VALUES ('seed{i}', {i * 7})")
            # A peer's row-id lease refill (`workplan-peer-writer.md`
            # territory, RP8 §8 named it orthogonal to CP3) is retryable
            # and unrelated to what this driver measures - seeding retries
            # through it rather than failing the run on it.
            if not (r.startswith('ERR') and 'retryable=1' in r):
                break
            time.sleep(0.01)
        if r.startswith('ERR'):
            raise RuntimeError(f'seed row {i}: {r[:200]}')
        ids.append((field(r, 'id'), i * 7))
    return ids


def parse_one_row(reply):
    """Header line, then at most one data line for a point lookup - the
    wire's one-line-per-response contract with '\\n'-escaped row
    separators (`command_dispatcher.cpp`'s `HandleSelect` comment)."""
    parts = reply.replace('\\n', '\n').split('\n')
    if len(parts) < 2 or not parts[1]:
        return None
    header = parts[0].split(',')
    row = parts[1].split(',')
    if len(header) != len(row):
        return None
    return dict(zip(header, row))


def row_matches(reply, pk, expected_n):
    row = parse_one_row(reply)
    if row is None:
        return False
    try:
        return int(row.get('id', -1)) == pk and int(row.get('n', -1)) == expected_n
    except ValueError:
        return False


def read_autocommit(conn, pk, tries=40):
    last = None
    for _ in range(tries):
        start = time.perf_counter()
        r = conn.cmd(f"SELECT * FROM rt WHERE id = {pk}")
        us = (time.perf_counter() - start) * 1e6
        if not r.startswith('ERR'):
            return {'total_us': us, 'select_us': us}, r
        last = r
        time.sleep(0.02)
    return None, last


def read_in_txn(conn, pk, level, tries=40):
    """BEGIN [ISOLATION LEVEL <level>]; SELECT; COMMIT, timed leg by leg.
    A retryable failure on any leg rolls back and retries the whole unit -
    the R6-8 review's rule (`txn_2pc_cost_probe.py`'s `run_txn` docstring),
    because a retry inside an explicit transaction is a fresh BEGIN."""
    last = None
    for _ in range(tries):
        begin_line = 'BEGIN' if level is None else f'BEGIN ISOLATION LEVEL {level}'
        t0 = time.perf_counter()
        r0 = conn.cmd(begin_line)
        t1 = time.perf_counter()
        if r0.startswith('ERR'):
            last = r0
            time.sleep(0.02)
            continue
        r1 = conn.cmd(f"SELECT * FROM rt WHERE id = {pk}")
        t2 = time.perf_counter()
        if r1.startswith('ERR'):
            conn.cmd('ROLLBACK')
            last = r1
            time.sleep(0.02)
            continue
        r2 = conn.cmd('COMMIT')
        t3 = time.perf_counter()
        if r2.startswith('ERR'):
            conn.cmd('ROLLBACK')
            last = r2
            time.sleep(0.02)
            continue
        return {
            'begin_us': (t1 - t0) * 1e6,
            'select_us': (t2 - t1) * 1e6,
            'commit_us': (t3 - t2) * 1e6,
            'total_us': (t3 - t0) * 1e6,
        }, r1
    return None, last


def run_arm(args):
    workdir = os.path.join(args.workdir, f'{args.arm}-{args.rows}')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)

    cores = 2
    placement = 'rotate'
    conf = write_conf(workdir, args.port, cores, placement)
    out = {'arm': args.arm, 'rows': args.rows, 'cores': cores, 'placement': placement,
           'reps': args.reps, 'txns_per_rep': args.txns}

    err = open(os.path.join(workdir, 's.stderr'), 'a')
    proc = subprocess.Popen([args.server, '--config', conf], stdout=err,
                            stderr=subprocess.STDOUT)
    spare = []
    try:
        wait_for_port(args.port, os.path.join(workdir, 's.stderr'))
        c0 = Conn(args.port)
        owner_core = build_relation(c0)
        out['owner_core'] = owner_core
        seeded = seed_rows(c0, args.rows)  # [(pk, expected_n), ...], seed order

        level = None if args.arm != 'rr' else 'RR'

        # Warm-up, outside every measurement: the relation's first page
        # allocation and the first shipped statement's dedup-table entry,
        # neither of which is what R1/R2 are about.
        for w in range(args.warmup):
            pk, _ = seeded[w % args.rows]
            if args.arm == 'autocommit':
                read_autocommit(c0, pk)
            else:
                read_in_txn(c0, pk, level)

        reps, matched, refusals = [], 0, 0
        total_reads = 0
        for rep in range(args.reps):
            legs = {'begin_us': [], 'select_us': [], 'commit_us': [], 'total_us': []}
            for t in range(args.txns):
                pk, expected_n = seeded[t % args.rows]
                if args.arm == 'autocommit':
                    timing, reply = read_autocommit(c0, pk)
                else:
                    timing, reply = read_in_txn(c0, pk, level)
                total_reads += 1
                if timing is None:
                    refusals += 1
                    out.setdefault('last_refusal', reply)
                    continue
                for k, v in timing.items():
                    legs[k].append(v)
                if row_matches(reply, pk, expected_n):
                    matched += 1
                else:
                    out.setdefault('first_mismatch', {'pk': pk, 'reply': reply})
            reps.append({k: percentiles(v) for k, v in legs.items() if v})
        out['per_rep'] = reps
        out['total_reads'] = total_reads
        out['matched'] = matched
        out['refused'] = refusals
        out['rows_match'] = (matched == total_reads)

        c0.cmd('STOP')
        try:
            proc.wait(timeout=60)
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
    ap.add_argument('--arm', choices=('autocommit', 'rc', 'rr'), required=True)
    ap.add_argument('--rows', type=int, default=1000)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--txns', type=int, default=200)
    ap.add_argument('--warmup', type=int, default=20)
    ap.add_argument('--port', type=int, default=22950)
    ap.add_argument('--server', default=os.path.join(ROOT, 'build-release/kds_server'))
    ap.add_argument('--workdir', default=os.path.expanduser('~/kds-rr-read'))
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    result = run_arm(args)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(result, f, indent=2)
    print(json.dumps(result, indent=2))
    if not result.get('rows_match'):
        print('FAIL: a read answered a row other than the one seeded', file=sys.stderr)
        return 1
    if result.get('refused'):
        print(f"NOTE: {result['refused']} read(s) refused", file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
