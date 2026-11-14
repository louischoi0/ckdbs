import json
S = '/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad'
R1 = S + '/run/20261114T021002Z/m1'
R2 = S + '/run/m1swap'


def cpu(p):
    d = json.load(open(p))
    return sum(int(r['total_us']) for r in d['wait_stats_rows'] if r['event'] == 'cpu.on_cpu')


reps = []
for i in (1, 2, 3):
    reps.append(('1', i, R1, 'base', 'close'))   # label base = A, close = B
for i in (1, 2, 3):
    reps.append(('2', i, R2, 'close', 'base'))   # label close = A, base = B
rows = []
cpus = []
for batch, i, root, la, lb in reps:
    d = json.load(open(f'{root}/m1-rep{i}.json'))
    ca = d['pools'][la]['cores']; cb = d['pools'][lb]['cores']
    clean = '0' not in ca and '0' not in cb
    matched = set(ca) <= {'2', '3'} and set(cb) <= {'2', '3'}
    tag = 'matched {2,3}' if matched else ('clean' if clean else 'core 0 involved')
    for arm in ('update-disjoint', 'update-disjoint-again', 'update-hot', 'insert-omitted', 'select-hot'):
        a = d['arms'][la][arm]; b = d['arms'][lb][arm]
        rows.append((batch, i, ','.join(ca), ','.join(cb), tag, arm, a, b))
    ca_ = cpu(f'{root}/m1-rep{i}-{la}.views.json'); cb_ = cpu(f'{root}/m1-rep{i}-{lb}.views.json')
    cpus.append((batch, i, ca_, cb_))
print('| batch.rep | core A / B | placement | arm | A p50 | B p50 | Δ p50 (B-A) | A QPS | B QPS | QPS B vs A |')
print('|---|---|---|---|---|---|---|---|---|---|')
for batch, i, ca, cb, tag, arm, a, b in rows:
    if arm not in ('update-disjoint', 'update-hot', 'insert-omitted', 'select-hot'):
        continue
    print(f"| {batch}.{i} | {ca} / {cb} | {tag} | {arm} | {a['p50_us']:.1f} µs | {b['p50_us']:.1f} µs | {b['p50_us']-a['p50_us']:+.1f} µs | {a['qps']:,.0f} | {b['qps']:,.0f} | {100*(b['qps']/a['qps']-1):+.1f} % |")
print()
for batch, i, a, b in cpus:
    print(f"cpu {batch}.{i}: A {a:,} B {b:,} (A-B) per stmt {(a-b)/240000:+.2f} us")
print()
# full percentile table for matched reps
print('| batch.rep | arm | build | ops | p0 | p25 | p50 | p95 | p99 | mean | QPS |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for batch, i, root, la, lb in reps:
    if (batch, i) not in (('2', 1), ('2', 3)):
        continue
    d = json.load(open(f'{root}/m1-rep{i}.json'))
    for arm in ('update-disjoint', 'update-disjoint-again', 'update-hot', 'insert-omitted', 'select-hot'):
        for name, lab in (('A', la), ('B', lb)):
            a = d['arms'][lab][arm]
            print(f"| {batch}.{i} | {arm} | {name} | {a['ops']:,} | {a['p0_us']:.1f} µs | {a['p25_us']:.1f} µs | {a['p50_us']:.1f} µs | {a['p95_us']:.1f} µs | {a['p99_us']:.1f} µs | {a['mean_us']:.1f} µs | {a['qps']:,.0f} QPS |")
