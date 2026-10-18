# CC-S0: what real relation sizes would buy the join order

Work order: `instructions/v3.2.0/workorder-cc-plan-statistics.md` (§9 CC-S0, §10). Raw files, the
ANALYZE of every statement on every arm and the harness:
`bench/v3.2.0/archive/cc-s0-premise-v3.2.0-178-g65dc45c5/`.

**Finding.** The gate is met. On `v3.2.0-178-g65dc45c5`, choosing the other join order for the two
scenario statements cuts server CPU per statement by 21.6-38.1% on scenario 1's compare-all at every
size from 200 to 10,000 result rows, and by 32.2-60.7% on scenario 2's customer-statement from 180 to
8,234 freights (every one of the 3 interleaved pairs, so the minimum is the figure that counts).
CB's data-free planner picks the slow order in all twelve of those cells and in both spellings of each
statement, because it prices every relation at 100,000 rows. The flip is not universal: at scenario 2's
driver defaults (200,000 cargos, 10,795 freights) the alternative saves 0.7-5.3% per pair, which is
inside the noise floor, so a planner that flips on the 100,000-row prior alone would be right at 10k
and indifferent at 200k, and one that reads real sizes distinguishes them. The relation-size prior of
CC §1 is within 0.75-0.98x of the true row count on all 30 measured relations of height 2 or more,
and would be wrong by 4-7x on 14 of the 22 height-1 relations if the leaf-fill formula were applied
there, which is why the height-1 exact slot count is load-bearing. CB's order search costs about
21 us per 3-relation statement (a FilterScan + Probe + Probe read), 25-32% of the statement at 200-1,000
rows and under 2% at 60,480. The seed step could not be timed: no steward exists at this commit.

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-18; build 11:54:38-11:55:48 UTC; load 11:56-12:01:33 UTC (not measured); measurement 12:01:49-12:04:58 UTC; relation statistics 12:06 UTC |
| Worktree / branch | `cc-stats-planner` / `worktree-cc-stats-planner` |
| `git describe --tags` | `v3.2.0-178-g65dc45c5` (commit time 2026-10-18 11:05:45 UTC) |
| Tree | The binary was built from `git archive 65dc45c5` extracted to `/home/cdkbs/bench-runs/cc-s0/src`, so it is the clean commit. The worktree itself carried another session's uncommitted edits to `src/exec/step_compiler.cpp`, `src/exec/join_order.cpp`, `include/kds/exec/join_order.hpp`, `include/kds/exec/step_compiler.hpp` and `src/server/command_dispatcher.cpp` (appeared 11:54); an in-tree build at 11:54 failed on them (`ChainCost` undeclared) and was discarded. None of it is in the measured binary. HEAD of the branch, `2cbefa4d`, is docs-only on top of the measured commit |
| Binary | `/home/cdkbs/bench-runs/cc-s0/kds_server`, sha256 `8ce595c65ad1dcdc003443d96896d4c6f6b346fa262bf05b9a1de7c9d1ce495e`. Source binary `src/build-release/kds_server` mtime 2026-10-18 11:55:47.9, copy 11:56:07; both newer than the commit (11:05:45), so it is the engine at `65dc45c5`. Every server, including the loader, was started from the copy |
| Build type | Release (`CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`), no frame pointers |
| Device | `/dev/root`, ext4, data files under `/home/cdkbs/bench-runs/cc-s0/` (`df -T` checked). Not tmpfs |
| Host | AMD EPYC 9V74, 8 CPUs. Loadavg 1.03-1.39 at the start of every measured block (156 blocks), `pgrep cc1plus|cmake --build|ctest` empty in all 156. Before the measurement, from 11:52 to about 11:59, another session's `kds_tests` run held loadavg at 8-15; the build and the load phase overlapped it, the measurement did not |
| Server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, default `inline_cell_width` (64), `join_order = planned` on server P and `written` on W and W2, each pinned to CPU 2 with `taskset`; ports 25601-25603 |
| Dataset | One loader server wrote every relation once; its file was copied to P, W and W2, so all three servers hold byte-identical data. Result rows were compared across all five arms before timing: the row multiset is equal in all 12 cells |
| Gate metric | Server CPU per statement: the sum over every server thread of `/proc/<pid>/task/*/schedstat` run-time, taken before and after each block and divided by its statement count. `tools/kds_sampler.c` was not used: it splits CPU, it does not total it better than the scheduler's own counter, and no split was needed for a gate on the total |

