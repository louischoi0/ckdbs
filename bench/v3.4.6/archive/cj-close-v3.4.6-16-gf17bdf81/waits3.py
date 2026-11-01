import os, shutil, subprocess, sys, time
sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/cj-wait-events-ash/tools")
from ckdbs_cli import ServerConnection

R = "/home/louis/bench-runs/cj-close"
wd = R + "/work-waits"
shutil.rmtree(wd, ignore_errors=True)
os.makedirs(wd)
conf = wd + "/w.conf"
open(conf, "w").write(f"data_file = {wd}/w.db\nport = 15760\ncores = 1\nlog_file = w.log\nlog_dir = {wd}\nlog_level = warn\nbuffer_pool_frames = 4096\n")
p = subprocess.Popen(["taskset", "-c", "1", R + "/bin/kds_server-b", "--config", conf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    try:
        c = ServerConnection("127.0.0.1", 15760)
        break
    except OSError:
        time.sleep(0.1)
q = c.send_command


def snap(label):
    r = q("SELECT event,count,total_us,max_us FROM sys.wait_stats WHERE core IS NULL AND total_us > 0")
    out = {}
    if r.startswith("ERR"): print(r)
    for line in r.split("\\n")[1:]:
        e, n, t, m = line.split(",")
        out[e] = (int(n), int(t), int(m))
    return out


def diff(a, b):
    return {e: (b[e][0] - a.get(e, (0, 0, 0))[0], b[e][1] - a.get(e, (0, 0, 0))[1], b[e][2]) for e in b}


q("CREATE TABLE t (id int64, v int64, w int64) BTREE")
for lo in range(1, 1001, 500):
    q("INSERT INTO t VALUES " + ",".join(f"({i},{i},{i})" for i in range(lo, lo + 500)))
s0 = snap("load")
for k in range(1, 20001):
    q(f"SELECT * FROM t WHERE id = {1 + (k * 7919) % 1000}")
s1 = snap("reads")
for k in range(2000):
    q(f"INSERT INTO t VALUES ({6000000 + k}, 1, 1)")
s2 = snap("inserts")
print("20,000 point reads:", diff(s0, s1))
print("2,000 single-row INSERTs:", diff(s1, s2))
c.close()
p.terminate()
p.wait()
shutil.rmtree(wd, ignore_errors=True)
