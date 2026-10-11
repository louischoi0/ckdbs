import json, re, glob, ast
R = "/home/cdkbs/bench-runs/at"
m = json.load(open(f"{R}/res/analysis.json"))["cells"]
s = json.load(open(f"{R}/res-swap/analysis.json"))["cells"]

# host check
rows = []
for res in ("res", "res-swap"):
    for f in sorted(glob.glob(f"{R}/{res}/n*.host.txt")):
        t = open(f).read()
        la = float(re.search(r"loadavg_start=([\d.]+)", t).group(1))
        bf = ast.literal_eval(re.search(r"cpu_busy_during=(\{.*\})", t).group(1))
        q = ast.literal_eval(re.search(r"cpu_busy_1s_before=(\{.*\})", t).group(1))
        comp = re.search(r"competing_compile_or_ctest=(.*)", t).group(1)
        oth = max(v for k, v in bf.items() if k not in (6, 7))
        othq = max(v for k, v in q.items() if k not in (6, 7))
        rows.append((res, f.split("/")[-1], la, oth, othq, comp, bf[6], bf[7]))
print("host runs", len(rows), "load", min(r[2] for r in rows), max(r[2] for r in rows),
      "others max during", max(r[3] for r in rows), "others max before", max(r[4] for r in rows),
      "competing", sum(1 for r in rows if r[5] != "none"),
      "cpu6", min(r[6] for r in rows), max(r[6] for r in rows), "cpu7", min(r[7] for r in rows), max(r[7] for r in rows))

names = {"1000|insert-assert-existing": "INSERT, assertion, existing group (1000 rows)",
         "1000|insert-none-existing": "INSERT, no assertion (control)",
         "1000|insert-none-again": "INSERT, no assertion, repeated (noise floor)",
         "1000|select": "pk point SELECT (control, 1000 rows)",
         "10000|select-cr": "pk point SELECT (control, 10000 rows)",
         "1000|ping": "SHOW META (client/socket floor, 1000-row runs)",
         "10000|ping": "SHOW META (client/socket floor, 10000-row runs)",
         "1000|create": "CREATE ASSERTION, 1000 rows", "10000|create": "CREATE ASSERTION, 10000 rows",
         "1000|drop": "DROP ASSERTION (unchanged code), 1000 rows", "10000|drop": "DROP ASSERTION (unchanged code), 10000 rows"}
order = list(names)
print("\n## matrix")
print("| cell | A QPS | B QPS | B-A QPS | d p50 us (B-A) med [IQR] | B>A runs | swap d p50 | binary effect x us | x % |")
for k in order:
    c, w = m[k], s[k]
    qa, qb = 1e6 / c["A_mean"], 1e6 / c["B_mean"]
    qp = ""
    dm, ds = c["d_p50"]["median"], w["d_p50"]["median"]
    x = (dm - ds) / 2
    print(f"| {names[k]} | {qa:,.0f} | {qb:,.0f} | {qb-qa:+,.0f} | {dm:+.2f} [{c['d_p50']['q1']:+.2f}, {c['d_p50']['q3']:+.2f}] | {c['d_p50']['B_gt_A']}/{c['d_p50']['n']} | {ds:+.2f} [{w['d_p50']['q1']:+.2f}, {w['d_p50']['q3']:+.2f}] ({w['d_p50']['B_gt_A']}/{w['d_p50']['n']}) | {x:+.2f} | {100*x/c['A_p50']:+.2f}% |")
print("\n## dist (main pass, per-run percentile, median over runs; us)")
print("| cell | engine | ops/run | runs | p0 | p25 | p50 | p75 | p99 | engine us/op |")
for k in order:
    c = m[k]
    n = 16000 if "create" not in k and "drop" not in k else 20
    for lab, nm in (("A", "A (6b0d8412)"), ("B", "B")):
        e = c.get(f"{lab}_engine_us_per_op")
        print(f"| {names[k]} | {nm} | {n} | {c['runs']} | {c[lab+'_p0']:.1f} | {c[lab+'_p25']:.1f} | {c[lab+'_p50']:.1f} | {c[lab+'_p75']:.1f} | {c[lab+'_p99']:.1f} | {'' if 'create' in k or 'drop' in k else round(e,2)} |")
print("\n## d percentile deltas B-A (main median [q1,q3]) and swap")
for k in order:
    c, w = m[k], s[k]
    parts = []
    for p in ("p0", "p25", "p50", "p75", "p99"):
        parts.append(f"{p} {c['d_'+p]['median']:+.2f} [{c['d_'+p]['q1']:+.2f},{c['d_'+p]['q3']:+.2f}] sw {w['d_'+p]['median']:+.2f}")
    print(names[k], "|", " ; ".join(parts))
print("\n## per-run d p50 lists")
for k in order:
    print(k, "main", [round(x, 2) for x in m[k]["d_p50"]["all"]], "swap", [round(x, 2) for x in s[k]["d_p50"]["all"]])
print("\n## engine d")
for k in ("1000|insert-assert-existing", "1000|insert-none-existing", "1000|insert-none-again", "1000|select", "1000|ping", "10000|select-cr"):
    c, w = m[k], s[k]
    print(k, "A", round(c["A_engine_us_per_op"], 2), "B", round(c["B_engine_us_per_op"], 2), "d", round(c["d_engine_us_per_op"]["median"], 2), "swap d", round(w["d_engine_us_per_op"]["median"], 2))
