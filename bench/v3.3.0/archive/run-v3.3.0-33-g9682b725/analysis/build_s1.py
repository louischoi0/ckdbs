import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from doc import *
from an import *
import host
from s1_tables import SHAPES, sweep, cellruns


def win(tags):
    s = [host.STARTS[t] for t in tags if t in host.STARTS]
    e = [host.ENDS[t][0] for t in tags if t in host.ENDS]
    return min(s)[11:19], max(e)[11:19]


kt = [p + c for p in PFX for c in ("s1-c1-g", "s1-c8-g", "s1-c1-s")]
st = [p + f"s1s-{n}-c1-g" for p in PFX for n in ("n252", "n1008", "n10080")]
pt = [p + c for p in PFX for c in ("pg-s1", "pg-s1s-n252", "pg-s1s-n1008", "pg-s1s-n10080")]
plt = [f"pl-s1-{o}-r{i}" for i in (1, 2, 3) for o in ("planned", "written")]

K = cellruns("s1-c1-g")
O = cellruns("s1b-c1-g", True)
PGR = runs(new, "pg-s1")


def w(d, sh, key="warm"):
    return sweep(d)[sh][key]["qps"]


def mw(rr, sh, key="warm"):
    return med([w(d, sh, key) for d in rr if sweep(d).get(sh) and sweep(d)[sh].get(key)])


bl = ph(K[0], "read-bar-lookup")
sync_cost = 1e6 / med([x["qps"] for d in K for x in d["meta"]["write_sweep"] if x["batch"] == 1]) - 1e6 / med([x["qps"] for d in K for x in d["meta"]["write_sweep"] if x["batch"] == 1000])
a1 = med([x["qps"] for d in K for x in d["meta"]["write_sweep"] if x["batch"] == 1])
a1000 = med([x["qps"] for d in K for x in d["meta"]["write_sweep"] if x["batch"] == 1000])
br = ph(K[0], "read-bar-range")
sh_ = ph(K[0], "read-symbol-history")
pbr = ph(PGR[0], "read-bar-range")
psh = ph(PGR[0], "read-symbol-history")
pbl = ph(PGR[0], "read-bar-lookup")

