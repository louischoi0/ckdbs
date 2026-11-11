import json, glob, statistics as st
root = glob.glob('/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close/run/2*')[0]
arms = ['update-hot', 'update-disjoint', 'update-disjoint-again']
dp = []; dm = []; fl = {'base': [], 'close': []}
for rep in (1, 2, 3):
    d = json.load(open(f'{root}/m1swap/m1-rep{rep}.json'))
    for a in arms:
        b = d['arms']['base'][a]; c = d['arms']['close'][a]
        dp.append(c['p50_us'] - b['p50_us']); dm.append(c['mean_us'] - b['mean_us'])
    for l in ('base', 'close'):
        fl[l].append(abs(d['arms'][l]['update-disjoint']['p50_us'] - d['arms'][l]['update-disjoint-again']['p50_us']))
print('p50 deltas', [round(x, 1) for x in dp], 'mean', round(st.mean(dp), 2), 'median', round(st.median(dp), 2), 'sd', round(st.pstdev(dp), 2))
print('mean deltas', [round(x, 1) for x in dm], round(st.mean(dm), 2))
print('floors', fl)
for l in ('base', 'close'):
    ops = el = 0
    for rep in (1, 2, 3):
        d = json.load(open(f'{root}/m1swap/m1-rep{rep}.json'))
        for a in arms:
            x = d['arms'][l][a]; ops += x['ops']; el += x['elapsed_s']
    print(l, 'pooled update QPS', round(ops / el), 'mean us', round(1e6 * el / ops, 1))
for rep in (1, 2, 3):
    d = json.load(open(f'{root}/m1swap/m1-rep{rep}.json'))
    for l in ('base', 'close'):
        ops = sum(d['arms'][l][a]['ops'] for a in arms); el = sum(d['arms'][l][a]['elapsed_s'] for a in arms)
        print(rep, l, round(ops / el), 'QPS; mean', round(1e6 * el / ops, 1))
