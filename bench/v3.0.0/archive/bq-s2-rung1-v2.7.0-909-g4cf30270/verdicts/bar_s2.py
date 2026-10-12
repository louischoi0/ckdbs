"""BQ-S2 section 5.3 bar per arm at relaxed, from raw bl_mixed JSON + server logs.
usage: python3 -I bar_s2.py /home/cdkbs/bench-runs/bq/s2/out
Bulk cores = cores named by in-window (m0..m1) roll lines; non-bulk (NB)
clients = connect core not a bulk core.  NBs = connect AND end core not bulk.
Deltas m1 - m0, absent = 0."""
import glob, json, math, os, re, statistics as st, sys
from fractions import Fraction

OUT = sys.argv[1]
ROLL = re.compile(r'\[wal\] roll (.*)$')
ARMS = ('base', 'opt3', 's16', 's256', 's1024')
SEG = {'base': 64, 'opt3': 64, 's16': 16, 's256': 256, 's1024': 1024}


def kv(t):
    o = {}
    for tok in t.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                o[k] = int(v)
            except ValueError:
                o[k] = v
    return o


def rolls(log):
    return [kv(m.group(1)) for m in map(ROLL.search, open(log)) if m]


def load(d):
    js = glob.glob(d + '/*.json')
    if not js:
        return None
    j = json.load(open(js[0]))
    j['_log'] = js[0][:-5] + '.s.log'
    return j


def pooled(d, role, keep):
    cs = [c for c in d['clients'] if c['role'] == role and keep(c)]
    n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
    slow = sorted(s[1] / 1000 for c in cs for s in c['slow'])
    dropped = sum(c['slow_dropped'] for c in cs)
    res = {'n': n, 'nclients': len(cs)}
    for q in (99, 99.9):
        k = max(1, math.ceil(Fraction(str(q)) * n / 100))
        fast = n - len(slow)
        if dropped or n == 0:
            res[q] = None
        elif k > fast:
            res[q] = slow[k - fast - 1]
        else:
            res[q] = 0.0  # below the 1 ms floor
    return res


def delta(j, k):
    return (j['meta_m1'].get(k, 0) or 0) - (j['meta_m0'].get(k, 0) or 0)


def run_row(j, role):
    m0, m1 = j['m0_mono_ns'], j['m1_mono_ns']
    rl = rolls(j['_log'])
    win = [r for r in rl if m0 <= r['start_ns'] <= m1]
    bulk = sorted({r['core'] for r in win})
    ins = [c for c in j['clients'] if c['role'] == 'inserter']
    agg = j['read_from_schedule'] if role == 'reader' else j['writer_from_schedule']
    allc = pooled(j, role, lambda c: True)
    nb = pooled(j, role, lambda c: c['core'] not in bulk)
    nbs = pooled(j, role, lambda c: c['core'] not in bulk and c['core_end'] not in bulk)
    lsn = delta(j, 'wal_append_lsn')
    dt = (m1 - m0) / 1e9
    holds = [r['hold_us'] for r in win]
    return dict(agg99=agg.get('p99_us'), agg999=agg.get('p99.9_us'), aggmax=agg.get('max_us'),
                all99=allc[99], all999=allc[99.9], nb99=nb[99], nb999=nb[99.9], nbn=nb['nclients'],
                nbs99=nbs[99], nbs999=nbs[99.9], nbsn=nbs['nclients'],
                bulk=bulk, ins_core=[(c['core'], c['core_end']) for c in ins],
                rolls=len(win), rolls_ctr=delta(j, 'wal_rolls'), holds=holds,
                hold_sum_s=sum(holds) / 1e6, hold_max=max(holds) if holds else 0,
                rows_s=j.get('inserter_rows_per_s'), drv_lbps=j.get('log_bytes_per_s'),
                lsn_mibs=lsn / dt / 2**20 if lsn else None, errors=j.get('errors'),
                refusals=j.get('refusals'), mwin=j.get('moves_in_window'),
                load=j['host_before']['loadavg'].split()[0], window_s=dt,
                bodies={b: sum(1 for r in win if r.get('body') == b) for b in {r.get('body') for r in win}},
                miss_hold_share=(sum(r['hold_us'] for r in win if r.get('body') != 'hit') / sum(holds)) if holds else 0)


