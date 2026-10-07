#!/usr/bin/env python3
"""BE close part 4: where does a 1,024-row-statement bulk load start being refused? B only, no retry."""
import json, shutil, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/be-close/driver")
from pool_lib import *
from pool_past_cap import dist

res = []
for cap in [int(x) for x in sys.argv[1].split(",")]:
    for rep in range(int(sys.argv[2])):
        wd = f"{R}/ls-{cap}"
        shutil.rmtree(wd, ignore_errors=True)
        try:
            s = Server("B", wd, 15650, cap, durability="group").start()
        except Exception as e:
            res.append({"cap": cap, "rep": rep, "mount_refused": str(e)[-300:]})
            print(res[-1], flush=True)
            continue
        s.q("CREATE TABLE lt (id int64, day int64, v int64, pad char(100)) BTREE")
        n = int(sys.argv[3])
        lat, errs, firsts = [], 0, []
        s.sample_start()
        t0 = time.time()
        for lo in range(1, n + 1, 1024):
            t = time.perf_counter()
            r = s.q("INSERT INTO lt VALUES " + ",".join(row(i) for i in range(lo, lo + 1024)))
            lat.append((time.perf_counter() - t) * 1e6)
            if not r.startswith("INSERTED"):
                errs += 1
                if len(firsts) < 2:
                    firsts.append(r[:200])
        el = time.time() - t0
        cnt = s.q("SELECT COUNT(*) FROM lt")
        m = meta(s.conn)
        rss = s.sample_stop()
        d = {"cap": cap, "rep": rep, "stmts": len(lat), "refused": errs, "rows_present": cnt, "rows_sent": n, "seconds": round(el, 2),
             "rss_max_mib": round(rss["max_mib"], 1), "cap_mib": cap * 8 / 1024, "pool_refused": m.get("pool_refused"),
             "floor_msgs": firsts, "stmt_p50_us": round(dist(lat)["p50"]), "stmt_p99_us": round(dist(lat)["p99"]),
             "reclaimed_bg": m.get("pool_reclaimed_background"), "reclaimed_inline": m.get("pool_reclaimed_inline")}
        res.append(d)
        print(d, flush=True)
        s.stop()
        shutil.rmtree(wd, ignore_errors=True)
json.dump(res, open(f"{R}/pastcap/loadsweep.json", "w"), indent=1)
