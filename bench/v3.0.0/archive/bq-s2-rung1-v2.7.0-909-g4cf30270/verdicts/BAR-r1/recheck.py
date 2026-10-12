"""Independent recheck of BQ-S2 BAR verdict. usage: python3 -I recheck.py OUT"""
import glob, json, math, os, re, statistics as st, sys

OUT = sys.argv[1]
ARMS = ['base', 'opt3', 's16', 's256', 's1024']
RX = re.compile(r'\[wal\] roll (.*)')


def jl(d):
    g = glob.glob(os.path.join(d, '*.json'))
    if not g:
        return None, None
    return json.load(open(g[0])), g[0][:-len('.json')] + '.s.log'


def rolls(log):
    out = []
    for line in open(log):
        m = RX.search(line)
        if m:
            r = {}
            for t in m.group(1).split():
                k, _, v = t.partition('=')
                r[k] = int(v) if v.lstrip('-').isdigit() else v
            out.append(r)
    return out


def q(lat_sorted_desc_slow, n, p):
    # nearest rank: k = ceil(p/100 * n); value at rank k ascending.
    k = math.ceil(p * n / 100 - 1e-9)
    k = max(k, 1)
    nslow = len(lat_sorted_desc_slow)
    fast = n - nslow
    if k <= fast:
        return 0.0
    asc = lat_sorted_desc_slow[::-1]
    return asc[k - fast - 1]


def pool(j, role, pred):
    cs = [c for c in j['clients'] if c['role'] == role and pred(c)]
    n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
    sl = sorted((s[1] / 1e3 for c in cs for s in c['slow']), reverse=True)
    if n == 0:
        return None, None, 0
    return q(sl, n, 99), q(sl, n, 99.9), len(cs)


def iso(cell):
    j, _ = jl(f'{OUT}/relaxed/{cell}/base/r1')
    k = 'read_from_schedule' if cell.startswith('c1') else 'writer_from_schedule'
    return j[k]['p99_us'], j[k]['p99.9_us'], j['host_before']['loadavg']


ISO = {c: iso(c) for c in ['c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso']}
for c, v in ISO.items():
    print('ISO', c, f'{v[0]:.0f}us {v[1]:.0f}us load={v[2]}')
S1 = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed'
S1ISO = {}
for c in ISO:
    vals = []
    for d in sorted(glob.glob(f'{S1}/{c}/r*')):
        j, _ = jl(d)
        if j:
            k = 'read_from_schedule' if c.startswith('c1') else 'writer_from_schedule'
            vals.append((j[k]['p99_us'], j[k]['p99.9_us']))
    S1ISO[c] = vals
    print('S1ISO', c, [(round(a), round(b)) for a, b in vals])