## What was measured

Five arms per statement, run against three servers started from one data file. "A" is the spelling
the scenario driver writes; "B" is the same statement with the two relations swapped in the `FROM`
clause. Under `join_order = written` the SQL order is the execution order, so W:A and W:B are the two
hand-forced orders.

| Arm | Server | Spelling | What it is |
|---|---|---|---|
| P:A | P (`join_order = planned`) | A | CB's data-free planner on the driver's spelling |
| P:B | P | B | the same planner on the swapped spelling |
| W:A | W (`written`) | A | the driver's order, forced |
| W:B | W | B | the swapped order, forced; the candidate flip |
| W2:A | W2 (`written`) | A | a second server running W:A's statements: the noise-floor control |

Each cell runs the five arms in a rotating order (reversed on alternate cells and rounds), 3 rounds,
the same statement sequence on every arm (seeded per cell and round), 10% of the block as warm-up
before the timed block. Ops per block were fixed per cell from one calibration (about 1.2 s of W2:A) and are
the same on every arm: 3,000 / 810 / 299 / 89 for compare-all at 200 / 1K / 2,872 / 10K rows;
3,000 / 2,224 / 280 / 154 for customer-statement; 3,000 / 3,000 / 3,000 / 725 for the cross-join.

### Mapping from the sizes to the drivers

| Cell | Statement | Rows | How the data was made |
|---|---|---|---|
| c1 compare-all | `tools/scenario1_backtest.py` `compare_all_sql`: `FROM model_results AS r JOIN models AS m ON r.model_id = m.id` | 8 models and 200 / 1,000 / **2,872 (driver default)** / 10,000 results | The driver's own `SCHEMA` for `models` and `model_results` (BTREE), synthetic values, ids `(i-1) % 8 + 1` as the driver writes them. Not loaded by the driver: its default run also loads 60k bars and takes minutes, and only these two relations are read |
| c2 customer-statement | `tools/scenario2_freight.py`: `SELECT c.org_id, SUM(f.cbm) FROM freights AS f JOIN cargos AS c ON f.cargo_id = c.id WHERE c.org_id = X GROUP BY c.org_id` | 200 cargos / 180 freights; 1,000 / 879; **10,000 / 8,234 (the CA size)**; **200,000 / 10,795 (driver defaults)** | The scenario 2 driver itself, `--no-manifest --bookers 1 --seed 1`; sizes by `--cargos` and `--bookings` (200/180, 1000/900, 10000/9000, with `--organizations` 20 / 50 / 300, `--ships` 5 / 10 / 30, `--operations` 20 / 50 / 300) and the defaults with `--seconds 25`. Freights are fewer than `--bookings` because the driver rejects bookings on capacity and credit. X drawn from 1..organizations |
| c3 cross-join | `cross_section_sql`: `FROM daily_stats AS t JOIN daily_bars AS b ON t.bar_id = b.id JOIN symbols AS s ON t.symbol_id = s.id WHERE t.session_no = N` | 200 / 1,000 / 10,000 / **60,480 (driver default, 30 years x 8 symbols)** stats rows | The driver's `SCHEMA` for the three relations, synthetic values, N drawn per statement |

## Result 1: scenario 1 compare-all flips by 22-38%, at every size

CB's planner leaves the statement as the driver wrote it (`Scan model_results` + `Probe models`, one
pk descent per result row) and puts the swapped spelling back into the same order (`reordered=1`
on P:B). The forced alternative, `Scan models` (8 rows) then `Scan model_results` with a statement-local
build on `model_id`, replaces `n` descents with 9 opens. It saves 21.6% of server CPU per statement
at 200 rows and 34.5-38.1% from 1,000 rows up, in every pair.

| Rows (model_results) | Planned (P:A) | Written A | Written B | B saves vs planned, per pair | Minimum | Control W2 vs W:A | Gate |
|---|---|---|---|---|---|---|---|
| 200 | 152.2 us | 148.2 us | 119.3 us | 21.7%, 21.6%, 21.7% | 21.6% | +0.1% | PASS |
| 1,000 | 640.0 us | 626.3 us | 418.3 us | 35.7%, 34.5%, 34.7% | 34.5% | +2.5% | PASS |
| 2,872 (driver default) | 1,756.7 us | 1,740.9 us | 1,084.0 us | 38.1%, 38.3%, 38.2% | 38.1% | +0.7% | PASS |
| 10,000 | 5,757.8 us | 5,767.9 us | 3,612.6 us | 37.3%, 37.3%, 38.6% | 37.3% | +0.7% | PASS |

