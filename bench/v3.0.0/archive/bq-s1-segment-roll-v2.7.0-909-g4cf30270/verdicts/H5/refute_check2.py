import json, glob
B = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
for dur in ('relaxed', 'group'):
    for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2'):
        for r in ('r1', 'r2', 'r3'):
            j = json.load(open(glob.glob(f'{B}/{dur}/{cell}/{r}/*.json')[0]))
            print(dur, cell, r, 'attr', j.get('by_core_attribution'), 'valid', j.get('per_core_valid'),
                  'moves_in_window', j.get('moves_in_window'), 'targets', j.get('targets'), 'cores_end', j.get('cores_end'))
            rbc = j.get('read_by_core')
            if rbc: print('  read_by_core', json.dumps(rbc)[:700])
            if cell == 'c2':
                print('  writer_by_core', json.dumps(j.get('writer_by_core'))[:500])
                w = j['writer_from_schedule']
                print('  wfs', {k: w.get(k) for k in ('n', 'p99', 'p999', 'max')})
