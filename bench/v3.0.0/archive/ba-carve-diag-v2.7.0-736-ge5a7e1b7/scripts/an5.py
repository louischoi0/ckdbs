import re, glob
from datetime import datetime
D = '/home/cdkbs/bench-runs/carve-diag'
pat = re.compile(r'^(\d+) (\d+:\d+:\d+\.\d+) (\w+)\(\d+<([^>]*)>.*<([\d.]+)>$')
for f in sorted(glob.glob(D + '/out[45]-*/strace.txt')):
    first = last = None
    slow = []
    for line in open(f):
        m = pat.match(line.rstrip())
        if not m:
            continue
        t = datetime.strptime(m.group(2), '%H:%M:%S.%f')
        first = first or t
        last = t
        if m.group(4).endswith('d.db') and float(m.group(5)) > 0.05:
            slow.append(((t - first).total_seconds(), float(m.group(5)) * 1000))
    total = (last - first).total_seconds()
    print(f'{f.split("/")[-2]:18} life={total:5.1f}s slow data fsyncs (offset s, ms): ' +
          ', '.join(f'{o:.1f}/{d:.0f}' for o, d in slow))
