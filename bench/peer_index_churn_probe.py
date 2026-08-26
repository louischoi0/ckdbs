#!/usr/bin/env python3
"""The G1 sustained-churn probe: does index churn against a peer-owned
relation leave it unwritable?

The defect this reproduces (`docs/inflight/known-gaps.md`, `docs/inflight/in-progress/workplan-peer-writer.md`
PW1c-8): a peer's free-map copy is a mount-time snapshot advanced only by a
relation fault/write grant, so a **catalog** page core 0 allocates between
grants stays invisible to that peer -- and the fault seam reported that
staleness as absence. `CREATE INDEX`/`DROP INDEX` in a loop from core 0
against a peer-owned relation is the fastest way to make core 0 append a
catalog page while placing no relation on the peer: before the fix it
poisoned the relation at build 58, after which every write answered
`ERR page id not found`, non-retryable, for the rest of the mount.

Pass criteria, all three:

  builds        == the requested count (400), no refusal on the way
  poisoned_at   is None (the interleaved write probe never refused)
  insert_after  succeeded

`cores` is a parameter because the defect is not core-count dependent: it
reproduced identically at `cores = 4` (the pretask host) and `cores = 2`
(the 2-CPU host this was fixed on). Any instance with one peer reaches it.

    python3 bench/peer_index_churn_probe.py [--cores N] [--builds N] [--port N]

Note `placement = rotate` skips the system core, so on a two-core instance
every relation is the peer's -- which is why a churn that *creates
relations* heals what it breaks and does not reproduce this at all.
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


def insert_settles(conn, sql, tries=20, pause=0.05):
    """Whether `sql` succeeds, allowing for a *retryable* refusal.

    The defect this probe exists for is non-retryable and permanent. The
    engine's own correct behaviour includes transient `TXN_CONFLICT
    retryable=1` refusals - a lease being refilled, write rights still in
    flight - and counting one of those as poisoning would fail the probe on
    a healthy instance.
    """
    last = ''
    for _ in range(tries):
        last = conn.cmd(sql)
        if not last.startswith('ERR'):
            return True, last
        if 'retryable=1' not in last:
            return False, last
        time.sleep(pause)
    return False, last


def run(cores, port, workdir, builds, rows):
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
    with open(err_path, 'w') as err:
        proc = subprocess.Popen([os.path.join(ROOT, 'build-release/kds_server'),
                                 '--config', conf], stdout=err, stderr=subprocess.STDOUT)
    out = {'cores': cores, 'builds_requested': builds, 'rows': rows}
    spare = []
    stopped = False
    try:
        wait_for_port(port, err_path)
        ddl = session_on(port, 0, spare)
        created = ddl.cmd('CREATE TABLE t (id int64, owner varchar, balance int64) BTREE')
        if created.startswith('ERR'):
            raise RuntimeError(created)
        owner = int([x for x in ddl.cmd('DESCRIBE t').split()
                     if x.startswith('owner_core=')][0].split('=')[1])
        out['owner_core'] = owner
        # With no peer there is no lease, no mount-time map snapshot, and so
        # no defect to find: `--cores 1` measures the core-0 anchor ceiling
        # instead, which is a different and *correct* refusal. Say so rather
        # than printing a verdict about the wrong thing.
        out['vacuous'] = owner == 0
        writer = ddl if owner == 0 else session_on(port, owner, spare)

        for _ in range(300):
            if not writer.cmd("INSERT INTO t VALUES ('warm', 0)").startswith('ERR'):
                break
            time.sleep(0.01)
        writer.cmd('BEGIN')
        for i in range(rows):
            reply = writer.cmd(f"INSERT INTO t VALUES ('x', {i})")
            if reply.startswith('ERR'):
                out['load_error'] = reply[:200]
                break
        writer.cmd('COMMIT')

        made = 0
        poisoned_at = None
        first_error = None
        for i in range(builds):
            reply = ddl.cmd(f'CREATE INDEX ix{i} ON t (owner)')
            if reply.startswith('ERR'):
                first_error = reply[:200]
                break
            made += 1
            ddl.cmd(f'DROP INDEX ix{i}')
            # The interleaved write probe: the defect shows here, not in
            # the build loop, and it shows as a write that never recovers.
            if made % 10 == 0:
                ok, probe = insert_settles(writer, f"INSERT INTO t VALUES ('probe', {made})")
                if not ok:
                    poisoned_at = (made, probe[:200])
                    break
        out['builds'] = made
        out['first_error'] = first_error
        out['poisoned_at'] = poisoned_at
        # Read before STOP, so it does not count the shutdown flush: a
        # diagnostic, never the criterion.
        out['file_pages'] = os.path.getsize(db) // 8192
        settled, after = insert_settles(writer, "INSERT INTO t VALUES ('after', 1)")
        out['insert_after'] = after[:200]
        out['insert_after_ok'] = settled
        ddl.cmd('STOP')
        stopped = True
    finally:
        if stopped:
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(timeout=10)
        else:
            # Nothing sent STOP, so waiting for a clean exit would only hold
            # the port open through an error path.
            proc.terminate()
            proc.wait(timeout=10)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', type=int, default=2)
    ap.add_argument('--builds', type=int, default=400)
    ap.add_argument('--rows', type=int, default=2000)
    ap.add_argument('--port', type=int, default=22600)
    ap.add_argument('--workdir', default='/tmp/kds-peer-index-churn')
    args = ap.parse_args()

    result = run(args.cores, args.port, args.workdir, args.builds, args.rows)
    for key, value in result.items():
        print(f'{key} = {value}')
    if result.get('vacuous'):
        print('VACUOUS: the relation landed on core 0, so there is no peer and no '
              'snapshot to go stale; re-run with more cores')
        return 2
    passed = (result.get('builds') == args.builds and result.get('poisoned_at') is None
              and result.get('insert_after_ok'))
    print('PASS' if passed else 'FAIL')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
