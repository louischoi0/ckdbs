"""Fragments for the cores = 1 sections (AB, AQ pairs). Writes frag_*.md next to this file."""
import sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lib import *

SHAPES = [('select-pk', 'literal', 'point `SELECT`, literal', 3000), ('select-pk', 'prepared', 'point `SELECT`, prepared', 3000),
          ('update-lit', 'literal', 'point `UPDATE`, literal', 3000), ('update-lit', 'prepared', 'point `UPDATE`, prepared', 3000),
          ('update-range', 'literal', 'every-row `UPDATE`, literal', 60), ('update-range', 'prepared', 'every-row `UPDATE`, prepared', 60)]


def us(x, nd=2):
    return f'{x:,.{nd}f} µs'


def fmt3(v, nd=2, unit=' µs'):
    return ' / '.join(f'{x:,.{nd}f}' for x in v) + unit


def pct3(v):
    return ' / '.join(f'{x:+.1f}' for x in v) + ' %'


frags = {}

# ---- server CPU per statement, per pair ----
for pair, other in (('AB', 'B = `3e77e7d1`'), ('AQ', 'Q = `4cf30270`')):
    out, runs = load_pair(pair)
    lines = [f'| shape (driver path) | statements per arm | A = `cf889b63` r1 / r2 / r3 | {other} r1 / r2 / r3 | {other[0]} - A r1 / r2 / r3 | {other[0]} - A, % of A, r1 / r2 / r3 | median {other[0]} - A | spread of {other[0]} - A over runs | A, run to run (max - min) | 95 % interval of the mean {other[0]} - A (run as unit) | contains 0 | A within a run: 12 blocks (max - min), median of runs |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|']
    for shape, path, label, nops in SHAPES:
        s = pair_stats(out[(shape, path)])
        nd = 1 if shape == 'update-range' else 2
        lines.append(f"| {label} | {nops:,} stmt | {fmt3(s['a'], nd)} | {fmt3(s['b'], nd)} | {fmt3(s['d'], nd)} | {pct3(s['pct'])} | "
                     f"{s['med_d']:+,.{nd}f} µs ({s['med_pct']:+.1f} %) | {s['spread_d']:,.{nd}f} µs | {s['spread_a']:,.{nd}f} µs | "
                     f"[{s['lo']:+,.{nd}f}, {s['hi']:+,.{nd}f}] µs | {'yes' if s['lo'] <= 0 <= s['hi'] else '**no**'} | {med([max(x['block_server_us']) - min(x['block_server_us']) for x in out[(shape, path)]['A']]):,.{nd}f} µs |")
    frags[f'cpu_{pair}'] = '\n'.join(lines)

    # blocks
    lines = [f'| shape (driver path) | blocks with {other[0]} < A, r1 / r2 / r3 (of 12 each) | all 36 | sign test, one-sided p ({other[0]} lower) | mean of the 36 paired {other[0]} - A |',
             '|---|---|---|---|---|']
    for shape, path, label, nops in SHAPES:
        s = pair_stats(out[(shape, path)])
        allv = [v for r in s['blk'] for v in r]
        p = sign_p_lower(s['below_all'], s['nblk'])
        nd = 1 if shape == 'update-range' else 2
        lines.append(f"| {label} | {' / '.join(str(b) for b in s['below'])} blocks | {s['below_all']} of {s['nblk']} blocks | {p:.3f} | {sum(allv)/len(allv):+,.{nd}f} µs |")
    frags[f'blk_{pair}'] = '\n'.join(lines)

    # derived stmt per server-CPU second
    lines = [f'| shape (driver path) | A | {other} | {other[0]} vs A |', '|---|---|---|---|']
    for shape, path, label, nops in SHAPES:
        s = pair_stats(out[(shape, path)])
        a, b = 1e6 / med(s['a']), 1e6 / med(s['b'])
        nd = 1 if shape == 'update-range' else 0
        lines.append(f'| {label} | {a:,.{nd}f} stmt/CPU-s | {b:,.{nd}f} stmt/CPU-s | {100*(b/a-1):+.1f} % |')
    frags[f'rate_{pair}'] = '\n'.join(lines)

