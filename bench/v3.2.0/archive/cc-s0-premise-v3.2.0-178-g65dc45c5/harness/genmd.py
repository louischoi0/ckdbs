import json, statistics, sys
S = json.load(open(sys.argv[1] + ".summary"))
R = json.load(open(sys.argv[1]))
cpu = {(r["cell"], r["round"], r["arm"]): r["server_cpu_us"] for r in R}
label = {"P:A": "planned, spelling A", "P:B": "planned, spelling B", "W:A": "written, spelling A",
         "W:B": "written, spelling B", "W2:A": "written, spelling A, 2nd server (control)"}
SIZES = {"c1-compare-all-r200": "200", "c1-compare-all-r1k": "1,000", "c1-compare-all-r2872": "2,872 (driver default)",
         "c1-compare-all-r10k": "10,000",
         "c2-customer-statement-n200": "180 freights / 200 cargos", "c2-customer-statement-n1k": "879 / 1,000",
         "c2-customer-statement-ca10k": "8,234 / 10,000 (CA size)", "c2-customer-statement-df200k": "10,795 / 200,000 (defaults)",
         "c3-cross-join-x200": "200 stats rows", "c3-cross-join-x1k": "1,000", "c3-cross-join-x10k": "10,000",
         "c3-cross-join-x60k": "60,480 (driver default 30 y)"}
# 1. gate table
print("GATE")
print("| Cell | Rows | `planned` (P:A) | written A | written B | B saves vs planned, per pair | min over 3 pairs | Control W2 vs W:A | Gate (>= 15%) |")
print("|---|---|---|---|---|---|---|---|---|")
for c in S:
    if "W:B" not in S[c]: continue
    pa, wa, wb, w2 = (S[c][k]["cpu_median"] for k in ("P:A", "W:A", "W:B", "W2:A"))
    sp = [(cpu[(c, k, "P:A")] - cpu[(c, k, "W:B")]) / cpu[(c, k, "P:A")] * 100 for k in (1, 2, 3)]
    verdict = "PASS" if min(sp) >= 15 else "FAIL"
    print(f"| {c} | {SIZES[c]} | {pa:,.1f} us | {wa:,.1f} us | {wb:,.1f} us | {', '.join(f'{x:.1f}%' for x in sp)} | {min(sp):.1f}% | {100*(w2/wa-1):+.1f}% | {verdict} |")
# 2. matrix
print("\nMATRIX")
print("| Cell | Rows | Arm | Throughput (median of 3) | Server CPU per statement (median) | Delta CPU vs written A | Round spread of CPU | Ops per round |")
print("|---|---|---|---|---|---|---|---|")
for c in S:
    base = S[c]["W:A"]["cpu_median"]
    for arm in ("P:A", "P:B", "W:A", "W:B", "W2:A"):
        if arm not in S[c]: continue
        a = S[c][arm]
        d = "-" if arm == "W:A" else f"{100*(a['cpu_median']/base-1):+.1f}%"
        print(f"| {c} | {SIZES[c]} | {arm} | {a['qps_median']:,.0f} stmt/s | {a['cpu_median']:,.1f} us | {d} | {a['cpu_spread_pct']:.1f}% | {a['ops_per_round']:,} |")
# 3. latency
print("\nLATENCY")
print("| Cell | Arm | Statements | p0 | p25 | p50 | p95 | p99 | Server CPU / p50 |")
print("|---|---|---|---|---|---|---|---|---|")
for c in S:
    for arm in ("P:A", "W:A", "W:B", "W2:A"):
        if arm not in S[c]: continue
        a = S[c][arm]; l = a["lat"]
        print(f"| {c} | {arm} | {a['n_ops']:,} | {l['p0']:,.0f} us | {l['p25']:,.0f} us | {l['p50']:,.0f} us | {l['p95']:,.0f} us | {l['p99']:,.0f} us | {100*a['cpu_median']/l['p50']:.0f}% |")
