import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

OUT = []


def P(s=''):
    OUT.append(s)


def mib(j):
    return j['log_bytes_per_s'] / 1048576


# ---- C3 term (iii)
P('**C3, the bulk writer alone (term iii): rows/s and log MiB/s per run; the delta is the arm\'s mean against the base mean of this session**')
P()
P('| arm | rows/s r1 / r2 / r3 | mean | spread (max - min) | Δ mean vs base (this session) | Δ per run vs base mean | log MiB/s r1 / r2 / r3 | mean log rate | Δ vs base mean | syncs/s by the writer r1 / r2 / r3 | log MiB per sync (mean) |')
P('|---|---|---|---|---|---|---|---|---|---|---|')
base_rows = [load(S2, 'relaxed', 'c3', 'base', r)['inserter_rows_per_s'] for r in (1, 2, 3)]
base_mib = [mib(load(S2, 'relaxed', 'c3', 'base', r)) for r in (1, 2, 3)]
bm, bmm = st.mean(base_rows), st.mean(base_mib)
rowsS1 = [load(S1, 'relaxed', 'c3', None, r) for r in (1, 2, 3)]
for arm in ['base (BQ-S1)'] + ARMS:
    js = rowsS1 if arm.startswith('base (BQ') else [load(S2, 'relaxed', 'c3', arm, r) for r in (1, 2, 3)]
    rr = [j['inserter_rows_per_s'] for j in js]
    mm = [mib(j) for j in js]
    syn = [delta(j, 'wal_syncs') / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) for j in js]
    mps = [(delta(j, 'wal_append_lsn') / 1048576) / max(delta(j, 'wal_syncs'), 1) for j in js]
    ref = bm if not arm.startswith('base (BQ') else bm
    P(f'| {arm} | {" / ".join(f"{x:,.0f}" for x in rr)} | {st.mean(rr):,.0f} rows/s | {max(rr)-min(rr):,.0f} rows/s ({100*(max(rr)-min(rr))/st.mean(rr):.1f} %) | '
      f'{100*(st.mean(rr)/bm-1):+.1f} % | {" / ".join(f"{100*(x/bm-1):+.1f} %" for x in rr)} | {" / ".join(f"{x:.1f}" for x in mm)} MiB/s | '
      f'{st.mean(mm):.1f} MiB/s | {100*(st.mean(mm)/bmm-1):+.1f} % | {" / ".join(f"{x:.1f}" for x in syn)} /s | {st.mean(mps):.2f} MiB |')
P()
# inserter latency in C3
P('**C3 inserter statement latency (one 1,024-row `INSERT` per statement, closed loop), median of the three runs per percentile**')
P()
P('| arm | statements per run (mean) | ' + ' | '.join(PCT_NAMES) + ' |')
P('|---|---|' + '---|' * 7)
for arm in ['base (BQ-S1)'] + ARMS:
    js = rowsS1 if arm.startswith('base (BQ') else [load(S2, 'relaxed', 'c3', arm, r) for r in (1, 2, 3)]
    v = [med([j['inserter'][p] for j in js]) for p in PCT]
    P(f'| {arm} | {st.mean(j["inserter"]["n"] for j in js):,.0f} | ' + ' | '.join(us(x) for x in v) + ' |')
P()

# inserter rows/s inside each cell
P('**Bulk writer throughput inside every cell (rows/s, mean of the three runs; range in brackets)**')
P()
P('| cell | ' + ' | '.join(f'{a}' for a in ['base (BQ-S1)'] + ARMS) + ' |')
P('|---|' + '---|' * 6)
for cell in ['c3', 'c2'] + C1:
    row = []
    for arm in ['base (BQ-S1)'] + ARMS:
        js = [load(S1, 'relaxed', cell, None, r) for r in (1, 2, 3)] if arm.startswith('base (BQ') else [load(S2, 'relaxed', cell, arm, r) for r in (1, 2, 3)]
        rr = [j['inserter_rows_per_s'] for j in js]
        row.append(f'{st.mean(rr):,.0f} rows/s [{min(rr):,.0f}-{max(rr):,.0f}]')
    P(f'| {cell} | ' + ' | '.join(row) + ' |')
P()
P('**Longest bulk `INSERT` statement (max of the inserter column, median of three runs)**')
P()
P('| cell | ' + ' | '.join(f'{a}' for a in ['base (BQ-S1)'] + ARMS) + ' |')
P('|---|' + '---|' * 6)
for cell in ['c3', 'c2'] + C1:
    row = []
    for arm in ['base (BQ-S1)'] + ARMS:
        js = [load(S1, 'relaxed', cell, None, r) for r in (1, 2, 3)] if arm.startswith('base (BQ') else [load(S2, 'relaxed', cell, arm, r) for r in (1, 2, 3)]
        row.append(' / '.join(us(j['inserter']['max_us']) for j in js))
    P(f'| {cell} | ' + ' | '.join(row) + ' |')
P()
print('\n'.join(OUT))
