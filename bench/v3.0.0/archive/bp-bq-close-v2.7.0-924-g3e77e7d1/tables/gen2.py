"""Fragments for the bisect, pooled ladder, cores = 4, lock and profile sections."""
import sys, os, glob, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lib import *

frags = {}
IDS = {
    'AB': ('cf889b63', '3e77e7d1'), 'AQ': ('cf889b63', '4cf30270'), 'S2S1': ('cf889b63', '1592007e'),
    'S4S6BQ': ('ccf9cfc1', '3e77e7d1'), 'MERGE': ('ccf9cfc1', '6139c2de'), 'S4': ('6139c2de', 'd555cb55'),
    'S6': ('d555cb55', '3e77e7d1'), 'MERGEALIGNED': ('ccf9cfc1 aligned', '6139c2de aligned'),
    'PREMERGE_FIX': ('ccf9cfc1', 'rollfix'), 'CLOSE_FIX': ('3e77e7d1', 'rollfix'),
}
LABEL = {
    'AB': 'BP + BQ, whole close', 'AQ': 'BQ alone (its instruments)', 'S2S1': 'BP-S2 + BP-S1 (retired kinds, counters)',
    'S4S6BQ': 'BQ merge + BP-S4 + BP-S6', 'MERGE': 'BQ merged onto BP-S3', 'S4': 'BP-S4 (purge on core 0\'s tick)',
    'S6': 'BP-S6 (64 lock partitions, no executed change at cores = 1)', 'MERGEALIGNED': 'BQ merge, both aligned builds',
    'PREMERGE_FIX': 'rollfix against BP-S3', 'CLOSE_FIX': 'rollfix against the close',
}
ORDER = ['AB', 'AQ', 'S2S1', 'S4S6BQ', 'MERGE', 'S4', 'S6', 'MERGEALIGNED', 'PREMERGE_FIX', 'CLOSE_FIX']

# ---- the ladder of paired pairs ----
lines = ['| pair | from (first server) | to (second server) | what lies between | every-row `UPDATE`, literal: to - from per run, % | median | 95 % interval of the mean (µs) | prepared: per run, % | median | 95 % interval (µs) | blocks to < from, literal / prepared (of 36) |',
         '|---|---|---|---|---|---|---|---|---|---|---|']
for pair in ORDER:
    out, runs = load_pair(pair)
    sl = pair_stats(out[('update-range', 'literal')])
    sp = pair_stats(out[('update-range', 'prepared')])
    ia, ib = IDS[pair]
    lines.append(f"| {pair} | `{ia}` | `{ib}` | {LABEL[pair]} | {' / '.join(f'{x:+.1f}' for x in sl['pct'])} % | {sl['med_pct']:+.1f} % | [{sl['lo']:+,.0f}, {sl['hi']:+,.0f}] µs | "
                 f"{' / '.join(f'{x:+.1f}' for x in sp['pct'])} % | {sp['med_pct']:+.1f} % | [{sp['lo']:+,.0f}, {sp['hi']:+,.0f}] µs | {sl['below_all']} / {sp['below_all']} blocks |")
frags['ladder'] = '\n'.join(lines)

# ---- select-pk across the same pairs ----
lines = ['| pair | from -> to | point `SELECT`, literal: per run, % | median | 95 % interval of the mean (µs) | prepared: per run, % | median | 95 % interval (µs) |', '|---|---|---|---|---|---|---|---|']
for pair in ORDER:
    out, runs = load_pair(pair)
    sl = pair_stats(out[('select-pk', 'literal')])
    sp = pair_stats(out[('select-pk', 'prepared')])
    ia, ib = IDS[pair]
    lines.append(f"| {pair} | `{ia}` -> `{ib}` | {' / '.join(f'{x:+.1f}' for x in sl['pct'])} % | {sl['med_pct']:+.1f} % | [{sl['lo']:+.2f}, {sl['hi']:+.2f}] µs | "
                 f"{' / '.join(f'{x:+.1f}' for x in sp['pct'])} % | {sp['med_pct']:+.1f} % | [{sp['lo']:+.2f}, {sp['hi']:+.2f}] µs |")
frags['ladder_select'] = '\n'.join(lines)

# ---- pooled absolute server CPU per binary ----
pool = {}
for name, (ia, ib) in IDS.items():
    out, runs = load_pair(name)
    for key, arms in out.items():
        for i, (xa, xb) in enumerate(zip(arms['A'], arms['B'])):
            pool.setdefault((key, ia, 'first'), []).append(xa['server_cpu_us'])
            pool.setdefault((key, ib, 'second'), []).append(xb['server_cpu_us'])
