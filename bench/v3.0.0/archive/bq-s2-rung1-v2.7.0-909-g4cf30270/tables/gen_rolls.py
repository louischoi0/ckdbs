import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

OUT = []


def P(s=''):
    OUT.append(s)


LOADCELLS = C1 + ['c2', 'c3']


def win_s(j):
    return (j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9


# ---- A. hold per arm x cell
P('**Roll hold per arm and cell, relaxed (pooled over the three runs; rolls with `start_ns` in `[m0, m1]`)**')
P()
P('| cell | arm | rolls r1 / r2 / r3 | pooled n | p0 | p25 | p50 | p95 | p99 | max | summed hold per run (r1 / r2 / r3) | share of the window under a hold (r1 / r2 / r3) |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|')
hold_pool = {}
for cell in LOADCELLS:
    for arm in ARMS:
        js = [load(S2, 'relaxed', cell, arm, r) for r in (1, 2, 3)]
        ws = [inwin(j) for j in js]
        H = [x['hold_us'] for w in ws for x in w]
        hold_pool[(cell, arm)] = H
        sums = [sum(x['hold_us'] for x in w) for w in ws]
        shares = [s / 1e6 / win_s(j) for s, j in zip(sums, js)]
        P(f'| {cell} | {arm} | {" / ".join(str(len(w)) for w in ws)} | {len(H)} | {us(min(H))} | {us(nr(H,25))} | {us(nr(H,50))} | '
          f'{us(nr(H,95))} | {us(nr(H,99))} | {us(max(H))} | {" / ".join(f"{s/1e6:.1f} s" for s in sums)} | '
          f'{" / ".join(f"{100*x:.1f} %" for x in shares)} |')
P()
# S1 base rows
P('**BQ-S1 base rows for the same cells (the baseline; 3 runs, same binary `4cf30270`)**')
P()
P('| cell | arm | rolls r1 / r2 / r3 | pooled n | p0 | p25 | p50 | p95 | p99 | max | summed hold per run | share of window |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|')
for cell in LOADCELLS:
    js = [load(S1, 'relaxed', cell, None, r) for r in (1, 2, 3)]
    ws = [inwin(j) for j in js]
    H = [x['hold_us'] for w in ws for x in w]
    sums = [sum(x['hold_us'] for x in w) for w in ws]
    shares = [s / 1e6 / win_s(j) for s, j in zip(sums, js)]
    P(f'| {cell} | base, BQ-S1 | {" / ".join(str(len(w)) for w in ws)} | {len(H)} | {us(min(H))} | {us(nr(H,25))} | {us(nr(H,50))} | '
      f'{us(nr(H,95))} | {us(nr(H,99))} | {us(max(H))} | {" / ".join(f"{s/1e6:.1f} s" for s in sums)} | '
      f'{" / ".join(f"{100*x:.1f} %" for x in shares)} |')
P()

# ---- B. arm-level pooled hold (all five load cells) and per MiB
P('**Hold by segment size, every relaxed load cell pooled (C1 x 3, C2, C3; 3 runs each)**')
P()
P('| arm | rolls | rolls per 30 s window (mean of 15 runs) | p0 | p25 | p50 | p95 | p99 | max | p50 ÷ segment MiB | summed hold per 30 s window, median [min, max] of 15 runs |')
P('|---|---|---|---|---|---|---|---|---|---|---|')
armH = {}
for arm in ARMS:
    H, per_run, nr_runs = [], [], 0
    cnt = []
    for cell in LOADCELLS:
        for r in (1, 2, 3):
            j = load(S2, 'relaxed', cell, arm, r)
            w = inwin(j)
            H += [x['hold_us'] for x in w]
            per_run.append(sum(x['hold_us'] for x in w) / 1e6 / win_s(j) * 30)
            cnt.append(len(w) / win_s(j) * 30)
    armH[arm] = H
    P(f'| {arm} ({SEG[arm]:,} MiB) | {len(H)} | {st.mean(cnt):.1f} | {us(min(H))} | {us(nr(H,25))} | {us(nr(H,50))} | {us(nr(H,95))} | '
      f'{us(nr(H,99))} | {us(max(H))} | {nr(H,50)/1000/SEG[arm]:.2f} ms/MiB | {med(per_run):.1f} s [{min(per_run):.1f}, {max(per_run):.1f}] |')
P()

# ---- C. step split
P('**The step split of the hold, relaxed, shares of the summed hold (every load cell pooled per arm)**')
P()
STEP = [('seal', 'seal_us'), ('flush', 'flush_us'), ('gate (c1)', 'sync_gate_us'), ('own sync (c2+c3)', 'sync_us'),
        ('body wait (d)', 'body_wait_us'), ('inline body (d)', 'body_us'), ('header (e)', 'header_us'),
        ('rename (f)', 'rename_us'), ('dirsync (g)', 'dirsync_us')]
P('| arm | rolls | summed hold | ' + ' | '.join(n for n, _ in STEP) + ' | residual |')
P('|---|---|---|' + '---|' * (len(STEP) + 1))
steps_med = {}
for arm in ['base (S1)'] + ARMS:
    W = []
    for cell in LOADCELLS:
        for r in (1, 2, 3):
            j = load(S1, 'relaxed', cell, None, r) if arm == 'base (S1)' else load(S2, 'relaxed', cell, arm, r)
            W += inwin(j)
    tot = sum(x['hold_us'] for x in W)
    sh = [sum(x[k] for x in W) / tot for _, k in STEP]
    resid = 1 - sum(sh)
    steps_med[arm] = {n: nr([x[k] for x in W], 50) for n, k in STEP}
    P(f'| {arm} | {len(W)} | {tot/1e6:.0f} s | ' + ' | '.join(f'{100*s:.1f} %' for s in sh) + f' | {100*resid:.1f} % |')
P()
P('Medians of each step per roll (the typical roll, not the summed share):')
P()
P('| arm | hold | seal | gate | own sync | body wait | header | rename | dirsync |')
P('|---|---|---|---|---|---|---|---|---|')
for arm in ['base (S1)'] + ARMS:
    W = []
    for cell in LOADCELLS:
        for r in (1, 2, 3):
            j = load(S1, 'relaxed', cell, None, r) if arm == 'base (S1)' else load(S2, 'relaxed', cell, arm, r)
            W += inwin(j)
    g = lambda k, p=50: us(nr([x[k] for x in W], p))
    P(f'| {arm} | {g("hold_us")} | {g("seal_us")} | {g("sync_gate_us")} | {g("sync_us")} | {g("body_wait_us")} | {g("header_us")} | {g("rename_us")} | {g("dirsync_us")} |')
P()
# p99 versions: header, dirsync
P('Tail of the create steps (p99 per roll) and of the gate:')
P()
P('| arm | gate p50 | gate p95 | gate p99 | header p50 | header p99 | header max | dirsync p50 | dirsync p99 | waited writer sync, summed per roll, p50 | p95 | p99 | per waited sync p50 |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for arm in ['base (S1)'] + ARMS:
    W = []
    for cell in LOADCELLS:
        for r in (1, 2, 3):
            j = load(S1, 'relaxed', cell, None, r) if arm == 'base (S1)' else load(S2, 'relaxed', cell, arm, r)
            W += inwin(j)
    g = lambda k, p=50: us(nr([x[k] for x in W], p))
    WS = [x for x in W if x.get('waited_syncs', 0) > 0]
    per = [x['waited_sync_tail_us'] / x['waited_syncs'] for x in WS]
    P(f'| {arm} | {g("sync_gate_us")} | {g("sync_gate_us",95)} | {g("sync_gate_us",99)} | {g("header_us")} | {g("header_us",99)} | '
      f'{us(max(x["header_us"] for x in W))} | {g("dirsync_us")} | {g("dirsync_us",99)} | '
      f'{us(nr([x["waited_sync_tail_us"] for x in WS],50))} | {us(nr([x["waited_sync_tail_us"] for x in WS],95))} | '
      f'{us(nr([x["waited_sync_tail_us"] for x in WS],99))} | {us(nr(per,50))} |')
P()

# ---- D. body outcomes
P('**Step d, the body: outcomes per arm (every relaxed load cell)**')
P()
P('| arm | rolls | hit | waited | inline | missed rolls: summed hold ÷ all hold | body wait + inline build ÷ all hold | C3 only: missed ÷ all hold (r1 / r2 / r3) |')
P('|---|---|---|---|---|---|---|---|')
for arm in ['base (S1)'] + ARMS:
    W = []
    c3 = []
    for cell in LOADCELLS:
        for r in (1, 2, 3):
            j = load(S1, 'relaxed', cell, None, r) if arm == 'base (S1)' else load(S2, 'relaxed', cell, arm, r)
            w = inwin(j)
            W += w
            if cell == 'c3':
                tot = sum(x['hold_us'] for x in w)
                c3.append(sum(x['hold_us'] for x in w if x['body'] != 'hit') / tot)
    tot = sum(x['hold_us'] for x in W)
    cnt = {k: sum(1 for x in W if x['body'] == k) for k in ('hit', 'waited', 'inline')}
    miss = sum(x['hold_us'] for x in W if x['body'] != 'hit') / tot
    bw = sum(x['body_wait_us'] + x['body_us'] for x in W) / tot
    P(f'| {arm} | {len(W)} | {cnt["hit"]} | {cnt["waited"]} | {cnt["inline"]} | {100*miss:.1f} % | {100*bw:.1f} % | {" / ".join(f"{100*x:.1f} %" for x in c3)} |')
P()

# ---- E. opt3 bimodality
P('**opt3 and base, per run: the two modes of the hold (relaxed)**')
P()
P('| cell | arm | run | JSON written (UTC) | rolls | hold p50 | p90 | p99 | max | rolls under 50 ms | first roll over 150 ms, s into the window | hold p50 by thirds of the window (1st / 2nd / 3rd) | checkpoint runs in window | segments removed in window |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for cell in LOADCELLS:
    for arm in ('base', 'opt3'):
        for r in (1, 2, 3):
            j = load(S2, 'relaxed', cell, arm, r)
            w = inwin(j)
            H = [x['hold_us'] for x in w]
            m0 = j['m0_mono_ns']
            slow = [(x['start_ns'] - m0) / 1e9 for x in w if x['hold_us'] > 150000]
            th = []
            for a in range(3):
                seg = [x['hold_us'] for x in w if a * 10 <= (x['start_ns'] - m0) / 1e9 < (a + 1) * 10]
                th.append(us(nr(seg, 50)) if seg else 'no rolls')
            P(f'| {cell} | {arm} | r{r} | {hhmm(j["_mtime"])} | {len(w)} | {us(nr(H,50))} | {us(nr(H,90))} | {us(nr(H,99))} | {us(max(H))} | '
              f'{sum(1 for h in H if h < 50000)} of {len(H)} | {("%.1f" % min(slow)) if slow else "none"} | {" / ".join(th)} | {delta(j, "checkpoint_runs"):.0f} | {delta(j, "wal_segments_removed"):.0f} |')
P()

# ---- F. segments / recycling
P('**The log at the end of the window: segments removed (recycled) and live, per arm (mean of the 15 relaxed load runs)**')
P()
P('| arm | segments removed in window, per run (min / median / max) | runs with 0 removed | live segments at the last roll of the window (median [max]) |')
P('|---|---|---|---|')
for arm in ['base (S1)'] + ARMS:
    rem, live = [], []
    for cell in LOADCELLS:
        for r in (1, 2, 3):
            j = load(S1, 'relaxed', cell, None, r) if arm == 'base (S1)' else load(S2, 'relaxed', cell, arm, r)
            rem.append(delta(j, 'wal_segments_removed'))
            w = inwin(j)
            if w:
                live.append(w[-1]['sync_segs'])
    P(f'| {arm} | {min(rem):.0f} / {med(rem):.0f} / {max(rem):.0f} | {sum(1 for x in rem if x == 0)} of {len(rem)} | {med(live):.0f} [{max(live)}] |')
P()
print('\n'.join(OUT))
