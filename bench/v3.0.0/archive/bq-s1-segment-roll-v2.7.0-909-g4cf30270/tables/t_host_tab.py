import sys, glob, json, collections
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

which = sys.argv[1]
rows = collections.defaultdict(list)
for f in sorted(glob.glob(OUT + '/*/**/bl-mixed-*.json', recursive=True)):
    d = json.load(open(f))
    parts = f[len(OUT) + 1:].split('/')
    if parts[0] != which:
        continue
    key = (parts[1], parts[2])
    rows[key].append((float(d['host_before']['loadavg'].split()[0]), float(d['host_after']['loadavg'].split()[0]),
                      len(d['host_before']['competing']) + len(d['host_after']['competing'])))
print('| ' + ('durability' if which == 'bq' else 'commit') + ' | cell | 1-min load before the run (r1 / r2 / r3) | 1-min load after the run (r1 / r2 / r3) | competing processes listed by the run\'s two host stamps (r1 / r2 / r3) |')
print('|---|---|---|---|---|')
order = {c: i for i, c in enumerate(CELLS)}
for k in sorted(rows, key=lambda k: (k[0], order.get(k[1], 99), k[1])):
    v = rows[k]
    print(f'| {k[0]} | {k[1]} | ' + ' / '.join(f'{b:.2f}' for b, a, c in v) + ' | ' + ' / '.join(f'{a:.2f}' for b, a, c in v) + ' | ' + ' / '.join(str(c) for b, a, c in v) + ' |')
