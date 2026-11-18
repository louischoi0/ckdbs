# DF-S0 spike: DF's statement-path work, as spiked, adds about 0.1 to 0.35 us of server CPU per point statement, under the 0.5 us bar by its median but not resolved from the run's noise

**Thesis.** On `df-s0-spike` at `b2a4f70a` (`v3.4.12-69-gb2a4f70a`), one binary run with the spike's mask at 0 (**A**, and **A2**, the same mask as the in-run control), 63 (**B**, every spiked path) and 61 (**C**, every path but bit 2, the wait-statistics delta), on `cores = 1`, `durability = relaxed`, a point `SELECT` and a point `UPDATE ... SET v = v + 1`, each over the literal and the prepared driver path, 15 to 17 paired runs per cell: **B costs more than A by a median of +0.18 / +0.35 us (select, literal / prepared) and +0.23 / +0.11 us (update, literal / prepared) of server CPU per statement, which is 0.5 % to 1.8 % of a 20-22 us statement and below the bar of 0.5 us by every median.** The bar is not *established* by this run: the largest `A2 - A` gap in a cell (0.41 to 1.00 us) is as large as or larger than the distance from the median to the bar, and the worst single run of B - A in a cell is +0.54 to +1.29 us. By the benchmark's own rule (`bo-close`'s: median against the largest control gap) every cell is **not resolvable**, none is "over", and none can be called "inside" with the resolution this host gave. Two further readings narrow it without resolving it: B against the mean of A and A2 (the estimate that cancels the control's own offset) is +0.33 / +0.29 / +0.31 / +0.11 us, with B above that mean in 16 of 17, 14 of 15, 14 of 16 and 11 of 17 runs; and the **wait-statistics delta alone (bit 2, `B - C`) is +0.27 / +0.23 us on the point `SELECT` and +0.07 / +0.05 us on the point `UPDATE`**, the larger part of what the point `SELECT` pays in B and still inside the bar by its median. The suspicion that bit 2 is the most expensive part is borne out in size ordering on the read (B - C +0.27 / +0.23 us against C - A +0.08 / +0.10 us) and not on the write (B - C is within a quarter of the control's typical gap). **The spike prices work, not the final design**: it is cheaper than a design that records the same things only if the design's attribution is as cheap as a thread-local add, which is what the spike used. **Part C's per-index fold ran in no cell**: table `t` has no secondary index, `select-pk` is a tuple key lookup and `update-expr` runs no step chain, so bit 1 priced the executor's totals on the `SELECT` only and never took the fold's latch or map.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat` over every server thread, per statement, as `tools/bn_census.py` reports it; wall time is the client's. Scripts: `archive/df-s0-spike-v3.4.12-69-gb2a4f70a/scripts/` (`run.sh` is the exact invocation; the driver's flags are in `tools/bn_census.py`'s docstring).

| Item | Value |
|---|---|
| Executed | 2026-11-18 UTC. Build 07:07:54-07:09:16; probe (one run, verification of the arms) 07:09:30-07:10:17; measurement 07:10:36-07:30:50 (20 min 14 s, three invocations `w1`-`w3`; `archive/.../progress.txt` has every stamp); `w4` was started by the script and killed at its start (07:30:51) to stay in budget |
| Worktree / branch / commit | worktree `da-q8-memory-bound-key`, branch `df-s0-spike`, `b2a4f70a`, `git describe --tags` = `v3.4.12-69-gb2a4f70a`, committed 2026-11-18 07:06:46 UTC; `git status` clean at the start; nothing in the tree was edited. The spike is **never to land on `main`** |
| Binary provenance | Built from `git archive b2a4f70a` exported to `/home/louis/bench-runs/df-s0/src` (a clean export, not the worktree's `build-release/`), `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j6`, gcc 14.2.0, finished 07:09:16.06 UTC, after the commit time. The measured copy `bin/kds_server`: sha256 `8eb14f75b223e5a2c12b314de52d586a9c2a44d52e824a162ea9e22f658121e5` (identical to the build output; `archive/.../binaries.txt`). Every arm's server was started from that one copy |
| Arms | One binary, the mask from the environment: each arm is a two-line wrapper (`scripts/w-A`, `w-A2`, `w-B`, `w-C`: `KDS_DF_SPIKE=<mask> exec .../bin/kds_server "$@"`). Verified before the run, on a live B server, that `/proc/<pid>/exe` is the copied binary and `/proc/<pid>/environ` carries `KDS_DF_SPIKE=63`; `exec` keeps the pid, so the driver's schedstat accounting and `taskset` pinning apply to the engine itself. **A** = 0, **A2** = 0 (control), **B** = 63, **C** = 61 |
| Host / device | Intel Xeon Platinum 8581C, 8 vCPU, Linux 6.12.116+deb13-cloud-amd64. Data files and WAL under `/home/louis/bench-runs/df-s0/wd-*/` on `/dev/nvme0n1p1`, **ext4, not tmpfs** (`df -T`); removed after each invocation |
| Pinning | One server process per arm, one busy at a time, on CPU 2; the driver on CPU 4. Ports 34021, 34031, 34041 |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, other keys default; fresh server and data file per arm per run. `KDS_DF_SPIKE` is the only difference between arms |
| Drivers | `tools/bn_census.py` sha256 `35693dc24f17...` (the same bytes BO's close ran) and `tools/kwp.py` `0ce19a116ea8...`, unmodified. Gates: `--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1 --quiet-load 2 --quiet-wait 30` |
| Suite | Not executed: no code was changed by this measurement |

## 1. What was run, what was cut, what was discarded

Four arms in every run, a fresh server and data file per arm per run, the arms interleaved block by block (6 blocks per cell, the first server of a block rotating by one per block and per run), both driver paths in every run. Row mapping: `--rows 1000` is the size of table `t` (the point `SELECT` and `UPDATE` target); `--ops 2000` gives 1,998 timed statements per arm per path per run.

| invocation | start order | runs | shapes | status |
|---|---|---|---|---|
| `w1` | A, A2, B, C | 6 runs | `select-pk`, `update-expr` | kept |
| `w2` | B, C, A, A2 | 6 runs | same | kept |
| `w3` | A2, C, B, A | 6 runs | same | kept |
| `w4` | C, B, A2, A | 0 runs | same | cut: killed at its start, at the 20-minute mark; nothing kept |

Shapes: `select-pk` is `SELECT * FROM t WHERE id = ?`, `update-expr` is the plain `UPDATE t SET v = v + 1 WHERE id = ?` (the driver's `update-lit` is a constant-set variant and was not run; the spill variants were not run). 18 runs in the three invocations, 24 planned.

**Discarded as unclean.** The driver's own cell gate (`clean`: SMT siblings at most 20 % busy and foreign tasks at most 15 % of the pinned CPUs) marked 28 arm-cells unclean after its one retry, which are 7 (run, shape, path) cells of 72. All four arms of such a cell share one time window, so each is dropped whole from every paired table: run 1 select prepared, run 7 update literal and prepared (a load of 6-7 from another session on the host), run 16 select literal and prepared and update literal, run 17 select prepared. What remains is **15 to 17 paired runs per cell** (select literal 17, select prepared 15, update literal 16, update prepared 17). The raw JSON keeps every cell. A stricter filter that also drops every cell naming a competing task in its host record (other sessions' short-lived shells, and the build below) leaves 14 to 17 and changes no median by more than 0.03 us and no conclusion (`archive/.../analysis-strict.md`). Every `bad` reply count is 0. **A foreign build did land during the run**: another session's `cmake --build` (7-8 `cc1plus`, load 5-7.8) ran 07:24-07:28 UTC, inside `w3`, and the load stayed at 7.9-8.3 with no `cc1plus` to the run's end (07:28:46-07:30:46) (watchdog log `watch.txt`; one `cc1plus` also at 07:13:46 in `w1`, no effect seen; a load of 5.1-7.6 with no `cc1plus` at 07:18:46-07:20:46 in `w2`). The driver's cell gate caught it (runs 4 and 5 of `w3`, the global runs 16 and 17, are the unclean cells above, and the invocation stretched to 8 min 13 s through the gate's waits and retries); those cells are dropped and the cells that passed the gate are kept, but a build that starts and ends between two cells is not excluded by any gate other than the host record. As a sensitivity, `w1` and `w2` alone (11-12 runs per cell, before any foreign build) give B - A of +0.24 / +0.35 / +0.21 / +0.06 us and B - C of +0.29 / +0.29 / +0.01 / +0.10 us, the same picture, with the same largest control gaps (`archive/.../analysis-w1w2-only.md`).

**Cut or not run.**
- **`cores = 8`**: not run. `tools/bn_census.py` writes `cores = 1` itself and has no flag for it (`getattr(a, 'cores', 1)`); no unmodified tool under `tools/` was found that runs these arms at `cores = 8` with the server-CPU accounting, and the task forbade editing the driver. DF-Q10 (a)'s cores = 8 cell is **not measured**.
- **The sweep over 200 / 1K / 10K rows (rule 9)**: every cell is 1,000 rows. A point statement by key does not scale with rows and the claim is a fixed per-statement cost; that is the argument here, not a measurement.
- **`INSERT`, `DELETE`, range, scan, `strict` and `group` durability, concurrency (two connections sharing one part's latch)**: out of scope; the spike's latch is taken uncontended in this run, which is the case a single connection exercises.
- **The PostgreSQL floor**: no twin exists for a recording cost.
- **Latency decomposition into waits (rule 3)**: does not apply. The measured unit is server CPU, not a delay; the wall times in section 3 are context.
- **Runs**: 18 of the 24 planned; a measurement budget, not a failure.

## 2. Headline

Throughput is derived from the median server CPU of A (`1e6 / us`). Every delta is in **us of server CPU per statement**, paired by run (the four arms of one run's cell), median with [min, max] over runs and, in parentheses, the number of runs in which the second arm costs more. The resolution of the run is the `A2 - A` column. The bar is 0.5 us at `cores = 1` (DF-Q10 (a)).

| cell | runs | A | B - A (every spiked path) | C - A (without bit 2) | B - C (bit 2 alone) | A2 - A typical / largest abs | B - mean(A, A2) | verdict for B - A against 0.5 us |
|---|---|---|---|---|---|---|---|---|
| select-pk literal | 17 runs | 48,882 /s (20.46 us) | +0.18 us [-0.11, +1.00 us] (15 of 17) | +0.08 us [-0.42, +0.61 us] (11 of 17) | +0.27 us [-0.23, +0.91 us] (14 of 17) | 0.21 us / 0.70 us | +0.33 us | not resolvable (median +0.18 us, control +/- 0.70 us) |
| select-pk prepared | 15 runs | 50,555 /s (19.78 us) | +0.35 us [-0.17, +1.29 us] (13 of 15) | +0.10 us [-0.33, +1.39 us] (8 of 15) | +0.23 us [-0.76, +1.06 us] (12 of 15) | 0.29 us / 0.69 us | +0.29 us | not resolvable (median +0.35 us, control +/- 0.69 us) |
| update-expr literal | 16 runs | 45,343 /s (22.05 us) | +0.23 us [-0.13, +1.05 us] (15 of 16) | +0.14 us [-0.39, +0.82 us] (12 of 16) | +0.07 us [-0.61, +0.91 us] (10 of 16) | 0.19 us / 0.41 us | +0.31 us | not resolvable (median +0.23 us, control +/- 0.41 us) |
| update-expr prepared | 17 runs | 47,017 /s (21.27 us) | +0.11 us [-0.79, +0.54 us] (12 of 17) | +0.14 us [-0.84, +1.26 us] (10 of 17) | +0.05 us [-1.09, +0.42 us] (11 of 17) | 0.29 us / 1.00 us | +0.11 us | not resolvable (median +0.11 us, control +/- 1.00 us) |

**Reading.** Every median B - A (+0.11 to +0.35 us) is below the bar and every median is positive; none of the four cells is over it. The verdict is "not resolvable" in all four because the rule used here (median plus the largest control gap against the bar) cannot place a cost closer than the control spreads, and the control spreads 0.41 to 1.00 us at its worst run. Read as a point estimate the price of DF's statement-path work as spiked (part C's index fold excepted, which ran in no cell), on this binary and host, is **0.1 to 0.35 us per point statement (0.5 % to 1.8 %)**; as a bound the run supports only "no cell's median reaches 0.5 us and no cell's paired range shows a sustained cost near it": the worst single run in a cell is +0.54 to +1.29 us, in single runs where the control also moves by as much.

**What the sign tests add** (one-sided, unadjusted, four correlated cells; B and C against the mean of A and A2 per run):

| cell | A2 - A median [min, max] | A2 above A | B above mean(A, A2) | p | C above mean(A, A2) | p |
|---|---|---|---|---|---|---|
| select-pk literal | -0.13 us [-0.34, +0.70 us] | 5 of 17 | 16 of 17 | 0.0001 | 12 of 17 | 0.0717 |
| select-pk prepared | +0.04 us [-0.63, +0.69 us] | 8 of 15 | 14 of 15 | 0.0005 | 8 of 15 | 0.5000 |
| update-expr literal | -0.08 us [-0.41, +0.36 us] | 8 of 16 | 14 of 16 | 0.0021 | 13 of 16 | 0.0106 |
| update-expr prepared | -0.00 us [-0.93, +1.00 us] | 8 of 17 | 11 of 17 | 0.1662 | 8 of 17 | 0.6855 |

The control sits on both sides of zero here (A2 above A in 5 to 8 of 15-17 runs), unlike BO's close where it carried a one-sided offset, so the mean-of-controls estimate is close to B - A. B's sign is consistent against the control in three of the four cells (p 0.0001 to 0.002) and not in the update-prepared cell (p 0.17, the cell with the widest control spread). **So a cost of the size of +0.1 to +0.35 us is detectable by sign and not by magnitude**, which is the shape of a real but small cost.

**DF-Q6 (bit 2, the wait-statistics delta).** `B - C` is +0.27 / +0.23 us on the point `SELECT` (B above C in 14 of 17 and 12 of 15 runs) and +0.07 / +0.05 us on the point `UPDATE` (10 of 16 and 11 of 17). By its median this source is inside the 0.5 us bar by itself in every cell and is 0.2-0.3 us at most; it is not resolved from the control (`C - A` and `B - C` ranges both reach about +-1 us). The read shows the larger delta (two extra clock reads, a copy and a 20-event loop make the same fixed addition in both shapes; the write's own cost is larger and its run-to-run spread is wider, so the same addition is lost in it). **The spike supports "the wait delta fits inside the bar" at the median and does not support "it is free" or "it is the dominant part"**: `C - A`, which is everything except bit 2, is +0.08 to +0.14 us, a quarter to a half of the typical control gap.

## 3. Wall latency (client side) and per-arm CPU

Statement wall time per arm, median over runs of each run's percentile (kept cells only); `statements/s` is derived from the mean (`1e6 / mean us`, a serial single-connection driver). The server and driver CPU per statement overlap and are not a partition of the wall time. There is no latency effect to read: the arms' p0, p25, p50 and p90 agree to within 0.5 us, and p99 moves by more than that between arms in no consistent direction.

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | literal | A | 33,966 statements | 47.5 us | 49.2 us | 50.1 us | 57.3 us | 73.7 us | 18,803 /s | 20.46 us | 31.93 us |
| select-pk | literal | A2 | 33,966 statements | 47.5 us | 49.2 us | 50.1 us | 57.6 us | 74.1 us | 18,708 /s | 20.36 us | 32.01 us |
| select-pk | literal | B | 33,966 statements | 47.6 us | 49.4 us | 50.3 us | 57.7 us | 75.9 us | 18,588 /s | 20.87 us | 31.75 us |
| select-pk | literal | C | 33,966 statements | 47.4 us | 49.3 us | 50.2 us | 57.6 us | 76.2 us | 18,650 /s | 20.53 us | 32.08 us |
| select-pk | prepared | A | 29,970 statements | 44.0 us | 45.5 us | 46.3 us | 52.8 us | 67.6 us | 20,301 /s | 19.78 us | 28.64 us |
| select-pk | prepared | A2 | 29,970 statements | 43.9 us | 45.5 us | 46.3 us | 53.4 us | 70.1 us | 20,157 /s | 19.91 us | 28.61 us |
| select-pk | prepared | B | 29,970 statements | 44.3 us | 45.8 us | 46.6 us | 53.6 us | 73.6 us | 20,050 /s | 20.04 us | 28.72 us |
| select-pk | prepared | C | 29,970 statements | 44.3 us | 45.7 us | 46.4 us | 52.8 us | 69.9 us | 20,106 /s | 19.92 us | 28.53 us |
| update-expr | literal | A | 31,968 statements | 37.0 us | 42.3 us | 43.0 us | 50.6 us | 68.0 us | 21,628 /s | 22.05 us | 23.33 us |
| update-expr | literal | A2 | 31,968 statements | 37.0 us | 42.1 us | 42.9 us | 50.3 us | 69.0 us | 21,625 /s | 21.88 us | 23.33 us |
| update-expr | literal | B | 31,968 statements | 37.8 us | 42.4 us | 43.1 us | 50.6 us | 69.1 us | 21,350 /s | 22.28 us | 23.30 us |
| update-expr | literal | C | 31,968 statements | 37.4 us | 42.3 us | 43.0 us | 51.1 us | 70.1 us | 21,488 /s | 22.23 us | 23.44 us |
| update-expr | prepared | A | 33,966 statements | 36.8 us | 38.5 us | 39.2 us | 46.7 us | 66.7 us | 23,503 /s | 21.27 us | 20.29 us |
| update-expr | prepared | A2 | 33,966 statements | 37.2 us | 38.5 us | 39.2 us | 46.3 us | 64.9 us | 23,545 /s | 21.38 us | 20.21 us |
| update-expr | prepared | B | 33,966 statements | 36.5 us | 38.7 us | 39.4 us | 46.8 us | 66.0 us | 23,391 /s | 21.45 us | 20.30 us |
| update-expr | prepared | C | 33,966 statements | 36.8 us | 38.7 us | 39.3 us | 46.6 us | 65.0 us | 23,469 /s | 21.45 us | 20.26 us |

`ops` is the sum of timed statements over the kept runs (15 to 17 runs of 1,998). The latency percentiles here are per-statement client wall times and carry p0, p25, p50, p90 and p99 (the driver reports p90, not p95).

## 4. What this teaches, and its limits

- **The point statement's server CPU is 20-22 us at this commit on this host** (v3.3.0's close read 24-32 us on a different host and kernel; that is context and not a baseline, a different version on different hardware). The spike's extra paths add a fraction of a microsecond to it; the client-seen wall time (43-50 us p50) does not move.
- **Bit 2 is the part to look at first, but on the read only.** Its median price is as large as `B - A`'s on the point `SELECT` (+0.27 / +0.23 us against +0.18 / +0.35 us; medians of differences do not add) and a small share on the point `UPDATE`, which is the same fixed cost against a larger denominator of noise.
- **Resolution.** `A2 - A` reaches 0.41 to 1.00 us in one run of a cell and has a typical gap of 0.2-0.3 us; the run's resolution is about that, not better. This is the second measurement on this engine (BO's close was the first) that cannot place a 0.1-0.5 us cost better than 0.2-0.7 us, so a bar of 0.5 us is at the edge of what an interleaved single-connection A/B on this host can adjudicate; a result that must be exact needs more runs or a CPU-count (cycles or instructions) measurement, not more of this one.
- **Limits.** Mask 0 is the same binary: its residual branches, the larger statement-shape struct and the larger session object are present in A, so `B - A` under-prices a build without the spike by that residual, which this run does not measure. The spike prices work, not the final design's attribution: it uses thread-local adds and dispatcher members where the design carries counts on the transaction and the statement (DF-Q7), so a design whose attribution costs more than that is not priced here. One connection and one core: the part's latch is taken uncontended, and `cores = 8` was not run (section 1). The measured cells are two shapes; nothing here prices an `INSERT`, a `DELETE`, a range or a scan, the statements whose executor totals are larger.
- **Work-order items.** DF-Q10 (a)'s bar "all of DF adds no more server CPU a point statement than BO's recording did, 0.5 us at cores = 1" has its first data point here: the median is under it in every cell; it is not resolved from noise, and the `cores = 8` half has none. DF-Q6's question (is the wait-statistics source inside the bar) reads "yes at the median, not resolved" with +0.05 to +0.27 us.
