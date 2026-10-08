#!/usr/bin/env python3
"""BE close: bulk load, past-the-cap scans, scan resistance, one arm at a time.

usage: pool_past_cap.py ARM CAP --big2 ROWS [--big4 ROWS] [--hot N] --out JSON [--port P] [--scan-timeout S]
"""
import argparse, json, os, shutil, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/be-close/driver")
from pool_lib import *


def pct(l, p):
    s = sorted(l)
    return s[max(0, min(len(s) - 1, int(-(-p * len(s) // 100)) - 1))] if s else 0.0


def dist(l):
    return {"n": len(l), "p0": min(l) if l else 0, "p25": pct(l, 25), "p50": pct(l, 50), "p90": pct(l, 90),
            "p99": pct(l, 99), "max": max(l) if l else 0, "sum_s": sum(l) / 1e6}


def pool_delta(a, b):
    return {k: b.get(k, 0) - a.get(k, 0) for k in b if k.startswith("pool_") and k not in ("pool_budget",)}


def pool_now(m):
    return {k: m[k] for k in m if k.startswith("pool_")}


def load(s, table, nrows, out, label):
    s.q(f"CREATE TABLE {table} (id int64, day int64, v int64, pad char(100)) BTREE")
    lat, errs, first_err = [], 0, None
    s.sample_start()
    t0 = time.time()
    m0 = meta(s.conn)
    marks = []
    for lo in range(1, nrows + 1, 1024):
        sql = "INSERT INTO " + table + " VALUES " + ",".join(row(i) for i in range(lo, lo + 1024))
        tries = 0
        while True:
            t = time.perf_counter()
            r = s.q(sql)
            el1 = (time.perf_counter() - t) * 1e6
            if r.startswith("INSERTED"):
                lat.append(el1)
                break
            errs += 1
            first_err = first_err or r[:300]
            tries += 1
            if "buffer pool full" not in r or tries > 200:
                lat.append(el1)
                break
            time.sleep(0.05)  # the refused statement wrote nothing; retry it
        if (lo // 1024) % 256 == 0:
            marks.append((round(time.time() - t0, 1), s.rss()["VmRSS"] // 1024))
    el = time.time() - t0
    m1 = meta(s.conn)
    rss = s.sample_stop()
    out["load_" + label] = {"table": table, "rows": nrows, "stmts": len(lat), "refusals_retried": errs, "elapsed_s": round(el, 2),
                            "rows_per_s": round(nrows / el), "errors": errs, "first_error": first_err,
                            "stmt_latency_us": dist(lat), "rss": rss, "rss_trace_mib": marks,
                            "vmhwm_mib": s.rss()["VmHWM"] / 1024, "pool_end": pool_now(m1),
                            "pool_delta": pool_delta(m0, m1)}
    print(label, "load", out["load_" + label]["elapsed_s"], "s errs", errs, "rss max", rss["max_mib"], flush=True)


def timed(s, sql):
    t = time.perf_counter()
    r = s.q(sql)
    return (time.perf_counter() - t), r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arm")
    ap.add_argument("cap", type=int)
    ap.add_argument("--big2", type=int, required=True)
    ap.add_argument("--big4", type=int, default=0)
    ap.add_argument("--days", type=int, default=0, help="probes per pass (default: all days of big2)")
    ap.add_argument("--hot", type=int, default=1024)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=15630)
    ap.add_argument("--pin", type=int, default=None)
    ap.add_argument("--skip-probes", action="store_true")
    a = ap.parse_args()
    wd = f"{R}/pc-{a.arm}-{a.cap}"
    shutil.rmtree(wd, ignore_errors=True)
    out = {"arm": a.arm, "cap": a.cap, "cap_bytes_mib": a.cap * 8192 / 2**20, "loadavg_start": open("/proc/loadavg").read().strip()}
    s = Server(a.arm, wd, a.port, a.cap, durability="group", pin=a.pin).start()
    out["mount"] = {k: v for k, v in meta(s.conn).items() if k.startswith(("pool_", "recovery_checkpoint"))}
    out["rss_idle_mib"] = s.rss()["VmRSS"] / 1024
    # ---- part 4 (and the load of the scan targets)
    load(s, "big2", a.big2, out, "big2")
    if a.big4:
        load(s, "big4", a.big4, out, "big4")
    out["datafile_mib"] = os.path.getsize(f"{wd}/{a.arm}.db") / 2**20
    s.stop()

    def cold():
        sv = Server(a.arm, wd, a.port, a.cap, durability="group", pin=a.pin).start()
        return sv

    # ---- part 2
    s = cold()
    m = meta(s.conn)
    out["part2_mount_pool"] = pool_now(m)
    s.sample_start()
    m0 = meta(s.conn)
    dt, r = timed(s, "SELECT COUNT(*), MIN(v), MAX(v) FROM big2")
    m1 = meta(s.conn)
    out["p2a_single_scan"] = {"seconds": round(dt, 3), "reply": r[:100], "pool_delta": pool_delta(m0, m1), "pool_resident_after": m1.get("pool_resident")}
    print("2a", round(dt, 2), "s", r[:60], flush=True)
    if not a.skip_probes:
        # ids per day: 4096; pages per day ~ 76
        ndays = a.big2 // 4096
        if a.days:
            ndays = min(ndays, a.days)
        passes = {}
        for name in ("p2b_probes_pass1", "p2c_probes_pass2"):
            lat, errs = [], 0
            m0 = meta(s.conn)
            t0 = time.time()
            for d in range(ndays):
                lo, hi = d * 4096 + 1, (d + 1) * 4096
                dt, r = timed(s, f"SELECT COUNT(*), MIN(v), MAX(v) FROM big2 WHERE id BETWEEN {lo} AND {hi}")
                lat.append(dt * 1e6)
                if not r.startswith("count"):
                    errs += 1
            m1 = meta(s.conn)
            out[name] = {"days": ndays, "elapsed_s": round(time.time() - t0, 2), "errors": errs,
                         "probe_latency_us": dist(lat), "pool_delta": pool_delta(m0, m1),
                         "pool_resident_after": m1.get("pool_resident")}
            print(name, out[name]["elapsed_s"], "s", flush=True)
        # (c2) hot repeat: 20 days x 5 back-to-back probes; first vs 2nd..5th
        lat1, latn = [], []
        m0 = meta(s.conn)
        t0 = time.time()
        for d in range(min(20, ndays)):
            lo, hi = (d * 37 % ndays) * 4096 + 1, ((d * 37 % ndays) + 1) * 4096
            for k in range(5):
                dt, r = timed(s, f"SELECT COUNT(*), MIN(v), MAX(v) FROM big2 WHERE id BETWEEN {lo} AND {hi}")
                (lat1 if k == 0 else latn).append(dt * 1e6)
        m1 = meta(s.conn)
        out["p2c_hot_repeat"] = {"first_probe_us": dist(lat1), "repeat_probe_us": dist(latn),
                                 "elapsed_s": round(time.time() - t0, 2), "pool_delta": pool_delta(m0, m1)}
    out["rss_part2"] = s.sample_stop()
    out["rss_part2"]["vmhwm_mib"] = s.rss()["VmHWM"] / 1024
    out["pool_after_part2"] = pool_now(meta(s.conn))
    s.stop()

    # ---- part 3
    if a.big4:
        s = cold()
        rows4 = a.big4
        stride = max(1, rows4 // a.hot)
        ids = [1 + k * stride for k in range(a.hot)]

        def hot_pass():
            lat = []
            m0 = meta(s.conn)
            for i in ids:
                dt, r = timed(s, f"SELECT v FROM big4 WHERE id = {i}")
                lat.append(dt * 1e6)
            m1 = meta(s.conn)
            return lat, pool_delta(m0, m1)

        s.sample_start()
        res = {}
        for k in range(3):
            lat, pd = hot_pass()
            res[f"warm{k}"] = {"lat": dist(lat), "pool_delta": pd}
        lat, pd = hot_pass()
        res["before_scan"] = {"lat": dist(lat), "pool_delta": pd}
        m0 = meta(s.conn)
        dt, r = timed(s, "SELECT COUNT(*), MIN(v), MAX(v) FROM big4")
        m1 = meta(s.conn)
        res["scan_4x"] = {"seconds": round(dt, 3), "reply": r[:100], "pool_delta": pool_delta(m0, m1),
                          "pool_resident_after": m1.get("pool_resident")}
        lat, pd = hot_pass()
        res["after_scan"] = {"lat": dist(lat), "pool_delta": pd}
        lat, pd = hot_pass()
        res["after_scan_pass2"] = {"lat": dist(lat), "pool_delta": pd}
        out["part3"] = {"hot_ids": a.hot, "stride": stride, **res, "rss": s.sample_stop()}
        s.stop()
    out["loadavg_end"] = open("/proc/loadavg").read().strip()
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    shutil.rmtree(wd, ignore_errors=True)


if __name__ == "__main__":
    main()
