import glob, json
OUT = '/home/cdkbs/bench-runs/close/out/ba'
for side in 'AQB':
    v = []
    for r in (1, 2, 3):
        j = json.load(open(glob.glob(f'{OUT}/insert1-{side}-r{r}/*.json')[0]))
        v.append(j['meta_delta']['contention_wal_stream_wait_us'] / j['statements'])
    print(side, [round(x, 3) for x in v])
