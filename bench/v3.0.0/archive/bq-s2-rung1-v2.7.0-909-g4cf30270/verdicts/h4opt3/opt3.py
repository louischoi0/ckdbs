"""BQ-S2 H4 option 3 vs base, relaxed, from raw bl_mixed JSON + server logs.
usage: python3 -I opt3.py"""
import glob, json, math, re, statistics as st
from fractions import Fraction
OUT = '/home/cdkbs/bench-runs/bq/s2/out'
ROLL = re.compile(r'\[wal\] roll (.*)$')


def nr(xs, p):
    xs = sorted(xs)
    if not xs:
        return None
    return xs[max(1, math.ceil(Fraction(str(p)) * len(xs) / 100)) - 1]


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


def load(d):
    js = glob.glob(d + '/*.json')
    assert len(js) == 1, d
    j = json.load(open(js[0]))
    lg = glob.glob(d + '/*.s.log')
    assert len(lg) == 1, d
    j['_rolls'] = [kv(m.group(1)) for m in map(ROLL.search, open(lg[0])) if m]
    return j


def delta(j, k):
    return j['meta_m1'].get(k, 0) - j['meta_m0'].get(k, 0)


def pooled(j, role, keep):
    """nearest-rank p99/p99.9 (us) over clients of role passing keep(c)."""
    cs = [c for c in j['clients'] if c['role'] == role and keep(c)]
    n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
    slow = sorted(s[1] / 1000 for c in cs for s in c['slow'])
    drop = sum(c['slow_dropped'] for c in cs)
    res = {'n': n, 'cores': sorted({c['core'] for c in cs})}
    for q in (99, 99.9):
        k = max(1, math.ceil(Fraction(str(q)) * n / 100))
        fast = n - len(slow)
        res[q] = None if (drop or n == 0) else (slow[k - fast - 1] if k > fast else '<1ms')
    return res


def ms(x):
    """us -> ms string"""
    return 'None' if x is None else (x if isinstance(x, str) else f'{x / 1000:.1f}')


def run(cell, arm, r):
    j = load(f'{OUT}/relaxed/{cell}/{arm}/r{r}')
    lo, hi = j['measure_from_mono_ns'], j['stop_at_mono_ns']
    win = [x for x in j['_rolls'] if x['start_ns'] >= lo and x['end_ns'] <= hi]
    rc = sorted({x['core'] for x in win})
    ins = [c['core'] for c in j['clients'] if c['role'] == 'inserter']
    role = 'reader' if j['readers'] else 'writer'
    nb = pooled(j, role, lambda c: c['core'] not in rc)
    nb_ins = pooled(j, role, lambda c: c['core'] not in ins)
    agg = j['read_from_schedule'] if role == 'reader' else j['writer_from_schedule']
    return dict(j=j, win=win, rc=rc, ins=ins, nb=nb, nb_ins=nb_ins, agg=agg, role=role)


print('# relaxed isolated (base, r1 only)')
ISO = {}
for c in ('c1-iso-segregated-no0', 'c1-iso-batch0', 'c1-iso-spread', 'c2-iso'):
    j = load(f'{OUT}/relaxed/{c}/base/r1')
    a = j['read_from_schedule'] if c.startswith('c1') else j['writer_from_schedule']
    ISO[c] = a
    print(f'  {c:22} p99={a["p99_us"]} p99.9={a["p99.9_us"]} n={a["n"]} errors={j["errors"]} '
          f'moves_in_window={j["moves_in_window"]} load_before={j["host_before"]["loadavg"]}')

STEPS = ['hold_us', 'sync_gate_us', 'sync_us', 'create_us', 'body_wait_us', 'body_us', 'header_us',
         'rename_us', 'dirsync_us', 'seal_us']