bins = [('cf889b63', 'A, BP\'s and BQ\'s opening'), ('1592007e', 'BP-S1 (counters)'), ('ccf9cfc1', 'BP-S3 head, before BQ is merged'),
        ('4cf30270', 'Q, BQ\'s instruments on A\'s base'), ('6139c2de', 'BQ merged into BP'), ('d555cb55', '+ BP-S4'), ('3e77e7d1', 'B, the close (+ BP-S6)'),
        ('rollfix', 'B with the per-append RollRecord made lazy (uncommitted)'), ('ccf9cfc1 aligned', 'aligned build'), ('6139c2de aligned', 'aligned build')]


def pooled(key, b):
    v = pool.get((key, b, 'first'), []) + pool.get((key, b, 'second'), [])
    return v


def pooled_role(key, b, role):
    return pool.get((key, b, role), [])


rows = []
for key, nd in ((('update-range', 'literal'), 0), (('update-range', 'prepared'), 0)):
    pass
lines = ['| binary | role in the build lineage | sessions (runs) pooled | every-row `UPDATE`, literal: mean (min - max) | vs `ccf9cfc1` | vs `cf889b63` pooled | prepared: mean (min - max) | vs `ccf9cfc1` | vs `cf889b63` pooled |',
         '|---|---|---|---|---|---|---|---|---|']
ref = {}
for key in (('update-range', 'literal'), ('update-range', 'prepared')):
    ref[(key, 'ccf')] = statistics.mean(pooled(key, 'ccf9cfc1'))
    ref[(key, 'A')] = statistics.mean(pooled(key, 'cf889b63'))
for b, role in bins:
    cells = []
    for key in (('update-range', 'literal'), ('update-range', 'prepared')):
        v = pooled(key, b)
        m = statistics.mean(v)
        cells.append(f'{m:,.0f} µs ({min(v):,.0f} - {max(v):,.0f}) = {1e6/m:,.1f} stmt/CPU-s | {100*(m/ref[(key,"ccf")]-1):+.1f} % | {100*(m/ref[(key,"A")]-1):+.1f} %')
    n = len(pooled(('update-range', 'literal'), b))
    lines.append(f'| `{b}` | {role} | {n} | {cells[0]} | {cells[1]} |')
frags['pooled'] = '\n'.join(lines)

# A in its two session families
lines = ['| `cf889b63` in | sessions | literal, per run | mean | prepared, per run | mean |', '|---|---|---|---|---|---|']
for fam, prs in (('the AB and S2S1 sessions (second server: `3e77e7d1`, `1592007e`)', ['AB', 'S2S1']), ('the AQ sessions (second server: `4cf30270`)', ['AQ'])):
    vl, vp = [], []
    for p in prs:
        out, runs = load_pair(p)
        vl += [x['server_cpu_us'] for x in out[('update-range', 'literal')]['A']]
        vp += [x['server_cpu_us'] for x in out[('update-range', 'prepared')]['A']]
    lines.append(f"| {fam} | {len(vl)} | {' / '.join(f'{x:,.0f}' for x in vl)} µs | {statistics.mean(vl):,.0f} µs | {' / '.join(f'{x:,.0f}' for x in vp)} µs | {statistics.mean(vp):,.0f} µs |")
frags['A_families'] = '\n'.join(lines)

# role effect: same binary in both roles
lines = ['| binary | first-server sessions (mean of n) | second-server sessions (mean of n) | second vs first |', '|---|---|---|---|']
for b in ('3e77e7d1', '6139c2de', 'd555cb55'):
    for key in (('update-range', 'literal'), ('update-range', 'prepared'), ('select-pk', 'literal'), ('select-pk', 'prepared')):
        f1 = pooled_role(key, b, 'first')
        s1 = pooled_role(key, b, 'second')
        nd = 0 if key[0] == 'update-range' else 2
        label = ('every-row `UPDATE`' if key[0] == 'update-range' else 'point `SELECT`') + f', {key[1]}'
        lines.append(f"| `{b}`, {label} | {statistics.mean(f1):,.{nd}f} µs (n={len(f1)}) | {statistics.mean(s1):,.{nd}f} µs (n={len(s1)}) | {100*(statistics.mean(s1)/statistics.mean(f1)-1):+.1f} % |")
frags['role'] = '\n'.join(lines)

# ---- cores = 4 ----
BA = OUT + '/ba'
ba = {}
for shape in ('point', 'insert1'):
    for side in 'AQB':
        for r in (1, 2, 3):
            f = glob.glob(f'{BA}/{shape}-{side}-r{r}/*.json')[0]
            ba[(shape, side, r)] = json.load(open(f))
