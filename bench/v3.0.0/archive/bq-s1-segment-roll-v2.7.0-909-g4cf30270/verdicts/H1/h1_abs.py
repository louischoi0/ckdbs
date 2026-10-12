"""Absolute step times (ms) per durability over H0's cells (C1 placements + C2),
rolls starting in the measurement window: p50 / p90 / max per step, and the
prediction's 'e-g together are a few ms'."""
import importlib.util, sys
spec = importlib.util.spec_from_file_location('h1', '/home/cdkbs/bench-runs/bq/verdicts/H1/h1_shares.py')
src = open('/home/cdkbs/bench-runs/bq/verdicts/H1/h1_shares.py').read().replace('\nmain()\n', '\n')
ns = {}
exec(compile(src, 'h1_shares.py', 'exec'), ns)
out = sys.argv[1]
rolls = ns['load'](out, 'in')
nr = ns['nearest_rank']
for dur in ('relaxed', 'group'):
    rs = [r for r in rolls if r['dur'] == dur and r['cell'] != 'c3']
    print(f'{dur} n={len(rs)}')
    for k in ['hold', 'a', 'c', 'c_gate', 'c_sync', 'd', 'e', 'f', 'g', 'efg']:
        if k == 'hold':
            xs = [r['hold_us'] for r in rs]
        elif k == 'efg':
            xs = [r['st']['e'] + r['st']['f'] + r['st']['g'] for r in rs]
        else:
            xs = [r['st'][k] for r in rs]
        print(f'  {k:7} p50={nr(xs,50)/1000:8.2f} p90={nr(xs,90)/1000:8.2f} p99={nr(xs,99)/1000:8.2f} max={max(xs)/1000:8.2f} ms  sum={sum(xs)/1e6:7.2f}s')
    print('  bodies', {b: sum(1 for r in rs if r['body'] == b) for b in ('hit', 'waited', 'inline')})