The saving is a per-row one. A line fitted through the 200-row and 10,000-row cells (derived from the
medians above, not a separate measurement) gives 0.573 us per result row on the forced A order and
0.357 us on B, and a fixed part of 34 us on A against 48 us on B. So
B's fixed part is 14 us higher and its cost 0.22 us lower on every row, which puts the break-even near
65 rows (14 / 0.22, derived) and is why the saving rises from 21.6% at 200 rows to 37-38% and stops
rising. The plans (`ANALYZE`, whole table in
the archive) show where it comes from:

| Rows | Order | examined | pages | opens |
|---|---|---|---|---|
| 200 | A: Scan r, Probe m | 400 | 202 | 201 |
| 200 | B: Scan m, Scan r + build | 383 | 17 | 9 |
| 2,872 | A | 5,744 | 2,898 | 2,873 |
| 2,872 | B | 5,393 | 209 | 9 |
| 10,000 | A | 20,000 | 10,091 | 10,001 |
| 10,000 | B | 18,758 | 729 | 9 |

## Result 2: scenario 2 customer-statement flips by 32-61% up to the CA size and by nothing at 200k cargos

The planner again keeps the driver's order (`Scan freights` + `Probe cargos`, filter `org_id` after
the probe) in both spellings. The alternative is `FilterScan cargos` on `org_id`, then `Scan freights`
with a build on `cargo_id`. It wins by 41.7-60.7% at 200 and 1,000 cargos and 32.2-46.2% at the CA
size, and loses its advantage when cargos reach 200,000.

| Cargos / freights | Planned (P:A) | Written A | Written B | B saves vs planned, per pair | Minimum | Control W2 vs W:A | Gate |
|---|---|---|---|---|---|---|---|
| 200 / 180 | 136.1 us | 131.2 us | 74.0 us | 46.1%, 45.5%, 41.7% | 41.7% | +0.8% | PASS |
| 1,000 / 879 | 490.3 us | 489.6 us | 182.2 us | 62.8%, 62.8%, 60.7% | 60.7% | -0.1% | PASS |
| 10,000 / 8,234 (CA size) | 4,307.1 us | 4,322.6 us | 2,325.1 us | 46.2%, 32.2%, 46.2% | 32.2% | +0.1% | PASS |
| 200,000 / 10,795 (defaults, 1 booker) | 7,262.4 us | 7,297.9 us | 7,094.6 us | 5.3%, 0.7%, 2.3% | 0.7% | -0.2% | FAIL |

The CA-size cell has one slow B round (2,918.9 us against 2,262.3 and 2,325.1; a 28.2% spread). The
other two B rounds save 46.2%, the slow one 32.2%, and the gate holds on the slow one.

The default-size cell is not a failure of the idea, it is the crossover. The plans:

| Cargos / freights | Order | examined | pages | opens |
|---|---|---|---|---|
| 10,000 / 8,234 | A: Scan f, Probe c | 16,468 | 8,313 | 8,235 |
| 10,000 / 8,234 | B: FilterScan c, Scan f + build | 18,264 | 209 | 46 |
| 200,000 / 10,795 | A | 21,590 | 10,898 | 10,796 |
| 200,000 / 10,795 | B | 210,803 | 2,111 | 103 |

B examines ten times the rows at 200k cargos and costs the same, because a rejected row is rejected
on its bytes before any decode (BM) at no more than about 34 ns a row (7,095 us over 210,803 rows examined, an upper bound that includes everything else the statement does), while each freight's probe is a descent. The
two relations are 10k and 8k rows at the CA size and 200k and 10.8k at the defaults; the order that
wins flips on how many probes there are against how many rows the other side rejects, and only the
sizes of both relations say which. Cargos per organization at the defaults is 100 and freights per
organization about 5.4, against 33 and 27 at the CA size.

## Result 3: CB's order search costs about 21 us per 3-relation statement

On scenario 1's backtest read (`FilterScan` + `Probe` + `Probe`) the planner chooses the written order
in every cell (`ANALYZE` of P and W are identical), so the planned minus written difference is the
search alone.

