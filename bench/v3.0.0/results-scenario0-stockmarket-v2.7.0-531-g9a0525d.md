# Scenario 0 (stock market) at `v2.7.0-531-g9a0525d`

**One line.** Autocommit trades are still bound by the durability wait, but
the wait no longer costs the same at every core count: at `cores = 8`
`strict` now runs at 331.7 tps (median of three) against 169.4 tps at
`cores = 1`, where the f6ed10c run had them equal (213.0 and 212.1). The
one-core cells are 17-20 % below f6ed10c. And at `cores = 8` under `strict`
one trade insert in every run of three is refused `TXN_CONFLICT` by the
btree's bounded re-descent, which the driver does not retry, so that cell
exits 1 with one torn trade (section 6).

Driver: `tools/scenario0_stockmarket.py`, unmodified, `--txn` off (four
autocommit statements per business transaction, four durability points).
Arguments and cell order are those of the f6ed10c BTREE run
(`bench/v3.0.0/archive/scenario0-btree-v2.7.0-157-gf6ed10c/run_all.sh` at
`9a0525d`), so the delta in section 7 is like-for-like in shape, not in
server configuration (section 7 says where they differ).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-09-30, 06:41:18 to 06:43:56 UTC (run 1), 06:44:17 to 06:46:50 (run 2), 06:46:50 to 06:49:25 (run 3); per-cell times in section 8 |
| Worktree / branch | `bench-rerun-scenarios` / `worktree-bench-rerun-scenarios` |
| HEAD | `fc2d343`, which over `9a0525d` only deletes `bench/v3.0.0/` and edits `bench/README.md` (`git diff --stat 9a0525d HEAD -- src include tools CMakeLists.txt` is empty) |
| Engine commit measured | `9a0525d`, `git describe --tags` = `v2.7.0-531-g9a0525d` ("AY-S11: AY closed") |
| Tree cleanliness | Clean at the start (`git status` empty); nothing was edited or built during the cells |
| Binary provenance | `/home/cdkbs/bench-runs/rebaseline-9a0525d/kds_server`, a copy of `build-release/kds_server`; **sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`**; the source binary's mtime is 2026-09-30 06:30:43 UTC, after both `9a0525d` (06:19:05) and `fc2d343` (06:29:21), so it is not older than HEAD. Every server in every cell was started from the copy. The hash is the one AY-S11's arm B recorded for `0552d55`, which is expected: `src/`, `include/` and `CMakeLists.txt` have not changed since |
| Device | data files under `/home/cdkbs/bench-runs/rebaseline-9a0525d/data/`, `/dev/root`, `ext4` (`df -T` at run time, also for `/tmp`). Not tmpfs. Each cell had a fresh data file, deleted after it |
| Build type | Release (`CMAKE_BUILD_TYPE:STRING=Release` in `build-release/CMakeCache.txt`); `cmake --build build-release --target kds_server -j8` finished before the first cell |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket x 4 cores x 2 threads, Linux 7.0.0-1014-azure |
| Server config | `log_level = warn`, `cores` and `durability` per cell, everything else default (no `placement` and no `peer_listeners`: both are retired and refused by name). Auth and TLS off |
| Ports | 15600 to 15603; never 15432 |
| PostgreSQL | not installed on this host; the floor was not measured for this shape |

## 2. What was run

Twelve cells: the four of the f6ed10c matrix (`cores` 1 and 8 by `group`
and `strict`) run **three times each**, so the noise floor is measured from
inside the run (three complete passes of the eight-cell scenario0 and
scenario2 interleave, scenario2 in its own file). Order inside a pass is the
predecessor's: `s0-c1-g`, [scenario2 cells], `s0-c8-g`, `s0-c1-s`, [scenario2],
`s0-c8-s`. Runs 2 and 3 carry the prefix `r2-` and `r3-`.

Arguments, identical to the f6ed10c run: `--users 100 --accounts-per-user 3
--assets 30 --traders 8 --txn-per-user 50 --verify 200 --seed 1 --sync`. The
work target is 5,000 committed business transactions (100 users x 50), 625
per trader, each 2 `INSERT trades` + 2 `UPDATE accounts`, on 100 users, 287
accounts and 30 assets; the run ends with `trades` holding 10,000 rows.
`--profit` is on (its default), a reporting process alongside the traders.
Rule 9's 200 / 1K / 10K sweep is **not executed** for this driver: its size
is fixed by the arguments above, which mirror the predecessor, and the
measured unit (a four-statement transaction on a 287-row `accounts`
relation) does not scale with `trades`' row count. The table sizes are the
ones stated.

Host load and build check before every cell (`/proc/loadavg` and
`pgrep -a -f "cc1plus|cmake --build|ctest"`): section 8. **No build or
test process was live at any cell.** The one-minute load reads 0.25 at the
first cell and 1.1 to 2.6 afterwards, which is the previous cell's own
eight traders and reporter still in the average, not a competitor.

## 3. Throughput

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | f6ed10c |
|---|---|---|---|---|---|---|---|---|
| `s0-c1-g` | 1 | group | 613.8 tps | 626.0 tps | 637.4 tps | **626.0 tps** | 3.8 % | 750.6 tps |
| `s0-c8-g` | 8 | group | 689.9 tps | 661.0 tps | 645.8 tps | **661.0 tps** | 6.8 % | 794.3 tps |
| `s0-c1-s` | 1 | strict | 169.4 tps | 172.5 tps | 162.6 tps | **169.4 tps** | 6.1 % | 212.1 tps |
| `s0-c8-s` | 8 | strict | 281.3 tps | 331.7 tps | 337.6 tps | **331.7 tps** | 20.0 % | 213.0 tps |

Every cell reached its 5,000-transaction target. The median is the
headline; the spread column is the noise floor for that cell.

**Durability is still the axis, and core count now moves it.** At
`cores = 1`, `group` is 3.70x `strict` (626.0 / 169.4). At `cores = 8` it
is 1.99x (661.0 / 331.7): `strict` gains +95.8 % from the extra cores
(169.4 to 331.7 tps) while `group` gains +5.6 % (626.0 to 661.0 tps), which
is inside the 6.8 % spread of the `c8-g` cell. The f6ed10c run saw +0.4 %
under `strict` and +5.8 % under `group`.

## 4. Percentiles

The measured unit, one business transaction over its four round trips,
and the four statements it is made of. All values in microseconds; `ops`
is the phase's count in that cell. Run 1 in full:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s0-c1-g` | txn | 5,000 ops | 9,791.1 µs | 11,243.1 µs | 11,769.4 µs | 20,667.2 µs | 28,226.6 µs | 36,900.0 µs | 0 errors |
| `s0-c1-g` | trade-insert | 10,000 ops | 1,742.2 µs | 2,746.9 µs | 2,884.6 µs | 5,400.6 µs | 11,321.2 µs | 21,429.4 µs | 0 errors |
| `s0-c1-g` | account-update | 10,000 ops | 1,461.5 µs | 2,752.2 µs | 2,890.6 µs | 5,471.3 µs | 10,456.2 µs | 21,426.5 µs | 0 errors |
| `s0-c1-g` | profit-scan | 300 ops | 387.3 µs | 1,488.2 µs | 1,573.2 µs | 2,538.6 µs | 5,310.1 µs | 11,603.1 µs | 0 errors |
| `s0-c1-g` | profit-insert | 300 ops | 2,441.7 µs | 2,689.2 µs | 2,837.2 µs | 4,567.1 µs | 5,998.8 µs | 16,263.4 µs | 0 errors |
| `s0-c8-g` | txn | 5,000 ops | 5,005.7 µs | 9,732.7 µs | 10,338.3 µs | 18,829.3 µs | 27,599.2 µs | 49,323.8 µs | 0 errors |
| `s0-c8-g` | trade-insert | 10,000 ops | 1,186.6 µs | 2,351.5 µs | 2,505.8 µs | 5,114.6 µs | 9,242.7 µs | 41,696.1 µs | 0 errors |
| `s0-c8-g` | account-update | 10,000 ops | 1,245.9 µs | 2,355.0 µs | 2,511.2 µs | 5,448.2 µs | 10,246.0 µs | 25,453.6 µs | 0 errors |
| `s0-c8-g` | profit-scan | 300 ops | 153.5 µs | 197.8 µs | 219.4 µs | 362.7 µs | 512.2 µs | 730.3 µs | 0 errors |
| `s0-c8-g` | profit-insert | 300 ops | 1,782.0 µs | 2,125.2 µs | 2,255.8 µs | 3,826.9 µs | 7,063.6 µs | 9,731.8 µs | 0 errors |
| `s0-c1-s` | txn | 5,000 ops | 4,604.6 µs | 37,576.2 µs | 41,228.3 µs | 80,927.9 µs | 95,307.9 µs | 134,410.8 µs | 0 errors |
| `s0-c1-s` | trade-insert | 10,000 ops | 1,125.1 µs | 8,918.1 µs | 9,907.6 µs | 20,795.0 µs | 28,946.0 µs | 56,294.2 µs | 0 errors |
| `s0-c1-s` | account-update | 10,000 ops | 1,156.3 µs | 8,946.8 µs | 9,945.6 µs | 20,682.3 µs | 28,915.2 µs | 49,347.9 µs | 0 errors |
| `s0-c1-s` | profit-scan | 400 ops | 10,294.0 µs | 18,889.9 µs | 19,918.7 µs | 31,519.6 µs | 39,063.2 µs | 40,380.8 µs | 0 errors |
| `s0-c1-s` | profit-insert | 400 ops | 17,655.5 µs | 19,993.4 µs | 21,199.8 µs | 33,642.1 µs | 46,982.8 µs | 54,559.5 µs | 0 errors |
| `s0-c8-s` | txn | 5,001 ops | 8,425.2 µs | 16,952.5 µs | 20,250.3 µs | 45,447.2 µs | 56,883.8 µs | 81,571.0 µs | 1 error |
| `s0-c8-s` | trade-insert | 10,002 ops | 1,831.4 µs | 3,869.4 µs | 4,938.5 µs | 11,600.2 µs | 18,126.6 µs | 42,392.6 µs | 1 error |
| `s0-c8-s` | account-update | 10,002 ops | 1,343.3 µs | 3,849.9 µs | 4,937.0 µs | 11,538.1 µs | 18,294.3 µs | 41,832.5 µs | 0 errors |
| `s0-c8-s` | profit-scan | 100 ops | 134.2 µs | 147.9 µs | 158.5 µs | 190.9 µs | 237.7 µs | 387.1 µs | 0 errors |
| `s0-c8-s` | profit-insert | 100 ops | 1,850.5 µs | 2,211.0 µs | 2,323.4 µs | 4,409.0 µs | 6,079.8 µs | 25,000.1 µs | 0 errors |