CO_RATIO = ratio(med([ph(new(f'pl-s1-planned-r{i}'), 'compare-one')['qps'] for i in (1, 2, 3)]), med([ph(new(f'pl-s1-written-r{i}'), 'compare-one')['qps'] for i in (1, 2, 3)]))
VOK = sum(1 for t in kt + st if new(t) and new(t)['meta']['verify_problems'] == [])
VN = len([t for t in kt + st if new(t)])
txt = f"""# Scenario 1 (backtest) at `{DESC}`

**One line.** `tools/scenario1_backtest.py`, **unmodified**, now runs end to end: the `HEAP` relations of its schema (`daily_stats`,
`model_results`, `write_probe`) are btrees with a hidden tuple key since BZ, so the SUS-1 refusal that stopped it at v3.1.0 is gone and
the BTREE copy that run needed is not used. On 60,480 bars an unindexed one-session equality (`day-slice`) runs
{f1(mw(K, 'day-slice'), ' qps')} and a Cabin on its filter column lifts it {mw(K, 'day-slice', 'cabin') / mw(K, 'day-slice'):.1f}x to
{f1(mw(K, 'day-slice', 'cabin'), ' qps')}; the backtest's three-relation join runs {f1(mw(K, 'cross-join'), ' qps')} cold-or-warm and
{f1(mw(K, 'cross-join', 'cabin'), ' qps')} with the Cabin; a durable autocommit insert is {f0(a1, ' rows/s')} and a 1,000-row batch
{f0(a1000, ' rows/s')}. Against the v3.1.0 BTREE-copy numbers (cross-version) the single-connection read shapes moved by less than
their own spreads, except `cores = 8`, whose one-connection reads gained 28 to 31 % because they no longer pay the 22 % penalty they paid there
(section 9). Against PostgreSQL 18.6, **re-measured in the same window**, KDS is ahead on the non-index equality scan
({ratio(mw(K, 'day-slice'), mw(PGR, 'day-slice'))}), the join ({ratio(mw(K, 'cross-join'), mw(PGR, 'cross-join'))}) and the pk-anchored join
({ratio(mw(K, 'point-join'), mw(PGR, 'point-join'))}), and behind on every shape that returns many rows or folds a whole relation
(`bar-range` {ratio(mw(K, 'bar-range'), mw(PGR, 'bar-range'))}, `symbol-history` {ratio(mw(K, 'symbol-history'), mw(PGR, 'symbol-history'))},
`model-join` {ratio(mw(K, 'model-join'), mw(PGR, 'model-join'))}). **The engine's join planner (CB/CC) reorders two of the backtest's joins and it does not move them**: section 6.

Driver: `tools/scenario1_backtest.py`, unmodified, with `--seed 1 --bars-clustered btree --verify --sync --analyze`, driver
documentation: `bench/docs/` is closed and the tool's own `--help` carries the flags (`bench/README.md`). Measured by `ck-tester`.
Comparator for the KDS deltas (cross-version): `bench/v3.1.0/results-scenario1-backtest-v3.1.0-30-gcd7c7c4e.md`, whose cells ran the
BTREE-only **copy** of the driver (three `HEAP` keywords changed); this run is the first with the driver as shipped, so the write-side
phases compare a hidden-key btree against an explicit one, which is the same structure. PostgreSQL 18.6: `tools/pg_scenario1_backtest.py`,
**fresh**, three runs at the driver's defaults and three at each sweep size.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; KDS matrix cells {win(kt)[0]} to {win(kt)[1]} UTC (nine cells); size sweep {win(st)[0]} to {win(st)[1]} UTC (nine cells); PostgreSQL cells {win(pt)[0]} to {win(pt)[1]} UTC (twelve cells); join-order probe {win(plt)[0]} to {win(plt)[1]} UTC (six cells). Per-cell windows: section 10 |
{COMMON.replace('results-scenario{N}-*', 'results-scenario1-backtest')}| Driver | **unmodified** `tools/scenario1_backtest.py`; exit code 0 and no verify problem in every KDS cell (`verify_problems` empty in {VOK} of {VN}) |

## 2. What was run

The v3.1.0 plan, every cell, with the driver as shipped:

| cell | cores | durability | runs | arguments |
|---|---|---|---|---|
| `s1-c1-g`, `r2-`, `r3-` | 1 | group | 3 | `--seed 1 --bars-clustered btree --verify --sync --analyze` (60,480 bars) |
| `s1-c8-g`, `r2-`, `r3-` | 8 | group | 3 | same |
| `s1-c1-s`, `r2-`, `r3-` | 1 | strict | 3 | same |
| `s1s-n252-c1-g` | 1 | group | 3 | same plus `--years 1 --symbols 1 --exchanges 1` (252 bars) |
| `s1s-n1008-c1-g` | 1 | group | 3 | same plus `--years 1 --symbols 4 --exchanges 2` (1,008 bars) |
| `s1s-n10080-c1-g` | 1 | group | 3 | same plus `--years 5 --symbols 8 --exchanges 2` (10,080 bars) |
| `pg-s1`, `r2-`, `r3-` | - | `synchronous_commit = on` | 3 | `pg_scenario1_backtest.py --seed 1 --verify` (60,480 bars) |
| `pg-s1s-n252` / `n1008` / `n10080` | - | same | 3 each | same plus the sweep's `--years/--symbols/--exchanges` |
| `pl-s1-planned-r1..3`, `pl-s1-written-r1..3` | 1 | group | 3 each | the `s1-c1-g` arguments, the server started with `join_order = written` for the second arm, ANALYZE replies dumped |

**Row-count axis (rule 9).** Bars = 252 sessions per year x years x symbols; `daily_stats` has one row per bar and `daily_bars` one per bar.
The driver's flags map to 252, 1,008, 10,080 and (default `--years 30 --symbols 8`) 60,480 bars, 128,536 rows over seven relations at the default size.
The sweep cells run the same driver at the smaller sizes and are shown in section 7. A cell is one fresh server and a fresh data file,
deleted afterwards. The size is a row label in every sweep table.

Cut: nothing. The operator lifted the budget for this run, so the size sweep that the v3.1.0 run was stopped in (two runs at 252 and 1,008 bars, one at 10,080)
ran with three runs at every size here.

## 3. Results

The QPS sweep (the driver's own controlled matrix, 100 statements per cell, 8 distinct arguments in the warm cases): median of three
runs per cell. `cold` = every argument seen for the first time, `warm` = the same few arguments cycled, `cabin` = warm with a Cabin
declared on the filter column and observed. Qps is the driver's own figure. The v3.1.0 columns are the BTREE-copy cells of
`bench/v3.1.0/results-scenario1-backtest-v3.1.0-30-gcd7c7c4e.md`, re-derived from its archived JSON (three runs), **cross-version**.

{NOISE_NOTE}

{T('s1_tables.py', 'mat')}
**A Cabin lifts the two scan shapes by an order of magnitude and nothing else.** `day-slice` {mw(K, 'day-slice', 'cabin') / mw(K, 'day-slice'):.1f}x and
`cross-join` {mw(K, 'cross-join', 'cabin') / mw(K, 'cross-join'):.1f}x at `cores = 1`; `symbol-history` returns 7,560 rows and gains nothing, and `model-join` is a
probe-per-row join over 359 rows (1.01x). Cold and warm agree within 3 % on the walk shapes: the data is in the pool.

**Writes: the durability wait is the cost of a row.** Rows per `BEGIN`/`COMMIT`; median of three runs with the spread in brackets:

{T('s1_tables.py', 'writes')}
**Concurrency: KDS scales with cores, not with connections.** One join (`cross-join`) issued over 1, 2, 4 and 8 connections:

{T('s1_tables.py', 'conc')}
At `cores = 1` the join does not scale with connections ({med([x['qps'] for d in K for x in d['meta']['concurrency_sweep'] if x['connections'] == 8]) / med([x['qps'] for d in K for x in d['meta']['concurrency_sweep'] if x['connections'] == 1]):.2f}x at eight): one core's statements run on one thread, so a rising
aggregate is only round-trip overlap. At `cores = 8` it reaches
{med([x['qps'] for d in cellruns('s1-c8-g') for x in d['meta']['concurrency_sweep'] if x['connections'] == 8]) / med([x['qps'] for d in cellruns('s1-c8-g') for x in d['meta']['concurrency_sweep'] if x['connections'] == 1]):.2f}x at eight connections, with a 22 % to 42 % run-to-run spread at four and eight connections: the sessions are placed on cores as they arrive
(BL-S3) and the eight clients share eight CPUs with the reactors.

Every phase of the driver, median over the three `c1-g` runs, against v3.1.0 (the load phases are single-connection insert loops, the folds are
whole-relation reads):

{T('s1_tables.py', 'pm')}
## 4. Percentiles

Every phase of run 1 of each default-size cell, in the driver's own order (the unit is the statement's round trip; `ops` is the count in the
cell). The five percentiles are the driver's; `max` and `errors` are beside them.

{T('s1_tables.py', 'phases')}
## 5. Where the time goes

The matrix is serial reads on one connection, so a statement is a fixed cost plus a per-row cost, and a write is a statement plus the durability wait. What
the cells measure of each, `c1-g`:

| Component | This run | How derived |
|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **{bl['mean_us']:,.1f} µs** mean, p0 {bl['p0_us']:,.1f} µs | `read-bar-lookup`, run 1, 200 ops; PostgreSQL's same phase: {pbl['mean_us']:,.1f} µs mean, p0 {pbl['p0_us']:,.1f} µs |
| Per-row cost of a returned row | **{br['mean_us'] / 200:,.1f} µs per row** (`read-bar-range`, 200 rows, {br['mean_us']:,.0f} µs) and {sh_['mean_us'] / 7560:,.1f} µs per row (`read-symbol-history`, 7,560 rows, {sh_['mean_us'] / 1000:,.1f} ms) | PostgreSQL: {pbr['mean_us'] / 200:,.1f} µs and {psh['mean_us'] / 7560:,.1f} µs per row |
| Per-row cost of an examined, unreturned row | about {(ph(K[0], 'read-day-slice')['mean_us'] - bl['mean_us']) / 60480 * 1000:,.0f} ns | `read-day-slice` examines all 60,480 `daily_stats` rows and returns 8 (the ANALYZE table in the stdout of each cell): (mean - fixed) / 60,480 |
| Durability / commit | **{sync_cost:,.0f} µs per commit** | autocommit insert {1e6 / a1:,.0f} µs per row, a 1,000-row batch {1e6 / a1000:,.1f} µs per row; the difference is one durable commit. PostgreSQL's: {1e6 / med([x['qps'] for d in PGR for x in d['meta']['write_sweep'] if x['batch'] == 1]) - 1e6 / med([x['qps'] for d in PGR for x in d['meta']['write_sweep'] if x['batch'] == 1000]):,.0f} µs |
| Lock or conflict wait | none | one connection (the concurrency sweep reads only) |
| Write statement | {1e6 / a1000:,.1f} µs per row at batch 1,000 | the insert path with the sync amortised away |

The fixed round trip is the one place PostgreSQL is cheaper per statement ({ratio(bl['mean_us'], pbl['mean_us'])} of its mean for the same phase, KDS slower by that factor);
the per-row cost of a returned row is about twice PostgreSQL's, which is why every shape that returns many rows sits at 0.43x to 0.56x of it. The
engine is cheap where it examines and discards (a walked unindexed equality beats PostgreSQL's seq scan by 1.4x) and dear where it must build and send
a row: a result row costs {br['mean_us'] / 200:,.1f} to {sh_['mean_us'] / 7560:,.1f} µs against PostgreSQL's {pbr['mean_us'] / 200:,.1f} to {psh['mean_us'] / 7560:,.1f} µs.

## 6. Joins and the plan the engine chose

The engine chooses an inner-join block's order since CB-S2 and prices it from the join planner's statistics since CC. Scenario 1 has three join
statements: `backtest-read` (three relations), `compare-all` and `compare-one` / `model-join` (two). ANALYZE of each, on a server in this run's
state (`pl-s1-planned-r1`, the full reply is in `bench/v3.3.0/archive/scenario1-{DESC}/plans/`):

| statement | plan (default `join_order = planned`) | `reordered` | examined | with `join_order = written` | examined |
|---|---|---|---|---|---|
| `backtest-read` (`cross-join`) | `FilterScan daily_stats` -> `Probe daily_bars` -> `Probe symbols` (order = memo, est 8 / 8 / 8) | no | 60,496 | same chain | 60,496 |
| `compare-all` | `Scan models` (8 rows) -> `Scan model_results`, **`build on=col1`, `inner_built=1`, `build_rows=2,872`, `build_probes=7`** | **yes** (`reordered=1`, switch 0.19) | 5,393 | `Scan model_results` -> `Probe models` per row (2,872 opens, 2,898 pages) | 5,744 |
| `compare-one` (`model-join`) | `Lookup models key=1` -> `FilterScan model_results` (est 1 / 362) | **yes** (`reordered=1`, switch 0.84) | 2,873 | `FilterScan model_results` -> `Probe models` (359 opens, `memo_hits=358`) | 3,231 |

The planner does what it was built to do on the two joins whose written order is the dear one, and **it moves their throughput by 1 % to 6 %, which is inside the noise**.
Three runs each of the default and of the `join_order = written` arm (same binary, same cell shape, same window):

{T('plan_ab.py').split('Scenario 3,')[0]}
`compare-one` is the only phase that reads above its spreads (2.7 % and 3.6 %), by 6 % ({CO_RATIO}), and the `model-join` sweep row
moves 2 % with spreads of 4 %. The join is a 359-row result out of 2,872 examined rows; its time is the walk of `model_results` (26 pages) and the
round trip, not the join order. The `point-join` rows (0.68x to 0.70x for the planned arm) are the 45 % bimodal row of section 3 in a different mood
in each arm (spreads 45 % and 32 %), not a finding. **Why `model-join` stays at 0.56x of PostgreSQL**: both engines examine the 2,872 rows of
`model_results` for one `model_id`; PostgreSQL's 1,092 qps against KDS's 615 qps is the per-row result cost of section 5 (359 rows returned),
not a plan difference: the KDS plan is the one the planner picked, and the alternative order is the same speed within 6 %.

## 7. The size sweep (rule 9)

`c1-g`, 252 / 1,008 / 10,080 bars (60,480 is section 3), three runs each; v3.1.0 columns are its sweep, which had two runs at 252 and 1,008 bars and one at
10,080 (cross-version, fewer runs, so a delta of 10 % or less is not read):

{T('s1_tables.py', 'sizes')}
**Fixed costs and per-row costs separate cleanly.** The pk shapes are flat: `bar-lookup` 12,863 to 13,185 qps across 252, 1,008 and 10,080 bars (11,264 qps at 60,480 with a 13 % spread) and `point-join`
12,631 to 12,918 qps across all four sizes, as a descent should be. The scan shapes scale with rows: `day-slice` goes 10,916 -> 7,141 -> 2,432 -> 591 qps from
252 to 60,480 bars, and the Cabin makes it **flat** (11,684 -> 8,708 -> 6,550 -> 6,558 qps): the Cabin's lift is 1.1x at 252 bars and 11.1x at 60,480,
so a Cabin on a relation that fits in one or two pages buys nothing (the walk is already a round trip).

## 8. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor: it says whether the engine is in the right league and nothing finer. The twin ran three times at the driver's defaults
(`pg-s1`, `r2-pg-s1`, `r3-pg-s1`; the twelve PostgreSQL cells ran {win(pt)[0]} to {win(pt)[1]} UTC, after every KDS cell of the matrix and sweep and never concurrent with one) and three times at each sweep size.
PG's `index built` column is a btree index; KDS's Cabin is the counterpart, and they are different structures. Ratios are KDS `c1-g` (median of three)
over PostgreSQL (median of three), above 1.00x = KDS faster.

{T('s1_pg.py', 'pg')}
Ratios across the sweep sizes, KDS `c1-g` warm over PostgreSQL warm (and indexed):

{T('s1_pg.py', 'pgsizes')}
**The league reading.** KDS is in the same league on pk reads (0.77x to 1.32x of PG, within the round-trip gap), ahead on the unindexed equality scan at 60,480 bars
and on both joins that start from a pk or a scan with probes (1.3x to 2.0x at every size), and **behind by half on result-heavy shapes at every size**
(`bar-range` 0.51x to 0.52x at 252, 1,008 and 10,080 bars, `symbol-history` 0.42x to 0.48x): that ratio does not change with the row count, which marks it as a per-row,
not a fixed, cost. At 252 bars `day-slice` is 0.80x: with one page the walk is the round trip and PG's round trip is 20 % cheaper.

## 9. Delta against the previous run of this shape (cross-version)

The comparator is v3.1.0's BTREE-copy cells (section 3 carries every shape; section 3's last table every phase). 240 commits apart, so nothing here is attributable to one change.
Read against each side's spread:

| measure | now | v3.1.0 | delta | read |
|---|---|---|---|---|
| `c1-g` `day-slice` warm | {f1(mw(K, 'day-slice'), ' qps')} | {f1(mw(O, 'day-slice'), ' qps')} | {delta(mw(K, 'day-slice'), mw(O, 'day-slice'))} | inside the larger spread (7 %); not a finding |
| `c1-g` `cross-join` warm | {f1(mw(K, 'cross-join'), ' qps')} | {f1(mw(O, 'cross-join'), ' qps')} | {delta(mw(K, 'cross-join'), mw(O, 'cross-join'))} | inside the spread (10.5 % at v3.1.0) |
| `c1-g` `model-join` warm | {f1(mw(K, 'model-join'), ' qps')} | {f1(mw(O, 'model-join'), ' qps')} | {delta(mw(K, 'model-join'), mw(O, 'model-join'))} | at the edge of the spreads (6.5 % now, 3.9 % then); the reordered plan of section 6 accounts for 2 % of it at most |
| `c8-g` `day-slice` warm | {f1(mw(cellruns('s1-c8-g'), 'day-slice'), ' qps')} | {f1(mw(cellruns('s1b-c8-g', True), 'day-slice'), ' qps')} | {delta(mw(cellruns('s1-c8-g'), 'day-slice'), mw(cellruns('s1b-c8-g', True), 'day-slice'))} | **a finding**: spreads 2.3 % and 0.5 % |
| `c8-g` `cross-join` warm | {f1(mw(cellruns('s1-c8-g'), 'cross-join'), ' qps')} | {f1(mw(cellruns('s1b-c8-g', True), 'cross-join'), ' qps')} | {delta(mw(cellruns('s1-c8-g'), 'cross-join'), mw(cellruns('s1b-c8-g', True), 'cross-join'))} | **a finding**: spreads 1.2 % and 0.6 % |
| Cabin lifts (`day-slice` / `cross-join`, `c1-g`) | 11.1x / 9.7x | 11.9x / 10.5x | - | the Cabin arms moved -0.2 % and -3.8 % (inside spread) |
| autocommit insert | {f0(a1, ' rows/s')} | 773 rows/s | {delta(a1, 773.3)} | inside the spread (26 % then) |

At v3.1.0 a one-connection read on an eight-core instance ran 22 % slower than on a one-core instance (430 against 551 qps for `day-slice`); in this run the same pair is
554 against 591 qps (6 % slower). The cross-version gain is therefore the eight-core penalty shrinking, not the one-core engine getting faster
(+7.3 % at `cores = 1`, inside the spread). This run does not say which commit removed it. Every rate of the phases the run shares with v3.1.0 (section 3, last table)
moved between -6.1 % and +8.7 % except the phases of 1 to 8 operations (`sync`, `load-exchanges`, `load-symbols`, `load-models`), whose spreads are 12 % to 58 %.

## 10. Noise floor, contamination and host

The noise floor is the 3-run spread of one cell: {sp(spread([mw([d], 'day-slice') for d in K]))} for `day-slice`, {sp(spread([mw([d], 'cross-join') for d in K]))} for `cross-join`, {sp(spread([mw([d], 'model-join') for d in K]))} for `model-join`,
{sp(spread([mw([d], 'bar-lookup') for d in K]))} for `bar-lookup`. Two shapes are bimodal in a run (the sweep's 100 statements alternate between a fast and a slow mode): `point-join`
(spread 2 % to 45 % depending on the cell) and `bar-lookup` under `strict` (28.5 %). Medians of three damp it and the table shows the spread beside it; no delta is
claimed on them.

**Contamination.** No KDS cell was discarded: the validity check (`cellvalid.py`, 140 evaluations over the whole run, one per cell including the other
scenarios') found no build or test process and no foreign process at 50 % CPU or more in any window. One batch of PostgreSQL cells (scenario 3's) failed in 3 to 4 seconds on
`role "ec2-user" does not exist` (the twin's default `--user`); the master and the watchdog were stopped by recorded PID, the six failed attempts were moved to
`contaminated/` in the run archive, the command line got `--user cdkbs`, and the cells were re-run. Scenario 1's PostgreSQL cells were not affected.

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

{T('host.py', *kt, *st, *pt, *plt)}
## 11. What the run says about the engine

1. **BZ removed the cost of running this scenario as designed.** The unmodified driver runs and its write side (heap-shaped tables, now hidden-key btrees) costs the
   same as the BTREE copy did: `result-insert` {f1(med([ph(d, 'result-insert')['qps'] for d in K]), ' qps')} against 749 qps, `load-stats` -2.7 %. A heap-by-design append loses nothing to a btree here because the hidden
   key is issued in order, so every insert is a tail append (`heap-and-tuple.md` §3.1b).
2. **The join planner has nothing to win in this scenario.** It reorders both two-relation joins and they are 1 % to 6 % faster, inside the noise. The reorder is real and the plan numbers show it
   (`compare-all` reads 209 pages instead of 2,898, `compare-one` examines 2,873 rows instead of 3,231), yet `compare-all` runs 57.7 qps against 57.0 qps: the statement returns 2,872 rows
   at about 6 µs each, so its 17 ms is result delivery, and a 14-fold cut in pages read is invisible beside it. The `cross-join` is anchored on a 60,480-row `FilterScan` whichever order is tried.
   CB/CC's payoff would show where the walk, not the row, is the cost; this driver has no such join.
3. **Where KDS loses to PostgreSQL is the row, not the plan**: a returned row costs {br['mean_us'] / 200:,.1f} to {sh_['mean_us'] / 7560:,.1f} µs against {pbr['mean_us'] / 200:,.1f} to {psh['mean_us'] / 7560:,.1f} µs, at every size from 252 to 60,480 bars. That puts the cost in
   result delivery (tuple decode, projection, KWP row-batch encoding) rather than in the examined-row walk. The client is `tools/kwp.py` for KDS and `tools/pg_wire.py` for PostgreSQL, each a Python
   decoder; whether the factor of two lives in the engine or in the two decoders is **not separated by this run**.
4. **A Cabin's value is the walk it replaces**: 1.1x at 252 bars, 11.1x at 60,480; flat in n. The controller-free declared Cabin is the right tool for a filter column
   on a relation beyond a few pages and no tool at all below it.
5. **The eight-core instance is no longer slower than the one-core instance for one connection** (section 9), and it is {ratio(med([x['qps'] for d in cellruns('s1-c8-g') for x in d['meta']['concurrency_sweep'] if x['connections'] == 8]), med([x['qps'] for d in PGR for x in d['meta']['concurrency_sweep'] if x['connections'] == 8]))} PostgreSQL on eight connections where PostgreSQL
   plateaus at {f0(med([x['qps'] for d in PGR for x in d['meta']['concurrency_sweep'] if x['connections'] == 8]), ' qps')}.

## 12. Archive

`bench/v3.3.0/archive/scenario1-{DESC}/`: per-cell JSON (KDS and PostgreSQL), the driver stdout of each cell, the server configs, the join-order probe's ANALYZE replies
(`plans/`). What the four scenarios share is in `bench/v3.3.0/archive/run-{DESC}/`: the run scripts (`lib.sh`, `run_all.sh`, `run_plan.sh`), the timeline, the monitor and
watchdog logs, the binary hash, and the table generators under `analysis/`. No data file is kept.
"""
write("results-scenario1-backtest", txt)
