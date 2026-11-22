import json
d = json.load(open('/home/louis/bench-runs/dp-close/archive/q234/dp_close.json'))
print(d['sha256'], d['started'])
def f(x, base):
    dl = x.get('delta_vs_' + base, {})
    l = x['lat']
    return (x['cpu_us_median'], dl.get('median_us'), dl.get('min_us'), dl.get('max_us'),
            'lat', l['ops'], l['p0'], l['p25'], l['p50'], l['p95'], l['p99'])
for n, v in d['q2'].items():
    print('q2', n, 'host', v['host_before']['loadavg'], v['host_after']['loadavg'])
    for k, x in v['arms'].items():
        print('   ', k, f(x, 'N1'))
for n, v in d['q3'].items():
    for s, w in v.items():
        print('q3', n, s, 'rows/stmt', w['rows_per_stmt'], 'k', w['k'], 'host', w['host_before']['loadavg'], w['host_after']['loadavg'])
        for k, x in w['arms'].items():
            print('   ', k, f(x, 'H'))
print(d['q4']['depth_past_bound'])
v = d['q4']['depths']
print('q4 host', v['host_before']['loadavg'], v['host_after']['loadavg'])
for k, x in v['arms'].items():
    print('   ', k, f(x, 'H'))