BIN = {'A': 'A `cf889b63`', 'Q': 'Q `4cf30270`', 'B': 'B `3e77e7d1`'}
lines = ['| shape | binary | runs | stmt/s median (min - max) | spread (max - min, % of median) | p0 | p25 | p50 | p95 | p99 (min - max) | max (min - max) | errors / refusals |', '|---|---|---|---|---|---|---|---|---|---|---|---|']
for shape in ('point', 'insert1'):
    for side in 'AQB':
        js = [ba[(shape, side, r)] for r in (1, 2, 3)]
        t = [j['statements_per_s'] for j in js]
        g = lambda k: med([j[k] for j in js])
        lines.append(f"| {shape} | {BIN[side]} | 3 | {med(t):,.0f} stmt/s ({min(t):,.0f} - {max(t):,.0f}) | {100*(max(t)-min(t))/med(t):.1f} % | {g('p0_us'):,.1f} µs | {g('p25_us'):,.1f} µs | {g('p50_us'):,.1f} µs | {g('p95_us'):,.1f} µs | "
                     f"{g('p99_us'):,.1f} µs ({min(j['p99_us'] for j in js):,.1f} - {max(j['p99_us'] for j in js):,.1f}) | {g('max_us')/1000:,.1f} ms ({min(j['max_us'] for j in js)/1000:,.1f} - {max(j['max_us'] for j in js)/1000:,.1f}) | "
                     f"{sum(j['errors'] for j in js)} / {sum(j['refusals'] for j in js)} |")
frags['ba_summary'] = '\n'.join(lines)

