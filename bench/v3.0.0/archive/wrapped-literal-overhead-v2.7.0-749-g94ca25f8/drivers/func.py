#!/usr/bin/env python3
"""Functional sanity: A and B answer a wrapped literal differently (so B is the changed engine)."""
import os, subprocess, sys, time, shutil, socket
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-int-literal-wrap-in-where/tools")
from ckdbs_cli import ServerConnection

for lab, port in (("A", 15641), ("B", 15642)):
    d = f"/home/cdkbs/bench-runs/lw/func-{lab}"
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    open(f"{d}/s.conf", "w").write(f"data_file = {d}/kds.db\ncores = 1\nport = {port}\ndurability = relaxed\n"
                                   f"buffer_pool_frames = 4096\nlog_dir = {d}\nlog_file = kds.log\nlog_level = warn\n")
    pr = subprocess.Popen(["taskset", "-c", "6", f"/home/cdkbs/bench-runs/lw/kds_server-{lab}", "--config", f"{d}/s.conf"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 1).close(); break
        except OSError:
            time.sleep(0.2)
    c = ServerConnection("127.0.0.1", port)
    c.send_command("CREATE TABLE t (id int64, qty int64) BTREE")
    c.send_command("INSERT INTO t VALUES (0, 7), (5, 9)")
    for sql in ("SELECT * FROM t WHERE id = 18446744073709551616",
                "SELECT * FROM t WHERE qty = 18446744073709551621",
                "SELECT * FROM t WHERE id = 5"):
        print(lab, repr(sql), "->", repr(c.send_command(sql))[:90])
    pr.terminate(); pr.wait(10)
    shutil.rmtree(d, ignore_errors=True)
