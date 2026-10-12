import sys
sys.path.insert(0, '/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bp-per-core-residue/93896f60-b552-41c7-9a00-cc438c9ec661/scratchpad')
from lib import *
from pooled import IDS

tail = {}
for name, (ia, ib) in IDS.items():
    out, runs = load_pair(name)
    for key in (('update-range', 'literal'), ('update-range', 'prepared')):
        for side, bid in (('A', ia), ('B', ib)):
            for i, x in enumerate(out[key][side]):
                tail.setdefault(bid, []).append((name, key[1], i + 1, round(x['p99_us'] / 1000, 1), round(x['mean_us'] / 1000, 2), round(x['p50_us'] / 1000, 2)))
for bid, v in tail.items():
    mx = [x[3] for x in v]
    print(bid, 'n arms', len(v), 'longest ms: min %.0f med %.0f max %.0f' % (min(mx), statistics.median(mx), max(mx)), 'arms >300ms:', sum(1 for m in mx if m > 300))
    print('    ', mx)
