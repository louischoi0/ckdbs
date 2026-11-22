import json, glob, statistics as st
R = "/home/louis/bench-runs/dp-close/archive/q1b"
for shape in ("point", "insert1", "pkmix"):
    d = {}
    for arm in ("A", "A2", "B"):
        d[arm] = []
        for r in range(1, 7):
            f = glob.glob(f"{R}/k1-{shape}-{arm}-r{r}/*.json")
            d[arm].append(json.load(open(f[0])) if f else None)
    n = min(sum(1 for j in d[a] if j) for a in d)
    ok = [r for r in range(6) if all(d[a][r] for a in d)]
    t = {a: [d[a][r]["statements_per_s"] for r in ok] for a in d}
    m = {a: st.median(t[a]) for a in t}
    pb = [100 * (b / a - 1) for a, b in zip(t["A"], t["B"])]
    pa = [100 * (b / a - 1) for a, b in zip(t["A"], t["A2"])]
    print(f"{shape}: runs {len(ok)}")
    for a in d:
        print("  ", a, f"median {m[a]:,.0f}/s min {min(t[a]):,.0f} max {max(t[a]):,.0f} spread {100*(max(t[a])-min(t[a]))/m[a]:.1f}%",
              "p0/p25/p50/p95/p99 median us",
              [round(st.median([d[a][r][k] for r in ok]), 1) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")],
              "stmts", sum(d[a][r]["statements"] for r in ok), "err", sum(d[a][r]["errors"] for r in ok))
    print(f"   B/A-1 paired median {st.median(pb):+.2f}% [{min(pb):+.2f},{max(pb):+.2f}]   A2/A-1 (floor) {st.median(pa):+.2f}% [{min(pa):+.2f},{max(pa):+.2f}]  B/A2-1 median-of-medians {100*(m['B']/m['A2']-1):+.2f}%")
    print("   load before:", sorted({j['host_before']['loadavg'].split()[0] if isinstance(j['host_before'], dict) else str(j['host_before']).split()[0] for a in d for j in d[a] if j}))
