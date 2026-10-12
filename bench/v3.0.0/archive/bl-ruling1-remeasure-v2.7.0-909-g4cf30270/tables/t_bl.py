import sys, glob, json, statistics
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

KEYS = ['p0_us', 'p25_us', 'p50_us', 'p95_us', 'p99_us', 'p99.9_us', 'max_us']
PL = ['segregated-no0', 'spread', 'batch0']
BIN = ['3338cf0c', '05cd011f']


def L(b, pl, r):
    p = f'{OUT}/bl/{b}/{pl}/r{r}'
    js = glob.glob(p + '/bl-mixed-*.json')[0]
    return json.load(open(js))


mode = sys.argv[1]
if mode == 'runs':
    for kind, key in (('schedule', 'read_from_schedule'), ('service', 'read_service')):
        print(f'\n#### {kind}')
        print('| placement | commit | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |')
        print('|---|---|---|---|---|---|---|---|---|---|---|')
        for pl in PL:
            for b in BIN:
                for r in (1, 2, 3):
                    p = L(b, pl, r)[key]
                    print(f'| {pl} | {b} | r{r} | {p["n"]} | ' + ' | '.join(fmt(p[k]) for k in KEYS) + ' |')

if mode == 'med':
    for kind, key in (('schedule', 'read_from_schedule'), ('service', 'read_service')):
        print(f'\n#### {kind}')
        print('| placement | commit | n (3 runs) | p0 | p25 | p50 | p95 | p99 | p99.9 | max |')
        print('|---|---|---|---|---|---|---|---|---|---|')
        for pl in PL:
            for b in BIN:
                ps = [L(b, pl, r)[key] for r in (1, 2, 3)]
                print(f'| {pl} | {b} | {sum(p["n"] for p in ps)} | ' + ' | '.join(fmt(statistics.median(p[k] for p in ps)) for k in KEYS) + ' |')

if mode == 'delta':
    print('| placement | figure | 3338cf0c (r1 / r2 / r3) | spread of the 3338cf0c runs (max - min) | 05cd011f (r1 / r2 / r3) | spread of the 05cd011f runs | median 05cd011f / median 3338cf0c | ranges overlap? |')
    print('|---|---|---|---|---|---|---|---|')
    for pl in PL:
        for kind, key in (('schedule', 'read_from_schedule'), ('service', 'read_service')):
            for q in ('p50_us', 'p99_us', 'p99.9_us', 'max_us'):
                a = [L('3338cf0c', pl, r)[key][q] for r in (1, 2, 3)]
                b = [L('05cd011f', pl, r)[key][q] for r in (1, 2, 3)]
                ma, mb = statistics.median(a), statistics.median(b)
                ov = 'yes' if not (max(b) < min(a) or max(a) < min(b)) else 'no'
                print(f'| {pl} | {kind} {q.replace("_us", "")} | ' + ' / '.join(fmt(x) for x in a) + f' | {fmt(max(a) - min(a))} | ' + ' / '.join(fmt(x) for x in b) + f' | {fmt(max(b) - min(b))} | {mb / ma:.4f} ({100 * (mb / ma - 1):+.0f} %) | {ov} |')

