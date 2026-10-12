import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from bp_lib import *

OUT = []


def P(s=''):
    OUT.append(s)


R = {r: bn(r) for r in (1, 2, 3)}


def cpu(r, sh, pa, b, key='server_cpu_us'):
    return R[r][2][(sh, pa, b)][key]


def label(sh, pa):
    return f'{sh} ({pa})'


# ---- T1 server CPU per statement
P('**Server on-CPU time per statement (`/proc/<pid>/task/*/schedstat`, every server thread, over the arm), `cores = 1`, `relaxed`, 10,000 rows**')
P()
P('| shape (driver path) | statements per arm | A = `1592007e` r1 / r2 / r3 | B = `ccf9cfc1` r1 / r2 / r3 | B - A r1 / r2 / r3 | B - A, % of A, r1 / r2 / r3 | median B - A | spread of B - A (max - min) | A, run to run (max - min) | A, within a run: 12 blocks (max - min), median of runs |')
P('|---|---|---|---|---|---|---|---|---|---|')
for sh, pa in SHAPES:
    A = [cpu(r, sh, pa, 'A') for r in (1, 2, 3)]
    B = [cpu(r, sh, pa, 'B') for r in (1, 2, 3)]
    D = [b - a for a, b in zip(A, B)]
    PC = [100 * d / a for a, d in zip(A, D)]
    ops = R[1][2][(sh, pa, 'A')]['ops']
    bl = [max(R[r][2][(sh, pa, 'A')]['block_server_us']) - min(R[r][2][(sh, pa, 'A')]['block_server_us']) for r in (1, 2, 3)]
    nd = 2 if sh != 'update-range' else 1
    f = lambda x: (f'{x:,.{nd}f}')
    P(f'| {label(sh, pa)} | {ops:,} stmt | {" / ".join(f(x) + " µs" for x in A)} | {" / ".join(f(x) + " µs" for x in B)} | '
      f'{" / ".join(f"{x:+,.{nd}f} µs" for x in D)} | {" / ".join(f"{x:+.1f} %" for x in PC)} | {med(D):+,.{nd}f} µs ({med(PC):+.1f} %) | '
      f'{max(D) - min(D):,.{nd}f} µs | {max(A) - min(A):,.{nd}f} µs | {f(med(bl))} µs |')
P()

# ---- T1b the same as statements per server-CPU second (derived)
P('**The same, as statements per server-CPU second (derived: 1,000,000 / CPU µs per statement; median of the three runs)**')
P()
P('| shape (driver path) | A | B | B vs A |')
P('|---|---|---|---|')
for sh, pa in SHAPES:
    a = med([1e6 / cpu(r, sh, pa, 'A') for r in (1, 2, 3)])
    b = med([1e6 / cpu(r, sh, pa, 'B') for r in (1, 2, 3)])
    P(f'| {label(sh, pa)} | {a:,.0f} stmt/CPU-s | {b:,.0f} stmt/CPU-s | {100 * (b / a - 1):+.1f} % |')
P()

# ---- T2 paired blocks
P('**Paired blocks: the arm is cut into 12 blocks taken alternately, so block `i` of A and of B share a host moment; B - A per block, in µs of server CPU per statement**')
P()
P('| shape (driver path) | run | mean of 12 paired B - A | standard error | blocks with B < A | sign test, one-sided p |')
P('|---|---|---|---|---|---|')
from math import comb
for sh, pa in SHAPES:
    allD = []
    for r in (1, 2, 3):
        a = R[r][2][(sh, pa, 'A')]['block_server_us']
        b = R[r][2][(sh, pa, 'B')]['block_server_us']
        d = [y - x for x, y in zip(a, b)]
        allD += d
        nd = 2 if sh != 'update-range' else 1
        neg = sum(1 for x in d if x < 0)
        p = sum(comb(12, k) for k in range(neg, 13)) / 2 ** 12  # P(at least `neg` negatives under H0)
        P(f'| {label(sh, pa)} | r{r} | {st.mean(d):+,.{nd}f} µs | {st.stdev(d) / len(d) ** .5:,.{nd}f} µs | {neg} of 12 | {p:.3f} |')
    neg = sum(1 for x in allD if x < 0)
    n = len(allD)
    p = sum(comb(n, k) for k in range(neg, n + 1)) / 2 ** n
    P(f'| {label(sh, pa)} | **all 36** | **{st.mean(allD):+,.{nd}f} µs** | {st.stdev(allD) / n ** .5:,.{nd}f} µs | **{neg} of {n}** | {p:.3f} |')
