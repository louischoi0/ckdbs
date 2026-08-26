#!/usr/bin/env python3
"""A6: does sustained *shipped DML* leave a peer-owned relation unwritable?

The G1 defect (`docs/inflight/known-gaps.md`, `docs/inflight/in-progress/workplan-peer-writer.md` PW1c-8)
appeared after ~58 shipped `CREATE INDEX`es on a peer-owned relation: a
catalog page core 0 allocated between grants stayed invisible to the peer,
and from then on every write to that relation answered `ERR page id not
found`, non-retryable, for the rest of the mount. `peer_index_churn_probe.py`
is that probe, and it churns **DDL**.

Statement shipping makes DML the high-volume traveller on the same wiring:
where a shipped `CREATE INDEX` was rare, a shipped `INSERT` runs thousands
of times more often. This probe therefore drives the DML form -- every
statement issued from a **core-0 session against a peer-owned relation**, so
each one is shipped (SS2's fork) rather than executed locally -- and asks
the same question G1 asked: does the relation stay writable, throughout and
across a restart?

Pass criteria, all of them:

  shipped               > 0            (the statements really crossed a ring)
  poisoned_at           is None        (the interleaved probe never refused
                                        non-retryably)
  rows_before_restart   == accepted    (rows in = rows out)
  rows_after_restart    == accepted    (the restart lost nothing)
  insert_after_restart  succeeded

    python3 bench/shipped_dml_churn_probe.py [--cores N] [--iterations N] [--port N]

`--cores 1` is vacuous by construction: with one core nothing is foreign,
the fork short-circuits, and no statement ships. The probe says so rather
than printing a verdict about a workload it did not run.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from multicore_benchmark import Conn, wait_for_port  # noqa: E402


def core_of(conn):
    for token in conn.cmd('SHOW META').split():
        if token.startswith('core='):
            return int(token.split('=')[1])
    return None


def meta_field(conn, key):
    for token in conn.cmd('SHOW META').split():
        if token.startswith(key + '='):
            return token.split('=', 1)[1]
    return None


def session_on(port, core, spare):
    # Non-matching connections are kept open rather than closed: a closed
    # session's core goes straight back to the next connection, so dropping
    # them would retry the same core forever.
    for _ in range(256):
        candidate = Conn(port)
        if core_of(candidate) == core:
            return candidate
        spare.append(candidate)
    raise RuntimeError(f'no session landed on core {core}')


def settles(conn, sql, tries=20, pause=0.05):
    """Whether `sql` succeeds, allowing for a *retryable* refusal.

    Returns (ok, reply, retries) -- the retry count is reported rather than
    swallowed, because a healthy instance's transient refusals are the
    population this probe must not confuse with the permanent one.

    The defect this probe exists for is non-retryable and permanent. A
    healthy instance still refuses transiently -- a row-id lease being
    refilled, write rights in flight -- and counting one of those as
    poisoning would fail the probe on a working engine.
    """
    last = ''
    for attempt in range(tries):
        last = conn.cmd(sql)
        if not last.startswith('ERR'):
            return True, last, attempt
        if 'retryable=1' not in last:
            return False, last, attempt
        time.sleep(pause)
    return False, last, tries


def start_server(workdir, conf, err_path):
    err = open(err_path, 'a')
    return subprocess.Popen([os.path.join(ROOT, 'build-release/kds_server'),
                             '--config', conf], stdout=err, stderr=subprocess.STDOUT)


def count_rows(conn, table):
    reply = conn.cmd(f'SELECT COUNT(*) FROM {table}')
    if reply.startswith('ERR'):
        return None, reply[:200]
    # "COUNT(*)\n<n>" -- the header line, then the one row.
    parts = reply.replace('\\n', '\n').split('\n')
    for part in reversed(parts):
        token = part.strip()
        if token.isdigit():
            return int(token), reply[:200]
    return None, reply[:200]


def run(cores, port, workdir, iterations, probe_every):
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, 's.conf')
    db = os.path.join(workdir, 's.db')
    with open(conf, 'w') as f:
        f.write(f"data_file = {db}\nport = {port}\ncores = {cores}\n"
                f"placement = {'creating' if cores == 1 else 'rotate'}\n"
                f"peer_listeners = {'off' if cores == 1 else 'on'}\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = error\n")
    err_path = os.path.join(workdir, 's.stderr')
    proc = start_server(workdir, conf, err_path)

    out = {'cores': cores, 'iterations_requested': iterations}
    spare = []
    stopped = False
    try:
        wait_for_port(port, err_path)
        # **Every statement below is issued from core 0**, which is what
        # makes it shipped: the relation rotates onto a peer, and core 0's
        # fork carries the statement to that peer rather than refusing it.
        client = session_on(port, 0, spare)
        created = client.cmd('CREATE TABLE t (id int64, owner varchar, balance int64) BTREE')
        if created.startswith('ERR'):
            raise RuntimeError(created)
        owner = int([x for x in client.cmd('DESCRIBE t').split()
                     if x.startswith('owner_core=')][0].split('=')[1])
        out['owner_core'] = owner
        out['vacuous'] = owner == 0

        accepted = 0
        deleted = 0
        poisoned_at = None
        retryable_refusals = 0
        for i in range(iterations):
            ok, reply, retries = settles(client, f"INSERT INTO t VALUES ('churn', {i})")
            if not ok:
                poisoned_at = (i, reply[:200])
                break
            accepted += 1
            retryable_refusals += retries
            # The three write verbs, so the churn is DML-shaped rather than
            # an insert loop: an UPDATE and a DELETE ship through the same
            # fork and take the same owner-side path.
            if i % 7 == 3:
                ok, reply, retries = settles(
                    client, f"UPDATE t SET owner = 'moved' WHERE balance = {i}")
                if not ok:
                    poisoned_at = (i, reply[:200])
                    break
                retryable_refusals += retries
            if i % 11 == 5:
                ok, reply, retries = settles(client, f"DELETE FROM t WHERE balance = {i}")
                if not ok:
                    poisoned_at = (i, reply[:200])
                    break
                retryable_refusals += retries
                deleted += 1
            if probe_every and (i + 1) % probe_every == 0:
                # The interleaved probe: G1 showed here, as a write that
                # never recovered.
                ok, reply, retries = settles(client, f"INSERT INTO t VALUES ('probe', {-(i + 1)})")
                if not ok:
                    poisoned_at = (i, reply[:200])
                    break
                retryable_refusals += retries
                accepted += 1

        out['poisoned_at'] = poisoned_at
        out['accepted'] = accepted - deleted
        out['retryable_refusals'] = retryable_refusals
        # Proof the statements crossed a ring rather than running locally.
        out['shipped'] = meta_field(client, 'shipped_statements')
        out['shipped_refusals'] = meta_field(client, 'shipped_refusals')
        out['shipped_identity_mismatches'] = meta_field(client, 'shipped_identity_mismatches')
        out['shipped_late_executed'] = meta_field(client, 'shipped_late_executed')
        out['cross_core_write_refusals'] = meta_field(client, 'cross_core_write_refusals')

        rows, raw = count_rows(client, 't')
        out['rows_before_restart'] = rows
        out['count_reply'] = raw

        client.cmd('STOP')
        stopped = True
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=10)

        # ---- The restart half ------------------------------------------
        # G1's failure outlived nothing (it was a mount-local staleness),
        # but a churn that corrupted the relation's pages or its free map
        # would show here and not before.
        for conn in spare:
            try:
                conn.close()
            except Exception:
                pass
        spare = []
        proc = start_server(workdir, conf, err_path)
        stopped = False
        wait_for_port(port, err_path)
        after = session_on(port, 0, spare)
        rows_after, raw_after = count_rows(after, 't')
        out['rows_after_restart'] = rows_after
        out['count_reply_after'] = raw_after
        ok, reply, retries = settles(after, "INSERT INTO t VALUES ('after', -1)")
        out['insert_after_restart_ok'] = ok
        out['insert_after_restart'] = reply[:200]
        out['file_pages'] = os.path.getsize(db) // 8192
        after.cmd('STOP')
        stopped = True
    finally:
        if stopped:
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(timeout=10)
        else:
            proc.terminate()
            proc.wait(timeout=10)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', type=int, default=4)
    ap.add_argument('--iterations', type=int, default=2000)
    ap.add_argument('--probe-every', type=int, default=50)
    ap.add_argument('--port', type=int, default=22640)
    ap.add_argument('--workdir', default='/tmp/kds-shipped-dml-churn')
    args = ap.parse_args()

    result = run(args.cores, args.port, args.workdir, args.iterations, args.probe_every)
    for key, value in result.items():
        print(f'{key} = {value}')
    if result.get('vacuous'):
        print('VACUOUS: the relation landed on core 0, so nothing shipped; re-run with '
              'more cores')
        return 2
    accepted = result.get('accepted')
    passed = (result.get('poisoned_at') is None
              and (result.get('shipped') or '0') != '0'
              and result.get('rows_before_restart') == accepted
              and result.get('rows_after_restart') == accepted
              and result.get('insert_after_restart_ok'))
    print('PASS' if passed else 'FAIL')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
