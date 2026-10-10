"""Attribute the ~100 ms stall B shows at 10,000 rows: after each arm print the carve / checkpoint longest and
the slow statements (> 20 ms) of that arm with their index. B only: A has no contention_* counters."""
import os, shutil, subprocess, sys, time
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks/tools")
from ckdbs_cli import ServerConnection
import bb_overhead_benchmark as bb

binary, rows, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
wd = f"/home/cdkbs/bench-runs/ba-close/w-probe-{os.path.basename(binary)}"
shutil.rmtree(wd, ignore_errors=True); os.makedirs(wd)
conf = f"{wd}/s.conf"
open(conf, "w").write(f"data_file = {wd}/s.db\nport = 31931\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
p = subprocess.Popen(["taskset", "-c", "4", binary, "--config", conf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
c = None
for _ in range(300):
    try:
        c = ServerConnection("127.0.0.1", 31931); break
    except OSError:
        time.sleep(0.1)
f = open(out, "w")


def log(*a):
    print(*a, file=f, flush=True)


def keys():
    m = bb.meta_all(c)
    return {k: v for k, v in m.items() if "carve" in k or "checkpoint" in k or k in ("wal_syncs", "pages_dirty", "pool_dirty")}


t_load = time.perf_counter()
for t in "kr":
    bb.must(c, f"CREATE TABLE be_{t} (id int64, v int64, w int64) BTREE", "CREATED")
    for lo in range(1, rows + 1, 500):
        hi = min(rows, lo + 499)
        bb.must(c, f"INSERT INTO be_{t} VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(lo, hi + 1)), "INSERTED")
log("loaded", round(time.perf_counter() - t_load, 2), "s", keys())
n = 10_000_000
for arm in ("bulk20", "range100", "bulk20", "range100"):
    slow = []
    t0 = time.perf_counter()
    for i in range(3000):
        if arm == "bulk20":
            base = n + 1; n += 20
            sql = "INSERT INTO be_k VALUES " + ",".join(f"({j}, {j}, {j})" for j in range(base, base + 20))
        else:
            lo = 1 + (i * 7919) % max(1, rows - 100)
            sql = f"SELECT COUNT(*), MIN(id), MAX(id) FROM be_r WHERE id >= {lo} AND id < {lo + 100}"
        a = time.perf_counter(); c.send_command(sql); d = (time.perf_counter() - a) * 1e6
        if d > 20000:
            slow.append((i, int(d), round(time.perf_counter() - t0, 3)))
    log(arm, "slow(>20ms) (index, us, t_since_arm_start_s):", slow, keys())
c.close(); p.terminate(); p.wait(20)
shutil.rmtree(wd, ignore_errors=True)