Runs 2 and 3, the measured unit and the trade insert only (everything is
in the archive). Run 2:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s0-c1-g` | txn | 5,000 ops | 9,428.1 µs | 10,950.5 µs | 11,584.3 µs | 19,921.0 µs | 26,740.1 µs | 33,351.4 µs | 0 errors |
| `s0-c1-g` | trade-insert | 10,000 ops | 1,605.7 µs | 2,674.9 µs | 2,833.2 µs | 5,388.4 µs | 10,135.9 µs | 23,081.4 µs | 0 errors |
| `s0-c8-g` | txn | 5,000 ops | 5,716.1 µs | 9,779.0 µs | 10,706.8 µs | 19,599.4 µs | 26,259.4 µs | 58,317.1 µs | 0 errors |
| `s0-c8-g` | trade-insert | 10,000 ops | 1,186.7 µs | 2,319.8 µs | 2,506.4 µs | 5,504.8 µs | 9,568.8 µs | 48,541.2 µs | 0 errors |
| `s0-c1-s` | txn | 5,000 ops | 5,106.0 µs | 37,026.9 µs | 40,595.4 µs | 80,020.6 µs | 95,088.1 µs | 116,505.9 µs | 0 errors |
| `s0-c1-s` | trade-insert | 10,000 ops | 1,235.8 µs | 8,786.5 µs | 9,704.3 µs | 20,580.3 µs | 28,330.7 µs | 50,627.6 µs | 0 errors |
| `s0-c8-s` | txn | 5,001 ops | 5,730.9 µs | 10,254.1 µs | 11,701.7 µs | 41,219.0 µs | 52,858.8 µs | 90,694.5 µs | 1 error |
| `s0-c8-s` | trade-insert | 10,002 ops | 282.7 µs | 2,469.6 µs | 2,711.3 µs | 10,332.4 µs | 15,251.3 µs | 56,169.6 µs | 1 error |

Run 3:

| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|---|
| `s0-c1-g` | txn | 5,000 ops | 9,925.1 µs | 10,861.1 µs | 11,502.9 µs | 19,479.3 µs | 24,106.9 µs | 29,576.4 µs | 0 errors |
| `s0-c1-g` | trade-insert | 10,000 ops | 1,637.7 µs | 2,655.0 µs | 2,803.2 µs | 5,202.6 µs | 9,750.0 µs | 16,552.6 µs | 0 errors |
| `s0-c8-g` | txn | 5,000 ops | 6,648.5 µs | 10,155.7 µs | 11,081.2 µs | 19,997.6 µs | 26,517.8 µs | 65,809.0 µs | 0 errors |
| `s0-c8-g` | trade-insert | 10,000 ops | 1,199.8 µs | 2,417.0 µs | 2,604.9 µs | 5,421.9 µs | 10,461.1 µs | 25,451.3 µs | 0 errors |
| `s0-c1-s` | txn | 5,000 ops | 4,740.9 µs | 38,519.5 µs | 42,845.6 µs | 85,545.3 µs | 103,398.7 µs | 159,419.3 µs | 0 errors |
| `s0-c1-s` | trade-insert | 10,000 ops | 1,167.9 µs | 9,111.8 µs | 10,196.9 µs | 21,974.0 µs | 30,047.0 µs | 73,381.0 µs | 0 errors |
| `s0-c8-s` | txn | 5,001 ops | 5,464.6 µs | 11,244.1 µs | 17,997.4 µs | 31,052.2 µs | 39,089.7 µs | 58,584.7 µs | 1 error |
| `s0-c8-s` | trade-insert | 10,002 ops | 345.3 µs | 2,672.4 µs | 4,139.3 µs | 8,530.5 µs | 14,272.1 µs | 50,027.7 µs | 1 error |

`trade-insert` (a clustered-btree append) and `account-update` (a btree
overwrite by pk) are the same statement to within 0.4 % at p50 in every
cell of run 1 (2,884.6 vs 2,890.6 us at `s0-c1-g`, 9,907.6 vs 9,945.6 us at
`s0-c1-s`, 4,938.5 vs 4,937.0 us at `s0-c8-s`). The page work that
distinguishes them is invisible under the wait both share (section 5), the
same reading the f6ed10c run reached.

## 5. Where the time goes

A transaction latency is the sum of its four statements' round trips, and
the four statements account for all of it here (each mean times its ops
over the transaction's mean times its ops):

| cell | trade-insert | account-update | unattributed (driver, socket, scheduling between statements) | txn mean |
|---|---|---|---|---|
| `s0-c1-g` | 50.0 % | 50.0 % | 0.0 % | 13,017 µs |
| `s0-c8-g` | 49.5 % | 50.5 % | 0.1 % | 11,556 µs |
| `s0-c1-s` | 50.0 % | 50.0 % | 0.0 % | 44,064 µs |
| `s0-c8-s` | 50.1 % | 49.9 % | 0.0 % | 22,520 µs |

So the decomposition is by statement, and inside a statement by what the
group-versus-strict pair exposes:

| Wait | Estimate | How derived |
|---|---|---|
| Durability (fsync / group flush) | dominant. `trade-insert` p50 is 2,884.6 us under `group` and 9,907.6 us under `strict` at one core, so about 7,000 us (71 %) of a strict statement is the durability wait; at eight cores 2,505.8 vs 4,938.5 us, about 2,430 us (49 %) | group-vs-strict delta at fixed cores, run 1 |
| Queueing behind other clients on the same reactor | large at `cores = 1`. Eight traders share one reactor and one commit path: `s0-c1-s` runs 169.4 tps x 4 statements = 678 durable statements per second, one every 1.5 ms, and the per-statement p50 of 9.9 ms is about seven of them in line | tps arithmetic and the p50, one core |
| Page work (descent, leaf write, splits) | not separable: inside the 0.4 % by which insert and update agree | the two phases' p50s |
| Lock or conflict wait | no client-visible refusal except the one refused insert per `c8-s` run (section 6); the server logs of `r3-s0-c8-g` and `r3-s0-c8-s` each also carry one `catalog changed ... runs again` re-run of an `UPDATE accounts`, absorbed by the server | error columns, server logs |
| Client and socket round trip | present in every number and not isolated (no `--server-log` was used); the `p0` of `trade-insert`, 1,125 to 1,831 us across the four cells of run 1, is the cheapest trade insert each cell reached, an upper bound on the fixed part | phase p0 |

## 6. Correctness, refusals and the one anomaly

`--verify 200` read back 200 balances in every one of the twelve cells and
**all matched**. Every phase of every `c1-*` cell and every `c8-g` cell
replied with zero errors.

| cell | run | committed | torn | engine error replies (all phases) | driver exit |
|---|---|---|---|---|---|
| `s0-c1-g` | 1 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `s0-c8-g` | 1 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `s0-c1-s` | 1 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `s0-c8-s` | 1 | 5,000 txns | 1 txn | 1 reply | exit 1 |
| `r2-s0-c1-g` | 2 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `r2-s0-c8-g` | 2 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `r2-s0-c1-s` | 2 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `r2-s0-c8-s` | 2 | 5,000 txns | 1 txn | 1 reply | exit 1 |
| `r3-s0-c1-g` | 3 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `r3-s0-c8-g` | 3 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `r3-s0-c1-s` | 3 | 5,000 txns | 0 txns | 0 replies | exit 0 |
| `r3-s0-c8-s` | 3 | 5,000 txns | 1 txn | 1 reply | exit 1 |

The JSON's per-phase `errors` sum to 2 in each `c8-s` cell because the
`txn` phase counts the same refusal again as the driver's own `ERR partial`;
the engine refused one statement.

**`s0-c8-s` refused one trade insert in every run (3 of 3), and never in
this scenario's other cells (0 of 9).** The reply, identical in the three
runs, key 113 and page 138 included (only the statement's values differed):

    ERR TXN_CONFLICT retryable=1 btree descent for key 113 from page 138 gave up after 5 attempts:
    each leaf it reached had already given the key away. Either the chain is being split faster than
    a descent can cross it, or this core is descending from a root that has since grown a level

with the server logging it at `WARN [btree]` and `WARN [query]`. It is the
clustered tree's bounded re-descent (`docs/spec/heap-and-tuple.md` §5,
AT-S5c: a write descent whose leaf no longer covers the key restarts from
the root, `kMaxDescentRestarts` times, then refuses `TxnConflict`) reached
under eight traders on eight cores appending to a fresh `trades` relation.
The message names two causes and does not choose; `src/storage/btree/btree.cpp`
(the comment above the refusal) calls a **stale root** the everyday one - a
descent from a pre-growth root misses the key "every time, ... without
anything racing at all". The same key and page in 3 of 3 runs, and in run 3
a `catalog changed between this statement's resolution and its first write`
re-run logged in the same second (absorbed by the server, never a client
error), fit that reading; this run does not prove it. The same refusal
reached scenario2's `s2-c8-g` under `group` (keys 107 and 135, pages 145 and
147), so "only under `strict`" holds for this scenario alone. The refusal is
documented and `retryable=1`; **this driver does not retry**, so that leg is
lost, the transaction is counted `torn` (1) and the driver exits 1.
`docs/inflight/known-gaps.md` at `9a0525d` has no entry recording that this
workload reaches it. The measured TPS in that cell is over the 5,000
transactions that did commit.

