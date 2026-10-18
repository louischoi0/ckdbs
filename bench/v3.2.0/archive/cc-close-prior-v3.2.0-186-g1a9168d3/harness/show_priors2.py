import json
d = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/priors.json'))
r2 = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/restart.json'))
old = json.load(open('/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/bench/v3.2.0/archive/cc-close-v3.2.0-184-gc547ef14/R1/priors.json'))
oldm = {r['name']: r for r in old['rows']}
K = ('plan_stats_generation', 'plan_stats_relations', 'plan_stats_seeds', 'plan_stats_seed_pages', 'plan_stats_publishes',
     'plan_stats_steward_us_max', 'pool_misses', 'pool_hits', 'pool_resident')
for k in ('B', 'A'):
    for ph in ('after_mount', 'after_statements', 'after_idle', 'after_view', 'after_count'):
        if ph in d[k]: print(k, ph, {x: d[k][ph].get(x) for x in K})
for l in d['B']['wait_log']: print(l['t'], l['meta'])
print('RESTART')
for ph in ('after_mount', 'after_statements', 'after_idle', 'after_view'):
    print(ph, {x: r2[ph].get(x) for x in K})
for l in r2['wait_log']: print(l['t'], l['meta'])
print('PRIORS fresh / restart / old')
lo, hi = 9, 0
rr = {r['name']: r for r in r2['rows']}
for r in d['B']['rows']:
    if r['est_rows'] is None: continue
    t = int(r['true_rows']); e = int(r['est_rows']); o = oldm.get(r['name'])
    oe = int(o['est_rows']) if o and o.get('est_rows') else None
    e2 = rr.get(r['name'], {}).get('est_rows')
    print(f"{r['name']:22} h={r['height']} leaves={r['leaves']:>5} true={t:>7} est={e:>7} r={e/t:.2f} old_est={oe} old_r={(oe/t if oe else 0):.2f} restart_est={e2} restart_r={(int(e2)/t if e2 else 0):.2f}")
    lo = min(lo, e / t); hi = max(hi, e / t)
print('range', lo, hi)
