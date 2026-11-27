#!/usr/bin/env python3
"""Markdown tables for `tools/dx_close.py`'s run directory.

    tools/dx_close_report.py <run-dir>
"""
import json, os, sys
from statistics import median

SIZES = (200, 1000, 10000)
SHAPES = ("sel_pk", "ins", "upd_nk", "rb")
REL = {"bt": "BTREE", "hp": "HEAP"}


def main(rd):
    res = json.load(open(os.path.join(rd, "dx.json")))
    rounds = sorted({r["round"] for r in res})
    by = {(r["round"], r["arm"]): r for r in res}
    print(f"rounds: {len(rounds)}; cells per arm and round: 24\n")
    # pool counters
    bad = [(r["round"], r["arm"]) for r in res if r["pool_after"]["pool_refused"] or r["pool_after"]["pool_reclaimed_inline"]
           or r["pool_after"]["pool_reclaimed_background"] or r["pool_after"]["pool_batches_inline"]
           or r["pool_after"]["pool_batches_background"]]
    print(f"arm-rounds with any refusal or reclaim: {len(bad)} of {len(res)} {bad[:5]}")
    for arm in ("A", "A2", "B"):
        rs = [r for r in res if r["arm"] == arm]
        print(f"{arm}: pool_resident after, min-max {min(r['pool_after']['pool_resident'] for r in rs):.0f}-"
              f"{max(r['pool_after']['pool_resident'] for r in rs):.0f}; misses max {max(r['pool_after']['pool_misses'] for r in rs):.0f}; "
              f"hits {sorted({r['pool_after']['pool_hits'] for r in rs})[:3]}")
    print("\n| relation | rows | shape | A cpu us/unit | A2 cpu | B cpu | B / A, median [min-max] | control A2 / A, median [min-max] | B / A outside control |")
    print("|---|---|---|---|---|---|---|---|---|")
    allB, allC, byshape, byshapeC = [], [], {s: [] for s in SHAPES}, {s: [] for s in SHAPES}
    out = 0
    for kind in ("bt", "hp"):
        for size in SIZES:
            for shape in SHAPES:
                v = lambda arm, r: by[(r, arm)]["cells"][f"{kind}{size}"][shape]["cpu_us_med"]
                rb = [v("B", r) / v("A", r) for r in rounds]
                rc = [v("A2", r) / v("A", r) for r in rounds]
                mb, mc = median(rb), median(rc)
                o = not (min(rc) <= mb <= max(rc))
                out += o
                allB.append(mb); allC.append(mc); byshape[shape].append(mb); byshapeC[shape].append(mc)
                print(f"| {REL[kind]} | {size} | {shape} | {median(v('A', r) for r in rounds):.3f} | {median(v('A2', r) for r in rounds):.3f} | "
                      f"{median(v('B', r) for r in rounds):.3f} | {mb:.3f} [{min(rb):.3f}-{max(rb):.3f}] | {mc:.3f} [{min(rc):.3f}-{max(rc):.3f}] | {'YES' if o else 'no'} |")
    print(f"\nmedian over {len(allB)} cells: B/A {median(allB):.4f} (cells {min(allB):.3f}-{max(allB):.3f}); A2/A {median(allC):.4f} (cells {min(allC):.3f}-{max(allC):.3f}); B/A outside control in {out}")
    for s in SHAPES:
        print(f"shape {s}: B/A median over 6 cells {median(byshape[s]):.4f} [{min(byshape[s]):.3f}-{max(byshape[s]):.3f}]; control {median(byshapeC[s]):.4f} [{min(byshapeC[s]):.3f}-{max(byshapeC[s]):.3f}]")
    # per-round totals (sum of the 24 cell medians)
    print("\nper-round totals over 24 cells (sum of cpu medians): ratio B/A, A2/A")
    tb, tc = [], []
    for r in rounds:
        s = {a: sum(by[(r, a)]["cells"][f"{k}{z}"][sh]["cpu_us_med"] for k in ("bt", "hp") for z in SIZES for sh in SHAPES) for a in ("A", "A2", "B")}
        tb.append(s["B"] / s["A"]); tc.append(s["A2"] / s["A"])
    print("B/A", [round(x, 4) for x in tb], "median", round(median(tb), 4), "min-max", round(min(tb), 4), round(max(tb), 4))
    print("A2/A", [round(x, 4) for x in tc], "median", round(median(tc), 4), "min-max", round(min(tc), 4), round(max(tc), 4))
    # by position
    for arm in ("A", "A2", "B"):
        pos = {p: [] for p in range(3)}
        for r in res:
            if r["arm"] == arm:
                pos[r["order_in_round"]].append(sum(r["cells"][f"{k}{z}"][sh]["cpu_us_med"] for k in ("bt", "hp") for z in SIZES for sh in SHAPES))
        print(f"{arm} total cpu by position: " + ", ".join(f"{p}:{median(v):.1f}" for p, v in pos.items()))
    # percentile tables, client latency, median across rounds of each percentile, A vs B (and A2)
    print("\n### client latency (us), median over rounds of each phase's percentile\n")
    print("| relation | rows | shape | arm | ops/round | p0 | p25 | p50 | p95 | p99 | server cpu us | server share of p50 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for kind in ("bt", "hp"):
        for size in SIZES:
            for shape in SHAPES:
                for arm in ("A", "A2", "B"):
                    ph = [by[(r, arm)]["cells"][f"{kind}{size}"][shape] for r in rounds]
                    m = lambda k: median(p["lat"][k] for p in ph)
                    cpu = median(p["cpu_us_med"] for p in ph)
                    print(f"| {REL[kind]} | {size} | {shape} | {arm} | {ph[0]['lat']['ops']} | {m('p0'):.1f} | {m('p25'):.1f} | {m('p50'):.1f} | {m('p95'):.1f} | {m('p99'):.1f} | {cpu:.2f} | {cpu / m('p50') * 100:.0f} % |")
    print("\n### derived throughput (units/s = 1e6 / mean client latency), median over rounds, A vs B")
    for shape in SHAPES:
        a = [1e6 / by[(r, "A")]["cells"][f"{k}{z}"][shape]["lat"]["mean"] for r in rounds for k in ("bt", "hp") for z in SIZES]
        b = [1e6 / by[(r, "B")]["cells"][f"{k}{z}"][shape]["lat"]["mean"] for r in rounds for k in ("bt", "hp") for z in SIZES]
        c = [1e6 / by[(r, "A2")]["cells"][f"{k}{z}"][shape]["lat"]["mean"] for r in rounds for k in ("bt", "hp") for z in SIZES]
        print(f"{shape}: A {median(a):,.0f}/s  A2 {median(c):,.0f}/s  B {median(b):,.0f}/s  B/A {median(b) / median(a):.4f}  A2/A {median(c) / median(a):.4f}")
    print("\nhost load per arm-round (1-min), min-max:", min(float(r['host']['loadavg'].split()[0]) for r in res), max(float(r['host']['loadavg'].split()[0]) for r in res), "competing:", max(r['host']['competing'] for r in res))


if __name__ == "__main__":
    main(sys.argv[1])
