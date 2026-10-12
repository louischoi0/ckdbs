import glob, json
OUT = '/home/cdkbs/bench-runs/close/out'
n = bad = 0
for f in glob.glob(OUT + '/ba/*/*.json'):
    j = json.load(open(f))
    n += 1
    if j['host_before']['competing'] or j['host_after']['competing']:
        bad += 1
        print('competing', f, j['host_before'], j['host_after'])
print('ba cells', n, 'with competing', bad)
# profile jsons (bn_census runs with a sampler)
for f in glob.glob(OUT + '/profile/*.json'):
    j = json.load(open(f))
    for r in j['runs']:
        print(f.split('/')[-1], r['host_before'], r['host_after'], r.get('quiet_gate'))
# bn arms, count by dir
for d in ('bn', 'bisect', 'bisect2', 'aligned', 'rollfix'):
    c = 0
    for f in glob.glob(f'{OUT}/{d}/*.json'):
        j = json.load(open(f))
        for r in j['runs']:
            c += len(r['arms'])
            for a in r['arms']:
                assert a['bad'] == 0, (f, a['shape'], a['bad'])
            assert all(v['range_v_ok'] and v['spill_txt_ok'] for v in r['end_state'].values()), f
            for k in ('pages-unspilled', 'pages-spilled'):
                for v in r[k].values():
                    assert v['pages_allocated_delta'] == 0, (f, k, v)
    print(d, 'arms', c, 'all bad=0, end states ok, pages_allocated_delta=0')