anymet = []
HOLD = {a: [] for a in ARMS}
HELD = {a: [] for a in ARMS}
NR = {a: [] for a in ARMS}
STEPS = {a: {} for a in ARMS}
TOT = {a: 0 for a in ARMS}
BODYW = {a: [0, 0, 0] for a in ARMS}
mism = []
for cell in ['c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3']:
    role = 'reader' if cell.startswith('c1') else 'writer'
    agk = 'read_from_schedule' if role == 'reader' else 'writer_from_schedule'
    isoc = 'c2-iso' if cell == 'c2' else cell.replace('c1-', 'c1-iso-')
    print('==', cell)
    for arm in ARMS:
        for d in sorted(glob.glob(f'{OUT}/relaxed/{cell}/{arm}/r*')):
            j, log = jl(d)
            if j is None:
                print('  MISSING', d)
                continue
            m0, m1 = j['m0_mono_ns'], j['m1_mono_ns']
            R = [r for r in rolls(log) if m0 <= r['start_ns'] <= m1]
            ctr = (j['meta_m1'].get('wal_rolls', 0) or 0) - (j['meta_m0'].get('wal_rolls', 0) or 0)
            md = j['meta_delta'].get('wal_rolls')
            bulk = {r['core'] for r in R}
            holds = [r['hold_us'] for r in R]
            for r in R:
                TOT[arm] += r['hold_us']
                for k, v in r.items():
                    if k.endswith('_us') and k not in ('hold_us',) and isinstance(v, int) and not k.startswith('waited'):
                        STEPS[arm][k] = STEPS[arm].get(k, 0) + v
                BODYW[arm][0] += 1
                if r.get('body') != 'hit':
                    BODYW[arm][1] += 1
            if cell == 'c3':
                lsn = j['meta_delta'].get('wal_append_lsn')
                print(f'  {arm:6} {os.path.basename(d)} rows/s={j["inserter_rows_per_s"]:.0f} drvMiB/s={j["log_bytes_per_s"]/2**20:.1f} '
                      f'lsnMiB/s={lsn/((m1-m0)/1e9)/2**20:.1f} rolls={len(R)}/{ctr}/{md} errors={j["errors"]} load={j["host_before"]["loadavg"].split()[0]}')
                NR.setdefault(('c3', arm), []).append((j['inserter_rows_per_s'], j['log_bytes_per_s'], lsn / ((m1 - m0) / 1e9)))
                continue
            HOLD[arm] += holds
            HELD[arm].append(sum(holds) / 1e6)
            NR[arm].append(len(R))
            ag = j[agk]
            a99, a999, _ = pool(j, role, lambda c: True)
            if abs(a99 - ag['p99_us']) > 0.5 or abs(a999 - ag['p99.9_us']) > 0.5:
                mism.append((cell, arm, d, a99, ag['p99_us'], a999, ag['p99.9_us']))
            n99, n999, nn = pool(j, role, lambda c: c['core'] not in bulk)
            i99, i999 = ISO[isoc][0], ISO[isoc][1]
            imax99 = max([i99] + [v[0] for v in S1ISO[isoc]])
            imax999 = max([i999] + [v[1] for v in S1ISO[isoc]])
            for lab, p, pp in (('all', ag['p99_us'], ag['p99.9_us']), ('NB', n99, n999)):
                if p is None:
                    continue
                t1 = p <= 2 * i99
                t2 = pp <= 5 * i999
                t1g = p <= 2 * imax99
                t2g = pp <= 5 * imax999
                if t1 or t1g:
                    anymet.append((cell, arm, d[-2:], lab, p, pp, t1, t2, t1g, t2g))
                print(f'  {arm:6} {d[-2:]} {lab:3} n={nn if lab=="NB" else "-"} p99={p/1e3:9.1f}ms ({p/i99:7.1f}x) p99.9={pp/1e3:9.1f}ms ({pp/i999:6.1f}x) '
                      f'best99={p/imax99:6.1f}x best999={pp/imax999:6.1f}x bulk={sorted(bulk)} rolls={len(R)}/{ctr}/{md} held={sum(holds)/1e6:.2f}s '
                      f'max={max(holds)/1e3 if holds else 0:.0f}ms')
print('\nmismatch driver vs pooled:', mism)
print('\nruns meeting term (i) under any pairing:')
for x in anymet:
    print('  ', x)


def nrk(xs, p):
    xs = sorted(xs)
    return xs[max(1, math.ceil(p * len(xs) / 100 - 1e-9)) - 1]


print('\nholds pooled C1+C2:')
for a in ARMS:
    H = HOLD[a]
    print(f'  {a:6} runs={len(NR[a])} rolls/run={st.mean(NR[a]):.1f} p50={nrk(H,50)/1e3:.1f}ms p99={nrk(H,99)/1e3:.1f}ms max={max(H)/1e3:.0f}ms held/run={st.mean(HELD[a]):.2f}s')
print('\nstep shares (C1+C2+C3, all rolls):')
for a in ARMS:
    top = sorted(STEPS[a].items(), key=lambda kv: -kv[1])[:6]
    print(f'  {a:6} rolls={BODYW[a][0]} nonhit={BODYW[a][1]} ', ', '.join(f'{k}={100*v/TOT[a]:.1f}%' for k, v in top))
print('\nC3:')
b = NR[('c3', 'base')]
for a in ARMS:
    x = NR[('c3', a)]
    for i, lab in enumerate(('rows/s', 'drvMiB', 'lsnMiB')):
        mb = st.mean(v[i] for v in b)
        ma = st.mean(v[i] for v in x)
        print(f'  {a:6} {lab} mean={ma:.0f} vs base {100*(ma/mb-1):+.1f}% base spread {100*(max(v[i] for v in b)-min(v[i] for v in b))/mb:.1f}%')
