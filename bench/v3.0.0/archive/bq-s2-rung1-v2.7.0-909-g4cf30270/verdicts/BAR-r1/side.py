import glob, json, os, sys
OUT = sys.argv[1]
for c in ['c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso']:
    for dur in ('relaxed', 'group'):
        for d in sorted(glob.glob(f'{OUT}/{dur}/{c}/*/r*')):
            g = glob.glob(d + '/*.json')
            if not g:
                print(dur, c, d, 'NO JSON'); continue
            j = json.load(open(g[0]))
            print(dur, c, d[len(OUT):], 'wal_rolls', j['meta_delta'].get('wal_rolls'), 'load', j['host_before']['loadavg'],
                  'longest', {k: v for k, v in j.get('meta_longest', {}).items()} if isinstance(j.get('meta_longest'), dict) else None)
for f in sorted(glob.glob(f'{OUT}/mount/*.json')):
    j = json.load(open(f))
    print(os.path.basename(f), {k: v for k, v in j.items() if not isinstance(v, (dict, list))})
    for k, v in j.items():
        if isinstance(v, dict):
            print('   ', k, {a: b for a, b in v.items() if not isinstance(b, (dict, list))})
# c2 s1024 r1 latency max
for d in sorted(glob.glob(f'{OUT}/relaxed/c2/s1024/r*')):
    j = json.load(open(glob.glob(d + '/*.json')[0]))
    print('c2 s1024', d[-2:], 'wfs max', j['writer_from_schedule']['max_us'])
