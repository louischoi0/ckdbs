#!/usr/bin/env python3
"""Tables for the BO close from out/w*.json (bn_census A/A'/B runs)."""
import json, os, statistics as st, sys

R = os.path.dirname(os.path.abspath(__file__)) + "/out/"
med = st.median
FILES = ["w1", "w2", "w3", "w4", "w5", "w6", "w7"]
ORDER = {"w1": "B, A', A", "w2": "A, A', B", "w3": "A', B, A", "w4": "B, A, A'",
         "w5": "A, A', B", "w6": "B, A, A'", "w7": "A', B, A", "w8": "A, B, A'"}
from math import comb


def runs_of(n):
    p = R + n + ".json"
    return json.load(open(p))["runs"] if os.path.exists(p) else []


def cells_of(files):
    runs = []
    for f in files:
        runs += runs_of(f)
    cells = {}
    for r in runs:
        for a in r["arms"]:
            cells.setdefault((a["shape"], a["path"], a["bin"]), []).append(a)
    return runs, cells


def row(name, A, A2, B, per_stmt=True):
    dBA = [y - x for x, y in zip(A, B)]
    dAA = [y - x for x, y in zip(A, A2)]
    aAA = [abs(d) for d in dAA]
    tps = lambda v: 1e6 / med(v)
    verdict = "inside" if abs(med(dBA)) <= max(aAA) else "**OUTSIDE**"
    sign = "B slower" if med(dBA) > 0 else "B faster"
    return (f"| {name} | {len(A)} | {tps(A):,.0f} /s | {tps(A2):,.0f} /s | {tps(B):,.0f} /s | "
            f"{med(dBA):+.2f} us ({100 * med(dBA) / med(A):+.1f} %, {sign}) [{min(dBA):+.2f}, {max(dBA):+.2f}] us | "
            f"{sum(1 for d in dBA if d > 0)} of {len(A)} | {med(aAA):.2f} / {max(aAA):.2f} us | {verdict} |")


def main():
    runs, cells = cells_of(FILES)
    shapes = ["select-pk", "insert", "delete-pk", "update-range"]
    print("#### headline (pooled over all invocations); server CPU per statement\n")
    print("| cell | runs | A | A' | B | B - A paired median [min, max] | B above A | A' - A typical / largest abs gap | verdict |\n|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            if (sh, path, "A") not in cells:
                continue
            v = {a: [x["server_cpu_us"] for x in cells[(sh, path, a)]] for a in ("A", "A2", "B")}
            print(row(f"{sh} {path}", v["A"], v["A2"], v["B"]))
    print("\n#### sign tests, per-run paired server CPU: B - A, B - A', A' - A, and B against the mean of A and A'\n")
    print("| cell | runs | B - A median | B above A | B - A' median | B above A' | A' - A median | A' above A | B - mean(A, A') median | B above mean | one-sided sign-test p, B above mean |\n|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            if (sh, path, "A") not in cells:
                continue
            v = {a: [x["server_cpu_us"] for x in cells[(sh, path, a)]] for a in ("A", "A2", "B")}
            n = len(v["A"])
            dBA = [b - a for a, b in zip(v["A"], v["B"])]
            dBA2 = [b - a for a, b in zip(v["A2"], v["B"])]
            dAA = [b - a for a, b in zip(v["A"], v["A2"])]
            dM = [b - (a + c) / 2 for a, c, b in zip(v["A"], v["A2"], v["B"])]
            k = sum(1 for d in dM if d > 0)
            p = sum(comb(n, i) for i in range(k, n + 1)) / 2 ** n
            print(f"| {sh} {path} | {n} | {med(dBA):+.2f} us | {sum(d > 0 for d in dBA)} of {n} | {med(dBA2):+.2f} us | {sum(d > 0 for d in dBA2)} of {n} | {med(dAA):+.2f} us | {sum(d > 0 for d in dAA)} of {n} | {med(dM):+.2f} us ({100 * med(dM) / med(v['A']):+.1f} %) | {k} of {n} | {p:.4f} |")
    print("\n#### by invocation: B - A and A' - A, paired median [min, max], us of server CPU\n")
    print("| file | start order | shape | path | runs | A | A' | B | B - A | A' - A |\n|---|---|---|---|---|---|---|---|---|---|")
    for f in FILES:
        rs = runs_of(f)
        if not rs:
            continue
        for sh in shapes:
            for path in ("literal", "prepared"):
                try:
                    v = {a: [next(x["server_cpu_us"] for x in r["arms"] if x["shape"] == sh and x["path"] == path and x["bin"] == a) for r in rs] for a in ("A", "A2", "B")}
                except StopIteration:
                    continue
                dB = [y - x for x, y in zip(v["A"], v["B"])]
                dA = [y - x for x, y in zip(v["A"], v["A2"])]
                print(f"| {f} | {ORDER[f]} | {sh} | {path} | {len(rs)} | {med(v['A']):.2f} | {med(v['A2']):.2f} | {med(v['B']):.2f} | {med(dB):+.2f} [{min(dB):+.2f}, {max(dB):+.2f}] | {med(dA):+.2f} [{min(dA):+.2f}, {max(dA):+.2f}] |")
    print("\n#### statement wall latency (client side), us; median over runs of each run's percentile\n")
    print("| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU | driver CPU |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            for arm in ("A", "A2", "B"):
                if (sh, path, arm) not in cells:
                    continue
                L = cells[(sh, path, arm)]
                m = lambda k: med([x[k] for x in L])
                print(f"| {sh} | {path} | {arm.replace('A2', chr(65) + chr(39))} | {sum(x['ops'] for x in L)} | {m('p0_us'):.1f} us | {m('p25_us'):.1f} us | {m('p50_us'):.1f} us | {m('p90_us'):.1f} us | {m('p99_us'):.1f} us | {1e6 / m('mean_us'):,.0f} /s | {m('server_cpu_us'):.2f} us | {m('driver_cpu_us'):.2f} us |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    es = all(all(v.values()) for r in runs for v in r.get("end_state", {}).values())
    unclean = [(i, a["shape"], a["path"], a["bin"]) for i, r in enumerate(runs) for a in r["arms"] if not a["host"].get("clean", True)]
    print(f"\nbad replies: {bad}; end_state all ok: {es}; runs: {len(runs)}; unclean cells: {len(unclean)} {unclean[:6]}")
    # idle burn
    for k in ("idle_cpu_ns_per_s", "idle"):
        pass


main()
