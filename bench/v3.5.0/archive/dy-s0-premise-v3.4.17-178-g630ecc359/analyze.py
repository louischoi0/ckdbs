#!/usr/bin/env python3
"""DY-S0 analysis: per-roll lines + driver JSON -> markdown tables. usage: analyze.py <out_root> [<archive_script_copy_dest>]"""
import glob, json, math, os, re, sys

root = sys.argv[1]
KV = re.compile(r'(\w+)=([\w.]+)')


def pct(xs, p):
    if not xs:
        return float('nan')
    s = sorted(xs)
    k = max(1, math.ceil(p / 100 * len(s)))
    return s[min(k, len(s)) - 1] if p > 0 else s[0]


def dist(xs, ps=(0, 25, 50, 75, 90, 99, 100)):
    return [pct(xs, p) for p in ps]


def fmt(v, unit=1000.0):
    return f'{v / unit:.2f}'


def load(d):
    j = json.load(open(glob.glob(d + '/*.json')[0]))
    j = j if isinstance(j, dict) else j[0]
    rolls = []
    for l in open(glob.glob(d + '/*.s.log')[0]):
        if '[wal] roll' not in l:
            continue
        r = {k: (int(v) if v.isdigit() else v) for k, v in KV.findall(l)}
        rolls.append(r)
    m0, m1 = j['m0_mono_ns'], j['m1_mono_ns']
    win = [r for r in rolls if m0 <= r['start_ns'] <= m1]
    return j, rolls, win


runs = {}
for d in sorted(glob.glob(root + '/*-r[0-9]')):
    name = os.path.basename(d)
    if not glob.glob(d + '/*.json'):
        print('NO JSON', name, file=sys.stderr)
        continue
    runs[name] = load(d)

out = []
P = out.append
H = '| p0 | p25 | p50 | p75 | p90 | p99 | max |'

