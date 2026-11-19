import json, sys
for f in sys.argv[1:]:
    for l in open(f):
        r = json.loads(l)
        n = r['statements']; d = r['delta']
        print(f.split('/')[-2], r['label'], round(r['stmt_per_s']), {k: round(v) for k, v in r['lat_us'].items()},
              'cpu', round(r['cpu_us_per_stmt'], 1), 'logB', round(d['wal_append_lsn'] / n, 1),
              'fi', d['pool_fetch_images'], 'drained', d['pool_dirty_drained'], 'misses', d['pool_misses'],
              'ckpg', d['contention_checkpoint_pages_written'],
              'ckdur', [c['duration_us'] for c in r['checkpoints']], 'load_s', round(r['load_s']), r['loadavg'],
              'ringfull', d['wal_ring_full'], 'syncs', d['wal_syncs'])
        print('   waits us/stmt', {k: round(v[1] / n, 1) for k, v in r['waits_us'].items() if v[1] / n > 0.5}, 'mean', round(r['lat_mean_us']))
