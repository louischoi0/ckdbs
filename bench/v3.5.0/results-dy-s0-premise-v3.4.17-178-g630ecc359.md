# DY-S0 premise: what a segment roll's latch hold is spent on, and what would be left under the latch if the sync and the segment's naming left it

**Thesis.** On `v3.4.17-178-g630ecc359`, in BQ-S1's cell, **DY-R6's 10 ms bar on the roll's hold is reachable in principle: with the roll's device sync, the gate wait, the new segment's header write, rename and directory sync all removed, what stays under the latch is a seal and a flush of at most 74 us over 122 measured rolls at the three rungs (R1 = R2: the body was a `hit` in every one of them), and no roll leaves more than 74 us.** The bar's other half, a co-located point read's p99 within 2x of its p99 alone, is not decided by this file: at `relaxed` the reader's schedule p99 is 1.3-1.7 *thousand* times its isolated p99 and 98 % of its slow samples fall inside a roll's window, but at `group` and `strict` the reader is already within the bar (0.8x), so the ratio only has something to remove at `relaxed`. Three things stand between "reachable in principle" and "reachable", and this premise check measured none of them as solved (section 4).

What the hold is, per rung (pooled over the measured runs; n = rolls in the 30 s windows):

| rung | runs | rolls | hold p50 | hold p99 | hold max | R1 p99 | R2 p99 | R1 max | rolls with hold > 10 ms | rolls with R1 > 10 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| `relaxed` | 2 | 60 | 574.42 ms | 3,001.34 ms | 3,001.34 ms | 74 us | 74 us | 74 us | 46 of 69 in the log (all rolls incl. warm-up) | 0 of 60 |
| `group` | 2 | 43 | 1.65 ms | 15.34 ms | 15.34 ms | 27 us | 27 us | 27 us | 3 of 50 in the log | 0 of 43 |
| `strict` | 1 | 19 | 1.39 ms | 15.79 ms | 15.79 ms | 26 us | 26 us | 26 us | 1 of 24 in the log | 0 of 19 |

(p99 of 60, 43 and 19 samples is the maximum: it is stated because the bar is written at p99, and none of these p99s is a tail estimate. The "> 10 ms" column counts every roll in the server log, warm-up and setup rolls included; the percentile columns use the in-window rolls only; the generated tables are section 9.)

Findings, each in its section:

- **(a) At `relaxed`, 95.2 % of the hold is one step, the wait on the writer thread's `fdatasync` already in flight (`sync_gate_us`); the roll's own sync is 2.8 %, the new segment's create 2.0 %, seal and flush together 0.0 %.** Every one of the 46 rolls over 10 ms in the log spends at least half its hold in the gate. Removing the gate removes the stall; no step that would stay under the latch is a candidate (section 2).
- **(b) At `group` and `strict` the hold is the roll's own sync (69-74 %) and its create (21-29 %), a median 1.4-1.7 ms; the 15 ms p99 is one outlier roll per run in a device call** (tail sync 11-13 ms, or the header's sync 12.5 ms). Both leave the latch under DY's mechanism.
- **(c) The body was never late.** `body=hit` in 122 of 122 rolls at all three rungs; `waited` 0, `inline` 0, `body_wait_us` and `body_us` both 0. R1's two body terms are therefore **unexercised, not shown small**: a roll that found its body unready would put the preparer's build (DG measured 35-50 ms of zero writes and an `fsync` of 287-503 ms) under the latch, over the bar by itself. This cell, at 40-63 MiB/s of log, is one in which the preparer keeps ahead of a roll every 0.9-1.6 s.
- **(d) Whether one thread can keep up with the syncs if they leave the latch is not established at `relaxed`.** The syncs that finished while a roll waited took a median 0.69 s and a p90 1.95 s against a nominal interval of 1.03 s between rolls; 14 of 58 consecutive rolls had (gate + own sync) longer than that nominal interval. At `group` and `strict` the question does not arise (0 of 41 and 0 of 18; sync a median 0.9-1.1 ms).
- **(e) The log is not recycled at `relaxed` on this tree**: `wal_first_segment` 0 and 0 segments removed in both runs, up to 36 live segments in a roll's sync. At `group` and `strict` the first segment moved to 10 (10 removed) and the sync covered up to 25.

**Baseline.** This engine's own earlier numbers for the shape are BQ-S1's at `v2.7.0-909-g4cf30270` (`bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md`: `relaxed` hold p50 331 ms, p99 788 ms, max 1.21 s; `group` p50 6.1 ms, p99 20.6 ms) and CZ's close at `v3.4.12-113-g74861601` (`bench/v3.4.12/results-cz-close-v3.4.12-113-g74861601.md`: `relaxed` median about 250 ms). **They are not like-for-like claims**: BQ's host was an 8-vCPU Azure VM on ext4, this one is a 24-CPU Intel P/E workstation on xfs/LVM/NVMe; the engine is 1,000-odd commits later; BQ and CZ ran different cell lengths and pinning. Read as same-direction evidence only: the `relaxed` hold is hundreds of milliseconds at all three points (574 ms here, 331 ms BQ, ~250 ms CZ), and in each of them it is the gate. This file is the baseline DY-S1..S3 are read against, **at this host**. PostgreSQL is not measured for this shape (no server installed on this host; rule 4: a floor, not a hole).

