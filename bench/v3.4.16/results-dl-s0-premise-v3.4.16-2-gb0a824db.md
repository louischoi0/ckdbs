# DL-S0 premise: what a statistic costs the statements it describes

`instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md` §5 "DL-S0's cells" M1-M4, DL-Q4.

**Finding.** The run was not quiet and only one effect is large enough to read through the noise: **the
collector's eviction scan.** Once more than 4,096 statement shapes are live, a statement costs **202.8 us of
server CPU with the collector set against 51.9 us with it unset (+150.9 us, 3 of 3 paired lifetimes, 6,298 against
46,656 statements a second, -85.8 %)**, and the statements wait 0.65 times each on the `optimizer` latch for **970 us a
statement**. That is 7 times the arm's own spread, so it is outside it, and it is the one cell that approached a limit
(the collector's latch). Everything else is **not resolved**: a second, ungated measurement session (`di-close`,
16 more client processes and two more servers) ran on the same 8 vCPUs for 10 of the 11 minutes, so the within-arm spread at
`cores` 8 is 6-8 us of CPU and 40-60 % of throughput, and the collector's price on a point `SELECT`
(+0.94 us at `cores` 1, +1.28 us at `cores` 8, 3 of 4 lifetimes slower each) is inside it. What is measured and
clean is the **wait census**: the `optimizer` latch is the only one of `optimizer`, `cabin_stats` and `stats_ring` a plain
point `SELECT` waits on, 0.0201 waits and 0.62 us a statement at `cores` 8, and **it falls to exactly zero when the collector
is unset**, so those waits are the collector's. The `join8` cell **did not exercise its path** (its shape is never
captured, so the counter it switches off is never reached) and DL-Q4's captured-plan word is not priced; the diagnostic
that shows why, and a variant that does exercise it, are in the archive but were not run as a cell because the host never
went quiet again (a 63-minute hold).

## Stamp

