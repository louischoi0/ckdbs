import sys, collections
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

LOADCELLS = ['c3', 'c2', 'c1-segregated-no0', 'c1-batch0', 'c1-spread']


def ms(v):
    us = v
    if us == 0:
        return '0'
    if us < 1000:
        return f'{us / 1e3:.2f}'
    return f'{us / 1e3:.0f}' if us >= 1e5 else f'{us / 1e3:.1f}'


mode = sys.argv[1]
if mode == 'raw':
    print('| durability | cell | run | roller core(s) | core 0 | core 1 | core 2 | core 3 |')
    print('|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        for cell in LOADCELLS:
            for r, d, sl in runs(dur, cell):
                w, _ = rolls(sl, d)
                rc = collections.Counter(x['core'] for x in w)
                cs = []
                for k in range(4):
                    st = delta(d, f'load_c{k}_waited_wal_stream_us')
                    dr = delta(d, f'load_c{k}_wal_drain_latch_wait_us')
                    cv = delta(d, f'load_c{k}_wal_roll_blocked_wait_us')
                    sg = delta(d, f'load_c{k}_waited_wal_sync_gate_us')
                    cs.append(f'{ms(st)} / {ms(dr)} / {ms(cv)} / {ms(sg)}')
                print(f'| {dur} | {cell} | r{r} | ' + ', '.join(f'c{k}x{v}' for k, v in sorted(rc.items())) + ' | ' + ' | '.join(cs) + ' |')

if mode == 'sum':
    # per run: core 0 vs the rest; the roller core excluded from "other"
    print('| durability | cell | run | window hold (all rolls, summed) | core 0: stream wait | core 0: drain wait | core 0: roll-covered | non-0 non-roller cores: stream wait (sum) | their roll-covered (sum) | roller core(s): sync-gate wait |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        for cell in LOADCELLS:
            for r, d, sl in runs(dur, cell):
                w, _ = rolls(sl, d)
                rollers = set(x['core'] for x in w)
                hold = sum(x['hold_us'] for x in w)
                g = lambda k, key: delta(d, f'load_c{k}_{key}')
                others = [k for k in range(4) if k != 0 and k not in rollers]
                o_st = sum(g(k, 'waited_wal_stream_us') for k in others)
                o_cv = sum(g(k, 'wal_roll_blocked_wait_us') for k in others)
                sg = sum(g(k, 'waited_wal_sync_gate_us') for k in rollers)
                tag = '' if 0 not in rollers else ' (core 0 also rolled)'
                print(f'| {dur} | {cell} | r{r} | {ms(hold)} ms | {ms(g(0, "waited_wal_stream_us"))} ms | {ms(g(0, "wal_drain_latch_wait_us"))} ms | {ms(g(0, "wal_roll_blocked_wait_us"))} ms{tag} | {ms(o_st)} ms | {ms(o_cv)} ms | {ms(sg)} ms |')

if mode == 'c3':
    print('| durability | run | C3 rows/s | log MiB/s (append-LSN delta over m0..m1) | log MiB/s excluding the summed roll hold | rolls | hold total / window |')
    print('|---|---|---|---|---|---|---|')
    import statistics
    for dur in DURS:
        rows, mibs = [], []
        for r, d, sl in runs(dur, 'c3'):
            secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
            mib = delta(d, 'wal_append_lsn') / secs / 2**20
            w, _ = rolls(sl, d)
            hold = sum(x['hold_us'] for x in w) / 1e6
            mibx = delta(d, 'wal_append_lsn') / (secs - hold) / 2**20
            rows.append(d['inserter_rows_per_s']); mibs.append(mib)
            print(f'| {dur} | r{r} | {d["inserter_rows_per_s"]:,.0f} | {mib:.2f} | {mibx:.1f} | {len(w)} | {100 * hold / secs:.1f} % |')
        print(f'| {dur} | mean (spread = max - min) | {statistics.mean(rows):,.0f} (spread {max(rows) - min(rows):,.0f} = {100 * (max(rows) - min(rows)) / statistics.mean(rows):.2f} %) | {statistics.mean(mibs):.2f} (spread {max(mibs) - min(mibs):.2f}) | | | |')

if mode == 'ins':
    print('| durability | cell | run | inserter rows/s | log MiB/s | inserter statement p50 | p99 | max |')
    print('|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        for cell in ['c3', 'c2', 'c1-segregated-no0', 'c1-batch0', 'c1-spread']:
            for r, d, sl in runs(dur, cell):
                i = d['inserter']
                print(f'| {dur} | {cell} | r{r} | {d["inserter_rows_per_s"]:,.0f} | {d["log_bytes_per_s"] / 2**20:.1f} | {fmt(i["p50_us"])} | {fmt(i["p99_us"])} | {fmt(i["max_us"])} |')
