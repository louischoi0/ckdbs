import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from doc import *
from an import *
import host


def win(tags):
    s = [host.STARTS[t] for t in tags if t in host.STARTS]
    e = [host.ENDS[t][0] for t in tags if t in host.ENDS]
    return min(s)[11:19], max(e)[11:19]


def allr(base):
    return [p + base for p in PFX]


core = [p + c for c in ("s2-c8-s", "s2-c1-s", "s2-c1-g", "s2-c8-g") for p in PFX]
rrt = ["rr-s2-c1-g", "rr-s2-c8-g"]
xb = [f"x-b{b}-s2-c8-{s}" for b in (1, 2, 4, 16) for s in "gs"]
sat = [f"sat-b{b}-s2-c8-{s}" for b in (16, 32) for s in "gs"]
b1t = [p + c for c in ("b1-s2-c1-g", "b1-s2-c1-s") for p in PFX]
pgt = [p + "pg-s2-b1" for p in PFX]

T_ = lambda tag: (new(tag) or {})
tp = lambda tag: [d["meta"]["tps"] for d in runs(new, tag)]
tpo = lambda tag: [d["meta"]["tps"] for d in runs(lambda t: old(2, t), tag)]
m = {t: med(tp(t)) for t in ("s2-c1-g", "s2-c8-g", "s2-c1-s", "s2-c8-s", "b1-s2-c1-g", "b1-s2-c1-s")}
mo = {t: med(tpo(t)) for t in m}
pgm = med([d["meta"]["tps"] for d in runs(new, "pg-s2-b1")])
PGB = new("pg-s2-b1")
KB = new("b1-s2-c1-g")
OB = old(2, "b1-s2-c1-g")


def stm_table():
    rows = ["| statement | KDS `b1-s2-c1-g` p50 | PostgreSQL p50 | KDS/PG (below 1.00x = KDS faster) | KDS mean | PG mean |", "|---|---|---|---|---|---|"]
    for n in ["booking", "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert", "operation-update", "org-update", "commit"]:
        a, b = ph(KB, n), ph(PGB, n)
        rows.append(f"| {n} | {a['p50_us']:,.1f} µs | {b['p50_us']:,.1f} µs | {a['p50_us'] / b['p50_us']:.2f}x | {a['mean_us']:,.1f} µs | {b['mean_us']:,.1f} µs |")
    return "\n".join(rows) + "\n"


