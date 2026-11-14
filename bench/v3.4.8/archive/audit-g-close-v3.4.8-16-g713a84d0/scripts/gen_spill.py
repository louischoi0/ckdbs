import json, statistics as st
S = '/tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad'
print('| rows | phase | build | ops/rep | p0 | p25 | p50 | p95 | p99 | QPS | QPS, B vs A |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for rows in (200, 1000, 10000):
    data = {l: [json.load(open(f'{S}/out/spill-{l}-{rows}-r{r}.json'))['phases'] for r in (1, 2)] for l in 'AB'}
    for ph in ('insert-spill', 'insert-inline', 'update-spill', 'update-inline'):
        q = {}
        for l in 'AB':
            ps = [[p for p in run if p['phase'] == ph][0] for run in data[l]]
            q[l] = {k: st.mean(p[k] for p in ps) for k in ('p0_us', 'p25_us', 'p50_us', 'p95_us', 'p99_us', 'qps')}
            q[l]['ops'] = ps[0]['ops']
        for l in 'AB':
            d = '' if l == 'A' else f"{100*(q['B']['qps']/q['A']['qps']-1):+.1f} %"
            x = q[l]
            print(f"| {rows} | {ph} | {l} | {x['ops']:,} | {x['p0_us']:.0f} µs | {x['p25_us']:.0f} µs | {x['p50_us']:.0f} µs | {x['p95_us']:.0f} µs | {x['p99_us']:.0f} µs | {x['qps']:,.0f} QPS | {d} |")
# per-rep QPS spread for the spill phases
print()
for rows in (200, 1000, 10000):
    for ph in ('insert-spill', 'update-spill'):
        s = []
        for l in 'AB':
            s.append(l + ' ' + ' / '.join(f"{[p for p in json.load(open(f'{S}/out/spill-{l}-{rows}-r{r}.json'))['phases'] if p['phase']==ph][0]['qps']:,.0f}" for r in (1, 2)))
        print(rows, ph, '; '.join(s))
