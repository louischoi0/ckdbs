import sys, statistics
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

LOADCELLS = ['c3', 'c2', 'c1-segregated-no0', 'c1-batch0', 'c1-spread']
mode = sys.argv[1]
if mode == 'cover':
    print('| durability | cell | roll hold, summed per run (r1 / r2 / r3) | share of the 30 s window under a hold (r1 / r2 / r3) |')
    print('|---|---|---|---|')
    for dur in DURS:
        for cell in LOADCELLS:
            hs, sh = [], []
            for r, d, sl in runs(dur, cell):
                w, _ = rolls(sl, d)
                secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
                h = sum(x['hold_us'] for x in w) / 1e6
                hs.append(f'{h:.1f} s'); sh.append(f'{100 * h / secs:.1f} %')
            print(f'| {dur} | {cell} | ' + ' / '.join(hs) + ' | ' + ' / '.join(sh) + ' |')

if mode == 'sync':
    print('| durability | cell | run | writer syncs/s (wal_syncs) | log MiB per sync | syncs inline | syncs by the writer thread |')
    print('|---|---|---|---|---|---|---|')
    for dur in DURS:
        for cell in ['c3', 'c2', 'c2-iso', 'c1-segregated-no0', 'c1-iso-segregated-no0']:
            for r, d, sl in runs(dur, cell):
                secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
                s = delta(d, 'wal_syncs')
                lsn = delta(d, 'wal_append_lsn') / 2**20
                print(f'| {dur} | {cell} | r{r} | {s / secs:.1f} | {(lsn / s) if s else 0:.2f} | {delta(d, "contention_syncs_inline"):.0f} | {delta(d, "contention_syncs_writer"):.0f} |')

if mode == 'p99bound':
    # share of rolls whose waited syncs: count of waited syncs and total tail wait vs 30 s window
    for dur in DURS:
        for cell in ['c3']:
            for r, d, sl in runs(dur, cell):
                w, _ = rolls(sl, d)
                secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
                ws = sum(x['waited_syncs'] for x in w)
                wt = sum(x['waited_sync_tail_us'] + x['waited_sync_rest_us'] for x in w) / 1e6
                gt = sum(x['sync_gate_us'] for x in w) / 1e6
                print(dur, cell, f'r{r}', 'syncs waited by rolls', ws, 'of', delta(d, 'wal_syncs'), 'their summed duration', f'{wt:.1f} s', 'gate wait', f'{gt:.1f} s', 'window', f'{secs:.1f} s')
