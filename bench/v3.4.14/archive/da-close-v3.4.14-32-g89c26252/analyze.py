import json, statistics as st
R = [json.loads(l) for l in open("/home/louis/bench-runs/da-close/raw.jsonl")]
for cn in ("select1", "select8", "fk1", "select8off"):
    rs = [r for r in R if r["cn"] == cn]
    print(f"== {cn}")
    for arm in ("A", "B"):
        a = sorted([r for r in rs if r["arm"] == arm], key=lambda r: r["round"])
        c = [r["cpu_us_per_stmt"] for r in a]
        q = [r["qps"] for r in a]
        print(arm, "cpu/stmt", [round(x, 2) for x in c], "mean %.2f range %.2f" % (st.mean(c), max(c) - min(c)))
        print(arm, "qps", [round(x) for x in q], "mean %.0f" % st.mean(q), "range %.0f" % (max(q) - min(q)))
        print("  pcts(median of lifetimes)", {k: st.median(r[k] for r in a) for k in ("p0", "p25", "p50", "p90", "p99")},
              "stmts", sum(r["stmts"] for r in a), "bad", sum(r["bad"] for r in a), "n", len(a))
    A = {r["round"]: r for r in rs if r["arm"] == "A"}
    B = {r["round"]: r for r in rs if r["arm"] == "B"}
    d = [B[i]["cpu_us_per_stmt"] - A[i]["cpu_us_per_stmt"] for i in A if i in B]
    dq = [B[i]["qps"] / A[i]["qps"] - 1 for i in A if i in B]
    print("paired cpu delta B-A", [round(x, 2) for x in d], "mean %.2f" % st.mean(d), "B>A in", sum(x > 0 for x in d), "/", len(d))
    print("paired qps delta %", [round(100 * x, 1) for x in dq], "mean %.1f" % (100 * st.mean(dq)))
