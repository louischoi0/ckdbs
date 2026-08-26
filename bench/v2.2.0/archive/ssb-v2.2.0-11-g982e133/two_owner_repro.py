#!/usr/bin/env python3
"""Why does a two-owner join get answered from some sessions and refused
from others? Prints, per session core, the owners the chain touches and the
verbatim reply."""
import os
import subprocess
import sys
import time

W = "/home/ubuntu/ckdbs/.claude/worktrees/workplan-v2.2.0"
sys.path.insert(0, os.path.join(W, "tools"))
from multicore_benchmark import Conn, field, wait_for_port  # noqa: E402

wd = "/home/ubuntu/ssb/repro"
os.makedirs(wd, exist_ok=True)
conf = os.path.join(wd, "s.conf")
port = 19900
with open(conf, "w") as f:
    f.write(f"data_file = {wd}/s.db\nport = {port}\ncores = 4\n"
            f"placement = rotate\npeer_listeners = on\n"
            f"log_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
err = open(os.path.join(wd, "s.err"), "w")
proc = subprocess.Popen(["/home/ubuntu/ssb/bin/kds_server", "--config", conf],
                        stdout=err, stderr=subprocess.STDOUT)
try:
    wait_for_port(port, os.path.join(wd, "s.err"))
    # a setup connection on core 0
    setup = None
    for _ in range(200):
        c = Conn(port)
        if field(c.cmd("SHOW META"), "core") == 0:
            setup = c
            break
        c.close()
    names = [f"r{i}" for i in range(6)]
    owners = {}
    for n in names:
        setup.cmd(f"CREATE TABLE {n} (id int64, owner varchar, balance int64) BTREE")
        owners[n] = int(field(setup.cmd(f"DESCRIBE {n}"), "owner_core"))
    print("owners:", owners)
    # one session per core, and each runs every ordered pair with distinct owners
    per = {}
    for _ in range(400):
        c = Conn(port)
        k = field(c.cmd("SHOW META"), "core")
        if k not in per:
            per[k] = c
        else:
            c.close()
        if len(per) == 4:
            break
    for core in sorted(per):
        conn = per[core]
        for a in names:
            for b in names:
                if a == b or owners[a] == owners[b]:
                    continue
                q = f"SELECT {a}.balance FROM {a} JOIN {b} ON {b}.id = {a}.id"
                r = conn.cmd(q)
                verdict = "ERR" if r.startswith("ERR") else "OK "
                print(f"  session core {core}: {a}(own {owners[a]}) x "
                      f"{b}(own {owners[b]}) -> {verdict} {r[:150]}")
            break  # one outer relation per core is enough
    setup.cmd("STOP")
finally:
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