| Stats rows | Planned (P:A) | Written (W:A) | Control W2 | Planned minus written, per pair | Median | Planned over written |
|---|---|---|---|---|---|---|
| 200 | 86.9 us | 65.8 us | 65.1 us | 21.0, 23.1, 20.5 us | 21.0 us | +32.0% |
| 1,000 | 109.6 us | 88.0 us | 86.6 us | 22.3, 21.2, 22.0 us | 22.0 us | +24.6% |
| 10,000 | 349.5 us | 332.0 us | 332.3 us | 20.7, 24.3, 7.1 us | 20.7 us | +5.3% |
| 60,480 (driver default) | 1,634.6 us | 1,607.4 us | 1,614.4 us | 34.3, 2.1, 31.5 us | 31.5 us | +1.7% |

At 60,480 rows the scan is 1.6 ms and the pairs scatter by more than the search costs (the control
differs from W:A by -11.6, +6.5 and +7.0 us), so that row is bounded by the noise floor and not a
measurement of the search; the 21 us of the three smaller cells is. For two relations
(compare-all, customer-statement) the search is 4.0 us at 200 rows (c1: 152.2 against 148.2 us, 2.7%;
c2: 136.1 against 131.2 us, 3.7%, on a cell whose rounds spread 11%) and inside the noise floor from 1,000
rows up. The step compiler's own comment gives 15 us for the six orders of a three-relation point
join (`src/exec/step_compiler.cpp:2155-2157`), and that shape is now skipped; the 21 us here is the
price of a search that is not skipped.

## Result 4: the relation-size prior is inside 2x where it applies

`sys.relation_stats` at this commit carries `height` and `leaves` (walked), no estimate. The CC §1
prior was therefore computed offline from them: height 1 reads the root's live slots (exact), height 2
has an exact leaf count, rows = leaves x `LeafRowCapacity(row_size)` x 0.75. Capacity is
`floor(8140 / (25 + row_size))` with 8140 = `kMaxTuplePayloadSize` (8115) + `kSlotOnDiskSize` (5) + the
20-byte tuple header, and `row_size` is the sum of the declared column widths (int64 8, int32 4,
varchar 64, no null bitmap; checked against the fullest leaves: `cargos` capacity 100, observed 100.0
rows per leaf, `charges` 133 against 132.9). The three height-3 relations are scored on their walked
leaf count; the engine's random-path estimator over the internal levels is not built and its error is
not in these rows.

| Relation | Height | Leaves | Live rows | Prior | Prior / true |
|---|---|---|---|---|---|
| `model_results` 200 / 1K / 2,872 / 10K | 2 | 2 / 10 / 26 / 91 | 200 / 1,000 / 2,872 / 10,000 | 166 / 832 / 2,164 / 7,576 | 0.83 / 0.83 / 0.75 / 0.76 |
| `daily_bars` 200 / 1K / 10K | 2 | 3 / 12 / 115 | 200 / 1,000 / 10,000 | 196 / 783 / 7,504 | 0.98 / 0.78 / 0.75 |
| `daily_stats` 200 / 1K / 10K | 2 | 3 / 15 / 145 | 200 / 1,000 / 10,000 | 155 / 776 / 7,504 | 0.78 / 0.78 / 0.75 |
| `daily_bars` 60,480 | 3 | 696 | 60,480 | 45,414 | 0.75 |
| `daily_stats` 60,480 | 3 | 877 | 60,480 | 45,385 | 0.75 |
| `cargos` 200 / 1K / 10K (CA) | 2 | 2 / 10 / 100 | 200 / 1,000 / 10,000 | 150 / 750 / 7,500 | 0.75 |
| `cargos` 200,000 (defaults) | 3 | 2,000 | 200,000 | 150,000 | 0.75 |
| `freights` 180 / 879 / 8,234 / 10,795 | 2 | 2 / 9 / 79 / 103 | 180 / 879 / 8,234 / 10,795 | 158 / 709 / 6,221 / 8,111 | 0.88 / 0.81 / 0.76 / 0.75 |
| `charges` 1,019 / 4,929 / 46,338 / 60,884 | 2 | 8 / 38 / 349 / 458 | same | 798 / 3,790 / 34,813 / 45,686 | 0.78 / 0.77 / 0.75 / 0.75 |
| `organizations`, `operations`, `ships` (the height-2 members) | 2 | 2-65 | 50-2,000 | | 0.76-0.93 |

