import json, glob
for n in ('relaxed-mixed-r1', 'relaxed-mixed-r2', 'group-mixed-r1', 'strict-mixed-r1'):
    j = json.load(open(glob.glob(f'/home/ukjin/bench-runs/dy-s0/out/{n}/*.json')[0]))
    j = j if isinstance(j, dict) else j[0]
    print(n, 'targets', j['targets'], 'roles', j['roles'])
    for k, v in j['read_by_core_end'].items():
        print('  core', k, str(v)[:400])
    print('  readers (index, core, core_end)', [(c['index'], c['core'], c['core_end']) for c in j['clients'] if c['role'] == 'reader'], 'moves', j['moves'])
