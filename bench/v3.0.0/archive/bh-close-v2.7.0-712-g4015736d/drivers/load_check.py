import sys, os, subprocess, time
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bh-purge-key/tools")
from ckdbs_cli import ServerConnection
wd = "/home/cdkbs/bench-runs/bh-close2/w-lc"; os.makedirs(wd, exist_ok=True)
open(wd + "/s.conf", "w").write(f"data_file = {wd}/s.db\nport = 31731\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
p = subprocess.Popen(["taskset", "-c", "2", sys.argv[1], "--config", wd + "/s.conf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    try:
        c = ServerConnection("127.0.0.1", 31731, timeout=60); break
    except OSError:
        time.sleep(0.1)
print(c.send_command("CREATE TABLE b (id int64, v int64, w int64) BTREE"))
for lo in range(1, 1001, 500):
    print(c.send_command("INSERT INTO b VALUES " + ",".join(f"({i},{i},{i})" for i in range(lo, lo + 500)))[:100])
print(c.send_command("SELECT COUNT(*) FROM b"))
print(c.send_command("SELECT COUNT(*) FROM b WHERE id BETWEEN 167 AND 332"))
print(c.send_command("DELETE FROM b WHERE id BETWEEN 167 AND 332"))
print(c.send_command("SELECT COUNT(*) FROM b"))
p.terminate(); p.wait()
