import sys, subprocess
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from s1 import *
import s1b, abx
from stamp import *

kt = [p + c for p in PFX for c in ("s1-c1-g", "s1-c8-g", "s1-c1-s")]
st = [p + f"s1s-{n}-c1-g" for p in PFX for n in ("n252", "n1008", "n10080")]
abt = [f"ab-{v}{i}-s1-c1-g" for i in (1, 2, 3) for v in ("old", "new")]
plt = [f"pl-s1-{o}-r{i}" for i in (1, 2, 3) for o in ("planned", "written")]
K, KO, PGR = N("s1-c1-g"), Mo(1, "s1-c1-g"), Mo(1, "pg-s1")
BRN, BRM, BRP = (ph(x[0], 'read-bar-range') for x in (K, KO, PGR))
SHN, SHM, SHP = (ph(x[0], 'read-symbol-history') for x in (K, KO, PGR))


def mw(rr, sh, key="warm"):
    return med(sv(rr, sh, key))


def verify_ok():
    ts = [t for t in kt + st + abt + plt if now(t)]
    return sum(1 for t in ts if now(t)["meta"]["verify_problems"] == []), len(ts)


VOK, VN = verify_ok()
AB = {}
for sh in ("bar-range", "symbol-history", "model-join", "day-slice", "cross-join", "bar-lookup"):
    o, n_ = abx.pairs("ab-{v}{i}-s1-c1-g")
    AB[sh] = (med([sweep(d)[sh]["warm"]["qps"] for d in o]), med([sweep(d)[sh]["warm"]["qps"] for d in n_]))
OLDPH, NEWPH = abx.pairs("ab-{v}{i}-s1-c1-g")


def abr(ph_name):
    return med(pv(NEWPH, ph_name)) / med(pv(OLDPH, ph_name))


def plan_tbl():
    r = subprocess.run(["python3", "-I", "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis/plan_ab2.py"], capture_output=True, text=True)
    t = r.stdout
    return t.split("Scenario 3,")[0].split("\n", 2)[2] if "Scenario 3," in t else t


import rcsec
RC = rcsec.section()

