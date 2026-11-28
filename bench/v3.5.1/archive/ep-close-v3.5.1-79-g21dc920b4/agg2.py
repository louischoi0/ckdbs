import json, glob, statistics as S, math
O = "/home/ukjin/workspaces/ckdbs/.claude/worktrees/ep-bench/out"


def runs(cell, arm):
    return [json.load(open(f)) for f in sorted(glob.glob(f"{O}/{cell}*-{arm}-*.json"))]


def mwu(a, b):
    n1, n2 = len(a), len(b)
    allv = sorted([(x, 0) for x in a] + [(x, 1) for x in b])
    ranks = {}
    i = 0
    while i < len(allv):
        j = i
        while j < len(allv) and allv[j][0] == allv[i][0]:
            j += 1
        for k in range(i, j):
            ranks.setdefault(allv[k][0], []).append((i + j + 1) / 2)
        i = j
    r1 = sum(S.mean(ranks[x]) for x in a)
    u = r1 - n1 * (n1 + 1) / 2
    mu = n1 * n2 / 2
    sd = math.sqrt(n1 * n2 * (n1 + n2 + 1) / 12)
    return math.erfc(abs((u - mu) / sd) / math.sqrt(2))


def m(v): return round(S.median(v), 1)


for cell, key, thr in (("s0txn", "txn", 4500), ("s0g4", "txn", 0), ("s5s4", "mix", 0)):
    ra = {a: runs(cell, a) for a in "AB"}

    def rate(j):
        return j["meta"]["mix"]["new_orders_per_minute"] if cell == "s5s4" else j["meta"]["tps"]

    def ph(j, n=key):
        return [p for p in j["phases"] if p["name"] == n][0]

    print("==", cell, {a: len(ra[a]) for a in "AB"})
    rt = {a: [rate(j) for j in ra[a]] for a in "AB"}
    print(" all: medians", {a: m(rt[a]) for a in "AB"}, "B/A", round(S.median(rt["B"]) / S.median(rt["A"]), 4), "MWU p", round(mwu(rt["A"], rt["B"]), 3))
    sub = ["trade-insert", "account-update", "profit-scan", "profit-insert"] if cell == "s0txn" else (["new-order", "payment", "order-status", "delivery", "stock-level"] if cell == "s5s4" else [])
    if thr:
        sets = {a: [j for j in ra[a] if rate(j) >= thr] for a in "AB"}
        print(" slow-mode runs (<%d tps):" % thr, {a: len(ra[a]) - len(sets[a]) for a in "AB"}, "fast n", {a: len(sets[a]) for a in "AB"})
        fr = {a: [rate(j) for j in sets[a]] for a in "AB"}
        print(" fast rate median", {a: m(fr[a]) for a in "AB"}, "B/A", round(S.median(fr["B"]) / S.median(fr["A"]), 4), "min/max", {a: (round(min(fr[a])), round(max(fr[a]))) for a in "AB"}, "MWU p", round(mwu(fr["A"], fr["B"]), 3))
        print(" fast spread %", {a: round((max(fr[a]) - min(fr[a])) / S.median(fr[a]) * 100, 1) for a in "AB"})
        slr = {a: [rate(j) for j in ra[a] if rate(j) < thr] for a in "AB"}
        print(" slow rate median", {a: m(slr[a]) for a in "AB"})
        use = sets
    else:
        use = ra
    for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "mean_us", "max_us"):
        print("  ", key, k, {a: m([ph(j)["latency"][k] for j in use[a]]) for a in "AB"})
    print("   cpu/op", {a: m([ph(j)["server_cpu_us_per_op"] for j in use[a]]) for a in "AB"}, "ops", {a: m([ph(j)["latency"]["ops"] for j in use[a]]) for a in "AB"}, "ops/s", {a: m([ph(j)["throughput_ops"] for j in use[a]]) for a in "AB"})
    for sp in sub:
        print("  ", sp, {a: {k[:-3]: m([ph(j, sp)["latency"][k] for j in use[a]]) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")} for a in "AB"}, "ops", {a: m([ph(j, sp)["latency"]["ops"] for j in use[a]]) for a in "AB"})
    for a in "AB":
        print("   breakdown", a, {k: m([ph(j)["breakdown"].get(k) or 0 for j in use[a]]) for k in ph(use[a][0])["breakdown"]})
    print(" runs with any phase error:", {a: sum(1 for j in ra[a] if any(p["errors"] for p in j["phases"])) for a in "AB"})
