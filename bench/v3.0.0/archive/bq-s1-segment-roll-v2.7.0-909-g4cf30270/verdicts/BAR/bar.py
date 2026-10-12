"""BQ section 5.3 bar, H2 premise, H6 ring cap and BL ruling 1, recomputed from
raw bl_mixed JSON + server logs. Independent of analyze_s1.py.
usage: python3 -I bar.py <out_dir>"""
import glob, json, math, os, re, statistics as st, sys
from fractions import Fraction

OUT = sys.argv[1]
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


def rolls(log):
    rs = []
    for line in open(log):
        m = ROLL.search(line)
        if m:
            rs.append(kv(m.group(1)))
    return rs


def runs(cell):
    out = []
    for js in sorted(glob.glob(cell + '/r*/*.json')):
        d = json.load(open(js))
        d['_log'] = js[:-5] + '.s.log'
        d['_r'] = js.split('/')[-2]
        out.append(d)
    return out


def pooled(d, role, keep):
    """nearest-rank p99/p99.9 (from schedule) over clients of `role` passing
    keep(client): exact where the rank lands in the >=1 ms kept samples."""
    cs = [c for c in d['clients'] if c['role'] == role and keep(c)]
    n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
    slow = sorted(s[1] / 1000 for c in cs for s in c['slow'])
    dropped = sum(c['slow_dropped'] for c in cs)
    res = {'n': n, 'slow': len(slow), 'cores': sorted({c['core'] for c in cs})}
    for q in (99, 99.9):
        k = max(1, math.ceil(Fraction(str(q)) * n / 100))
        fast = n - len(slow)
        if dropped or n == 0:
            res[q] = None
        elif k > fast:
            res[q] = slow[k - fast - 1]
        else:
            res[q] = '<1000'
    return res


def fmt(x):
    return 'None' if x is None else (x if isinstance(x, str) else f'{x:.1f}')


def ins_core(d):
    return [c['core'] for c in d['clients'] if c['role'] == 'inserter']


def cell_table(dur, cell):
    rs = runs(f'{OUT}/bq/{dur}/{cell}')
    rows = []
    for d in rs:
        role = 'reader' if d['readers'] else 'writer'
        agg = d['read_from_schedule'] if role == 'reader' else d['writer_from_schedule']
        ic = ins_core(d)
        allc = pooled(d, role, lambda c: True)
        nb = pooled(d, role, lambda c: c['core'] not in ic) if ic else allc
        lo, hi = d['measure_from_mono_ns'], d['stop_at_mono_ns']
        rl = rolls(d['_log'])
        win = [r for r in rl if r['start_ns'] >= lo and r['end_ns'] <= hi]
        lsn0, lsn1 = d['meta_m0'].get('wal_append_lsn'), d['meta_m1'].get('wal_append_lsn')
        dt = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
        lsn_rate = (lsn1 - lsn0) / dt if lsn0 is not None else None
        mwin = [r for r in rl if r['start_ns'] >= d['m0_mono_ns'] and r['end_ns'] <= d['m1_mono_ns']]
        held = sum(r['hold_us'] for r in mwin) / 1e6
        rows.append(dict(
            run=d['_r'], role=role, agg99=agg.get('p99_us'), agg999=agg.get('p99.9_us'),
            all99=allc[99], all999=allc[99.9], nb99=nb[99], nb999=nb[99.9], nbcores=nb['cores'],
            ins=ic, ins_end=[c['core_end'] for c in d['clients'] if c['role'] == 'inserter'],
            moves=d['moves'], mwin=d['moves_in_window'], pcv=d['per_core_valid'],
            rows_s=d['inserter_rows_per_s'], drv_lbps=d['log_bytes_per_s'],
            lsn_rate=lsn_rate,
            lsn_rate_unstalled=((lsn1 - lsn0) / (dt - held)) if lsn0 is not None and dt > held else None,
            rolls_win=len(win), rolls_log=len(rl), rolls_m=len(mwin),
            roll_cores=sorted({r['core'] for r in win}),
            holds=[r['hold_us'] for r in win], rl=win, load=d['host_before']['loadavg'].split()[0],
            seconds=d['seconds'], dev=d.get('data_device'),
            drain=sum(d['meta_delta'].get(f'load_c{k}_wal_drain_latch_wait_us', 0) for k in range(4)),
            ck_longest=d['meta_longest'].get('contention_checkpoint_longest_us')))
    return rows


def med(xs):
    xs = [x for x in xs if isinstance(x, (int, float))]
    return st.median(xs) if xs else None