lines = ['| shape | binary | run | stmt/s | p0 | p25 | p50 | p95 | p99 | max | sessions per core (c0 / c1 / c2 / c3) | client CPU share (max of 8 sessions) | load before (1 / 5 / 15 min) |', '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
for shape in ('point', 'insert1'):
    for side in 'AQB':
        for r in (1, 2, 3):
            j = ba[(shape, side, r)]
            hist = collections.Counter(int(c) for c in j['session_cores'])
            lines.append(f"| {shape} | {BIN[side]} | r{r} | {j['statements_per_s']:,.0f} stmt/s | {j['p0_us']:,.1f} µs | {j['p25_us']:,.1f} µs | {j['p50_us']:,.1f} µs | {j['p95_us']:,.1f} µs | {j['p99_us']:,.1f} µs | {j['max_us']/1000:,.1f} ms | "
                         f"{' / '.join(str(hist.get(c, 0)) for c in range(4))} | {max(j['client_cpu_share']):.2f} | {' / '.join(j['host_before']['loadavg'].split()[:3])} |")
frags['ba_runs'] = '\n'.join(lines)

# waits per statement (named waits), median over three runs
lines = ['| shape | binary | mean latency, Little (8 sessions / stmt/s) | page-latch waits per statement | frame-table wait per statement | lock-partition wait per statement | WAL stream-latch wait per statement | WAL sync-gate wait per statement | sum of the named waits | share of mean latency |', '|---|---|---|---|---|---|---|---|---|---|']
for shape in ('point', 'insert1'):
    for side in 'AQB':
        js = [ba[(shape, side, r)] for r in (1, 2, 3)]
        def per(k):
            return med([j['meta_delta'].get(k, 0) / j['statements'] for j in js])
        lat = med([8 / j['statements_per_s'] * 1e6 for j in js])
        ft, lp, ws, sg = per('contention_frame_table_wait_us'), per('contention_lock_partition_wait_us'), per('contention_wal_stream_wait_us'), per('contention_wal_sync_gate_wait_us')
        pl = per('contention_page_latch_waits')
        tot = ft + lp + ws + sg
        lines.append(f"| {shape} | {BIN[side]} | {lat:,.1f} µs | {pl:.3f} waits (no duration kept) | {ft:.3f} µs | {lp:.3f} µs | {ws:.3f} µs | {sg:.3f} µs | {tot:.3f} µs | {100*tot/lat:.2f} % |")
frags['ba_waits'] = '\n'.join(lines)

# roll instruments in insert1
lines = ['| binary | run | rolls | summed hold | longest hold | sync gate | create (header + dir sync + rename) | body hit / waited / inline | max statement (any session) |', '|---|---|---|---|---|---|---|---|---|']
for side in 'QB':
    for r in (1, 2, 3):
        j = ba[('insert1', side, r)]
        md = j['meta_delta']
        lines.append(f"| {BIN[side]} | r{r} | {md['wal_rolls']:.0f} | {md['wal_roll_hold_us']/1000:,.1f} ms | {md['wal_roll_hold_max_us']/1000:,.1f} ms | {md['wal_roll_sync_gate_us']/1000:,.1f} ms | {md['wal_roll_create_us']/1000:,.1f} ms | "
                     f"{md.get('wal_roll_body_hit',0):.0f} / {md.get('wal_roll_body_waited',0):.0f} / {md.get('wal_roll_body_inline',0):.0f} | {j['max_us']/1000:,.1f} ms |")
frags['ba_rolls'] = '\n'.join(lines)

lines = ['| binary | run | longest carve | longest out-of-run hold of a reactor | longest checkpoint step | max statement |', '|---|---|---|---|---|---|']
for side in 'AQB':
    for r in (1, 2, 3):
        j = ba[('insert1', side, r)]
        ml = j['meta_longest']
        lines.append(f"| {BIN[side]} | r{r} | {ml['contention_carve_longest_us']/1000:,.1f} ms | {ml['sched_hold_out_run_longest_us']/1000:,.1f} ms | {ml['contention_checkpoint_longest_us']/1000:,.1f} ms | {j['max_us']/1000:,.1f} ms |")
frags['ba_longest'] = '\n'.join(lines)

# ---- lock cells ----
LD = OUT + '/lock'
rows = {}
for mode in ('plain', 'fence'):
    for side in ('S6A', 'B'):
        for r in range(3):
            f = f'{LD}/{side}-locks-relaxed-k8-{mode}-r{r}.json'
            m = json.load(open(f))
            b = json.load(open(f.replace('.json', '.bench.json')))
            stm = b['sessions'] * b['ops_per_session_per_arm'] * 6
            a = b['arms']['B']
            ma, mb = m['meta_after'], m['meta_before']
            dl = lambda k: ma.get(k, 0) - mb.get(k, 0)
            rows[(mode, side, r)] = dict(cpu=m['server_cpu_s'] * 1e6 / stm, ud=a['update-disjoint'], uda=a['update-disjoint-again'], ping=a['ping'], walks=dl('contention_fence_walks') / stm,
                                         pw=dl('contention_lock_partition_waits') / stm, pwus=dl('contention_lock_partition_wait_us') / stm, lb=m['loadavg_before'])
SIDE = {'S6A': 'S6A `ccf9cfc1` (64 x 8 partitions)', 'B': 'B `3e77e7d1` (64 partitions)'}
lines = ['| held fence | binary | server CPU per statement, r0 / r1 / r2 | median | update-disjoint stmt/s, r0 / r1 / r2 | median | lock-partition waits per statement, r0 / r1 / r2 | fence walks per statement |', '|---|---|---|---|---|---|---|---|']
for mode in ('plain', 'fence'):
    for side in ('S6A', 'B'):
        v = [rows[(mode, side, r)] for r in range(3)]
        lines.append(f"| {mode} | {SIDE[side]} | {' / '.join(f'{x['cpu']:.2f}' for x in v)} µs | **{med([x['cpu'] for x in v]):.2f} µs** | {' / '.join(f'{x['ud']['qps']:,.0f}' for x in v)} stmt/s | {med([x['ud']['qps'] for x in v]):,.0f} stmt/s | "
                     f"{' / '.join(f'{x['pw']*1000:.1f}' for x in v)} per 1,000 | {statistics.mean(x['walks'] for x in v):.3f} |")
frags['lock_cpu'] = '\n'.join(lines)

lines = ['| binary | fence premium over plain, per run r0 / r1 / r2 (CPU per statement) | median | premium of the medians |', '|---|---|---|---|']
for side in ('S6A', 'B'):
    pr = [100 * (rows[('fence', side, r)]['cpu'] / rows[('plain', side, r)]['cpu'] - 1) for r in range(3)]
    mp = 100 * (med([rows[('fence', side, r)]['cpu'] for r in range(3)]) / med([rows[('plain', side, r)]['cpu'] for r in range(3)]) - 1)
    lines.append(f"| {SIDE[side]} | {' / '.join(f'{x:+.1f}' for x in pr)} % | {med(pr):+.1f} % | {mp:+.1f} % |")
frags['lock_premium'] = '\n'.join(lines)

lines = ['| held fence | binary | arm | ops per run | p0 | p25 | p50 | p95 | p99 | stmt/s | errors |', '|---|---|---|---|---|---|---|---|---|---|---|']
for mode in ('plain', 'fence'):
    for side in ('S6A', 'B'):
        for arm, nm in (('ud', 'update-disjoint'), ('uda', 'update-disjoint-again'), ('ping', 'ping (control, never touches a tuple unit)')):
            v = [rows[(mode, side, r)][arm] for r in range(3)]
            g = lambda k: med([x[k] for x in v])
            lines.append(f"| {mode} | {SIDE[side].split(' ')[0] + ' ' + SIDE[side].split(' ')[1]} | {nm} | {v[0]['ops']:,} | {g('p0_us'):,.1f} µs | {g('p25_us'):,.1f} µs | {g('p50_us'):,.1f} µs | {g('p95_us'):,.1f} µs | {g('p99_us'):,.1f} µs | {g('qps'):,.0f} stmt/s | {sum(x['errors'] for x in v)} |")
frags['lock_lat'] = '\n'.join(lines)

for k, v in frags.items():
    open(f'{HERE}/frag_{k}.md', 'w').write(v + '\n')
    print('=====', k)
    print(v)