P()

# ---- T3 throughput (derived) + latency percentiles
P('**Throughput of the serial driver (derived: QPS = 1,000,000 / mean wall µs per statement, from the arm\'s mean; median of the three runs)**')
P()
P('| shape (driver path) | A | B | B vs A | A r1 / r2 / r3 | B r1 / r2 / r3 |')
P('|---|---|---|---|---|---|')
for sh, pa in SHAPES:
    qa = [1e6 / R[r][2][(sh, pa, 'A')]['mean_us'] for r in (1, 2, 3)]
    qb = [1e6 / R[r][2][(sh, pa, 'B')]['mean_us'] for r in (1, 2, 3)]
    nd = 0 if sh != 'update-range' else 1
    P(f'| {label(sh, pa)} | {med(qa):,.{nd}f} QPS | {med(qb):,.{nd}f} QPS | {100 * (med(qb) / med(qa) - 1):+.1f} % | '
      f'{" / ".join(f"{x:,.{nd}f}" for x in qa)} QPS | {" / ".join(f"{x:,.{nd}f}" for x in qb)} QPS |')
P()
P('For `update-range` the mean is not a typical statement: every one of B\'s six arms (3 runs x 2 paths) has one statement of 0.91-1.09 s among its 60, which puts the mean at 23.5-25.9 ms against medians of 6.54-6.72 ms; A\'s longest statements are 17-196 ms. The rate at the median is in the next table and agrees with the CPU (section 3).')
P()
P('**Wall-clock latency per statement (client side), median of the three runs per percentile; `p90` stands where the rule asks for `p95` (the driver records p90)**')
P()
P('| shape (driver path) | binary | statements per run | p0 | p25 | p50 | p90 | p99 | 1,000,000 / p50 |')
P('|---|---|---|---|---|---|---|---|---|')
for sh, pa in SHAPES:
    for b in ('A', 'B'):
        v = lambda k: med([R[r][2][(sh, pa, b)][k] for r in (1, 2, 3)])
        nd = 1
        P(f'| {label(sh, pa)} | {b} | {R[1][2][(sh, pa, b)]["ops"]:,} | {us(v("p0_us"))} | {us(v("p25_us"))} | {us(v("p50_us"))} | {us(v("p90_us"))} | {us(v("p99_us"))} | {1e6 / v("p50_us"):,.1f} QPS |')
P()
P('The `update-range` percentiles are over 60 statements: p90 is the 54th and p99 the 60th, i.e. the maximum, so only p0-p50 describe the typical statement there.')
P()

# ---- T4 driver CPU and idle burn
P('**Controls: the client\'s own on-CPU time per statement (the driver thread, identical work on both sides) and the server\'s idle burn**')
P()
P('| shape (driver path) | driver CPU, A r1 / r2 / r3 | driver CPU, B r1 / r2 / r3 | B - A, median |')
P('|---|---|---|---|')
for sh, pa in SHAPES:
    A = [cpu(r, sh, pa, 'A', 'driver_cpu_us') for r in (1, 2, 3)]
    B = [cpu(r, sh, pa, 'B', 'driver_cpu_us') for r in (1, 2, 3)]
    P(f'| {label(sh, pa)} | {" / ".join(f"{x:,.2f} µs" for x in A)} | {" / ".join(f"{x:,.2f} µs" for x in B)} | {med([b - a for a, b in zip(A, B)]):+,.2f} µs |')
P()
P('| run | idle CPU of the server per second, A | B | B - A |')
P('|---|---|---|---|')
for r in (1, 2, 3):
    i = R[r][1]['idle_cpu_us_per_s']
    P(f'| r{r} | {i["A"]:,.0f} µs/s | {i["B"]:,.0f} µs/s | {i["B"] - i["A"]:+,.0f} µs/s ({100 * (i["B"] / i["A"] - 1):+.1f} %) |')
P()

# ---- T5 host per cell
hs = {}
for line in open('/home/cdkbs/bench-runs/bp-s1/v2/out/hoststamps.log'):
    hs.setdefault('x', []).append(line.rstrip())
