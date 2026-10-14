#!/usr/bin/env python3
import json, os, statistics as st, sys
R = "/home/cdkbs/bench-runs/bw-update-fix/out/"
med = st.median
ARMS = ("A", "A2", "B", "C")
def load(n):
    p = R + n + ".json"
    return json.load(open(p))["runs"] if os.path.exists(p) else []
def gap(a, b): return [y - x for x, y in zip(a, b)]
f2 = lambda v: f"{v:.2f}"
def row(name, V, unit_dec=2):
    A, A2, B, C = (V[a] for a in ARMS)
    out = f"| {name} | {len(A)} | {med(A):.2f} | {med(A2):.2f} | {med(B):.2f} | {med(C):.2f} |"
    maa = [abs(x) for x in gap(A, A2)]
    for tag, X in (("B-A", B), ("C-A", C), ("C-B", None)):
        d = gap(B, C) if X is None else gap(A, X)
        base = med(B) if X is None else med(A)
        out += f" {med(d):+.2f} [{min(d):+.1f}, {max(d):+.1f}] ({100*med(d)/base:+.1f} %) |"
    out += f" {med(maa):.2f} / {max(maa):.2f} |"
    return out
HDR = ("| cell | n runs | A0 | A0' | B | C | B - A0 paired median [min, max] (% of A0) | C - A0 | C - B (% of B) | A0' - A0 typical / largest abs gap |\n|---|---|---|---|---|---|---|---|---|---|")
def tables(files, label):
    runs = []
    for f in files: runs += load(f)
    cells = {}
    for r in runs:
        for a in r["arms"]:
            cells.setdefault((a["shape"], a["path"], a["bin"]), []).append(a)
    shapes = sorted({k[0] for k in cells})
    print(f"\n#### {label}: server CPU per statement, us\n")
    print(HDR)
    for sh in shapes:
        for path in ("literal", "prepared"):
            v = {arm: [x["server_cpu_us"] for x in cells[(sh, path, arm)]] for arm in ARMS}
            print(row(f"{sh} {path}", v))
    print(f"\n#### {label}: statement wall latency, us (median over runs of each run's percentile)\n")
    print("| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            for arm in ARMS:
                L = cells[(sh, path, arm)]
                m = lambda k: med([x[k] for x in L])
                print(f"| {sh} | {path} | {arm} | {sum(x['ops'] for x in L)} | {m('p0_us'):.1f} | {m('p25_us'):.1f} | {m('p50_us'):.1f} | {m('p90_us'):.1f} | {m('p99_us'):.1f} | {1e6/m('mean_us'):.0f} | {m('server_cpu_us'):.1f} | {m('driver_cpu_us'):.1f} |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    unclean = sum(1 for r in runs for a in r["arms"] if a.get("host", {}).get("clean") is False)
    tot = sum(1 for r in runs for a in r["arms"])
    es = all(v for r in runs for v in [all(all(x.values()) for x in r["end_state"].values())])
    loads = [r["host_before"]["loadavg"].split()[0] for r in runs]
    print(f"\nn runs {len(runs)}; bad replies {bad}; end_state all ok {es}; unclean cells {unclean} of {tot}; loadavg before each run: {loads}")
if __name__ == "__main__":
    fs = [f for f in ("u1", "u2") if load(f)]
    tables(fs, "u1+u2 pooled, 1,000 rows")
    for f in fs: tables([f], f"{f} alone")
