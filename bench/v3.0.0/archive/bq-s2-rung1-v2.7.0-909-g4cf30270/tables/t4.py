import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

tot = 0
for cell in CELLS:
    full = 24007 if cell == 'c2' else 12006
    for arm in ARMS:
        for r in (1, 2, 3):
            j = load(S2, 'relaxed', cell, arm, r)
            n = j[agg_key(j)]['n']
            if n < full:
                tot += 1
                print(cell, arm, r, 'n', n, 'missing', full - n, f'({100*(full-n)/full:.1f} %)')
print('runs short:', tot)
# S1
for cell in CELLS:
    full = 24007 if cell == 'c2' else 12006
    for r in (1, 2, 3):
        j = load(S1, 'relaxed', cell, None, r)
        n = j[agg_key(j)]['n']
        if n < full:
            print('S1', cell, r, 'n', n, 'missing', full - n)