All 30 height-2 and height-3 relations (full table in `relation-stats-vs-true-counts.json`) fall in
0.75-0.98x. The 0.75 floor is the assumed fill against leaves that are in fact full (an ascending
key appends into full leaves; `cargos` holds 100.0 rows per 100-row leaf), so the prior is biased low by the
assumed-fill margin on every append-built tree. The 22 height-1 relations (`models`, `symbols`,
`fees`, `recipes`, `ships`, `operations` at the small sizes) hold 5-93 rows in one leaf. The leaf formula
would estimate them at 6.5-6.8x (8-row `models`, `symbols`), 4.2x (12-row `fees`, 20-row `operations`) and
4.7x (5-row `ships`), so the §1 rule that a root leaf is counted, not estimated, is what keeps the prior in
range there. Only append-built trees were measured; a tree grown by random-order inserts has leaves at about
0.69 fill and was not built here.

## Where the time goes

These are read statements with no commit, so there is no fsync or lock wait to account for. A statement's
latency as the client sees it is the server's CPU plus the client's decode and the socket round trip; the
last column is the server's share of the client's p50.

| Cell | Server CPU share of p50, W:A | Server CPU share of p50, W:B |
|---|---|---|
| c1 compare-all 200 / 1K / 2,872 / 10K rows | 45% / 43% / 43% / 41% | 40% / 34% / 31% / 30% |
| c2 customer-statement 200 / 1K / 10K / 200K cargos | 89% / 96% / 99% / 99% | 77% / 88% / 98% / 99% |
| c3 cross-join 200 / 1K / 10K / 60K rows | 71% / 77% / 90% / 97% | not run |

compare-all returns every row (the client walks 200-10,000 rows of 7 fields per reply, in Python), so
more than half of its p50 is client work that the join order does not touch; the flip's 37% CPU saving is
a 15% saving in the client's p50 at 2,872 rows (4,083 to 3,473 us). customer-statement returns one row
and is server-bound, so its CPU saving is its latency saving.

## Latency distributions

Pooled over the 3 rounds of each arm (client-observed, one connection, `tools/kwp.py` `query`, which
counts and walks the fields without building Python objects). Statements = 3 x ops per round.

