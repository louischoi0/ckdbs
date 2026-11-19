#!/usr/bin/env python3
"""Per lifetime and pooled round-paired median server-CPU deltas (us) of B - A and the control A2 - A,
per cell, dropping every (round, cell) with a competing build/test process in any arm's block.
usage: an_point.py life1.json life2.json ...  (lifetimes with every block competing are shown and excluded
by the caller by not passing them to --pool)  --pool a,b,c selects the pooled set."""
import collections, json, random, statistics, sys

files = [f for f in sys.argv[1:] if not f.startswith("--")]
pool = None
for i, f in enumerate(sys.argv):
    if f == "--pool": pool = set(sys.argv[i + 1].split(","))
files = [f for f in files if not (pool and f == sys.argv[sys.argv.index("--pool") + 1])]
cells = ["sel-200", "sel-1k", "sel-10k", "upd-200", "upd-1k", "upd-10k"]
pairs = [("B", "A"), ("A2", "A"), ("B", "A2"), ("B", "M")]   # M = mean of the two A-binary servers
per = {}
allp = collections.defaultdict(list)     # pooled (cell, pair) -> list of round deltas (clean lives in pool)
absn = collections.defaultdict(lambda: collections.defaultdict(list))
meta = {}
for f in files:
    o = json.load(open(f)); life = o[0]["life"]
    bad = {(r["round"], r["cell"]) for r in o if r["host"]["competing"]}
    meta[life] = (len(o), len(bad), max(float(r["host"]["loadavg"].split()[0]) for r in o),
                  min(float(r["host"]["loadavg"].split()[0]) for r in o))
    d = collections.defaultdict(dict)
    for r in o:
        if (r["round"], r["cell"]) not in bad: d[(r["cell"], r["engine"])][r["round"]] = r
    for c in cells:
        for e, b in pairs:
            if b == "M":
                rs = sorted(set(d[(c, "B")]) & set(d[(c, "A")]) & set(d[(c, "A2")]))
                xs = [d[(c, "B")][k]["server_cpu_us"] - (d[(c, "A")][k]["server_cpu_us"] + d[(c, "A2")][k]["server_cpu_us"]) / 2 for k in rs]
            else:
                rs = sorted(set(d[(c, e)]) & set(d[(c, b)]))
                xs = [d[(c, e)][k]["server_cpu_us"] - d[(c, b)][k]["server_cpu_us"] for k in rs]
            per[(life, c, e, b)] = (statistics.median(xs) if xs else None, len(xs))
            if pool is None or life in pool:
                allp[(c, e, b)] += xs
        if pool is None or life in pool:
            for e in ("A", "B", "A2"):
                for k, r in d[(c, e)].items(): absn[c][e].append(r)
print("lifetime: blocks, competing blocks, load min..max:", meta)
lives = sorted(meta, key=int)
print("\n| Pair | Cell | " + " | ".join(f"life {l}" for l in lives) + " | pooled median [95% CI] (n rounds) |")
for e, b in pairs:
    for c in cells:
        row = []
        for l in lives:
            m, n = per[(l, c, e, b)]
            row.append("-" if m is None else f"{m:+.2f} (n={n})")
        xs = allp[(c, e, b)]
        rng = random.Random(f"{c}{e}{b}")
        bs = sorted(statistics.median(rng.choices(xs, k=len(xs))) for _ in range(2000))
        print(f"| {e} - {b} | {c} | " + " | ".join(row) + f" | {statistics.median(xs):+.2f} [{bs[50]:+.2f}, {bs[1949]:+.2f}] ({len(xs)}) |")
print("\n| Cell | Arm | blocks | QPS | p0 | p25 | p50 | p95 | p99 | server CPU us/stmt |")
for c in cells:
    for e in ("A", "A2", "B"):
        rs = absn[c][e]; g = lambda k: statistics.median(x[k] for x in rs)
        print(f"| {c} | {e} | {len(rs)} | {g('qps'):,.0f} | {g('p0'):.1f} | {g('p25'):.1f} | {g('p50'):.1f} | {g('p95'):.1f} | {g('p99'):.1f} | {g('server_cpu_us'):.2f} |")