## 0. Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-27, **10:15-10:21 UTC** (nine runs, `hoststamps.log`; measurement wall time **349 s** from first launch to last run's end, 5 min 49 s). Build 10:13:58-10:14:28 UTC (**28 s** wall, configure + 172 ninja steps) |
| Worktree / branch | `dy-segment-roll-off-the-latch`, branch `worktree-dy-segment-roll-off-the-latch` |
| Commit | `630ecc359`, `git describe --tags` = **`v3.4.17-178-g630ecc359`** |
| Tree | Clean at the start (`git status` empty). No engine source was touched. New files under `bench/v3.5.0/` only, uncommitted |
| Binary | `build-release/kds_server` built in this worktree, `-DCMAKE_BUILD_TYPE=Release` (Ninja), mtime **2026-11-27 05:14:28.44 -0500** (= 10:14:28 UTC), **after** HEAD's commit time (2026-11-26 20:12:53 -0500), so the binary is HEAD's. Measured a **copy** at `/home/ukjin/bench-runs/dy-s0/bin/kds_server`, **sha256 `7d27de1c82964aa36c0d42215af5f033dabac6fa4647e5b30f6bc581b5d2322f`**; the driver hashes it per run and every run's JSON carries `bin_sha256` |
| Device | Data file and WAL directory on `/dev/mapper/fedora-root`, **xfs** (`rw,relatime,seclabel,inode64,logbufs=8,logbsize=32k,sunit=8,swidth=1024`) on LVM over NVMe, 406 GB free; checked with `df -T`. **Not tmpfs.** `/tmp` is tmpfs on this host, so the run directories are under `/home/ukjin/bench-runs/dy-s0/`, not the scratchpad the task named; the scratchpad holds only scripts. No dedicated WAL device |
| Host | Intel Core Ultra 7 270K Plus, 24 CPUs (P-cores 0-7, E-cores 8-23), `intel_pstate` powersave, Linux 7.3.4. **Server pinned: reactors on CPUs 0,1,2,3 (`reactor_cpus`), the driver and so the server's other threads (WAL writer, data-sync, page reader) under `taskset -c 0-7`; clients on CPUs 8-23.** Per-run load 0.99-3.09 at start (the 1-minute figure still carried the build's tail for the first runs); `pgrep -f "cc1plus|cmake --build|ctest|ninja"` per run in `hoststamps.log` found only this session's own shell wrapper (its argv names the pattern). One unrelated idle `kds_server` of another project (`firestrike`, pid 484305) was running throughout, untouched. No ninja or ctest from another session |
| Server configuration | `cores = 4`, `durability` = the cell's, `buffer_pool_frames = 131072`, `log_level = info`, `reactor_cpus = 0,1,2,3`; every other key default (64 MiB segments, 1 MiB ring, checkpoint cadence 5,000 ms, session balancer live). One fresh server and data file per run |
| Time budget | Measurement inside 20 minutes (5 min 49 s run + about 10 min planning and analysis); no gate hold (host was quiet) |
| Suite | **Not executed**: no engine change in this stage |

## 1. The cell, what was reused and what changed

Reused from BQ-S1's C1 (`bench/v3.0.0/archive/bq-s1-segment-roll-v2.7.0-909-g4cf30270/bq_s1_matrix.py`, driver `tools/bl_mixed.py`, unmodified at this commit): `--arm mixed --readers 6 --rate 400` (a 1,024-row bulk `INSERT` closed loop, a 10,000-row unindexed walker, 6 open-loop pk point readers at 400/s in total), `--cores 4 --seconds 30 --warmup 2 --runs 1 --frames 131072 --log-level info --ctl-core 0`, `--placement spread`, one run per fresh server. The isolated arm is `--arm isolated` with the same readers. Command per run is in `run.sh` (archived); the one the driver ran, from `console.log`.

Changed, and why it matters:

| item | BQ-S1 | this run |
|---|---|---|
| host | 8 vCPU Azure VM, ext4, servers on CPUs 0,2,4,6 and clients on their SMT siblings | 24-CPU Intel P/E workstation, xfs; server reactors on CPUs 0-3 (P-cores), other server threads on 0-7, clients on 8-23 |
| placement | C1 at `segregated-no0`, `batch0` and `spread`, 3 runs | **`spread` only** (cut: the other two placements) |
| runs per rung | 3 | **`relaxed` 2 mixed + 2 isolated, `group` 2 mixed + 1 isolated, `strict` 1 mixed + 1 isolated** (cut to the 20-minute budget) |
| `max-load` | 1.5 default | 4 (the first runs started while the 1-minute load still held the build's tail; a rule-4 stamp is per run) |

**The co-located reader.** BQ-S1 names no single "co-located point reader" cell; this file reads it as C1's 6 point readers at `spread`, where at placement the readers sit on cores 0,1,2,3,0,1, the walker on core 2 and the bulk inserter on core 3 (core 0 is the log's owner, and is where the stream-latch wait lands). **The load placer moved sessions during the window** (`moves_in_window` 6-18), so a reader ended on a different core from its start in most runs and no per-core reading of the readers is claimed. One reader (index 3) was placed on the inserter's core and two (0 and 4) on core 0, the log owner's.

Order of runs: `relaxed-mixed-r1`, `relaxed-iso-r1`, `relaxed-mixed-r2`, `group-mixed-r1`, `group-iso-r1`, `strict-mixed-r1`, `strict-iso-r1`, `relaxed-iso-r2`, `group-mixed-r2`. Watchdog: a beat file written per run start/end and polled by this session about once a minute; no run stalled or was orphaned, every run's rc was 0.

## 2. Where the hold goes (per step, per rung)

The step names are `stream.cpp`'s, as in BQ-S1 section 0: **a** seal, **b** flush, **c1** the sync gate (a wait on the writer thread's sync in flight), **c2** the roll's own sync (tail and non-tail segments), **d** the segment body (`hit` = ready), **e** the new segment's header write and sync, **f** rename, **g** directory `fsync`. Full per-step distributions with p0..max are in section 9; the shares are:

