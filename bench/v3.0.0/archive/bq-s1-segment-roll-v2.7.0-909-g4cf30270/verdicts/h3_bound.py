"""H3's upper bound at relaxed: a roll's hold with every non-tail sync
removed (its own and the waited-out writer syncs' rest), per cell."""
import glob, re

ROLL = re.compile(r'\[wal\] roll (.*)$')
OUT = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed'
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3'):
    holds, bounds = [], []
    for f in glob.glob(f'{OUT}/{cell}/r*/*.s.log'):
        for line in open(f):
            m = ROLL.search(line)
            if not m:
                continue
            kv = dict(t.split('=', 1) for t in m.group(1).split() if '=' in t)
            hold = int(kv['hold_us'])
            rest = int(kv.get('sync_rest_us', 0)) + int(kv.get('waited_sync_rest_us', 0))
            holds.append(hold)
            bounds.append(max(0, hold - rest))
    holds.sort(); bounds.sort()
    q = lambda xs, p: xs[min(len(xs) - 1, int(p * len(xs)))]
    print(f'{cell:20} rolls={len(holds)} hold p50={q(holds,.5)/1e3:.0f}ms p99={q(holds,.99)/1e3:.0f}ms | '
          f'without non-tail syncs: p50={q(bounds,.5)/1e3:.0f}ms p99={q(bounds,.99)/1e3:.0f}ms min={bounds[0]/1e3:.0f}ms')
