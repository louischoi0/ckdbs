#!/usr/bin/env python3
"""Reads raw/r100-n*-run*.json and prints the results tables (markdown).
Per cell and arm the figure is the median over runs of that run's statistic."""
import glob, json, os, re, statistics as st, sys

RAW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "raw")
SIZES = [200, 1000, 10000, 100000]
SHAPES = ["range100", "win100-rows", "between100"]
ARMS = ["A", "Ap", "B", "PG"]
runs = {}
for f in sorted(glob.glob(os.path.join(RAW, "r100-n*-run*.json"))):
    j = json.load(open(f))
    if "host_after" not in j:
        continue
    for c in j["cells"]:
        runs.setdefault((c["rows"], c["shape"]), []).append((j["run"], c))


def med(n, s, arm, key):
    return st.median(c["arms"][arm][key] for _, c in runs[(n, s)])


def per_run(n, s, arm, key):
    return [round(c["arms"][arm][key], 1) for _, c in sorted(runs[(n, s)], key=lambda x: x[0])]


def main():
    print("## Headline: p50 wall (us) and server CPU (us/stmt), median of runs\n")
    print("| rows | shape | runs | A cpu | A' cpu | B cpu | PG cpu | B/A cpu | B/PG cpu | A p50 | A' p50 | B p50 | PG p50 | B/A p50 | B/PG p50 | A-A' cpu | B-A cpu |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for n in SIZES:
        for s in SHAPES:
            if (n, s) not in runs:
                continue
            k = len(runs[(n, s)])
            m = {a: (med(n, s, a, "server_cpu_us"), med(n, s, a, "p50_us")) for a in ARMS}
            print(f"| {n} | {s} | {k} | " + " | ".join(f"{m[a][0]:.1f}" for a in ARMS) +
                  f" | {m['B'][0]/m['A'][0]:.3f} | {m['B'][0]/m['PG'][0]:.2f} | " +
                  " | ".join(f"{m[a][1]:.1f}" for a in ARMS) +
                  f" | {m['B'][1]/m['A'][1]:.3f} | {m['B'][1]/m['PG'][1]:.2f} | "
                  f"{m['Ap'][0]-m['A'][0]:+.1f} | {m['B'][0]-m['A'][0]:+.1f} |")
    print("\n## Throughput (derived 1e6 / p50 wall), stmts/s\n")
    print("| rows | shape | A | A' | B | PG |")
    print("|---|---|---|---|---|---|")
    for n in SIZES:
        for s in SHAPES:
            if (n, s) in runs:
                print(f"| {n} | {s} | " + " | ".join(f"{1e6/med(n, s, a, 'p50_us'):,.0f}" for a in ARMS) + " |")
    print("\n## Distribution (median over runs), us\n")
    print("| rows | shape | arm | ops/run | p0 | p25 | p50 | p95 | p99 |")
    print("|---|---|---|---|---|---|---|---|---|")
    for n in SIZES:
        for s in SHAPES:
            if (n, s) in runs:
                for a in ARMS:
                    ops = runs[(n, s)][0][1]["arms"][a]["ops"]
                    print(f"| {n} | {s} | {a} | {ops} | " + " | ".join(
                        f"{med(n, s, a, k):.1f}" for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")) + " |")
    print("\n## Per-run server CPU (us/stmt): A | A' | B | PG, by run\n")
    for n in SIZES:
        for s in SHAPES:
            if (n, s) in runs:
                print(f"- {n} {s}: " + "; ".join(f"{a}={per_run(n, s, a, 'server_cpu_us')}" for a in ARMS))
    print("\n## Host per cell (loadavg at start, CPU 2 / CPU 4 busy %, other-session processes seen)\n")
    for n in SIZES:
        for s in SHAPES:
            if (n, s) in runs:
                row = []
                for r, c in sorted(runs[(n, s)], key=lambda x: x[0]):
                    b = c["cpu_busy_pct"]
                    g = lambda i: b.get(str(i), b.get(i))
                    comp = [x for x in c["host_before"]["competing"] if "r100" not in x]
                    ct = any("ctest" in x or "kds_tests" in x for x in comp)
                    row.append(f"r{r}: load {c['host_before']['loadavg'].split()[0]}, c2 {g(2):.0f}, c4 {g(4):.0f}, others-busy {sum(g(i) for i in (0,1,3,5,6,7))/6:.0f}")
                print(f"- {n} {s}: " + " | ".join(row))
    print("\nbad replies:", sum(c["arms"][a]["bad"] for v in runs.values() for _, c in v for a in ARMS))


main()


def paired():
    print("\n## Paired per-run ratios (server CPU): A'/A (null) and B/A, min..max over runs; quiet run 4 alone\n")
    print("| rows | shape | A'/A min..max | B/A min..max | run-4 (quiet) A'/A | run-4 B/A | run-4 B/PG cpu | run-4 B/A p50 wall | run-4 A cpu us | run-4 B cpu us |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for n in SIZES:
        for s in SHAPES:
            cs = {r: c for r, c in runs[(n, s)]}
            nul = [c["arms"]["Ap"]["server_cpu_us"] / c["arms"]["A"]["server_cpu_us"] for c in cs.values()]
            ba = [c["arms"]["B"]["server_cpu_us"] / c["arms"]["A"]["server_cpu_us"] for c in cs.values()]
            q = cs[4]["arms"]
            print(f"| {n} | {s} | {min(nul):.3f}..{max(nul):.3f} | {min(ba):.3f}..{max(ba):.3f} | "
                  f"{q['Ap']['server_cpu_us']/q['A']['server_cpu_us']:.3f} | {q['B']['server_cpu_us']/q['A']['server_cpu_us']:.3f} | "
                  f"{q['B']['server_cpu_us']/q['PG']['server_cpu_us']:.2f} | {q['B']['p50_us']/q['A']['p50_us']:.3f} | "
                  f"{q['A']['server_cpu_us']:.1f} | {q['B']['server_cpu_us']:.1f} |")


paired()
