import json, glob, os, statistics
D = "/home/cdkbs/ckdbs/.claude/worktrees/online-index-build/build-release/ci-close"

def pct(xs, p):
    xs = sorted(xs)
    i = min(len(xs) - 1, max(0, int(round(p / 100 * (len(xs) - 1)))))
    return xs[i]

print("| n | phase | binary | run | QPS (ops/elapsed) | p0 | p25 | p50 | p95 | p99 | p100 (ms) | WAL B/op |")
sizes = (200, 10000, 200000)
agg = {}
for n in sizes:
    for ph in ("ins", "upd2", "upd1"):
        for v in ("base", "closing"):
            qs = []
            pool = []
            for i in range(3):
                f = f"{D}/a-{v}-{n}-{i}.json"
                if not os.path.exists(f):
                    continue
                d = json.load(open(f))
                p = d["phases"][ph]
                lat = [x * 1000 for x in p["lat"]]
                qs.append(len(lat) / p["elapsed"])
                pool += lat
                wb = (p["wal"][1] - p["wal"][0]) / len(lat)
                print(f"| {n} | {ph} | {v} | {i} | {qs[-1]:.0f} | " + " | ".join(f"{pct(lat,q):.3f}" for q in (0, 25, 50, 95, 99, 100)) + f" | {wb:.0f} |")
            agg[(n, ph, v)] = (qs, pool)
print()
print("| n | phase | base QPS runs | closing QPS runs | base mean | closing mean | delta % | base spread % | closing spread % |")
for n in sizes:
    for ph in ("ins", "upd2", "upd1"):
        b, _ = agg[(n, ph, "base")]
        c, _ = agg[(n, ph, "closing")]
        if not b or not c:
            continue
        mb, mc = statistics.mean(b), statistics.mean(c)
        sp = lambda x: (max(x) - min(x)) / statistics.mean(x) * 100
        print(f"| {n} | {ph} | {' / '.join(f'{x:.0f}' for x in b)} | {' / '.join(f'{x:.0f}' for x in c)} | {mb:.0f} | {mc:.0f} | {(mc/mb-1)*100:+.1f} | {sp(b):.1f} | {sp(c):.1f} |")
print()
print("pooled percentiles (ms)")
print("| n | phase | binary | ops | p0 | p25 | p50 | p95 | p99 | p100 |")
for n in sizes:
    for ph in ("ins", "upd2", "upd1"):
        for v in ("base", "closing"):
            q, pool = agg[(n, ph, v)]
            if pool:
                print(f"| {n} | {ph} | {v} | {len(pool):,} | " + " | ".join(f"{pct(pool,x):.3f}" for x in (0, 25, 50, 95, 99, 100)) + " |")