| item | value |
|---|---|
| executed | 2026-11-20 05:27:13-05:38:13 UTC (driver start / DONE) |
| branch / commit | `worktree-dl-statistics-queue-no-statement` at `b0a824db`, `git describe --tags` = `v3.4.16-2-gb0a824db`, committed 2026-11-20 04:58:28 UTC |
| binary | **`b0a824db` plus a measurement-only spike that is on no branch**: a clean `git archive b0a824db` at `/home/louis/ck-tmp/dl-s0`, patched by `spike_patch.py`, `spike_patch2.py`, `spike_patch3.py` (archived), Release build, mtime 2026-11-20 05:22:57 UTC (after the commit). Four env switches, each read once: `KDS_DL_NO_COLLECTOR` (no dispatcher and no Cabin store is handed the Cabin controller's signal collector), `KDS_DL_NO_CABIN_COUNT` (`CabinStore::NoteHit`/`NoteMiss`/`NoteHint` return at once), `KDS_DL_NO_PLAN_COUNT` (`CapturedPlans::CountServed` skipped), `KDS_DL_NO_POOL_COUNT` (the buffer pool's hit count skipped). With none set the engine is `b0a824db`. Copy `/home/louis/bench-runs/dl-s0/kds_spike`, sha256 `77150cd56b6e0ff2a028334124f212c53afb75da6dcd0b51d278041ce8b0ffb7`; every server started from the copy |
| tree | the measured tree is the archive, clean by construction. The DL worktree itself became dirty (another agent's edits, builds and test runs) after the run began and was not measured |
| device | data files under `/home/louis/bench-runs/dl-s0/run` on `/dev/nvme0n1p1` (ext4, `df -T`); host 8 vCPU Intel Xeon Platinum 8581C |
| server config | `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `cores` 1 (pinned to CPU 2) or 8, `cabin_optimizer` default (off) except `tick8` (on, `cabin_optimizer_snapshot_interval_ms = 100`); all else default |
| quiet gate | held 196 s (05:23:56 to 05:27:12: another session's `ctest -j8` for about 150 s, then the 45 s settle; load average 4.77 at the start). **The gate did not cover the other session's measurement driver** (`di-close`: `di_arms.py` and `kds_A`/`kds_B`, started 05:28:27, finished 05:37:23): it ran on the same host through every cell from `select1` round 4 to about the middle of `cabin8`. The sampler (`sampler.log`, from 05:31:38) saw another `kds_*` server in 65 of 79 five-second samples; per-row `load` is 2.6-4.5 at `cores` 1 and 6-23 at `cores` 8 (the run's own 16 clients alone make about 17). A resident `build-sandbox/kds_server` of another project (about 10 % of one CPU) ran throughout. The driver's per-row `busy` (build or suite) was 0 except `select1 set` round 1 and `cabin8 counted` round 4 (1, transient) |
| budget | the run took 11 minutes of the 20. A second pass over the contaminated cells (`go2.sh`, then `go3.sh` with the `join8i` variant) waited 63 minutes (05:38:52-06:42:09) for a quiet host and gave up twice: builds and a full test suite in this worktree and the other session's second driver kept the load above 4. That hold is outside the budget; nothing in this document is from a second pass |

## Method

`dl_s0.py` (archived; documents its own cells in its docstring; not yet in `bench/docs/`). One server lifetime per arm per
round, arms alternated per round (the first arm first in odd rounds), fresh data file, 2 s warm then 8 s measured. Server CPU is
`utime + stime` of the server over the measured window divided by completed statements, so it includes kernel socket time.
Before and after each window the driver reads `sys.meta` and stores the non-zero deltas of its contention block, so every
per-statement wait below is a count the engine kept, divided by the statements that window completed. Rows: `t` of 10,000 rows
(`CREATE TABLE ... BTREE`), `SELECT * FROM t WHERE id = <random>` (a tuple key point read: it does not scale with rows, one size
measured, a departure from bench rule 9). `cores` 1 is one serial client with the server pinned to CPU 2; `cores` 8 is 16
closed-loop Python processes on the same 8 vCPUs, so the server is not saturated on all eight and the clients compete with it.

| cell | cores | clients | arm A / arm B (same binary) | lifetimes per arm | question |
|---|---|---|---|---|---|
| select1 | 1 | 1 | collector set / unset (`KDS_DL_NO_COLLECTOR`) | 4 | M2, M1 |
| select8 | 8 | 16 | collector set / unset | 4 | M2, M1 |
| insert1 | 1 | 1 | keyed `INSERT`, collector set only | 2 | M1 |
| insert8 | 8 | 16 | keyed `INSERT`, collector set only | 2 | M1 |
| storm8 | 8 | 16 + 1 reader | reader looping `sys.statement_stats`, `sys.relation_stats`, `sys.meta` / no reader | 3 | M3 |
| tick8 | 8 | 16 | `cabin_optimizer = on`, 100 ms tick / off | 3 | M3 |
| evict8 | 8 | 16 | 6,000 distinct statement shapes (`w` of 12 columns, four of them filtered with an operator each; `id = k` in every one): collector set / unset | 3 | M2 (the eviction scan) |
| pool8 | 8 | 16 | `select8`'s statement: the buffer pool's hit count counted / skipped (`KDS_DL_NO_POOL_COUNT`) | 3 | DL-Q4 |
| join8 | 8 | 16 | `ja JOIN jb ON ja.id = jb.id WHERE ja.id = k` (two 10,000-row relations): `CountServed` counted / skipped (`KDS_DL_NO_PLAN_COUNT`) | 3 | DL-Q4 |
| cabin8 | 8 | 16 | `SELECT * FROM o WHERE state = k`, `o` of 2,000 rows with `CREATE CABIN ON o(state)`: Cabin counters as built / skipped (`KDS_DL_NO_CABIN_COUNT`) | 4 | M4 |

Every round count is the driver's default for the cell; none was cut. Rows are 10,000 except `cabin8` (2,000); the three-size sweep of
bench rule 9 was not run.

## Results

Spread = max - min of one arm's lifetimes. QPS is client-side completed statements a second. Raw: `raw.jsonl`, `analysis.txt`.

| cell | arm | server CPU a statement, mean (per lifetime) | CPU spread | QPS, mean | QPS spread | statements | bad |
|---|---|---|---|---|---|---|---|
| select1 | set | 23.71 us (23.63 22.67 22.17 26.36) | 4.19 us | 21,955 qps | 6,146 qps | 703,019 | 0 |
| select1 | unset | 22.77 us (23.28 22.39 22.58 22.82) | 0.89 us | 23,297 qps | 816 qps | 746,028 | 0 |
| select8 | set | 37.18 us (38.45 36.09 39.07 35.12) | 3.94 us | 72,282 qps | 33,969 qps | 2,323,793 | 0 |
| select8 | unset | 35.90 us (39.87 35.11 34.51 34.12) | 5.75 us | 80,707 qps | 37,325 qps | 2,592,417 | 0 |
| insert1 | set | 33.89 us (32.56 35.22) | 2.66 us | 15,092 qps | 31 qps | 241,656 | 0 |
| insert8 | set | 53.13 us (54.30 51.95) | 2.35 us | 54,521 qps | 1,276 qps | 875,825 | 0 |
| storm8 | reader | 41.09 us (44.27 38.76 40.24) | 5.52 us | 65,806 qps | 26,424 qps | 1,587,145 | 0 |
| storm8 | quiet | 37.23 us (36.08 34.74 40.86) | 6.12 us | 74,217 qps | 37,753 qps | 1,788,124 | 0 |
| tick8 | on | 35.68 us (35.60 36.44 34.99) | 1.45 us | 86,190 qps | 5,983 qps | 2,076,245 | 0 |
| tick8 | off | 36.64 us (35.48 38.75 35.69) | 3.28 us | 76,394 qps | 29,107 qps | 1,841,485 | 0 |
| evict8 | set | 202.78 us (201.16 192.97 214.22) | 21.25 us | 6,298 qps | 1,390 qps | 151,396 | 0 |
| evict8 | unset | 51.90 us (57.43 48.50 49.78) | 8.93 us | 46,656 qps | 23,528 qps | 1,123,995 | 0 |
| pool8 | counted | 37.42 us (35.32 41.33 35.61) | 6.01 us | 73,764 qps | 40,985 qps | 1,777,234 | 0 |
| pool8 | skipped | 35.55 us (34.83 36.93 34.90) | 2.11 us | 81,644 qps | 15,329 qps | 1,969,386 | 0 |
| join8 | counted | 54.83 us (54.80 53.99 55.70) | 1.72 us | 47,161 qps | 9,970 qps | 1,136,749 | 0 |
| join8 | skipped | 51.14 us (51.16 51.49 50.76) | 0.73 us | 70,550 qps | 73 qps | 1,699,202 | 0 |
| cabin8 | counted | 37.82 us (43.15 36.34 35.37 36.43) | 7.78 us | 87,876 qps | 54,632 qps | 2,820,950 | 0 |
| cabin8 | skipped | 36.91 us (40.64 37.16 35.58 34.24) | 6.39 us | 89,888 qps | 47,244 qps | 2,885,301 | 0 |

No statement failed in any lifetime (`bad` = 0 in all 58). The lifetimes of the `cores` 8 cells fall into two groups on one cell's
own numbers: about 34-36 us and 85,000-91,000 qps in a quiet-looking lifetime, 38-44 us and 42,000-65,000 qps in a contended one
(for instance `select8` set round 3, `pool8` counted round 2, `join8` counted rounds 1-3). Which group a lifetime fell in follows
the other session's driver, not the arm.

### Paired deltas, first arm minus second, same round

| cell | CPU delta a statement, per round | mean | first arm slower in | QPS delta, per round | mean | outside the arm's own spread |
|---|---|---|---|---|---|---|
| select1 (set - unset), all 4 rounds | +0.35 +0.28 -0.41 +3.54 us | +0.94 us | 3 / 4 | -2.9 -0.1 +1.6 -22.1 % | -5.9 % | no: inside the 4.19 us range |
| select1, rounds 1-3 only (round 4's set lifetime started at about 05:28:30, as the other driver did, and read 26.36 us; this exclusion is after the fact and is shown beside the full set, not instead of it) | +0.35 +0.28 -0.41 us | +0.07 us | 2 / 3 | -2.9 -0.1 +1.6 % | -0.5 % | no: inside the 1.46 us set range |
| select8 (set - unset) | -1.42 +0.97 +4.56 +1.00 us | +1.28 us | 3 / 4 | +18.9 -10.7 -36.9 -2.1 % | -7.7 % | no: inside the 5.75 us range |
| storm8 (reader - quiet) | +8.19 +4.01 -0.62 us | +3.86 us | 2 / 3 | -35.4 -11.3 +26.8 % | -6.6 % | no |
| tick8 (on - off) | +0.13 -2.31 -0.70 us | -0.96 us | 1 / 3 | +7.1 +39.5 +0.2 % | +15.6 % | no |
| evict8 (set - unset) | +143.73 +144.47 +164.44 us | +150.88 us | 3 / 3 | -80.7 -87.8 -88.9 % | -85.8 % | **yes**: the arm ranges (193-214 us against 48-57 us) do not touch; 7 times the larger range |
| pool8 (counted - skipped) | +0.49 +4.40 +0.71 us | +1.87 us | 3 / 3 | -2.2 -34.9 +4.4 % | -10.9 % | no: median round +0.71 us; the +4.40 round is a contended counted lifetime (p50 247 us) |
| join8 (counted - skipped) | +3.64 +2.50 +4.94 us | +3.70 us | 3 / 3 | -34.0 -25.6 -39.8 % | -33.1 % | the arms do not overlap, **and the arms ran the same code** (see below), so this is the floor and not a finding |
| cabin8 (counted - skipped) | +2.52 -0.82 -0.22 +2.18 us | +0.92 us | 2 / 4 | -19.9 +13.3 -1.9 -5.3 % | -3.4 % | no: inside the 7.78 us range |

### Latency (client round trip, us; median of the per-lifetime percentiles)

Includes the Python client and the socket; it is not server time. Counts are the statements of all of an arm's lifetimes.

| cell | arm | n | p0 | p25 | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|
| select1 | set | 703,019 | 35 us | 39 us | 40.5 us | 49.5 us | 68 us | 140.5 us |
| select1 | unset | 746,028 | 35 us | 39 us | 39 us | 47.5 us | 66 us | 150.5 us |
| select8 | set | 2,323,793 | 29 us | 109 us | 169 us | 412.5 us | 933 us | 2,581.5 us |
| select8 | unset | 2,592,417 | 29 us | 97.5 us | 143 us | 308.5 us | 690 us | 2,441 us |
| insert1 | set | 241,656 | 32.5 us | 50 us | 56.5 us | 84 us | 141.5 us | 639 us |
| insert8 | set | 875,825 | 28.5 us | 112.5 us | 175.5 us | 510 us | 1,474 us | 3,936 us |
| storm8 | reader | 1,587,145 | 28 us | 108 us | 172 us | 464 us | 1,224 us | 3,302 us |
| storm8 | quiet | 1,788,124 | 29 us | 99 us | 147 us | 329 us | 982 us | 2,438 us |
| tick8 | on | 2,076,245 | 29 us | 99 us | 148 us | 307 us | 665 us | 2,243 us |
| tick8 | off | 1,841,485 | 28 us | 100 us | 147 us | 344 us | 795 us | 2,893 us |
| evict8 | set | 151,396 | 63 us | 1,172 us | 2,015 us | 4,913 us | 9,891 us | 14,703 us |
| evict8 | unset | 1,123,995 | 57 us | 174 us | 256 us | 547 us | 1,235 us | 3,203 us |
| pool8 | counted | 1,777,234 | 28 us | 100 us | 150 us | 315 us | 716 us | 3,075 us |
| pool8 | skipped | 1,969,386 | 30 us | 98 us | 143 us | 319 us | 850 us | 2,837 us |
| join8 | counted | 1,136,749 | 38 us | 146 us | 246 us | 664 us | 1,628 us | 3,804 us |
| join8 | skipped | 1,699,202 | 36 us | 121 us | 184 us | 378 us | 808 us | 2,606 us |
| cabin8 | counted | 2,820,950 | 30.5 us | 90.5 us | 134 us | 262 us | 521.5 us | 1,725 us |
| cabin8 | skipped | 2,885,301 | 29.5 us | 94.5 us | 139 us | 281 us | 569 us | 1,929 us |

The `cores` 8 tails vary between lifetimes of one arm by more than the arms differ (`select8` set p99 609-1,183 us over four
lifetimes, unset 590-1,121 us; per-lifetime values in `detail.txt`), so no tail difference outside `evict8` is a finding.

### Waits (M1), per statement, from the contention block

Count of waits and microseconds of wait a statement, mean over the arm's lifetimes, from `sys.meta`'s contention block (`topwaits.txt`,
`analysis.txt`). `kOptimizer` is two latches (the collector's and the Cabin controller's view latch); in a cell with no `SHOW CABIN_OPTIMIZER`
reader and `cabin_optimizer` off the only other taker of the view latch is core 0's tick, so the `optimizer` rows are the collector's, and the
unset arms (where there is no collector) confirm it by reading zero.

| cell | arm | `optimizer` | `cabin_stats` | `cabin_partition` | `stats_ring` | largest other latch (not a DL store) |
|---|---|---|---|---|---|---|
| select1 | set | 0 waits, 0 us | 0, 0 us | 0, 0 us | 0, 0 us | none: no latch waited at all at `cores` 1 |
| select1 | unset | 0, 0 us | 0, 0 us | 0, 0 us | 0, 0 us | none |
| insert1 | set | 0, 0 us | 0, 0 us | 0, 0 us | 0, 0 us | none (a transaction-id carve, 29-30 a window) |
| select8 | set | **0.0201 waits, 0.622 us** | 0, 0 us | 0, 0 us | 0, 0 us | `frame_table` 0.678 waits, 4.24 us |
| select8 | unset | **0, 0 us** | 0, 0 us | 0, 0 us | 0, 0 us | `frame_table` 0.729 waits, 4.03 us |
| insert8 | set | 0, 0 us | 0, 0 us | 0, 0 us | 0, 0 us | `window` 0.092 waits, 20.2 us; `frame_table` 4.19 waits, 16.1 us |
| storm8 | reader | 0.0182 waits, 0.610 us | 0, 0 us | 0, 0 us | **0.0006 waits, 0.029 us** | |
| storm8 | quiet | 0.0211 waits, 0.709 us | 0, 0 us | 0, 0 us | 0, 0 us | |
| tick8 | on | 0.0204 waits, 0.575 us | 0, 0 us | 0, 0 us | 0, 0 us | |
| tick8 | off | 0.0200 waits, 0.633 us | 0, 0 us | 0, 0 us | 0, 0 us | |
| evict8 | set | **0.6531 waits, 969.96 us** | 0, 0 us | 0, 0 us | 0, 0 us | `frame_table` 0.214 waits, 1.48 us |
| evict8 | unset | 0, 0 us | 0, 0 us | 0, 0 us | 0, 0 us | `frame_table` 0.470 waits, 3.99 us |
| pool8 | counted | 0.0219 waits, 0.798 us | 0, 0 us | 0, 0 us | 0, 0 us | |
| pool8 | skipped | 0.0221 waits, 0.626 us | 0, 0 us | 0, 0 us | 0, 0 us | |
| cabin8 | counted | 0.0521 waits, 0.743 us | 0.0033 waits, 0.044 us | 0.1056 waits, 1.505 us | 0, 0 us | `frame_table` 0.870 waits, 3.95 us |
| cabin8 | skipped | 0.0183 waits, 0.384 us | 0, 0 us | 0.0691 waits, 1.261 us | 0, 0 us | `frame_table` 0.855 waits, 4.00 us |

`stats_ring` waited only in the reader arm of `storm8` (about 950 waits over 1.59 million statements, 0.029 us a statement) and in
no other lifetime. The reader made 10,479, 12,861 and 8,748 view reads in its three 8 s windows. `tick8` took 240 ticks over three windows
(80 a window, as 100 ms asks for), so the tick ran, and it moved the `optimizer` waits from 0.0200 to 0.0204 a statement. The `optimizer` wait a
statement is 1 statement in 50 at `cores` 8, about 31 us when it happens; at `cores` 1 a single reactor cannot meet itself, and no latch
counted a wait.

## Against this engine's own previous numbers

`bench/v3.4.14/results-da-close-v3.4.14-32-g89c26252.md`, its B arm (`v3.4.14-32-g89c26252`), the same host and driver shape (the collector is set in that
engine, as in this run's default arm), measured on a quiet host. This run's host was not quiet at `cores` 8.

| cell | DA close B arm | this run, collector set | this run, collector unset |
|---|---|---|---|
| select1 | 22.30 us, 23,957 qps | 23.71 us, 21,955 qps (rounds 1-3: 22.82 us) | 22.77 us, 23,297 qps |
| select8 | 34.39 us, 110,509 qps | 37.18 us, 72,282 qps | 35.90 us, 80,707 qps |

At `cores` 1 the set arm is +1.41 us on the full set and +0.52 us on rounds 1-3 (DA's range was 0.91 us); the unset arm is +0.47 us. At `cores` 8 the
throughput is 27-35 % lower than DA's with CPU a statement +1.5 to +2.8 us: the other session's 16 client processes took CPU the clients and the
server had on DA's quiet run, which is the reading this host's load figures support; it is not an engine change, and the two commits' engines
were not compared here.

## What it says

- **The collector's eviction scan is the one statistic here that makes statements wait, and it is severe.** With the collector set and more shapes
  than its table's 4,096 entries (6,000 shapes, 16 clients) every statement spends 970 us waiting on the latch in the `optimizer` row, 0.65 times
  over, 202.8 us of CPU a statement, 6,298 qps, p50 2,015 us; unset the same load runs at 51.9 us, 46,656 qps, p50 256 us. This holds on a
  default configuration (`cabin_optimizer` off), where nothing reads what the collector collected: it is exactly what the work order's second rule
  ("a switched-off consumer collects nothing") removes, and its expected gain on a workload with many live shapes is a factor of about 4 in CPU
  and 7 in throughput. A caution on the unset arm: it still drops statements from `sys.statement_stats` (345,627 of 1,123,995, 30.7 %, against
  11,114 of 151,396, 7.3 %, set), the statement-stats table is a different store, and its drop rate is of the same order as the collector's price
  and is not hidden by it.
- **On a point `SELECT` the collector's price is smaller than this run can resolve.** The 8-core census gives an upper bound on the latch part: 0.62 us
  of wait a statement, one statement in 50 waiting (zero in the unset arm); the paired CPU delta (+1.28 us, 3 of 4) and throughput delta (-7.7 %) are
  inside a 5.75 us / 37,000 qps spread. At `cores` 1 it is +0.94 us (3 of 4), +0.07 us (2 of 3) on the three rounds before the other
  driver's start; the collector's price is at most about the cell's floor there, not demonstrated. DA-S0's lesson (a latch that is "free" at one core is not free
  at eight) is not contradicted: the wait is there at eight (0.62 us) and absent at one, but its price in throughput is not resolved here.
- **No `stats_ring` or `cabin_stats` wait appears on any statement that does not read a statistics view or probe a Cabin.** An
  `INSERT` at `cores` 1 and 8 touches none of the four DL latches at all (insert8 is bound by the visibility `window` wait, 20.2 us a statement, and the
  `frame_table` latch, 16.1 us); a point `SELECT` waits on `frame_table` (4.2 us), seven times what it waits on the collector.
  The frame table's latch is CW's, not DL's (`workorder-dl` §6), and the census puts it there: it is the largest wait of a read at `cores` 8.
- **A reader of statistics views costs the statements it describes almost nothing in waits, and nothing this run can resolve in tail.** About 1,300
  reads a second beside 16 clients produced 0.029 us of `stats_ring` wait a statement. The reader arm's p99 is higher (median 1,224 us against 982 us)
  and p99.9 (3,302 against 2,438 us) but each lies inside the quiet arm's own lifetime-to-lifetime range (p99 643-1,179 us, p99.9 1,799-3,645 us),
  so DL-Q9's M3 bar (inside the no-reader arm's spread) is not failed on this data and not shown to hold on a quiet host either. The controller tick
  (80 ticks, 100 ms) adds nothing measurable (0.0204 against 0.0200 optimizer waits a statement).
- **A Cabin probe's counters add latch waits, not a measurable cost.** Counted against skipped: +0.036 `cabin_partition` waits (+0.24 us), +0.0033
  `cabin_stats` waits (+0.04 us) and +0.034 `optimizer` waits (+0.36 us), about +0.65 us of added wait a statement in total, while the CPU delta
  is +0.92 us (2 of 4) in a 7.78 us range, p50 134 against 139 us, p99 522 against 569 us. The step was a `CabinProbe`
  (`ANALYZE`: `step 0 CabinProbe o cabin=1 on=col1`, `cabin_recorded=1`, in both arms). The `cabin_partition` latch is waited on in the skipped arm too
  (0.069 waits, 1.26 us), so most of it is the store's own lookup, not the counters.
- **DL-Q4's two words are not priced.** `pool8`: the buffer pool's hit count counted against skipped is +0.71 us on the median round and +1.87 us on the
  mean (3 of 3 counted slower, one round carrying a contended counted lifetime), inside a 6.01 us spread: not resolved, nothing shows it is above
  the floor. `join8` **measured nothing about `CountServed`**: the join's order is never captured, so the patch's switch (which sits in the served path)
  is never reached and both arms ran the same code. A 40 s diagnostic of this exact shape (`diag_join.out`: 351,415 statements) read
  `captured_plans = 0`, `captured_served = 0`, `plan_stats_publishes = 0` throughout; the point join is a tuple key lookup and probe in the written order and
  the steward publishes no prior for it. The same diagnostic with `CREATE INDEX ixv ON ja(v)` and `WHERE ja.v = k` (`diag_join2.py`) captured the order
  within 5 s and served 67,107 compiles in 10 s (`captured_plans = 1`), so a cell over that shape (`join8i` in `dl_s0b.py`) would exercise the word; it
  was not run. The `join8` pair is useful in one way: two arms of identical code differed by +3.70 us and -33 % in 3 of 3 rounds with non-overlapping ranges,
  which is what the other session's load did to this cell, and sets the floor for `cores` 8 cells read from this run at no less than about 4 us and 30 %.
- **The floor.** The largest same-arm spread at `cores` 1 is 4.19 us (select1 set; 1.46 us without the one lifetime that overlapped the other
  driver's start) and 0.89 us for unset; at `cores` 8 it is 5.75-8.93 us of CPU on the point-read shapes (`cabin8` counted 7.78, `storm8` quiet 6.12,
  `pool8` counted 6.01, `select8` unset 5.75) and 15,000-55,000 qps. Only `evict8` clears it.

## Limits

- **The host was shared with another session's measurement for 10 of the 11 minutes**, ungated: no delta at `cores` 8 below about 6-9 us of CPU or
  about 40 % of throughput is a finding, and the second pass that would have replaced the contended cells never ran (63 minutes of waiting, a build and a
  test suite in this worktree and the other session's second driver). A rerun on a quiet host is the first thing the work order needs before it
  prices M2, M3, M4 or DL-Q4's pool word.
- `join8` did not exercise its path; `join8i` exists, was diagnosed (not run as a cell) and is unmeasured. DL-Q4's captured-plan word is **not resolved**.
- `evict8` has three lifetimes per arm and the drop rates of `sys.statement_stats` differ between arms (above); the cell shows the collector's latch is
  the bound at 6,000 shapes, not how the cost grows with the shape count.
- One cardinality (10,000 rows, 2,000 for `cabin8`), `durability = relaxed` only, no row-count sweep (bench rule 9), no `strict` rung (rule 4b's durability
  axis); the unit of rule 3's decomposition is the contention block's latches, and client round trip, fsync and lock waits were not split out.
  Clients are Python processes sharing the vCPUs with the server, so the server was never saturated on all eight; waits and CPU are the server's, tails
  include the client.
- `insert1`/`insert8` are two lifetimes each and carry no arm to compare; they price nothing, they report that no DL latch is on an insert's path.
- The spike is one patch on one commit: `KDS_DL_NO_COLLECTOR` unsets the signal collector wherever it is handed, so the unset arm also skips everything the
  collector would have fed; it is the "consumer off collects nothing" configuration and not a smaller version of the code a DL-S1 would write.
- No PMU, no cache-line counters: where the wait-free words (the pool hit count, the Cabin counters, the served count) cost time in cache traffic
  rather than latch waits, the contention block cannot see it, and only the paired CPU delta can, which is inside the noise.

Raw data and tools: `bench/v3.4.16/archive/dl-s0-premise-v3.4.16-2-gb0a824db/` (`raw.jsonl`, `dl_s0.py`, `dl_s0b.py` with the unrun `join8i`,
`spike_patch.py`, `spike_patch2.py`, `spike_patch3.py`, `analyze.py`, `analysis.txt`, `detail.py`, `detail.txt`, `topwaits.py`, `topwaits.txt`,
`diag_join.py`, `diag_join.out`, `diag_join2.py`, `driver.log`, `run.status`, `sampler.log`, `gate.status`, `gate3.status`, `gate.sh`, `go.sh`, `go3.sh`,
`driver.start`, `driver.end`).
