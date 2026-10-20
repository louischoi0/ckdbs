#!/usr/bin/env python3
"""analyze.py CELLS.jsonl: per (engine, mode, size) median of reps + spread, per-row slope (least squares
over sizes 1..60480 and over 1..10000), latency percentiles (pooled over reps), wire bytes per row."""
import json, sys, statistics as st, collections

cells = [json.loads(l) for l in open(sys.argv[1])]
M = ("wall_us_per_stmt", "server_cpu_us_per_stmt", "client_cpu_us_per_stmt")
g = collections.defaultdict(list)
for c in cells:
    g[(c["engine"], c["mode"], c["size"])].append(c)


def med(key, m):
    v = [c[m] for c in g[key]]
    return st.median(v), (max(v) - min(v)) / st.median(v) if st.median(v) else 0


def fit(xs, ys):
    n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return b, my - b * mx


sizes = sorted({c["size"] for c in cells})
out = {}
print("## per cell (median of reps; spread = (max-min)/median)")
print("engine mode size n | wall us/stmt (spr) | server cpu us/stmt (spr) | client cpu us/stmt (spr)")
for e in ("kds", "pg"):
    for mode in ("drain", "split", "values", "inline"):
        for s in sizes:
            k = (e, mode, s)
            if k not in g:
                continue
            row = [med(k, m) for m in M]
            print(e, mode, s, g[k][0]["n"], "|", " | ".join(f"{v:.1f} ({sp*100:.0f}%)" for v, sp in row))
print("\n## slopes (us per returned row) by least squares on the per-size medians; intercept = fixed us/stmt")
for label, lim in (("sizes 1..10000", 10000), ("sizes 100..60480", 10 ** 9)):
    for e in ("kds", "pg"):
        for mode in ("drain", "split", "values", "inline"):
            for m in M:
                xs = [s for s in sizes if s <= lim and (lim == 10000 or s >= 100) and (e, mode, s) in g]
                ys = [med((e, mode, s), m)[0] for s in xs]
                if len(xs) < 2:
                    continue
                b, a = fit(xs, ys)
                out[(label, e, mode, m)] = (b, a)
                print(f"{label:18s} {e} {mode:6s} {m:24s} slope {b:8.3f} us/row  intercept {a:9.1f} us")
print("\n## wire bytes per reply (drain mode) and per row")
for e in ("kds", "pg"):
    xs = [s for s in sizes if (e, "drain", s) in g]
    ys = [g[(e, "drain", s)][0]["first_result"] for s in xs]
    if len(xs) < 2:
        continue
    b, a = fit(xs, ys)
    print(e, "bytes/row slope", round(b, 2), "fixed bytes", round(a, 1), " per size:", dict(zip(xs, ys)))
print("\n## latency percentiles us per statement (pooled over reps)")
def pct(v, p):
    v = sorted(v); i = min(len(v) - 1, max(0, round(p / 100 * (len(v) - 1)))); return v[i]
for mode in ("drain", "values"):
    for s in (1, 100, 1000, 10000, 60480):
        for e in ("kds", "pg"):
            v = [x for c in g.get((e, mode, s), []) for x in c["lat_us"]]
            if not v:
                continue
            print(mode, s, e, "n", len(v), " ".join(f"p{p}={pct(v,p):.0f}" for p in (0, 25, 50, 95, 99)))
