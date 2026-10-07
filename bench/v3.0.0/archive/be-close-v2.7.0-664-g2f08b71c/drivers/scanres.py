#!/usr/bin/env python3
"""BE close part 3, harder variants: hot set size and touch count vs a 4x-cap scan.

usage: scanres.py ARM CAP ROWS --out JSON
Loads one table (ROWS, ~4x cap in pages), then for each variant restarts cold,
warms the hot set (point lookups, one per leaf page), reads it once more
(`before`), runs SELECT COUNT(*) over the table, reads the hot set (`after`).
"""
import argparse, json, shutil, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/be-close/driver")
from pool_lib import *
from pool_past_cap import dist, pool_delta, timed, load


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arm"); ap.add_argument("cap", type=int); ap.add_argument("rows", type=int)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=15640)
    a = ap.parse_args()
    wd = f"{R}/sr-{a.arm}-{a.cap}"
    shutil.rmtree(wd, ignore_errors=True)
    out = {"arm": a.arm, "cap": a.cap, "rows": a.rows, "variants": []}
    s = Server(a.arm, wd, a.port, a.cap, durability="group").start()
    load(s, "big4", a.rows, out, "big4")
    s.stop()
    # (hot pages, warm passes): hot set = one point lookup per leaf page
    for hot, warm in [(1024, 1), (8192, 1), (8192, 3), (12288, 1)]:
        s = Server(a.arm, wd, a.port, a.cap, durability="group").start()
        ids = [1 + k * (a.rows // hot) for k in range(hot)]

        def pas():
            m0 = meta(s.conn)
            t0 = time.perf_counter()
            for i in ids:
                s.q(f"SELECT v FROM big4 WHERE id = {i}")
            el = time.perf_counter() - t0
            m1 = meta(s.conn)
            d = pool_delta(m0, m1)
            return {"seconds": round(el, 4), "misses": d.get("pool_misses"), "hits": d.get("pool_hits")}
        v = {"hot_pages": hot, "warm_passes": warm}
        v["warm"] = [pas() for _ in range(warm)]
        v["before"] = pas()
        m0 = meta(s.conn)
        dt, r = timed(s, "SELECT COUNT(*), MIN(v), MAX(v) FROM big4")
        m1 = meta(s.conn)
        v["scan"] = {"seconds": round(dt, 2), "misses": pool_delta(m0, m1).get("pool_misses")}
        v["after"] = pas()
        v["after2"] = pas()
        out["variants"].append(v)
        print(a.arm, v, flush=True)
        s.stop()
    json.dump(out, open(a.out, "w"), indent=1)
    shutil.rmtree(wd, ignore_errors=True)


if __name__ == "__main__":
    main()
