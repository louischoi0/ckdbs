#!/usr/bin/env python3
"""Tables for results-bl-ruling2-remeasure-v3.4.0-22-g418670a3.md, from the
driver JSON under r1..r3/ beside this file."""
import glob, json, os, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS = ['p0_us', 'p25_us', 'p50_us', 'p95_us', 'p99_us', 'p99.9_us', 'max_us']
SUB = sys.argv[1] if len(sys.argv) > 1 else ''
CELLS = [('mixed', 'segregated-no0'), ('mixed', 'segregated'), ('mixed', 'batch0')] if SUB == 'nobal' else [('isolated', 'segregated-no0'), ('mixed', 'segregated-no0'), ('mixed', 'spread'), ('mixed', 'batch0')]


def fmt(us):
    if us >= 1e6:
        return f'{us / 1e6:.2f} s'
    if us >= 1e3:
        return f'{us / 1e3:.1f} ms'
    return f'{us:.0f} µs'


def load(arm, pl, r):
    js = glob.glob(f'{HERE}/{SUB + "/" if SUB else ""}r{r}/bl-mixed-k4-{pl}-{arm}-*.json')
    return json.load(open(js[0])) if js else None


def latch(d, k):
    m0, m1 = {}, d.get('meta_delta', {})
    key = f'load_c{k}_waited_wal_stream_us'
    return (float(m1.get(key, 0)) - float(m0.get(key, 0))) / 1e6


for kind, key in (('schedule', 'read_from_schedule'), ('service', 'read_service')):
    print(f'\n#### reader {kind} latency, per run')
    print('| arm | placement | run | n | ' + ' | '.join(k.replace('_us', '') for k in KEYS) + ' |')
    print('|---|---|---|---|' + '---|' * len(KEYS))
    for arm, pl in CELLS:
        for r in (1, 2, 3):
            d = load(arm, pl, r)
            if d is None:
                continue
            p = d[key]
            print(f'| {arm} | {pl} | r{r} | {p["n"]} | ' + ' | '.join(fmt(p[k]) for k in KEYS) + ' |')
    print(f'\n#### reader {kind} latency, median of runs')
    print('| arm | placement | runs | ' + ' | '.join(k.replace('_us', '') for k in KEYS) + ' |')
    print('|---|---|---|' + '---|' * len(KEYS))
    for arm, pl in CELLS:
        ps = [d[key] for d in (load(arm, pl, r) for r in (1, 2, 3)) if d]
        if ps:
            print(f'| {arm} | {pl} | {len(ps)} | ' + ' | '.join(fmt(statistics.median(p[k] for p in ps)) for k in KEYS) + ' |')

print('\n#### batch sessions, moves, stream-latch wait per core (window)')
print('| arm | placement | run | inserter rows/s | walker stmts | moves (run / window) | errors / refusals | c0 | c1 | c2 | c3 |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for arm, pl in CELLS:
    for r in (1, 2, 3):
        d = load(arm, pl, r)
        if d is None:
            continue
        w = d.get('walker') or {}
        print(f'| {arm} | {pl} | r{r} | {d.get("inserter_rows_per_s", 0):,.0f} | {w.get("n", 0):,} | '
              f'{d.get("moves")} / {d.get("moves_in_window")} | {d.get("errors")} / {d.get("refusals")} | '
              + ' | '.join(f'{latch(d, k):.2f} s' for k in range(4)) + ' |')