def nr(xs, p):
    xs = sorted(xs)
    return xs[max(1, math.ceil(Fraction(str(p)) * len(xs) / 100)) - 1] if xs else None


def f(x):
    if x is None:
        return 'None'
    if x == 0.0:
        return '<1ms'
    return f'{x / 1000:.1f}ms' if x >= 1000 else f'{x:.0f}us'


ISO = {}
for cell in ('c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso'):
    j = load(f'{OUT}/relaxed/{cell}/base/r1')
    key = 'read_from_schedule' if cell.startswith('c1') else 'writer_from_schedule'
    ISO[cell] = (j[key]['p99_us'], j[key]['p99.9_us'], j[key]['max_us'], j['errors'], j['moves_in_window'])
    print(f'ISO {cell:24} p99={f(ISO[cell][0])} p99.9={f(ISO[cell][1])} max={f(ISO[cell][2])} '
          f'errors={j["errors"]} moves_in_window={j["moves_in_window"]} load={j["host_before"]["loadavg"].split()[0]}')

# S1's isolated arms at the same base binary, for the noise floor
S1 = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed'
S1ISO = {}
for cell in ISO:
    vals = []
    for d in sorted(glob.glob(f'{S1}/{cell}/r*')):
        j = load(d)
        if j:
            key = 'read_from_schedule' if cell.startswith('c1') else 'writer_from_schedule'
            vals.append((j[key]['p99_us'], j[key]['p99.9_us']))
    S1ISO[cell] = vals
    print(f'S1 ISO {cell:21} p99={[f(v[0]) for v in vals]} p99.9={[f(v[1]) for v in vals]}')

summary = {}
print()
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2'):
    role = 'reader' if cell.startswith('c1') else 'writer'
    iso = ISO['c2-iso' if cell == 'c2' else cell.replace('c1-', 'c1-iso-')]
    s1iso = S1ISO['c2-iso' if cell == 'c2' else cell.replace('c1-', 'c1-iso-')]
    iso99_max = max([iso[0]] + [v[0] for v in s1iso])
    iso999_max = max([iso[1]] + [v[1] for v in s1iso])
    print(f'== {cell} (iso p99 {f(iso[0])}, p99.9 {f(iso[1])}; most favourable iso over S2+S1 runs: '
          f'{f(iso99_max)} / {f(iso999_max)})')
    for arm in ARMS:
        R = []
        for d in sorted(glob.glob(f'{OUT}/relaxed/{cell}/{arm}/r*')):
            j = load(d)
            if j:
                R.append(run_row(j, role))
        if not R:
            continue
        for key, ik, lim, imax in (('agg99', 0, 2, iso99_max), ('agg999', 1, 5, iso999_max),
                                   ('nb99', 0, 2, iso99_max), ('nb999', 1, 5, iso999_max),
                                   ('nbs99', 0, 2, iso99_max), ('nbs999', 1, 5, iso999_max)):
            vals = [r[key] for r in R]
            ratios = [(v / iso[ik]) if v is not None else None for v in vals]
            best = min(v for v in vals if v is not None) / imax if any(v is not None for v in vals) else None
            okk = [x is not None and x <= lim for x in ratios]
            summary.setdefault((cell, arm), {})[key] = (vals, ratios, best)
            print(f'  {arm:6} {key:7} vals={[f(v) for v in vals]} ratio/iso={[None if x is None else round(x, 1) for x in ratios]} '
                  f'limit {lim}x met={okk} best-pairing ratio={None if best is None else round(best, 1)}')
        print(f'  {arm:6} bulk cores={[r["bulk"] for r in R]} inserter(connect,end)={[r["ins_core"] for r in R]} '
              f'NB clients={[r["nbn"] for r in R]} NBs clients={[r["nbsn"] for r in R]} moves_in_window={[r["mwin"] for r in R]}')
        print(f'  {arm:6} rolls(log/ctr)={[(r["rolls"], r["rolls_ctr"]) for r in R]} hold max={[f(r["hold_max"]) for r in R]} '
              f'held s/window={[round(r["hold_sum_s"], 2) for r in R]} agg max={[f(r["aggmax"]) for r in R]} '
              f'errors={[r["errors"] for r in R]} load={[r["load"] for r in R]}')
        for r in R:
            for a, b in (('agg99', 'all99'), ('agg999', 'all999')):
                if r[b] and abs(r[a] - r[b]) > 0.2:
                    print(f'   !! pooled mismatch {cell} {arm} {a} {r[a]} {r[b]}')
    print()

