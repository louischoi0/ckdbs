"""BQ-S2 H2 verdict: segment size. Recomputed from raw bl_mixed JSON + server
logs under /home/cdkbs/bench-runs/bq/s2/out. Read-only.
usage: python3 -I h2.py"""
import glob, json, math, os, re, statistics as st
from fractions import Fraction

OUT = '/home/cdkbs/bench-runs/bq/s2/out'
ROLL = re.compile(r'\[wal\] roll (.*)$')
ARMS = ('s16', 'base', 'opt3', 's256', 's1024')
SEG = {'s16': 16, 'base': 64, 'opt3': 64, 's256': 256, 's1024': 1024}
# BQ-S1's own isolated arms at 4cf30270 (verdicts/BAR/bar.out), driver agg p99/p99.9 us
S1_ISO = {'c1-segregated-no0': ([279.2, 215.8, 561.2], [2472.6, 803.2, 2532.7]),
          'c1-batch0': ([379.3, 206.2, 353.6], [2270.2, 1007.8, 1554.5]),
          'c1-spread': ([226.5, 216.2, 444.4], [1142.0, 976.3, 2151.0]),
          'c2': ([487.1, 213.1, 471.4], [2920.4, 1843.2, 3092.1])}


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


def nr(xs, p):
    xs = sorted(xs)
    return xs[max(1, math.ceil(Fraction(str(p)) * len(xs) / 100)) - 1] if xs else None


def load(d):
    js = glob.glob(d + '/*.json')
    if not js:
        return None
    j = json.load(open(js[0]))
    j['_rolls'] = [kv(m.group(1)) for m in map(ROLL.search, open(js[0][:-5] + '.s.log')) if m]
    j['_dir'] = d
    return j


def delta(j, k):
    return j['meta_m1'].get(k, 0) - j['meta_m0'].get(k, 0)


def pooled(j, role, keep):
    cs = [c for c in j['clients'] if c['role'] == role and keep(c)]
    n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
    slow = sorted(s[1] / 1000 for c in cs for s in c['slow'])
    res = {}
    for q in (99, 99.9):
        k = max(1, math.ceil(Fraction(str(q)) * n / 100))
        fast = n - len(slow)
        res[q] = None if (n == 0 or any(c['slow_dropped'] for c in cs)) else (slow[k - fast - 1] if k > fast else 0.0)
    return res


