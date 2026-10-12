import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from bp_lib import *
bad = 0
n = 0
for r in (1, 2, 3):
    d, run, arms = bn(r)
    for k, a in arms.items():
        n += 1
        bad += a['bad']
    print(r, run['end_state'], run['pages-unspilled'], run['pages-spilled'])
print('arms', n, 'bad total', bad)
