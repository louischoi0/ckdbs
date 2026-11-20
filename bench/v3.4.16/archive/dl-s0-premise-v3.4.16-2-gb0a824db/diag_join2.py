#!/usr/bin/env python3
"""Diagnostic: does the join8 shape ever get a captured order, and how long does that take?
Starts one spike server (default env), loads ja/jb like dl_s0.py, runs the join shape for 40 s
from one client and prints sys.meta's captur*/plan* rows every 5 s."""
import os, subprocess, sys, time, random
sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/dl-statistics-queue-no-statement/tools")
import kwp
wd = "/home/louis/bench-runs/dl-s0/diag2"
SQL = sys.argv[1]
os.makedirs(wd, exist_ok=True)
data = wd + "/d.db"
subprocess.run(["rm", "-rf", data, data + ".wal"])
conf = wd + "/d.conf"
open(conf, "w").write(f"data_file = {data}\nport = 27990\ncores = 8\ndurability = relaxed\n"
                      f"buffer_pool_frames = 65536\nlog_file = d.log\nlog_dir = {wd}\nlog_level = warn\n")
p = subprocess.Popen(["/home/louis/bench-runs/dl-s0/kds_spike", "--config", conf],
                     stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
for _ in range(100):
    try:
        c = kwp.Connection("127.0.0.1", 27990, timeout=120)
        break
    except OSError:
        time.sleep(0.1)
for t in ("ja", "jb"):
    c.execute(f"CREATE TABLE {t} (id int64, v int64) BTREE")
    for lo in range(1, 10001, 500):
        c.execute(f"INSERT INTO {t} VALUES " + ",".join(f"({i},{i})" for i in range(lo, lo + 500)))


def show():
    _, rows, _, _ = c.execute("SELECT * FROM sys.meta")
    out = []
    for r in rows:
        v = [x.decode() if x is not None else "" for x in r]
        if v[1].startswith("captur") or v[1].startswith("plan") or v[1].startswith("admin"):
            out.append(f"{v[1]}={v[2]}")
    return " ".join(out)


c.execute("CREATE INDEX ixv ON ja(v)")
print("start", show())
t0 = time.time()
n = 0
last = 0
while time.time() - t0 < 12:
    c.query(SQL.replace("K", str(random.randint(1, 9990))))
    n += 1
    if time.time() - last > 5:
        last = time.time()
        print(f"{time.time()-t0:.0f}s n={n}", show())
_, rows, _, _ = c.execute("ANALYZE " + SQL.replace("K","7") + "")
print(" | ".join((r[0] or b"").decode() for r in rows)[:700])
c.close()
p.terminate()
p.wait(30)
