import json
p = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/plans4.json'))
m = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/gain4-meta.json'))
for c, v in p.items():
    print(c)
    for k in ('A', 'B', 'BO'):
        t = '|'.join(v[k])
        i = t.find('planned order')
        print('  ', k, t[:130].split('|')[0][:120], '::', t[i:i + 70] if i >= 0 else 'no reorder', '::', [x for x in v[k] if 'Scan freights' in x and 'opens' in x or 'Scan model_results' in x and 'opens' in x][:1])
for k, v in m.items():
    print(k, {x: v.get(x) for x in ('plan_stats_generation', 'plan_stats_relations', 'plan_stats_seeds', 'plan_stats_seed_pages', 'plan_stats_steward_us_max', 'pool_misses', 'pool_resident', 'plan_memo_hits')})
