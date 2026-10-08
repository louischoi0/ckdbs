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
n = 1000
c.send_command("CREATE TABLE b (id int64, v int64, w int64) BTREE")
for lo in range(1, n + 1, 500):
    c.send_command("INSERT INTO b VALUES " + ",".join(f"({i},{i},{i})" for i in range(lo, lo + 500)))
# purge only the second leaf's keys 167..332
print(c.send_command("DELETE FROM b WHERE id BETWEEN 167 AND 332"), c.send_command("PURGE FROM b WHERE id BETWEEN 167 AND 332"))
for k in (167, 168, 167, 167):
    print("insert", k, c.send_command(f"INSERT INTO b VALUES ({k}, 0, 0)")[:120])
print("insert 332", c.send_command("INSERT INTO b VALUES (332, 0, 0)")[:120])
print("insert 250", c.send_command("INSERT INTO b VALUES (250, 0, 0)")[:120])
print("insert 167 again", c.send_command("INSERT INTO b VALUES (167, 0, 0)")[:120])
print("rows 160..170:", c.send_command("SELECT id FROM b WHERE id BETWEEN 160 AND 170")[:200])
# purge a window that leaves some keyed slots in the leaf
print(c.send_command("DELETE FROM b WHERE id BETWEEN 333 AND 400"), c.send_command("PURGE FROM b WHERE id BETWEEN 333 AND 400"))
print("insert 333 (leaf partly purged)", c.send_command("INSERT INTO b VALUES (333, 0, 0)")[:120])
p.terminate(); p.wait()