for dur in ('relaxed', 'group', 'strict'):
    mixed = {n: v for n, v in runs.items() if n.startswith(dur + '-mixed')}
    iso = {n: v for n, v in runs.items() if n.startswith(dur + '-iso')}
    if not mixed:
        continue
    P(f'\n## {dur}\n')
    # per-run summary
    P(f'### {dur}: runs\n')
    P('| run | rolls in window | window s | interval between rolls p50 (ms) | log MiB/s | rows/s | wal_first_segment | segments removed | sync_segs max | reader p99 from-send (us) | reader p99 from-schedule (us) | slow(>1ms) samples |')
    P('|---|---|---|---|---|---|---|---|---|---|---|---|')
    allwin = []
    for n, (j, rolls, win) in mixed.items():
        ts = [r['start_ns'] for r in win]
        iv = [(b - a) / 1e6 for a, b in zip(ts, ts[1:])]
        md = j['meta_delta']
        w = (j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9
        sl = sum(len(c.get('slow', [])) for c in j['clients'] if c['role'] == 'reader')
        P(f"| {n} | {len(win)} (counter {md.get('wal_rolls')}) | {w:.1f} | {pct(iv, 50):.0f} | {j['log_bytes_per_s'] / 2**20:.1f} | {j['inserter_rows_per_s']:.0f} | "
          f"{j['meta_m1'].get('wal_first_segment')} | {md.get('wal_segments_removed')} | {max([r['sync_segs'] for r in win] or [0])} | "
          f"{j['read_service']['p99_us']:.0f} | {j['read_from_schedule']['p99_us']:.0f} | {sl} |")
        allwin += [(n, r, win) for r in win]
    rolls = [r for _, r, _ in allwin]
    N = len(rolls)
    # hold dist
    P(f'\n### {dur}: roll hold (ms), pooled over {len(mixed)} run(s), {N} rolls\n')
    P('| series ' + H)
    P('|---|---|---|---|---|---|---|---|')
    def row(label, xs):
        P(f'| {label} | ' + ' | '.join(fmt(v) for v in dist(xs)) + ' |')
    row('hold', [r['hold_us'] for r in rolls])
    for n, (j, rl, win) in mixed.items():
        row(f'hold, {n} (n={len(win)})', [r['hold_us'] for r in win])
    # decomposition
    tot = sum(r['hold_us'] for r in rolls) or 1
    steps = [('seal', 'seal_us'), ('flush', 'flush_us'), ('sync_gate (wait on writer-thread sync)', 'sync_gate_us'),
             ('sync (roll\'s own, total)', 'sync_us'), ('  sync tail', 'sync_tail_us'), ('  sync rest (non-tail segs)', 'sync_rest_us'),
             ('create (total)', 'create_us'), ('  body_wait', 'body_wait_us'), ('  body inline', 'body_us'),
             ('  header write+sync', 'header_us'), ('  rename', 'rename_us'), ('  dirsync', 'dirsync_us')]
    P(f'\n### {dur}: the hold per step (ms; share = sum of step / sum of hold)\n')
    P('| step | share of hold ' + H)
    P('|---|---|---|---|---|---|---|---|---|')
    for lab, k in steps:
        xs = [r[k] for r in rolls]
        P(f'| {lab} | {100 * sum(xs) / tot:.1f} % | ' + ' | '.join(fmt(v) for v in dist(xs)) + ' |')
    resid = [r['hold_us'] - r['seal_us'] - r['flush_us'] - r['sync_gate_us'] - r['sync_us'] - r['create_us'] for r in rolls]
    P(f'| unaccounted (hold - listed) | {100 * sum(resid) / tot:.1f} % | ' + ' | '.join(fmt(v) for v in dist(resid)) + ' |')
    bc = {k: sum(1 for r in rolls if r['body'] == k) for k in ('hit', 'waited', 'inline')}
    P(f'\nBody outcome: hit {bc["hit"]}, waited {bc["waited"]}, inline {bc["inline"]} (of {N}).')
    sg = [r['sync_segs'] for r in rolls]
    P(f'Segments covered by the roll\'s own sync (sync_segs): p0/p50/p99/max = {pct(sg,0)}/{pct(sg,50)}/{pct(sg,99)}/{pct(sg,100)}; waited_syncs>0 in {sum(1 for r in rolls if r["waited_syncs"])} of {N} rolls.')
    # R1 R2
    R1 = [r['seal_us'] + r['flush_us'] + r['body_wait_us'] + r['body_us'] for r in rolls]
    R2 = [r['seal_us'] + r['flush_us'] for r in rolls]
    P(f'\n### {dur}: the residual under the latch if sync, header, rename, dirsync leave it (us; bar 10,000 us)\n')
    P('| residual | n | p50 | p90 | p99 | max | rolls > 10 ms |')
    P('|---|---|---|---|---|---|---|')
    for lab, xs in (('R1 = seal + flush + body_wait + body', R1), ('R2 = seal + flush', R2)):
        P(f'| {lab} | {N} | {pct(xs,50)} | {pct(xs,90)} | {pct(xs,99)} | {pct(xs,100)} | {sum(1 for x in xs if x > 10000)} |')
    # keep-up
    P(f'\n### {dur}: could one thread keep up with the syncs if they left the latch\n')
    kp_n = kp_late = 0
    wsz = []
    late_pairs = []
    for n, (j, rl, win) in mixed.items():
        for a, b in zip(win, win[1:]):
            iv = (b['start_ns'] - a['start_ns']) / 1000
            s = a['sync_gate_us'] + a['sync_us']
            ws = a['waited_sync_tail_us'] + a['waited_sync_rest_us']
            kp_n += 1
            wsz.append((s, ws, iv))
            if s > iv:
                kp_late += 1
    if kp_n:
        P('| series | p50 | p90 | p99 | max |')
        P('|---|---|---|---|---|')
        for lab, idx in (('gate + own sync (us)', 0), ('waited writer-thread sync, tail+rest (us)', 1), ('interval to next roll (us)', 2)):
            xs = [w[idx] for w in wsz]
            P(f'| {lab} | {pct(xs,50)} | {pct(xs,90)} | {pct(xs,99)} | {pct(xs,100)} |')
        late2 = sum(1 for w in wsz if w[1] > w[2])
        nom = [64 * 2**20 / j['log_bytes_per_s'] * 1e6 for j, _, _ in mixed.values()]
        nomv = sum(nom) / len(nom)
        lateN = sum(1 for w in wsz if w[0] > nomv)
        lateN2 = sum(1 for w in wsz if w[1] > nomv)
        P(f'\nNominal interval (one 64 MiB segment at the run\'s mean log byte rate) = {nomv / 1000:.0f} ms; this is the stalled-free-rate-free reading, because the measured intervals above are stretched by the holds themselves. (gate + own sync) exceeds it in **{lateN}** of {kp_n} rolls; the waited writer-thread sync exceeds it in **{lateN2}**.')
        P(f'\nConsecutive roll pairs: {kp_n}. (gate + own sync) of roll k exceeds the interval to roll k+1 in **{kp_late}** pairs; the waited writer-thread sync of roll k exceeds that interval in **{late2}**.')
    # reader
    P(f'\n### {dur}: the point readers (us)\n')
    P('| arm | metric | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |')
    P('|---|---|---|---|---|---|---|---|---|---|')
    for lab, arms in (('with bulk writer', mixed), ('alone (isolated)', iso)):
        for n, (j, _, _) in arms.items():
            for m in ('read_service', 'read_from_schedule'):
                d = j[m]
                P(f"| {lab}, {n} | {m} | {d['n']} | {d['p0_us']:.0f} | {d['p25_us']:.0f} | {d['p50_us']:.0f} | {d['p95_us']:.0f} | {d['p99_us']:.0f} | {d['p99.9_us']:.0f} | {d['max_us']:.0f} |")
    for n, (j, rl, win) in mixed.items():
        for m in ('read_service', 'read_from_schedule'):
            for ni, (jj, _, _) in iso.items():
                a, b = j[m]['p99_us'], jj[m]['p99_us']
                P(f'\np99 ratio {m}, {n} / {ni} = {a / b:.1f}x ({a:.0f} / {b:.0f} us).')
    # slow-sample alignment
    P(f'\n### {dur}: slow reader samples (> {mixed[next(iter(mixed))][0]["slow_threshold_us"]:.0f} us from schedule) against roll windows\n')
    P('| run | slow samples | overlapping a roll [start,end] | overlapping a roll + 50 ms | > 10 ms and overlapping | > 10 ms total | slow outside any roll window (us, max) |')
    P('|---|---|---|---|---|---|---|')
    for n, (j, rl, win) in mixed.items():
        slow = [tuple(s) for c in j['clients'] if c['role'] == 'reader' for s in c.get('slow', [])]
        wins = [(r['start_ns'], r['end_ns']) for r in rl]
        def ov(s, pad):
            a, l = s
            b = a + l  # the driver's slow sample is [stamp_ns, latency_ns]
            return any(a <= e + pad and b >= st for st, e in wins)
        o0 = [s for s in slow if ov(s, 0)]
        o50 = [s for s in slow if ov(s, 50_000_000)]
        big = [s for s in slow if s[1] > 10_000_000]
        bigo = [s for s in big if ov(s, 0)]
        out_max = max([s[1] / 1000 for s in slow if not ov(s, 0)] or [0])
        P(f'| {n} | {len(slow)} | {len(o0)} | {len(o50)} | {len(bigo)} | {len(big)} | {out_max:.0f} |')
    # counters
    P(f'\n### {dur}: SHOW META wal_roll_* deltas (m1 - m0)\n')
    P('| run | wal_rolls | hold_us | gate_us | sync_us | create_us | body hit/waited/inline | c0 stream-latch wait (us) | c0 roll_blocked_wait_us | wal_syncs |')
    P('|---|---|---|---|---|---|---|---|---|---|')
    for n, (j, rl, win) in mixed.items():
        md = j['meta_delta']
        P(f"| {n} | {md.get('wal_rolls')} | {md.get('wal_roll_hold_us')} | {md.get('wal_roll_sync_gate_us')} | {md.get('wal_roll_sync_us')} | {md.get('wal_roll_create_us')} | "
          f"{md.get('wal_roll_body_hit')}/{md.get('wal_roll_body_waited')}/{md.get('wal_roll_body_inline')} | {md.get('load_c0_waited_wal_stream_us')} | {md.get('load_c0_wal_roll_blocked_wait_us')} | {md.get('wal_syncs')} |")

print('\n'.join(out))
