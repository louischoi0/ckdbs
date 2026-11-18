#!/usr/bin/env python3
"""Tables for the DF-S0 spike run from out/w*.json (bn_census arms A, A2, B, C).
usage: analyze.py OUTDIR [w1 w2 ...]   (default: every wN.json in OUTDIR)"""
import glob, json, os, statistics as st, sys
from math import comb

D = sys.argv[1].rstrip("/") + "/"
FILES = sys.argv[2:] or sorted(os.path.basename(p)[:-5] for p in glob.glob(D + "w*.json"))
med = st.median
BAR = 0.5


def runs_of(n):
    return json.load(open(D + n + ".json"))["runs"]


runs = []
for f in FILES:
    runs += runs_of(f)
# A (run, shape, path) cell enters the paired tables only if every arm's host record is clean
# (the driver's own gate: clean = true); with STRICT=1 it must also name no competing task.
STRICT = os.environ.get("STRICT") == "1"
cells = {}
dropped = []
for i, r in enumerate(runs):
    keys = {(a["shape"], a["path"]) for a in r["arms"]}
    for k in sorted(keys):
        arms = [a for a in r["arms"] if (a["shape"], a["path"]) == k]
        ok = all(a["host"].get("clean", True) and not (STRICT and a["host"].get("competing")) for a in arms)
        if not ok:
            dropped.append((i, k))
            continue
        for a in arms:
            cells.setdefault((a["shape"], a["path"], a["bin"]), []).append(a)
SH = ["select-pk", "update-expr"]
PA = ["literal", "prepared"]


def V(sh, pa, arm, key="server_cpu_us"):
    return [x[key] for x in cells[(sh, pa, arm)]]


def delta(x, y):
    return [b - a for a, b in zip(x, y)]


def summ(d):
    return f"{med(d):+.2f} [{min(d):+.2f}, {max(d):+.2f}] ({sum(1 for x in d if x > 0)} of {len(d)})"


print("#### headline: paired per-run server CPU us per statement, median [min, max] (runs with the first arm's cost exceeded by the second)\n")
print("| cell | runs | A | B - A | C - A | B - C | A2 - A typical / largest abs | B - mean(A,A2) | C - mean(A,A2) | verdict B - A vs 0.5 us |\n|---|---|---|---|---|---|---|---|---|---|")
for sh in SH:
    for pa in PA:
        A, A2, B, C = (V(sh, pa, a) for a in ("A", "A2", "B", "C"))
        n = len(A)
        dBA, dCA, dBC, dAA = delta(A, B), delta(A, C), delta(C, B), delta(A, A2)
        aAA = [abs(x) for x in dAA]
        m = [(a + c) / 2 for a, c in zip(A, A2)]
        dBm, dCm = delta(m, B), delta(m, C)
        res = max(aAA)
        # verdict: the cell's median B - A, widened by the run's resolution (largest |A2 - A|), against the bar
        if med(dBA) + res <= BAR:
            verdict = "inside"
        elif med(dBA) - res > BAR:
            verdict = "over"
        else:
            verdict = "not resolvable (median %+.2f +/- %.2f straddles %.1f)" % (med(dBA), res, BAR)
        print(f"| {sh} {pa} | {n} | {1e6 / med(A):,.0f} /s ({med(A):.2f} us) | {summ(dBA)} | {summ(dCA)} | {summ(dBC)} | {med(aAA):.2f} / {max(aAA):.2f} | {med(dBm):+.2f} | {med(dCm):+.2f} | {verdict} |")
print("\n#### the control: A2 - A and sign tests (one-sided, unadjusted)\n")
print("| cell | A2 - A median [min, max] | A2 above A | B - mean(A,A2) above 0 | p | C - mean above 0 | p |\n|---|---|---|---|---|---|---|")
for sh in SH:
    for pa in PA:
        A, A2, B, C = (V(sh, pa, a) for a in ("A", "A2", "B", "C"))
        n = len(A)
        m = [(a + c) / 2 for a, c in zip(A, A2)]
        out = []
        for X in (B, C):
            k = sum(1 for d in delta(m, X) if d > 0)
            out.append((k, sum(comb(n, i) for i in range(k, n + 1)) / 2 ** n))
        print(f"| {sh} {pa} | {summ(delta(A, A2))} | {sum(1 for d in delta(A, A2) if d > 0)} of {n} | {out[0][0]} of {n} | {out[0][1]:.4f} | {out[1][0]} of {n} | {out[1][1]:.4f} |")
print("\n#### client-side wall latency per statement, us (median over runs of each run's percentile)\n")
print("| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
for sh in SH:
    for pa in PA:
        for arm in ("A", "A2", "B", "C"):
            L = cells[(sh, pa, arm)]
            m = lambda k: med([x[k] for x in L])
            print(f"| {sh} | {pa} | {arm} | {sum(x['ops'] for x in L)} | {m('p0_us'):.1f} | {m('p25_us'):.1f} | {m('p50_us'):.1f} | {m('p90_us'):.1f} | {m('p99_us'):.1f} | {1e6 / m('mean_us'):,.0f} /s | {m('server_cpu_us'):.2f} | {m('driver_cpu_us'):.2f} |")
bad = sum(a["bad"] for r in runs for a in r["arms"])
unclean = [(i, a["shape"], a["path"], a["bin"]) for i, r in enumerate(runs) for a in r["arms"] if not a["host"].get("clean", True)]
comp = []
print(f"\nbad replies: {bad}; runs: {len(runs)}; unclean cells: {len(unclean)} {unclean[:8]}; cells dropped as not clean (strict={STRICT}): {len(dropped)} {dropped}")
