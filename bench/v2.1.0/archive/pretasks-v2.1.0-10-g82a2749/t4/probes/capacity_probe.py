#!/usr/bin/env python3
"""Does the 32 MiB wall belong to the peer, or to the engine?

The T4 probe's index churn drove a `cores = 4` instance into
`ERR page id not found` on every INSERT, with the data file stopped at
exactly 33,554,432 bytes = 4096 pages. Two hypotheses:

  A. the engine cannot grow past that however it is driven, or
  B. only **core 0** can grow the file, so a peer handed page ids past the
     current capacity fails - and fails with a NotFound rather than a
     retryable refusal.

This runs the same churn on `cores = 1` (everything core 0's) and reports the
file size it reaches. If it passes 32 MiB, A is dead and B stands.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, '/home/ubuntu/ckdbs/.claude/worktrees/worktree-v2.2.0-pretasks-stmtshipping/tools')
from multicore_benchmark import Conn, wait_for_port  # noqa: E402

ROOT = '/home/ubuntu/ckdbs/.claude/worktrees/worktree-v2.2.0-pretasks-stmtshipping'


def run(cores, port, workdir, builds=400):
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
    out = {'cores': cores}
    try:
        wait_for_port(port, err_path)
        c = Conn(port)
        # On a peer-listener server the DDL session must be core 0's.
        if cores > 1:
            while True:
                meta = c.cmd('SHOW META')
                if ' core=0 ' in meta or meta.endswith(' core=0'):
                    break
                core = [t for t in meta.split() if t.startswith('core=')][0]
                if core == 'core=0':
                    break
                c.close()
                c = Conn(port)
        r = c.cmd('CREATE TABLE t (id int64, owner varchar, balance int64) BTREE')
        if r.startswith('ERR'):
            raise RuntimeError(r)
        owner = [t for t in c.cmd('DESCRIBE t').split() if t.startswith('owner_core=')][0]
        out['owner'] = owner
        # Rows: enough that an index build allocates real pages.
        writer = c
        if cores > 1:
            # find a session on the owner core
            oc = int(owner.split('=')[1])
            while True:
                w = Conn(port)
                m = w.cmd('SHOW META')
                if f' core={oc}' in m:
                    writer = w
                    break
                w.close()
        for attempt in range(200):
            rr = writer.cmd("INSERT INTO t VALUES ('warm', 0)")
            if not rr.startswith('ERR'):
                break
            time.sleep(0.01)
        writer.cmd('BEGIN')
        for i in range(2000):
            rr = writer.cmd(f"INSERT INTO t VALUES ('x', {i})")
            if rr.startswith('ERR'):
                out['load_error'] = rr
                break
        writer.cmd('COMMIT')

        first_error = None
        made = 0
        for i in range(builds):
            r = c.cmd(f'CREATE INDEX ix{i} ON t (owner)')
            if r.startswith('ERR'):
                if first_error is None:
                    first_error = r
                break
            made += 1
            c.cmd(f'DROP INDEX ix{i}')
        out['builds'] = made
        out['first_error'] = first_error
        out['file_bytes'] = os.path.getsize(db)
        out['file_pages'] = out['file_bytes'] // 8192
        probe = writer.cmd("INSERT INTO t VALUES ('after', 1)")
        out['insert_after'] = probe[:120]
        c.cmd('STOP')
    finally:
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=10)
    return out


if __name__ == '__main__':
    print(run(1, 22100, '/home/ubuntu/mcbench2/cap/c1'))
    print(run(4, 22200, '/home/ubuntu/mcbench2/cap/c4'))
