import glob, json, re, math, sys
out = sys.argv[1]
R = re.compile(r'\[wal\] roll (.*)$')


def kv(t):
    o = {}
    for x in t.split():
        if '=' in x:
            k, v = x.split('=', 1)
            try:
                o[k] = int(v)
            except ValueError:
                o[k] = v
    return o


pool = {}
for js in sorted(glob.glob(out + '/bq/*/*/r*/*.json')):
    p = js.split('/')
    dur, cell, run = p[-4], p[-3], p[-2]
    d = json.load(open(js))
    lo, hi = d['measure_from_mono_ns'], d['stop_at_mono_ns']
    allr = [kv(R.search(l).group(1)) for l in open(js[:-5] + '.s.log') if R.search(l)]
    inr = [r for r in allr if lo <= r['start_ns'] < hi]
    md = d.get('meta_delta', {})
    wr = md.get('wal_rolls')
    m1 = d.get('meta_m1', {}).get('wal_rolls') if isinstance(d.get('meta_m1'), dict) else None
    resid = max([r['hold_us'] - (r['seal_us'] + r['flush_us'] + r['sync_gate_us'] + r['sync_us'] + r['create_us']) for r in allr] or [0])
    print(dur, cell, run, 'lines', len(allr), 'in', len(inr), 'delta wal_rolls', wr, 'm1', m1, 'resid_max_us', resid)
    if cell != 'c3' and inr:
        pool.setdefault(dur, []).extend(inr)
for dur, rs in pool.items():
    hs = sorted(r['hold_us'] for r in rs)
    k = math.ceil(.99 * len(hs)) - 1
    thr = hs[k]
    t = [r for r in rs if r['hold_us'] >= thr]
    T = sum(r['hold_us'] for r in t)
    c = sum(r['sync_gate_us'] + r['sync_us'] for r in t)
    g = sum(r['dirsync_us'] for r in t)
    e = sum(r['header_us'] for r in t)
    dd = sum(r['body_wait_us'] + r['body_us'] for r in t)
    print(dur, len(rs), thr, len(t), 'c%.1f d%.1f e%.1f g%.1f' % (100 * c / T, 100 * dd / T, 100 * e / T, 100 * g / T))
    efg = sorted(r['header_us'] + r['rename_us'] + r['dirsync_us'] for r in rs)
    print(' efg p50', efg[len(efg) // 2], 'max', efg[-1])
allp = pool['relaxed'] + pool['group']
hs = sorted(r['hold_us'] for r in allp)
thr = hs[math.ceil(.99 * len(hs)) - 1]
t = [r for r in allp if r['hold_us'] >= thr]
T = sum(r['hold_us'] for r in t)
print('both-dur pooled thr', thr, 'n', len(t), 'c%.1f' % (100 * sum(r['sync_gate_us'] + r['sync_us'] for r in t) / T))
