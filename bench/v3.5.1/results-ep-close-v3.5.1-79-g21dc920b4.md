# EP close: the undo chain refusal is gone, and the fix costs nothing a run can see

**One line.** At `21dc920b4` (`v3.5.1-79-g21dc920b4`), the change that asks
`Classify` before the cycle guard in `ResolveThroughUndo` **removes the
"undo chain revisits the record" refusal: 0 refusals in 70 runs of EO's
scenario 0 shape, against 16 refused runs of 70 (18 refusals) on the
binary one commit before it**. It **adds no cost the harness can resolve**:
in the 84 of 140 scenario 0 runs that ran at the host's fast speed, the
transaction's p0, p25, p50 and server CPU a transaction differ by -0.6 % to -2.3 % (B against A), inside a run-to-run spread of about 20 %;
on the two other shapes (scenario 0 at four traders, scenario 5 at four
terminals on `strict`) the throughput difference is -4.6 % and -4.0 %,
inside spreads of 88 % and 30 % to 36 %. The undo pages the purge reclaims
are the same: 32.8 pages recycled per 1,000 transactions on both binaries,
and 2 pages live. EP changed no purge bound, so none was expected to move.

**The host was not quiet in one way the quiet gate cannot see, and it set
the noise floor for everything below.** Scenario 0 runs in two speeds on
this host, about 6,000 transactions/s and about 2,800, in both arms
(23 of 70 base runs and 33 of 70 close runs were slow; the slow runs' log flush costs 2.3x more and their p99 is 13x). That
is a property of the host's device or memory state during this window, not
of either binary, and it means a mean throughput over all runs is not the
figure to read the A/B from. The figures that are comparable are the
per-operation ones, in the fast runs, and the paired ones.

Measured by CLA in the session that closed EP, on the worktree
`ep-undo-chain-revisit`; no `ck-tester` agent was spawned. Raw files are in
`bench/v3.5.1/archive/ep-close-v3.5.1-79-g21dc920b4/`.

## Stamp

| Field | Value |
|---|---|
| Version, commit | Close arm B: `v3.5.1-79-g21dc920b4`, branch `worktree-ep-undo-chain-revisit`. Base arm A: `v3.5.1-78-g012820e3e` (EP's opening commit). The tree was clean at the start of the run. The only edits since are documents (this file and the EP-Q3 note in EO's close). The change between the arms is `src/txn/visibility.cpp` `ResolveThroughUndo`, in `EP-S1`; the rest is comments, tests and documents (`instructions/v3.2.0/workorder-ep-undo-chain-revisit.md` §7) |
| Binaries (copies, every server started from its copy) | A `kds_server.base` sha256 `33fb581da4380d4c…` (full hash in `archive/ep-close-v3.5.1-79-g21dc920b4/binaries.sha256`), mtime 2026-11-28 08:26:41 -05:00 (13:26:41 UTC), after `012820e3e`'s commit time 13:21:45 UTC. B `kds_server.close` sha256 `dde367e0898b3174…`, mtime 08:53:48 -05:00 (13:53:48 UTC), 12 s after `21dc920b4`'s commit time (08:53:36 -05:00), and its mtime equals `build-release/kds_server`'s to within 0.1 s. Both are Release (`build-release`). Client: `tools/eqbench/target/release/eqbench` built in this worktree, copy sha256 `3f04e45c940dbb55…`, the same for both arms |
| Date and time | 2026-11-28, 13:55:33 to 14:36:57 UTC, 180 runs. Timed by a driver written for this run (`archive/…/ep_ab.py`), with a 60 s watchdog on its heartbeat file (`wd.sh`); the watchdog never found it orphaned: its four launches were the plan's extensions (cell 1 from 12 to 50 to 70 pairs, cells 2 and 3 from 4 to 10), and one watchdog I killed by mistake was relaunched by hand with no run lost |
| Device | Data file and WAL under `/home/ukjin/workspaces/ckdbs/.claude/worktrees/ep-bench/work/<run>/data/`, on `/dev/mapper/fedora-root`, xfs (`df -T` at the start), never tmpfs. Each run's data file and WAL were deleted after the run. Only the binary copies are on disk beside it |
| Server | `tools/eqbench/servers.sh kds-up`: `cores = 1`, `buffer_pool_frames = 65536`, `durability = group` (scenario 5: `strict`), `log_level = warn`, port 17611, taskset CPUs 0 to 7. A fresh server and data file a run. The harness (`eqbench`) on CPUs 8 to 23, seed 1 |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0 to 7), governor `powersave`. **Memory pressure: swap 8,026 to 8,152 MB of 8,191 MB used** and 9.8 GB free at the checks made during and after the run (other sessions' memory). Another session's processes ran on the host throughout |
| Quiet gate | Before every run: no build or test process (`pgrep -f "cc1plus\|cmake --build\|ctest\|kds_tests\|cargo build\|rustc"`) and at most 8 % of all CPUs busy over 3 s. All 180 runs passed: the busy percentage before a run was 0.5 % to 7.7 % (median 1.0 %), the 1-minute load average at most 3.94 (each in its `.host.txt`) |
| Procedure | Interleaved A then B per pair, a fresh server each. Cell 1: 50 ABAB pairs, then 20 BAAB pairs (B first), because the first 50 showed B in the slow speed more often than A (26 against 15 of 50), and the reversed order did not (7 against 8 of 20). Cells 2 and 3: 10 ABAB pairs each |
| Rule | Throughput = the driver's `tps` (scenario 0) or `new_orders_per_minute` (scenario 5, not tpmC). Latencies are the median over runs of each run's own percentile. Noise floor = (max - min) / median over runs of the same arm and speed. A difference under the floor is not a finding. Rate columns are measured, not derived |
| Verification | Every return code is 0. Every check of every run passed on both arms (scenario 0's balance conservation, trades and profit rows; scenario 5's TPC-C conditions 1 to 4). A refused read is not counted as an operation and no check was taken from it |
| Version-series | The comparison base is this engine's own last run for each shape, EO's close `bench/v3.5.1/results-eo-close-v3.5.1-63-g9543cf720.md`. EO ran other sizes of the same scenarios (the delta is stated per cell and says which differ). PostgreSQL is the floor and an A/B between two KDS binaries has no PostgreSQL twin; the floor was not measured for this change. No Oracle control was run |

## 1. The refusal is gone

On scenario 0 at `txn=1 fk=1` (EO's reproduction: `--scenario s0 --set
users=300 --set assets=100 --set seconds=6 --set traders=3 --set
profit_users=20 --set txn=1 --set fk=1`), the base binary refused the
reporter's read, `SELECT id, user_id, balance, asset_qty, trade_count,
opened_day FROM s0_accounts WHERE user_id = N`, in 16 of 70 runs, 18 times
in all. The close binary refused none in 70. Every refusal read "undo chain
revisits the record at <address>; the links are cyclic" and arrived as the
driver's `profit-scan:other` error; the server's `kds.log` carries the same
18 lines in the same 16 runs. No run failed a check, because the harness
takes no figure from a refused operation and the verification reads after.

| source | binary | runs | runs refused | refusals |
|---|---|---|---|---|
| this run, cell 1 (50 ABAB + 20 BAAB pairs) | A, `012820e3e` | 70 | **16** | 18 |
| this run, cell 1 | B, `21dc920b4` | 70 | **0** | 0 |
| this run, cell 2 (`txn=0`, four traders, 1,000 users) | A / B | 10 / 10 | 0 / 0 | 0 / 0 |
| this run, cell 3 (scenario 5, four terminals, `strict`) | A / B | 10 / 10 | 0 / 0 | 0 / 0 |
| EP-S0 (`workorder-ep-undo-chain-revisit.md` §7), the same reproduction, a base plus a diagnostic dump | `c162c02932825f3d` | 30 | 13 | 22 |
| EP-S3 pre-close (CLA's run before this one, a binary of the same engine source as B, before a comment-only edit) | same engine source as B | 60 | **0** | 0 |
| EO's close, scenario 0 and scenario 5 at the engine before EP | `e961b65c1852fe50…` | 69 KDS runs | 5 | 10 |

The base's rate in this run, 16 of 70 (23 %), is lower than the 13 of 30
of the diagnostic binary and 5 of 69 across EO's wider matrix. The refusal
needs an undo page recycled before a growth lands on a version whose record
was the page's first, so its rate depends on how many transactions a run
completes, and slow-speed runs complete fewer. Zero refusals on the close
binary across 130 runs of this shape (70 here, 60 in the pre-close run) put
its rate under 2.3 % at 95 % confidence (rule of three: 3 / 130), and 0 of
70 against 16 of 70 is a Fisher exact one-sided p of 6e-6.

Scenario 5 had one refusal in EO's close (four terminals, `strict`). In
this run's 10 base runs of the same shape there were none, so the scenario 5
refusal EP-S0 left unverified (`workorder-ep-undo-chain-revisit.md` §7,
EP-S0) is **still unverified as this cause**: the shape did not reproduce
it on the base binary here either (0 of 10), and 0 of 10 on the close binary
carries no weight.

## 2. The overhead: nothing a run can resolve

### 2.1 Scenario 0, `txn=1 fk=1`, three traders (the `txn` phase, per transaction)

A transaction is a `BEGIN`, an insert into `s0_trades`, an update of
`s0_accounts` and a `COMMIT`. The reporter's profit scan (20 users) runs
once a second beside the traders. Figures are medians over runs; the
fast-run rows are the comparison (84 runs, 47 base and 37 close, those at or
above 4,500 transactions/s), and the all-run rows are printed beside them
because a mean over a bimodal population is a mixture and not an engine
property.

| rows | binary | runs | rate | p0 | p25 | p50 | p95 | p99 | mean | server CPU a txn |
|---|---|---|---|---|---|---|---|---|---|---|
| fast runs | A base | 47 | 6,034 txn/s | 201.3 µs | 451.2 µs | 471.1 µs | 613.6 µs | 911.6 µs | 496.8 µs | 35.9 µs |
| fast runs | B close | 37 | 6,100 txn/s | 199.5 µs | 448.1 µs | 468.4 µs | 614.3 µs | 902.6 µs | 491.4 µs | 35.1 µs |
| fast runs, B against A | | | +1.1 % | -0.9 % | -0.7 % | -0.6 % | +0.1 % | -1.0 % | -1.1 % | -2.3 % |
| all runs | A base | 70 | 5,956 txn/s | 202.0 µs | 451.3 µs | 471.6 µs | 615.4 µs | 919.4 µs | 503.3 µs | 36.1 µs |
| all runs | B close | 70 | 4,922 txn/s | 201.5 µs | 448.9 µs | 469.2 µs | 617.9 µs | 952.5 µs | 609.1 µs | 35.6 µs |
| slow runs | A / B | 23 / 33 | 2,812 / 2,840 txn/s | | | 514.3 µs (both, pooled) | | 12,203.8 µs (both, pooled) | | 43.6 µs (both, pooled) |

Operations a run (median): 36,239 transactions on A and 36,637 on B in the
fast runs. The median of a transaction is 469 to 471 µs on both binaries in
the fast runs, the median of each of its statements is the same to the
tenth of a microsecond (below), and the server CPU is 35.1 to 35.9 µs.

**The noise floor.** Fast-run throughput spreads 20.4 % (A) and 22.8 % (B),
(max - min) / median. A and B differ by +1.1 % in that median, a
Mann-Whitney p of 0.008 that does not survive the selection: the 4,500
cut-off puts 47 base runs and 37 close runs into the comparison, and the
two arms' fast-run ranges, 4,904 to 6,135 and 4,775 to 6,166, overlap
completely. The control is the arm against itself: the first 40 pairs'
odd-numbered base runs against its even-numbered ones differ by 0.26 %
(`agg.py`, "control A-odd vs A-even"). **No delta under about 20 % in
throughput is a finding here, and none was found.** The latency rows move
by 0.1 % to 2.3 %, on per-run figures whose own spread across runs is 11 % to 25 % (p0 to p25).

| statement (fast runs) | binary | ops a run | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `trade-insert` | A | 72,430 | 12.9 µs | 21.6 µs | 26.1 µs | 160.8 µs | 189.4 µs |
| `trade-insert` | B | 73,212 | 12.7 µs | 21.4 µs | 25.6 µs | 160.7 µs | 191.5 µs |
| `account-update` | A | 72,430 | 10.5 µs | 16.9 µs | 22.1 µs | 156.1 µs | 185.2 µs |
| `account-update` | B | 73,212 | 10.4 µs | 16.7 µs | 21.6 µs | 155.8 µs | 186.2 µs |
| `profit-scan` (the reader that walks undo) | A | 100 | 28.0 µs | 35.4 µs | 46.0 µs | 265.7 µs | 322.5 µs |
| `profit-scan` | B | 100 | 27.6 µs | 34.3 µs | 43.4 µs | 256.5 µs | 316.7 µs |
| `profit-insert` | A | 100 | 145.7 µs | 233.6 µs | 269.2 µs | 373.5 µs | 439.4 µs |
| `profit-insert` | B | 100 | 145.7 µs | 230.3 µs | 268.9 µs | 362.6 µs | 428.8 µs |

`profit-scan` is the one statement of the shape that walks into undo (the
reporter reads accounts the traders are updating), so it is where the
changed code runs, and its p50 is 46.0 µs on A and 43.4 µs on B, with 100
operations a run on both. A statement with 100 samples a run has a p50 whose run-to-run spread is wide, so its 2.6 µs lower p50 on B is no gain either; it shows no cost.

**Where a transaction's time goes** (the harness's breakdown, a mean per
transaction, fast runs). The harness measures four waits and the rest is a
residual it cannot attribute.

| wait | A base | B close | note |
|---|---|---|---|
| durability / commit (`commit.wal_flush`, 1.00 flush a transaction) | 137.0 µs (28 %) | 136.2 µs (28 %) | `group` rung; fsync |
| write and read statements (execution) | 34.4 µs (7 %) | 33.4 µs (7 %) | server execution |
| parse and plan | 1.8 µs (0.4 %) | 1.7 µs (0.3 %) | `parse_optimise` |
| lock or conflict wait | 0 µs | 0 µs | none recorded; `conflict_retries` 0 |
| disk read | 0 µs | 0 µs | the pool holds the data |
| client and socket round trips | not measured by the harness | not measured by the harness | the residual: a transaction's mean 496.8 µs less the three above 173.2 µs is 323.6 µs on A (and 320.1 µs on B: 491.4 less 171.3), the four round trips of `BEGIN`, two statements and `COMMIT` |

The commit wait is the largest measured wait, as it is for a writing transaction in EO's close.
The change touches none of the four measured waits: 137.0 against 136.2 µs
for the commit and 34.4 against 33.4 µs for execution are inside the
fast-run floor.

### 2.2 Scenario 0, `txn=0`, four traders, `group` (cell 2; 10 ABAB pairs)

EO's four-traders cell ran 10,000 users and 10,000 assets for 60 s and
three rounds. **This cell was cut to 1,000 users, 1,000 assets and 15 s**
(`--set users=1000 --set assets=1000 --set seconds=15 --set profit_users=20
--set traders=4`) to fit the budget, so no delta against EO's number is
taken, only the two arms against each other. Both arms read a wide spread
(88 % to 89 %), the same two host speeds.

| binary | runs | rate | p0 | p25 | p50 | p95 | p99 | mean | server CPU a txn | spread |
|---|---|---|---|---|---|---|---|---|---|---|
| A base | 10 | 2,388 txn/s | 828.1 µs | 1,074.1 µs | 1,171.2 µs | 2,028.7 µs | 5,853.2 µs | 1,674.1 µs | 43.5 µs | 89.0 % |
| B close | 10 | 2,277 txn/s | 820.3 µs | 1,074.5 µs | 1,167.6 µs | 1,932.9 µs | 3,549.4 µs | 1,756.4 µs | 41.0 µs | 88.4 % |
| B against A | | -4.6 % | -0.9 % | 0.0 % | -0.3 % | -4.7 % | -39.4 % | +4.9 % | -5.8 % | |

Per transaction the harness's breakdown reads commit 800.8 µs (A) and 821.8
µs (B), execution 62.6 and 60.0 µs, parse and plan 2.6 and 2.3 µs. A
four-trader transaction waits 800 µs for a commit because four writers share
one core's group flush. **The delta in rate, -4.6 %, is inside an 88 %
spread, and the p99's -39 % is a tail percentile of 10 runs whose own
spread is 265 % and 442 %: neither is a finding.** The body of the
distribution (p0 to p50) is the same to 0.9 % or less. For orientation, EO's
larger cell read 2,364 txn/s with p0 795.7 µs, p25 1,106.8 µs and p50
1,475.5 µs (`results-eo-close-v3.5.1-63-g9543cf720.md`, s0 `four-traders`);
this cell's smaller population reads 2,277 to 2,388 txn/s and a p25 within 3
% of EO's, and no claim is made beyond that.

### 2.3 Scenario 5, four terminals, `strict` (cell 3; 10 ABAB pairs)

Cut from EO's 60 s run, 10 warm-up transactions of each kind and 300
single-terminal samples to **20 s, 5 warm-up and 60 samples a kind** (`--set
terminals=4 --set seconds=20 --set alone=60 --set warmup=5`); the shape, the
four terminals, the scale (2 warehouses, 10,000 items) and the `strict`
rung are EO's.

| binary | runs | new orders/min (not tpmC) | p0 | p25 | p50 | p95 | p99 | mean | server CPU an op | spread |
|---|---|---|---|---|---|---|---|---|---|---|
| A base | 10 | 44,097 | 259.2 µs | 719.9 µs | 1,539.6 µs | 5,649.9 µs | 8,483.1 µs | 2,414.2 µs | 218.1 µs | 30.4 % |
| B close | 10 | 42,328 | 253.3 µs | 724.0 µs | 1,561.2 µs | 6,166.0 µs | 15,237.8 µs | 2,510.1 µs | 217.1 µs | 35.5 % |
| B against A | | -4.0 % | -2.3 % | +0.6 % | +1.4 % | +9.1 % | +79.6 % | +4.0 % | -0.5 % | |
| EO's close, `strict` (3 rounds, 60 s) | | 43,439 | 251.3 µs | 712.6 µs | 1,539.9 µs | 5,904.9 µs | 14,894.2 µs | 2,464.3 µs | 228.5 µs | |

The mix's operations a run: 33,144 (A) and 31,882 (B), median. **The
-4.0 % in new orders a minute is inside the 30 % to 36 % spread (Mann-Whitney
p = 0.45; the paired B/A median is 0.99, range 0.71 to 1.16), and the p95 and
p99 differences are in the tail of a mix whose own tail spreads 69 % to 143
%.** Server CPU a transaction, which does not move with the client or the
host's speed, is 218.1 against 217.1 µs, a difference of 0.5 %. The harness's
breakdown for the mix: commit 355.0 and 378.9 µs, execution 509.9 and 518.5
µs, lock 143.9 and 156.7 µs, parse and plan 33.7 and 32.5 µs (A and B, a mean
over the mix).

| kind (60 samples a run, one terminal alone) | binary | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| new-order | A | 733.7 µs | 960.9 µs | 1,180.5 µs | 1,585.7 µs | 1,929.4 µs |
| new-order | B | 729.7 µs | 946.9 µs | 1,175.6 µs | 1,447.8 µs | 1,727.8 µs |
| payment | A | 437.8 µs | 445.9 µs | 458.7 µs | 552.4 µs | 747.0 µs |
| payment | B | 439.4 µs | 448.6 µs | 463.4 µs | 561.9 µs | 862.9 µs |
| order-status | A | 395.9 µs | 412.3 µs | 421.0 µs | 559.6 µs | 868.3 µs |
| order-status | B | 396.5 µs | 415.0 µs | 423.5 µs | 520.6 µs | 572.3 µs |
| delivery | A | 1,371.1 µs | 1,392.5 µs | 1,411.6 µs | 1,915.6 µs | 2,479.8 µs |
| delivery | B | 1,360.7 µs | 1,384.1 µs | 1,402.5 µs | 2,039.9 µs | 2,734.2 µs |
| stock-level | A | 431.4 µs | 446.7 µs | 456.4 µs | 572.3 µs | 806.5 µs |
| stock-level | B | 433.7 µs | 450.9 µs | 463.6 µs | 595.2 µs | 860.5 µs |

These are 60 samples a kind a run; the p0 to p50 columns move by -0.8 % to
+1.6 % between the arms, and the p99 columns, from 60 samples, move both
ways (-10 % to +13 %, and -34 % for order-status).

## 3. Undo growth (EP-R3)

EP changed no bound of the purge, so this is a statement of growth and
not a price. The undo log's two counters, read from `sys.meta`'s `undo`
section after each scenario and before the server stops (`SELECT * FROM
sys.meta`: `undo_pages_live`, `undo_pages_recycled`). **EO's close recorded
neither counter, so there is no EO figure to set beside these.** They are
the baseline the next run of the same shapes is read against.

| cell | binary | runs | undo pages live (min / median / max) | undo pages recycled (min / median / max) | recycled a 1,000 operations, fast runs for cell 1 |
|---|---|---|---|---|---|
| 1: s0 `txn=1 fk=1`, 3 traders | A base | 70 | 2 / 2 / 2 | 475 / 1,172 / 1,207 | 32.8 |
| 1 | B close | 70 | 2 / 2 / 2 | 494 / 976.5 / 1,213 | 32.8 |
| 2: s0 `txn=0`, 4 traders | A base | 10 | 2 / 2 / 2 | 396 / 1,192 / 1,437 | 33.3 |
| 2 | B close | 10 | 2 / 2 / 2 | 488 / 1,138 / 1,463 | 33.3 |
| 3: s5 four terminals, `strict` | A base | 10 | 2 / 4 / 6 | 9,764 / 10,916.5 / 12,174 | 329.4 |
| 3 | B close | 10 | 2 / 3 / 7 | 9,173 / 10,604 / 11,885 | 332.6 |

Live pages plateau at 2 pages (3 to 7 at scenario 5's end) while 1,000 to
12,000 pages are recycled: the purge reclaims as it did, and the fix
changes none of it. The recycled count tracks the number of transactions a
run completed (a slow run completes fewer), which is why the per-1,000
column is the one to compare, and it is the same on both binaries (32.8
against 32.8; 33.3 against 33.3; 329.4 against 332.6).

## 4. What the run teaches

**The refusal was a false refusal, and the measurement agrees with the
proof.** `workorder-ep-undo-chain-revisit.md` §7 argued from the source that
a link into a recycled page exists and is never followed, and that the guard
had been asked about it before the walk decided to follow it. The data here
is the third independent line: 0 refusals of 130 runs on the fixed engine
against 16 of 70 on the unfixed one, on the same shape, with every check
of every run passing on both. No wrong version appears anywhere: no check
failed on either binary in 180 runs.

**The change is a reordering of two calls and costs a reordering's price,
which is nothing this harness resolves.** The per-statement body of the
fast runs agrees to the tenth of a microsecond (26.1 against 25.6 µs on the
insert, 22.1 against 21.6 µs on the update), the reader that walks undo
shows 46.0 against 43.4 µs on 100 samples a run, and server CPU a
transaction is 35.9 against 35.1 µs. The hot path adds no work: a reader
that never walks into undo reaches neither call, which is what EP-R4 said.
The lower bound on what this run could have seen is the floor, about 20 %
in throughput and about 1 % to 2 % in the median per-operation figures of
the fast runs: a regression under those would not show. None of the figures
suggests one.

**The host has a slow mode that the quiet gate does not detect, and an A/B
must be built to survive it.** The same binary read about 6,000 or about
2,800 transactions/s depending on the run. In the slow runs the
log flush (`commit.wal_flush` plus the commit breakdown) costs 314.5 µs
a transaction against 136.7 µs, the p99 is 12.2 ms against 0.90 ms, and
server CPU a transaction rises from 35.7 to 43.6 µs. That
is the signature of the log sync's device or of memory pressure on the
box (swap was 98 % full), not of a code path. The first 50 ABAB pairs read
B slow in 26 runs and A slow in 15, which would have looked like a 40 %
regression in a mean. The reversed order (B first) read 7 and 8. **A
mean throughput over all runs is not a measurement here; the per-operation
figures in the fast runs, the paired ratio and a swapped order are.** A
later run on this box should record the device's flush latency per run
beside the throughput (the harness's own `commit.wal_flush` already does,
per transaction) and gate on it.

**EO's wider matrix needs no re-run for this defect.** The refusals EO
counted were 10 false refusals in 5 of 69 KDS runs; none of its figures is
taken from a refused operation, and EO's file carries the EP-Q3 note. This
run's cell 3 does not show scenario 5's single refusal on the base
binary, so scenario 5's case stays unverified as this cause (section 1).

## What was cut, and what is not measured

- **No row-count sweep (rule 9).** The change is the order of two checks
  inside a version walk, which scales with the chain length of a row and
  not with the row count, and the three cells run one cardinality each:
  scenario 0 at 586 accounts (300 users, two to a user) and 1,992 accounts
  (1,000 users), scenario 5 at EO's scale. A sweep over 200, 1,000 and
  10,000 would have measured the host's two speeds again at three sizes.
- **Cells 2 and 3 are cut to 10 pairs, a shorter scenario each** (section
  2.2 and 2.3 say how), so neither carries a delta against EO. Cell 1 is
  EO's reproduction as given (`seconds=6`), and has no EO figure of the
  same shape: EO's s0 ran `txn=0` at 10,000 users.
- **Not measured: PostgreSQL and Oracle.** An A/B between two KDS builds
  has no twin; the floor question does not arise for a reordering.
- **Not measured: two cores.** EP does not touch the two-core wake.
- **Not decomposed: the residual client and socket round trips** (323.6 µs of a transaction's 496.8 µs on A and 320.1 µs of 491.4 µs on B) are the difference
  between the transaction's mean and the three measured waits; the harness
  does not time a round trip apart from the wait.
- **Not resolved: what put the host into its slow speed.** The candidates
  are the xfs log device, the full swap, and another session's I/O.
  A per-run device flush latency and `iostat` are what would tell; no run
  here recorded them.
- **A debug-build, sim and test suite run were not part of this
  measurement.** The suite at S0 to S2 (5,442 of 5,446, four flakes passing
  alone) is in the work order's §7.

## Reproducing

The driver is `archive/ep-close-v3.5.1-79-g21dc920b4/ep_ab.py` (an A/B
orchestrator over `tools/eqbench/servers.sh` and an unmodified `eqbench`; the
harness is documented in `tools/eqbench/README.md`), run under `wd.sh`
with a plan such as `'{"s0txn":50,"s0txnR":20,"s0g4":10,"s5s4":10}'` and
the two binaries named in its header. `agg.py`, `agg2.py` and `agg3.py` read
the archived JSON and print every figure in this file. The raw files per run
are `<cell>-<arm>-<n>.json` (the harness's summary), `.stderr`, `.serverlog`
(the server's `kds.log`), `.meta.txt` (`sys.meta`'s undo section), `.host.txt`
(load and busy percentage before the run) and `.rc`.
