#!/usr/bin/env python3
"""Does core 0's answer to a two-owner join carry rows, or is it empty?
Rows are inserted from each relation's owner, then the join is asked from a
core-0 session and from a peer session."""
import os
import subprocess
import sys

W = "/home/ubuntu/ckdbs/.claude/worktrees/workplan-v2.2.0"
sys.path.insert(0, os.path.join(W, "tools"))
from multicore_benchmark import Conn, collect_connections, field, is_retryable, wait_for_port  # noqa: E402

wd = "/home/ubuntu/ssb/repro2"
os.makedirs(wd, exist_ok=True)
conf = os.path.join(wd, "s.conf")
port = 19940
with open(conf, "w") as f:
    f.write(f"data_file = {wd}/s.db\nport = {port}\ncores = 4\n"
            f"placement = rotate\npeer_listeners = on\n"
            f"log_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
err = open(os.path.join(wd, "s.err"), "w")
proc = subprocess.Popen(["/home/ubuntu/ssb/bin/kds_server", "--config", conf],
                        stdout=err, stderr=subprocess.STDOUT)
try:
    wait_for_port(port, os.path.join(wd, "s.err"))
    got, _ = collect_connections(port, {0: 1}, 400)
    setup = got[0][0]
    owners = {}
    for n in ("a", "b"):
        setup.cmd(f"CREATE TABLE {n} (id int64, owner varchar, balance int64) BTREE")
        owners[n] = int(field(setup.cmd(f"DESCRIBE {n}"), "owner_core"))
    print("owners:", owners)
    # rows, written from a session on each owner
    per, _ = collect_connections(port, {owners["a"]: 1, owners["b"]: 1}, 400)
    for n in ("a", "b"):
        c = per[owners[n]][0]
        for i in range(4):
            r = c.cmd(f"INSERT INTO {n} VALUES ('x', {i})")
            while r.startswith("ERR") and is_retryable(r):
                r = c.cmd(f"INSERT INTO {n} VALUES ('x', {i})")
            if r.startswith("ERR"):
                print("insert failed:", r)
    q = "SELECT a.balance FROM a JOIN b ON b.id = a.id"
    print("core0 session:", setup.cmd(q)[:300])
    peer = per[owners["a"]][0]
    print(f"core{owners['a']} session:", peer.cmd(q)[:300])
    print("core0 count a:", setup.cmd("SELECT COUNT(*) FROM a")[:120])
    print("core0 count b:", setup.cmd("SELECT COUNT(*) FROM b")[:120])
    setup.cmd("STOP")
finally:
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
