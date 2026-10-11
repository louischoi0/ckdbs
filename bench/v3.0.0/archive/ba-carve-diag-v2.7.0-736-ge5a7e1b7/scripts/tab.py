import json, glob, os, re
D = '/home/cdkbs/bench-runs/carve-diag'
for d in sorted(glob.glob(D + '/out*')):
    js = glob.glob(d + '/*.json')
    if not js:
        print(os.path.basename(d), 'no json'); continue
    j = json.load(open(js[0]))
    flat = {}
    def walk(o, p=''):
        if isinstance(o, dict):
            for k, v in o.items(): walk(v, k)
        elif isinstance(o, list):
            for v in o: walk(v, p)
        else:
            if re.search(r'carve|checkpoint', p): flat[p] = o
    walk(j)
    keys = [k for k in flat if 'longest' in k or k.endswith('carves') or k.endswith('checkpoint_runs')]
    print(os.path.basename(d), {k: flat[k] for k in sorted(keys)}, 'stmt/s', j.get('stmt_per_s', j.get('throughput')))
