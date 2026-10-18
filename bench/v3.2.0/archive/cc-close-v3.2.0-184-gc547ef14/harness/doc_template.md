# CC close: the join planner's statistics cost nothing off the planning path and buy 13-62 % of server CPU on the flips CC-S0 found

Work order: `instructions/v3.2.0/workorder-cc-plan-statistics.md` (§9 CC-close, §10), spec
`docs/spec/plan-stats.md`. Raw files, every block's per-statement latencies, the plans, the SHOW META
snapshots and the harness: `bench/v3.2.0/archive/cc-close-v3.2.0-184-gc547ef14/`.

**Finding.** On `v3.2.0-184-gc547ef14` (B, branch `worktree-cc-stats-planner`) against the opening commit
`v3.2.0-178-g65dc45c5` (A), measured in two full interleaved runs (R1, R2) and seven partial ones (ov2, ov3, the same-plan and memo-miss cells, gain2, gain3, gain4, restart2):

1. **The paths that do not plan did not move.** Point read, tuple-key three-relation join, single-row INSERT and
   single-row UPDATE are inside the noise floor of the run in mean CPU, p50 and p99: B against A pooled over
   36 paired rounds (20 for INSERT and UPDATE) is +0.81 % (point read), +0.43 % (join), -0.10 % (INSERT), +0.50 % (UPDATE), while the control
   server running A's binary sits +0.86 %, +0.82 %, +0.65 %, -0.09 % from A. Gate met.
2. **The S0 flips are realised.** With statistics published, B's server CPU per statement is 12.6-13.6 % lower on
   scenario 1 compare-all at 200 results and 32.9-39.6 % lower at 1,000 to 10,000; 38.2-39.5 % lower on
   scenario 2 customer-statement at 200 cargos and 60.4-61.7 % lower at 1,000. The 15 % gate from the flip is met on
   six of eight cells; 200 results is short (the planned path adds about 25 µs a statement and the statement is
   144 µs), and 200,000 cargos is the cell S0 called indifferent and the planner flips anyway (`switch=0.68`).
3. **One result needs its caveat.** At the CA size (10,000 cargos) the flip saves 29.1 / 31.6 % in the full
   matrix and 44.7-46.7 % on a server that never had other tables created in it; at 200,000 cargos the flipped plan
   costs +10.5 / +7.4 % in the full matrix and -4.6 % to +4.1 % on clean servers. The difference is not B's code:
   the same swapped plan under `join_order = written` on **A's** binary is 30.7 % and 12.7 % dearer on a server on
   which four small tables were created and loaded after mount (section "An engine effect the harness triggered").
   The cause is not diagnosed here.
4. **A memo hit pays for itself; a miss costs 3 µs.** The 3-relation cross-join with a repeated literal is 17.5 %
   (200 rows) and 15.1 % (1,000 rows) cheaper on B because a hit skips CB's order search; a statement whose literal
   no execution repeats costs +2.7 % to +3.0 % (+3 to +4 µs) at 200 and 1,000 rows and +1.1 % at 10,000.
5. **The steward is unmeasurable.** B's idle CPU over 30 s is 0.3 ms above A's in one run and 0.8 ms below it in the
   other, against a control spread of 1.8-10.5 ms; its longest step is 54-69 µs, an upper bound of 0.007 % of core 0.
