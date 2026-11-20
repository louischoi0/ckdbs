#!/usr/bin/env python3
"""DH-S1 spike: tables from dh_s0.py's runs.jsonl (arms A0, B, P1, P2; ids <cell>r<round>).

    dh_s1_report.py RUNS.jsonl

Every cell carries its unit. Checkpoint runs are the server's counter
`contention_checkpoint_runs` over the window.
"""
import json, sys
from collections import defaultdict

rs = [json.loads(l) for l in open(sys.argv[1])]
cells = defaultdict(lambda: defaultdict(list))
for r in rs:
    cells[r["id"].rsplit("r", 1)[0] if r["id"][-2] == "r" else r["id"]][r["arm"]].append(r)


def mean(x):
    return sum(x) / len(x)


def spread(x):
    return 100 * (max(x) - min(x)) / mean(x) if len(x) > 1 else None


def n(x, unit, d=0):
    return "-" if x is None else f"{x:,.{d}f} {unit}"


print("| cell | arm | runs | mean | spread of runs | vs A0 | gap recovered, (arm - B) / (A0 - B) | outside the floor? |")
print("|---|---|---|---|---|---|---|---|")
for c, arms in cells.items():
    a0, b = mean([r["stmt_per_s"] for r in arms["A0"]]), mean([r["stmt_per_s"] for r in arms["B"]])
    fl = max(spread([r["stmt_per_s"] for r in arms[a]]) or 0 for a in arms)
    for arm in ("A0", "B", "P1", "P2"):
        if arm not in arms:
            continue
        v = [r["stmt_per_s"] for r in arms[arm]]
        m = mean(v)
        rec = "-" if arm in ("A0", "B") else f"{100 * (m - b) / (a0 - b):.0f} % of the gap"
        out = "-"
        if arm not in ("A0", "B"):
            # No arm repeated: there is no floor, so nothing is "outside" it.
            out = "single run" if fl == 0 else ("yes" if abs(m - b) / b * 100 > fl else "no")
            out += f" (delta vs B {100 * (m / b - 1):+.1f} %, floor {fl:.1f} %)"
        sp = spread(v)
        print(f"| {c} | {arm} | {', '.join(f'{x:,.0f} stmt/s' for x in v)} | {n(m, 'stmt/s')} | "
              f"{'single run' if sp is None else f'{sp:.1f} %'} | {100 * (m / a0 - 1):+.1f} % | {rec} | {out} |")
print()
print("| run | arm | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | wal_flush wait | run_queue wait | on_cpu | page_read wait | ckpt runs (counter) | ckpt pages written | pages drained | bad |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in rs:
    d, k, l, w = r["delta"], r["statements"], r["lat_us"], r["waits_us"]

    def wu(ev):
        return n(w.get(ev, [0, 0])[1] / k, "us/stmt", 1)
    print(f"| {r['id']} | {r['arm']} | {n(r['stmt_per_s'], 'stmt/s')} | {n(l['0'], 'us')} | {n(l['25'], 'us')} | {n(l['50'], 'us')} | "
          f"{n(l['90'], 'us')} | {n(l['99'], 'us')} | {n(r['cpu_us_per_stmt'], 'us/stmt', 1)} | "
          f"{n(d['wal_append_lsn'] / k, 'B/stmt')} | {n(d['pool_fetch_images'] / k, 'img/stmt', 3)} | {wu('commit.wal_flush')} | "
          f"{wu('sched.run_queue')} | {wu('cpu.on_cpu')} | {wu('io.page_read')} | {n(d['contention_checkpoint_runs'], 'runs')} | "
          f"{n(d['contention_checkpoint_pages_written'], 'pages')} | {n(d['pool_dirty_drained'], 'pages')} | {n(r['bad'], 'stmts')} |")