def ratio_block(name, mixed, iso, key, factor):
    m = [r[key] for r in mixed]
    i = [r[key] for r in iso]
    mn = [x for x in m if isinstance(x, (int, float))]
    inn = [x for x in i if isinstance(x, (int, float))]
    s = f'  {name:8} mixed={[fmt(x) for x in m]} iso={[fmt(x) for x in i]}'
    if len(mn) == len(m) and len(inn) == len(i) and mn and inn:
        rm = med(mn) / med(inn)
        best = min(mn) / max(inn)
        worst = max(mn) / min(inn)
        paired = [a / b for a, b in zip(mn, inn)]
        verdict = 'MET' if rm <= factor else 'MISSED'
        if best > factor:
            verdict += ' (even best pairing)'
        elif worst <= factor:
            verdict += ' (every pairing)'
        else:
            verdict += ' (pairing-dependent)'
        s += (f'\n           ratio med/med={rm:.2f}x range=[{best:.2f}x, {worst:.2f}x] '
              f'run-paired={[round(x, 2) for x in paired]} bar {factor}x -> {verdict}'
              f'\n           spread(max-min) mixed={max(mn) - min(mn):.1f}us iso={max(inn) - min(inn):.1f}us')
    else:
        s += '  (a value lies below the 1 ms kept-sample floor)'
    return s


print('# BQ 5.3 bar at 4cf30270 (v2.7.0-909-g4cf30270)')
allrows = {}
for dur in ('relaxed', 'group'):
    for cell in sorted(os.listdir(f'{OUT}/bq/{dur}')):
        allrows[(dur, cell)] = cell_table(dur, cell)

for dur in ('relaxed', 'group'):
    print(f'\n## {dur}')
    pairs = [(f'c1-{p}', f'c1-iso-{p}') for p in ('segregated-no0', 'batch0', 'spread')] + [('c2', 'c2-iso')]
    for mc, ic in pairs:
        M, I = allrows[(dur, mc)], allrows[(dur, ic)]
        print(f'{mc} vs {ic}: inserter connect={[r["ins"] for r in M]} end={[r["ins_end"] for r in M]} '
              f'roll cores={[r["roll_cores"] for r in M]} moves_in_window={[r["mwin"] for r in M]} '
              f'iso moves_in_window={[r["mwin"] for r in I]} NB connect cores={M[0]["nbcores"]} '
              f'load={[r["load"] for r in M]} iso load={[r["load"] for r in I]} '
              f'ckpt longest us={[r["ck_longest"] for r in M]}')
        print(ratio_block('agg p99', M, I, 'agg99', 2))
        print(ratio_block('agg p999', M, I, 'agg999', 5))
        print(ratio_block('NB p99', M, I, 'nb99', 2))
        print(ratio_block('NB p999', M, I, 'nb999', 5))
        for r in M + I:
            for a, b in (('agg99', 'all99'), ('agg999', 'all999')):
                if isinstance(r[b], float) and abs(r[a] - r[b]) > 0.11:
                    print('   !! pooled mismatch', mc, r['run'], a, r[a], r[b])

print('\n## C3 (the 3% term base)')
for dur in ('relaxed', 'group'):
    C = allrows[(dur, 'c3')]
    for r in C:
        print(f'  {dur} {r["run"]}: rows/s={r["rows_s"]:.0f} log MiB/s(lsn m0..m1)={r["lsn_rate"] / 2**20:.2f} '
              f'(driver {r["drv_lbps"] / 2**20:.2f}) excl-roll-hold={r["lsn_rate_unstalled"] / 2**20:.2f} '
              f'rolls in window={r["rolls_win"]} -> {r["rolls_win"] * 64 / r["seconds"]:.2f} MiB/s '
              f'load={r["load"]} dev={r["dev"]}')
    rs = [r['rows_s'] for r in C]
    ls = [r['lsn_rate'] / 2**20 for r in C]
    print(f'  {dur} C3 rows/s mean={st.mean(rs):.0f} spread={max(rs) - min(rs):.0f} '
          f'({100 * (max(rs) - min(rs)) / st.mean(rs):.2f}%) log MiB/s mean={st.mean(ls):.2f} '
          f'spread={max(ls) - min(ls):.2f} ({100 * (max(ls) - min(ls)) / st.mean(ls):.2f}%) '
          f'3% of rows/s={0.03 * st.mean(rs):.0f}')


def corr(x, y):
    if len(x) < 3 or st.pstdev(x) == 0 or st.pstdev(y) == 0:
        return None
    return st.correlation(x, y)


