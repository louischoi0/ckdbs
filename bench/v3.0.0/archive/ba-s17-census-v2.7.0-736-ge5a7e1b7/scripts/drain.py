import json, glob, collections

S4 = '/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks/bench/v3.0.0/archive/ba-s4-census-v2.7.0-725-gc390a624/kds'
for name, d in (('B', '/home/cdkbs/bench-runs/ba-s17/kds'), ('S4', S4)):
    agg = collections.defaultdict(lambda: [0, 0, 0, 0, 0])
    for f in glob.glob(d + '/*.json'):
        r = json.load(open(f))
        k = (r['durability'], r['cores'])
        a = agg[k]
        a[0] += r['meta_delta'].get('contention_drain_passes_pending', 0)
        a[1] += r['statements']
        a[2] += r['meta_delta'].get('contention_syncs_inline', 0)
        a[3] += r['meta_delta'].get('contention_syncs_writer', 0)
        a[4] += 1
    for k in sorted(agg):
        a = agg[k]
        print(name, k, 'runs', a[4], 'drain passes with a commit staged %s' % format(a[0], ',.0f'), 'statements %s' % format(a[1], ',.0f'),
              'passes/statement %.2f' % (a[0] / a[1]), 'inline syncs %s writer syncs %s' % (format(a[2], ',.0f'), format(a[3], ',.0f')))
