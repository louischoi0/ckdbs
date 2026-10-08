import sys, os, subprocess, time
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bh-purge-key/tools")
from ckdbs_cli import ServerConnection
B = "/home/cdkbs/bench-runs/bh-close"
wd = B + "/w-repro"; os.makedirs(wd, exist_ok=True)
open(wd + "/s.conf", "w").write(f"data_file = {wd}/s.db\nport = 31731\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
p = subprocess.Popen(["taskset", "-c", "2", sys.argv[1], "--config", wd + "/s.conf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    try:
        c = ServerConnection("127.0.0.1", 31731, timeout=60); break
    except OSError:
        time.sleep(0.1)
n = int(sys.argv[2])
print(c.send_command("CREATE TABLE p (id int64, v int64, w int64) BTREE"))
for lo in range(1, n + 1, 500):
    hi = min(n, lo + 499)
    r = c.send_command("INSERT INTO p VALUES " + ",".join(f"({i},{i},{i})" for i in range(lo, hi + 1)))
print("load", r[:80])
print(c.send_command(f"DELETE FROM p WHERE id BETWEEN 1 AND {n}")[:100])
print(c.send_command(f"PURGE FROM p WHERE id BETWEEN 1 AND {n}")[:100])
print(c.send_command("SELECT COUNT(*) FROM p")[:100])
for lo in range(1, n + 1, 500):
    hi = min(n, lo + 499)
    r = c.send_command("INSERT INTO p VALUES " + ",".join(f"({i},{i},{i})" for i in range(lo, hi + 1)))
    print("reinsert", lo, r[:300])
    break
print("one:", c.send_command("INSERT INTO p VALUES (1,1,1)")[:200])
print("one:", c.send_command("INSERT INTO p VALUES (2,1,1)")[:200])
print("two asc:", c.send_command("INSERT INTO p VALUES (3,1,1),(4,1,1)")[:200])
print("two desc:", c.send_command("INSERT INTO p VALUES (6,1,1),(5,1,1)")[:200])
print(c.send_command("SELECT COUNT(*) FROM p")[:100])
p.terminate(); p.wait()
