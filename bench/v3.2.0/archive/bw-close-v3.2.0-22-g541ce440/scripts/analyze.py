#!/usr/bin/env python3
import json, os, statistics as st, sys
R = "/home/cdkbs/bench-runs/bw-close/out/"
med = st.median
def load(n):
    p = R + n + ".json"
    return json.load(open(p))["runs"] if os.path.exists(p) else []

def gap(a, b):  # per-run lists
    d = [y - x for x, y in zip(a, b)]
    return d

def summ(name, A, A2, B, unit, pct=True, dec=2):
    dBA = gap(A, B); dAA = [abs(x) for x in gap(A, A2)]
    f = lambda v: f"{v:.{dec}f}"
    below = sum(1 for d in dBA if d < 0)
    return (f"| {name} | {len(A)} | {f(med(A))} | {f(med(A2))} | {f(med(B))} | {f(med(dBA))} [{f(min(dBA))}, {f(max(dBA))}] | "
            f"{100*med(dBA)/med(A):+.1f} % | {below} of {len(A)} | {f(med(dAA))} / {f(max(dAA))} | {'inside' if abs(med(dBA)) <= max(dAA) else 'outside'} |")

HDR = ("| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest |gap| | B - A vs largest gap |\n"
       "|---|---|---|---|---|---|---|---|---|---|")

def scan_tables(files, label):
    runs = []
    for f in files: runs += load(f)
    out = []
    cells = {}
    for r in runs:
        for a in r["arms"]:
            cells.setdefault((a["cell"], a["path"], a["arm"]), []).append(a)
    # net row
    relof = {}
    for r in runs:
        for a in r["arms"]:
            if a["kind"] == "scan": relof[a["cell"]] = (a["rel"], a["rows"])
    print(f"\n#### {label}: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us\n")
    print(HDR)
    for cell, (rel, rows) in relof.items():
        for path in ("literal", "prepared"):
            vals = {}
            for arm in ("A", "A2", "B"):
                sc = cells[(cell, path, arm)]; pt = cells[("point:" + rel, path, arm)]
                vals[arm] = [(s["server_cpu_us"] - p["server_cpu_us"]) / rows * 1000 for s, p in zip(sc, pt)]
            print(summ(f"{cell} ({rows}) net row ns {path}", vals["A"], vals["A2"], vals["B"], "ns"))
    for cell, (rel, rows) in relof.items():
        for path in ("literal", "prepared"):
            v = {arm: [p["server_cpu_us"] for p in cells[("point:" + rel, path, arm)]] for arm in ("A", "A2", "B")}
            print(summ(f"point:{rel} CPU us {path}", v["A"], v["A2"], v["B"], "us"))
    # percentiles
    print(f"\n#### {label}: latency of one scan (wall, client side), us; median over runs of each run's percentile; ops = total scans over runs\n")
    print("| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan | driver CPU us/scan |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
    for cell in relof:
        for path in ("literal", "prepared"):
            for arm in ("A", "A2", "B"):
                L = cells[(cell, path, arm)]
                m = lambda k: med([x[k] for x in L])
                print(f"| {cell} | {path} | {arm} | {sum(x['ops'] for x in L)} | {m('p_p0'):.1f} | {m('p_p25'):.1f} | {m('p_p50'):.1f} | {m('p_p95'):.1f} | {m('p_p99'):.1f} | {1e6/m('p_p50'):.0f} | {m('server_cpu_us'):.1f} | {m('driver_cpu_us'):.1f} |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    wait = sum(a["window_wait_us"] for r in runs for a in r["arms"])
    miss = sum(a["pool_misses"] for r in runs for a in r["arms"] if a["kind"] == "scan")
    dig = set()
    for r in runs:
        for c, d in r["digest"].items():
            dig.add((c, len(set(d.values()))))
    print(f"\nbad replies: {bad}; window_wait_us total: {wait}; scan-cell pool_misses total: {miss}; digest arms-distinct per cell: {sorted(dig)}; n runs {len(runs)}")
    unclean = [(i, a["cell"], a["path"], a["arm"]) for i, r in enumerate(runs) for a in r["arms"] if a.get("clean") is False]
    print(f"unclean windows: {len(unclean)} {unclean[:6]}")

def shape_tables(files, label):
    runs = []
    for f in files: runs += load(f)
    cells = {}
    for r in runs:
        for a in r["arms"]:
            cells.setdefault((a["shape"], a["path"], a["bin"]), []).append(a)
    shapes = sorted({k[0] for k in cells})
    print(f"\n#### {label}: server CPU per statement, us (an insert/select-pk statement; update-range = one every-row UPDATE of 1,000 rows)\n")
    print(HDR)
    for sh in shapes:
        for path in ("literal", "prepared"):
            v = {arm: [x["server_cpu_us"] for x in cells[(sh, path, arm)]] for arm in ("A", "A2", "B")}
            print(summ(f"{sh} CPU us {path}", v["A"], v["A2"], v["B"], "us"))
    print(f"\n#### {label}: statement wall latency, us; median over runs of each run's percentile\n")
    print("| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            for arm in ("A", "A2", "B"):
                L = cells[(sh, path, arm)]
                m = lambda k: med([x[k] for x in L])
                print(f"| {sh} | {path} | {arm} | {sum(x['ops'] for x in L)} | {m('p0_us'):.1f} | {m('p25_us'):.1f} | {m('p50_us'):.1f} | {m('p90_us'):.1f} | {m('p99_us'):.1f} | {1e6/m('mean_us'):.0f} | {m('server_cpu_us'):.1f} | {m('driver_cpu_us'):.1f} |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    es = all(all(v.values()) for r in runs for k in ("end_state",) if k in r for v in r[k].values())
    print(f"\nbad replies: {bad}; end_state all ok: {es}; n runs {len(runs)}")
    pa = [r.get("pages-unspilled") for r in runs]
    print("pages_allocated_delta (unspilled) per arm per run:", [{k: v["pages_allocated_delta"] for k, v in p.items()} for p in pa if p][:3])

if __name__ == "__main__":
    scan_tables([f for f in ("s1", "s2") if load(f)], "scan 10,000 rows, s1+s2 pooled")
    for f in ("s1", "s2"):
        if load(f): scan_tables([f], f"scan 10,000 rows, {f} alone")
    if load("z"): scan_tables(["z"], "scan 200 / 1,000 rows")
    shape_tables([f for f in ("u1", "u2") if load(f)], "u1+u2 pooled, 1,000 rows")
    for f in ("u1", "u2"):
        if load(f): shape_tables([f], f"{f} alone")
