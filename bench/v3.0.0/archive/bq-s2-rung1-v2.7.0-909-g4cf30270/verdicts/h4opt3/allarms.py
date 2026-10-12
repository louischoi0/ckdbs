"""Every arm's best relaxed NB ratio against the S2 isolated arm (does any arm meet the bar?),
plus C3 rows/s and hold p50 per run for every arm (to see whether r1's host period hurt every arm)."""
import sys
sys.path.insert(0, '/home/cdkbs/bench-runs/bq/verdicts/s2/h4opt3')
import importlib.util, io, contextlib
spec = importlib.util.spec_from_file_location('o', '/home/cdkbs/bench-runs/bq/verdicts/s2/h4opt3/opt3.py')
o = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(o)
ARMS = ('base', 'opt3', 's16', 's256', 's1024')
for cell, iso in (('c1-segregated-no0', 'c1-iso-segregated-no0'), ('c1-batch0', 'c1-iso-batch0'),
                  ('c1-spread', 'c1-iso-spread'), ('c2', 'c2-iso')):
    i = o.ISO[iso]
    for arm in ARMS:
        rs = [o.run(cell, arm, r) for r in (1, 2, 3)]
        p99 = [R['nb'][99] for R in rs]
        p999 = [R['nb'][99.9] for R in rs]
        r99 = [round(x / i['p99_us'], 1) if isinstance(x, float) else x for x in p99]
        r999 = [round(x / i['p99.9_us'], 1) if isinstance(x, float) else x for x in p999]
        met = [isinstance(a, float) and a <= 2 and isinstance(b, float) and b <= 5 for a, b in zip(r99, r999)]
        print(f'{cell:18} {arm:5} NB p99/iso={r99} p99.9/iso={r999} any run meets both={any(met)}')
for arm in ARMS:
    rs = [o.run('c3', arm, r) for r in (1, 2, 3)]
    print(f'c3 {arm:5} rows/s={[round(R["j"]["inserter_rows_per_s"]) for R in rs]} '
          f'hold p50 ms={[round(o.nr([x["hold_us"] for x in R["win"]], 50) / 1e3, 1) for R in rs]} '
          f'rolls={[len(R["win"]) for R in rs]}')
