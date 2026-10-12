import sys, os, re, json, glob, statistics
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lib import *

frags = {}
# ---- host table from the stamps ----
groups = {}
for f, kind in ((OUT + '/hoststamps.log', 'chain'), (OUT + '/bisect/stamps.log', 'bisect'), (OUT + '/bisect2/stamps.log', 'bisect2'),
                (OUT + '/aligned/stamps.log', 'aligned'), (OUT + '/rollfix/stamps.log', 'rollfix')):
    for line in open(f):
        m = re.match(r'(\S+ \S+) (.*?) load=(\S+) (\S+) (\S+)(?: competing=(\d+))?$', line.strip())
        if not m or ' after ' in line:
            continue
        t, what, l1, l5, l15, comp = m.groups()
        key = what.split()[0] + (' ' + what.split()[1] if what.split()[0] in ('bn', 'ba', 'lock') else '')
        if kind == 'chain':
            if what.startswith('ba '):
                key = 'ba ' + what.split()[1]
            elif what.startswith('bn '):
                key = 'bn'
            elif what.startswith('lock '):
                key = 'lock'
        else:
            key = kind
        groups.setdefault(key, []).append((t, float(l1), float(l5), float(l15), int(comp) if comp else None))
lines = ['| cell group | cells | first - last start (UTC) | 1-min load at start | 5-min load | 15-min load | resident non-benchmark `python3`/`kds_server` processes counted by the gate |', '|---|---|---|---|---|---|---|']
names = {'bn': 'cores = 1 pairs AB, AQ (`bn_census`)', 'ba point': 'cores = 4 `point`', 'ba insert1': 'cores = 4 `insert1`', 'lock': 'lock cells, cores = 8',
         'bisect': 'bisect: S2S1, S4S6BQ', 'bisect2': 'bisect: MERGE, S4, S6', 'aligned': 'MERGEALIGNED', 'rollfix': 'PREMERGE_FIX, CLOSE_FIX'}
for k in ('bn', 'ba point', 'ba insert1', 'lock', 'bisect', 'bisect2', 'aligned', 'rollfix'):
    v = groups[k]
    comp = sorted(set(x[4] for x in v if x[4] is not None))
    lines.append(f"| {names[k]} | {len(v)} | {v[0][0][11:]} - {v[-1][0][11:]} | {min(x[1] for x in v):.2f} - {max(x[1] for x in v):.2f} | {min(x[2] for x in v):.2f} - {max(x[2] for x in v):.2f} | "
                 f"{min(x[3] for x in v):.2f} - {max(x[3] for x in v):.2f} | {', '.join(str(c) for c in comp) if comp else 'not recorded by this script'} |")
frags['host'] = '\n'.join(lines)

# ---- profile ----
P = OUT + '/profile'
d = json.load(open(HERE + '/prof2.json'))
srv = {}
for side in ('pre', 'post', 'fix', 'close'):
    for l in open(f'{P}/{side}.log'):
        if l.startswith('{') and '"prof"' in l:
            j = json.loads(l)
            srv[(side, j['path'])] = j['server_cpu_us']
nm = 'kds::wal::WalManager::Append'
lines = ['| profiled binary | dump | samples | `WalManager::Append` self (samples, share) | `WalManager::Append` inclusive | `WalStream::Append` (its callee) | server CPU per statement under the sampler (literal / prepared) |', '|---|---|---|---|---|---|---|']
lab = {'pre': '`ccf9cfc1`', 'post': '`6139c2de`', 'fix': 'rollfix', 'close': 'B `3e77e7d1`'}
for side in ('pre', 'post', 'fix', 'close'):
    tot = [0, 0, 0, 0]
    for i, (n, sl, inc) in enumerate(d[side]):
        tot[0] += n
        tot[1] += sl.get(nm, 0)
        tot[2] += inc.get(nm, 0)
        tot[3] += sl.get('kds::wal::WalStream::Append', 0)
        lines.append(f"| {lab[side]} | dump.{i} ({'literal' if i == 0 else 'prepared'}) | {n} | {sl.get(nm,0)} ({100*sl.get(nm,0)/n:.2f} %) | {inc.get(nm,0)} ({100*inc.get(nm,0)/n:.2f} %) | {sl.get('kds::wal::WalStream::Append',0)} ({100*sl.get('kds::wal::WalStream::Append',0)/n:.2f} %) | "
                     f"{srv[(side,'literal')] if i==0 else srv[(side,'prepared')]:,.0f} µs |")
    lines.append(f"| **{lab[side]} both dumps** | | **{tot[0]}** | **{tot[1]} ({100*tot[1]/tot[0]:.2f} %)** | **{tot[2]} ({100*tot[2]/tot[0]:.2f} %)** | **{tot[3]} ({100*tot[3]/tot[0]:.2f} %)** | |")
frags['profile'] = '\n'.join(lines)

# Poisson comparison
import math
def z(a, na, b, nb):
    pa, pb = a / na, b / nb
    p = (a + b) / (na + nb)
    return (pb - pa) / math.sqrt(p * (1 - p) * (1 / na + 1 / nb))
tot = {}
for side in ('pre', 'post', 'fix', 'close'):
    tot[side] = (sum(n for n, sl, inc in d[side]), sum(sl.get(nm, 0) for n, sl, inc in d[side]))
for s in ('post', 'fix', 'close'):
    print('z pre->', s, round(z(tot['pre'][1], tot['pre'][0], tot[s][1], tot[s][0]), 2))
print('z fix vs post', round(z(tot['post'][1], tot['post'][0], tot['fix'][1], tot['fix'][0]), 2))
for k, v in frags.items():
    open(f'{HERE}/frag_{k}.md', 'w').write(v + '\n')
    print('=====', k)
    print(v)