txt = f"""# Scenario 1 (backtest) at `{DESC}`

**One line.** Same engine binary as the morning's run, new client decoder: every shape that returns many rows runs 1.7x to 1.9x faster, and nothing else moved
by more than 2 % to 6 % (A/B, section 9). On 60,480 bars a 200-row key range (`bar-range`) runs {f1(mw(K, 'bar-range'), ' qps')} against {f1(mw(KO, 'bar-range'), ' qps')} in the morning,
a 7,560-row symbol history {f1(mw(K, 'symbol-history'), ' qps')} against {f1(mw(KO, 'symbol-history'), ' qps')}, and the 359-row `model-join` {f1(mw(K, 'model-join'), ' qps')} against {f1(mw(KO, 'model-join'), ' qps')}.
Interleaved on the same host with the old client (three A/B pairs) the same three shapes are {AB['bar-range'][1] / AB['bar-range'][0]:.2f}x, {AB['symbol-history'][1] / AB['symbol-history'][0]:.2f}x and {AB['model-join'][1] / AB['model-join'][0]:.2f}x,
and the cost of a returned row falls from 6.9 to 3.6 µs (`bar-range`) and from 9.5 to 4.8 µs (`symbol-history`) against PostgreSQL 18.6's 3.2 and 4.1 µs. That puts the many-row shapes at
{ratio(mw(K, 'bar-range'), mw(PGR, 'bar-range'))} (`bar-range`), {ratio(mw(K, 'symbol-history'), mw(PGR, 'symbol-history'))} (`symbol-history`) and {ratio(mw(K, 'model-join'), mw(PGR, 'model-join'))} (`model-join`) of PostgreSQL's, from 0.49x, 0.43x and 0.56x.
**Two caveats govern the rest of the file.** First, the one-core scan shapes (`day-slice`, `cross-join`, `backtest-read`) read 13 % to 16 % *below* the morning, and the A/B shows the cause is not the client:
the old client reads {f0(AB['day-slice'][0], ' qps')} on `day-slice` in this window, the new {f0(AB['day-slice'][1], ' qps')}, the morning {f0(mw(KO, 'day-slice'), ' qps')} (section 9). Host or placement, not separated.
Second, PostgreSQL is reused from the morning's run, so a KDS/PG ratio on a scan shape carries that drift; the ratios on the many-row shapes (where the client moved the number by a factor of two) do not depend on it in any way that changes a reading.

Driver: `tools/scenario1_backtest.py`, unmodified, `--seed 1 --bars-clustered btree --verify --sync --analyze` (flags in `bench/docs/`). Measured by `ck-tester`.
Comparator (this engine's own last run, **same binary, old client**): `bench/v3.3.0/results-scenario1-backtest-{MD}.md`. PostgreSQL 18.6: the same file's twin runs
(`tools/pg_scenario1_backtest.py`, three runs per cell), **not re-run**: {BUDGET}.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; matrix cells {wtext(kt)} (nine cells); size sweep {wtext(st)} (nine cells); A/B pairs {wtext(abt)} (six cells); join-order probe {wtext(plt)} (six cells). Per-cell windows: section 11 |
{COMMONROWS}| Driver | **unmodified** `tools/scenario1_backtest.py`; exit code 0 and no verify problem in every KDS cell (`verify_problems` empty in {VOK} of {VN}) |
| Unchanged from the morning | {PATHS} |

## 2. What was run

The morning's plan, every cell, with the same repeats (`run_all.sh` in the archive is the morning's, with its PostgreSQL stage removed and the A/B stage added):

| cell | cores | durability | runs | arguments |
|---|---|---|---|---|
| `s1-c1-g`, `r2-`, `r3-` | 1 | group | 3 | `--seed 1 --bars-clustered btree --verify --sync --analyze` (60,480 bars) |
| `s1-c8-g`, `r2-`, `r3-` | 8 | group | 3 | same |
| `s1-c1-s`, `r2-`, `r3-` | 1 | strict | 3 | same |
| `s1s-n252-c1-g`, `s1s-n1008-c1-g`, `s1s-n10080-c1-g` (+ `r2-`, `r3-`) | 1 | group | 3 each | same plus `--years 1 --symbols 1 --exchanges 1` (252 bars), `--years 1 --symbols 4 --exchanges 2` (1,008), `--years 5 --symbols 8 --exchanges 2` (10,080) |
| `ab-old{{1,2,3}}-s1-c1-g`, `ab-new{{1,2,3}}-s1-c1-g` | 1 | group | 3 pairs, interleaved old, new, old, new, old, new | the `s1-c1-g` arguments; `old` = `a40882af^`'s `tools/kwp.py` and `tools/ckdbs_cli.py` from `oldtools/`, `new` = this worktree's |
| `pl-s1-planned-r1..3`, `pl-s1-written-r1..3` | 1 | group | 3 each | the `s1-c1-g` arguments, the second arm's server started with `join_order = written`, ANALYZE replies dumped |
| PostgreSQL | - | - | not run | taken from `bench/v3.3.0/archive/scenario1-{MD}/json/pg-s1*.json` (morning, 04:25 to 04:33 UTC) |

**Row-count axis (rule 9).** Bars = 252 sessions per year x years x symbols; `daily_stats` has one row per bar. The driver's flags map to 252, 1,008, 10,080 and (default `--years 30 --symbols 8`) 60,480 bars.
A returned-row count is fixed per shape: `bar-range` 200 rows, `symbol-history` one symbol's sessions (252, 252, 1,260 and 7,560 rows at the four sizes), `model-join` 359, `day-slice` 8.
A cell is one fresh server and a fresh data file, deleted afterwards.

Cut: nothing from the morning's KDS plan. The morning's three `bar-lookup` re-run cells were not repeated (its verdict, a bimodal shape, is read again in section 3 from this run's own spreads).

## 3. Results

The QPS sweep (the driver's controlled matrix, 100 statements per cell, 8 distinct arguments in the warm cases), median of three runs. `cold` = every argument seen for the first time, `warm` = the same few
arguments cycled, `cabin` = warm with a Cabin declared on the filter column. "Read" applies the rule of `{SUMMARY}` ("What counts as a finding"): a delta below the larger spread is not a finding,
a spread above 20 % carries no claim. **A delta against the morning is client plus host; only section 9 separates them**, and its A/B says where the host part is not zero.

#### cell c1-g: cores = 1, group

{matrix('c1-g')}
#### cell c8-g: cores = 8, group

{matrix('c8-g')}
#### cell c1-s: cores = 1, strict

{matrix('c1-s')}
**The shapes that return rows moved by a factor of two; the shapes that return a handful did not move, or lost 13 % to 16 % to a host effect.** `bar-range`, `symbol-history` and `model-join` are
+84 % / +98 % / +68 % at `c1-g`, +84 % / +94 % / +70 % at `c8-g` and +80 % / +91 % / +67 % at `c1-s`, every one far above its spread. `day-slice` and `cross-join` are -14 % at `c1-g`, -13 % / -14 % at `c8-g` and
-16 % / -16 % at `c1-s`: by the rule, inside the spread or past the 20 % line at `c1-g` (spreads 18.6 % and 20.6 %) and `c8-g` (18.8 % and 17.6 %: one slow run in three) and a finding at `c1-s` (spreads 1.0 % to 2.7 %). Their Cabin arms, which are
statements returning 8 rows by a cache hit, are **+16 % to +26 %** (a round trip of 8 rows decodes cheaper), so the same cells moved in opposite directions: the scan got slower and the reply got cheaper. Section 9's A/B
puts the scan's -14 % on the host.
`bar-lookup` and `point-join` are bimodal at `c1-g` and `c1-s` (spreads 29 % and 38 % at `c1-g`, 33 % and 44 % at `c1-s`) and steady at `c8-g` (2.8 % and 0.6 %); no delta is claimed on the bimodal cells.

Every phase of the driver, median over the three `c1-g` runs. The PostgreSQL column is the morning's run:

{phase_table('c1-g')}
**Writes: the durability wait is the cost of a row.** Rows per `BEGIN`/`COMMIT`; median of three runs with the spread in brackets (writes return no rows; the delta is the host's, not the client's):

{writes()}
**Concurrency.** One join (`cross-join`) issued over 1, 2, 4 and 8 connections:

{conc()}
The `cores = 1` cells do not scale with connections (the aggregate is round-trip overlap) and `cores = 8` reaches {med([x['qps'] for d in N('s1-c8-g') for x in d['meta']['concurrency_sweep'] if x['connections'] == 8]) / med([x['qps'] for d in N('s1-c8-g') for x in d['meta']['concurrency_sweep'] if x['connections'] == 1]):.2f}x at eight connections
(spreads of 16 % to 26 % at two, four and eight: the sessions are placed on cores as they arrive, BL-S3). The `cross-join` rows are below the morning's by 13 % to 16 % at one connection: the scan effect of section 9, not the concurrency.

## 4. Percentiles

Run 1 of each cell, every phase of the driver (microseconds; the five percentiles are the driver's). The phases that return rows, now against the morning's run 1 and PostgreSQL's run 1, come first:

{s1b.cmp_pct(['read-bar-range', 'read-symbol-history', 'compare-one', 'compare-all', 'agg-by-session', 'backtest-read'])}
The distributions moved as a whole, not by a tail: `read-bar-range` p0 {BRN['p0_us']:,.0f} µs against {BRM['p0_us']:,.0f} µs in the morning and {BRP['p0_us']:,.0f} µs for PostgreSQL, p50 {BRN['p50_us']:,.0f} against {BRM['p50_us']:,.0f}
(PostgreSQL {BRP['p50_us']:,.0f}) and p99 {BRN['p99_us']:,.0f} against {BRM['p99_us']:,.0f} (PostgreSQL {BRP['p99_us']:,.0f}); `read-symbol-history` p50 {SHN['p50_us'] / 1000:,.1f} ms against {SHM['p50_us'] / 1000:,.1f} ms (PostgreSQL {SHP['p50_us'] / 1000:,.1f} ms).
A decoder cost is paid per row on every statement, so the floor (p0) and the body (p25, p50) move with the tail.

{cellrun_pct('c1-g', 'cores = 1, group')}
{cellrun_pct('c8-g', 'cores = 8, group')}
{cellrun_pct('c1-s', 'cores = 1, strict')}
## 5. Where the time goes

The matrix is serial reads on one connection, so a statement is a fixed cost plus a per-row cost, and a write is a statement plus the durability wait. What the cells measure of each, `c1-g` run 1:

{s1b.components()}
The size sweep separates the per-row term from the fixed term: the returned-row cost (mean of the warm sweep minus the cell's own `bar-lookup` mean, over the rows returned), now against the morning's and PostgreSQL's:

{perrow()}
**The per-row cost of a returned row is flat across a 240x range of relation size and across two row counts, and it is now within 2 % to 19 % of PostgreSQL's.** KDS spends 3.54 to 3.61 µs per row on a 200-row range at every size
(PostgreSQL 3.23 to 3.47 µs) and 4.51 to 4.84 µs on a symbol history (PostgreSQL 3.88 to 4.16 µs); the morning's figures were 6.9 to 7.1 and 8.9 to 9.5 µs. The cost lived in the Python decoder
(`tools/kwp.py`), not in the engine and not in the relation: the same binary, a different client, a ratio of two. Section 10 puts a number on what is left.
The fixed round trip is the one place PostgreSQL is cheaper per statement at the larger sizes (KDS 79 µs against 68 µs at 60,480 bars); at 252 and 1,008 bars KDS's is the cheaper (74 against 100 µs, 76 against 87 µs),
which makes the pk reads 1.34x and 1.15x of PostgreSQL's there and 0.86x at 60,480 bars (the cell is bimodal; section 3).

## 6. Joins and the plan the engine chose

The engine chooses an inner-join block's order since CB-S2 and prices it from the join planner's statistics since CC. The client has no part in a plan; this section repeats the morning's probe on the same binary
(`pl-s1-planned-r1..3`, `pl-s1-written-r1..3`), three runs each of the default and of `join_order = written`, same window:

{plan_tbl()}
The planned and written arms are compared with each other in one window, so the host drift of section 9 cancels. The plans themselves (the ANALYZE replies under `plans/` in the archive) are those of the morning:
`compare-all` and `compare-one` are reordered (`reordered=1`), `backtest-read` keeps its written order, and `compare-all` carries the statement-local build.
**The order choice buys the same 6 % to 7 % on the two reordered joins as in the morning, and a shorter statement does not enlarge it.** `compare-one` is 1.07x planned over written (spreads 1.9 % and 1.8 %, so a finding by the rule) and the
`model-join` sweep 1.06x to 1.08x (spreads 1.3 % to 6.6 %); `compare-all` is 0.96x (spread 6.4 %), inside its spread. The morning measured 1.06x and 1.02x on the same rows with the old decoder. The statement that was 1.64 ms is 0.98 ms now, so the planner's
saving (3,231 rows examined against 2,873) is 7 % of a statement of either length: its payoff scales with the walk, which the client change did not touch. The `point-join` rows (0.97x and 0.99x) are the bimodal row of section 3 (spread 32 % to 34 % in the
planned arm, 0.6 % to 2.3 % in the written) and carry no claim.

## 7. The size sweep

`c1-g`, 252 / 1,008 / 10,080 bars (60,480 is section 3), three runs each, now against the morning and against PostgreSQL's (morning) warm qps; the last two columns are the ratio of KDS to PostgreSQL, now and in the morning:

{sizes()}
**Fixed and per-row costs separate cleanly at every size.** The pk shapes are flat (`bar-lookup` 12,962 to 13,408 qps across 252, 1,008 and 10,080 bars; `point-join` 12,521 to 12,855 qps at all but the bimodal 60,480-bar cell).
The scan shapes scale with rows (`day-slice` 11,106 -> 7,661 -> 2,285 -> 509 qps). The row-returning shapes are flat in relation size (`bar-range` 1,253 to 1,279 qps at every size) and
scale with the rows returned (`symbol-history` 826 -> 815 -> 164 -> 27 qps as the symbol's sessions go 252 -> 252 -> 1,260 -> 7,560). At every size the morning-to-now ratio of a many-row shape is
1.8x to 2.0x (`bar-range` 1.83x to 1.92x; `symbol-history` 1.92x to 1.98x), so the decoder cost is per row and independent of the relation.

## 8. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor: it says whether the engine is in the right league and nothing finer. Its twin was **not re-run**; its cells are the morning's `pg-s1*` runs (three runs each; 04:25 to 04:33 UTC). Ratios are KDS `c1-g`
(median of three, this run) over PostgreSQL (median of three, the morning's), above 1.00x = KDS faster; the morning's KDS ratio is beside it:

{pgratio()}
| rows per BEGIN/COMMIT | KDS c1-g (now) | PG18 (morning's run) | KDS/PG |
|---|---|---|---|
""" + "".join(
    f"| {b} | {f0(med([x['qps'] for d in N('s1-c1-g') for x in d['meta']['write_sweep'] if x['batch'] == b]), ' rows/s')} | "
    f"{f0(med([x['qps'] for d in Mo(1, 'pg-s1') for x in d['meta']['write_sweep'] if x['batch'] == b]), ' rows/s')} | "
    f"{ratio(med([x['qps'] for d in N('s1-c1-g') for x in d['meta']['write_sweep'] if x['batch'] == b]), med([x['qps'] for d in Mo(1, 'pg-s1') for x in d['meta']['write_sweep'] if x['batch'] == b]))} |\n"
    for b in (1, 10, 100, 1000)) + f"""
**The league reading.** The shapes that returned many rows were at half of PostgreSQL's speed and are at 0.83x to 0.94x of it (`bar-range` {ratio(mw(K, 'bar-range'), mw(PGR, 'bar-range'))}, `symbol-history` {ratio(mw(K, 'symbol-history'), mw(PGR, 'symbol-history'))},
`model-join` {ratio(mw(K, 'model-join'), mw(PGR, 'model-join'))}, `compare-all` 0.84x, `compare-one` 0.94x, `agg-by-session` 0.92x). KDS is still ahead on the pk-anchored join (1.50x to 1.98x), the unindexed equality scan
(`day-slice` 1.23x at 60,480 bars, a figure the host drift lowers; 1.00x to 1.11x at 1,008 and 10,080 bars) and the three big load phases (`load-sessions`, `load-bars`, `load-stats`: 1.47x to 1.65x), and behind on whole-relation folds (`agg-global` 0.68x,
`agg-distinct` 0.68x), which return one row and are not the decoder's. Row cost is no longer the thing that sets KDS apart from PostgreSQL on this scenario; the folds and `bar-lookup` are.

## 9. The client, isolated: old against new, interleaved

Three pairs, old, new, old, new, old, new, on the same host, one `s1-c1-g` cell each (a fresh server and data file per cell, the same binary copy; the old client is `a40882af^`'s `tools/kwp.py` and
`tools/ckdbs_cli.py` from `oldtools/`, imported first through `PYTHONPATH`). Spread = (max - min) / median over the three; the rule of the summary applies.

{abx.s1_sweep()}
And every phase of the driver:

{abx.phases('ab-{v}{i}-s1-c1-g')}
**The decoder explains the whole movement of the row-returning shapes and none of the movement of the others.** In one window, with the engine identical, `bar-range` {AB['bar-range'][1] / AB['bar-range'][0]:.2f}x,
`symbol-history` {AB['symbol-history'][1] / AB['symbol-history'][0]:.2f}x, `model-join` {AB['model-join'][1] / AB['model-join'][0]:.2f}x, `compare-all` {abr('compare-all'):.2f}x, `compare-one` {abr('compare-one'):.2f}x, `agg-by-session`
{abr('agg-by-session'):.2f}x (7,560 groups returned), and the Cabin-served 8-row statements 1.2x to 1.3x; everything that returns one row or a handful sits between 0.97x and 1.06x
(`day-slice` {AB['day-slice'][1] / AB['day-slice'][0]:.3f}x, `cross-join` {AB['cross-join'][1] / AB['cross-join'][0]:.3f}x, `bar-lookup` {AB['bar-lookup'][1] / AB['bar-lookup'][0]:.3f}x, the folds 1.01x to 1.02x). The loads, the writes and `sync` are
inside their spreads, which fits a client change that touches only row batches.

**The -14 % on the scan shapes is the host.** The old client, the morning's client, reads `day-slice` at {f0(AB['day-slice'][0], ' qps')} in this window and the morning's own cells read {f0(mw(KO, 'day-slice'), ' qps')}: -{100 * (1 - AB['day-slice'][0] / mw(KO, 'day-slice')):.0f} %
with nothing changed but the time of day. `cross-join` the same ({f0(AB['cross-join'][0], ' qps')} old now against {f0(mw(KO, 'cross-join'), ' qps')} then). Scenario 4's `off` walk, on a 10,000-row varchar equality, did not drift (its server CPU is
2.20 s here and 2.22 s in the morning), and scenario 3's integer-column scans did (the old client reads them 11 % to 12 % below the morning's cells, the same sign), so the effect belongs to the scans of the large integer relations. This run does not say whether it is
frequency, a neighbouring tenant or where the server thread landed; it says the interleaved pairs are the only reading of the client's effect that the drift does not touch.

## 10. What moved, why, and what is left of the gap to PostgreSQL

1. **The decoder was the per-row gap.** Same binary, same data, same statements: the only difference between the arms of section 9 is the client's row-batch decode and the renderer lookup, and a returned row costs 3.4 to 4.8 µs
   instead of 7 to 9.3 µs. This answers the question the morning's file left open ("whether the factor of two lives in the engine or in the two Python wire decoders is not separated"): it lived in the decoder, and
   `bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md`'s verdict (84 % of the per-row wall gap is the decoder) is borne out in end-to-end scenario numbers.
2. **What remains is {ratio(mw(K, 'symbol-history'), mw(PGR, 'symbol-history'))} to {ratio(mw(K, 'model-join'), mw(PGR, 'model-join'))} of PostgreSQL on the many-row shapes**, a per-row difference of 0.3 to 0.8 µs (section 5). The row-cost attribution
   (`bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md`, whose corrected verdict is 84 % decoder and 16 % engine-side) found the engine-side 0.42 µs per row is not CPU but the *absence of overlap*: KDS builds the whole reply and sends it
   at statement completion (`WireResultSink::Emit`, `KwpSession::Deliver`, BN-R7), so the server's 0.43 µs of CPU per row is added to the client's time instead of overlapped with it, as PostgreSQL's streaming backend allows.
   {RC}
3. **No limit was found for the many-row shapes**; none was sought: they are serial single-connection reads, bounded by the client's per-row work and now by the engine's non-overlapped reply, not by a resource.
4. **Insights about the engine.** The engine's own cost of a returned row is 0.42 to 0.45 µs of server CPU (new client and old, this window), flat; everything else a scenario driver pays per row is Python. The decoder fix halves the
   driver's per-row cost and leaves the engine's term exactly where it was (0.42 µs of wall that is not overlapped), which is why that term is now 9 % of a returned row (0.42 of 4.74 µs) instead of 4.9 % of the old client's 9.25 µs.
   Where a result bears on a stated design expectation: `docs/spec/protocol.md`'s 64 KiB batch target and the single-write reply (BN-R7) are chosen for syscall count; this scenario puts the price on them, a reply that cannot overlap
   server and client, at 0.42 µs per row of a 12-column read, and that price is the largest term the engine still owns on a many-row read. The join planner's payoff, which a 6 µs-per-row decoder hid, is re-read in section 6.

#### Row-cost attribution, KDS only (`rowcost/` in the run archive)

The attribution's own cells (`SELECT * FROM daily_stats WHERE id BETWEEN 1 AND N`, N = 1 to 60,480, 12 columns, three repeats per point, windows of about 3 s; the server pinned to CPU 2 and the client to CPU 6) on this run's binary copy,
with the old and the new client in turn on fresh servers (`values`: cells become Python ints; `send`: the scenario drivers' path). PostgreSQL's column is the attribution file's (06:19 to 06:26 UTC), **not re-measured**, so every "new / PG" is a cross-window ratio of 2 to 3 hours.

{rcsec.table()}
Per statement, median of three repeats (spread in brackets):

{rcsec.sizes_table()}
Latency distribution of one statement, pooled over the three repeats (n = statements):

{rcsec.dist_table()}
The wall time per statement is halved at every size from 100 rows up (0.51x to 0.56x), and a one-row statement is unchanged (`send`: 83.5 against 83.8 µs). The distributions move as a whole (p0 to p99), as in the scenario cells.

Three cells of these sweeps overlapped a sample with a foreign process at 50 % CPU or more and were re-run (`rowcost/contaminated/` in the archive; the summary's Contamination lists them); the tables above are the clean sweeps.

## 11. Noise floor, contamination and host

The noise floor is the 3-run spread of one cell: {sp(spread(sv(K, 'bar-range', 'warm')))} for `bar-range`, {sp(spread(sv(K, 'symbol-history', 'warm')))} for `symbol-history`, {sp(spread(sv(K, 'model-join', 'warm')))} for `model-join`, 18.6 % for `day-slice`
(one slow run in three) and 29 % for `bar-lookup`, which is bimodal as in the morning: a fast mode of 12,300 to 13,400 qps and a slow one of 9,100 to 9,200 qps (2 of the 9 matrix runs, none of the 6 A/B runs); `point-join`
has a slow mode of 8,700 to 10,600 qps (4 of 9 matrix runs, 1 of 6 A/B runs). No delta is claimed on either. The A/B spreads (section 9) are 0.4 % to 4.8 % on the six non-bimodal warm shapes (`cross-join` in its Cabin arm, 18.1 % in the old arm, is the widest).

Contamination: one KDS cell of the whole run (scenario 2's `s2-c1-s`) was discarded and re-run because a `claude` process of another session used more than 50 % of a CPU for one 1-second sample
inside its window; none of scenario 1's cells was contaminated (`logs/validity.log`). The re-run's attempt is under `contaminated/` in the run archive.

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

{hostrows(kt + st + abt + plt)}
## 12. Archive

`bench/v3.3.0/archive/scenario1-{DESC}/`: per-cell JSON, the driver stdout of each cell, the server configs, the join-order probe's ANALYZE replies (`plans/`). What the four scenarios share is in
`bench/v3.3.0/archive/run-{DESC}/`: the run scripts, the timeline, the monitor and watchdog logs, the binary hashes, the table generators under `analysis/`, and `rowcost/`. No data file is kept.
"""
write("results-scenario1-backtest", txt)
