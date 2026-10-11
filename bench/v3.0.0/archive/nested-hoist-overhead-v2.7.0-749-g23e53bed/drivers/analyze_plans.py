import os, subprocess, sys, time, socket, shutil
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-nested-uncorrelated-subquery/tools")
from ckdbs_cli import ServerConnection
OUT = "/home/cdkbs/bench-runs/nh"
d = f"{OUT}/work-plans"
shutil.rmtree(d, ignore_errors=True)
os.makedirs(d)
res = {}
for lab, port in (("A", 15631), ("B", 15630)):
    open(f"{d}/{lab}.conf", "w").write(f"data_file = {d}/{lab}.db\ncores = 1\nport = {port}\ndurability = relaxed\nbuffer_pool_frames = 131072\nlog_dir = {d}\nlog_file = {lab}.log\nlog_level = warn\n")
    p = subprocess.Popen(["taskset", "-c", "6", f"{OUT}/kds_server-{lab}", "--config", f"{d}/{lab}.conf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 1).close(); break
        except OSError:
            time.sleep(0.2)
    c = ServerConnection("127.0.0.1", port)
    n = 1000
    for s in ["CREATE TABLE a (id int64, v int64) BTREE", "CREATE TABLE b (id int64, a_id int64, v int64) BTREE", "CREATE TABLE c (id int64, tag int64) BTREE"]:
        c.send_command(s)
    c.send_command("INSERT INTO a VALUES " + ",".join(f"({i},{i%7})" for i in range(1, n + 1)))
    c.send_command("INSERT INTO b VALUES " + ",".join(f"({i},{2*i},0)" for i in range(1, n // 2 + 1)))
    c.send_command("INSERT INTO c VALUES " + ",".join(f"({i},{i})" for i in range(1, 51)))
    c.send_command("CREATE INDEX ix_b_aid ON b(a_id)")
    stmts = {
        "corr-exists-scan": "SELECT a.id FROM a WHERE EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id)",
        "unc-in-scan": "SELECT a.id FROM a WHERE a.id IN (SELECT b.a_id FROM b WHERE b.id BETWEEN 1 AND 20)",
        "unc-exists-scan": "SELECT a.id FROM a WHERE a.v = 3 AND EXISTS (SELECT c.id FROM c WHERE c.tag = 5)",
        "nested-some-scan": "SELECT a.id FROM a WHERE EXISTS (SELECT b.id FROM b WHERE b.a_id = a.id AND EXISTS (SELECT c.id FROM c WHERE c.tag = 5))",
    }
    for k, q in stmts.items():
        res[(lab, k)] = c.send_command("ANALYZE " + q).replace("\\n", "\n")
    p.terminate(); p.wait(10)
for k in ["corr-exists-scan", "unc-in-scan", "unc-exists-scan", "nested-some-scan"]:
    for lab in ("A", "B"):
        print(f"=== {k} [{lab}]\n{res[(lab, k)][:1500]}\n")
shutil.rmtree(d, ignore_errors=True)
