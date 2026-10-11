import glob, re
wb, sy = [], []
for f in glob.glob('/home/cdkbs/bench-runs/carve-diag/out2-diag-*/server.stderr'):
    for line in open(f):
        m = re.search(r'persist-page writeback_us=(\d+) sync_us=(\d+)', line)
        if m:
            wb.append(int(m.group(1))); sy.append(int(m.group(2)))
wb.sort(); sy.sort()
n = len(sy)
q = lambda v, p: v[min(len(v) - 1, int(len(v) * p))]
print('n', n, 'wb p50', q(wb, .5), 'max', wb[-1])
print('sync p50', q(sy, .5), 'p90', q(sy, .9), 'max', sy[-1], 'over 50ms', sum(1 for s in sy if s > 50000))