| Cell | Arm | Statements | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| c1 compare-all 200 | P:A | 9,000 | 311 us | 322 us | 328 us | 352 us | 400 us |
| c1 compare-all 200 | W:A | 9,000 | 308 us | 320 us | 327 us | 363 us | 416 us |
| c1 compare-all 200 | W:B | 9,000 | 279 us | 292 us | 297 us | 322 us | 396 us |
| c1 compare-all 200 | W2:A | 9,000 | 308 us | 320 us | 325 us | 354 us | 421 us |
| c1 compare-all 1K | P:A | 2,430 | 1,407 us | 1,451 us | 1,473 us | 1,578 us | 1,646 us |
| c1 compare-all 1K | W:A | 2,430 | 1,406 us | 1,436 us | 1,453 us | 1,573 us | 1,619 us |
| c1 compare-all 1K | W:B | 2,430 | 1,194 us | 1,222 us | 1,241 us | 1,346 us | 1,407 us |
| c1 compare-all 1K | W2:A | 2,430 | 1,415 us | 1,463 us | 1,485 us | 1,544 us | 1,683 us |
| c1 compare-all 2,872 | P:A | 897 | 3,957 us | 4,049 us | 4,103 us | 4,336 us | 4,736 us |
| c1 compare-all 2,872 | W:A | 897 | 3,966 us | 4,034 us | 4,083 us | 4,295 us | 4,678 us |
| c1 compare-all 2,872 | W:B | 897 | 3,336 us | 3,422 us | 3,473 us | 3,620 us | 3,685 us |
| c1 compare-all 2,872 | W2:A | 897 | 3,975 us | 4,066 us | 4,117 us | 4,284 us | 4,501 us |
| c1 compare-all 10K | P:A | 267 | 13,422 us | 13,711 us | 14,048 us | 14,760 us | 14,945 us |
| c1 compare-all 10K | W:A | 267 | 13,476 us | 13,833 us | 14,111 us | 14,607 us | 15,200 us |
| c1 compare-all 10K | W:B | 267 | 11,364 us | 11,758 us | 11,978 us | 12,518 us | 13,465 us |
| c1 compare-all 10K | W2:A | 267 | 13,589 us | 13,794 us | 14,104 us | 14,499 us | 14,635 us |
| c2 customer-statement 200 | P:A | 9,000 | 131 us | 151 us | 165 us | 197 us | 211 us |
| c2 customer-statement 200 | W:A | 9,000 | 132 us | 144 us | 147 us | 175 us | 195 us |
| c2 customer-statement 200 | W:B | 9,000 | 77 us | 90 us | 97 us | 132 us | 141 us |
| c2 customer-statement 200 | W2:A | 9,000 | 131 us | 145 us | 160 us | 193 us | 212 us |
| c2 customer-statement 1K | P:A | 6,672 | 476 us | 507 us | 516 us | 557 us | 631 us |
| c2 customer-statement 1K | W:A | 6,672 | 476 us | 502 us | 511 us | 552 us | 694 us |
| c2 customer-statement 1K | W:B | 6,672 | 179 us | 195 us | 208 us | 247 us | 275 us |
| c2 customer-statement 1K | W2:A | 6,672 | 472 us | 507 us | 518 us | 556 us | 595 us |
| c2 customer-statement 10K (CA) | P:A | 840 | 4,166 us | 4,227 us | 4,317 us | 4,577 us | 4,991 us |
| c2 customer-statement 10K (CA) | W:A | 840 | 4,172 us | 4,237 us | 4,353 us | 4,580 us | 5,773 us |
| c2 customer-statement 10K (CA) | W:B | 840 | 2,272 us | 2,311 us | 2,364 us | 2,979 us | 3,085 us |
| c2 customer-statement 10K (CA) | W2:A | 840 | 4,190 us | 4,259 us | 4,394 us | 4,578 us | 4,955 us |
| c2 customer-statement 200K (defaults) | P:A | 462 | 7,162 us | 7,259 us | 7,338 us | 7,794 us | 8,353 us |
| c2 customer-statement 200K (defaults) | W:A | 462 | 7,228 us | 7,318 us | 7,364 us | 7,724 us | 9,239 us |
| c2 customer-statement 200K (defaults) | W:B | 462 | 6,970 us | 7,091 us | 7,148 us | 7,439 us | 8,102 us |
| c2 customer-statement 200K (defaults) | W2:A | 462 | 7,195 us | 7,295 us | 7,370 us | 7,806 us | 8,459 us |
| c3 cross-join 200 | P:A | 9,000 | 107 us | 111 us | 113 us | 140 us | 158 us |
| c3 cross-join 200 | W:A | 9,000 | 89 us | 91 us | 92 us | 114 us | 128 us |
| c3 cross-join 200 | W2:A | 9,000 | 84 us | 90 us | 97 us | 135 us | 145 us |
| c3 cross-join 1K | P:A | 9,000 | 129 us | 133 us | 136 us | 160 us | 173 us |
| c3 cross-join 1K | W:A | 9,000 | 109 us | 112 us | 115 us | 136 us | 148 us |
| c3 cross-join 1K | W2:A | 9,000 | 106 us | 112 us | 114 us | 132 us | 143 us |
| c3 cross-join 10K | P:A | 9,000 | 357 us | 376 us | 385 us | 425 us | 463 us |
| c3 cross-join 10K | W:A | 9,000 | 336 us | 356 us | 371 us | 411 us | 486 us |
| c3 cross-join 10K | W2:A | 9,000 | 335 us | 350 us | 362 us | 402 us | 481 us |
| c3 cross-join 60,480 | P:A | 2,175 | 1,618 us | 1,656 us | 1,688 us | 1,733 us | 1,845 us |
| c3 cross-join 60,480 | W:A | 2,175 | 1,592 us | 1,641 us | 1,663 us | 1,707 us | 1,859 us |
| c3 cross-join 60,480 | W2:A | 2,175 | 1,600 us | 1,642 us | 1,666 us | 1,710 us | 1,809 us |

The flip moves the whole distribution, not a tail: on c1 at 2,872 rows W:B's p0 (3,336 us) is below W:A's
p0 (3,966 us) by 16%, and on c2 at the CA size W:B's p99 (3,085 us) is below W:A's p0 (4,172 us).

## Throughput matrix

Throughput is the client's statements per second over the timed block (median of 3 rounds); server
CPU per statement is the gate metric above; the round spread is (max - min) / median of the arm's 3
per-round CPU figures.

