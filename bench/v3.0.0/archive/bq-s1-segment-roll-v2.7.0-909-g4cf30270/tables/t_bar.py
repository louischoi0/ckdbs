import sys, statistics
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

PAIRS = [('c1-segregated-no0', 'c1-iso-segregated-no0'), ('c1-batch0', 'c1-iso-batch0'),
         ('c1-spread', 'c1-iso-spread'), ('c2', 'c2-iso')]


def sched(d, cell):
    return d['writer_from_schedule'] if cell.startswith('c2') else d['read_from_schedule']


print('| durability | cell | figure | mixed (r1 / r2 / r3) | spread of mixed | isolated (r1 / r2 / r3) | spread of isolated | median ratio (bar: 2x p99, 5x p99.9) | ratio range (min mixed / max iso .. max mixed / min iso) | bar |')
print('|---|---|---|---|---|---|---|---|---|---|')
for dur in DURS:
    for mixed, iso in PAIRS:
        for q, lim in (('p99_us', 2), ('p99.9_us', 5)):
            m = [sched(d, mixed)[q] for _, d, _ in runs(dur, mixed)]
            i = [sched(d, iso)[q] for _, d, _ in runs(dur, iso)]
            ratio = statistics.median(m) / statistics.median(i)
            lo, hi = min(m) / max(i), max(m) / min(i)
            ok = 'met' if ratio <= lim else 'missed'
            if ratio <= lim and hi > lim:
                ok = 'met on medians; pairing decides'
            print(f'| {dur} | {mixed} | schedule {q.replace("_us", "")} | ' + ' / '.join(fmt(x) for x in m) + f' | {fmt(max(m) - min(m))} | ' + ' / '.join(fmt(x) for x in i) + f' | {fmt(max(i) - min(i))} | {ratio:,.2f}x (limit {lim}x) | {lo:,.2f}x .. {hi:,.1f}x | {ok} |')
