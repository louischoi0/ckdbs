import json, statistics as S, glob, collections
O = "/home/ukjin/workspaces/ckdbs/.claude/worktrees/ep-bench/out"
for arm in "AB":
    js = [json.load(open(f)) for f in sorted(glob.glob(f"{O}/s0txn*-{arm}-*.json"))]
    p = [[x for x in j["phases"] if x["name"] == "txn"][0] for j in js]
    print(arm, len(js), {k: round(S.median([q["latency"][k] for q in p]), 1) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "mean_us")},
          "rate", round(S.median([j["meta"]["tps"] for j in js])), "cpu", round(S.median([q["server_cpu_us_per_op"] for q in p]), 1),
          "ops", round(S.median([q["latency"]["ops"] for q in p])),
          {k: round(S.median([q["breakdown"][k] or 0 for q in p]), 1) for k in ("commit", "execution", "parse_optimise")})
    t = [j["meta"]["tps"] for j in js]
    print(" min", round(min(t)), "max", round(max(t)))
for arm in "AB":
    ev = collections.defaultdict(list)
    for f in glob.glob(f"{O}/s0txn*-{arm}-*.json"):
        j = json.load(open(f))
        if j["meta"]["tps"] < 4500:
            continue
        p = [x for x in j["phases"] if x["name"] == "txn"][0]
        for e in p["events"]:
            ev[e["name"]].append((e["count"], e["total_us"], p["latency"]["ops"]))
    print(arm, {k: (round(S.median([c / o for c, t, o in v]), 2), round(S.median([t / o for c, t, o in v]), 1)) for k, v in ev.items()})