## 7. Delta against the previous run of this shape

The comparator is
`bench/v3.0.0/results-scenario0-stockmarket-btree-v2.7.0-157-gf6ed10c.md`
section 3, read at `9a0525d` (the file is deleted from the tree; `git show
9a0525d:<path>`). **The two runs are 374 commits and many milestones apart
(`v2.7.0-157-gf6ed10c` to `v2.7.0-531-g9a0525d`), so no difference below is
attributable to one change.** The f6ed10c run is one draw per cell.

| cell | median | f6ed10c | delta |
|---|---|---|---|
| `s0-c1-g` | 626.0 tps | 750.6 tps | -16.6 % |
| `s0-c8-g` | 661.0 tps | 794.3 tps | -16.8 % |
| `s0-c1-s` | 169.4 tps | 212.1 tps | -20.1 % |
| `s0-c8-s` | 331.7 tps | 213.0 tps | +55.7 % |

Reading it:

- **`cores = 1`: -16.6 % (group) and -20.1 % (strict), outside the noise
  floor** of this run (3.8 % and 6.1 % spread over three runs). AM-S6
  (`results-am-s6-m1-baseline-v2.7.0-286-g1e7148f.md` at `9a0525d`) ran
  these two cells four times at f6ed10c: 658.4 to 773.9 tps (median 743.8,
  spread 17.5 %) and 202.1 to 209.9 tps (median 205.6, spread 3.9 %);
  against those medians the deltas are -15.8 % and -17.6 %, still outside
  both floors. The one-core path is slower than it was. Which of the
  intervening changes (locks, the instance read view, the single WAL
  stream among them) costs it is not measured here; the number says the
  sign and the size, not which milestone.
