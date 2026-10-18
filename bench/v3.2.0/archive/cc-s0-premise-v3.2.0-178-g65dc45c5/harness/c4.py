#!/usr/bin/env python3
"""C4: sys.relation_stats (height, leaves) against true COUNT(*) and DESCRIBE row size, from a copy of the pristine file."""
import json, os, shutil, subprocess, sys, time
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/tools")
import kwp

R = "/home/cdkbs/bench-runs/cc-s0"
wd = R + "/c4"
shutil.rmtree(wd, ignore_errors=True); os.makedirs(wd)
shutil.copy(R + "/load/pristine.db", wd + "/c4.db")
shutil.copytree(R + "/load/pristine.db.wal", wd + "/c4.db.wal")
open(wd + "/c4.conf", "w").write(f"data_file = {wd}/c4.db\nport = 25610\ncores = 1\ndurability = group\n"
                                  f"buffer_pool_frames = 65536\nlog_file = c4.log\nlog_dir = {wd}\nlog_level = warn\n")
p = subprocess.Popen([R + "/kds_server", "--config", wd + "/c4.conf"], stdout=open(wd + "/c4.stderr", "w"), stderr=subprocess.STDOUT)
for _ in range(100):
    try:
        c = kwp.Connection("127.0.0.1", 25610, timeout=300, client_name="c4"); break
    except OSError:
        time.sleep(0.1)
def vals(f, rows):
    return [[None if x is None else kwp.render_value(fl, x) for fl, x in zip(f, r)] for r in rows]
f, rows, _t, _n = c.execute("SELECT * FROM sys.relation_stats")
cols = [x["name"] if isinstance(x["name"], str) else x["name"].decode() for x in f]
out = []
for r in vals(f, rows):
    d = dict(zip(cols, r))
    name = d["name"]
    if d["namespace"] == "sys": continue
    _f, cr, _t, _n = c.execute(f"SELECT COUNT(*) FROM {name}")
    d["true_rows"] = vals(_f, cr)[0][0]
    _f, dr, _t, _n = c.execute(f"DESCRIBE {name}")
    d["describe"] = [" | ".join(str(x) for x in row) for row in vals(_f, dr)]
    out.append(d)
json.dump({"cols": cols, "rows": out}, open(sys.argv[1], "w"), indent=1)
c.close(); p.terminate(); p.wait(60)
print(len(out), "relations", cols)