| Cell | Rows | P:A | P:B | W:A | W:B | W2:A |
|---|---|---|---|---|---|---|
| c1 compare-all | 200 | 3,021 stmt/s | 3,011 stmt/s | 3,020 stmt/s | 3,331 stmt/s | 3,041 stmt/s |
| c1 compare-all | 1,000 | 682 stmt/s | 678 stmt/s | 689 stmt/s | 809 stmt/s | 674 stmt/s |
| c1 compare-all | 2,872 | 243 stmt/s | 243 stmt/s | 245 stmt/s | 289 stmt/s | 242 stmt/s |
| c1 compare-all | 10,000 | 71 stmt/s | 71 stmt/s | 71 stmt/s | 83 stmt/s | 71 stmt/s |
| c2 customer-statement | 200 cargos | 6,336 stmt/s | 6,232 stmt/s | 6,541 stmt/s | 10,520 stmt/s | 6,445 stmt/s |
| c2 customer-statement | 1,000 cargos | 1,936 stmt/s | 1,932 stmt/s | 1,934 stmt/s | 4,881 stmt/s | 1,937 stmt/s |
| c2 customer-statement | 10,000 cargos | 228 stmt/s | 230 stmt/s | 227 stmt/s | 422 stmt/s | 227 stmt/s |
| c2 customer-statement | 200,000 cargos | 136 stmt/s | 135 stmt/s | 135 stmt/s | 139 stmt/s | 136 stmt/s |
| c3 cross-join | 200 | 8,524 stmt/s | not run | 10,384 stmt/s | not run | 10,500 stmt/s |
| c3 cross-join | 1,000 | 7,110 stmt/s | not run | 8,458 stmt/s | not run | 8,517 stmt/s |
| c3 cross-join | 10,000 | 2,597 stmt/s | not run | 2,709 stmt/s | not run | 2,707 stmt/s |
| c3 cross-join | 60,480 | 589 stmt/s | not run | 598 stmt/s | not run | 595 stmt/s |

## Noise floor

Measured inside the run, from the W2:A control (a second server running the same statements as W:A)
and from the spread of the three per-round CPU figures.

| Quantity | Value |
|---|---|
| W2:A against W:A, median CPU, 12 cells | -1.5% to +2.5%; ten of the twelve within +/-1.1% |
| Per-arm round spread, typical cell | 0.1-4.9% |
| Per-arm round spread, outliers | c2 200: 10.8-19.6% on four arms (one slow round); c2 CA W:B 28.2% (one slow round); c3 200 W2:A 24.5% (one slow round); c2 1K W:B 7.1% |

A delta below about 2.5% (the largest control offset, on compare-all at 1,000 rows) is not a finding.
That covers planned against written A on every cell except the 3-relation cross-join at 200 / 1,000 / 10,000
rows, where the 21 us search is 5-32% and clears the floor in every pair but one (10,000 rows, pair 3: 7.1 us),
and compare-all at 200 rows and customer-statement at 200 cargos, where the 2-relation search (+2.7% and
+3.7%) is at the floor and is reported as a bound.

## Gate verdicts

| Gate (work order §9) | Result |
|---|---|
| At least one flip saves >= 15% of server CPU per statement over 3 interleaved pairs | **Met.** 7 of 8 flip cells pass on the minimum pair: compare-all 21.6 / 34.5 / 38.1 / 37.3% at 200 / 1K / 2,872 / 10K rows; customer-statement 41.7 / 60.7 / 32.2% at 200 / 1K / 10K cargos. The eighth cell (200,000 cargos, driver defaults) does not: 0.7-5.3% per pair, inside the floor. Scenario 2's CA-size cell passes on its slowest pair too |
| The prior is within 2x on every measured relation, or CC-Q6 is revisited | **Met, with the height-1 rule.** 0.75-0.98x on all 30 relations of height 2 or 3 (the height-3 ones on walked leaves, the random-path estimator not built). Not met if the leaf formula were applied to a one-leaf relation (up to 6.8x), so CC-Q6's 0.75 is not revisited and the exact root count is required |
| A seed step takes <= 200 us warm | **Not executed.** `stats::PlanStats` and its steward do not exist at `65dc45c5` (CC-S2 builds them); there is no step to time, and a stand-in (a page read through a bare client) would measure the client. The gate stays open for CC-S2 |

CC is therefore not cut to S1 plus S4 on this evidence. What the evidence does not cover is stated in
the next section.

