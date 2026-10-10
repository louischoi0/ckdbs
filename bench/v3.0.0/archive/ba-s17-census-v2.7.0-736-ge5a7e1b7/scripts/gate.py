import json, glob, collections, statistics as st


def load(d):
    o = collections.defaultdict(list)
    for f in sorted(glob.glob(d + '/*.json')):
        r = json.load(open(f))
        o[(r['shape'], r['durability'], r['cores'], r['sessions'])].append(r)
    return o


B = load('/home/cdkbs/bench-runs/ba-s17/kds')


def sh(r, kinds):
    span = r['span_s'] * 1e6 * max(1, r['cores'])
    return sum(r['meta_delta'].get('contention_%s_wait_us' % k, 0) for k in kinds) / span


print("sync gate >= 5% cells:")
for k, v in sorted(B.items()):
    m = st.median(sh(r, ['wal_sync_gate']) for r in v)
    if m >= 0.05:
        print(k, round(m * 100, 2))
print("ceiling by dur/cores (max median share):")
g = collections.defaultdict(list)
for k, v in B.items():
    g[(k[1], k[2])].append((st.median(sh(r, ['ceiling']) for r in v), k))
for kk, v in sorted(g.items()):
    print(kk, "max %.1f%%" % (max(v)[0] * 100), max(v)[1], " cells>=5%%: %d of %d" % (sum(1 for x in v if x[0] >= 0.05), len(v)))
r = B[('insert1', 'strict', 2, 16)][0]
print({k: v for k, v in r['meta_delta'].items() if 'ceiling' in k})
print("sync gate share, group cells k2, by sessions:")
for s in (1, 2, 4, 8, 16):
    print(s, [round(st.median(sh(r, ['wal_sync_gate']) for r in B[(sh_, 'group', 2, s)]) * 100, 2) for sh_ in ('trade', 'insert1', 'insertN', 'recent')])
print("sync gate share, group cells k4, by sessions:")
for s in (1, 2, 4, 8, 16):
    print(s, [round(st.median(sh(r, ['wal_sync_gate']) for r in B[(sh_, 'group', 4, s)]) * 100, 2) for sh_ in ('trade', 'insert1', 'insertN', 'recent')])
