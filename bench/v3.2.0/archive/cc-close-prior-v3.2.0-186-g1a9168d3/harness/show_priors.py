import json, sys
d = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/priors.json'))
B = d['B']
print(B['cols'])
print(json.dumps(B['rows'][0]))
for r in B['rows']:
    print(r['name'], r.get('height'), r.get('est_rows'), r['true_rows'])
for k in ('B', 'A'):
    for ph in ('after_mount', 'after_statements', 'after_idle'):
        m = d[k][ph]
        print(k, ph, {x: m.get(x) for x in ('plan_stats_generation', 'plan_stats_seeds', 'plan_stats_seed_pages', 'plan_stats_publishes', 'plan_stats_steward_us_max', 'pool_misses', 'pool_hits', 'pool_resident')})
print(B['wait_log'])
