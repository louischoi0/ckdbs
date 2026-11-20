#!/usr/bin/env python3
"""analyze.py FILES... : per cell/arm tables for the DL close (raw rows from dl_close.py)."""
import json, sys, statistics as st, collections

rows = []
for f in sys.argv[1:]:
    rows += [json.loads(l) for l in open(f)]
ORDER = ["select1", "select8", "fk1", "select8off", "cabin8", "join8i", "evict8", "storm8", "stormA8", "pool8x", "join8ix"]
cells = collections.OrderedDict((c, collections.OrderedDict()) for c in ORDER)
for r in rows:
    cells[r["cell"]].setdefault(r["arm"], []).append(r)
WAITS = ["optimizer", "cabin_stats", "stats_ring", "cabin_partition", "plan_table", "frame_table", "lock_partition", "window"]


def med(xs):
    return st.median(xs)


def waits(rs, name):
    n = sum(r["stmts"] for r in rs)
    w = sum(r["meta"].get(f"contention_{name}_waits", 0) for r in rs)
    u = sum(r["meta"].get(f"contention_{name}_wait_us", 0) for r in rs)
    return w / n, u / n


print("## per arm")
print("cell | arm | lifetimes | cpu us/stmt mean (per lifetime) | cpu spread | qps mean | qps spread | stmts | bad | load range | busy rows")
for c, arms in cells.items():
    for a, rs in arms.items():
        rs.sort(key=lambda r: r["round"])
        cpu = [r["cpu_us_per_stmt"] for r in rs]
        q = [r["qps"] for r in rs]
        print(f"{c} | {a} | {len(rs)} | {st.mean(cpu):.2f} ({' '.join('%.2f' % x for x in cpu)}) | {max(cpu)-min(cpu):.2f} | "
              f"{st.mean(q):,.0f} | {max(q)-min(q):,.0f} | {sum(r['stmts'] for r in rs):,} | {sum(r['bad'] for r in rs)} | "
              f"{min(float(r['load']) for r in rs):.1f}-{max(float(r['load']) for r in rs):.1f} | "
              f"{sum(1 for r in rs if r['busy'] not in ('0','') or r['other_servers'] not in ('0',''))}")
print("\n## latency, median of per-lifetime percentiles (us)")
print("cell | arm | n | p0 | p25 | p50 | p90 | p99 | p99.9 | max(median of max)")
for c, arms in cells.items():
    for a, rs in arms.items():
        print(f"{c} | {a} | {sum(r['stmts'] for r in rs):,} | " + " | ".join(
            f"{med([r[k] for r in rs]):g}" for k in ("p0", "p25", "p50", "p90", "p99", "p999", "pmax")) +
            "  [p99 range %d-%d, p99.9 range %d-%d]" % (min(r['p99'] for r in rs), max(r['p99'] for r in rs),
                                                      min(r['p999'] for r in rs), max(r['p999'] for r in rs)))
print("\n## paired deltas (first arm - second arm, same round)")
for c, arms in cells.items():
    ks = list(arms)
    if len(ks) != 2:
        continue
    a, b = ks
    ra = {r["round"]: r for r in arms[a]}
    rb = {r["round"]: r for r in arms[b]}
    rds = sorted(set(ra) & set(rb))
    dc = [ra[i]["cpu_us_per_stmt"] - rb[i]["cpu_us_per_stmt"] for i in rds]
    dq = [100 * (ra[i]["qps"] - rb[i]["qps"]) / rb[i]["qps"] for i in rds]
    sp = max(max(x["cpu_us_per_stmt"] for x in arms[k]) - min(x["cpu_us_per_stmt"] for x in arms[k]) for k in ks)
    spq = max(max(x["qps"] for x in arms[k]) - min(x["qps"] for x in arms[k]) for k in ks)
    print(f"{c} ({a}-{b}): cpu {' '.join('%+.2f' % x for x in dc)} mean {st.mean(dc):+.2f} median {med(dc):+.2f} us, first slower in "
          f"{sum(1 for x in dc if x > 0)}/{len(dc)}; qps% {' '.join('%+.1f' % x for x in dq)} mean {st.mean(dq):+.1f}%; "
          f"larger within-arm cpu spread {sp:.2f} us, qps spread {spq:,.0f}")
print("\n## waits per statement (waits, us) — pooled over the arm's lifetimes")
print("cell | arm | " + " | ".join(WAITS))
for c, arms in cells.items():
    for a, rs in arms.items():
        print(f"{c} | {a} | " + " | ".join("%.4f, %.2f us" % waits(rs, w) for w in WAITS))
print("\n## drop / busy / capture counts, summed over the arm's lifetimes (per-statement in brackets)")
for c, arms in cells.items():
    for a, rs in arms.items():
        n = sum(r["stmts"] for r in rs)
        agg = collections.Counter()
        for r in rs:
            for k, v in r["meta"].items():
                if not k.startswith("contention_"):
                    agg[k] += v
        print(f"{c} | {a} | stmts {n:,} | " + ", ".join(f"{k}={v:,} ({v/n:.4f})" for k, v in sorted(agg.items())) +
              (f" | reader view reads {sum(r['reads'] for r in rs):,}" if any(r["reads"] for r in rs) else ""))
