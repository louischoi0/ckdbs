import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from bp_lib import *
for pa in ('literal', 'prepared'):
    for r in (1, 2, 3):
        d, run, arms = bn(r)
        for b in ('A', 'B'):
            a = arms[('update-range', pa, b)]
            excess = (a['mean_us'] - a['p50_us']) * a['ops'] / 1e3
            print(pa, r, b, 'ops', a['ops'], 'p0', round(a['p0_us']), 'p50', round(a['p50_us']), 'p90', round(a['p90_us']), 'p99(max)', round(a['p99_us']), 'mean', round(a['mean_us']), 'excess over p50 total ms', round(excess), 'server_cpu', round(a['server_cpu_us']), 'blocks', [round(x) for x in a['block_server_us']])
