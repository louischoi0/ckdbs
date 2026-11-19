import json, statistics as st
R = [json.loads(l) for l in open("/home/louis/bench-runs/da-s0/raw.jsonl")]
for cell, cores in (("select", 1), ("select", 8), ("fk", 1)):
    rs = [r for r in R if r["cell"] == cell and r["cores"] == cores]
    print(f"== {cell} cores={cores}")
    for arm in ("on", "off"):
        a = sorted([r for r in rs if r["arm"] == arm], key=lambda r: r["round"])
        c = [r["cpu_us_per_stmt"] for r in a]
        q = [r["qps"] for r in a]
        print(arm, "cpu/stmt", [round(x, 2) for x in c], "mean %.2f range %.2f" % (st.mean(c), max(c) - min(c)))
        print(arm, "qps", [round(x) for x in q], "mean %.0f" % st.mean(q), "range %.0f" % (max(q) - min(q)))
        print("  pcts(median of lifetimes)", {k: st.median(r[k] for r in a) for k in ("p0", "p25", "p50", "p90", "p99")},
              "stmts", sum(r["stmts"] for r in a), "bad", sum(r["bad"] for r in a))
    on = {r["round"]: r for r in rs if r["arm"] == "on"}
    off = {r["round"]: r for r in rs if r["arm"] == "off"}
    d = [on[i]["cpu_us_per_stmt"] - off[i]["cpu_us_per_stmt"] for i in on]
    dq = [on[i]["qps"] / off[i]["qps"] - 1 for i in on]
    print("paired cpu delta on-off", [round(x, 2) for x in d], "mean %.2f" % st.mean(d), "on>off in", sum(x > 0 for x in d), "/6")
    print("paired qps delta %", [round(100 * x, 1) for x in dq], "mean %.1f" % (100 * st.mean(dq)))