print('\n## H2 premise: rolls per run, what the hold is made of')
for dur in ('relaxed', 'group'):
    pool = []
    for (d_, cell), R in sorted(allrows.items()):
        if d_ != dur or 'iso' in cell:
            continue
        print(f'  {dur} {cell:18} rolls/run in window={[r["rolls_win"] for r in R]} '
              f'in whole log={[r["rolls_log"] for r in R]} sum={sum(r["rolls_win"] for r in R)} '
              f'hold p50/p99/max={nr([h for r in R for h in r["holds"]], 50)}/'
              f'{nr([h for r in R for h in r["holds"]], 99)}/{max(h for r in R for h in r["holds"])}')
        for r in R:
            prev = None
            for x in sorted(r['rl'], key=lambda z: z['start_ns']):
                x['_gap'] = (x['start_ns'] - prev) / 1e6 if prev else None
                prev = x['end_ns']
                pool.append(x)
    H = [x['hold_us'] for x in pool]
    steps = {'gate(c wait)': 'sync_gate_us', 'own sync(c)': 'sync_us', 'create(d-g)': 'create_us',
             'seal(a)': 'seal_us', 'flush(b)': 'flush_us', 'body_wait(d)': 'body_wait_us',
             'body_inline(d)': 'body_us', 'header(e)': 'header_us', 'rename(f)': 'rename_us',
             'dirsync(g)': 'dirsync_us'}
    print(f'  {dur} pooled rolls={len(H)} hold p50={nr(H, 50)} p90={nr(H, 90)} p99={nr(H, 99)} '
          f'max={max(H)} mean={st.mean(H):.0f} us')
    print('   share of summed hold: ' + ', '.join(
        f'{k}={100 * sum(x[v] for x in pool) / sum(H):.1f}%' for k, v in steps.items()))
    bodies = {}
    for x in pool:
        bodies[x['body']] = bodies.get(x['body'], 0) + 1
    print(f'   bodies={bodies} missed-roll hold share='
          f'{100 * sum(x["hold_us"] for x in pool if x["body"] != "hit") / sum(H):.1f}%')
    g = [x for x in pool if x['_gap'] is not None]
    ys = [x['hold_us'] for x in g]
    for lab, f in (('sync_segs (live segments)', lambda x: x['sync_segs']),
                   ('waited_syncs', lambda x: x['waited_syncs']),
                   ('gap since prev roll ms', lambda x: x['_gap']),
                   ('waited_sync_tail_us', lambda x: x['waited_sync_tail_us']),
                   ('waited_sync_rest_us', lambda x: x['waited_sync_rest_us'])):
        xs = [f(x) for x in g]
        c = corr(xs, ys)
        print(f'   corr(hold, {lab}) = {None if c is None else round(c, 3)} (range {min(xs)}..{max(xs)})')
    # hold by live-segment tercile
    srt = sorted(g, key=lambda x: x['sync_segs'])
    t = len(srt) // 3
    for lab, part in (('low', srt[:t]), ('mid', srt[t:2 * t]), ('high', srt[2 * t:])):
        print(f'   live-segs {lab}: segs {part[0]["sync_segs"]}..{part[-1]["sync_segs"]} '
              f'hold median={st.median([x["hold_us"] for x in part]):.0f} us '
              f'own sync_rest median={st.median([x["sync_rest_us"] for x in part]):.0f} us')
    cg = [x['create_us'] for x in pool]
    print(f'   create(d-g) p50={nr(cg, 50)} p99={nr(cg, 99)} max={max(cg)} us; '
          f'sync_segs max={max(x["sync_segs"] for x in pool)}')

print('\n## H6 ring cap: log bytes arriving during one hold vs the ring')
for dur in ('relaxed', 'group'):
    for (d_, cell), R in sorted(allrows.items()):
        if d_ != dur or 'iso' in cell:
            continue
        rates = [r['lsn_rate'] / 2**20 for r in R]
        ur = [r['lsn_rate_unstalled'] / 2**20 for r in R]
        hs = [h for r in R for h in r['holds']]
        print(f'  {dur} {cell:18} rate MiB/s={[round(x, 1) for x in rates]} excl-hold={[round(x, 1) for x in ur]} '
              f'MiB over a p50 hold={st.mean(rates) * nr(hs, 50) / 1e6:.2f} over max hold={st.mean(rates) * max(hs) / 1e6:.2f}')
        for ring, lab in ((1 << 20, '1MiB'), (512 << 10, '512KiB')):
            over = tot = 0
            fills = []
            for r in R:
                for h in r['holds']:
                    tot += 1
                    over += r['lsn_rate'] * h / 1e6 > ring
                fills.append(ring / r['lsn_rate'] * 1e3)
            print(f'      ring {lab}: fill time at achieved rate={[round(x, 1) for x in fills]} ms; '
                  f'rolls whose hold exceeds it: {over}/{tot}')

print('\n## BL ruling 1: 05cd011f vs 3338cf0c (C1 mixed relaxed)')
for pl in ('segregated-no0', 'batch0', 'spread'):
    for sha in ('3338cf0c', '05cd011f'):
        R = runs(f'{OUT}/bl/{sha}/{pl}')
        s99 = [d['read_from_schedule']['p99_us'] for d in R]
        s999 = [d['read_from_schedule']['p99.9_us'] for d in R]
        v99 = [d['read_service']['p99_us'] for d in R]
        v50 = [d['read_service']['p50_us'] for d in R]
        nb = []
        for d in R:
            ic = ins_core(d)
            nb.append(pooled(d, 'reader', lambda c: c['core'] not in ic)[99])
        print(f'  {pl:15} {sha} secs={[d["seconds"] for d in R]} sched p99={s99} '
              f'(med {med(s99):.0f}, spread {max(s99) - min(s99):.0f}) p99.9={s999} '
              f'service p99={v99} (med {med(v99):.0f}, spread {max(v99) - min(v99):.0f}) svc p50={v50} '
              f'NB sched p99={[fmt(x) for x in nb]} moves_in_window={[d["moves_in_window"] for d in R]} '
              f'rows/s={[round(d["inserter_rows_per_s"]) for d in R]} load={[d["host_before"]["loadavg"].split()[0] for d in R]}')
