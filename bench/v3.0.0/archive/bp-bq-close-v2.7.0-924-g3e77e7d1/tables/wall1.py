import sys
sys.path.insert(0, '/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bp-per-core-residue/93896f60-b552-41c7-9a00-cc438c9ec661/scratchpad')
from lib import *

for name in ('AB', 'AQ'):
    out, runs = load_pair(name)
    for key in sorted(out):
        for side in 'AB':
            arms = out[key][side]
            def m(k):
                return [round(x[k], 1) for x in arms]
            print(name, key, side, 'mean_us', m('mean_us'), 'p50', m('p50_us'), 'p99/max', m('p99_us'), 'qps(mean)', [round(1e6 / x['mean_us'], 1) for x in arms],
                  'qps(p50)', [round(1e6 / x['p50_us'], 1) for x in arms], 'drv_cpu', m('driver_cpu_us'), 'bad', [x['bad'] for x in arms])
# competing and host loads
import glob
bad = 0
n = 0
for f in glob.glob(OUT + '/*/*.json'):
    if '/ba/' in f or '/lock/' in f or '/profile/' in f:
        continue
    j = json.load(open(f))
    if 'runs' not in j:
        continue
    for r in j['runs']:
        for a in r['arms']:
            n += 1
            if a['host']['competing']:
                bad += 1
                print('COMPETING', f, a['host'])
        if r['host_before']['competing'] or r['host_after']['competing']:
            print('COMPETING run', f)
print('arms', n, 'with competing', bad)
