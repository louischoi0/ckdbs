#!/usr/bin/env python3
"""Renders the tables of the results file from analysis.json / pooled.json."""
import json, sys

RES = sys.argv[1]
an = json.load(open(f"{RES}/analysis.json"))["cells"]
pl = json.load(open(f"{RES}/pooled.json"))
SIZES = [200, 1000, 10000]
GROUPS = [
    ("Correlated subquery (the per-outer-row `EvaluateSubChain` path)",
     ["corr-exists-point", "corr-notexists-point", "corr-exists-scan"]),
    ("Uncorrelated subquery",
     ["unc-exists-point", "unc-exists-scan", "unc-in-point", "unc-in-scan", "unc-scalar-point"]),
    ("Nested uncorrelated EXISTS (the new behaviour; A drops the inner chain)",
     ["nested-none-point", "nested-none-scan", "nested-some-point", "nested-some-scan"]),
    ("Controls (no subquery)",
     ["ctl-select-pk", "ctl-select-pk-again", "ctl-insert", "ctl-ping", "ctl-scan-filter", "ctl-scan-filter-again"]),
]
SCAN = {"corr-exists-scan", "unc-exists-scan", "unc-in-scan", "nested-none-scan", "nested-some-scan",
        "ctl-scan-filter", "ctl-scan-filter-again"}


def us(x):
    if x >= 10000:
        return f"{x/1000:,.2f} ms"
    return f"{x:,.1f} µs"


def sg(x, unit="µs"):
    return f"{x:+,.1f} {unit}"


def qps(mean_us):
    return 1e6 / mean_us


def main():
    out = []
    P = out.append
    # matrix
    P("| Arm | Rows | A tps | B tps | B vs A | Runs B slower (p50) |")
    P("|---|---:|---:|---:|---:|---:|")
    for title, arms in GROUPS:
        P(f"| **{title}** | | | | | |")
        for arm in arms:
            for n in SIZES:
                c = an.get(f"{n}|{arm}")
                if not c:
                    continue
                qa, qb = qps(c["A_mean"]), qps(c["B_mean"])
                P(f"| {arm} | {n:,} | {qa:,.0f} tps | {qb:,.0f} tps | {100*(qb/qa-1):+.2f} % | {c['d_p50']['B_gt_A']}/{c['d_p50']['n']} |")
    P("")
    # delta table
    P("| Arm | Rows | A p50 | A engine per statement | B p50 | median per-run Δp50 | IQR of per-run Δp50 | median per-run Δp99 | IQR of per-run Δp99 | median Δ engine per statement | IQR Δ engine | Δp50 per outer row |")
    P("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for title, arms in GROUPS:
        for arm in arms:
            for n in SIZES:
                c = an.get(f"{n}|{arm}")
                if not c:
                    continue
                d, d9, de = c["d_p50"], c["d_p99"], c["d_engine_us_per_op"]
                per_row = f"{1000*d['median']/n:+.1f} ns" if arm in SCAN else "n/a"
                P(f"| {arm} | {n:,} | {us(c['A_p50'])} | {us(c['A_engine_us_per_op'])} | {us(c['B_p50'])} | {sg(d['median'])} | [{d['q1']:+.1f}, {d['q3']:+.1f}] µs | "
                  f"{sg(d9['median'])} | [{d9['q1']:+.1f}, {d9['q3']:+.1f}] µs | {sg(de['median'])} | [{de['q1']:+.1f}, {de['q3']:+.1f}] µs | {per_row} |")
    P("")
    # distributions
    P("| Arm | Rows | Side | Statements | p0 | p25 | p50 | p75 | p99 |")
    P("|---|---:|---|---:|---:|---:|---:|---:|---:|")
    for title, arms in GROUPS:
        for arm in arms:
            for n in SIZES:
                for lab in ("A", "B"):
                    s = pl.get(f"{n}|{arm}|{lab}")
                    if not s:
                        continue
                    P(f"| {arm} | {n:,} | {lab} | {s['n']:,} | {us(s['p0'])} | {us(s['p25'])} | {us(s['p50'])} | {us(s['p75'])} | {us(s['p99'])} |")
    open(f"{RES}/tables.md", "w").write("\n".join(out) + "\n")
    print("\n".join(out))


main()
