import json, statistics
D = "/home/cdkbs/ckdbs/.claude/worktrees/online-index-build/build-release/ci-close"

def pct(xs, p):
    xs = sorted(xs)
    i = min(len(xs) - 1, max(0, int(round(p / 100 * (len(xs) - 1)))))
    return xs[i]

def meta(m):
    return {bytes.fromhex(r[1]).decode(): bytes.fromhex(r[2]).decode() for r in m[1]}

for v in ("base", "closing"):
    pools = {"pre": [], "in": [], "post": [], "all": []}
    builds, stalls, rates, wals, cpu, rd = [], [], [], [], [], []
    for i in range(3):
        r = json.load(open(f"{D}/b-{v}-{i}.json"))["runs"][0]
        b = r["build"]; s, e = b["start"], b["end"]
        w = r["writer"]
        pools["pre"] += [l * 1000 for t, l in w if t < s]
        pools["in"] += [l * 1000 for t, l in w if s <= t < e]
        pools["post"] += [l * 1000 for t, l in w if t >= e]
        pools["all"] += [l * 1000 for t, l in w]
        builds.append(b["elapsed"]); stalls.append(max(l for t, l in w))
        n_in = len([1 for t, l in w if s <= t < e])
        rates.append(n_in / (e - s))
        ma, mb = meta((0, b["meta_a"][1])), meta((0, b["meta_b"][1]))
        wals.append(int(ma["wal_append_lsn"]) - int(mb["wal_append_lsn"]))
        cpu.append(r["server_cpu_s"])
        rd.append(len([1 for t, l in r["reader"] if s <= t < e]) / (e - s))
        prer = len([1 for t, l in r["reader"] if t < s]) / s
        print(v, i, f"build {b['elapsed']:.3f} stall {stalls[-1]:.3f} in-build writer {n_in} ({rates[-1]:.0f}/s) reader in-build {rd[-1]:.0f}/s pre {prer:.0f}/s wal {wals[-1]:,} cpu {cpu[-1]:.1f}")
    print(v, "mean build", statistics.mean(builds), "mean wal", statistics.mean(wals), "mean rate", statistics.mean(rates))
    for k, xs in pools.items():
        print(f"| {v} | {k} | {len(xs):,} | " + " | ".join(f"{pct(xs,p):.3f}" for p in (0, 25, 50, 90, 99, 99.9, 100)) + " |")
