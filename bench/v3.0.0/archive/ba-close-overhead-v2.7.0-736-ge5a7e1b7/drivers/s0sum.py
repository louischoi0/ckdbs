import json, statistics as st
B = '/home/cdkbs/bench-runs/ba-close/s0/'
for tag in ("s0-c1-g", "s0-c8-g", "s0-c1-s", "s0-c8-s"):
    row = []
    for p in ("", "r2-", "r3-"):
        d = json.load(open(B + 'json/' + p + tag + '.json'))
        m = d['meta']
        ph = {x['phase']: x for x in d['phases']}
        row.append((m['tps'], m['committed'], m['torn'], m['rolled_back'], ph['txn']['errors'], round(ph['txn']['p50_us']), round(ph['txn']['p99_us'])))
    t = [r[0] for r in row]
    print(tag, 'tps', t, 'median', st.median(t), 'spread %.1f%%' % ((max(t) / min(t) - 1) * 100), 'committed', [r[1] for r in row], 'torn', [r[2] for r in row], 'errors', [r[4] for r in row], 'txn p50 us', [r[5] for r in row], 'p99 us', [r[6] for r in row])