- **`c8-g` -16.8 %** is the same sign but the previous cell was one draw
  from a distribution that AM-S6 measured as bimodal (469 to 781 tps across
  four runs at f6ed10c), so it is **not a finding**.
- **`c8-s` +55.7 %** and the change of shape described in section 3 are
  large. The f6ed10c `c8-s` (213.0 tps) is a single draw, but AM-S6's four
  f6ed10c runs of it read 207.7 to 216.3 tps (spread 4.1 %), so the
  comparator is not an outlier; +55.7 % is also above the ~50 % that AM-S6
  set as the eight-core noise floor on this host.
- **The `cores = 8` cells are not the same configuration.** f6ed10c ran
  `peer_listeners = on` and `placement = namespace`, so every relation
  belonged to core 0 and the other seven reactors owned nothing. Both keys
  are retired; sessions now run where they are accepted and every core
  writes. A delta at eight cores compares two architectures.

## 8. Noise floor and host stamps

The floor is the three-run spread in section 3: 3.8 % (`c1-g`), 6.8 %
(`c8-g`), 6.1 % (`c1-s`) and **20.0 % (`c8-s`)**. Any difference below
those figures in the matching cell is not a result. The `c8-s` spread is
driven by run 1's 281.3 tps against 331.7 and 337.6 and is not explained
here.
`recovery_checkpoint_us` was not sampled; the load phase served as the
device thermometer instead: `load-users` p50 is 1.24 to 1.42 ms across all
twelve cells, so no device stall is indicated.

