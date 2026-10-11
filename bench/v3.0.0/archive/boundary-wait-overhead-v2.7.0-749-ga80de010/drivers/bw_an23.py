#!/usr/bin/env python3
"""bw_an23.py RESDIR -> per-run summaries (RESDIR/summary.json) and a text print.

Per run (server slot): per connection and op type: n, p0/p25/p50/p75/p99/max/mean (us) over the connection's
measured span, QPS = n / (t_end - t_start); the writer's units inside the reader's [t_start, t_end] window;
the ceiling_waits / ceiling_wait_us counter deltas (instance-wide; read from the reader's SHOW META).
"""
import glob, gzip, json, os, re, statistics, sys

RES = sys.argv[1]


def nr(s, p):
    return s[max(1, -(-int(p) * len(s) // 100)) - 1]


def dist(v):
    s = sorted(v)
    return {"n": len(s), "p0": s[0], "p25": nr(s, 25), "p50": nr(s, 50), "p75": nr(s, 75), "p99": nr(s, 99),
            "max": s[-1], "mean": statistics.fmean(s)}


def metaval(m, k):
    mo = re.search(r"\b" + k + r"=([0-9.]+)", m)
    return float(mo.group(1)) if mo else None


out = {}
for hf in sorted(glob.glob(f"{RES}/*.host.json")):
    h = json.load(open(hf))
    tag = h["tag"]
    rec = {"host": h}
    reader = json.load(gzip.open(f"{RES}/{tag}.reader.json.gz", "rt"))
    writer = json.load(gzip.open(f"{RES}/{tag}.writer.json.gz", "rt"))
    win0, win1 = reader["t_start"], reader["t_end"]
    conns = {"reader": reader, "writer": writer}
    bp = f"{RES}/{tag}.bystander.json.gz"
    if os.path.exists(bp):
        conns["bystander"] = json.load(gzip.open(bp, "rt"))
    for name, d in conns.items():
        span = d["t_end"] - d["t_start"]
        c = {"core": d["want_core"], "attempts": d["connect_attempts"], "span_s": span, "errors": d["errors"],
             "first_error": d["first_error"], "ops": {}}
        tot = 0
        for k, v in d["lat"].items():
            e = dist(v)
            c["ops"][k] = e
            tot += len(v)
        if name == "writer":
            ends = d["ends"]["unit"]
            inwin = sum(1 for t in ends if win0 <= t <= win1)
            c["units_in_reader_window"] = inwin
            c["window_s"] = win1 - win0
            c["unit_qps_window"] = inwin / (win1 - win0)
            c["unit_qps_whole"] = len(ends) / span
        else:
            c["qps"] = tot / span
        rec[name] = c
    m0, m1 = reader["meta_before"], reader["meta_after"]
    rec["ceiling"] = {k: (metaval(m1, k) or 0) - (metaval(m0, k) or 0) for k in ("contention_ceiling_waits", "contention_ceiling_wait_us")}
    wm0, wm1 = writer["meta_before"], writer["meta_after"]
    rec["writer_wal"] = {k: (metaval(wm1, k) or 0) - (metaval(wm0, k) or 0) for k in ("wal_syncs",)}
    out[tag] = rec
json.dump(out, open(f"{RES}/summary.json", "w"), indent=1)
if __name__ == "__main__" and os.environ.get("BW_PRINT", "1") == "1":
    for tag, r in out.items():
        print(tag, "ceil", r["ceiling"], "wal_syncs", r["writer_wal"])
        for name in ("reader", "bystander", "writer"):
            if name not in r:
                continue
            c = r[name]
            for k, e in c["ops"].items():
                q = c.get("qps") or c.get("unit_qps_window")
                print(f"   {name:<9} core{c['core']} {k:<7} n={e['n']:<6} p0={e['p0']:.0f} p25={e['p25']:.0f} p50={e['p50']:.0f} "
                      f"p75={e['p75']:.0f} p99={e['p99']:.0f} max={e['max']:.0f} qps={q:.0f} err={c['errors']}")
