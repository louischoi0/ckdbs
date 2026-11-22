import json, glob, os, statistics as st, sys
R = os.path.expanduser("~/bench-runs/dp-close/archive/q1")
rows = []
for k in (1, 2):
    for shape in ("point", "insert1", "pkmix"):
        d = {}
        for arm in "AB":
            for r in (1, 2, 3):
                f = glob.glob(f"{R}/k{k}-{shape}-{arm}-r{r}/*.json")
                if not f:
                    continue
                j = json.load(open(f[0]))
                d.setdefault(arm, []).append(j)
        if len(d) < 2:
            continue
        def s(arm, key):
            return [j[key] for j in d[arm]]
        a, b = s("A", "statements_per_s"), s("B", "statements_per_s")
        ma, mb = st.median(a), st.median(b)
        # floor: A's own spread (max-min) of 3 runs, and B's
        fa, fb = (max(a) - min(a)), (max(b) - min(b))
        delta = mb - ma
        print(f"k={k} {shape:8s} A {[round(x) for x in a]} med {ma:,.0f}/s spread {fa:,.0f} ({100*fa/ma:.1f}%) | "
              f"B {[round(x) for x in b]} med {mb:,.0f}/s spread {fb:,.0f} ({100*fb/mb:.1f}%) | "
              f"B-A {delta:+,.0f}/s ({100*delta/ma:+.1f}%) | mean {st.mean(b)/st.mean(a)*100-100:+.1f}%")
        for key in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us"):
            print("     ", key, "A", [x for x in s("A", key)], "B", [x for x in s("B", key)])
        print("      errors", sum(s("A", "errors")), sum(s("B", "errors")), "refusals", sum(s("A", "refusals")), sum(s("B", "refusals")),
              "client_bound", [j["client_bound"] for j in d["A"] + d["B"]], "stmts", [j["statements"] for j in d["A"]+d["B"]])
        print("      load before", [j["host_before"].get("loadavg") if isinstance(j["host_before"], dict) else j["host_before"] for j in d["A"] + d["B"]])