| cell | run | precheck UTC | loadavg 1/5/15 | build/ctest processes |
|---|---|---|---|---|
| `s0-c1-g` | 1 | 2026-09-30T06:41:18Z | 0.25 / 1.21 / 1.37 | none |
| `s0-c8-g` | 1 | 2026-09-30T06:42:36Z | 1.38 / 1.35 / 1.41 | none |
| `s0-c1-s` | 1 | 2026-09-30T06:42:46Z | 2.40 / 1.57 / 1.48 | none |
| `s0-c8-s` | 1 | 2026-09-30T06:43:37Z | 1.82 / 1.53 / 1.46 | none |
| `r2-s0-c1-g` | 2 | 2026-09-30T06:44:17Z | 1.13 / 1.40 / 1.42 | none |
| `r2-s0-c8-g` | 2 | 2026-09-30T06:45:34Z | 1.19 / 1.36 / 1.41 | none |
| `r2-s0-c1-s` | 2 | 2026-09-30T06:45:43Z | 1.80 / 1.48 / 1.45 | none |
| `r2-s0-c8-s` | 2 | 2026-09-30T06:46:33Z | 2.06 / 1.56 / 1.47 | none |
| `r3-s0-c1-g` | 3 | 2026-09-30T06:46:50Z | 2.11 / 1.60 / 1.49 | none |
| `r3-s0-c8-g` | 3 | 2026-09-30T06:48:06Z | 1.44 / 1.51 / 1.46 | none |
| `r3-s0-c1-s` | 3 | 2026-09-30T06:48:15Z | 1.75 / 1.58 / 1.48 | none |
| `r3-s0-c8-s` | 3 | 2026-09-30T06:49:09Z | 2.64 / 1.79 / 1.56 | none |