| step | `relaxed` share of hold (60 rolls) | `group` (43) | `strict` (19) |
|---|---|---|---|
| seal | 0.0 % | 0.5 % | 0.7 % |
| flush | 0.0 % | 0.0 % | 0.0 % |
| sync gate (wait on the writer's sync) | **95.2 %** | 1.7 % | 3.7 % |
| roll's own sync | 2.8 % (tail 0.1 %, rest 2.7 %) | **69.0 %** (tail 34.3 %, rest 34.6 %) | **74.5 %** (tail 41.1 %, rest 33.4 %) |
| create (body wait, inline body, header, rename, dirsync) | 2.0 % (header 1.2 %, dirsync 0.8 %, body 0.0 %) | 28.8 % (header 17.0 %, dirsync 10.9 %) | 21.1 % (header 5.9 %, dirsync 14.0 %) |
| unaccounted | 0.0 % | 0.1 % | 0.1 % |
| body outcome | hit 60, waited 0, inline 0 | hit 43 / 0 / 0 | hit 19 / 0 / 0 |

The hold at `relaxed` is bimodal: a quarter of the in-window rolls hold under 4 ms (p25 3.94 ms) and the median is 574 ms; the cheap rolls are consistent with finding no sync in flight (the log line does not record that directly). The long ones wait out the writer thread's `fdatasync`. The `SHOW META` counters agree with the log lines: `wal_rolls` delta 30 and 30, `wal_roll_hold_us` 21.85 s and 21.30 s of the 30.7 s windows, of which `wal_roll_sync_gate_us` is 20.76 s and 20.29 s. Core 0's stream-latch wait was 21.7 s and 21.2 s of those windows (`contention_wal_stream_wait_us`): **70 % of the window, on the core that holds the log**, against 0.0-0.8 ms on cores 1 and 3 (`load_c1_waited_wal_stream_us` 0, `load_c3_waited_wal_stream_us` 769 us in r1). At `group` and `strict` that wait is 0.7-12 ms per window.

## 3. The residual: what stays under the latch

R1 = `seal_us + flush_us + body_wait_us + body_us` (what stays if the sync, the header, the rename and the directory sync leave the latch and a body that is not ready is still waited for or built under it). R2 = `seal_us + flush_us` (the floor: the body always ready). Computed per roll, then as a distribution; the bar is 10,000 us.

| rung | rolls | R1 p50 | R1 p90 | R1 p99 | R1 max | R2 p50 | R2 p90 | R2 p99 | R2 max | rolls over 10 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| `relaxed` | 60 | 19 us | 61 us | 74 us | 74 us | 19 us | 61 us | 74 us | 74 us | 0 |
| `group` | 43 | 15 us | 25 us | 27 us | 27 us | 15 us | 25 us | 27 us | 27 us | 0 |
| `strict` | 19 | 14 us | 24 us | 26 us | 26 us | 14 us | 24 us | 26 us | 26 us | 0 |

**No step puts R1 or R2 over the bar in any of the 122 rolls; the largest single residual is 74 us.** R1 equals R2 in every roll because every body was a hit. The residual is the seal's own time (record write and end-LSN fix) and an empty-ring flush: no device call is in it. **What this does not say**: (i) the residual is the old path's seal and flush, not DY's new "point the append at the next segment and record the unsynced one" step, whose cost nobody has measured (it must be a store and a list append under the latch; the proof owns that claim); (ii) the body term is unexercised (finding (c)); (iii) the hold with only the gate removed and the roll's own sync, header, rename and dirsync kept is also reported by the logs: `hold - sync_gate_us` has p50 1.9 ms at `relaxed` and a p99/max of 647 ms (one roll whose own non-tail sync took 646 ms, `sync_rest` max 646.34 ms, and one whose header took 378 ms), so removing the gate alone does not meet the bar, and the roll's own sync must leave too, as DY's step 2 has it.

## 4. Could the syncs leave the latch and one thread keep up

If the roll syncs nothing, the left segment's sync is the writer's. The per-roll proxy is (gate + own sync) against the interval to the next roll, and a second, interval-free proxy is against the nominal interval, one 64 MiB segment at the run's mean log rate. **The measured intervals are stretched by the holds themselves** (the bulk writer is blocked under every one), so the first reading flatters, and the nominal one is the honest comparison.

| rung | log MiB/s (run mean) | nominal interval | gate + own sync p50 / p90 / p99 / max | waited writer sync (tail+rest, as logged) p50 / p90 / max | rolls with (gate + own sync) > nominal interval | rolls with it > the next measured interval |
|---|---|---|---|---|---|---|
| `relaxed` | 61.3, 62.7 | 1,032 ms | 571 ms / 1,868 ms / 2,739 ms / 2,739 ms | 689 ms / 1,949 ms / 2,427 ms | **14 of 58** | 0 of 58 |
| `group` | 42.8, 48.0 | 1,414 ms | 1.11 ms / 2.42 ms / 14.1 ms / 14.1 ms | 0 / 0 / 1.27 ms | 0 of 41 | 0 of 41 |
| `strict` | 39.9 | 1,604 ms | 0.90 ms / 1.97 ms / 14.5 ms / 14.5 ms | 0 / 0.55 ms / 1.17 ms | 0 of 18 | 0 of 18 |

At `group` and `strict` a sync is two to three orders of magnitude shorter than an interval; one thread keeps up with room. At `relaxed` the syncs that finished while a roll waited are a median 0.69 s and a p90 1.95 s, the same order as the 1.03 s interval, and **14 of 58 rolls would find, by the nominal reading, the previous sync still unfinished when the next roll arrives** (0 of 58 by the stretched one). It is not breadth that makes them long: the non-tail segments of the roll's own sync cost a median 1.25 ms over a median of 20 live segments (about 60 us a segment, the figure `known-gaps.md:456` and BQ give), so the 0.7-2.4 s syncs are the flush of the log written since the last sync, on a device that is also taking the checkpoint's writeback. Narrowing a sync to the segments written since the last (DY-Q2) would not shorten them, as BQ-Q6 (a) found at `v2.7.0-909-g4cf30270`. **Consequence for DY-R1 at `relaxed`**: if the roll leaves the latch, the sealed segment's sync is the writer's and takes a median 0.7 s; the durable point may not pass that segment until it returns, and a second roll arriving in the meantime finds one segment unsynced already. How many segments may be unsynced at once, and what a reader of the durable point sees meanwhile (the checkpoint's log park, the recycler's fold), is the proof's, not this measurement's. This premise check cannot say whether the queue would stay at depth one; it says the depth is not zero-by-margin at `relaxed`, and is by a wide margin at `group` and `strict`.

## 5. The co-located point reader

| rung | arm | run | read_service p50 / p99 / max (us) | read_from_schedule p0 / p25 / p50 / p95 / p99 / p99.9 / max (us) | n |
|---|---|---|---|---|---|
| `relaxed` | with bulk | r1 | 256 / 634 / 2,298,025 | 63 / 366 / 456 / 1,281,390 / 1,892,781 / 2,150,126 / 2,298,172 | 11,999 |
| `relaxed` | with bulk | r2 | 264 / 638 / 2,989,946 | 78 / 369 / 452 / 702,080 / 1,477,422 / 2,613,827 / 2,990,067 | 11,538 |
| `relaxed` | alone | iso r1 | 680 / 873 / 3,350 | 151 / 790 / 898 / 1,064 / 1,118 / 1,742 / 3,447 | 12,006 |
| `relaxed` | alone | iso r2 | 683 / 882 / 2,413 | 150 / 806 / 897 / 1,069 / 1,129 / 1,746 / 4,309 | 12,006 |
| `group` | with bulk | r1 | 282 / 678 / 2,902 | 69 / 353 / 429 / 680 / 893 / 1,826 / 4,055 | 12,006 |
| `group` | with bulk | r2 | 278 / 688 / 7,787 | 97 / 344 / 429 / 683 / 909 / 1,603 / 7,917 | 12,006 |
| `group` | alone | iso r1 | 681 / 865 / 2,736 | 146 / 767 / 901 / 1,064 / 1,123 / 2,132 / 3,026 | 12,006 |
| `strict` | with bulk | r1 | 281 / 632 / 4,753 | 44 / 349 / 431 / 648 / 876 / 1,484 / 4,941 | 12,006 |
| `strict` | alone | iso r1 | 697 / 863 / 2,809 | 129 / 805 / 916 / 1,069 / 1,120 / 1,752 / 3,006 | 12,006 |

**The 2x bar's ratio, at p99 of the schedule latency (the open-loop latency that charges a stalled session for the wait it could not start in)**:

| rung | with bulk p99 | alone p99 (two runs at `relaxed`, one elsewhere) | ratio | inside 2x |
|---|---|---|---|---|
| `relaxed` | 1,892,781 us / 1,477,422 us | 1,118 us / 1,129 us | **1,676x and 1,308x** (against iso r2; 1,694x and 1,322x against iso r1) | no |
| `group` | 893 us / 909 us | 1,123 us | 0.8x / 0.8x | yes |
| `strict` | 876 us | 1,120 us | 0.8x | yes |

**The noise floor** is the two isolated `relaxed` runs against each other: p99 from schedule 1,118 vs 1,129 us (1.0 %), and the three isolated runs across the rungs 1,118-1,129 us. Every delta quoted from the denominators is far above it.

**A ratio below 1.0 is not a finding about the roll.** The reader with a bulk writer beside it is *faster* (p50 256-282 us) than the reader alone (p50 680-697 us): on this hybrid, powersave host an idle server's cores sleep between the readers' 400 statements a second, and a busy one does not. The isolated arm therefore overstates the "without a roll" latency; the bar's denominator should be a mixed run with the roll's effect subtracted rather than an isolated run (a `group` mixed run is the closest this file has), and at `group` the mixed p99 is 893-909 us. Taking that as the denominator, `relaxed` is still over 1,600x. Every number in this paragraph is `v3.4.17-178-g630ecc359`'s.

**Slow samples against rolls.** A slow sample is a reader statement more than 1,000 us late from its schedule; the driver records its due instant, and a sample "overlaps" a roll if `[due, due + latency]` intersects the roll's `[start_ns, end_ns]` (both CLOCK_MONOTONIC):

| run | slow samples | overlapping a roll | overlapping a roll, with 50 ms of padding | slow > 10 ms | of them overlapping a roll | longest slow sample outside any roll |
|---|---|---|---|---|---|---|
| `relaxed` r1 | 2,491 | 2,464 (98.9 %) | 2,476 | 2,438 | 2,438 | 7.25 ms |
| `relaxed` r2 | 1,814 | 1,791 (98.7 %) | 1,805 | 1,780 | 1,777 | 14.66 ms |
| `group` r1 | 53 | 0 | 2 | 0 | n/a | 4.06 ms |
| `group` r2 | 46 | 3 | 3 | 0 | n/a | 2.90 ms |
| `strict` r1 | 38 | 0 | 1 | 0 | n/a | 4.94 ms |

At `relaxed` essentially every reader sample slower than 10 ms (2,438 of 2,438 in r1; 1,777 of 1,780 in r2) is inside a roll's window: **the reader's tail is the roll's hold, and nothing else in this cell reaches 10 ms but 3 samples in r2 (longest 14.7 ms) and none in r1**. At `group` and `strict` no sample reaches 10 ms (maximum 7.9 ms) and the 1-8 ms residue is not a roll's (0-3 of 38-53 overlap one): it is the baseline the 2x bar's denominator contains.

## 6. Log retention and the sync's breadth

| rung | run | `wal_first_segment` at the window's end | segments removed in the window | `sync_segs` maximum in the window (live segments) |
|---|---|---|---|---|
| `relaxed` | r1 | 0 | 0 | 36 |
| `relaxed` | r2 | 0 | 0 | 33 |
| `group` | r1 | 10 | 10 | 18 |
| `group` | r2 | 10 | 10 | 25 |
| `strict` | r1 | 10 | 10 | 19 |

(`sync_segs` pooled per rung, from the per-roll lines: `relaxed` p0/p50/p99/max 4/20/36/36; `group` 3/14/25/25; `strict` 6/14/19/19; a roll's `sync_segs` is every live segment, the sync's contract.) **BQ finding (b) reads the same on this tree**: at `relaxed` the log is not recycled during the load (0 removed; first segment 0), so each sync covers every segment the run has written, 33-36 by its end. At `group` and `strict` the recycler removed 10 per run (BQ saw 20-22 on its longer-running host).

## 7. Not measured

- **DG's stall (commit or read latency while the preparer builds a body outside any roll's hold): not measured.** No existing instrument separates a preparer's build from a roll in this driver's output: `body=hit` says the body was ready, not when it was built or what it cost the commit path beside it. The reader's slow samples at `group`/`strict` (0-3 of 38-53 overlap a roll) are consistent with no large stall at 40-48 MiB/s but cannot be attributed to a build.
- The `relaxed` rung with the roll's sync actually absent: nothing in this stage changes the engine, so every "would be" in sections 3 and 4 is arithmetic on logged steps, never a run.
- Placements other than `spread`; C2's writers; C3; more than 2 runs at `relaxed` and 1-2 at the other rungs (cut for the budget). The `group` and `strict` p99s are the maxima of 43 and 19 rolls.
- The hold with a checkpoint pinned out of the window, or the device's own sync rate (no `iostat` was taken): whether the 0.7-2.4 s syncs are the device or the checkpoint's concurrent writeback is not separable here.
- The row-set sweep (200 / 1K / 10K): does not apply, as in BQ-S1 section 0; a roll's cost is a function of log bytes and the device, not of a relation's cardinality (`bl_point` and `bl_walk` hold 10,000 rows each; the inserter's relation grows by 2.9-4.7 million rows a run).
- **Overhead of the instruments**: not measured. Suite: not executed (no engine change).
- The `critics-developer` review: not requested for this stage by the task.

## 8. What the run teaches

1. **The `relaxed` hold is a queue behind the writer's sync, not the roll's work, and the premise holds that moving the roll's own sync is not the whole of DY's job: the *gate* is the stall, and it exists only because the roll takes `sync_mutex_`.** A roll that syncs nothing takes no gate; the roll's own sync (2.8 %) is not where the time is. DY's step 1 ("no device sync is made under the latch") is what removes the 95 %, and only if the roll also stops waiting on anything the writer holds.
2. **The bar's first half is reachable with margin on this tree (74 us max against 10,000 us), and the margin is two orders of magnitude, so a loose proof will not be caught by this half.** The second half (reader p99 within 2x) is `relaxed`'s alone, and 98 % of the reader's slow samples there are inside a roll.
3. **The weakest premise is not the latch, it is what the proof must hold after it:** at `relaxed` the sync the roll would hand off takes 0.7-2.4 s at a nominal 1.0 s between rolls, the log is never recycled, and DY-R1 says the durable point may not pass an unsynced sealed segment. Depth-one is not shown. At `group` and `strict` DY has more than enough room, but there the bar's hold half is already met at p50 (1.4-1.7 ms) and missed at p99 (15-16 ms) by one device outlier per run, which the same step 1 removes.
4. **The body term is the premise's blind spot:** R1 = R2 here only because the preparer was never late at 40-63 MiB/s. A cell that shortened the interval (a faster log, `--insert-rate` above this driver's closed loop, a slower device) is the one that would exercise `body_wait` and `body_us`, and DG already measured what a late body costs.
5. **Open decision acquiring its first data point: DY-Q4.** The bar at `relaxed` is the only place the 2x half bites; at `group` and `strict` the reader is at 0.8x with or without a roll. Reading the bar at `group` and `strict` only (DY-Q4 (b)) would make DY's reader bar vacuous. It is the operator's to decide.