if mode == 'ins':
    print('#### perrun')
    print('| placement | commit | run | inserter rows/s | inserter statement p50 | p99 | max | walker statements completed (closed loop) | walker p50 | walker p99 | moves (run / in window) | per_core_valid | errors / refusals |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for pl in PL:
        for b in BIN:
            rs = []
            for r in (1, 2, 3):
                d = L(b, pl, r)
                i = d['inserter']
                w = d['walker']
                rs.append(d['inserter_rows_per_s'])
                print(f'| {pl} | {b} | r{r} | {d["inserter_rows_per_s"]:,.0f} | {fmt(i["p50_us"])} | {fmt(i["p99_us"])} | {fmt(i["max_us"])} | {w["n"]:,} | {fmt(w["p50_us"])} | {fmt(w["p99_us"])} | {d["moves"]} / {d["moves_in_window"]} | {d["per_core_valid"]} | {d["errors"]} / {d["refusals"]} |')
            m = [x for x in rs]
    print()
    print('#### rows')
    print('| placement | commit | inserter rows/s (r1 / r2 / r3) | median | spread (max - min) |')
    print('|---|---|---|---|---|')
    for pl in PL:
        for b in BIN:
            rs = [L(b, pl, r)['inserter_rows_per_s'] for r in (1, 2, 3)]
            print(f'| {pl} | {b} | ' + ' / '.join(f'{x:,.0f}' for x in rs) + f' | {statistics.median(rs):,.0f} | {max(rs) - min(rs):,.0f} ({100 * (max(rs) - min(rs)) / statistics.median(rs):.1f} %) |')
    print()
    print('#### rowsdelta')
    print('| placement | median rows/s 3338cf0c | median rows/s 05cd011f | delta | 3338cf0c spread |')
    print('|---|---|---|---|---|')
    for pl in PL:
        a = [L('3338cf0c', pl, r)['inserter_rows_per_s'] for r in (1, 2, 3)]
        b = [L('05cd011f', pl, r)['inserter_rows_per_s'] for r in (1, 2, 3)]
        print(f'| {pl} | {statistics.median(a):,.0f} | {statistics.median(b):,.0f} | {100 * (statistics.median(b) / statistics.median(a) - 1):+.1f} % | {max(a) - min(a):,.0f} |')
    print()
    print('#### walker')
    print('| placement | walker statements completed 3338cf0c (r1 / r2 / r3) | 05cd011f (r1 / r2 / r3) | median ratio |')
    print('|---|---|---|---|')
    for pl in PL:
        a = [L('3338cf0c', pl, r)['walker']['n'] for r in (1, 2, 3)]
        b = [L('05cd011f', pl, r)['walker']['n'] for r in (1, 2, 3)]
        print(f'| {pl} | ' + ' / '.join(f'{x:,}' for x in a) + ' | ' + ' / '.join(f'{x:,}' for x in b) + f' | {statistics.median(b) / statistics.median(a):.2f}x |')

if mode == 'wait':
    def dl(d, k):
        try:
            return float(d['meta_m1'].get(k, 0)) - float(d['meta_m0'].get(k, 0))
        except (TypeError, ValueError):
            return 0.0
    print('| placement | commit | run | core 0: stream-latch wait | core 1 | core 2 | core 3 | cores 1-3 together | window |')
    print('|---|---|---|---|---|---|---|---|---|')
    for pl in PL:
        for b in BIN:
            for r in (1, 2, 3):
                d = L(b, pl, r)
                w = [dl(d, f'load_c{k}_waited_wal_stream_us') / 1e6 for k in range(4)]
                secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
                print(f'| {pl} | {b} | r{r} | ' + ' | '.join(f'{x:.2f} s' for x in w) + f' | {sum(w[1:]):.2f} s | {secs:.1f} s |')

if mode == 'misc':
    d = L('05cd011f', 'batch0', 1)
    print({k: d[k] for k in ('cores', 'seconds', 'warmup', 'rate', 'readers', 'writers', 'batch', 'walk_rows', 'frames' if 'frames' in d else 'cores', 'log_level', 'durability')})
    print(sorted(k for k in d['meta_m1'] if k.startswith('wal_roll'))[:5], 'wal_rolls' in d['meta_m1'])
    for b in BIN:
        for pl in PL:
            for r in (1, 2, 3):
                d = L(b, pl, r)
                print(b, pl, r, 'rolls', d['meta_m1'].get('wal_rolls'), 'cores_end', d['cores_end'], 'walker p50', d['walker']['p50_us'], 'n', d['walker']['n'], 'slow_dropped', d['slow_dropped'], 'rows/s', d['inserter_rows_per_s'])
