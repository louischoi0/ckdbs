import json, collections
R = []
for f in ['run1/out.jsonl', 'run2/out-pressure.jsonl', 'run3/out-pressure.jsonl']:
    for l in open(f):
        r = json.loads(l)
        if not r.get('recovery'):
            R.append(r)
a = lambda x: sum(x) / len(x)
want = ['select-pk-10000-c1', 'update-lit-10000-c1', 'update-lit-100000-c1', 'update-spill-assign-10000-c1',
        'insert-0-c1', 'update-lit-100000-c4', 'update-lit-1000000-c1']
print('| Cell | cores | build | client mean | commit.wal_flush | sched.run_queue | cpu.on_cpu | io.page_read | client+socket (remainder) | wal_flush share |')
print('|---|---|---|---|---|---|---|---|---|---|')
for c in want:
    for L in 'AB':
        rs = [r for r in R if r['cell'] == c and r['label'] == L]
        def w(ev):
            return a([r['waits_us'].get(ev, [0, 0])[1] / r['statements'] for r in rs])
        m = a([r['lat_mean_us'] for r in rs])
        f, q, o, p = w('commit.wal_flush'), w('sched.run_queue'), w('cpu.on_cpu'), w('io.page_read')
        rem = m - f - q - o - p
        print(f"| {c.rsplit('-c',1)[0]} | {c.rsplit('-c',1)[1]} | {L} | {m:,.0f} us | {f:,.0f} us | {q:,.0f} us | {o:,.0f} us | {p:,.1f} us | {rem:,.0f} us | {100*f/m:.0f} % |")
