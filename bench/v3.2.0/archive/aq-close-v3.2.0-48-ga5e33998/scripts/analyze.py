#!/usr/bin/env python3
"""Tables for the AQ close. Usage: analyze.py abx FILE... | analyze.py x FILE...
FILEs are the phase JSONs under out/ (paired by run index across files)."""
import json, statistics as st, sys
med = st.median
R = "/home/cdkbs/bench-runs/aq-close/out/"
SHAPE_ORDER = ["select-pk", "insert", "update-range", "plain-insert", "cab-insert", "plain-select", "cab-select",
               "x-insert-plain", "x-insert", "x-walk", "x-select"]


def runs_of(files):
    out = []
    for f in files:
        out += json.load(open(R + f + ".json"))["runs"]
    return out


def cells(runs, arms):
    c = {}
    for ri, r in enumerate(runs):
        for a in r["arms"]:
            c.setdefault((a["shape"], a["path"], a["bin"]), []).append(a)
    return c


def f2(v, d=2):
    return f"{v:.{d}f}"


def abx(files, label):
    runs = runs_of(files)
    c = cells(runs, ("A", "A2", "B"))
    shapes = [s for s in SHAPE_ORDER if any(k[0] == s for k in c)]
    print(f"\n#### {label}: server CPU per statement, us\n")
    print("| shape | path | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B below A | A' - A typical / largest abs gap | verdict |\n|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            v = {arm: [x["server_cpu_us"] for x in c[(sh, path, arm)]] for arm in ("A", "A2", "B")}
            dBA = [b - a for a, b in zip(v["A"], v["B"])]
            dAA = [abs(b - a) for a, b in zip(v["A"], v["A2"])]
            verdict = "inside" if abs(med(dBA)) <= max(dAA) else "OUTSIDE"
            print(f"| {sh} | {path} | {len(dBA)} | {f2(med(v['A']))} | {f2(med(v['A2']))} | {f2(med(v['B']))} | {f2(med(dBA))} [{f2(min(dBA))}, {f2(max(dBA))}] | "
                  f"{100*med(dBA)/med(v['A']):+.1f} % | {sum(1 for d in dBA if d < 0)} of {len(dBA)} | {f2(med(dAA))} / {f2(max(dAA))} | {verdict} |")
    print(f"\n#### {label}: statement wall latency, us (median over runs of each run's percentile); mean-derived QPS; CPU split\n")
    print("| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | QPS at mean (derived) | server CPU us | driver CPU us | wall mean - server CPU us (client, socket, scheduling) |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            for arm in ("A", "A2", "B"):
                L = c[(sh, path, arm)]
                m = lambda k: med([x[k] for x in L])
                print(f"| {sh} | {path} | {arm.replace('A2', 'A_')} | {sum(x['ops'] for x in L)} | {m('p0_us'):.1f} | {m('p25_us'):.1f} | {m('p50_us'):.1f} | {m('p90_us'):.1f} | {m('p99_us'):.1f} | {1e6/m('mean_us'):.0f} | {m('server_cpu_us'):.1f} | {m('driver_cpu_us'):.1f} | {m('mean_us')-m('server_cpu_us'):.1f} |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    unclean = [(i, a["shape"], a["path"], a["bin"]) for i, r in enumerate(runs) for a in r["arms"] if a.get("host", {}).get("clean") is False]
    print(f"\nbad replies: {bad}; unclean windows: {len(unclean)} {unclean[:8]}; n runs {len(runs)}")
    for i, r in enumerate(runs):
        es = r.get("end_state", {})
        print(f"run {i} end_state:", {k: {kk: vv for kk, vv in v.items() if kk != 'cabins'} for k, v in es.items()})
    print("quiet gate loads:", [r["quiet_gate"]["load"] for r in runs])


def cabins(files):
    runs = runs_of(files)
    for i, r in enumerate(runs[:1]):
        for arm, es in r.get("end_state", {}).items():
            for row in es.get("cabins", []):
                print(arm, {k: row[k] for k in row if k in ("on", "relation", "table", "column", "shape", "key", "observed", "entries", "hits", "misses")} or row)


def xonly(files, label):
    runs = runs_of(files)
    c = cells(runs, ("B", "B2"))
    print(f"\n#### {label}: B only, server CPU per statement, us; B2 = the same B binary started a second time (noise floor)\n")
    print("| shape | path | n runs | B | B2 | B2 - B paired, median [min, max] | largest abs gap |\n|---|---|---|---|---|---|---|")
    shapes = [s for s in SHAPE_ORDER if any(k[0] == s for k in c)]
    for sh in shapes:
        for path in ("literal", "prepared"):
            b = [x["server_cpu_us"] for x in c[(sh, path, "B")]]
            b2 = [x["server_cpu_us"] for x in c[(sh, path, "B2")]]
            d = [y - x for x, y in zip(b, b2)]
            print(f"| {sh} | {path} | {len(b)} | {f2(med(b))} | {f2(med(b2))} | {f2(med(d))} [{f2(min(d))}, {f2(max(d))}] | {f2(max(abs(x) for x in d))} |")
    print(f"\n#### {label}: latency (B and B2 pooled per run: median of the two arms' percentiles), us; QPS derived from the mean\n")
    print("| shape | path | ops (both arms) | p0 | p25 | p50 | p90 | p99 | QPS at mean (derived) | server CPU us | driver CPU us | wall mean - server CPU us |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in shapes:
        for path in ("literal", "prepared"):
            L = c[(sh, path, "B")] + c[(sh, path, "B2")]
            m = lambda k: med([x[k] for x in L])
            print(f"| {sh} | {path} | {sum(x['ops'] for x in L)} | {m('p0_us'):.1f} | {m('p25_us'):.1f} | {m('p50_us'):.1f} | {m('p90_us'):.1f} | {m('p99_us'):.1f} | {1e6/m('mean_us'):.0f} | {m('server_cpu_us'):.1f} | {m('driver_cpu_us'):.1f} | {m('mean_us')-m('server_cpu_us'):.1f} |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    print(f"\nbad replies: {bad}; n runs {len(runs)}")


if __name__ == "__main__":
    mode, files = sys.argv[1], sys.argv[2:]
    if mode == "abx":
        abx(files, "+".join(files))
    elif mode == "x":
        xonly(files, "+".join(files))
    elif mode == "cabins":
        cabins(files)
