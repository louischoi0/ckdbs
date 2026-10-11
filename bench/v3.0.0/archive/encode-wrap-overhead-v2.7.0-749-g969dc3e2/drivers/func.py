#!/usr/bin/env python3
"""Functional sanity: the A and B binaries answer a wrapped integer literal stored into a date / int
column differently (so B is the changed engine and C is the unchanged one)."""
import os, subprocess, sys, time, shutil, socket
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-date-wrapped-literal-stored/tools")
from ckdbs_cli import ServerConnection

for lab, port in (("A", 15741), ("B", 15742), ("C", 15743)):
    d = f"/home/cdkbs/bench-runs/ew/func-{lab}"
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    open(f"{d}/s.conf", "w").write(f"data_file = {d}/kds.db\ncores = 1\nport = {port}\ndurability = relaxed\n"
                                   f"buffer_pool_frames = 4096\nlog_dir = {d}\nlog_file = kds.log\nlog_level = warn\n")
    pr = subprocess.Popen(["taskset", "-c", "6", f"/home/cdkbs/bench-runs/ew/kds_server-{lab}", "--config", f"{d}/s.conf"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 1).close(); break
        except OSError:
            time.sleep(0.2)
    c = ServerConnection("127.0.0.1", port)
    c.send_command("CREATE TABLE t (id int64, d date, q int64) BTREE")
    for sql in ("INSERT INTO t VALUES (1, 18446744073709551616, 7)",
                "INSERT INTO t VALUES (2, '2026-08-09', 18446744073709551621)",
                "INSERT INTO t VALUES (3, '2026-08-09', 9)",
                "UPDATE t SET q = 18446744073709551621 WHERE id = 3"):
        print(lab, repr(sql), "->", repr(c.send_command(sql))[:100])
    pr.terminate(); pr.wait(10)
    shutil.rmtree(d, ignore_errors=True)
