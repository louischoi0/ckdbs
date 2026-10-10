import json, glob, os, statistics as st, sys
S = [json.loads(l) for l in open('/home/cdkbs/bench-runs/ba-close/host-samples.jsonl')]


def win(d):
    fs = glob.glob(d + '/*.json')
    m = [os.path.getmtime(f) for f in fs]
    return min(m) - 12, max(m)


def stat(name, a, b):
    w = [s for s in S if a <= s['t'] <= b]
    if not w:
        print(name, 'no samples')
        return
    named = sum(1 for s in w if any(k in x for x in s['foreign'] for k in ('kds_tests', 'ctest', 'cc1plus', 'cmake')))
    other = [round(st.mean(s['busy'][c] for s in w)) for c in ('0', '1', '2', '3', '6')]
    print(name, len(w), 'samples; load1 %.2f-%.2f' % (min(float(s['load']) for s in w), max(float(s['load']) for s in w)),
          '; samples with kds_tests/ctest/cc1plus/cmake busy: %d' % named, '; mean busy CPUs 0,1,2,3,6: %s' % other)


for d in ('overheadq3-c2', 'overheadq3-group-c2', 'overheadq-c1', 'overheadq2-c1', 'overhead-strict-c2'):
    a, b = win('/home/cdkbs/bench-runs/ba-close/' + d)
    stat(d, a, b)
import datetime
for name, t0, t1 in (('s0', '08:54:20', '08:57:30'), ('pgctl', '08:49:40', '08:54:20'), ('census-rerun', '08:00:00', '08:19:14')):
    h = lambda x: datetime.datetime.strptime('2026-10-10 ' + x, '%Y-%m-%d %H:%M:%S').replace(tzinfo=datetime.timezone.utc).timestamp()
    stat(name, h(t0), h(t1))