print('== c3 (term iii)')
C3 = {}
for arm in ARMS:
    R = []
    for d in sorted(glob.glob(f'{OUT}/relaxed/c3/{arm}/r*')):
        j = load(d)
        if j:
            R.append(run_row(j, 'writer'))
    C3[arm] = R
    rs = [r['rows_s'] for r in R]
    lb = [r['drv_lbps'] / 2**20 for r in R]
    ls = [r['lsn_mibs'] for r in R]
    H = [h for r in R for h in r['holds']]
    print(f'  {arm:6} rows/s={[round(x) for x in rs]} mean={st.mean(rs):.0f} spread={max(rs) - min(rs):.0f} '
          f'driver MiB/s={[round(x, 1) for x in lb]} mean={st.mean(lb):.1f} lsn MiB/s={[round(x, 1) for x in ls]} '
          f'mean={st.mean(ls):.1f} rolls={[r["rolls"] for r in R]} hold p50/p99/max={f(nr(H, 50))}/{f(nr(H, 99))}/{f(max(H) if H else 0)} '
          f'held s={[round(r["hold_sum_s"], 2) for r in R]} bodies={[r["bodies"] for r in R]} '
          f'errors={[r["errors"] for r in R]} load={[r["load"] for r in R]}')
b_rs = st.mean(r['rows_s'] for r in C3['base'])
b_lb = st.mean(r['drv_lbps'] for r in C3['base'])
b_ls = st.mean(r['lsn_mibs'] for r in C3['base'])
for arm in ARMS:
    rs = st.mean(r['rows_s'] for r in C3[arm])
    lb = st.mean(r['drv_lbps'] for r in C3[arm])
    ls = st.mean(r['lsn_mibs'] for r in C3[arm])
    print(f'  {arm:6} vs base: rows/s {100 * (rs / b_rs - 1):+.1f}%  driver MiB/s {100 * (lb / b_lb - 1):+.1f}%  '
          f'lsn MiB/s {100 * (ls / b_ls - 1):+.1f}%  (min arm run / max base run rows/s '
          f'{100 * (min(r["rows_s"] for r in C3[arm]) / max(r["rows_s"] for r in C3["base"]) - 1):+.1f}%, '
          f'max/min {100 * (max(r["rows_s"] for r in C3[arm]) / min(r["rows_s"] for r in C3["base"]) - 1):+.1f}%)')

print('\n== roll holds pooled over C1+C2 per arm')
for arm in ARMS:
    H = []
    n = 0
    for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2'):
        for d in sorted(glob.glob(f'{OUT}/relaxed/{cell}/{arm}/r*')):
            j = load(d)
            r = run_row(j, 'reader' if cell.startswith('c1') else 'writer')
            H += r['holds']
            n += 1
    print(f'  {arm:6} runs={n} rolls={len(H)} per run={len(H) / n:.1f} hold p50={f(nr(H, 50))} p99={f(nr(H, 99))} '
          f'max={f(max(H))} sum per run={sum(H) / n / 1e6:.2f}s')
