# DL close: what moving the statistics off the statements' path costs, and buys

`instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md` §5 DL-S6, DL-Q4, DL-Q9. Premise: `bench/v3.4.16/results-dl-s0-premise-v3.4.16-2-gb0a824db.md`.

**Finding.** DL regresses no cell. On six cells measured on a quiet host (`select1`, `select8`, `fk1`, `select8off`, `cabin8`, `join8i`) the closing
commit B is never slower than A, the tree DL lands on: server CPU a statement is -0.2 to +1.2 us in A's disfavour against a within-arm spread of
0.4-2.6 us, throughput differs by 0.6-2.2 % against spreads of 2-13 %, and the one wait DL targets is gone: A waits on the `optimizer` latch 0.019 times a
point `SELECT` at `cores` 8 (0.052 on a Cabin probe), B **0.0000 times in all 69 of its lifetimes** (the only B waits on `optimizer`, `cabin_stats` or
`stats_ring` anywhere are 38 `stats_ring` waits across the four lifetimes of `storm8`'s reader arm, 1.03 million statements). **The eviction cliff is
gone**: with 6,000 statement shapes live A costs 246 us of server CPU a statement at 4,445 qps and waits 0.60 times a statement on the `optimizer` latch for
1,139 us; B costs 58 us at 20,902 qps with no wait (+188 us, 3 of 3 pairs, the arms' ranges 10.7 us and 3.1 us wide, 17 times their spread). That cell, `storm8`
and `pool8x`/`join8ix` ran on a host that was **not quiet** (load 17-25, another session's driver and builds throughout): the cliff is large enough to read
through it, the other four are **not resolved** and stated as such below. **DL-Q4's two words are not priced above the floor**: `join8ix` now exercises
its path (the shape is captured and served on every statement, and the skip switch stops the count, both in the raw rows) but the host's noise at that
moment (8.3 us of spread) hides a price that is at most a few tenths of a microsecond; `pool8x` is +0.26 us median, inside 4.5 us.

## Stamp

| item | value |
|---|---|
| executed | pass 1 2026-11-20 13:29:20-13:47:31 UTC (all 92 lifetimes); pass 2 (six cells re-run alone, gated) 14:20-14:49 UTC, stopped on the coordinator's word at 14:52 UTC during `evict8` |
| branch | `worktree-dl-statistics-queue-no-statement` |
| A | `054dbc5e` (`v3.4.17-1-g054dbc5e`, origin/main, committed 2026-11-20 08:00:46 UTC): the tree DL lands on. **Deviation from the workflow**: A is not the commit the milestone opened from (`b0a824db`); DI and DH landed on main meanwhile, and A = the opening commit would measure DI + DH + DL together. B is DL alone on top of A (10 commits `054dbc5e..30e169df`) |
| B | `30e169df`, `git describe --tags` = `v3.4.17-11-g30e169df`, committed 2026-11-20 12:37:22 UTC, DL's closing commit |
| X | B plus two measurement-only switches on no branch (`spike_patch2.py`: env `KDS_DL_NO_PLAN_COUNT` skips `CapturedPlans::CountServed`; `spike_patch3.py`: env `KDS_DL_NO_POOL_COUNT` skips the buffer pool's `hits_.Add()`); archived |
| binaries | Release, each from a clean `git archive`, copied to `/home/louis/bench-runs/dl-close/` (mtime 2026-11-20 12:59:11 UTC, after both commits), every server started from the copy: `kds_a` sha256 `5e4127041aef7c1f94f4030c943b580f2b62f215367a4a647b66cab85757e12c`, `kds_b` `f5eee235635ca9aeebcb065b44a9d1d58bf32dde45dc84fbcadcb926d3bb1544`, `kds_x` `bd04821960f7c013ac54091dccf18a221f884d8a349e445d58c61c0c1975662e` |
| device | data files under `/home/louis/bench-runs/dl-close/run` and `run2` on `/dev/nvme0n1p1` (ext4, `df -T`); 8 vCPU Intel Xeon Platinum 8581C |
| server config | `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `cores` 1 (pinned to CPU 2) or 8, `cabin_optimizer` default (off), `access_statistics = off` only in `select8off`; all else default |
| quiet gate | pass 1: held 1,747 s (13:00:23-13:29:20: other sessions' builds and test suites, idle CPU 0 % throughout), released at 96 % idle for 30 s; **it was contaminated within a minute** (another session's `cmake --build` and `ctest` started at 13:30, then two other sessions' measurement drivers, `dj_close.py` and `dd_ab.py`/`dd_close_r.py`, with their servers). Pass 2 gated every cell on a clean 20 s of the sampler and rejected a cell whose window saw any foreign process: held 1,901 s before `select1`, then 30, 20, 274, 115, 24, 20, 20, 20, 20, 20 s; `run.log` and `hold.log` in the archive. Total hold about 70 minutes, outside the budget |
| budget | running time about 42 minutes (pass 1 18 min; pass 2 24 min of which 9 min were four rejected tries of `select1` and `select8`). The coordinator's wall-clock figure since 13:29 is longer and is the one against the 1-hour milestone budget; **the budget is overrun by that count**. Cut: pass 2 did not reach `evict8` (five of eight lifetimes, unfinished and not accepted), `storm8`, `stormA8`, `pool8x`, `join8ix`; those cells are pass 1 only |
| tests | not run, by instruction: this is a measurement of built binaries; no code was changed |

## Method

`dl_close.py` (archived with its docstring listing every cell; not yet in `bench/docs/`), one server lifetime per arm per round, arms alternated per
round, fresh data file, 2 s warm + 8 s measured, server CPU = server `utime + stime` over the window divided by completed statements (it includes
kernel socket time), contention and drop counters from `sys.meta` read before and after each window. Tables: `t` of 10,000 rows for the point reads
(`SELECT * FROM t WHERE id = k`, tuple key, one size - it does not scale with rows, a departure from bench rule 9 stated here), `cabin8` over `o` of
2,000 rows with `CREATE CABIN ON o(state)`, `fk1` inserts into a child with a foreign key to a 1,000-row parent, `join8i`
`SELECT ja.v, jb.v FROM ja JOIN jb ON ja.id = jb.id WHERE ja.v = k` over two 10,000-row relations with `CREATE INDEX ixv ON ja(v)`, `evict8` 6,000
statement shapes over a 12-column `w` of 10,000 rows. `cores` 1 is one serial client, `cores` 8 is 16 closed-loop Python processes on the same 8 vCPUs,
so the server is never saturated on all eight; tails include the client. `durability = relaxed` only; no `strict` rung and one cardinality (bench
rules 4b and 9 not met).

**`join8i` exercises the captured-plan path, verified from the raw rows**: `sys.meta`'s `captured_served` delta equals the statement count in every
`join8i` and counted `join8ix` lifetime (ratio 1.0000, e.g. 2,108,459 of 2,108,459 on A and 2,155,010 of 2,155,010 on B in pass 2) and is absent (zero)
in every skipped `join8ix` lifetime, so the switch is reached and the word is the only difference. DL-S0's `join8` cell was void for the lack of this.

**Quality of each row.** Every row carries `load`, `busy` (builds/suites at the end of the window) and `other_servers` in the raw file. The six
pass 2 cells are **clean**: sampler foreign count 0 throughout their windows, `busy` and `other_servers` 0 in all 40 rows, within-arm spreads 0.4-2.6 us
(against 4-9 us in DL-S0). The pass 1 cells below are **contaminated**: `busy` 1-12 and `other_servers` up to 7 in every row from `select8off` round 1 on, `load` 17-25
in every row of `cabin8` to `join8ix`, so absolute numbers (qps, tails) are not comparable with the clean cells; only same-cell arm contrasts are read, and
only when they exceed the cell's own spread.

## Results: the six clean cells (pass 2)

Server CPU a statement, mean (per lifetime), within-arm spread = max - min of one arm's lifetimes. QPS is client-side completed statements a second.
A is origin/main `054dbc5e`; B is `30e169df`.

| cell | cores / clients | arm | CPU us/stmt (per lifetime) | CPU spread | QPS | QPS spread | statements |
|---|---|---|---|---|---|---|---|
| select1 | 1 / 1 | A | 22.74 (23.72 22.47 22.23 23.11 22.59 22.35) | 1.49 us | 23,602 | 1,658 | 1,133,656 |
| select1 | 1 / 1 | B | 22.53 (22.98 22.39 22.42 22.75 22.29 22.35) | 0.69 us | 23,742 | 891 | 1,140,366 |
| select8 | 8 / 16 | A | 34.36 (34.02 34.51 34.24 34.09 35.12 34.20) | 1.11 us | 103,630 | 13,721 | 4,990,066 |
| select8 | 8 / 16 | B | 33.69 (33.77 34.23 33.72 33.64 33.71 33.08) | 1.15 us | 104,902 | 11,863 | 5,050,556 |
| fk1 (INSERT) | 1 / 1 | A | 24.63 (24.51 25.23 24.66 24.12) | 1.11 us | 21,674 | 737 | 694,017 |
| fk1 (INSERT) | 1 / 1 | B | 23.93 (24.13 23.36 23.57 24.67) | 1.30 us | 22,180 | 1,144 | 710,240 |
| select8off | 8 / 16 | A | 34.36 (34.31 34.10 34.57 34.45) | 0.47 us | 96,772 | 7,339 | 3,105,605 |
| select8off | 8 / 16 | B | 34.11 (33.56 34.17 33.88 34.85) | 1.29 us | 97,718 | 2,736 | 3,136,286 |
| cabin8 | 8 / 16 | A | 36.51 (36.46 36.77 36.50 36.33) | 0.43 us | 92,391 | 1,427 | 2,965,425 |
| cabin8 | 8 / 16 | B | 35.34 (34.95 35.89 35.22 35.29) | 0.93 us | 93,131 | 6,217 | 2,988,575 |
| join8i | 8 / 16 | A | 65.91 (65.44 65.47 65.22 67.49) | 2.28 us | 65,661 | 5,754 | 2,108,459 |
| join8i | 8 / 16 | B | 64.94 (64.31 64.07 64.68 66.68) | 2.61 us | 67,112 | 5,856 | 2,155,010 |

No statement failed in any lifetime (`bad` = 0 in all 92 of pass 1 and all 40 of pass 2).

### Paired deltas, A minus B, same round (positive = A slower, B faster)

| cell | CPU delta per round | mean / median | A slower in | QPS delta per round (A vs B) | mean | outside the larger within-arm spread |
|---|---|---|---|---|---|---|
| select1 | +0.74 +0.08 -0.20 +0.36 +0.30 -0.00 us | +0.21 / +0.19 us | 4 / 6 | -2.9 +0.3 +1.4 -1.6 -1.8 +0.9 % | -0.6 % | no: spread 1.49 us |
| select8 | +0.25 +0.28 +0.53 +0.45 +1.42 +1.13 us | +0.67 / +0.49 us | 6 / 6 | +0.2 -0.2 -2.6 -0.5 -3.4 -0.9 % | -1.2 % | no: inside 1.15 us (sign consistent) |
| fk1 | +0.38 +1.86 +1.09 -0.55 us | +0.70 / +0.73 us | 3 / 4 | -1.3 -4.3 -5.5 +2.3 % | -2.2 % | no: spread 1.30 us |
| select8off | +0.75 -0.07 +0.69 -0.40 us | +0.24 / +0.31 us | 2 / 4 | -1.2 +3.6 -5.8 -0.4 % | -0.9 % | no: spread 1.29 us |
| cabin8 | +1.50 +0.88 +1.29 +1.04 us | +1.18 / +1.16 us | 4 / 4 | -2.9 -0.5 +2.3 -1.9 % | -0.7 % | marginally: 1.18 us against a 0.93 us B range, arms' ranges (36.33-36.77 vs 34.95-35.89) do not overlap; qps inside spread |
| join8i | +1.13 +1.40 +0.54 +0.81 us | +0.97 / +0.97 us | 4 / 4 | -1.5 -2.0 -3.5 -1.6 % | -2.2 % | no: inside 2.61 us |

**1. B against A.** DL regresses none of the six cells. `select1`, `select8off` and `fk1` are noise. B is faster by about 1 us (3 %) on `select8`
(6 of 6), `cabin8` (4 of 4, ranges disjoint) and `join8i` (4 of 4), not larger than a spread except on `cabin8`; the direction is what the work order expects
(A's collector takes a latch and counts every statement, B's gate sends `cabin_optimizer = off` past it) but the size is at the floor. `select8off`
(access statistics off) shows nothing, as it should. The bar of DL-R7 (no cell slower outside its spread) is met on these cells.

### Latency, client round trip (us; median of the per-lifetime percentiles; includes the Python client and socket)

| cell | arm | n | p0 | p25 | p50 | p90 | p99 | p99.9 | max (median) | p99 range over lifetimes |
|---|---|---|---|---|---|---|---|---|---|---|
| select1 | A | 1,133,656 | 35 | 39 | 39.5 | 46 | 56.5 | 122 | 3,403 | 53-66 |
| select1 | B | 1,140,366 | 34.5 | 39 | 39 | 47 | 56 | 119 | 3,044 | 55-60 |
| select8 | A | 4,990,066 | 29.5 | 87 | 130.5 | 261.5 | 493.5 | 1,258 | 8,902 | 402-514 |
| select8 | B | 5,050,556 | 30.5 | 85.5 | 125.5 | 247.5 | 479.5 | 1,085 | 19,859 | 428-535 |
| fk1 | A | 694,017 | 32.5 | 38 | 39 | 48 | 72.5 | 204.5 | 194,271 | 68-78 |
| fk1 | B | 710,240 | 32.5 | 38 | 39 | 46.5 | 69.5 | 181.5 | 191,951 | 68-73 |
| select8off | A | 3,105,605 | 30 | 93.5 | 137.5 | 280 | 532 | 1,461 | 12,540 | 505-578 |
| select8off | B | 3,136,286 | 28.5 | 93.5 | 138 | 275.5 | 501.5 | 1,166 | 6,939 | 475-528 |
| cabin8 | A | 2,965,425 | 31.5 | 98 | 143.5 | 282 | 538 | 2,840 | 11,389 | 512-546 |
| cabin8 | B | 2,988,575 | 29 | 96 | 141.5 | 282.5 | 532 | 3,828 | 23,214 | 487-562 |
| join8i | A | 2,108,459 | 46 | 151.5 | 217.5 | 399.5 | 681 | 1,984 | 10,447 | 630-709 |
| join8i | B | 2,155,010 | 45 | 149 | 212.5 | 390 | 642 | 1,743 | 8,496 | 603-684 |

The `fk1` max (about 190 ms in both arms) is a one-off per lifetime shared by both, not an arm difference. Tails differ between arms by less than
the lifetime-to-lifetime p99 range in every row.

### Waits a statement (count, microseconds), pooled over lifetimes, from `sys.meta`'s contention block

The `optimizer` row is the Cabin controller's collector latch (A) - in these cells (`cabin_optimizer` off, no `SHOW CABIN_OPTIMIZER` reader) nothing else takes it.

| cell | arm | `optimizer` | `cabin_stats` | `stats_ring` | `cabin_partition` | `plan_table` | `frame_table` | `lock_partition` |
|---|---|---|---|---|---|---|---|---|
| select1 | A / B | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 (no latch waited at `cores` 1) |
| select8 | A | 0.0189, 0.26 us | 0 | 0 | 0 | 0 | 0.911, 3.01 us | 0.107, 0.59 us |
| select8 | B | **0, 0** | 0 | 0 | 0 | 0 | 0.892, 2.87 us | 0.112, 0.58 us |
| select8off | A | 0.0198, 0.32 us | 0 | 0 | 0 | 0 | 0.870, 3.30 us | 0.097, 0.62 us |
| select8off | B | **0, 0** | 0 | 0 | 0 | 0 | 0.830, 3.22 us | 0.103, 0.64 us |
| cabin8 | A | 0.0520, 0.52 us | 0.0033, 0.02 us | 0 | 0.107, 0.99 us | 0 | 0.784, 3.56 us | 0.094, 0.62 us |
| cabin8 | B | **0, 0** | **0, 0** | 0 | 0.068, 0.78 us | 0 | 0.718, 3.23 us | 0.098, 0.58 us |
| join8i | A | 0.0169, 0.17 us | 0 | 0.0142, 0.07 us | 0 | 0 | 4.671, 27.3 us | 0.136, 0.88 us |
| join8i | B | **0, 0** | 0 | **0, 0** | 0 | 0.0147, 0.09 us | 4.703, 25.3 us | 0.142, 0.89 us |
| fk1 | A / B | 0 / 0 (an INSERT touches no statistics latch) | | | | | 0 | 0 |

**4. Collector waits.** Confirmed: B waits on `optimizer`, `cabin_stats` and `stats_ring` zero times in every lifetime of every cell with `cabin_optimizer` off,
including the 69 B lifetimes of both passes, except the `storm8` reader arm (below). A's waits were 0.017-0.052 a statement, 0.17-0.52 us, small
next to the `frame_table` latch (0.7-4.7 waits, 3-27 us), which is CW's and not DL's and is the largest wait of a read at `cores` 8. A new row appears on B: `join8i`
waits 0.0147 times a statement on `plan_table` (0.09 us); the latch is also waited on in the skipped `join8ix` arm (0.0119) so it is not the served count, and the
counted/skipped contrast below does not move it. It is the captured-plan table's own latch (CX's, not a statistics store); it was not waited on in A's
`join8i`, where `stats_ring` waited instead (0.0142 waits, 0.07 us), so DL moved the wait off the statistics ring onto the plan table at about the same size.
Why is not shown here (one run, no isolating cell).

## Results: the cells that ran on a contended host (pass 1 only)

Pass 2 was stopped before these cells. Every row below overlapped another session's driver or build: `load` 17-25, `busy` and `other_servers` non-zero in every row
(per-row fields in `raw-pass1-contaminated.jsonl`), so the absolute CPU is 2-3 us above the clean cells' and the qps is a third to a half of them; the contrast
inside a cell is what is read.

| cell | arm | CPU us/stmt (per lifetime) | CPU spread | QPS | QPS spread | statements | load range |
|---|---|---|---|---|---|---|---|
| evict8 | A | 246.13 (242.21 243.29 252.90) | 10.69 us | 4,445 | 278 | 106,849 | 22.0-24.8 |
| evict8 | B | 58.27 (57.20 60.29 57.33) | 3.09 us | 20,902 | 2,014 | 503,261 | 22.8-24.2 |
| storm8 | reader | 44.05 (42.46 43.63 43.03 47.09) | 4.63 us | 32,069 | 2,802 | 1,031,875 | 23.6-25.3 |
| storm8 | quiet | 40.25 (38.74 38.31 41.81 42.13) | 3.82 us | 34,139 | 4,821 | 1,098,153 | 24.0-24.4 |
| stormA8 (on A) | reader | 41.79 (44.56 39.81 40.98) | 4.75 us | 47,233 | 25,291 | 1,138,366 | 23.2-24.9 |
| stormA8 (on A) | quiet | 37.38 (37.70 38.12 36.31) | 1.81 us | 46,743 | 22,075 | 1,126,581 | 21.3-25.2 |
| pool8x (on X) | counted | 36.73 (35.49 35.83 35.63 39.99) | 4.50 us | 57,691 | 23,242 | 1,852,816 | 18.6-20.9 |
| pool8x (on X) | skipped | 36.47 (36.26 35.45 35.38 38.79) | 3.42 us | 56,036 | 21,023 | 1,801,147 | 18.5-20.8 |
| join8ix (on X) | counted | 64.02 (65.43 61.57 65.13 63.93) | 3.85 us | 33,354 | 19,920 | 1,071,898 | 16.3-22.7 |
| join8ix (on X) | skipped | 65.03 (68.08 59.76 67.03 65.26) | 8.32 us | 33,835 | 34,720 | 1,087,270 | 17.6-20.8 |

Paired deltas (first - second, same round): evict8 A-B **+185.0 +183.0 +195.6 us, mean +187.9, 3 of 3, qps -78.1 -78.0 -80.0 %**; storm8 reader-quiet +3.72 +5.32 +1.22 +4.96
(mean +3.80 us, 4 of 4, qps mean -5.7 %); stormA8 reader-quiet +6.86 +1.69 +4.67 (mean +4.41 us, 3 of 3, qps mean +2.7 % over a 25,000 qps spread); pool8x counted-skipped
-0.78 +0.38 +0.26 +1.19 (mean +0.26 us, median +0.32, 3 of 4, qps mean +2.7 %); join8ix counted-skipped -2.65 +1.81 -1.90 -1.33 (mean -1.02 us, counted *faster* in 3 of 4, qps mean +5.4 %).

Latency of the same cells (client round trip, us, median of per-lifetime percentiles; the `load` makes every p99 above 3 ms and none is comparable with the clean table):

| cell | arm | n | p0 | p25 | p50 | p90 | p99 | p99.9 | p99 range | p99.9 range |
|---|---|---|---|---|---|---|---|---|---|---|
| evict8 | A | 106,849 | 73 | 930 | 2,320 | 8,159 | 15,997 | 26,268 | 15,760-20,354 | 24,889-38,381 |
| evict8 | B | 503,261 | 69 | 144 | 245 | 1,948 | 7,584 | 13,648 | 6,576-7,630 | 12,459-14,162 |
| storm8 | reader | 1,031,875 | 33 | 66 | 125.5 | 1,144 | 5,922.5 | 11,997 | 5,760-6,090 | 11,780-12,336 |
| storm8 | quiet | 1,098,153 | 33.5 | 72.5 | 126 | 1,083 | 5,642.5 | 12,066 | 4,996-6,119 | 9,173-13,140 |
| stormA8 | reader | 1,138,366 | 27 | 79 | 128 | 405 | 4,069 | 7,470 | 3,742-6,149 | 6,722-12,531 |
| stormA8 | quiet | 1,126,581 | 28 | 63 | 110 | 771 | 5,050 | 11,833 | 3,598-5,224 | 6,536-12,241 |
| pool8x | counted | 1,852,816 | 27 | 75 | 121.5 | 323.5 | 3,678 | 6,679 | 3,373-4,532 | 6,438-7,723 |
| pool8x | skipped | 1,801,147 | 27 | 72.5 | 122 | 376.5 | 3,749.5 | 6,625.5 | 3,308-4,313 | 6,370-7,755 |
| join8ix | counted | 1,071,898 | 44.5 | 111.5 | 183.5 | 1,019.5 | 5,499 | 8,265.5 | 4,068-6,919 | 7,285-12,444 |
| join8ix | skipped | 1,087,270 | 43.5 | 112 | 186 | 1,482 | 6,289.5 | 10,281 | 2,051-7,255 | 7,169-13,401 |

Five of eight `evict8` lifetimes of pass 2 (unfinished, not accepted: two of its rows had `busy` 8-9; the first A lifetime was clean) agree with pass 1 and are kept in
the archive (`p2-evict8-partial-unfinished.jsonl`): A 216.14 us (187.79 249.57 211.07), 6,282 qps; B 58.30 us (59.10 57.49), 31,263 qps; A-B +128.7 and +192.1 us, qps -75.5 and -84.4 %.

### Waits a statement, contended cells

| cell | arm | `optimizer` | `cabin_stats` | `stats_ring` | `frame_table` | `lock_partition` | drop / busy counts |
|---|---|---|---|---|---|---|---|
| evict8 | A | **0.598, 1,139 us** | 0 | 0 | 0.238, 7.1 us | 0.029, 2.3 us | `statement_stats_dropped` 475 (0.0044) |
| evict8 | B | **0, 0** | 0 | 0 | 0.220, 5.4 us | 0.046, 2.0 us | `statement_stats_dropped` 142,324 (0.283) |
| storm8 | reader | 0 | 0 | 38 waits, 12.9 ms in all (about 0.00004 a statement) | 0.372, 8.4 us | 0.045, 1.7 us | `statement_stats_busy` 254 (0.00025 a statement), 21,054 reader view reads |
| storm8 | quiet | 0 | 0 | 0 | 0.330, 6.4 us | 0.049, 2.2 us | none |
| stormA8 | reader | 0.0120, 0.59 us | 0 | 0.0005, 0.05 us | 0.509, 5.1 us | 0.051, 1.0 us | A has no `_busy` counter; 25,782 view reads |
| stormA8 | quiet | 0.0132, 0.62 us | 0 | 0 | 0.417, 4.5 us | 0.054, 0.9 us | none |
| pool8x | counted / skipped | 0 / 0 | 0 / 0 | 0 / 0 | 0.467, 3.9 us / 0.446, 3.7 us | 0.068 / 0.063 | none |
| join8ix | counted | 0 | 0 | 0 | 2.172, 28.3 us | 0.075, 2.3 us | `plan_stats_part_busy` 6; `captured_served` 1,071,898 (1.0000) |
| join8ix | skipped | 0 | 0 | 0 | 2.627, 29.0 us | 0.089, 2.7 us | `plan_stats_part_busy` 7; `captured_served` 0 |

**2. `evict8`: the cliff is gone.** With 6,000 shapes against the collector's 4,096-entry table A pays 246 us of server CPU and 1,139 us of `optimizer`-latch
wait a statement (0.60 waits), 4,445 qps, p50 2,320 us; B pays 58 us, no wait, 20,902 qps, p50 245 us: -76 % CPU, 4.7 times the throughput, 3 of 3 pairs, the arms' ranges
(242-253 us against 57-60 us) 17 times the spread, and the pass 2 lifetimes agree (A 216 us, B 58 us). This is the cell the work order was built for
and it reads cleanly despite the host. What B still pays is not the collector's: 58 us against 34 us on a point `SELECT` is the extra planning of 6,000
distinct statements, and **0.28-0.30 of the statements' `sys.statement_stats` records are dropped** in B (the table is full and the record gives up instead of
waiting) against 0.004-0.065 in A, whose statements are too slow to fill it; the drop rate follows the statement rate here and is reported without a bar (DL-Q9).

**3. `storm8` against `stormA8`.** Not resolved. The host's load (23-25) puts every p99 at 4-6 ms and the lifetime-to-lifetime ranges of one arm (p99 5.0-6.1 ms) cover the reader arm's. On B the reader arm's p99 is 5,923 us
against 5,643 us quiet and p99.9 11,997 against 12,066 us: inside the quiet arm's range, so DL-Q9's M3 bar (inside the no-reader arm's spread) is not failed here, and it is not shown to hold on
a quiet host either. A shows the same: p99 4,069 us with the reader, 5,050 quiet (the reader arm lower), p99.9 7,470 against 11,833. The CPU contrasts (+3.8 us on B, +4.4 us on A) are inside both arms'
4-5 us spreads and equal each other. What the counts say: on B the reader of `sys.statement_stats`, `sys.relation_stats` and `sys.meta` (21,054 view reads over four 8 s windows, about 650 a second) made
1.03 million statements wait **38 times in total** on `stats_ring` (12.9 ms, 0.00004 a statement, A: 0.0005 waits a statement, 0.05 us, plus 0.012 `optimizer` waits) and dropped **254 `statement_stats` records instead of waiting (`statement_stats_busy`,
0.00025 a statement)**; no other `*_busy` count (`plan_stats_part_busy`, `cabin_*`) moved in any storm lifetime. So the mechanism DL set out to add (drop and count, rather than wait) is visible and small:
a reader costs the described statements about 1 in 4,000 records of one store and no measurable wait. Whether it moves the tail needs a quiet host.

## DL-Q4: the two instance-shared words (X: counted - skipped)

| word | cell | CPU counted - skipped, per round | mean / median | counted slower in | QPS counted vs skipped, mean | within-arm CPU spread | above the floor |
|---|---|---|---|---|---|---|---|
| buffer pool's shared hit count (`hits_.Add()`), every page hit | pool8x (point `SELECT`, `cores` 8) | -0.78 +0.38 +0.26 +1.19 us | +0.26 / +0.32 us | 3 / 4 | +2.7 % (counted higher) | 4.50 / 3.42 us | **no** |
| captured plans' served count (`CountServed`), every compile of a captured shape | join8ix (`join8i`'s shape, `cores` 8) | -2.65 +1.81 -1.90 -1.33 us | -1.02 / -1.61 us | 1 / 4 | +5.4 % (counted higher) | 3.85 / 8.32 us | **no** |

**5.** Neither word is priced above the noise floor, and this run cannot say how far below it each is. Both cells ran at load 17-23 with spreads of 3.4-8.3 us (CPU) and 20,000-35,000 qps; the
signs of the paired deltas change from round to round, and the counted arm of `join8ix` is faster on the mean. The only reading these data support is an upper bound: whichever word costs
anything costs under the roughly 3-4 us the host's noise hides (about 10 % of a point read, 5 % of a join), and the median deltas (+0.32 us, -1.61 us) suggest well under 1 us. DL-Q4's
standing choice (a), the words left as they are, is neither confirmed nor contradicted by a price; a quiet re-run of the two cells (eight lifetimes each is about 4 minutes) is the missing
evidence. `join8ix` is, unlike DL-S0's `join8`, a valid cell: the counted arm served the shape on every statement and the skipped arm counted none.

## What it says

- **The latch that mattered is gone and nothing replaced it in cost.** A's `optimizer` waits (0.019 a point `SELECT`, 0.052 a Cabin probe, 0.60 under 6,000 shapes) are zero on B everywhere
  and the clean-cell CPU is the same or about 1 us lower. At `cores` 8 a read's largest wait is the frame table's latch (0.7-4.7 waits, 3-27 us), which DL did not touch (CW's).
- **DL's gain is on the extreme, not the middle.** At the comfortable middle the collector cost about a microsecond (select8, cabin8, join8i: +0.7 to +1.2 us in A, near the spread); at 6,000 live shapes it
  cost 188 us a statement and 79 % of the throughput. That is the shape of the premise DL-S0 measured (+150.9 us, -85.8 %), reproduced on the merged tree with the fix in place: B at 58 us is DL-S0's "unset" arm (51.9 us), so the
  rule ("a switched-off consumer collects nothing") brings the default configuration to the no-collector floor.
- **Drop-and-count has a visible rate under saturation of the statement table** (0.28-0.30 of records at 6,000 shapes in B). The rule says a statistic never waits; it does not say none is lost, and
  DL-Q9 set no bar. A statement-stats table that drops 3 in 10 records at 6,000 shapes is worth a decision of its own.
- **A contradicted expectation, none**: the work order's expectation that no `optimizer`, `cabin_stats` or `stats_ring` latch is waited on with the optimizer off is met in 69 of 69 B lifetimes.

## Limits

- **Pass 1's later cells and `evict8`, `storm8`, `stormA8`, `pool8x`, `join8ix` ran beside other sessions' builds, suites and measurement drivers** (load 17-25; `busy` and `other_servers` in every row). Only `evict8`
  clears that noise. `storm8`/`stormA8` (DL-Q9's tail bar) and both DL-Q4 words are **not resolved**; their quiet re-run was cut by the budget (the 81-minute wall clock the coordinator named), and by a host
  that did not go quiet for 70 minutes of holds.
- The pass 2 six-cell comparison is clean, but each A/B arm pair is 4-6 lifetimes of 8 s: a delta under about 1 us is not claimed.
- One cardinality (10,000 rows; 2,000 and 1,000 for `cabin8` and `fk1`'s parent); no three-size sweep (bench rule 9). `durability = relaxed` only; no `strict` (rule 4b's durability axis). Clients are Python processes on the same
  vCPUs as the server, so tails include client time and the server is not saturated on all eight.
- The wait census is the contention block's latches only; fsync, client round trip and lock waits are not decomposed (rule 3 does not apply to a CPU-per-statement comparison of one statement shape).
- `sys.meta` counters read by the driver: `captured_served`, `*_busy`, `*_dropped`, `contention_*`. A cell's per-statement waits are pooled counts over its lifetimes, not per-lifetime figures.
- No baseline against PostgreSQL (rule 4: the floor was not measured for this shape). The baseline is A, this engine's own origin/main; DL-S0's premise numbers (`v3.4.16-2-gb0a824db`, an older commit under a different load) are cited only for `evict8`, whose A arm here
  (246 us, 4,445 qps) is the same order as S0's collector-set arm (203 us, 6,298 qps).

Raw data and tools: `bench/v3.4.17/archive/dl-close-v3.4.17-11-g30e169df/` (`raw-pass1-contaminated.jsonl`, `p2-select1.jsonl`, `p2-select8.jsonl`, `p2-fk1.jsonl`, `p2-select8off.jsonl`,
`p2-cabin8.jsonl`, `p2-join8i.jsonl`, `p2-evict8-partial-unfinished.jsonl`, `rejected-*.jsonl` (the four contaminated tries), `dl_close.py`, `pass2.py`, `analyze.py`, `an-p2-clean.txt`, `an-pass1.txt`,
`spike_patch2.py`, `spike_patch3.py`, `gate_run.sh`, `sampler.sh`, `sampler-pass1.log`, `sampler2.log`, `gate-pass1.status`, `run.log`, `hold.log`, `driver.start`, `driver.end`).