## 9. The generated tables

The tables below are the analysis script's output verbatim (`analyze.py`, archived); `read_service` is from the send, `read_from_schedule` from the open-loop schedule; all times are the units stated in each heading. Their headings are A.relaxed, A.group, A.strict.


## A.relaxed (generated)

#### relaxed: runs

| run | rolls in window | window s | interval between rolls p50 (ms) | log MiB/s | rows/s | wal_first_segment | segments removed | sync_segs max | reader p99 from-send (us) | reader p99 from-schedule (us) | slow(>1ms) samples |
|---|---|---|---|---|---|---|---|---|---|---|---|
| relaxed-mixed-r1 | 30 (counter 30.0) | 30.7 | 882 | 61.3 | 140437 | 0.0 | 0.0 | 36 | 634 | 1892781 | 2491 |
| relaxed-mixed-r2 | 30 (counter 30.0) | 30.6 | 883 | 62.7 | 155733 | 0.0 | 0.0 | 33 | 638 | 1477422 | 1814 |

#### relaxed: roll hold (ms), pooled over 2 run(s), 60 rolls

| series | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|
| hold | 0.87 | 3.94 | 574.42 | 915.87 | 1868.45 | 3001.34 | 3001.34 |
| hold, relaxed-mixed-r1 (n=30) | 1.20 | 3.75 | 574.42 | 1328.79 | 1889.42 | 2305.89 | 2305.89 |
| hold, relaxed-mixed-r2 (n=30) | 0.87 | 144.48 | 573.04 | 802.66 | 1529.39 | 3001.34 | 3001.34 |

