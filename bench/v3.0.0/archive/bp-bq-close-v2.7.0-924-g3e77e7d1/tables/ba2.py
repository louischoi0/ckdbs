import glob, json, statistics, collections
OUT = '/home/cdkbs/bench-runs/close/out/ba'
for shape in ('point', 'insert1'):
    for side in 'AQB':
        for r in (1, 2, 3):
            f = glob.glob(f'{OUT}/{shape}-{side}-r{r}/*.json')[0]
            j = json.load(open(f))
            md = j['meta_delta']
            hist = collections.Counter(int(c) for c in j['session_cores'])
            rolls = {k: md[k] for k in md if 'roll' in k and md[k]}
            ws = {k: md[k] for k in ('contention_wal_stream_waits', 'contention_wal_stream_wait_us', 'contention_page_latch_waits', 'contention_frame_table_waits', 'contention_lock_partition_waits', 'wal_syncs', 'pages_allocated') if k in md}
            print(shape, side, r, j['statements_per_s'], 'p0/25/50/95/99/max', j['p0_us'], j['p25_us'], j['p50_us'], j['p95_us'], j['p99_us'], j['max_us'],
                  'place', [hist.get(c, 0) for c in range(4)], 'first_err', j['first_error'], 'sha', j['bin_sha256'][:8], 'dev', j['data_device'][:30])
            print('     ', ws, rolls)
            print('      mid', str(j['meta_gauges_mid'])[:200], 'longest', str(j['meta_longest'])[:200], 'setup', str(j['setup_meta'])[:150])
