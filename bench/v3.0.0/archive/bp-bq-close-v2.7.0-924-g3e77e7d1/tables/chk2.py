import glob, json
OUT = '/home/cdkbs/bench-runs/close/out'
d = []
for f in glob.glob(OUT + '/lock/*.bench.json'):
    b = json.load(open(f))
    a = b['arms']['B']
    d.append(abs(a['update-disjoint']['qps'] - a['update-disjoint-again']['qps']) / a['update-disjoint']['qps'] * 100)
print('lock repeat arm diff pct: min %.2f max %.2f n %d' % (min(d), max(d), len(d)))
v = []
for f in glob.glob(OUT + '/*/*.json'):
    if '/ba/' in f or '/lock/' in f or '/profile/' in f:
        continue
    j = json.load(open(f))
    if 'runs' not in j:
        continue
    for r in j['runs']:
        v += list(r['idle_cpu_us_per_s'].values())
print('idle burn us/s: min %.0f max %.0f n %d' % (min(v), max(v), len(v)))
# profile passes idle?
for s in ('pre', 'post', 'fix', 'close'):
    j = json.load(open(f'{OUT}/profile/{s}.json'))
    print(s, j['runs'][0]['idle_cpu_us_per_s'])