txt = f"""# Scenario 2 (freight) at `{DESC}`

**One line.** The eight-statement freight booking runs as fast as it did at v3.1.0 or a little faster, and the one place it moved
beyond its spreads is where the commit dominates. Eight contended bookers at `cores = 8` run {f1(m['s2-c8-g'], ' tps')} under `group` and
{f1(m['s2-c8-s'], ' tps')} under `strict` (medians of three, spreads 0.5 % and 3.4 %), against {f1(mo['s2-c8-g'], ' tps')} and {f1(mo['s2-c8-s'], ' tps')} at v3.1.0
(cross-version, {delta(m['s2-c8-g'], mo['s2-c8-g'])} and {delta(m['s2-c8-s'], mo['s2-c8-s'])}), and `cores = 1` is {f1(m['s2-c1-g'], ' tps')} (`group`) and {f1(m['s2-c1-s'], ' tps')} (`strict`),
inside the noise ({delta(m['s2-c1-g'], mo['s2-c1-g'])}, {delta(m['s2-c1-s'], mo['s2-c1-s'])}). **One durable booker equals PostgreSQL 18.6, re-measured in the same window**:
{f1(m['b1-s2-c1-g'], ' tps')} (`group`) and {f1(m['b1-s2-c1-s'], ' tps')} (`strict`) against {f1(pgm, ' tps')} ({ratio(m['b1-s2-c1-g'], pgm)} and {ratio(m['b1-s2-c1-s'], pgm)}); the commit is
{ph(KB, 'commit')['mean_us'] * ph(KB, 'commit')['ops'] / (ph(KB, 'booking')['mean_us'] * ph(KB, 'booking')['ops']) * 100:.0f} % of a booking on both engines and its mean is the same to the microsecond
({ph(KB, 'commit')['mean_us']:,.1f} µs against {ph(PGB, 'commit')['mean_us']:,.1f} µs). The booker sweep at `cores = 8` is written up this time (section 7): `group` climbs from 394 tps with one booker to
{f0(med([d['meta']['tps'] for d in runs(new, 'sat-b32-s2-c8-g')]), ' tps')} with 32, `strict` to {f0(med([d['meta']['tps'] for d in runs(new, 'sat-b32-s2-c8-s')]), ' tps')}, and the invariant check still fails under the default isolation and passes 400 of 400 under `repeatable-read`.

Driver: `tools/scenario2_freight.py`, unmodified, `--schema-only`, then `--load-only`, then the run on the same server, with the arguments and cell order of the v3.1.0
run (`run_all.sh` in the archive). Measured by `ck-tester`. Comparator (cross-version): `bench/v3.1.0/results-scenario2-freight-v3.1.0-30-gcd7c7c4e.md`, re-derived from its
archived JSON. PostgreSQL 18.6: `tools/pg_scenario2_freight.py` with `synchronous_commit = on`, **fresh, three runs**.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; the four core cells x 3 runs {win(core)[0]} to {win(core)[1]} UTC (interleaved with the isolation probe, the booker sweep and the single-booker cells in the order of section 2); isolation probe, booker sweep `x-b*` {win(rrt + xb)[0]} to {win(rrt + xb)[1]} UTC; single-booker cells {win(b1t)[0]} to {win(b1t)[1]} UTC; saturation cells {win(sat)[0]} to {win(sat)[1]} UTC; PostgreSQL {win(pgt)[0]} to {win(pgt)[1]} UTC |
{COMMON.replace('results-scenario{N}-*', 'results-scenario2-freight')}| Driver | **unmodified** `tools/scenario2_freight.py`, `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`; exit 0 in every cell |

## 2. What was run

| cells | cores | durability | bookers | runs | extra |
|---|---|---|---|---|---|
| `s2-c8-s`, `s2-c1-s`, `s2-c1-g`, `s2-c8-g` (+ `r2-`, `r3-`) | 8 / 1 / 1 / 8 | strict / strict / group / group | 8 | 3 each | - |
| `rr-s2-c1-g`, `rr-s2-c8-g` | 1 / 8 | group | 8 | 1 each | `--isolation repeatable-read` |
| `x-b{{1,2,4,16}}-s2-c8-{{g,s}}` | 8 | group and strict | 1, 2, 4, 16 | 1 each | `--bookers N` (the booker sweep) |
| `sat-b{{16,32}}-s2-c8-{{g,s}}` | 8 | group and strict | 16, 32 | 1 each | the same plus a 1-second whole-host CPU sampler |
| `b1-s2-c1-g`, `b1-s2-c1-s` (+ `r2-`, `r3-`) | 1 | group / strict | 1 | 3 each | `--bookers 1`, the PostgreSQL twin's shape |
| `pg-s2-b1` (+ `r2-`, `r3-`) | - | `synchronous_commit = on` | 1 | 3 | `pg_scenario2_freight.py` with the loaded sizes below |

Each cell: a fresh server and data file, `--schema-only`, `--load-only`, then the measured run on the same file. A booking is the driver's eight statements in
one transaction (cargo, credit, capacity and recipe reads; freight and charge inserts; operation and organization updates) and a commit; 3,000 bookings
per cell, with `--verify 100` invariant checks (400 checks) after. **The row-set axis is not swept** (rule 9): the loaded sizes are fixed by the arguments
(300 organizations, 30 ships, 300 operations, 4,000 cargos; 3,000 freights and about 17,000 charges accumulate) and the v3.1.0 plan it repeats never swept them. The
reads are pk descents and one 93-row recipe scan, which do not scale with the booking count; the writes grow `freights` and `charges` by 3,000 and 17,000 rows
inside every cell, which is a 17,000-row sweep of the insert path in one run, but there is no 200 / 1K / 10K axis and none is claimed.

Cut: nothing from the v3.1.0 plan; the booker sweep and the saturation cells that v3.1.0 left unwritten ran with the same arguments and are in section 7.

## 3. Throughput

Eight contended bookers. Each cell ran three times; the spread beside a median is this run's noise floor, and a delta smaller than the larger of the two spreads is not a finding.

{T('s2_tables.py', 'tp')}
| ratio of medians | now | v3.1.0 (cross-version) |
|---|---|---|
| `cores = 8` over `cores = 1`, `group` | {m['s2-c8-g'] / m['s2-c1-g']:.2f}x | {mo['s2-c8-g'] / mo['s2-c1-g']:.2f}x |
| `cores = 8` over `cores = 1`, `strict` | {m['s2-c8-s'] / m['s2-c1-s']:.2f}x | {mo['s2-c8-s'] / mo['s2-c1-s']:.2f}x |
| `group` over `strict`, `cores = 1` | {m['s2-c1-g'] / m['s2-c1-s']:.2f}x | {mo['s2-c1-g'] / mo['s2-c1-s']:.2f}x |
| `group` over `strict`, `cores = 8` | {m['s2-c8-g'] / m['s2-c8-s']:.2f}x | {mo['s2-c8-g'] / mo['s2-c8-s']:.2f}x |

Every cell committed its 3,000 bookings. The two eight-core cells are above both spreads (`group` {delta(m['s2-c8-g'], mo['s2-c8-g'])} against spreads 0.5 % and 1.5 %; `strict` {delta(m['s2-c8-s'], mo['s2-c8-s'])}
against 3.4 % and 3.2 %), small but real; the two one-core cells are inside the noise. Section 5 shows where the eight-core gain went: the commit.

The single-booker cells (the matched shape for PostgreSQL, section 9):

{T('s2_tables.py', 'b1')}
## 4. Percentiles

Run 1 of each of the four core cells, every phase of the driver (microseconds; the five percentiles are the driver's):

{T('s2_tables.py', 'pct', 's2-c1-g', 's2-c8-g', 's2-c1-s', 's2-c8-s')}
## 5. Where the time goes

A booking is the sum of its statements, its commit, and what is left (BEGIN, refused attempts, the client and the socket). Each phase's mean times its ops over the
booking's mean times its ops, run 1, now and at v3.1.0 (cross-version):

{T('s2_tables.py', 'shares')}
| Wait | Reading |
|---|---|
| Durability / commit | `commit` is {ph(new('s2-c1-g'), 'commit')['mean_us'] * ph(new('s2-c1-g'), 'commit')['ops'] / (ph(new('s2-c1-g'), 'booking')['mean_us'] * ph(new('s2-c1-g'), 'booking')['ops']) * 100:.1f} % of a booking at `s2-c1-g`, **{ph(new('s2-c8-g'), 'commit')['mean_us'] * ph(new('s2-c8-g'), 'commit')['ops'] / (ph(new('s2-c8-g'), 'booking')['mean_us'] * ph(new('s2-c8-g'), 'booking')['ops']) * 100:.1f} % at `s2-c8-g`** and 50.5 % at `s2-c8-s`; with one booker it is {ph(KB, 'commit')['mean_us'] * ph(KB, 'commit')['ops'] / (ph(KB, 'booking')['mean_us'] * ph(KB, 'booking')['ops']) * 100:.1f} % and its p50 ({ph(KB, 'commit')['p50_us']:,.1f} µs) is the device's sync: PostgreSQL's is {ph(PGB, 'commit')['p50_us']:,.1f} µs. At eight cores the commit, not the statements, bounds the booking |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | one core: 29.5 % for `charge-insert` alone (about 5.6 charges per booking) and 8.1 / 4.5 / 4.4 % for the three others; eight cores 12.2 / 2.7 / 2.4 / 2.3 % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | 7.0 to 8.3 % each at one core, 3.5 to 7.8 % at eight; `recipe-read` is the FilterScan and the largest |
| Lock or conflict wait | zero client-visible conflicts at every `cores = 1` cell and at `c8-g` 1 / 0 / 0; **54 / 42 / 39 at `c8-s`** (all on `operations` and `organizations` rows: `TXN_CONFLICT retryable=1`), all retried by the driver (v3.1.0: 90 / 63 / 45). Their cost sits in the unattributed column: 13.1 % at `s2-c8-s` against 4.4 % at `s2-c8-g` |
| Client and socket round trip | the unattributed column at the cells with no conflicts, 4.4 to 7.3 % |

Outcomes, conflicts and the invariant check, every cell of the matrix, with the conflict lines the server logged (`TXN_CONFLICT`; the second number is the btree
re-descent refusal `gave up after 5 attempts`, which a statement that wrote nothing re-runs under BA-S14 and which **reached the driver in no cell**):

{T('s2_tables.py', 'out')}
## 6. Correctness: what the invariant failures are

The `--verify 100` pass checks four invariants (booked volume against the sum of freights, ship capacity, outstanding credit against recomputed charges, charge rows against
rules) 400 times while the bookers run. Under the **default isolation** with more than one booker it fails 10 to 35 of 400 in every cell, `cores = 1` included
where no conflict is ever reported; with **one booker it fails 0 of 400 in all six cells**; under **`--isolation repeatable-read` it passes 400 of 400** at both core counts. The
checks read several statements' worth of state across the other bookers' commits, and read committed lets a commit land between two of them: the failures are consistent with the isolation
level showing, not with lost writes, because the same workload passes every check once it reads from one snapshot. This is as it was at v3.1.0 (13 to 34 of 400) and no evidence here says otherwise.

{T('s2_tables.py', 'iso')}
The repeatable-read probe costs no throughput (651.2 tps at `cores = 1`, 1,932.3 tps at `cores = 8` against 649.8 and 1,965.0 under the default), and it turns the failed checks into
client-visible conflicts (119 and 62 `conflicted` replies), retried by the driver: a refused write instead of a wrong read.

## 7. The booker sweep at `cores = 8`, written up this time

Bookers 1, 2, 4, 8, 16 (the v3.1.0 plan) and 32 (the saturation cells), `cores = 8`, one run each except 8 (three). `--bookings 3000` in every cell, so every cell does the same work
(equal work, not equal time). The v3.1.0 column is its single `x-b*` run (cross-version, one run, so a delta below the 3 % to 12 % single-run spread is not read; only the
`cores = 8, 8 bookers` pair has a measured spread):

{T('s2_tables.py', 'bk')}
**Throughput climbs with bookers on both durability rungs and the gap between them opens**: `group` {f0(med(tp('x-b1-s2-c8-g')), ' tps')} to
{f0(med(tp('sat-b32-s2-c8-g')), ' tps')} (9.6x for 32x the clients) and `strict` {f0(med(tp('x-b1-s2-c8-s')), ' tps')} to {f0(med(tp('sat-b32-s2-c8-s')), ' tps')} (7.6x), with `group` over `strict` going 0.98x at one booker, 1.24x
at two, 1.20x at four, 1.17x at eight, 1.28x at 16 and 1.23x at 32. The one-booker rows are the fsync-bound pair: their cross-version deltas (+9.0 % and +23.9 %; v3.1.0's `strict` single run is the slow one) are the device's, as section 8 shows for the matched `cores = 1` cells. **A single booker is not slower under `strict`**: it waits for its own sync either way. From two bookers on `strict` pays for the
staged commit that parks and publishes after its sync, plus the retried conflicts (54 at 8 bookers, 141 at 16, 351 at 32; zero up to two bookers and 11 at four).

What bounds it. The 1-second whole-host sampler around the saturation cells:

{T('cpu.py')}
No limit of the engine was reached in these cells: the host never exceeded 67 % busy (peak of any one-second sample), steal was zero, and each cell lasts under two seconds of measured booking time
(3,000 bookings at 3,000+ tps), so a one-second sampler sees two or three samples of it. The driver is Python threads in one process, so the client side (the GIL) is a candidate bound that this run
did not rule out, and the 32-booker invariant failures (69 and 75 of 400, against 13 at eight) say the contention, not the hardware, is rising. The cells are fixed at 3,000 bookings; a
saturation measurement needs a longer run (`--bookings` larger) and a second client process. **That is the open item of this scenario**: it has not found the ceiling.

## 8. Delta against the previous run of this shape (cross-version)

The comparator is `bench/v3.1.0/results-scenario2-freight-v3.1.0-30-gcd7c7c4e.md` (three runs per matrix cell), 240 commits apart. Throughput deltas: section 3 and 7. Statement level, run 1
against run 1, `cores = 8`, `strict` and the single-booker cell:

| cell | phase | p0 now | p0 v3.1.0 | p50 now | p50 v3.1.0 | p50 now/then | mean now | mean v3.1.0 |
|---|---|---|---|---|---|---|---|---|
""" + "\n".join(
    f"| `{tag}` | {n} | {ph(new(tag), n)['p0_us']:,.1f} µs | {ph(old(2, tag), n)['p0_us']:,.1f} µs | {ph(new(tag), n)['p50_us']:,.1f} µs | {ph(old(2, tag), n)['p50_us']:,.1f} µs | "
    f"{ph(new(tag), n)['p50_us'] / ph(old(2, tag), n)['p50_us']:.2f}x | {ph(new(tag), n)['mean_us']:,.1f} µs | {ph(old(2, tag), n)['mean_us']:,.1f} µs |"
    for tag in ("s2-c1-g", "s2-c8-g", "s2-c8-s", "b1-s2-c1-g")
    for n in ("booking", "commit", "freight-insert", "charge-insert", "operation-update", "cargo-lookup", "recipe-read")) + f"""

The statements did not get faster, and per statement they are slightly slower: every statement p50 in this table (the 20 rows other than `booking` and `commit`) is 1.01x to 1.11x of v3.1.0's (median about 1.04x), the same sign in all 20, and the `pk` control of scenario 4 reads 73.4 µs
against 70.1 µs (+4.7 %). Whether that 3 % to 7 % is the engine (240 commits) or the host (the same VM, a week later) is **not separated by this run**. The booking still comes out level or faster because the **commit** fell:
the single-booker row's movement ({delta(m['b1-s2-c1-g'], mo['b1-s2-c1-g'])}) is the commit, whose mean fell from {ph(OB, 'commit')['mean_us']:,.1f} to {ph(KB, 'commit')['mean_us']:,.1f} µs and p0 from {ph(OB, 'commit')['p0_us']:,.1f} to {ph(KB, 'commit')['p0_us']:,.1f} µs. At `cores = 8` the commit
mean also fell (2,271 against 2,498 µs `group`, 2,389 against 2,503 µs `strict`), which is where the {delta(m['s2-c8-g'], mo['s2-c8-g'])} and {delta(m['s2-c8-s'], mo['s2-c8-s'])} come from.
PostgreSQL's commit mean in this window is {ph(PGB, 'commit')['mean_us']:,.1f} µs, equal to KDS's, so the device's sync was about 18 % cheaper this week
than at v3.1.0, and it carried both engines. A single-booker delta against v3.1.0 is the device, not the engine (the same reading the v3.1.0 file made of its own 62 % commit share).

## 9. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor and answers "is the engine in the right league" and nothing finer. The twin drives **one** booking connection with its manifest reporter interleaved, so the matched KDS
cell is `--bookers 1`; the eight-booker cells have no PostgreSQL counterpart. Three runs each, same window, PostgreSQL after the KDS cells:

{T('s2_tables.py', 'pg')}
Statement level, run 1 of each (the write statements are the engine's strength, the recipe scan its weakness):

{stm_table()}
**KDS and PostgreSQL are the same machine on one durable client**: the booking mean is {ph(KB, 'booking')['mean_us']:,.0f} µs against {ph(PGB, 'booking')['mean_us']:,.0f} µs. The four write statements run at 0.73x to 0.84x of PostgreSQL's p50 (a
KDS write is cheaper than a PostgreSQL write by a quarter) and `recipe-read`, a 93-row FilterScan, runs at 1.94x (KDS slower), which is the per-row result cost scenario 1 isolates. The two roughly
cancel in the booking, and the commit is the same fsync.

## 10. Noise floor, contamination and host

The 3-run spreads of section 3 are the noise floor: 0.5 % to 4.1 % at the matrix cells and 2.3 % to 3.0 % at the single-booker cells; the PostgreSQL cells' is 3.0 %. The booker sweep's
single runs have no spread of their own; their floor is the 8-booker pair's 0.5 % (`group`) to 3.4 % (`strict`), and the repeat is the 16-booker pair, run twice (`x-b16` and `sat-b16`): 3,095.1 against 3,057.5 tps under `group` (1.2 %) and 2,417.2 against 2,382.1 tps under `strict` (1.5 %). A single-run delta in the sweep below about 3 % is therefore not read; the cross-version deltas of 12.2 % (16 `group`) and 5.3 % (16 `strict`) are single against single.

**Contamination.** No KDS cell was discarded (140 validity evaluations, none contaminated; `contaminated/` in the run archive holds only the failed PostgreSQL scenario-3 attempt, which is not scenario 2's).

Per-cell host stamps:

{T('host.py', *core, *rrt, *xb, *b1t, *sat, *pgt)}
## 11. What the run says about the engine

1. **The commit is the whole story at both ends.** One booker: commit 55.6 % of the booking, identical to PostgreSQL's to the microsecond; eight bookers at `cores = 8`: 57.2 % under `group`, 50.5 % under
   `strict`. The statements around it (reads at 66 to 87 µs, writes at 45 to 62 µs p50) are not what a booking waits for. A faster engine cannot show in this scenario without a faster device or a cheaper group commit.
2. **Eight cores do their job on commit-bound work**: `cores = 8` over `cores = 1` is {m['s2-c8-g'] / m['s2-c1-g']:.2f}x under `group` and {m['s2-c8-s'] / m['s2-c1-s']:.2f}x under `strict` for 8 bookers, and `group` over `strict` is {m['s2-c8-g'] / m['s2-c8-s']:.2f}x, the staged
   `strict` commit costing little over `group` once clients overlap (BA-S7).
3. **The isolation level, not the engine, is what the invariant check measures** (section 6): 400 of 400 under repeatable-read at no throughput cost.
4. **This run did not find the engine's limit under booking load** (section 7): 3,768 tps at 32 bookers with the host at most 67 % busy. Finding it needs a longer cell and a client that is not one Python process; that is the
   next measurement, not a conclusion.

## 12. Archive

`bench/v3.3.0/archive/scenario2-{DESC}/`: per-cell JSON (KDS and PostgreSQL), driver stdout, schema and load logs, server configs, the 1-second CPU samples of the saturation cells, the server logs. What the four
scenarios share is in `bench/v3.3.0/archive/run-{DESC}/`.
"""
write("results-scenario2-freight", txt)
