#!/usr/bin/env python3
"""A7: kill -9 mid-burst on a shipped write path, and what survives.

A shipped statement executes and commits on its **owner**, not on the core
its client is connected to. SS3 claims its redo therefore lives wholly in
the owner's WAL stream. This probe kills the instance mid-burst and checks
what a restart recovers -- rather than taking that claim on the header's
word.

Both sides of the wire go down together, because they are threads of one
process: what varies is *when*, so the burst runs continuously and the kill
lands wherever it lands.

What must hold, and why each is the criterion it is:

  every acked row is present     -- an ack is issued only after the owner's
                                    commit is durable (`DispatchAsync` parks
                                    on `IsDurable`), so an acked statement
                                    the restart cannot find is lost data.
  rows <= acked + 1              -- the client is synchronous, so at most
                                    one statement was in flight at the kill.
                                    A larger surplus means a statement ran
                                    that nobody asked for.
  owner redo >> arrival redo     -- SS3's claim, read off the two cores'
                                    `recovery_redo_applied`.
  the relation is writable after -- recovery left nothing half-held.

    python3 bench/shipped_kill_recovery_probe.py [--cores N] [--kill-after S]
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
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
    for _ in range(256):
        candidate = Conn(port)
        if core_of(candidate) == core:
            return candidate
        spare.append(candidate)
    raise RuntimeError(f'no session landed on core {core}')


def start_server(conf, err_path):
    err = open(err_path, 'a')
    return subprocess.Popen([os.path.join(ROOT, 'build-release/kds_server'),
                             '--config', conf], stdout=err, stderr=subprocess.STDOUT)


def count_rows(conn, table):
    reply = conn.cmd(f'SELECT COUNT(*) FROM {table}')
    if reply.startswith('ERR'):
        return None, reply[:200]
    for part in reversed(reply.replace('\\n', '\n').split('\n')):
        if part.strip().isdigit():
            return int(part.strip()), reply[:200]
    return None, reply[:200]


def run(cores, port, workdir, kill_after, max_rows):
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, 's.conf')
    db = os.path.join(workdir, 's.db')
    with open(conf, 'w') as f:
        f.write(f"data_file = {db}\nport = {port}\ncores = {cores}\n"
                f"placement = rotate\npeer_listeners = on\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = error\n")
    err_path = os.path.join(workdir, 's.stderr')
    proc = start_server(conf, err_path)

    out = {'cores': cores, 'kill_after_s': kill_after}
    spare = []
    try:
        wait_for_port(port, err_path)
        client = session_on(port, 0, spare)
        created = client.cmd('CREATE TABLE t (id int64, tag varchar, n int64) BTREE')
        if created.startswith('ERR'):
            raise RuntimeError(created)
        owner = int([x for x in client.cmd('DESCRIBE t').split()
                     if x.startswith('owner_core=')][0].split('=')[1])
        out['owner_core'] = owner
        out['vacuous'] = owner == 0
        if owner == 0:
            proc.terminate()
            proc.wait(timeout=10)
            return out

        # The burst, from core 0 so every statement ships. A timer kills the
        # instance underneath it.
        killer = threading.Timer(kill_after, proc.kill)
        killer.start()
        acked = 0
        last_error = None
        try:
            for i in range(max_rows):
                reply = client.cmd(f"INSERT INTO t VALUES ('burst', {i})")
                if reply.startswith('ERR'):
                    # Retryable refusals are the engine working; a permanent
                    # one ends the burst and is reported.
                    if 'retryable=1' not in reply:
                        last_error = reply[:200]
                        break
                    time.sleep(0.02)
                    continue
                acked += 1
        except (ConnectionError, OSError) as e:
            last_error = f'{type(e).__name__}: {e}'
        killer.cancel()
        proc.wait(timeout=30)
        out['acked'] = acked
        out['burst_ended_with'] = last_error
        out['killed'] = proc.returncode
        if acked == 0:
            out['error'] = 'nothing was acked before the kill; raise --kill-after'
            return out

        # ---- The restart -------------------------------------------------
        for conn in spare:
            conn.close()
        spare = []
        proc = start_server(conf, err_path)
        wait_for_port(port, err_path)
        after = session_on(port, 0, spare)
        rows, raw = count_rows(after, 't')
        out['rows_after_restart'] = rows
        out['count_reply'] = raw
        out['surplus'] = None if rows is None else rows - acked

        # Every acked row present: the last acked n is the one a lost tail
        # would drop first.
        probe = after.cmd(f"SELECT n FROM t WHERE n = {acked - 1}")
        out['last_acked_present'] = (not probe.startswith('ERR')
                                     and str(acked - 1) in probe)

        # SS3's claim, as two numbers. `recovery_redo_applied` is per core.
        out['redo_core0'] = meta_field(after, 'recovery_redo_applied')
        owner_conn = session_on(port, owner, spare)
        out['redo_owner'] = meta_field(owner_conn, 'recovery_redo_applied')
        out['shipped_running_owner'] = meta_field(owner_conn, 'shipped_running')

        writable = after.cmd("INSERT INTO t VALUES ('after', -1)")
        for _ in range(20):
            if not writable.startswith('ERR') or 'retryable=1' not in writable:
                break
            time.sleep(0.05)
            writable = after.cmd("INSERT INTO t VALUES ('after', -1)")
        out['writable_after'] = not writable.startswith('ERR')
        out['writable_reply'] = writable[:200]
        after.cmd('STOP')
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', type=int, default=4)
    ap.add_argument('--kill-after', type=float, default=3.0)
    ap.add_argument('--max-rows', type=int, default=100000)
    ap.add_argument('--port', type=int, default=22650)
    ap.add_argument('--workdir', default='/tmp/kds-shipped-kill')
    args = ap.parse_args()

    result = run(args.cores, args.port, args.workdir, args.kill_after, args.max_rows)
    for key, value in result.items():
        print(f'{key} = {value}')
    if result.get('vacuous'):
        print('VACUOUS: the relation landed on core 0, so nothing shipped')
        return 2
    if 'error' in result:
        print('FAIL')
        return 1
    surplus = result.get('surplus')
    redo_owner = int(result.get('redo_owner') or 0)
    redo_core0 = int(result.get('redo_core0') or 0)
    passed = (result.get('last_acked_present')
              and surplus is not None and 0 <= surplus <= 1
              and result.get('writable_after')
              and redo_owner > redo_core0)
    print('PASS' if passed else 'FAIL')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