def run_row(j, cell):
    role = 'writer' if cell.startswith('c2') else 'reader'
    agg = j['writer_from_schedule'] if role == 'writer' else j['read_from_schedule']
    lo, hi = j['measure_from_mono_ns'], j['stop_at_mono_ns']
    win = [r for r in j['_rolls'] if lo <= r['start_ns'] < hi]
    over = sum(max(0, min(r['end_ns'], hi) - max(r['start_ns'], lo)) for r in j['_rolls']) / 1e9
    bulk = sorted({r['core'] for r in win}) or sorted({r['core'] for r in j['_rolls']})
    nb_conn = pooled(j, role, lambda c: c['core'] not in bulk)
    nb_strict = pooled(j, role, lambda c: c['core'] not in bulk and c['core_end'] not in bulk)
    nbw = sum(delta(j, f'load_c{k}_waited_wal_stream_us') for k in range(4) if k not in bulk) / 1e6
    dt = (j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9
    return dict(run=os.path.basename(j['_dir']), p99=agg.get('p99_us'), p999=agg.get('p99.9_us'),
                mx=agg.get('max_us'), nb99=nb_conn[99], nb999=nb_conn[99.9], nbs99=nb_strict[99],
                nbs999=nb_strict[99.9], bulk=bulk, rolls_win=len(win), rolls_log=len(j['_rolls']),
                stall_s=over, holds=[r['hold_us'] for r in win], nb_wait_s=nbw, rows=j['inserter_rows_per_s'],
                mibs=delta(j, 'wal_append_lsn') / dt / 2**20, errors=j['errors'], refusals=j['refusals'],
                moves=j['moves_in_window'], load=j['host_before']['loadavg'].split()[0],
                ins_p999=j['inserter'].get('p99.9_us'), ins_max=j['inserter'].get('max_us'))


def fmt(x):
    if x is None:
        return 'NA'
    return f'{x / 1000:.1f}' if x >= 1000 else (f'{x:.2f}' if x else '<1')


R = {}
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3'):
    for arm in ARMS:
        R[(cell, arm)] = [run_row(j, cell) for j in map(load, sorted(glob.glob(f'{OUT}/relaxed/{cell}/{arm}/r*'))) if j]
ISO = {}
for cell, ic in (('c1-segregated-no0', 'c1-iso-segregated-no0'), ('c1-batch0', 'c1-iso-batch0'),
                 ('c1-spread', 'c1-iso-spread'), ('c2', 'c2-iso')):
    j = load(f'{OUT}/relaxed/{ic}/base/r1')
    a = j['writer_from_schedule'] if cell == 'c2' else j['read_from_schedule']
    ISO[cell] = (a['p99_us'], a['p99.9_us'], len(j['_rolls']), j['moves_in_window'], j['host_before']['loadavg'])

print('# BQ-S2 H2 (segment size), relaxed. Latencies in ms (from schedule); <1 = below the 1 ms kept floor')
print('\n## Isolated baselines (S2, base, one run each): p99 / p99.9 ms, rolls, moves, loadavg before')
for c, v in ISO.items():
    print(f'  {c:18} {v[0] / 1000:.3f} / {v[1] / 1000:.3f}  rolls={v[2]} moves={v[3]} load={v[4]}')
    print(f'  {"":18} S1 iso p99={S1_ISO[c][0]} p99.9={S1_ISO[c][1]} us')

print('\n## Bar terms (i) p99 <= 2x iso, (ii) p99.9 <= 5x iso; agg = every client of the role, '
      'nb = clients whose connect core is not a roll core, nbS = connect and end both not')
for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2'):
    i99, i999 = ISO[cell][0], ISO[cell][1]
    s1max99, s1max999 = max(S1_ISO[cell][0]), max(S1_ISO[cell][1])
    print(f'\n### {cell}: S2 iso p99={i99 / 1000:.3f} ms (2x={2 * i99 / 1000:.3f}), p99.9={i999 / 1000:.3f} ms '
          f'(5x={5 * i999 / 1000:.3f}); S1 iso max p99={s1max99 / 1000:.3f} p99.9={s1max999 / 1000:.3f}')
    for arm in ARMS:
        rs = R[(cell, arm)]
        p99 = [r['p99'] for r in rs]
        p999 = [r['p999'] for r in rs]
        r99 = [x / i99 for x in p99]
        r999 = [x / i999 for x in p999]
        t1 = 'MET' if st.median(p99) <= 2 * i99 else 'MISSED'
        t1b = 'every run' if max(r99) <= 2 else ('no run' if min(r99) > 2 else 'some run')
        t2 = 'MET' if st.median(p999) <= 5 * i999 else 'MISSED'
        print(f'  {arm:5} agg p99={[fmt(x) for x in p99]} x{[round(x, 1) for x in r99]} (i) {t1} [{t1b} within 2x]'
              f' | p99.9={[fmt(x) for x in p999]} x{[round(x, 1) for x in r999]} (ii) {t2} | max={[fmt(r["mx"]) for r in rs]}')
        print(f'        nb p99={[fmt(r["nb99"]) for r in rs]} nb p99.9={[fmt(r["nb999"]) for r in rs]} '
              f'nbS p99={[fmt(r["nbs99"]) for r in rs]} bulk cores={[r["bulk"] for r in rs]} '
              f'rolls in window={[r["rolls_win"] for r in rs]} moves={[r["moves"] for r in rs]} '
              f'err={[r["errors"] for r in rs]} load={[r["load"] for r in rs]}')

print('\n## Prediction: roll rate, per-roll hold, total stall (in-window rolls; every cell C1+C2+C3 pooled)')
for arm in ARMS:
    rs = [r for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3') for r in R[(cell, arm)]]
    hs = [h for r in rs for h in r['holds']]
    rate = st.mean(r['rolls_win'] for r in rs) / 30
    stall = [r['stall_s'] for r in rs]
    nbw = [r['nb_wait_s'] for r in rs]
    mib = st.mean(r['mibs'] for r in rs)
    print(f'  {arm:5} seg={SEG[arm]:5} MiB runs={len(rs)} rolls/s={rate:.3f} (log {mib:.1f} MiB/s -> '
          f'expect {mib / SEG[arm]:.3f}) n_holds={len(hs)} hold p50={nr(hs, 50) / 1e3:.0f} p90={nr(hs, 90) / 1e3:.0f} '
          f'max={max(hs) / 1e3:.0f} ms mean={st.mean(hs) / 1e3:.0f} | roll-held s per 30 s: '
          f'median={st.median(stall):.2f} range=[{min(stall):.2f},{max(stall):.2f}] | non-bulk cores stream-latch wait s '
          f'median={st.median(nbw):.2f} range=[{min(nbw):.2f},{max(nbw):.2f}]')

print('\n## C3 (term iii): rows/s, log MiB/s against base')
b = R[('c3', 'base')]
bm = st.mean(r['rows'] for r in b)
print(f'  base rows/s={[round(r["rows"]) for r in b]} mean={bm:.0f} spread={max(r["rows"] for r in b) - min(r["rows"] for r in b):.0f}')
for arm in ARMS:
    rs = R[('c3', arm)]
    m = st.mean(r['rows'] for r in rs)
    med = st.median(r['rows'] for r in rs)
    print(f'  {arm:5} rows/s={[round(r["rows"]) for r in rs]} mean={m:.0f} ({100 * (m / bm - 1):+.1f}%) '
          f'median={med:.0f} ({100 * (med / st.median(r["rows"] for r in b) - 1):+.1f}%) '
          f'MiB/s={[round(r["mibs"], 1) for r in rs]} inserter p99.9={[fmt(r["ins_p999"]) for r in rs]} '
          f'max={[fmt(r["ins_max"]) for r in rs]} load={[r["load"] for r in rs]}')

print('\n## The hold by step, per size (every roll in every relaxed non-iso log, whole log)')
STEPS = ('seal_us', 'flush_us', 'sync_gate_us', 'sync_us', 'body_wait_us', 'body_us', 'header_us', 'rename_us',
         'dirsync_us')
for arm in ARMS:
    pool = []
    for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2', 'c3'):
        for d in sorted(glob.glob(f'{OUT}/relaxed/{cell}/{arm}/r*')):
            j = load(d)
            pool += j['_rolls']
    H = [x['hold_us'] for x in pool]
    tot = sum(H)
    other = sum(x['create_us'] - x['body_wait_us'] - x['body_us'] - x['header_us'] - x['rename_us'] - x['dirsync_us']
                for x in pool)
    bodies = {}
    for x in pool:
        bodies[x['body']] = bodies.get(x['body'], 0) + 1
    print(f'\n  {arm} ({SEG[arm]} MiB) rolls={len(pool)} hold p50={nr(H, 50) / 1e3:.0f} p90={nr(H, 90) / 1e3:.0f} '
          f'max={max(H) / 1e3:.0f} ms bodies={bodies} missed-roll hold share='
          f'{100 * sum(x["hold_us"] for x in pool if x["body"] != "hit") / tot:.1f}%')
    print('    median ms : ' + ' '.join(f'{s[:-3]}={st.median(x[s] for x in pool) / 1e3:.1f}' for s in STEPS))
    print('    p90 ms    : ' + ' '.join(f'{s[:-3]}={nr([x[s] for x in pool], 90) / 1e3:.1f}' for s in STEPS))
    print('    share %   : ' + ' '.join(f'{s[:-3]}={100 * sum(x[s] for x in pool) / tot:.1f}' for s in STEPS)
          + f' create_other={100 * other / tot:.1f}')
    print(f'    gate: waited_syncs median={st.median(x["waited_syncs"] for x in pool)} waited tail fdatasync '
          f'median={st.median(x["waited_sync_tail_us"] for x in pool) / 1e3:.0f} ms p90='
          f'{nr([x["waited_sync_tail_us"] for x in pool], 90) / 1e3:.0f} ms; rest median='
          f'{st.median(x["waited_sync_rest_us"] for x in pool) / 1e3:.0f} ms; own tail fdatasync median='
          f'{st.median(x["sync_tail_us"] for x in pool) / 1e3:.1f} ms p90={nr([x["sync_tail_us"] for x in pool], 90) / 1e3:.1f}; '
          f'sync_segs median={st.median(x["sync_segs"] for x in pool)} max={max(x["sync_segs"] for x in pool)}')
    for b_ in sorted(bodies):
        sub = [x for x in pool if x['body'] == b_]
        print(f'    body={b_:7} n={len(sub)} hold median={st.median(x["hold_us"] for x in sub) / 1e3:.0f} ms '
              f'gate median={st.median(x["sync_gate_us"] for x in sub) / 1e3:.0f} body_wait median='
              f'{st.median(x["body_wait_us"] for x in sub) / 1e3:.0f} body_inline median={st.median(x["body_us"] for x in sub) / 1e3:.0f}')

print('\n## Mount probe v2 (clean STOP; restart to first connection)')
for arm in ('s16', 'base', 's256', 's1024'):
    for rep in (1, 2):
        m = json.load(open(f'{OUT}/mount/{arm}-r{rep}.json'))
        rc = m['recovery']
        net = m['mount_s'] - rc['recovery_checkpoint_us'] / 1e6
        print(f'  {arm:5} r{rep} mount_s={m["mount_s"]:.3f} completion-ckpt={rc["recovery_checkpoint_us"] / 1e6:.3f} '
              f'mount minus ckpt={net:.3f} analysis+redo={(rc["recovery_analysis_us"] + rc["recovery_redo_us"]) / 1e6:.3f} '
              f'longest INSERT={m["longest_insert_s"]:.3f} s rolls={m["rolls"]:.0f} log={m["log_bytes"] / 2**20:.0f} MiB')