6. **Steady state is one order.** In a 60 s mixed run the statistics generation moved 0 times (17 → 17 and
   16 → 16), and every statement's `ANALYZE` showed one executed order.

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-18. Build 14:24 UTC; full matrix R1 14:27:28-14:35:16; partial runs 14:35-14:44; full matrix R2 14:44:14-14:52:04; bisection runs to 14:59 UTC |
| Worktree / branch | `/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner` / `worktree-cc-stats-planner` |
| B: `git describe --tags` | `v3.2.0-184-gc547ef14` (commit `c547ef14`, 2026-10-18 14:23:41 UTC) |
| A: `git describe --tags` | `v3.2.0-178-g65dc45c5`, the CC-S0 binary built from `git archive` (`bench/v3.2.0/results-cc-s0-premise-v3.2.0-178-g65dc45c5.md`) |
| Tree | The worktree carried uncommitted edits to `CLAUDE.md` and `instructions/v3.2.0/index.md` only (documents, not build inputs); `git status` shows nothing else. The binary was built in this tree's `build-release` |
| Binary B | `build-release/kds_server` built `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, source mtime 2026-10-18 14:24:45 (after the commit time 14:23:41, so it is the engine at HEAD); copied to `/home/cdkbs/bench-runs/cc-close/kds_server_B`, sha256 `3bc1180ec90c3957a1c5d7066c5bd696eea71dd190d9ed858c7cb6da7ad9d3ac`. Every B server started from the copy. The main checkout's `build-release` was not touched |
| Binary A | `/home/cdkbs/bench-runs/cc-close/kds_server_A`, copied from the CC-S0 copy, sha256 `8ce595c65ad1dcdc003443d96896d4c6f6b346fa262bf05b9a1de7c9d1ce495e` (matches the S0 record); source binary mtime 2026-10-18 11:55:47, newer than the commit (11:05:45). A2 is a second server started from the same A copy |
| Build type | Release, no frame pointers |
| Device | `/dev/root`, ext4 (`df -T` checked), data files under `/home/cdkbs/bench-runs/cc-close/run/`. Not tmpfs |
| Host | AMD EPYC 9V74 80-core model string, 8 CPUs visible. Servers pinned to CPU 2 with `taskset`, the client to CPU 3, analysis on CPU 4. Host state per block is in the archive: {{host}} |
| Server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, default `inline_cell_width`, `join_order = planned`; ports 25701-25735 |
| Dataset | The CC-S0 loader's pristine file (`/home/cdkbs/bench-runs/cc-s0/load/pristine.db`: scenario 1's five relations at 200 / 1,000 / 2,872 / 10,000 results and 200 / 1,000 / 10,000 / 60,480 stats rows, scenario 2 driver-loaded at 200 / 1,000 / 10,000 / 200,000 cargos), copied to every server. Every server then created `ov_ex`, `ov_ins`, `ov_upd` (10,000 rows) for the overhead cells |
| Metric | Server CPU per statement: the sum over every server thread of `/proc/<pid>/task/*/schedstat` run time, before and after each block, divided by the block's statements. Throughput is client-observed statements per second of the same block. Latency is client-observed per statement |
| Not executed | The full test suite (the order was measurement only; `-DKDS_BUILD_TESTS=OFF`); sessions on a core other than 0 (`cores = 1`); `durability = strict`; the PostgreSQL floor |

## What was measured

Three servers per run, started from one pristine file, one active at a time: **A** (65dc45c5), **A2** (65dc45c5 again,
the noise floor from inside the run) and **B** (c547ef14). Each cell is a block of a fixed number of statements
(calibrated once to about 1 s on A2, the same on every arm), 10 % of it run first as warm-up, and the arms rotate
(reversed on alternate cells and rounds) over 5 rounds. Before the timed rounds all three servers ran the same
warm-up statements and then 2.5 s idle, so B's steward had published (`generation` 16-17 at the first timed block).
Scenario statements are written as the drivers write them (spelling A of S0's harness, no swapped spelling).
The floor for a delta is the larger of the A2-minus-A difference, half A's round-to-round spread and 2 %.

| Cell | Statement | Size and how it was made |
|---|---|---|
| ov-point-read | `SELECT * FROM daily_bars_x10k WHERE id = N` | 10,000 rows, tuple key |
| ov-join3-tuplekey | `daily_bars_x10k b JOIN symbols_x10k s ON b.symbol_id = s.id JOIN ov_ex e ON s.exchange_id = e.id WHERE b.id = N` (bar → symbol → exchange) | anchored on a tuple key, so the planner's skip applies |
| ov-insert | `INSERT INTO ov_ins (a, b, c) VALUES (...)`, tuple key issued | one row, autocommit |
| ov-update | `UPDATE ov_upd SET a = a + 1 WHERE id = N` | 10,000 rows, tuple key |
| c1-compare-all-r200 / r1k / r2872 / r10k | `tools/scenario1_backtest.py` `compare_all_sql`, `model_results r JOIN models m` | 8 models and 200 / 1,000 / 2,872 (driver default) / 10,000 results |
| c2-customer-statement-n200 / n1k / ca10k / df200k | `tools/scenario2_freight.py` customer statement, `freights f JOIN cargos c ... WHERE c.org_id = X GROUP BY c.org_id` | `--cargos` 200, 1,000, **10,000 (the CA size)**, 200,000 (driver defaults); 180 / 879 / 8,234 / 10,795 freights |
| c3-x200 / x1k / x10k / x60k | `cross_section_sql`, `daily_stats t JOIN daily_bars b JOIN symbols s WHERE t.session_no = N` | 200 / 1,000 / 10,000 / 60,480 stats rows (driver default 30 years x 8 symbols) |

The two partial rows that carry the size sweep (rule 9): scenario 1 and 2 and the cross-join carry four sizes each,
200 to 60,480. The overhead cells run at 10,000 rows only; a point read and a tuple-key join descend a height-2
tree whatever the row count, so they were not swept (cut, stated).

## Result 1: the paths that do not plan did not move

The point read and the tuple-key join compile no searched block (the planner skips a chain of tuple-key descents),
INSERT and UPDATE compile none either, and the compile pins the statistics snapshot only for a block it searches
(`plan-stats.md` PS9), so by design B adds nothing on these statements. The measurement agrees to within the floor. Four
replicates of the same cells (R1 and R2 are the two full runs, ov2 and ov3 are partial runs of the same servers'
recipe with 10 and 16 rounds); the pooled table counts every round of every replicate.

{{pooled}}

The point read is the only cell where B sits above A in every replicate but one, by 0.6-2.8 %; the control A2
sits above A by 1.3-1.9 % in the same two replicates (ov2, ov3), and B against A2 pooled is -0.03 % mean.
That is the shape of a process-to-process offset of about 1 %, not a cost; the 16-round replicate has B below A.

R1 (5 rounds), with the latency distributions:

{{ov1_t}}

{{ov1_d}}

R2 (5 rounds, full matrix repeated from fresh files):

{{ovR2_d}}

ov2 (10 rounds) and ov3 (16 rounds, read cells only):

{{ov2_d}}

{{ov3_d}}

INSERT and UPDATE p99 deltas in the tables above are group-commit noise (INSERT's p99 is 3.2-3.8 ms against a 1.2 ms
median and moves by up to 10 % between identical servers); the means sit inside the floor in every replicate.

**Where the time goes (rule 3).** The server's CPU is a fraction of what a client sees. Remainders are derived as
p50 minus server CPU on A in R1; this run did not instrument them separately (the commit wait at `group` durability
is not split from the socket round trip here).

{{waits_tbl}}

For a read, 31-38 % of the p50 is the client and socket round trip, which no engine change can reach. For a write
at `durability = group` 93 % is the commit wait plus the round trip; that is why a sub-5 % CPU change in INSERT
or UPDATE cannot show in latency, and why those two cells are gated on CPU.

## Result 2: the gain, steady state against CB's data-free planner

On all eight scenario cells B's statistics flip the join to the order S0 found cheaper (`reordered=1`); seven of them
pay for it, and the 200,000-cargo cell does not. The
first table is R1's server CPU, throughput and latency distribution; the second is the delta against A and
against the control. R2 repeats it from fresh files.

R1:

{{g1_t}}

{{g1_d}}

R2:

{{g2_d}}

Read against CC-S0 (`v3.2.0-178-g65dc45c5`, the forced alternative on W:B versus P:A):

| Cell | S0 forced alternative (minimum over 3 pairs) | B realised, R1 / R2 (clean-server figure in brackets) | 15 % gate |
|---|---|---|---|
| compare-all 200 results | -21.6 % | -12.6 % / -13.6 % | not met (planned path costs about 25 µs per statement on a 144 µs statement) |
| compare-all 1,000 | -34.5 % | -32.9 % / -36.3 % | met |
| compare-all 2,872 (default) | -38.1 % | -35.6 % / -39.2 % | met |
| compare-all 10,000 | -37.3 % | -36.6 % / -39.6 % | met |
| customer-statement 200 cargos | -41.7 % | -38.2 % / -39.5 % | met |
| customer-statement 1,000 | -60.7 % | -60.4 % / -61.7 % | met |
| customer-statement 10,000 (CA size) | -32.2 % (typical -46 %) | -29.1 % / -31.6 % (-44.7 % to -46.7 %) | met |
| customer-statement 200,000 (defaults) | -0.7 % (S0: indifferent) | +10.5 % / +7.4 % (-4.6 % to +4.1 %) | not met, and not expected to be |

The plans B chooses at steady state, from `ANALYZE` (the full lines are in the archive's `plans.json`):

{{plans1}}

**The planned path has a fixed cost.** The 200-result compare-all is 144 µs on B where S0's forced alternative was
119 µs (a cross-run comparison). On identical plans in one run (table below, `BP:drv` against `WB:swap`) B planned
costs +10.0 % (+19 µs) at n1k, +0.1 % at 10,000 results, +1.1 % at ca10k and +1.3 % at df200k over the same order
forced with `join_order = written`. A memo hit compiles the written order and then the remembered one, two
compiles where the forced order pays one (`plan-stats.md` §6a); that is the 19-25 µs, and it is why the gain is
short of the forced figure by 8-9 points at the smallest statement and by nothing from 10,000 results.

**First executions (cold) are data-free and cost what A costs.** Each cell's first 20 executions on a fresh server,
before the steward had seen the relation; A and B ran them in the same order. R1:

{{cold1}}

In R1 B's executions 2-20 are within -3.4 % to +1.0 % of A's mean CPU in every cell; the single first executions
differ by -5.6 % to +15 % either way (the first statement pays the page faults, 5.6-24.7 ms at 10,000-200,000
cargos). In R2 (below) the same holds within 7 % except compare-all at 2,872 results, where B's mean over
executions 2-20 is 25 % below A's: the steward's first publish landed during that cell's 20 executions and the flip
began there. The flip arrives with the first publish: one second after the
first execution (restart table, below).

R2's cold table:

{{cold2}}

### An engine effect the harness triggered

In both full runs the CA-size and default-size cells came out well short of the CC-S0 figure on B. A fresh B
re-run with the same warm-up history (`gain2`: servers A, A2, B as in the main run but without the overhead
tables, and `BL` warmed on the three cells only; 5 rounds) gave:

{{g2x_d}}

so the figure depended on the server's history. The bisection below shows what it depended on. `B0` had the gain warm-up only; `B1` had `ov_ex`,
`ov_ins`, `ov_upd` created and `ov_upd` loaded (10,000 rows) before the warm-up; `B2` and `B3` add the overhead
statements and the cold executions. A has the same tables in this run.

{{g3x_t}}

{{g3x_d}}

The overhead statements and the cold executions add nothing (`B1` = `B2` = `B3`); creating and loading the tables is
enough, and it is not B's code: under `join_order = written` the swapped order (the plan B picks) on **A's binary**
shows the same step, and so does B's binary:

{{g4x_t}}

{{g4x_d}}

`WA1` and `WB1` hold the four small tables created and loaded after mount; `WA0` and `WB0` do not. The slow plan is
`FilterScan cargos`, then `Scan freights` with a statement-local build; A's own driver-order plan (Probes) is
not slowed (A's ca10k mean is 4,306-4,380 µs with and without the tables). I did not find the cause; it is
independent of the CC statistics (`65dc45c5` shows it) and of the overhead statements, and it needs a diagnosis
(page placement of the pages allocated after mount, and the pool, are the first suspects, both unproven). For the
gain figures the clean-server numbers are the ones comparable with CC-S0, whose servers never had other tables
created; the full-matrix R1 and R2 figures are what the harness measured and are kept in the table above.

Same-run checks on clean servers (the extra cells: both binaries under `join_order = written`, the driver's
order and the swapped order, beside B planned; `BP` had no extra tables):

{{ex_t}}

{{ex_d}}

`WB:drv` against `WA:drv` and `WB:swap` against `WA:swap` are within -3.5 % to +4.1 % (the larger entries are the
7.2 % round spread of `WA:swap` at ca10k), so the two binaries run the same plan at the same cost; the realised
flip on a clean server is -37.3 % (10,000 results), -58.6 % (n1k), -44.7 % (ca10k) and -2.3 % (df200k).

The customer-statement at 200,000 cargos is the crossover S0 predicted: the planner's estimate (`est=1:91,0:6`,
switch 0.68) says the cargo-first order is 32 % cheaper, and the measured saving is between -4.6 % and +4.1 % on
clean servers. The prior for `cargos_df` is 131,072 against 200,000 (0.66x); the estimate of rows the filter
leaves (91 against the true 100 per organization) is fine, and the plan the model prices at 0.68 of the written order costs 0.95 to 1.04 of it. Where the model's error sits
(the price of a rejected row on the `FilterScan`, or the build) is not established here, but the inputs it was
given were close, so it is a cost-constant question and not a statistics one.

## Result 3: the order memo

The cross-join's plan does not change with statistics (`order=written`, `est=0:8,1:8,2:8` on B; the same
steps as A), so every B-over-A difference on this statement is the cost of choosing the order. With a repeated
literal B serves the order from the memo and compiles once; A searches every time.

R1:

{{m1_t}}

{{m1_d}}

R2:

{{m2_d}}

"Vary" draws `session_no` over the data (25, 125, 1,250, 7,560 distinct values), so at 200 and 1,000 rows the
literals repeat inside the 1,024-entry memo and mostly hit: the table below is not a miss measurement there.

{{vary1}}

**The miss cost** is the separate cell: an always-true conjunct (`AND t.ret_bp > -(1000 + k)`, `k` unique for
every execution) makes every execution miss the memo. Three planned servers, `PA`/`PA2` on A's binary and `BP` on
B's, 3 rounds each, all warmed with the same statements:

{{uq_t}}

{{uq_d}}

A hit saves about 17 µs (CB's search was 21 µs in S0); a miss costs B 3.0-4.0 µs more than CB's data-free search
at 200 and 1,000 rows and 4.1 µs at 10,000 rows; the statistics pricing and the memo store are that difference.
The hit's saving is 4.8 % of a 363 µs statement at 10,000 rows and 0.1 % (inside the floor) of a 1,634 µs one at 60,480 rows.

## Result 4: the steward

SHOW META on B at each phase boundary of R1 (`plan_stats` and `plan_memo` blocks; `part_drops` stayed 0, which
means no core's part table filled):

{{meta_tbl}}

The steward publishes only when a band moved or a fact was added: 16 publishes in warm-up, 1 more in the 2 minutes
of gain rounds and none after (generation 17 from 14:31 to the end of the run). Its work after convergence is the
round-robin re-seed of 28 relations within the page budget, about 2.7 seeds and 7.5 pages a second. The standing
cost, with every server idle for 30 s:

{{idle_tbl}}

B's idle CPU is within the control's spread of A's, so the steward's share of core 0 is at most the measurement's
resolution; the upper bound from `steward_us_max` (54-69 µs, one step a second) is 0.007 % of the core. The
gate (≤ 1 %) is met by two orders of magnitude.

Pool (SHOW META `pool` block). Steady-state pool misses did not move because of the steward: B's misses went from
8,819 (after warm-up) to 8,820 (after the gain rounds) while the steward read 966 pages, so every seed page was
resident. The hit counts differ because the plans do: B's flipped plans touch 5.4x fewer pool pages than A's
Probe plans in the same rounds.

{{pool_tbl}}

## Result 5: how good is the prior

`SELECT * FROM sys.relation_stats` on B after the memo phase (R2 shows the same values: the random-path estimate is
deterministic), against `COUNT(*)`. {{pr_sum}}. Relations a planned block never named carry no prior
(est_rows empty): {{noprior}}.

{{priors_tbl}}

Height 1 is exact (the root's live slots). Height 2 is 0.64-1.01 of the truth (0.91 at 200 rows, 0.71-0.72 at 1,000 and 2,872, 0.82 at 10,000), because the
estimate multiplies the root's children, exact, by a fixed leaf capacity and an assumed fill.
**Height 3 is the weak case:** `daily_bars_x60k` is 0.38x (23,170 against 60,480) and `cargos_df` 0.66x (131,072
against 200,000), while `daily_stats_x60k` is 1.08x. CC-S0 measured the formula at 0.75-0.98x on 30 relations of
height 2-3 and 4-6.8x off on small height-1 ones; the height-1 correction is in, the 0.38x of one height-3 relation
is the new data point. Only one gain cell involves a height-3 prior (`cargos_df`, 0.66x, the 200,000-cargo cell
whose flip was indifferent); the cross-join's 60,480-row relations (0.38x and 1.08x) leave `order=written`
in both.

## Result 6: stability and restart

A 60 s mixed run on B (six statements cycled: compare-all 2,872 and 10,000, customer-statement n1k and ca10k, the
cross-join with a varying literal at 10,000 rows and with a fixed one at 1,000), `ANALYZE` at 0, 15, 30, 45 and
60 s:

{{stab_tbl}}

Every one of the six statements showed one executed order across its five `ANALYZE`s in both runs (the label moves
between `planned` at the first, `memo` afterwards and, for the varying-literal statement, `written` on a miss; the
steps do not). Throughput is flat across the windows (R1: 3,051 → 2,971 statements per 10 s, a 2.6 % drift over the minute with no
generation change in it). Generation did not move, so the expected "0-1 bumps and 1 order" is
met with 0.

Restart (B stopped cleanly and started again on its file, three attempts; the statement is compare-all at 2,872
results, run in a loop from the first connection):

{{restart_tbl}}

The first publish comes about 1 s (`kPlanStatsTickNs`) after the first statement, and the warm plan (generation
> 0, `reordered=1`) is visible 1-2 ms after it; the 40 statements before it run data-free. A first start from a
freshly copied file takes 0.86-1.0 s to listen (the recovery checkpoint of 0.9 s), against 0.103 s for a clean
restart.

## Insight

* **Statistics paid for themselves where S0 said they would** and the margin is in how much of the saved work is
  fixed against per-row: at 200 results the statement is 144 µs and a memo hit's second compile eats a fifth of
  the saving, at 10,000 it is nothing.
* **The cost model is the next constraint, not the statistics.** The only cell where the planner's choice lost
  (200,000 cargos) is one where its inputs were roughly right (the filtered-cargo estimate is 91 against about 100
  true; the large relation's prior is 0.66x) and its price of the cargo-first plan (0.68 of the written order)
  was not (measured 0.95-1.04).
* **The prior is dependable up to height 2 and weakest where leaves are many** (height 3 at 0.38x-1.08x); this
  matters only where two relations of that size compete.
* **The memo is an exact cache** (`plan-stats.md` PS6) and its two effects are measurable: a hit removes CB's
  search (17 µs here), a miss adds 3-4 µs. Repeated statements are the common OLTP shape, so the expected cost is a saving.
* **A separate engine finding came out of the matrix:** a server on which a few small tables were created and
  loaded after mount runs the "filter the small side, build the big side" plan 12.7-30.7 % slower, on both
  binaries. Nothing about it is CC's, and it would have been invisible without the A2/BL/bisection runs.

No design expectation in `workorder-cc-plan-statistics.md` §9-§10 was contradicted except the two "expected 15 %
from the flip" cells named above (200 results short by 2-3 points; 200,000 cargos not expected, S0 §Result 2).

## Cut and not executed

* Sessions on a core other than 0: `cores = 1` as ordered; the work order's "core 0 and another core" is not executed.
* `durability = strict`, saturation with several clients, and the PostgreSQL floor were not measured. **Rule 4b:** no
  cell approached a limit; these are single-connection, no-regression and flip measurements, not ceilings.
* The overhead cells are at 10,000 rows (and 200 / 1,000 were not run) because they do not scale with rows.
* The full test suite was not run in this measurement; it is the stage's gate, not a measurement cell.
* The off-core-0 steward fault is not executed. The 60 s run is static (no DDL, no writes); statistics under write
  load were not measured.
* The cause of the post-mount-tables slowdown is not diagnosed.
* The cold table reports the first 20 executions per cell, not a per-statement breakdown of the compile.

## What went wrong during the run

* The main harness's own restart cell tested for `order=planned|memo` and so counted a data-free memo hit as warm
  (`warm_plan_s` 5 ms); its output (`restart.json`) is not used and not archived. `restart2.py` replaces it
  with the generation > 0 and `reordered=1` test; the table above is that run.
* The first launch of the bisection run (`gain3`) was killed by the previous batch's watchdog, which read an
  idle status file as an orphan and relaunched; the relaunch's servers failed to start once
  (`extra5.log` keeps the traceback). The watchdog was stopped, the status file is now written at launch, and
  `gain3` was re-run from the start; no number in this file comes from the interrupted launch.
* The harness for the cross-join "vary" arm drew literals from the data's own range, so its 200- and 1,000-row
  cells mostly hit the memo; the unique-literal cell was added to measure the miss.
* No server was left running; the data files and WAL directories stay under
  `/home/cdkbs/bench-runs/cc-close/run/` and are not archived.