# ---- wall-clock latency percentiles (median of the three runs per percentile) ----
lines = ['| shape (driver path) | binary (pair) | statements per run | p0 | p25 | p50 | p90 | p99 | 1,000,000 / p50 | 1,000,000 / mean |', '|---|---|---|---|---|---|---|---|---|---|']
for shape, path, label, nops in SHAPES:
    for pair, side, name in (('AB', 'A', 'A `cf889b63` (AB)'), ('AB', 'B', 'B `3e77e7d1`'), ('AQ', 'A', 'A `cf889b63` (AQ)'), ('AQ', 'B', 'Q `4cf30270`')):
        out, runs = load_pair(pair)
        arms = out[(shape, path)][side]
        g = lambda k: med([x[k] for x in arms])
        nd = 1 if shape == 'update-range' else 2
        pk = 'p99 (= max of 60)' if shape == 'update-range' else 'p99'
        lines.append(f"| {label} | {name} | {nops:,} | {g('p0_us'):,.{nd}f} µs | {g('p25_us'):,.{nd}f} µs | {g('p50_us'):,.{nd}f} µs | {g('p90_us'):,.{nd}f} µs | {g('p99_us'):,.{nd}f} µs | "
                     f"{1e6/g('p50_us'):,.{1 if shape=='update-range' else 0}f} QPS | {1e6/g('mean_us'):,.{1 if shape=='update-range' else 0}f} QPS |")
frags['wall'] = '\n'.join(lines)

# ---- client-side split for a point statement ----
lines = ['| shape (driver path) | binary (pair) | server on-CPU per statement | client thread on-CPU per statement | p50 wall | mean wall |', '|---|---|---|---|---|---|']
for shape, path, label, nops in SHAPES[:4]:
    for pair, side, name in (('AB', 'A', 'A (AB)'), ('AB', 'B', 'B'), ('AQ', 'B', 'Q')):
        out, runs = load_pair(pair)
        arms = out[(shape, path)][side]
        g = lambda k: med([x[k] for x in arms])
        lines.append(f"| {label} | {name} | {g('server_cpu_us'):.2f} µs | {g('driver_cpu_us'):.2f} µs | {g('p50_us'):.2f} µs | {g('mean_us'):.2f} µs |")
frags['split'] = '\n'.join(lines)

# ---- longest statement of the every-row arms, by role ----
lines = ['| pair | first-server binary | first server: longest statement of an arm (6 arms: 2 paths x 3 runs) | second-server binary | second server: longest statement of an arm (6 arms) |', '|---|---|---|---|---|']
roles = {'AB': ('A `cf889b63`', 'B `3e77e7d1`'), 'AQ': ('A `cf889b63`', 'Q `4cf30270`'), 'S2S1': ('A `cf889b63`', '`1592007e`'),
         'S4S6BQ': ('`ccf9cfc1`', 'B `3e77e7d1`'), 'MERGE': ('`ccf9cfc1`', '`6139c2de`'), 'S4': ('`6139c2de`', '`d555cb55`'),
         'S6': ('`d555cb55`', 'B `3e77e7d1`'), 'MERGEALIGNED': ('`ccf9cfc1` aligned', '`6139c2de` aligned'),
         'PREMERGE_FIX': ('`ccf9cfc1`', 'rollfix'), 'CLOSE_FIX': ('B `3e77e7d1`', 'rollfix')}
for pair, (ra, rb) in roles.items():
    out, runs = load_pair(pair)
    def lg(side):
        v = sorted(x['p99_us'] / 1000 for k in (('update-range', 'literal'), ('update-range', 'prepared')) for x in out[k][side])
        return f'{v[0]:,.0f}-{v[-1]:,.0f} ms'
    lines.append(f'| {pair} | {ra} | {lg("A")} | {rb} | {lg("B")} |')
frags['tail'] = '\n'.join(lines)

for k, v in frags.items():
    open(f'{HERE}/frag_{k}.md', 'w').write(v + '\n')
    print('=====', k)
    print(v)
