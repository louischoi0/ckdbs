import json, glob, os
R = os.path.dirname(os.path.abspath(__file__))
for f in sorted(glob.glob(R + '/json/p*-s1.json')):
    m = json.load(open(f))['meta']
    print(os.path.basename(f), 'verify_problems=', m.get('verify_problems'), 'durability=', m.get('durability'), 'cores=', m.get('cores'), [k for k in m if 'verif' in k])
print('rc:', sorted({open(f).read().strip() for f in glob.glob(R + '/logs/p*.rc')}))
n = 0
for f in glob.glob(R + '/logs/p*-sweep.verify'):
    n += open(f).read().count(' OK')
print('sweep verify OK lines:', n, '(expect 6 per run x 6 runs = 36)')
for f in glob.glob(R + '/logs/*.serverlog'):
    t = open(f).read()
    if any(w in t.lower() for w in ('error', 'corrupt', 'fatal')):
        print('serverlog mentions error:', os.path.basename(f))
