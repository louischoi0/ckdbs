# CY-S0 - does a correct literal selectivity change a scenario join's order, and does it pay?

**Finding: CY-Q1's gate is not met.** In 15 cells (5 scenario join statements x 200 / 1K / 10K rows) the order the planner chooses once its statistics are published is already the fastest forced order, to within the noise floor, in 14. The 15th (`cross-join-200`) is flagged by the pooled p50 and is rejected from the per-round p50s (below): the two orders differ by at most 3 us in every round. **No forced order that differs from the planner's, not only the p50-best, is 15 % faster at p50 or 15 % lower in server CPU than the planner's in any of the 4 rounds in any cell** (`an.py`, table "Every forced order"). So neither gate (a) (p50, CY-Q1 as worded) nor gate (b) (server CPU 15 % lower in every one of at least 3 rounds, CC's flip gate) is met. The cost-model constants are wrong where they are applied (error factors 0.08, one equality constant, to 12.5, `cross-join-10000`; the range-end constant is never applied to the tuple-key ranges here, see Question 1). The informed planner does not read them for the orders it has run: its `est=` is the observed cardinality rounded to its half-octave band (up to about 19 % off, for example 11585 against 10000), and that holds for the prefixes of the executed order only; the orders not run keep the priors and constants on every compile. The cost that is real is the **blind first compile** (`order=written gen=0`, every relation priced at 100000, no posterior, the 0.01 constant applied): 1.7x to 2.9x on `customer-statement`. The run cannot separate the prior from the posterior from the constant as its cause, so it does not say which of CY-S1 (priors) or CY-S2/S3 (column statistics) would reach it.

**Validity limit, stated up front:** the host showed a two-state shift (about +55 us latency and +20-25 us server CPU per statement at 200 rows) that hit every server and often flipped mid-block; see Validity limit. The verdict survives it because every losing order loses in every round.

This is an oracle bound (forced orders via `join_order = written`), not the planner run with injected statistics: no such seam exists in a release server.

## Stamp