P('**Host per cell** (`bn` cells: the driver\'s `host_before` / `host_after`; commit cells: the same, from `ba_census`; both list competing `cc1plus|cmake --build|ctest` processes, empty in every cell)')
P()
P('| cell | start (UTC) | load before (1 / 5 / 15 min) | load after | competing processes | quiet gate waited | `df -T` |')
P('|---|---|---|---|---|---|---|')
stamps = {'bn1': '14:21:13', 'bn2': '19:10:04', 'bn3': '19:10:16'}
for r in (1, 2, 3):
    run = R[r][1]
    P(f'| bn r{r} (12 blocks x 6 arms x 2 binaries) | {stamps["bn" + str(r)]} | {" / ".join(run["host_before"]["loadavg"].split()[:3])} | {" / ".join(run["host_after"]["loadavg"].split()[:3])} | '
      f'{run["host_before"]["competing"] or "none"} | {run["quiet_gate"]["waited_s"]:.0f} s | ext4 |')
cstart = {}
for line in open('/home/cdkbs/bench-runs/bp-s1/v2/out/hoststamps.log'):
    p = line.split()
    if len(p) > 6 and p[2] == 'ab' and p[3] == 'commit':
        cstart[(p[4], p[5], p[6])] = p[1]
for r in (1, 2, 3):
    for dur in ('group', 'strict'):
        for side in ('a', 'b'):
            d = commit(dur, side, r)
            P(f'| commit {dur} {side.upper()} r{r} | {cstart[(dur, side, "r" + str(r))]} | {" / ".join(d["host_before"]["loadavg"].split()[:3])} | {" / ".join(d["host_after"]["loadavg"].split()[:3])} | '
              f'{d["host_before"]["competing"] or "none"} | {d["wait_s"]:.0f} s | {d["data_device"].split()[-1]} |')
P()

# ---- T6 commit loops
P('**Commit loops (`ba_census.py --shape insert1`, one session, `cores = 1`, 20 s after a 2 s warm-up, one fresh server per cell); A and B adjacent in time, A first**')
P()
P('| durability | binary | statements per run | stmt/s r1 / r2 / r3 | median stmt/s | spread | p0 | p25 | p50 | p95 | p99 | max |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|')
CM = {}
for dur in ('group', 'strict'):
    for side in ('a', 'b'):
        ds = [commit(dur, side, r) for r in (1, 2, 3)]
        CM[(dur, side)] = ds
        sps = [d['statements_per_s'] for d in ds]
        v = lambda k: med([d[k] for d in ds])
        P(f'| {dur} | {side.upper()} | {" / ".join(f"{d["statements"]:,}" for d in ds)} | {" / ".join(f"{x:,.1f}" for x in sps)} | {med(sps):,.1f} stmt/s | {max(sps) - min(sps):,.1f} stmt/s ({100 * (max(sps) - min(sps)) / med(sps):.1f} %) | '
          f'{us(v("p0_us"))} | {us(v("p25_us"))} | {us(v("p50_us"))} | {us(v("p95_us"))} | {us(v("p99_us"))} | {us(v("max_us"))} |')
P()
P('**B against A, commit loops (median of three runs; paired by run in the last column)**')
P()
P('| durability | stmt/s A | stmt/s B | B vs A | paired by run: B vs A, r1 / r2 / r3 | p50 A | p50 B |')
P('|---|---|---|---|---|---|---|')
for dur in ('group', 'strict'):
    a = [d['statements_per_s'] for d in CM[(dur, 'a')]]
    b = [d['statements_per_s'] for d in CM[(dur, 'b')]]
    pa = [d['p50_us'] for d in CM[(dur, 'a')]]
    pb = [d['p50_us'] for d in CM[(dur, 'b')]]
    P(f'| {dur} | {med(a):,.1f} stmt/s | {med(b):,.1f} stmt/s | {100 * (med(b) / med(a) - 1):+.1f} % | {" / ".join(f"{100 * (y / x - 1):+.1f} %" for x, y in zip(a, b))} | {us(med(pa))} | {us(med(pb))} |')
P()

# ---- T7 what a commit is made of
P('**What a commit statement is made of (per statement, mean of the three runs of each cell)**')
P()
P('| durability | binary | syncs per statement | of which on the reactor thread (inline) | time inside the reactor\'s foreground polls per statement | `SHOW META` foreground CPU delta per statement |')
P('|---|---|---|---|---|---|')
for dur in ('group', 'strict'):
    for side in ('a', 'b'):
        ds = CM[(dur, side)]
        syn = st.mean(d['meta_delta']['wal_syncs'] / d['statements'] for d in ds)
        inl = st.mean(d['meta_delta']['contention_syncs_inline'] / d['statements'] for d in ds)
        pol = st.mean(d['meta_delta']['sched_foreground_polled_us'] / d['statements'] for d in ds)
        con = [d['meta_delta']['sched_foreground_consumed_us'] / d['statements'] for d in ds]
        P(f'| {dur} | {side.upper()} | {syn:.3f} | {inl:.3f} | {pol:,.0f} µs | {" / ".join(f"{x:+,.1f} µs" for x in con)} (r1 / r2 / r3) |')
