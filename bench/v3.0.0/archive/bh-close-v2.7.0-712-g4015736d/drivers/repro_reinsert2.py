import sys, os, subprocess, time, collections
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


def load(t):
    print(c.send_command(f"CREATE TABLE {t} (id int64, v int64, w int64) BTREE")[:30])
    for lo in range(1, n + 1, 500):
        hi = min(n, lo + 499)
        r = c.send_command(f"INSERT INTO {t} VALUES " + ",".join(f"({i},{i},{i})" for i in range(lo, hi + 1)))
        assert r.startswith("INSERTED"), r


# Cell 1: DELETE k, PURGE k, INSERT k, one key at a time, ascending, whole relation
load("a")
res = collections.Counter(); first = {}
for k in range(1, n + 1):
    assert c.send_command(f"DELETE FROM a WHERE id = {k}").startswith("DELETED 1")
    assert c.send_command(f"PURGE FROM a WHERE id = {k}") == "PURGED 1"
    r = c.send_command(f"INSERT INTO a VALUES ({k}, 0, 0)")
    key = r[:60] if r.startswith("ERR") else "INSERTED"
    key = key.split(" (")[0]
    res[key.split(" key ")[0] if "separator" not in key else "ERR separator"] += 1
    if r.startswith("ERR") and "e" not in first: first["e"] = (k, r[:160])
print("cell1 delete-purge-insert per key:", dict(res), first)
print("count", c.send_command("SELECT COUNT(*) FROM a"))

# Cell 2: delete all, purge all (one window), then insert keys singly ascending
load("b")
print(c.send_command(f"DELETE FROM b WHERE id BETWEEN 1 AND {n}")[:30], c.send_command(f"PURGE FROM b WHERE id BETWEEN 1 AND {n}")[:30])
ok = 0; bad = collections.Counter(); firstbad = None
for k in range(1, n + 1):
    r = c.send_command(f"INSERT INTO b VALUES ({k}, 0, 0)")
    if r.startswith("INSERTED"): ok += 1
    else:
        bad[r[:50]] += 1
        if firstbad is None: firstbad = (k, r[:160])
print("cell2 purge all then insert singly asc: ok", ok, "bad", dict(bad), firstbad)
print("count", c.send_command("SELECT COUNT(*) FROM b"))

# Cell 3: purge a middle window of 100 keys, re-insert them singly
load("c")
lo, hi = n // 2, n // 2 + 99
print(c.send_command(f"DELETE FROM c WHERE id BETWEEN {lo} AND {hi}")[:30], c.send_command(f"PURGE FROM c WHERE id BETWEEN {lo} AND {hi}")[:30])
ok = 0; bad = collections.Counter(); firstbad = None
for k in range(lo, hi + 1):
    r = c.send_command(f"INSERT INTO c VALUES ({k}, 0, 0)")
    if r.startswith("INSERTED"): ok += 1
    else:
        bad[r[:50]] += 1
        if firstbad is None: firstbad = (k, r[:160])
print("cell3 purge 100-key window then insert singly: ok", ok, "bad", dict(bad), firstbad)
print("count", c.send_command("SELECT COUNT(*) FROM c"))
p.terminate(); p.wait()
