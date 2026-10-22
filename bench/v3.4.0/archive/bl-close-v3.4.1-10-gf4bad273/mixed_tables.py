#!/usr/bin/env python3
"""BL-S5's mixed-cell tables, from mixed/r<run>/*.json beside this file
(tools/bl_mixed.py output): reader latency per arm, where each session ended
against its level, the partition's counters and the batch sessions' work."""
import glob, json, os, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS = ["p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "p99.9_us", "max_us"]
ARMS = [("isolated", "isolated-relaxed-w0-slicedflt-levels"), ("levels", "mixed-relaxed-w0-slicedflt-levels"),
        ("none", "mixed-relaxed-w0-slicedflt-r")]


def fmt(us):
    return f"{us / 1e3:.1f} ms" if us >= 1e3 else f"{us:.0f} µs"


def load(arm, r):
    pat = dict(ARMS)[arm]
    hits = [f for f in glob.glob(f"{HERE}/mixed/r{r}/bl-mixed-k4-kernel-*.json") if pat in f]
    return json.load(open(hits[0])) if hits else None


for kind, key in (("schedule", "read_from_schedule"), ("service", "read_service")):
    print(f"\n#### reader {kind} latency, per run")
    print("| arm | run | n | " + " | ".join(k[:-3] for k in KEYS) + " |")
    print("|---|---|---|" + "---|" * len(KEYS))
    for arm, _ in ARMS:
        for r in (1, 2, 3):
            d = load(arm, r)
            if d:
                p = d[key]
                print(f"| {arm} | r{r} | {p['n']} | " + " | ".join(fmt(p[k]) for k in KEYS) + " |")
    print(f"\n#### reader {kind} latency, median of runs")
    print("| arm | " + " | ".join(k[:-3] for k in KEYS) + " |")
    print("|---|" + "---|" * len(KEYS))
    for arm, _ in ARMS:
        ps = [load(arm, r)[key] for r in (1, 2, 3) if load(arm, r)]
        print(f"| {arm} | " + " | ".join(fmt(statistics.median(p[k] for p in ps)) for k in KEYS) + " |")

print("\n#### sessions' cores at the end (6 readers, walker, inserter), partition counters, batch work")
print("| arm | run | cores at end | readers on core 0 | batch sessions on core 0 | moves (run) | partition moves | batch_sessions at end | inserter rows/s | walker stmts |")
print("|---|---|---|---|---|---|---|---|---|---|")
for arm, _ in ARMS:
    for r in (1, 2, 3):
        d = load(arm, r)
        if not d:
            continue
        ends = [int(c) for c in d["cores_end"]]
        roles = d["roles"]
        md = d["meta_delta"]
        pm = sum(int(md.get(f"load_c{k}_partition_moves_out", 0)) for k in range(4))
        g = d.get("meta_m1") or {}
        bs = g.get("batch_sessions", "-") if isinstance(g, dict) else "-"
        rd0 = sum(1 for c, ro in zip(ends, roles) if ro == "reader" and c == 0)
        bt0 = sum(1 for c, ro in zip(ends, roles) if ro in ("walker", "inserter") and c == 0)
        w = d.get("walker") or {}
        print(f"| {arm} | r{r} | {', '.join(map(str, ends))} | {rd0} | {bt0} | {d['moves']} | {pm} | {bs} | "
              f"{d.get('inserter_rows_per_s', 0):,.0f} | {w.get('n', 0):,} |")
