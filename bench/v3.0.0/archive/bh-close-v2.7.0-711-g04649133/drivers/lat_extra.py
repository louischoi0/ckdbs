import glob, json
tot = {'reader': [0, 0], 'writer': [0, 0]}
rows = []
for f in sorted(glob.glob('/home/cdkbs/bench-runs/bh-close/purge-lat/c*-R*.json')):
    j = json.load(open(f))
    for k in ('reader', 'writer'):
        for s in j['background'][k]:
            tot[k][0] += s['ops']; tot[k][1] += s['errors']
    rows.append((f.split('/')[-1], j['run_s'], j['purge_first_attempt']['p50_us'], j['purge_first_attempt']['p99_us'],
                 j['delete']['p50_us'], j['delete']['p99_us'], j['delete_to_purge']['p50_us'], j['delete_to_purge']['p99_us']))
print(tot)
for r in rows:
    print(r)