## What this run teaches about the engine

- **The data-free model leaves 20-60% on the table exactly where one side is small or a build is
  cheap.** CB's planner is order-blind here: it keeps the written order for both spellings of both
  statements, so a user who writes the join in the cheaper order gets it by luck. `planned` is the
  written order's CPU within 0-4% on every compare-all and customer-statement cell, the flip costs it
  21.6-60.7% at the sizes up to the CA size, and nothing in the measured cells shows the swapped order
  losing.
- **The gain is a count of probes avoided, not a row count.** Both flips replace `n` pk descents (`opens` 201-10,796) with a
  statement-local build and 5-103 opens. The relation sizes the planner needs are the ones in
  Result 4: the build side must be known to fit under `join_build_max_rows`, and the probe side's
  count against the other side's rejection rate decides the winner. At 200,000 cargos the cheap
  byte-level rejection of 200k rows (BM) pays for 10.8k descents, so the 100,000-row prior would
  also have flipped that cell for nothing; with real sizes it can see there is nothing to gain.
- **The search is not free for short statements.** 21 us on a 3-relation read of 200 stats rows is
  +32%. The point of the memo (CC-S4) is concrete: it removes a cost that is a quarter to a third
  of a small statement and under 2% of one that scans 60k rows.
- **The prior's weak point is the number of leaves, not the fill.** Height 1 and 2 are exact in
  leaves, so the 0.75 fill is the entire error there (0.75-0.98x); height 3 adds the estimator's
  variance, which this run could not measure.

## What was cut and what this does not cover

- **Seed step: not executed** (no steward at this commit).
- **No cell with `join_build_max_rows`/4 exceeded.** The work order's expectation that the written order
  is kept once the built side outgrows the cap (S3's gate) was not exercised: the largest built relation is
  10,795 freights, against a cap of 65,536 (`cap/4` = 16,384). Whether the swapped order still wins as the build side nears the
  cap, and where it stops winning, is open.
- **Scenario 2 defaults** ran with `--seconds 25` and 1 booker, giving 10,795 freights (the driver's own
  default is 60 s). Fewer freights than a default-length run would leave; the cargos relation is the
  default 200,000.
- **Scenario 1 data is synthetic in the driver's schema**, not loaded by the driver: the values of
  `model_results` and the bars/stats are pseudo-random, so selectivities that depend on value
  distributions (none of the three statements filters on a value distribution except
  `session_no`, which is uniform) are the generator's.
- **Rounds: 3**, as the gate prescribes; no 4th for a tighter spread. The slow rounds noted in the noise table are
  kept in the medians; the minimum pair is what the gate reads.
- **Durability and writes are not exercised**: read statements only, `durability = group` recorded. A
  write-path or isolation effect of reordering is outside this census.
- **No CPU split**: the sampler was not run (see the Stamp).
- **PostgreSQL floor not measured** for these shapes (rule 4: stated, not apologised for).
- **Baseline.** No earlier `bench/v3.2.0/` file measures these shapes, so this run is the baseline
  that later numbers for compare-all, customer-statement and the cross-join are read against.
- **Budget.** Measured within the 20 minutes the work order allows for the census (11:52 start; the
  run itself 3 minutes 9 seconds; build 70 s). The rest of the clock went to the load (the 200,000-cargo
  load alone was 264 s) and a build attempt in the worktree lost to concurrent edits.
- **Watchdog.** A 60 s file-polled watch ran on the load and the measurement. Both raised an ORPHAN
  flag once: the measurement's at its first poll (the status file did not exist yet), the load's at 205
  s, during the 200,000-cargo load whose driver was alive (confirmed by process, then it finished). No
  run was orphaned and none was resumed.

## Reproducing

The harness and its exact invocations are in `archive/cc-s0-premise-v3.2.0-178-g65dc45c5/harness/`
(`build.sh`, `load.sh`, `cc_s0_load.py`, `cc_s0_run.py`, `run.sh`, `watch.sh`, and the analysis
scripts). The scenario drivers are the unmodified `tools/scenario2_freight.py` for scenario 2's data;
`bench/docs/` is closed (`bench/README.md`) and carries no entry for this harness. Raw per-statement
latencies are in `results-blocks-with-per-statement-latencies.json.gz`; the ANALYZE text of every
statement on every arm is in `analyze-plans-per-arm.json`.