P()

# ---- T8 per-run CI
P('**The run as the unit: mean of the three per-run B - A server-CPU deltas, its standard error (n = 3) and the 95 % interval (t, 2 degrees of freedom: 4.30)**')
P()
P('| shape (driver path) | mean of 3 run deltas | standard error | 95 % interval | contains 0 | detectable change at this resolution (half-width) | half-width as % of A |')
P('|---|---|---|---|---|---|---|')
for sh, pa in SHAPES:
    D = [cpu(r, sh, pa, 'B') - cpu(r, sh, pa, 'A') for r in (1, 2, 3)]
    A = med([cpu(r, sh, pa, 'A') for r in (1, 2, 3)])
    m = st.mean(D); se = st.stdev(D) / 3 ** .5; hw = 4.303 * se
    nd = 2 if sh != 'update-range' else 0
    P(f'| {label(sh, pa)} | {m:+,.{nd}f} µs | {se:,.{nd}f} µs | [{m - hw:+,.{nd}f}, {m + hw:+,.{nd}f}] µs | {"yes" if m - hw <= 0 <= m + hw else "no"} | {hw:,.{nd}f} µs | {100 * hw / A:.1f} % |')
P()

# ---- T9 wait accounting for a point statement
P('**What a point statement is made of (wall mean per statement, the server\'s and the client\'s CPU, median of the three runs)**')
P()
P('| shape (driver path) | binary | wall, mean | server CPU | client CPU | server + client CPU - wall | share of the wall that is the two processes\' CPU |')
P('|---|---|---|---|---|---|---|')
for sh, pa in SHAPES[:4]:
    for b in ('A', 'B'):
        w = med([R[r][2][(sh, pa, b)]['mean_us'] for r in (1, 2, 3)])
        sc = med([cpu(r, sh, pa, b) for r in (1, 2, 3)])
        dc = med([cpu(r, sh, pa, b, 'driver_cpu_us') for r in (1, 2, 3)])
        P(f'| {label(sh, pa)} | {b} | {w:.2f} µs | {sc:.2f} µs | {dc:.2f} µs | {sc + dc - w:+.2f} µs | {100 * (sc + dc) / w:.0f} % |')
P()

# ---- Appendix per run
P('**Appendix A1. Every `bn_census` arm: wall-clock latency, QPS (derived), server and client CPU**')
P()
P('| run | shape (driver path) | bin | statements | p0 | p25 | p50 | p90 | p99 | mean | QPS from mean (derived) | server CPU | client CPU |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for r in (1, 2, 3):
    for sh, pa in SHAPES:
        for b in ('A', 'B'):
            a = R[r][2][(sh, pa, b)]
            P(f'| r{r} | {label(sh, pa)} | {b} | {a["ops"]:,} | {us(a["p0_us"])} | {us(a["p25_us"])} | {us(a["p50_us"])} | {us(a["p90_us"])} | {us(a["p99_us"])} | {us(a["mean_us"])} | '
              f'{1e6 / a["mean_us"]:,.1f} QPS | {a["server_cpu_us"]:,.2f} µs | {a["driver_cpu_us"]:,.2f} µs |')
P()
P('**Appendix A2. Every commit-loop run**')
P()
P('| durability | binary | run | statements | stmt/s | p0 | p25 | p50 | p95 | p99 | max | checkpoint runs in the window |')
P('|---|---|---|---|---|---|---|---|---|---|---|---|')
for dur in ('group', 'strict'):
    for side in ('a', 'b'):
        for r in (1, 2, 3):
            d = commit(dur, side, r)
            P(f'| {dur} | {side.upper()} | r{r} | {d["statements"]:,} | {d["statements_per_s"]:,.1f} stmt/s | {us(d["p0_us"])} | {us(d["p25_us"])} | {us(d["p50_us"])} | {us(d["p95_us"])} | {us(d["p99_us"])} | {us(d["max_us"])} | {d["meta_delta"]["checkpoint_runs"]:.0f} |')
P()
print('\n'.join(OUT))
