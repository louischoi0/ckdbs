import sys, os, subprocess, time
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bj-expression-update/tools")
from ckdbs_cli import ServerConnection
wd = "/home/cdkbs/bench-runs/bj-close/w-probe"; os.makedirs(wd, exist_ok=True)
open(wd + "/s.conf", "w").write(f"data_file = {wd}/s.db\nport = 31901\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 65536\nlog_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
p = subprocess.Popen(["taskset", "-c", "2", sys.argv[1], "--config", wd + "/s.conf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    try:
        c = ServerConnection("127.0.0.1", 31901, timeout=60); break
    except OSError:
        time.sleep(0.1)
for sql in sys.argv[2:]:
    print(repr(sql[:90]), "->", repr(c.send_command(sql)[:200]))
p.terminate(); p.wait()
