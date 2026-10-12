import sys, statistics, collections
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

LOADCELLS = ['c3', 'c2', 'c1-segregated-no0', 'c1-batch0', 'c1-spread']
which = sys.argv[1]

data = {}  # (dur, cell) -> list of (r, d, in-window rolls, all rolls)
for dur in DURS:
    for cell in LOADCELLS:
        data[(dur, cell)] = []
        for r, d, sl in runs(dur, cell):
            w, a = rolls(sl, d)
            assert len(w) == int(delta(d, 'wal_rolls')), (dur, cell, r, len(w), delta(d, 'wal_rolls'))
            data[(dur, cell)].append((r, d, w, a))

if which == 'counts':
    print('| durability | cell | run | rolls in window | rolls in log | roller core(s) | wal_segments_removed | wal_first_segment (end) | live segments at last roll | wal_append_lsn delta | checkpoint runs (core 0 view) |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        for cell in LOADCELLS:
            for r, d, w, a in data[(dur, cell)]:
                cores = collections.Counter(x['core'] for x in w)
                live = max((x['sync_segs'] for x in w), default=0)
                print(f'| {dur} | {cell} | r{r} | {len(w)} | {len(a)} | '
                      + ', '.join(f'c{k}x{v}' for k, v in sorted(cores.items()))
                      + f' | {delta(d, "wal_segments_removed"):.0f} | {m1v(d, "wal_first_segment"):.0f} | {live} | {delta(d, "wal_append_lsn") / 2**20:.0f} MiB | {delta(d, "checkpoint_runs"):.0f} |')

if which == 'hold':
    print('| durability | cell | rolls (r1 / r2 / r3) | p0 | p25 | p50 | p95 | p99 | max | per-run p50 (r1 / r2 / r3) | per-run max (r1 / r2 / r3) |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        allh = []
        for cell in LOADCELLS:
            hs = [[x['hold_us'] for x in w] for _, _, w, _ in data[(dur, cell)]]
            h = [v for run in hs for v in run]
            allh += h
            p50 = [nr(x, 50) for x in hs]
            mx = [max(x) for x in hs]
            print(f'| {dur} | {cell} | {" / ".join(str(len(x)) for x in hs)} | ' + ' | '.join(fmt(nr(h, q)) for q in (0, 25, 50, 95, 99)) + f' | {fmt(max(h))} | '
                  + ' / '.join(fmt(x) for x in p50) + f', spread {fmt(max(p50) - min(p50))} | ' + ' / '.join(fmt(x) for x in mx) + f', spread {fmt(max(mx) - min(mx))} |')
        print(f'| {dur} | all five load cells | {len(allh)} | ' + ' | '.join(fmt(nr(allh, q)) for q in (0, 25, 50, 95, 99)) + f' | {fmt(max(allh))} | | |')

STEPS = [('a seal', lambda x: x['seal_us']), ('b flush', lambda x: x['flush_us']),
         ('c1 sync gate (waits out the writer thread\'s sync)', lambda x: x['sync_gate_us']),
         ('c2 own sync, tail fdatasync', lambda x: x['sync_tail_us']),
         ('c3 own sync, non-tail fdatasyncs', lambda x: x['sync_rest_us']),
         ('d body (wait on the preparer + inline build)', lambda x: x['body_wait_us'] + x['body_us']),
         ('e header write + fdatasync', lambda x: x['header_us']),
         ('f rename', lambda x: x['rename_us']),
         ('g directory fsync', lambda x: x['dirsync_us'])]

if which == 'steps':
    for sel in ('all', 'p99'):
        print(f'\n#### share of summed hold, {"all rolls" if sel == "all" else "rolls at or above the cell-and-durability hold p99"}')
        hdr = '| durability | cell | rolls | ' + ' | '.join(n.split(' ')[0] for n, _ in STEPS) + ' | residual |'
        print(hdr)
        print('|---|---|---|' + '---|' * (len(STEPS) + 1))
        for dur in DURS:
            pools = {c: [x for _, _, w, _ in data[(dur, c)] for x in w] for c in LOADCELLS}
            pools['all five'] = [x for c in LOADCELLS for x in pools[c]]
            for cell in LOADCELLS + ['all five']:
                rs = pools[cell]
                if sel == 'p99':
                    thr = nr([x['hold_us'] for x in rs], 99)
                    rs = [x for x in rs if x['hold_us'] >= thr]
                tot = sum(x['hold_us'] for x in rs)
                parts = [sum(f(x) for x in rs) for _, f in STEPS]
                res = tot - sum(parts)
                print(f'| {dur} | {cell} | {len(rs)} | ' + ' | '.join(f'{100 * p / tot:.1f} %' for p in parts) + f' | {100 * res / tot:.1f} % |')

if which == 'stepabs':
    print('| durability | step | p0 | p50 | p95 | p99 | max | summed over all rolls |')
    print('|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        rs = [x for c in LOADCELLS for _, _, w, _ in data[(dur, c)] for x in w]
        for name, f in STEPS + [('c (1+2+3) step c total', lambda x: x['sync_gate_us'] + x['sync_us']),
                               ('e+f+g', lambda x: x['header_us'] + x['rename_us'] + x['dirsync_us']),
                               ('hold', lambda x: x['hold_us'])]:
            v = [f(x) for x in rs]
            print(f'| {dur} | {name} | ' + ' | '.join(fmt(nr(v, q)) for q in (0, 50, 95, 99)) + f' | {fmt(max(v))} | {sum(v) / 1e6:.1f} s |')

if which == 'body':
    print('| durability | cell | run | hit | waited | inline | body wait + inline build (summed) | missed rolls\' hold / all hold |')
    print('|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        for cell in LOADCELLS:
            tm = th = 0
            for r, d, w, a in data[(dur, cell)]:
                c = collections.Counter(x['body'] for x in w)
                th_r = sum(x['hold_us'] for x in w)
                tm_r = sum(x['hold_us'] for x in w if x['body'] != 'hit')
                st = sum(x['body_wait_us'] + x['body_us'] for x in w)
                tm += tm_r; th += th_r
                print(f'| {dur} | {cell} | r{r} | {c["hit"]} | {c["waited"]} | {c["inline"]} | {fmt(st)} | {100 * tm_r / th_r:.2f} % |')
            print(f'| {dur} | {cell} | pooled | | | | | {100 * tm / th:.2f} % |')

if which == 'live':
    # segments live and sync breadth at first and last quartile of rolls
    print('| durability | cell | live segments at first / last roll (per run median) | own non-tail sync p50 first quarter | last quarter | waited-out writer syncs per roll | waited tail p50 | waited tail p99 | waited tail max | waited non-tail p50 | waited non-tail max |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    for dur in DURS:
        for cell in LOADCELLS:
            rs = [x for _, _, w, _ in data[(dur, cell)] for x in w]
            first = [x['sync_segs'] for _, _, w, _ in data[(dur, cell)] for x in w[:3]]
            last = [x['sync_segs'] for _, _, w, _ in data[(dur, cell)] for x in w[-3:]]
            q = len(rs) // 4
            ws = [x for _, _, w, _ in data[(dur, cell)] for x in w]
            q1 = [x['sync_rest_us'] for _, _, w, _ in data[(dur, cell)] for x in w[:len(w) // 4]]
            q4 = [x['sync_rest_us'] for _, _, w, _ in data[(dur, cell)] for x in w[-(len(w) // 4):]]
            wt = [x['waited_sync_tail_us'] for x in rs]
            wr = [x['waited_sync_rest_us'] for x in rs]
            ns = statistics.mean(x['waited_syncs'] for x in rs)
            print(f'| {dur} | {cell} | {statistics.median(first):.0f} / {statistics.median(last):.0f} | {fmt(nr(q1, 50))} | {fmt(nr(q4, 50))} | {ns:.2f} | {fmt(nr(wt, 50))} | {fmt(nr(wt, 99))} | {fmt(max(wt))} | {fmt(nr(wr, 50))} | {fmt(max(wr))} |')