| item | value |
|---|---|
| started / finished (host clock, UTC) | 2026-11-18 01:34 / 01:52 (build 01:34-01:36; run 3 started 01:46:54 and its rounds ran 01:48:46-01:50:50, the window before that being its blind compiles, counts and warm-up) |
| branch / commit | `worktree-cx-plan-change-option` / `e0cd5a93`, `git describe --tags` = `v3.4.12-18-ge0cd5a93` |
| tree | clean at the start of the stage. During the stage the worktree's HEAD moved: documents-only commits, and a checkout of `origin/main`, a merge and a checkout back at 01:39:46-01:40:06 UTC made by the parent session to land documents, so run 3 ran with HEAD at `1ba204ab`. `git diff --stat e0cd5a93 1ba204ab` touches only `docs/` and `instructions/`, so the engine and tools are identical to the measured commit `e0cd5a93`. Untracked: `bench/v3.4.12/` from this run |
| binary | `build-release/kds_server` built from this tree at HEAD `e0cd5a93` (commit time 01:33:40 UTC), binary mtime 2026-11-18 01:35:36 UTC, newer than the commit. Measured copy: `/home/louis/bench-runs/cy-s0/kds_server`, sha256 `2f9ecc07a4017ec0fb4eeb6207bb531a1c741b297ff7c80cbfc93c119da9f846` |
| build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF` |
| device | `/dev/nvme0n1p1` ext4 (`df -T`), data files in `/home/louis/bench-runs/cy-s0/run/` |
| server | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, rest default. **All three measured servers (P, W, W2) were pinned to the same CPU (CPU 2; `cy_s0.py` passes one `--cpu`) and the client was not pinned.** The F server (blind compile) ran before them on the same CPU, one at a time |
| host | 8 CPUs, load average max 1.19 at any block start, no `cc1plus`/`cmake --build`/`ctest` seen at any block |
| servers | F fresh `join_order = planned` (blind ANALYZE only), P `planned` (up to 6 x 40 warm statements per cell, then the order read from `ANALYZE`), W `written`, W2 `written` control (same spelling as W's first order). Fresh data file copied from one pristine file per server |
| rounds | 4 interleaved rounds, arm order rotated and alternated per cell and round, ~0.3 s of statements per block (time cap 4 s per block), same seeded statements across arms |

Run history, for honesty: run 1 was abandoned (the `b-t-s` order on the 10K cross-join is quadratic and did not finish); run 2 was discarded because servers left over from run 1 still held the ports, so it measured stale servers (no fresh data file per configuration, and its CPU column was zero); run 3, reported here, was started after the ports were verified free. The driver now refuses a port that is already served; that guard was added after run 3 started and so is in the archived harness but was not in the process that ran.

## Method

Driver and analysis: `bench/v3.4.12/archive/cy-s0-premise-v3.4.12-18-ge0cd5a93/harness/cy_s0.py`, `an.py` (derived from CC-S0's harness; raw: `results.json.gz`, `results.json.plans.json`, `tables.md` beside it; the driver wrote nothing to its `run.log`, which is not archived). Command: `cy_s0.py load` then `cy_s0.py run --rounds 4 --target-s 0.3`.

**Data is synthetic, built to the scenario schemas' shapes, not loaded by the scenario drivers** (cut for the budget; CC-S0 did the same for scenario 1). Size n in {200, 1000, 10000} means: `model_results` n rows over 8 models; `daily_stats`/`daily_bars` n rows (n/8 sessions x 8 symbols); `cargos` n rows over 20/50/300 organizations, `freights` 0.9 n (one per cargo); `users` n, `loans` 5 n. All BTREE, no secondary index.

Statements (the scenario drivers' own, `scenario1_backtest.py` compare-all/compare-one/cross-section, `scenario2_freight.py` customer-statement, `scenario3_library.py` join-no-literal):

| cell | statement shape | literal conjunct |
|---|---|---|
| compare-all | `model_results JOIN models ON model_id = id` | none |
| compare-one | the same `WHERE r.model_id = k` | equality |
| customer-statement | `freights JOIN cargos ON cargo_id = id WHERE c.org_id = k GROUP BY` | equality |
| cross-join | `daily_stats JOIN daily_bars JOIN symbols WHERE t.session_no = k` (4 connected orders forced; the two cross-product orders are not joinable) | equality |
| join-no-literal | `users JOIN loans ON l.user_id = u.id WHERE u.id BETWEEN 1 AND 16` | range pair |

Not searched, so not measured: scenario 1's `read-join-point` and `read-join-exists`, scenario 3's `join-loan-user` (pure tuple-key chains / subquery) and all of scenario 4 (no join; it is single-relation Cabin probing). Statements the planner does not search have no join order to flip.

Forced orders: `join_order = written` with the FROM clause spelled in the order. Planner arm P is the written spelling of the first order; the planner reorders it (its executed order is read off `ANALYZE`). Orders cut for the budget: `b-t-s` and `s-t-b` of `cross-join-10000` (the same orders are 20 ms and 0.27 ms per statement at 1K, i.e. quadratic and 2x worse, and the planner never picks them); `b-t-s` at 1K was time-capped at about 200 statements per block (per-statement figures are valid, equal-work is not).

## Question 1 - how wrong the constants are

Error factor = planner constant / true selectivity (above 1 is an over-estimate). The constants are equality 0.01 and range end 0.3. The true selectivity is a `COUNT(*)` with and without the conjunct on the same relation. **The two `u.id` rows are a tuple-key range, which the cost model does not price with the range-end constant** (`src/exec/join_order.cpp:152-153`: a tuple-key range is priced by its span band; the blind plan shows `est=0:16` for `u`); they are listed as measured selectivities only and are not part of the error claim.

| cell | conjunct | COUNT with | COUNT without | true selectivity | constant applied | error factor (constant / true) |
|---|---|---|---|---|---|---|
| compare-one-200 / 1K / 10K | r.model_id = 3 | 25 / 125 / 1250 | 200 / 1000 / 10000 | 0.125 | 0.01 | 0.08 (under by 12.5x), all sizes |
| customer-statement-200 | c.org_id = 3 | 10 | 200 | 0.05 | 0.01 | 0.2 |
| customer-statement-1K | c.org_id = 3 | 20 | 1000 | 0.02 | 0.01 | 0.5 |
| customer-statement-10K | c.org_id = 3 | 34 | 10000 | 0.0034 | 0.01 | 2.9 |
| cross-join-200 / 1K / 10K | t.session_no = 3 | 8 | 200 / 1000 / 10000 | 0.04 / 0.008 / 0.0008 | 0.01 | 0.25 / 1.25 / 12.5 |
| join-no-literal-200 / 1K / 10K | u.id <= 16 | 16 | 200 / 1000 / 10000 | 0.08 / 0.016 / 0.0016 | not applied by the cost model: tuple key range, priced by span | n/a |
| join-no-literal (all sizes) | u.id >= 1 | n | n | 1.0 | not applied by the cost model: tuple key range, priced by span | n/a |

A constant cannot be right at all three sizes for a fixed-count predicate: the true selectivity of "one value in k" falls as 1/n, the constant does not. **The constants actually applied are wrong by 0.08 (`compare-one`, under by 12.5x at every size) to 12.5 (`cross-join-10000`, over).**

**Joint versus product (the only premise S0 can give S3).** No scenario statement carries two literal conjuncts on different columns of one relation. The only multi-conjunct step is the `BETWEEN` pair on `u.id` (`>= 1` and `<= 16`), whose joint COUNT (16 of n) equals the product of its parts (1.0 x 16/n): ratio 1.00 at every size - **but that is a pair the cost model never prices as two conjuncts (a tuple-key range, priced by span)**, so it says nothing about correlation. **This run gives column-group statistics (CY-S3) no evidence for them**: the scenarios have no correlated column group to measure.

Estimate against observation, planner's own `ANALYZE` (full lines in `tables.md`). Blind (`gen=0`) every relation is priced at 100000 rows (`est=0:100000` for both `compare-all` relations) and the equality constant 0.01 gives 1000 for a filtered relation at every size: `compare-one` r step est 1000 against obs 25 / 125 / 1250 (40x over, 8x over, 0.8x); `cross-join` est 1000 on each of three steps against obs 8. Informed (`gen` > 0, `order=memo`) `est=` is the observed cardinality **rounded to its half-octave band** (`src/stats/plan_stats.cpp:128-133`, `src/exec/join_order.cpp:199-203`), off by up to about 19 %: `compare-all-10000` est 11585 against obs 10000, `compare-one-10000` 1448 against 1250, `join-no-literal` 91 against 80; `cross-join est=0:8,1:8,2:8` against obs 8. **That is for the prefixes of the order that ran; the orders not run keep the priors and constants on every compile** (`docs/inflight/known-gaps.md:1740-1746`). The informed order was nevertheless the fastest forced one here.

## Question 2 - which plans it costs (oracle: fastest forced order vs the planner's informed order)

The gate is evaluated per cell: (a) fastest forced order differs from the planner's informed order and is 15 % or more faster at p50, beyond the noise floor (the W2 control against the same-spelling W arm); (b) server CPU 15 % or more lower in every round (4 rounds here). Server CPU is the sum of the server's schedstat.

| cell | planner order | fastest forced | its p50 (us) | P p50 (us) | delta p50 | noise floor | (a) | (b) rounds (of 4) | (b) | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| compare-all-200 | m-r | m-r (same) | 323 | 339 | 4.7 % | 2.5 % | no | 1/4 | no | neither |
| compare-one-200 | m-r | m-r (same) | 78 | 78 | 0.0 % | 9.7 % | no | 0/4 | no | neither |
| customer-statement-200 | c-f | c-f (same) | 75 | 82 | 8.3 % | 3.8 % | no | 1/4 | no | neither |
| cross-join-200 | t-b-s | t-s-b | 91 | 114 | 20.1 % | 0.8 % | flagged, rejected | 0/4 | no | see below |
| join-no-literal-200 | u-l | u-l (same) | 130 | 131 | 0.4 % | 0.1 % | no | 0/4 | no | neither |
| compare-all-1000 | m-r | m-r (same) | 1502 | 1520 | 1.2 % | 0.1 % | no | 0/4 | no | neither |
| compare-one-1000 | m-r | m-r (same) | 177 | 182 | 2.8 % | 0.0 % | no | 0/4 | no | neither |
| customer-statement-1000 | c-f | c-f (same) | 171 | 170 | -0.6 % | 0.1 % | no | 0/4 | no | neither |
| cross-join-1000 | t-b-s | t-b-s (same) | 97 | 99 | 2.1 % | 2.1 % | no | 0/4 | no | neither |
| join-no-literal-1000 | u-l | u-l (same) | 256 | 255 | -0.6 % | 1.7 % | no | 0/4 | no | neither |
| compare-all-10000 | m-r | m-r (same) | 13216 | 13292 | 0.6 % | 1.9 % | no | 0/4 | no | neither |
| compare-one-10000 | m-r | m-r (same) | 1520 | 1489 | -2.1 % | 1.6 % | no | 0/4 | no | neither |
| customer-statement-10000 | c-f | c-f (same) | 1919 | 1931 | 0.6 % | 0.4 % | no | 0/4 | no | neither |
| cross-join-10000 | t-b-s | t-b-s (same; two quadratic orders not run) | 318 | 341 | 6.8 % | 2.0 % | no | 0/4 | no | neither |
| join-no-literal-10000 | u-l | u-l (same) | 1541 | 1554 | 0.9 % | 1.5 % | no | 0/4 | no | neither |

(The (b) column counts rounds where the fastest forced arm's CPU was 15 % below P's; where the forced arm is the planner's own order it is a same-order control and the count is incidental.)

**`cross-join-200`, the one flag, rejected from the per-round p50s.** Per-round p50 (us, rounds 1-4): W:t-s-b 129 / 126 / 81 / 80, W:t-b-s 129 / 129 / 80 / 81, P 133 / 132 / 81 / 81, W2 131 / 129 / 80 / 84. The two orders differ by at most 3 us in every round, and `t-s-b` is 15 % faster than P in 0 of 4 rounds by p50 as well as by CPU. The pooled-p50 gap (91 against P's 114 us) is a time-state shift: every arm ran about 128 us in rounds 1-2 and about 80 us in rounds 3-4, and the pooled p50 of arms with different round mixes lands in between; it is not a bimodal-distribution statement about one arm. Mean QPS 9523 (`t-s-b`) against 9292 / 9094 / 9046 (the three `t-b-s` arms), CPU 63 against 65-67 us. Both orders probe 8 rows by tuple key and differ only in which probe runs first; no selectivity enters the choice. By the p50 letter of CY-Q1 on pooled data it would pass; by rounds it does not. The W and W2 arms' pooled p50 differ from P by about 9-10 % (125 / 126 against 114), not 12-15 %.

**Same-order gaps.** P is slower than its own forced order beyond the noise floor in six cells (gap = (P - forced) / forced on pooled p50; noise floor = W2 against W of the same spelling): `compare-all-200` 4.9 % against 2.5 %, `customer-statement-200` 9.0 % against 3.8 %, `join-no-literal-200` 0.4 % against 0.1 %, `compare-all-1000` 1.3 % against 0.1 %, `compare-one-1000` 2.9 % against 0.0 %, `cross-join-10000` 7.3 % against 2.0 % (table "Same-order gap" in `tables.md`). This is the planned path's own overhead. It biases the comparison toward *finding* a flip (it makes P look slower), so the "no" is conservative.

What does cost: the orders the planner never picks. Forced against the planner's order (pooled p50, us), `customer-statement` `f-c` is 1.7x / 2.9x / 2.2x slower (131 / 500 / 4186 against 75 / 171 / 1919), `compare-one` `r-m` 1.30x / 1.2x / 1.2x (102 / 212 / 1880 against 78 / 177 / 1520), `compare-all` `r-m` 1.2x / 1.2x / 1.05x, `cross-join-1000` `s-t-b` 2.8x and `b-t-s` 205x. These are the stakes of a wrong order; the informed planner avoids all of them in this data.

## Question 3 - what a blind first compile costs

The F server (fresh, `gen=0`) printed `order=written` for every cell but `compare-one`. It did search: `switch=1` means the written order was the cheapest under blind inputs (every relation at 100000, the 0.01 constant, no posterior). Its order against the informed order:

| cell | blind order | informed order | differs | p50 us, blind order / informed order (200 rows, 1K, 10K) | blind cost (x) |
|---|---|---|---|---|---|
| compare-all | r-m (written) | m-r | yes | 394 / 323, 1761 / 1502, 13902 / 13216 | 1.22x, 1.17x, 1.05x |
| customer-statement | f-c (written) | c-f | **yes** | 131 / 75, 500 / 171, 4186 / 1919 | 1.75x, 2.9x, 2.2x |
| compare-one | m-r (planned, `switch=0.83`) | m-r | no | - | none |
| cross-join | t-b-s (written) | t-b-s | no | - | none |
| join-no-literal | u-l (written) | u-l | no | - | none |

Only the written spelling was measured blind; the opposite spelling of `compare-all` and `customer-statement` would plausibly blind-compile to the good order (not measured), so the blind cost depends on how the statement is written, as `parser-v2.md` I12 says of `written`. For the scenario drivers' own spellings two of five shapes pay, and `customer-statement` pays most. **The blind arm lacked priors and posteriors and applied the 0.01 constant; this run cannot separate the three as the cause**, so the cost is not attributable to the relation prior alone.

## Latency and CPU, all arms

p0 / p25 / p50 / p95 / p99 in microseconds over all four rounds pooled; QPS derived as 1e6 / mean latency; CPU is the server's schedstat per *completed* statement (mean of rounds; the driver divided by planned statements, `an.py` rescales to completed ones, which only changes `cross-join-1000 b-t-s`: 19785 us per statement, not 1978). Statement counts are `n`; `ops/block` is completed statements per block (time-capped for `cross-join-1000 b-t-s`).

| cell | arm | ops/block | n | p0 (us) | p25 (us) | p50 (us) | p95 (us) | p99 (us) | QPS (derived, 1/s) | server CPU (us/statement) |
|---|---|---|---|---|---|---|---|---|---|---|
| compare-all-200 | P:r-m | 777 | 3108 | 291 | 309 | 339 | 483 | 552 | 2825 | 141 |
| compare-all-200 | W2:r-m | 777 | 3108 | 336 | 366 | 404 | 533 | 591 | 2413 | 188 |
| compare-all-200 | W:m-r | 777 | 3108 | 287 | 301 | 323 | 464 | 532 | 2893 | 131 |
| compare-all-200 | W:r-m | 777 | 3108 | 333 | 356 | 394 | 534 | 593 | 2461 | 185 |
| compare-one-200 | W:m-r | 2000 | 8000 | 70 | 72 | 78 | 142 | 199 | 9987 | 50 |
| compare-one-200 | W2:r-m | 2000 | 8000 | 76 | 79 | 92 | 157 | 223 | 8895 | 58 |
| compare-one-200 | P:r-m | 2000 | 8000 | 72 | 76 | 78 | 142 | 178 | 11314 | 49 |
| compare-one-200 | W:r-m | 2000 | 8000 | 76 | 80 | 102 | 153 | 219 | 8875 | 59 |
| customer-statement-200 | W:c-f | 2000 | 8000 | 67 | 72 | 75 | 127 | 157 | 11151 | 64 |
| customer-statement-200 | W:f-c | 2000 | 8000 | 124 | 127 | 131 | 186 | 247 | 7047 | 117 |
| customer-statement-200 | P:f-c | 2000 | 8000 | 73 | 76 | 82 | 134 | 157 | 10172 | 71 |
| customer-statement-200 | W2:f-c | 2000 | 8000 | 126 | 129 | 136 | 188 | 233 | 6736 | 122 |
| cross-join-200 | W:s-t-b | 2000 | 8000 | 106 | 109 | 113 | 171 | 190 | 7955 | 89 |
| cross-join-200 | W2:t-b-s | 2000 | 8000 | 77 | 82 | 126 | 142 | 157 | 9046 | 66 |
| cross-join-200 | P:t-b-s | 2000 | 8000 | 78 | 81 | 114 | 147 | 160 | 9094 | 67 |
| cross-join-200 | W:t-b-s | 2000 | 8000 | 77 | 80 | 125 | 141 | 152 | 9292 | 65 |
| cross-join-200 | W:t-s-b | 2000 | 8000 | 77 | 80 | 91 | 138 | 154 | 9523 | 63 |
| cross-join-200 | W:b-t-s | 2000 | 8000 | 940 | 994 | 1015 | 1111 | 1536 | 967 | 947 |
| join-no-literal-200 | P:u-l | 2000 | 8000 | 123 | 127 | 131 | 207 | 252 | 6759 | 101 |
| join-no-literal-200 | W2:u-l | 2000 | 8000 | 122 | 126 | 131 | 200 | 227 | 6685 | 99 |
| join-no-literal-200 | W:l-u | 2000 | 8000 | 139 | 144 | 149 | 214 | 250 | 6099 | 117 |
| join-no-literal-200 | W:u-l | 2000 | 8000 | 122 | 126 | 130 | 199 | 224 | 6848 | 98 |
| compare-all-1000 | W:m-r | 175 | 700 | 1359 | 1447 | 1502 | 1676 | 1903 | 658 | 515 |
| compare-all-1000 | W2:r-m | 175 | 700 | 1598 | 1702 | 1760 | 1906 | 2081 | 565 | 747 |
| compare-all-1000 | P:r-m | 175 | 700 | 1364 | 1465 | 1520 | 1713 | 2045 | 651 | 521 |
| compare-all-1000 | W:r-m | 175 | 700 | 1610 | 1709 | 1761 | 1931 | 2082 | 563 | 760 |
| compare-one-1000 | W:m-r | 1356 | 5424 | 169 | 174 | 177 | 213 | 282 | 5463 | 84 |
| compare-one-1000 | W:r-m | 1356 | 5424 | 202 | 208 | 212 | 261 | 379 | 4533 | 120 |
| compare-one-1000 | P:r-m | 1356 | 5424 | 170 | 178 | 182 | 215 | 295 | 5328 | 89 |
| compare-one-1000 | W2:r-m | 1356 | 5424 | 197 | 208 | 212 | 313 | 435 | 4416 | 124 |
| customer-statement-1000 | P:f-c | 587 | 2348 | 161 | 165 | 170 | 206 | 284 | 5676 | 153 |
| customer-statement-1000 | W:f-c | 587 | 2348 | 451 | 481 | 500 | 564 | 685 | 1965 | 449 |
| customer-statement-1000 | W:c-f | 587 | 2348 | 158 | 163 | 171 | 212 | 317 | 5619 | 155 |
| customer-statement-1000 | W2:f-c | 587 | 2348 | 454 | 481 | 500 | 567 | 693 | 1966 | 448 |
| cross-join-1000 | W:t-s-b | 2000 | 8000 | 92 | 95 | 98 | 136 | 172 | 9620 | 72 |
| cross-join-1000 | W:t-b-s | 2000 | 8000 | 93 | 95 | 97 | 120 | 151 | 9905 | 70 |
| cross-join-1000 | P:t-b-s | 2000 | 8000 | 94 | 97 | 99 | 148 | 178 | 9376 | 73 |
| cross-join-1000 | W2:t-b-s | 2000 | 8000 | 92 | 96 | 99 | 153 | 173 | 9125 | 74 |
| cross-join-1000 | W:s-t-b | 2000 | 8000 | 233 | 252 | 268 | 334 | 377 | 3620 | 213 |
| cross-join-1000 | W:b-t-s | 201 | 800 | 19561 | 19770 | 19910 | 20865 | 22050 | 50 | 19785 |
| join-no-literal-1000 | W:l-u | 1164 | 4656 | 252 | 268 | 283 | 361 | 507 | 3371 | 224 |
| join-no-literal-1000 | W2:u-l | 1164 | 4656 | 224 | 239 | 252 | 326 | 450 | 3791 | 201 |
| join-no-literal-1000 | P:u-l | 1164 | 4656 | 226 | 241 | 255 | 331 | 402 | 3753 | 202 |
| join-no-literal-1000 | W:u-l | 1164 | 4656 | 224 | 241 | 256 | 333 | 423 | 3731 | 201 |
| compare-all-10000 | W:m-r | 30 | 120 | 12929 | 13100 | 13216 | 14207 | 15503 | 75 | 4151 |
| compare-all-10000 | W:r-m | 30 | 120 | 13513 | 13790 | 13902 | 14426 | 15036 | 72 | 6675 |
| compare-all-10000 | P:r-m | 30 | 120 | 13007 | 13189 | 13292 | 13768 | 14292 | 75 | 4179 |
| compare-all-10000 | W2:r-m | 30 | 120 | 13786 | 14050 | 14173 | 14595 | 15138 | 70 | 6973 |
| compare-one-10000 | P:r-m | 157 | 628 | 1383 | 1459 | 1489 | 1643 | 1683 | 663 | 646 |
| compare-one-10000 | W:r-m | 157 | 628 | 1732 | 1817 | 1880 | 2028 | 2588 | 528 | 1012 |
| compare-one-10000 | W:m-r | 157 | 628 | 1372 | 1476 | 1520 | 1651 | 1903 | 654 | 659 |
| compare-one-10000 | W2:r-m | 157 | 628 | 1704 | 1805 | 1850 | 1980 | 2182 | 534 | 989 |
| customer-statement-10000 | P:f-c | 70 | 280 | 1827 | 1898 | 1931 | 2144 | 2594 | 507 | 1869 |
| customer-statement-10000 | W2:f-c | 70 | 280 | 4060 | 4164 | 4205 | 4443 | 4582 | 236 | 4108 |
| customer-statement-10000 | W:c-f | 70 | 280 | 1812 | 1886 | 1919 | 2052 | 2253 | 517 | 1832 |
| customer-statement-10000 | W:f-c | 70 | 280 | 4040 | 4126 | 4186 | 4461 | 4958 | 236 | 4112 |
| cross-join-10000 | W:t-s-b | 853 | 3412 | 277 | 306 | 331 | 407 | 471 | 2957 | 268 |
| cross-join-10000 | W2:t-b-s | 853 | 3412 | 278 | 302 | 324 | 394 | 463 | 3011 | 264 |
| cross-join-10000 | P:t-b-s | 853 | 3412 | 280 | 320 | 341 | 420 | 468 | 2865 | 281 |
| cross-join-10000 | W:t-b-s | 853 | 3412 | 277 | 299 | 318 | 401 | 510 | 3035 | 263 |
| join-no-literal-10000 | W:l-u | 181 | 724 | 1483 | 1554 | 1577 | 1687 | 2101 | 627 | 1477 |
| join-no-literal-10000 | W:u-l | 181 | 724 | 1440 | 1504 | 1541 | 1659 | 1916 | 644 | 1446 |
| join-no-literal-10000 | P:u-l | 181 | 724 | 1446 | 1524 | 1554 | 1691 | 2041 | 633 | 1466 |
| join-no-literal-10000 | W2:u-l | 181 | 724 | 1449 | 1538 | 1564 | 1660 | 1803 | 636 | 1453 |

## Validity limit - the host's two-state shift

Every server showed a two-state shift of about +55 us latency and +20-25 us server CPU per statement at 200 rows, often flipping mid-block (`compare-one-200` `W:m-r` round 1: 75 to 134 us; `join-no-literal-200` round 2: every arm about 190 us; `cross-join-200`: every arm about 128 us in rounds 1-2 and about 80 us in rounds 3-4). The pooled noise-floor column hides it (W2 and W of the same spelling can sit in the same state and agree to 0.1 %). Per-round p50s for the 200-row cells are in `tables.md`. All three measured servers were pinned to the **same CPU** and the client was unpinned; **the cause was not verified**. It is a validity limit on every 200-row number, not a cut. The verdict survives it because every losing order loses in every round: the forced orders worse than the planner's are worse in all 4 rounds, and no forced order is 15 % better in any round (`an.py` table "Every forced order"; that table was recomputed from `results.json.gz`).

## What this teaches about the engine

- **Column statistics would not have changed a scenario plan here.** The planner's informed order equalled the fastest forced order in 14 of 15 cells and the fifteenth is a time-state artifact. The wrong constants did not matter to the orders measured because, for a shape that has run, `est=` is the observed cardinality rounded to its half-octave band, for the prefixes of the executed order; the orders not run keep the constants. On this data the informed order was the best order, so no constant was exercised to a loss. CY-Q1 (a) is not met by a scenario statement and (b) is not met by any. By the operator's gate S2 and S3 are not to be built on this evidence.
- **What the data cannot say.** The scenario columns are uniform (a model id, a session number, an organization: one value in k), so there is no skewed column and no pair of correlated columns; the hot-value/rare-value case a statistic exists for is not in the scenarios. The optional synthetic Zipf cell did not run (budget). The only multi-conjunct step is a tuple-key range the cost model never prices as two conjuncts.
- **The cost that is real is the cold compile.** `customer-statement` blind is 1.75-2.9x slower than informed on its written spelling. That is CX-Q19's population. CY-Q1 already says CY-S1 does not wait for this gate. CY-Q10 (not persisting posteriors) acquires its first data point: the shape's first order is wrong with no prior, no posterior and a constant in play, and this run cannot separate them, so it does not say which a persisted prior alone would cure.
- **Planned-path overhead** is above its noise floor in six cells (0.4 % to 9.0 %, table in the same-order paragraph), at 200 and 1K rows and at `cross-join-10000`.

## What was cut, and what did not run

- The synthetic Zipf cell did not run (budget); no skewed-column evidence either way.
- Scenario data is synthetic to the scenario schemas' shapes, not loaded by the scenario drivers. Scenario 3's `join-loan-user`, scenario 1's `read-join-point` / `read-join-exists` and scenario 4 entirely are not searched joins and were not measured. Sizes are scaled to 200 / 1K / 10K relation rows, not the drivers' own sizes.
- Two quadratic orders of `cross-join-10000` not run; `cross-join-1000 b-t-s` time-capped at about 200 statements per block. The row check compared block row totals, not result sets; its one mismatch is the time-capped `cross-join-1000 b-t-s`.
- 4 rounds, not more; the three-way `ANALYZE` of a second spelling per cell was not collected; est/obs is for the first spelling of each cell only; selectivity COUNTs use one literal (value 3) per cell, not a sweep.
- No test suite was run (no code changed by this stage); no `--verify` mode exists for these drivers' reduced use.
