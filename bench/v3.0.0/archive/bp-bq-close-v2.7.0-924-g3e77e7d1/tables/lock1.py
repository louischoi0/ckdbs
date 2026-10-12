import json, statistics
D = '/home/cdkbs/bench-runs/close/out/lock'
med = statistics.median
rows = {}
for mode in ('plain', 'fence'):
    for side in ('S6A', 'B'):
        for r in range(3):
            f = f'{D}/{side}-locks-relaxed-k8-{mode}-r{r}.json'
            m = json.load(open(f))
            b = json.load(open(f.replace('.json', '.bench.json')))
            stm = b['sessions'] * b['ops_per_session_per_arm'] * 6
            a = b['arms']['B']
            ma, mb = m['meta_after'], m['meta_before']
            dl = lambda k: ma.get(k, 0) - mb.get(k, 0)
            rec = dict(cpu=m['server_cpu_s'] * 1e6 / stm, stm=stm,
                       ud=a['update-disjoint'], uda=a['update-disjoint-again'], ping=a['ping'], io=a['insert-omitted'], sh=a['select-hot'], uh=a['update-hot'],
                       walks=dl('contention_fence_walks') / stm, pw=dl('contention_lock_partition_waits') / stm, pwus=dl('contention_lock_partition_wait_us') / stm,
                       wfw=dl('contention_lock_wait_for_waits') / stm, bench_s=m['bench_s'], lb=m['loadavg_before'], sha=m['bin_sha256'][:8], dev=m['data_device'][:20],
                       holder=m['holder_log'])
            rows[(mode, side, r)] = rec
            print(mode, side, r, 'cpu %.2f' % rec['cpu'], 'ud qps %.0f p0/25/50/95/99 %s %s %s %s %s max %s' % (rec['ud']['qps'], rec['ud']['p0_us'], rec['ud']['p25_us'], rec['ud']['p50_us'], rec['ud']['p95_us'], rec['ud']['p99_us'], rec['ud']['max_us']),
                  'again %.0f ping %.0f' % (rec['uda']['qps'], rec['ping']['qps']), 'walks/stmt %.3f pw/stmt %.4f pwus/stmt %.3f wfw %.5f' % (rec['walks'], rec['pw'], rec['pwus'], rec['wfw']),
                  'bench_s', rec['bench_s'], rec['lb'], rec['sha'], rec['dev'], 'uh errs', rec['uh']['errors'])
print()
for side in ('S6A', 'B'):
    for mode in ('plain', 'fence'):
        v = [rows[(mode, side, r)] for r in range(3)]
        print(side, mode, 'cpu med %.2f' % med([x['cpu'] for x in v]), 'range', [round(x['cpu'], 2) for x in v],
              'pw/stmt', [round(x['pw'], 4) for x in v], 'pwus/stmt', [round(x['pwus'], 3) for x in v])
for r in range(3):
    print('r', r, 'premium S6A %.1f%%' % (100 * (rows[('fence', 'S6A', r)]['cpu'] / rows[('plain', 'S6A', r)]['cpu'] - 1)),
          'B %.1f%%' % (100 * (rows[('fence', 'B', r)]['cpu'] / rows[('plain', 'B', r)]['cpu'] - 1)))
