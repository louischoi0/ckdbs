#!/usr/bin/env python3
"""M0 analysis. usage: analyze_m0.py <out/m0 dir> [cell ...]
For every (cell, arm, rows): B - A median of the per-run differences (p50, p99, QPS, server CPU per statement) with a
bootstrap 95 % CI (4,000 resamples of the runs, seed 1), and the A and B pooled medians."""
import glob, json, random, statistics as st, sys

OUT = sys.argv[1]
CELLS = sys.argv[2:] or ["relaxed-c1", "relaxed-c4", "aa-c1", "aa-c4", "group-c1", "group-c4"]
random.seed(1)
ARMS = ["iso", "insert", "insert-again", "select-pk", "update-pk", "ping"]
DESC = {"iso": "SET ISOLATION LEVEL (control)", "insert": "INSERT, pk omitted", "insert-again": "INSERT again (replicate)",
        "select-pk": "SELECT by pk", "update-pk": "UPDATE by pk", "ping": "SHOW META (not a control)"}


def boot(v, n=4000):
    if len(v) < 2:
        return (v[0], v[0]) if v else (0, 0)
    b = sorted(st.median(random.choices(v, k=len(v))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


def load(cell, rows):
    return [json.load(open(f)) for f in sorted(glob.glob(f"{OUT}/{cell}/n{rows}-r*.json"))]


def series(d, arm, fn):
    return [fn(s["arms"]["A"][arm], s["arms"]["B"][arm]) for s in d if arm in s["arms"]["A"]]


def cpu_per_stmt(a):
    c = a["cpu"]
    n = c["sched_foreground_polls"]
    return c["sched_foreground_polled_us"] / max(1, a["ops"] * (1 - c["blocks_moved"] / 12.0) if False else a["ops"])


def main():
    res = {}
    for cell in CELLS:
        for rows in (200, 1000, 10000):
            d = load(cell, rows)
            if not d:
                continue
            for arm in ARMS:
                if arm not in d[0]["arms"]["A"]:
                    continue
                p50 = series(d, arm, lambda a, b: b["p50_us"] - a["p50_us"])
                p99 = series(d, arm, lambda a, b: b["p99_us"] - a["p99_us"])
                qps = series(d, arm, lambda a, b: b["qps"] - a["qps"])
                # server CPU per statement: foreground polled us over statements in the blocks counted
                def cpu(a):
                    c = a["cpu"]
                    blocks_ok = 12 - c.get("blocks_moved", 0)
                    stm = a["ops"] * blocks_ok / 12.0
                    return c["sched_foreground_polled_us"] / max(1.0, stm)
                cp = series(d, arm, lambda a, b: cpu(b) - cpu(a))
                Aq = st.median([s["arms"]["A"][arm]["qps"] for s in d]); Bq = st.median([s["arms"]["B"][arm]["qps"] for s in d])
                A50 = st.median([s["arms"]["A"][arm]["p50_us"] for s in d]); B50 = st.median([s["arms"]["B"][arm]["p50_us"] for s in d])
                A99 = st.median([s["arms"]["A"][arm]["p99_us"] for s in d]); B99 = st.median([s["arms"]["B"][arm]["p99_us"] for s in d])
                Ac = st.median([cpu(s["arms"]["A"][arm]) for s in d]); Bc = st.median([cpu(s["arms"]["B"][arm]) for s in d])
                errs = sum(s["arms"][l][arm]["errors"] for s in d for l in "AB")
                res[(cell, arm, rows)] = dict(n=len(d), p50=(st.median(p50), boot(p50)), p99=(st.median(p99), boot(p99)),
                                              qps=(st.median(qps), boot(qps)), cpu=(st.median(cp), boot(cp)),
                                              A50=A50, B50=B50, A99=A99, B99=B99, Aq=Aq, Bq=Bq, Ac=Ac, Bc=Bc, errs=errs,
                                              sd50=st.pstdev(p50) if len(p50) > 1 else 0.0)
    json.dump({"|".join(map(str, k)): v for k, v in res.items()}, open(f"{OUT}/analysis.json", "w"), indent=1)
    for cell in CELLS:
        print(f"\n### {cell}")
        print("| shape | rows | runs | A p50 | B p50 | B-A p50 [95 % CI] | A p99 | B p99 | B-A p99 [CI] | A QPS | B QPS | B-A QPS [CI] | A srv CPU/stmt | B srv CPU/stmt | B-A [CI] | errs |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for arm in ARMS:
            for rows in (200, 1000, 10000):
                r = res.get((cell, arm, rows))
                if not r:
                    continue
                f = lambda t, u="µs": f"{t[0]:+.2f} {u} [{t[1][0]:+.2f}, {t[1][1]:+.2f}]"
                print(f"| {DESC[arm]} | {rows:,} | {r['n']} | {r['A50']:.1f} µs | {r['B50']:.1f} µs | {f(r['p50'])} | {r['A99']:.1f} µs | {r['B99']:.1f} µs | {f(r['p99'])} | "
                      f"{r['Aq']:,.0f} /s | {r['Bq']:,.0f} /s | {f(r['qps'], '/s')} | {r['Ac']:.2f} µs | {r['Bc']:.2f} µs | {f(r['cpu'])} | {r['errs']} |")


main()
