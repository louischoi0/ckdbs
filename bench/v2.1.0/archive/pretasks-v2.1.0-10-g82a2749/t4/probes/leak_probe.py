#!/usr/bin/env python3
"""Does a REFUSED shipped CREATE INDEX still consume pages?

On the pre-merge tree, 6,670 refused attempts exhausted a single-page free
map. FM2-FM5 raised the ceiling to kMaxPageCount; the question left is
whether the refusal itself still allocates. Measured by file size across N
refused attempts, with no retry, on a peer whose extent lease is spent.
"""
import os
import subprocess
import sys
import time

ROOT = '/home/ubuntu/ckdbs/.claude/worktrees/worktree-v2.2.0-pretasks-stmtshipping'
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from multicore_benchmark import Conn, collect_connections, field, wait_for_port  # noqa: E402

wd = '/home/ubuntu/mcbench2/leak'
os.makedirs(wd, exist_ok=True)
port = 24600
conf = os.path.join(wd, 's.conf')
db = os.path.join(wd, 's.db')
with open(conf, 'w') as f:
    f.write(f"data_file = {db}\nport = {port}\ncores = 4\nplacement = rotate\n"
            f"peer_listeners = on\nlog_file = s.log\nlog_dir = {wd}\nlog_level = error\n")
errp = os.path.join(wd, 's.stderr')
with open(errp, 'w') as err:
    proc = subprocess.Popen([os.path.join(ROOT, 'build-release/kds_server'), '--config', conf],
                            stdout=err, stderr=subprocess.STDOUT)
try:
    wait_for_port(port, errp)
    setup = collect_connections(port, {0: 1}, 512)[0][0][0]
    print(setup.cmd('CREATE TABLE t (id int64, owner varchar, balance int64) BTREE'))
    owner = int(field(setup.cmd('DESCRIBE t'), 'owner_core'))
    w = collect_connections(port, {owner: 1}, 512)[0][owner][0]
    for _ in range(300):
        if not w.cmd("INSERT INTO t VALUES ('warm', 0)").startswith('ERR'):
            break
        time.sleep(0.01)
    w.cmd('BEGIN')
    for i in range(3000):
        w.cmd(f"INSERT INTO t VALUES ('x', {i})")
    w.cmd('COMMIT')
    time.sleep(0.5)
    before = os.path.getsize(db)
    refused = ok = 0
    for i in range(300):
        r = setup.cmd(f'CREATE INDEX lk{i} ON t (owner)')
        if r.startswith('ERR'):
            refused += 1
        else:
            ok += 1
            setup.cmd(f'DROP INDEX lk{i}')
    time.sleep(0.5)
    after = os.path.getsize(db)
    print(f"owner=core{owner} attempts=300 refused={refused} built={ok}")
    print(f"file {before} -> {after} bytes ({(after-before)//8192} pages) "
          f"= {(after-before)/8192/max(1,refused):.2f} pages per refusal")
    print("insert after:", w.cmd("INSERT INTO t VALUES ('after', 1)")[:80])
    setup.cmd('STOP')
finally:
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.terminate()
        proc.wait(timeout=10)
