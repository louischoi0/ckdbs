import sys, os, subprocess, time, glob
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bj-expression-update/tools")
from ckdbs_cli import ServerConnection
import bb_overhead_benchmark as bb
binary, label = sys.argv[1], sys.argv[2]
wd = f"/home/cdkbs/bench-runs/bj-close/w-probe2-{label}"; os.makedirs(wd, exist_ok=True)
open(wd + "/s.conf", "w").write(f"data_file = {wd}/s.db\nport = 31902\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
p = subprocess.Popen(["taskset", "-c", "2", binary, "--config", wd + "/s.conf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    try:
        c = ServerConnection("127.0.0.1", 31902, timeout=60); break
    except OSError:
        time.sleep(0.1)
rows = 1000
for t in ("a", "b"):
    bb.must(c, f"CREATE TABLE {t} (id int64, v int64, w int64) BTREE", "CREATED")
    for lo in range(1, rows + 1, 500):
        bb.must(c, f"INSERT INTO {t} VALUES " + ",".join(f"({i},{i},{i})" for i in range(lo, lo + 500)), "INSERTED")
bb.must(c, "CREATE TABLE i (id int64, v int64, w int64) BTREE", "CREATED")


def meta():
    return bb.meta_all(c)


def sz():
    return sum(os.path.getsize(f) for f in glob.glob(wd + "/*") if os.path.isfile(f))


m0 = meta(); s0 = sz()
print("keys with wal/log/ckpt:", [k for k in m0 if any(x in k for x in ("wal", "log", "ckpt", "checkpoint", "undo", "sync"))])
mx = 0
for q in range(240):
    for t in ("a", "b")[q % 2:q % 2 + 1]:
        bb.must(c, f"UPDATE {t} SET v = {q} WHERE id >= 1", "UPDATED")
m1 = meta(); s1 = sz()
print("after 240 range updates: dir bytes", s0, "->", s1)
lat = []
for n in range(10_000_001, 10_003_001):
    t0 = time.perf_counter(); c.send_command(f"INSERT INTO i VALUES ({n}, {n}, {n})"); lat.append(time.perf_counter() - t0)
m2 = meta(); s2 = sz()
print("insert 3000: max ms", round(max(lat) * 1000, 1), "n>50ms", sum(1 for x in lat if x > 0.05), "dir bytes", s2)
for k in sorted(m2):
    if m2[k] != m0.get(k) and any(x in k for x in ("wal", "log", "ckpt", "checkpoint", "undo", "sync", "recovery", "pool_dirty", "pool_re")):
        print(" ", k, m0.get(k), m1.get(k), m2.get(k))
p.terminate(); p.wait()
