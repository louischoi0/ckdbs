import sys
sys.path.insert(0, '/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bp-per-core-residue/93896f60-b552-41c7-9a00-cc438c9ec661/scratchpad')
from lib import *

IDS = ['AB', 'AQ', 'S2S1', 'S4S6BQ', 'MERGE', 'S4', 'S6', 'MERGEALIGNED', 'PREMERGE_FIX', 'CLOSE_FIX']
for side in 'AB':
    ex, ratio = [], []
    for name in IDS:
        out, runs = load_pair(name)
        for key in (('update-range', 'literal'), ('update-range', 'prepared')):
            for x in out[key][side]:
                excess = (x['mean_us'] - x['p50_us']) * x['ops'] / 1e3  # ms over 60 statements
                ex.append(excess)
                ratio.append(excess / (x['p99_us'] / 1e3))
    print(side, 'excess over median, total ms per 60-statement arm: min %.0f med %.0f max %.0f; as multiples of the longest statement: min %.2f med %.2f max %.2f' % (
        min(ex), statistics.median(ex), max(ex), min(ratio), statistics.median(ratio), max(ratio)))
