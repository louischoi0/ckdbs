"""P11 re-read: at cores = 2 (the clean cells), does the placement of sessions across the two cores move statements/s?
For each (shape, durability, sessions) cell, each run's throughput relative to the cell's mean is set against that run's
imbalance |sessions on core 0 - sessions/2| / (sessions/2) (0 = even split, 1 = all on one core). Reports, per census and
durability, the least-squares slope of relative throughput on imbalance (a slope of -0.3 means a fully lopsided placement
costs 30 %), the number of runs, and the between-run spread that the slope must be read against."""
import glob, json, collections, statistics as st, sys

S4 = sys.argv[1]
B = sys.argv[2]


def load(d):
    return [json.load(open(f)) for f in sorted(glob.glob(d + "/*.json"))]


def run(name, rs, k):
    cell = collections.defaultdict(list)
    for r in rs:
        if r["cores"] == k and r["sessions"] >= 2:
            cell[(r["shape"], r["durability"], r["sessions"])].append(r)
    pts = collections.defaultdict(list)
    for key, v in cell.items():
        if len(v) < 2:
            continue
        m = st.mean(r["statements_per_s"] for r in v)
        for r in v:
            n = r["sessions"]
            cnt = collections.Counter(int(c) for c in r["session_cores"])
            imb = (max(cnt.values()) / n - 1 / k) / (1 - 1 / k)
            pts[key[1]].append((imb, r["statements_per_s"] / m - 1, key))
    for dur in ("relaxed", "group", "strict"):
        p = pts[dur]
        xs = [a for a, _, _ in p]; ys = [b for _, b, _ in p]
        mx, my = st.mean(xs), st.mean(ys)
        den = sum((x - mx) ** 2 for x in xs)
        slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else float("nan")
        spread = st.pstdev(ys)
        lop = [y for x, y, _ in p if x >= 0.5]
        even = [y for x, y, _ in p if x < 0.5]
        print(f"| {name} | cores {k} | {dur} | {len(p)} runs | slope {slope:+.3f} | sd of relative throughput {spread * 100:.1f} % | "
              f"lopsided (imbalance >= 0.5) median {st.median(lop) * 100 if lop else float('nan'):+.1f} % over {len(lop)} runs | "
              f"even median {st.median(even) * 100 if even else float('nan'):+.1f} % over {len(even)} |")


print("| census | cores | durability | runs | slope | spread | lopsided | even |")
print("|---|---|---|---|---|---|---|---|")
for k in (2, 4):
    run("B", load(B), k)
    run("S4", load(S4), k)