pool = {}
table = {}
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3'):
    print(f'\n## {cell}')
    isoc = {'c2': 'c2-iso', 'c3': None}.get(cell, cell.replace('c1-', 'c1-iso-'))
    for arm in ('base', 'opt3'):
        for r in (1, 2, 3):
            R = run(cell, arm, r)
            j = R['j']
            w = R['win']
            H = [x['hold_us'] for x in w]
            bod = {}
            for x in w:
                bod[x['body']] = bod.get(x['body'], 0) + 1
            pool.setdefault((cell, arm), []).extend(w)
            table.setdefault((cell, arm), []).append(R)
            line = (f'  {arm:4} r{r} errs={j["errors"]} ref={j["refusals"]} moves_win={j["moves_in_window"]} '
                    f'ins={R["ins"]}->{[c["core_end"] for c in j["clients"] if c["role"] == "inserter"]} '
                    f'roll_cores={R["rc"]} rolls={len(w)} (meta {delta(j, "wal_rolls"):.0f}) '
                    f'hold p50/p99/max ms={ms(nr(H, 50))}/{ms(nr(H, 99))}/{ms(max(H) if H else None)} '
                    f'sum_hold_s={sum(H) / 1e6:.2f} body={bod} '
                    f'body_wait_ms={sum(x["body_wait_us"] for x in w) / 1e3:.1f} '
                    f'body_inline_ms={sum(x["body_us"] for x in w) / 1e3:.1f} '
                    f'header sum/max ms={sum(x["header_us"] for x in w) / 1e3:.0f}/'
                    f'{max([x["header_us"] for x in w] or [0]) / 1e3:.0f} '
                    f'gate sum ms={sum(x["sync_gate_us"] for x in w) / 1e3:.0f} '
                    f'create sum ms={sum(x["create_us"] for x in w) / 1e3:.0f} '
                    f'load_before={j["host_before"]["loadavg"].split()[:3]}')
            if cell != 'c3':
                nb, ni = R['nb'], R['nb_ins']
                line += (f'\n          agg p99/p99.9 ms={ms(R["agg"]["p99_us"])}/{ms(R["agg"]["p99.9_us"])} '
                         f'NB(roll cores) cores={nb["cores"]} n={nb["n"]} p99/p99.9 ms={ms(nb[99])}/{ms(nb[99.9])} '
                         f'NB(ins connect) cores={ni["cores"]} p99/p99.9 ms={ms(ni[99])}/{ms(ni[99.9])}')
                if isinstance(nb[99], float):
                    i = ISO[isoc]
                    line += (f'\n          NB/iso p99={nb[99] / i["p99_us"]:.0f}x (bar 2x) '
                             f'p99.9={nb[99.9] / i["p99.9_us"]:.0f}x (bar 5x)')
            else:
                dt = (j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9
                line += (f'\n          rows/s={j["inserter_rows_per_s"]:.0f} log MiB/s={j["log_bytes_per_s"] / 2**20:.2f} '
                         f'lsn MiB/s={delta(j, "wal_append_lsn") / dt / 2**20:.2f}')
            print(line)

print('\n## per cell x arm: run lists, median, spread (max-min)')
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2'):
    for key in (99, 99.9):
        vals = {}
        for arm in ('base', 'opt3'):
            v = [R['nb'][key] for R in table[(cell, arm)]]
            vals[arm] = v
        ok = all(isinstance(x, float) for a in vals.values() for x in a)
        if ok:
            b, o = vals['base'], vals['opt3']
            print(f'  {cell:18} NB p{key}: base={[round(x / 1e3) for x in b]} med {st.median(b) / 1e3:.0f} spread {(max(b) - min(b)) / 1e3:.0f} | '
                  f'opt3={[round(x / 1e3) for x in o]} med {st.median(o) / 1e3:.0f} spread {(max(o) - min(o)) / 1e3:.0f} | '
                  f'opt3-base med {(st.median(o) - st.median(b)) / 1e3:+.0f} ms; ranges overlap={min(o) <= max(b) and min(b) <= max(o)}')
        else:
            print(f'  {cell:18} NB p{key}: {vals}')
    for key in ('p99_us', 'p99.9_us'):
        b = [R['agg'][key] for R in table[(cell, 'base')]]
        o = [R['agg'][key] for R in table[(cell, 'opt3')]]
        print(f'  {cell:18} agg {key}: base={[round(x / 1e3) for x in b]} opt3={[round(x / 1e3) for x in o]} '
              f'med {st.median(b) / 1e3:.0f} -> {st.median(o) / 1e3:.0f} ms')
b = [R['j']['inserter_rows_per_s'] for R in table[('c3', 'base')]]
o = [R['j']['inserter_rows_per_s'] for R in table[('c3', 'opt3')]]
print(f'  c3 rows/s base={b} mean {st.mean(b):.0f} spread {max(b) - min(b):.0f} ({100 * (max(b) - min(b)) / st.mean(b):.1f}%) | '
      f'opt3={o} mean {st.mean(o):.0f} spread {max(o) - min(o):.0f} | delta {100 * (st.mean(o) - st.mean(b)) / st.mean(b):+.1f}%')
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2'):
    b = [R['j']['inserter_rows_per_s'] for R in table[(cell, 'base')]]
    o = [R['j']['inserter_rows_per_s'] for R in table[(cell, 'opt3')]]
    print(f'  {cell:18} inserter rows/s base={[round(x) for x in b]} opt3={[round(x) for x in o]}')

print('\n## pooled roll hold distribution and step shares per cell x arm (in-window rolls)')
for (cell, arm), w in pool.items():
    H = [x['hold_us'] for x in w]
    if not H:
        continue
    tot = sum(H)
    bod = {}
    for x in w:
        bod[x['body']] = bod.get(x['body'], 0) + 1
    miss = sum(x['hold_us'] for x in w if x['body'] != 'hit')
    hd = [x['header_us'] for x in w]
    cr = [x['create_us'] for x in w]
    wt = [x['waited_sync_tail_us'] for x in w]
    print(f'  {cell:18} {arm:4} n={len(H)} hold ms p25/p50/p90/p99/max={ms(nr(H, 25))}/{ms(nr(H, 50))}/{ms(nr(H, 90))}/'
          f'{ms(nr(H, 99))}/{ms(max(H))} mean={st.mean(H) / 1e3:.1f} body={bod} missed-hold share={100 * miss / tot:.1f}%\n      '
          + ' '.join(f'{s[:-3]}={100 * sum(x[s] for x in w) / tot:.1f}%' for s in STEPS[1:])
          + f'\n      header ms p50/p99/max={ms(nr(hd, 50))}/{ms(nr(hd, 99))}/{ms(max(hd))}'
          f' create(d-g) ms p50/p99/max={ms(nr(cr, 50))}/{ms(nr(cr, 99))}/{ms(max(cr))}'
          f' waited_sync_tail ms p50/p99={ms(nr(wt, 50))}/{ms(nr(wt, 99))}')
