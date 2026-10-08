#!/usr/bin/env python3
"""BG-S1: BE-S6's scan-resistance cell, with the hand's laps counted.

usage: scanres_laps.py ARM CAP ROWS --out JSON

The cell is BE-S6's (`archive/be-close-v2.7.0-664-g2f08b71c/drivers/scanres.py`):
load one table of about four times the pool, then per variant restart cold,
warm the hot set (one pk point lookup per chosen leaf), read it once more
(`before`), scan the table, read the hot set again (`after`).

Added for BG-S1:
  - every phase records its `pool_*` deltas, so the scan's steps are
    `pool_batch_steps` + `pool_background_steps` and its laps are those
    over `pool_slots`;
  - the `after` pass is read one lookup at a time, recording each lookup's
    misses, so which hot rows survived is known by position in the warm
    order (a cold pool fills slots in fault order).
"""
import argparse, json, shutil, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/bg-s1/driver")
from pool_lib import *
from pool_past_cap import pool_delta, timed, load


def steps(d):
    return d.get("pool_batch_steps", 0) + d.get("pool_batch_steps_background", 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arm"); ap.add_argument("cap", type=int); ap.add_argument("rows", type=int)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=15640)
    ap.add_argument("--variants", default="1024:1,8192:1,8192:3,12288:1")
    a = ap.parse_args()
    wd = f"{R}/sr-{a.arm}-{a.cap}"
    shutil.rmtree(wd, ignore_errors=True)
    out = {"arm": a.arm, "cap": a.cap, "rows": a.rows, "variants": []}
    s = Server(a.arm, wd, a.port, a.cap, durability="group").start()
    load(s, "big4", a.rows, out, "big4")
    s.stop()
    for spec in a.variants.split(","):
        hot, warm = (int(x) for x in spec.split(":"))
        s = Server(a.arm, wd, a.port, a.cap, durability="group").start()
        ids = [1 + k * (a.rows // hot) for k in range(hot)]

        def pas():
            m0 = meta(s.conn)
            t0 = time.perf_counter()
            for i in ids:
                s.q(f"SELECT v FROM big4 WHERE id = {i}")
            el = time.perf_counter() - t0
            d = pool_delta(m0, meta(s.conn))
            return {"seconds": round(el, 4), "misses": d.get("pool_misses"),
                    "hits": d.get("pool_hits"), "steps": steps(d)}

        v = {"hot_pages": hot, "warm_passes": warm}
        v["warm"] = [pas() for _ in range(warm)]
        v["before"] = pas()
        m0 = meta(s.conn)
        dt, _ = timed(s, "SELECT COUNT(*), MIN(v), MAX(v) FROM big4")
        m1 = meta(s.conn)
        d = pool_delta(m0, m1)
        slots = m1.get("pool_slots", 0)
        v["scan"] = {"seconds": round(dt, 2), "misses": d.get("pool_misses"),
                     "batch_steps": d.get("pool_batch_steps"),
                     "batch_steps_background": d.get("pool_batch_steps_background"),
                     "reclaimed_inline": d.get("pool_reclaimed_inline"),
                     "reclaimed_background": d.get("pool_reclaimed_background"),
                     "slots": slots, "laps": round(steps(d) / slots, 3) if slots else None}
        # The after pass, one lookup at a time: which hot rows were lost.
        lost = []
        m_prev = meta(s.conn)
        t0 = time.perf_counter()
        for k, i in enumerate(ids):
            s.q(f"SELECT v FROM big4 WHERE id = {i}")
            m_next = meta(s.conn)
            lost.append(int(m_next.get("pool_misses", 0) - m_prev.get("pool_misses", 0)))
            m_prev = m_next
        v["after"] = {"seconds": round(time.perf_counter() - t0, 4), "misses": sum(lost),
                      "rows_with_a_miss": sum(1 for x in lost if x > 0),
                      "rows_without": sum(1 for x in lost if x == 0)}
        v["after_lost"] = lost
        v["after2"] = pas()
        out["variants"].append(v)
        print(a.arm, {k: v[k] for k in v if k != "after_lost"}, flush=True)
        s.stop()
    json.dump(out, open(a.out, "w"), indent=1)
    shutil.rmtree(wd, ignore_errors=True)


if __name__ == "__main__":
    main()