## 9. What the run says about the engine

1. **The durability wait is the workload at one core, and one reactor
   serialises it.** 169.4 tps under `strict` is one durable statement per
   1.5 ms with eight traders waiting (no other trader count was run). Eight cores let the waits
   overlap (331.7 tps, statement p50 halved to 4.9 ms). That is the first
   data point in this series where `strict` scales with `cores`; it is
   consistent with the single WAL stream (AR0 M0) letting every core append
   under core 0's latch while each core waits for its own flush, and it is
   not proven by this run, which has no server-side breakdown.
2. **`group` hides the core count, `strict` exposes it.** 626 to 661 tps for
   `group` against 169 to 332 tps for `strict`: 8x the cores buy `group`
   5.6 %, inside that cell's 6.8 % spread. What bounds `group` (the flush
   interval is one candidate) is not isolated by this run.
3. **Insert and update are one cost.** A btree append and a btree overwrite
   agree to within 0.9 % at p50 in every cell of the three runs (0.4 % in
   run 1). Whatever the leaf split costs, it is
   below the resolution of a workload whose statements each wait for
   durability.
4. **A newly created relation under eight concurrent appenders can refuse an
   insert** (section 6). The design accepts this (bounded restart, then a
   retryable refusal); this run gives the first rate for it: 1 refusal in
   10,000 trade inserts, in this scenario only under `strict` at eight
   cores, in 3 of 3 runs, and never at one core or under `group`. Scenario2
   met it under `group` at eight cores (3 refusals in 2 of 3 runs), so
   `strict` is not a condition of it.
5. **`c8-s`'s 20 % spread is the widest in the matrix**, so any later
   eight-core `strict` delta needs its own repeat before it is believed.
6. **The floor is host CPU at eight cores.** The `cores = 8` cells put eight
   reactors, eight trader processes and the reporter on eight CPUs, so how
   much of the `strict` gain is the engine and how much the client's own
   CPU is not separable here. No cell of this scenario reached a limit the
   engine can be blamed for other than the durability wait and the one
   refused insert; `cores` above the CPU count was not tried.

Archive: `bench/v3.0.0/archive/scenario0-v2.7.0-531-g9a0525d/`: per-cell
JSON, driver stdout, prechecks, server logs, configs, run scripts, the
timeline and the binary hash.