#### relaxed: the hold per step (ms; share = sum of step / sum of hold)

| step | share of hold | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| seal | 0.0 % | 0.00 | 0.01 | 0.02 | 0.04 | 0.06 | 0.07 | 0.07 |
| flush | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| sync_gate (wait on writer-thread sync) | 95.2 % | 0.00 | 2.21 | 569.93 | 913.81 | 1852.85 | 2740.01 | 2740.01 |
| sync (roll's own, total) | 2.8 % | 0.54 | 0.96 | 1.48 | 2.26 | 15.21 | 646.40 | 646.40 |
|   sync tail | 0.1 % | 0.05 | 0.05 | 0.05 | 0.24 | 1.02 | 13.21 | 13.21 |
|   sync rest (non-tail segs) | 2.7 % | 0.29 | 0.88 | 1.25 | 1.80 | 14.97 | 646.34 | 646.34 |
| create (total) | 2.0 % | 0.30 | 0.38 | 0.45 | 0.51 | 10.19 | 378.30 | 378.30 |
|   body_wait | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   body inline | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   header write+sync | 1.2 % | 0.11 | 0.12 | 0.13 | 0.16 | 0.21 | 377.79 | 377.79 |
|   rename | 0.0 % | 0.01 | 0.02 | 0.02 | 0.02 | 0.04 | 0.12 | 0.12 |
|   dirsync | 0.8 % | 0.15 | 0.23 | 0.28 | 0.33 | 0.44 | 305.87 | 305.87 |
| unaccounted (hold - listed) | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.01 | 0.01 | 0.01 |

Body outcome: hit 60, waited 0, inline 0 (of 60).
Segments covered by the roll's own sync (sync_segs): p0/p50/p99/max = 4/20/36/36; waited_syncs>0 in 46 of 60 rolls.

#### relaxed: the residual under the latch if sync, header, rename, dirsync leave it (us; bar 10,000 us)

| residual | n | p50 | p90 | p99 | max | rolls > 10 ms |
|---|---|---|---|---|---|---|
| R1 = seal + flush + body_wait + body | 60 | 19 | 61 | 74 | 74 | 0 |
| R2 = seal + flush | 60 | 19 | 61 | 74 | 74 | 0 |

#### relaxed: could one thread keep up with the syncs if they left the latch

| series | p50 | p90 | p99 | max |
|---|---|---|---|---|
| gate + own sync (us) | 571036 | 1867982 | 2739008 | 2739008 |
| waited writer-thread sync, tail+rest (us) | 689040 | 1949244 | 2426632 | 2426632 |
| interval to next roll (us) | 882470.012 | 2180411.845 | 3052939.06 | 3052939.06 |

Nominal interval (one 64 MiB segment at the run's mean log byte rate) = 1032 ms; this is the stalled-free-rate-free reading, because the measured intervals above are stretched by the holds themselves. (gate + own sync) exceeds it in **14** of 58 rolls; the waited writer-thread sync exceeds it in **14**.

Consecutive roll pairs: 58. (gate + own sync) of roll k exceeds the interval to roll k+1 in **0** pairs; the waited writer-thread sync of roll k exceeds that interval in **0**.

#### relaxed: the point readers (us)

| arm | metric | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| with bulk writer, relaxed-mixed-r1 | read_service | 11999 | 28 | 168 | 256 | 442 | 634 | 1522544 | 2298025 |
| with bulk writer, relaxed-mixed-r1 | read_from_schedule | 11999 | 63 | 366 | 456 | 1281390 | 1892781 | 2150126 | 2298172 |
| with bulk writer, relaxed-mixed-r2 | read_service | 11538 | 29 | 195 | 264 | 453 | 638 | 1100418 | 2989946 |
| with bulk writer, relaxed-mixed-r2 | read_from_schedule | 11538 | 78 | 369 | 452 | 702080 | 1477422 | 2613827 | 2990067 |
| alone (isolated), relaxed-iso-r1 | read_service | 12006 | 89 | 580 | 680 | 824 | 873 | 1486 | 3350 |
| alone (isolated), relaxed-iso-r1 | read_from_schedule | 12006 | 151 | 790 | 898 | 1064 | 1118 | 1742 | 3447 |
| alone (isolated), relaxed-iso-r2 | read_service | 12006 | 91 | 595 | 683 | 825 | 882 | 1157 | 2413 |
| alone (isolated), relaxed-iso-r2 | read_from_schedule | 12006 | 150 | 806 | 897 | 1069 | 1129 | 1746 | 4309 |

p99 ratio read_service, relaxed-mixed-r1 / relaxed-iso-r1 = 0.7x (634 / 873 us).

p99 ratio read_service, relaxed-mixed-r1 / relaxed-iso-r2 = 0.7x (634 / 882 us).

p99 ratio read_from_schedule, relaxed-mixed-r1 / relaxed-iso-r1 = 1693.8x (1892781 / 1118 us).

p99 ratio read_from_schedule, relaxed-mixed-r1 / relaxed-iso-r2 = 1676.2x (1892781 / 1129 us).

p99 ratio read_service, relaxed-mixed-r2 / relaxed-iso-r1 = 0.7x (638 / 873 us).

p99 ratio read_service, relaxed-mixed-r2 / relaxed-iso-r2 = 0.7x (638 / 882 us).

p99 ratio read_from_schedule, relaxed-mixed-r2 / relaxed-iso-r1 = 1322.1x (1477422 / 1118 us).

p99 ratio read_from_schedule, relaxed-mixed-r2 / relaxed-iso-r2 = 1308.4x (1477422 / 1129 us).

#### relaxed: slow reader samples (> 1000 us from schedule) against roll windows

| run | slow samples | overlapping a roll [start,end] | overlapping a roll + 50 ms | > 10 ms and overlapping | > 10 ms total | slow outside any roll window (us, max) |
|---|---|---|---|---|---|---|
| relaxed-mixed-r1 | 2491 | 2464 | 2476 | 2438 | 2438 | 7250 |
| relaxed-mixed-r2 | 1814 | 1791 | 1805 | 1777 | 1780 | 14659 |

#### relaxed: SHOW META wal_roll_* deltas (m1 - m0)

| run | wal_rolls | hold_us | gate_us | sync_us | create_us | body hit/waited/inline | c0 stream-latch wait (us) | c0 roll_blocked_wait_us | wal_syncs |
|---|---|---|---|---|---|---|---|---|---|
| relaxed-mixed-r1 | 30.0 | 21849727.0 | 20764962.0 | 242710.0 | 841073.0 | 30.0/0.0/0.0 | 21733038.0 | 21729746.0 | 481.0 |
| relaxed-mixed-r2 | 30.0 | 21297495.0 | 20289710.0 | 982443.0 | 24553.0 | 30.0/0.0/0.0 | 21168168.0 | 21165438.0 | 471.0 |

## A.group (generated)

#### group: runs

| run | rolls in window | window s | interval between rolls p50 (ms) | log MiB/s | rows/s | wal_first_segment | segments removed | sync_segs max | reader p99 from-send (us) | reader p99 from-schedule (us) | slow(>1ms) samples |
|---|---|---|---|---|---|---|---|---|---|---|---|
| group-mixed-r1 | 20 (counter 20.0) | 30.1 | 1261 | 42.8 | 96324 | 10.0 | 10.0 | 18 | 678 | 893 | 53 |
| group-mixed-r2 | 23 (counter 23.0) | 30.7 | 1184 | 48.0 | 114203 | 10.0 | 10.0 | 25 | 688 | 909 | 46 |

#### group: roll hold (ms), pooled over 2 run(s), 43 rolls

| series | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|
| hold | 0.75 | 1.35 | 1.65 | 2.38 | 4.20 | 15.34 | 15.34 |
| hold, group-mixed-r1 (n=20) | 0.75 | 1.01 | 1.55 | 1.76 | 3.10 | 12.74 | 12.74 |
| hold, group-mixed-r2 (n=23) | 1.08 | 1.45 | 1.70 | 2.59 | 4.20 | 15.34 | 15.34 |

#### group: the hold per step (ms; share = sum of step / sum of hold)

| step | share of hold | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| seal | 0.5 % | 0.00 | 0.01 | 0.01 | 0.02 | 0.03 | 0.03 | 0.03 |
| flush | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| sync_gate (wait on writer-thread sync) | 1.7 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 1.00 | 1.00 |
| sync (roll's own, total) | 69.0 % | 0.40 | 0.90 | 1.11 | 1.42 | 2.17 | 14.13 | 14.13 |
|   sync tail | 34.3 % | 0.05 | 0.15 | 0.19 | 0.20 | 0.30 | 12.90 | 12.90 |
|   sync rest (non-tail segs) | 34.6 % | 0.15 | 0.66 | 0.95 | 1.23 | 1.64 | 1.94 | 1.94 |
| create (total) | 28.8 % | 0.29 | 0.39 | 0.44 | 0.49 | 0.55 | 13.10 | 13.10 |
|   body_wait | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   body inline | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   header write+sync | 17.0 % | 0.11 | 0.12 | 0.12 | 0.14 | 0.16 | 12.54 | 12.54 |
|   rename | 0.7 % | 0.01 | 0.02 | 0.02 | 0.02 | 0.02 | 0.05 | 0.05 |
|   dirsync | 10.9 % | 0.16 | 0.24 | 0.28 | 0.32 | 0.36 | 0.90 | 0.90 |
| unaccounted (hold - listed) | 0.1 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |

Body outcome: hit 43, waited 0, inline 0 (of 43).
Segments covered by the roll's own sync (sync_segs): p0/p50/p99/max = 3/14/25/25; waited_syncs>0 in 2 of 43 rolls.

#### group: the residual under the latch if sync, header, rename, dirsync leave it (us; bar 10,000 us)

| residual | n | p50 | p90 | p99 | max | rolls > 10 ms |
|---|---|---|---|---|---|---|
| R1 = seal + flush + body_wait + body | 43 | 15 | 25 | 27 | 27 | 0 |
| R2 = seal + flush | 43 | 15 | 25 | 27 | 27 | 0 |

#### group: could one thread keep up with the syncs if they left the latch

| series | p50 | p90 | p99 | max |
|---|---|---|---|---|
| gate + own sync (us) | 1110 | 2423 | 14132 | 14132 |
| waited writer-thread sync, tail+rest (us) | 0 | 0 | 1274 | 1274 |
| interval to next roll (us) | 1240966.547 | 1994747.524 | 3690532.997 | 3690532.997 |

Nominal interval (one 64 MiB segment at the run's mean log byte rate) = 1414 ms; this is the stalled-free-rate-free reading, because the measured intervals above are stretched by the holds themselves. (gate + own sync) exceeds it in **0** of 41 rolls; the waited writer-thread sync exceeds it in **0**.

Consecutive roll pairs: 41. (gate + own sync) of roll k exceeds the interval to roll k+1 in **0** pairs; the waited writer-thread sync of roll k exceeds that interval in **0**.

#### group: the point readers (us)

| arm | metric | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| with bulk writer, group-mixed-r1 | read_service | 12006 | 37 | 222 | 282 | 490 | 678 | 1168 | 2902 |
| with bulk writer, group-mixed-r1 | read_from_schedule | 12006 | 69 | 353 | 429 | 680 | 893 | 1826 | 4055 |
| with bulk writer, group-mixed-r2 | read_service | 12006 | 44 | 215 | 278 | 491 | 688 | 920 | 7787 |
| with bulk writer, group-mixed-r2 | read_from_schedule | 12006 | 97 | 344 | 429 | 683 | 909 | 1603 | 7917 |
| alone (isolated), group-iso-r1 | read_service | 12006 | 85 | 559 | 681 | 820 | 865 | 1461 | 2736 |
| alone (isolated), group-iso-r1 | read_from_schedule | 12006 | 146 | 767 | 901 | 1064 | 1123 | 2132 | 3026 |

p99 ratio read_service, group-mixed-r1 / group-iso-r1 = 0.8x (678 / 865 us).

p99 ratio read_from_schedule, group-mixed-r1 / group-iso-r1 = 0.8x (893 / 1123 us).

p99 ratio read_service, group-mixed-r2 / group-iso-r1 = 0.8x (688 / 865 us).

p99 ratio read_from_schedule, group-mixed-r2 / group-iso-r1 = 0.8x (909 / 1123 us).

#### group: slow reader samples (> 1000 us from schedule) against roll windows

| run | slow samples | overlapping a roll [start,end] | overlapping a roll + 50 ms | > 10 ms and overlapping | > 10 ms total | slow outside any roll window (us, max) |
|---|---|---|---|---|---|---|
| group-mixed-r1 | 53 | 0 | 2 | 0 | 0 | 4055 |
| group-mixed-r2 | 46 | 3 | 3 | 0 | 0 | 2903 |

#### group: SHOW META wal_roll_* deltas (m1 - m0)

| run | wal_rolls | hold_us | gate_us | sync_us | create_us | body hit/waited/inline | c0 stream-latch wait (us) | c0 roll_blocked_wait_us | wal_syncs |
|---|---|---|---|---|---|---|---|---|---|
| group-mixed-r1 | 20.0 | 49317.0 | 991.0 | 36663.0 | 11372.0 | 20.0/0.0/0.0 | 660.0 | 630.0 | 3080.0 |
| group-mixed-r2 | 23.0 | 69659.0 | 1003.0 | 45396.0 | 22853.0 | 23.0/0.0/0.0 | 10907.0 | 10405.0 | 3903.0 |

## A.strict (generated)

#### strict: runs

| run | rolls in window | window s | interval between rolls p50 (ms) | log MiB/s | rows/s | wal_first_segment | segments removed | sync_segs max | reader p99 from-send (us) | reader p99 from-schedule (us) | slow(>1ms) samples |
|---|---|---|---|---|---|---|---|---|---|---|---|
| strict-mixed-r1 | 19 (counter 19.0) | 30.5 | 1588 | 39.9 | 91287 | 10.0 | 10.0 | 19 | 632 | 876 | 38 |

#### strict: roll hold (ms), pooled over 1 run(s), 19 rolls

| series | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|
| hold | 0.91 | 1.22 | 1.39 | 1.47 | 2.40 | 15.79 | 15.79 |
| hold, strict-mixed-r1 (n=19) | 0.91 | 1.22 | 1.39 | 1.47 | 2.40 | 15.79 | 15.79 |

#### strict: the hold per step (ms; share = sum of step / sum of hold)

| step | share of hold | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| seal | 0.7 % | 0.00 | 0.01 | 0.01 | 0.02 | 0.02 | 0.03 | 0.03 |
| flush | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| sync_gate (wait on writer-thread sync) | 3.7 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.55 | 0.94 | 0.94 |
| sync (roll's own, total) | 74.5 % | 0.44 | 0.77 | 0.90 | 1.08 | 1.11 | 14.49 | 14.49 |
|   sync tail | 41.1 % | 0.05 | 0.14 | 0.19 | 0.22 | 0.24 | 13.64 | 13.64 |
|   sync rest (non-tail segs) | 33.4 % | 0.32 | 0.56 | 0.75 | 0.88 | 0.93 | 0.98 | 0.98 |
| create (total) | 21.1 % | 0.30 | 0.38 | 0.41 | 0.46 | 0.48 | 1.27 | 1.27 |
|   body_wait | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   body inline | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   header write+sync | 5.9 % | 0.11 | 0.11 | 0.12 | 0.12 | 0.16 | 0.26 | 0.26 |
|   rename | 0.9 % | 0.01 | 0.01 | 0.02 | 0.02 | 0.02 | 0.04 | 0.04 |
|   dirsync | 14.0 % | 0.18 | 0.24 | 0.27 | 0.30 | 0.34 | 0.99 | 0.99 |
| unaccounted (hold - listed) | 0.1 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |

Body outcome: hit 19, waited 0, inline 0 (of 19).
Segments covered by the roll's own sync (sync_segs): p0/p50/p99/max = 6/14/19/19; waited_syncs>0 in 2 of 19 rolls.

#### strict: the residual under the latch if sync, header, rename, dirsync leave it (us; bar 10,000 us)

| residual | n | p50 | p90 | p99 | max | rolls > 10 ms |
|---|---|---|---|---|---|---|
| R1 = seal + flush + body_wait + body | 19 | 14 | 24 | 26 | 26 | 0 |
| R2 = seal + flush | 19 | 14 | 24 | 26 | 26 | 0 |

#### strict: could one thread keep up with the syncs if they left the latch

| series | p50 | p90 | p99 | max |
|---|---|---|---|---|
| gate + own sync (us) | 903 | 1970 | 14488 | 14488 |
| waited writer-thread sync, tail+rest (us) | 0 | 551 | 1169 | 1169 |
| interval to next roll (us) | 1588151.167 | 3020938.499 | 3366278.183 | 3366278.183 |

Nominal interval (one 64 MiB segment at the run's mean log byte rate) = 1604 ms; this is the stalled-free-rate-free reading, because the measured intervals above are stretched by the holds themselves. (gate + own sync) exceeds it in **0** of 18 rolls; the waited writer-thread sync exceeds it in **0**.

Consecutive roll pairs: 18. (gate + own sync) of roll k exceeds the interval to roll k+1 in **0** pairs; the waited writer-thread sync of roll k exceeds that interval in **0**.

#### strict: the point readers (us)

| arm | metric | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| with bulk writer, strict-mixed-r1 | read_service | 12006 | 34 | 219 | 281 | 457 | 632 | 893 | 4753 |
| with bulk writer, strict-mixed-r1 | read_from_schedule | 12006 | 44 | 349 | 431 | 648 | 876 | 1484 | 4941 |
| alone (isolated), strict-iso-r1 | read_service | 12006 | 72 | 588 | 697 | 822 | 863 | 1017 | 2809 |
| alone (isolated), strict-iso-r1 | read_from_schedule | 12006 | 129 | 805 | 916 | 1069 | 1120 | 1752 | 3006 |

p99 ratio read_service, strict-mixed-r1 / strict-iso-r1 = 0.7x (632 / 863 us).

p99 ratio read_from_schedule, strict-mixed-r1 / strict-iso-r1 = 0.8x (876 / 1120 us).

#### strict: slow reader samples (> 1000 us from schedule) against roll windows

| run | slow samples | overlapping a roll [start,end] | overlapping a roll + 50 ms | > 10 ms and overlapping | > 10 ms total | slow outside any roll window (us, max) |
|---|---|---|---|---|---|---|
| strict-mixed-r1 | 38 | 0 | 1 | 0 | 0 | 4941 |

#### strict: SHOW META wal_roll_* deltas (m1 - m0)

| run | wal_rolls | hold_us | gate_us | sync_us | create_us | body hit/waited/inline | c0 stream-latch wait (us) | c0 roll_blocked_wait_us | wal_syncs |
|---|---|---|---|---|---|---|---|---|---|
| strict-mixed-r1 | 19.0 | 40611.0 | 1493.0 | 30250.0 | 8575.0 | 19.0/0.0/0.0 | 12088.0 | 11597.0 | 3263.0 |
