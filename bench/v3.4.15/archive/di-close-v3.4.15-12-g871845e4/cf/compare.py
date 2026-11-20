#!/usr/bin/env python3
"""Folds CF's `<arm>-r<rep>.json` files into a comparison of every pair of arms.

CF-S0's arms A, B, C give the three CF-Q1 (a) asks:

    replay's gain  A against B  (recording on in both)
    the net        A against C  (everything Waystone does, against nothing)
    recording      B against C  (the cost replay has to pay for)

CF-S2's are base, close and close-opt.

Per cell and arm: the median over reps of throughput and server CPU per
statement, and the spread (max - min) / median. A delta is marked `~` when its
size is inside the larger of the two arms' spreads. Also checks that every
arm's reply digest agrees (invariant 8) and prints each arm's `replays=`.

Usage: compare.py OUT [ARM ...]   (default arms A B C; CF-S2 passes base close close-opt)
"""
import glob, json, os, statistics, sys


def main():
    out = sys.argv[1]
    arms = sys.argv[2:] or ["A", "B", "C"]
    pairs = [(x, y) for i, x in enumerate(arms) for y in arms[i + 1:]]
    runs = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(out, "*-r*.json")))]
    by = {}
    for r in runs:
        for c in r["cells"]:
            by.setdefault(c["cell"], {}).setdefault(r["arm"], []).append(c)
    cells = [c["cell"] for c in runs[0]["cells"]]

    def stat(xs):
        m = statistics.median(xs)
        return m, (max(xs) - min(xs)) / m * 100 if m else 0.0

    def delta(cell, metric, x, y, better_low):
        mx, sx = stat([c[metric] for c in by[cell][x]])
        my, sy = stat([c[metric] for c in by[cell][y]])
        d = (mx - my) / my * 100
        gain = -d if better_low else d
        mark = "~" if abs(d) <= max(sx, sy) else ""
        return f"{gain:+.1f}%{mark}"

    print("| cell | arm | reps | qps | spread | server CPU us/stmt | spread | p50 us | replays (ANALYZE) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for cell in cells:
        for arm in arms:
            cs = by[cell].get(arm, [])
            if not cs:
                continue
            q, qs = stat([c["qps"] for c in cs])
            u, us = stat([c["server_cpu_us"] for c in cs])
            p50 = statistics.median(c["p50_us"] for c in cs)
            rp = sorted({c["analyze_replays"] for c in cs})
            print(f"| {cell} | {arm} | {len(cs)} | {q:,.0f} | {qs:.1f}% | {u:.2f} | {us:.1f}% "
                  f"| {p50:.1f} | {','.join(map(str, rp))} |")
    print()
    print("Gains are positive when the first arm is better (more qps, less CPU); `~` = inside the larger spread.")
    print()
    print("| cell | " + " | ".join(f"{x}/{y} qps | {x}/{y} CPU" for x, y in pairs) + " |")
    print("|---|" + "---|---|" * len(pairs))
    for cell in cells:
        if not all(arm in by[cell] for arm in arms):
            continue
        row = [d for x, y in pairs for d in (delta(cell, "qps", x, y, False),
                                             delta(cell, "server_cpu_us", x, y, True))]
        print(f"| {cell} | " + " | ".join(row) + " |")
    print()
    for cell in cells:
        digests = {c["digest"] for arm in by[cell] for c in by[cell][arm]}
        reps = "; ".join(f"{arm} replays={sorted({c['analyze_replays'] for c in by[cell].get(arm, [])})}"
                         for arm in arms)
        print(f"- {cell}: digests {'identical' if len(digests) == 1 else 'DIFFER ' + str(digests)}"
              f" across {sum(len(v) for v in by[cell].values())} runs; {reps}")
    print()
    for r in runs:
        pc = r["precheck"]
        print(f"- {r['arm']}-r{r['rep']} finished {r['finished']} UTC, load {pc['loadavg']}, "
              f"waited {pc.get('quiet_waited_s', 0)} s, competing {len(pc['competing'])}")


if __name__ == "__main__":
    main()
