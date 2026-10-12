import glob, json, re, statistics as st
R = '/home/cdkbs/bench-runs/bq/s2/out/relaxed'
KV = re.compile(r'(\w+)=(\S+)')
for arm in ['s16', 'base', 's256', 's1024']:
    vals = []
    for cell in ['c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3']:
        for r in (1, 2, 3):
            d = f'{R}/{cell}/{arm}/r{r}'
            j = json.load(open(glob.glob(d + '/*.json')[0]))
            w0, w1 = j['measure_from_mono_ns'], j['stop_at_mono_ns']
            bulk = set()
            for lf in glob.glob(d + '/*.s.log'):
                for l in open(lf):
                    if '[wal] roll ' in l:
                        kv = dict(KV.findall(l))
                        if w0 <= int(kv['start_ns']) < w1:
                            bulk.add(int(kv['core']))
            m0, m1 = j['meta_m0'], j['meta_m1']
            tot = 0
            for c in range(j['cores']):
                if c in bulk:
                    continue
                k = f'load_c{c}_waited_wal_stream_us'
                tot += m1.get(k, 0) - m0.get(k, 0)
            vals.append(tot / 1e6)
    print(arm, 'nonbulk stream wait s med', round(st.median(vals), 2), 'range', round(min(vals), 2), round(max(vals), 2))
