# BM-S6 remedy: B2 costs +8.9 and +3.2 µs more than A on the every-row `UPDATE` (B: +27.8 and +31.7), a gap that stays inside the A-against-A' spread

**Thesis.** On `worktree-bm-scan-kernel-open`, B2 (`952c67e5`, `v2.7.0-948-g952c67e5`: B plus "a passing row builds no `Status`
on UPDATE's path") runs the every-row `UPDATE r SET v = ? WHERE id >= 1` over 1,000 rows (`cores = 1`, `relaxed`, every page
resident) **8.9 µs (+1.2 %) literal and 3.2 µs (+0.4 %) prepared slower than A** (`fa80da9d`, `v2.7.0-940-gfa80da9d`, BM's
opening) in the pooled median of 15 paired runs, against **+27.8 µs (+3.8 %) and +31.7 µs (+4.3 %) for B** (`6b771ec1`,
`v2.7.0-946-g6b771ec1`) in the same runs. B2 is faster than B by 20.9 and 26.6 µs in the pooled median, and slower than B in 2 and
1 of 15 runs. The brief's question was whether B2 is back within about 10 µs of A, the close's typical A-against-A' gap. **On the
pooled median, yes on both paths, by 1.1 and 6.8 µs. Not cleanly:**

- B2 is slower than A in **12 of 15 runs literal and 11 of 15 prepared**. A residual of a few µs (0.4-1.2 %) is more likely
  present than absent, and this run cannot separate it from the instance noise;
- the three batches of five runs read B2 - A = +2.4 / +8.9 / +16.9 µs literal and +1.6 / +3.2 / +14.8 µs prepared. The third batch,
  started in a different server order, is above 10 µs on both paths while B itself reads +43.0 and +38.8 µs in it;
- this run's own noise floor is not smaller than the close's: the same-bytes control A' differs from A by a typical 14.7 µs
  literal and 7.5 µs prepared (median of |A - A'| over 15 runs), 41.8 and 28.9 µs at the worst run. The close's typical gap was
  9.7-10.7 µs. By the bar of BM-Q12 as written (median B2 - A against the largest A-against-A' gap) the cell is **within the
  spread** on both paths, in all three batches; against the typical gap it is above in 4 of the 6 cell-batches, by 0.3 to 7.2 µs.

The two cells that are not the question hold. **The point `SELECT`** reads B2 - A = +0.15 µs of 29.5 (+0.5 %) literal and +0.43 µs
of 27.1 (+1.6 %) prepared, spread 0.35 and 0.58 µs (5 runs). **The scan row** (`bn`, 10,000 rows, 5 runs) is 30.5 ns for B2
against 66.0 ns for A in the same run, -35.5 ns (-53.7 %), flat across the two paths, rows and counts identical.

The run was held to the operator's 20-minute budget for a stage's measurement (section 8 lists what that cut). Section 1 carries the
guard cell, section 2 the batches, section 3 the other two cells, section 4 the latency distributions, section 5 the waits, section 6
the noise floor and host, section 7 what the run teaches. The drivers are documented in their own docstrings (`bench/README.md`:
`bench/docs/` stays closed).

| Item | Value |
|---|---|
| Executed | 2026-10-13, UTC. Build about 06:10:35-06:11:11. `bn_census` batch 1 (A, A', B, B2 and the point `SELECT`, 5 runs) 06:11:28-06:14:32. `bm_scan` `bn` cell (A, B2; 5 runs) 06:14:32-06:15:41. Batch 2 (every-row `UPDATE` only, 5 runs) 06:15:42-06:17:39. Batch 3 (every-row `UPDATE` only, 5 runs) 06:18:02-06:19:58 |
| Worktree / branch | `bm-scan-kernel-open`, branch `worktree-bm-scan-kernel-open`, HEAD `952c67e5` |
| Commits measured | **A** = `fa80da9dbd654d3dfd5456d75c45adcf31d50712`, `v2.7.0-940-gfa80da9d`. **B** = `6b771ec11701b394870cd60f269545a6a34976cb`, `v2.7.0-946-g6b771ec1`, committed 2026-10-13 04:41:04 UTC. **B2** = `952c67e5`, `v2.7.0-948-g952c67e5`, committed 2026-10-13 06:09:54 UTC. B2 differs from B in `src/exec/chain_frame.cpp`, `row_codec.cpp` and `step_vm.cpp` (51 insertions, 28 deletions: the remedy; `git diff --stat 6b771ec1 952c67e5 -- src include`) and, outside the binary, in the results and driver commit `34ab4269`; A to B is the BM series |
| Tree | The worktree was **dirty**: CLAUDE.md, `docs/inflight/known-gaps.md`, `docs/spec/parser-v2.md`, `include/kds/exec/step_vm.hpp`, `instructions/v3.0.0/raft-marks-2026-10-13.md`, `kds.conf.sample`, `manual/sql/sql.md` (`archive/.../host.txt`). **None of it is in a measured number**: B2 was built from `git archive 952c67e5` into the scratchpad, never from the worktree; A and B are the close's binaries, byte for byte (hashes below) |
| Binary provenance | Copies in `/home/cdkbs/bench-runs/bm-s6r/bin/`, hashed after the copy, every server started from them (`archive/.../binaries.txt`). **A** and **A'** (the same file started as a second server) `c4f442aebe7e3a7d4ea504b94e16d757c1770ffb4ae71fcae09b075faaab45d6` (source mtime 2026-10-13 04:50:18 for the copy in `bm-s6/bin/`, built 04:45:27, from the close). **B** `963894d08781bd2d37b682b1a453be210ba7590e91fc157129ae8b272ce069ca` (built 04:46:56). **B2** `dbb42c2d6f4fba515e56c86b7fadc8a50172da08117e269b16c3e2cd20abf9a4`, source `build/kds_server` mtime 2026-10-13 06:11:11.98 UTC, 1 min 17 s after its commit, so not older than the commit. `sha256sum -c` of A and B against the close's `SHA256SUMS` passed before the first cell. A binary older than its commit: none |
| Build | B2: `CMAKE_BUILD_TYPE=Release` (read from `CMakeCache.txt`), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, no extra flags, target `kds_server`, `-j8`; gcc 13.3.0 |
| Host / device | 8 vCPU AMD EPYC 9V74 (4 cores x 2 threads), Linux 7.0.0-1014-azure, 16 GB. **Data files and WAL under `/home/cdkbs/bench-runs/bm-s6r/` on `/dev/root`, ext4, not tmpfs** (`df -T`); the binaries' copies are on the same device |
| Pinning | Servers (one process per arm, one busy at a time) on CPU 2, driver on CPU 4; the SMT siblings 3 and 5 and CPUs 0, 1, 6, 7 were left to whatever else ran |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, Waystone at its defaults; `scan_kernel` not written (default); a fresh server and data file per run and arm; ports 32601 (batch 1), 32611 (`bn`), 32621 (batch 2), 32631 (batch 3) |
| Host load | Other sessions' Claude processes were resident and idle; no other session's benchmark, build or test run was seen on the pinned CPUs. 1-min load average at each run's gate 0.97-1.96 (a gate wait of up to 55 s), host-watch samples every 10 s from 06:11:28: median 1.57, max 4.38 at the first sample (the end of this run's own build). Section 6 |
| Drivers | `tools/bn_census.py` sha256 `d7b80765...` and `tools/bm_scan.py` `1896ef20...`, both the close's, unchanged. Tables: `archive/.../scripts/s6r_*.py` (this stage's, not in `tools/`) |
| Suite | **Not run in this session.** B2 is CLA's remedy commit and was not changed here. The drivers' end-state checks (`range_v_ok`, `spill_txt_ok`: the final column values of every row) passed for every arm of every run, and `bn`'s row digest and `ANALYZE` counts were identical for A and B2 (run 0, the run that records them) |

## 1. The every-row `UPDATE`: B2 is 20.9-26.6 µs under B and 3.2-8.9 µs over A

`update-range` on `bn_census`'s 1,000-row table, 240 statements per path per arm per run, **15 paired runs** in three batches of five
(A, A', B and B2 started as four fresh servers per run; the start order differs by batch, section 2). Server CPU per statement is
the measure; the rate is 1e6 divided by it. Median over 15 runs, then the deltas paired by run:

**Statements/s (derived, 1e6 / median server CPU µs; each statement updates 1,000 rows)**

| path | A | A' (same bytes as A) | B (`6b771ec1`) | B2 (`952c67e5`) |
|---|---|---|---|---|
| literal | 1,357 /s | 1,342 /s | 1,305 /s | 1,328 /s |
| prepared | 1,354 /s | 1,349 /s | 1,309 /s | 1,344 /s |

**Server CPU per statement (µs), median over 15 runs [min, max]; deltas paired by run**

| path | A | A' | B | B2 | B - A | B2 - A | B2 - B | A - A' (S = largest abs; typical = median abs) |
|---|---|---|---|---|---|---|---|---|
| literal | 736.99 [724.66, 775.69] | 745.21 [724.06, 783.71] | 766.33 [752.58, 802.69] | 753.07 [723.50, 824.87] | +27.77 [+5.09, +74.98] (+3.8 %) | +8.93 [-13.71, +64.41] (+1.2 %) | -20.89 [-58.38, +37.04] (-2.8 %) | -1.40 [-41.75, +34.35] (S = 41.75, typical 14.69) |
| prepared | 738.33 [718.96, 793.77] | 741.09 [726.17, 764.84] | 764.07 [747.91, 791.34] | 744.21 [719.39, 797.00] | +31.69 [-17.79, +48.35] (+4.3 %) | +3.22 [-34.75, +60.45] (+0.4 %) | -26.58 [-50.43, +21.62] (-3.6 %) | -3.94 [-27.47, +28.93] (S = 28.93, typical 7.54) |

**Runs in which the first arm is slower than the second (of 15 paired runs)**

| path | B2 slower than A | B slower than A | B2 slower than B | A slower than A' | A' slower than A |
|---|---|---|---|---|---|
| literal | 12 of 15 | 15 of 15 | 2 of 15 | 7 of 15 | 8 of 15 |
| prepared | 11 of 15 | 14 of 15 | 1 of 15 | 4 of 15 | 11 of 15 |

Reading it. B is slower than A in 29 of 30 runs (median +27.8 and +31.7 µs); B2 gives back 75 % (literal) and 84 % (prepared) of that in the median. The
rest is a median of +8.9 and +3.2 µs against a control, A', that sits +1.4 and +3.9 µs above A on the same runs: on the prepared path the
residual is the size of the control's own offset; on the literal path it is about 7 µs more than the control's. The sign counts
(12 and 11 of 15) lean the same way and are not independent of the host's drift, since the four servers of a run share one
host window. **The reading this run supports: the remedy removes most of B's cost, and any residual is under about 10 µs (1.2 %),
inside what two instances of one executable do here, and not proven to be zero.**

The single outlying run is batch 3, run 3: B2 reads +64.4 and +60.5 µs over A in both paths, and +37.0 and +21.6 µs over B. The next
run of the same batch reads B2 - A = +8.5 and +31.2. The medians above do not depend on it: without that run they are +8.7 and +3.15 µs.

## 2. The three batches: the residual rises with the batch's own noise

The batches differ only in the order the four servers are started (the driver starts them in `--arm` order and rotates the
*statement* order inside a run, not the start order). Medians over the batch's 5 runs, µs of server CPU per statement:

| batch (server start order) | path | A | A' | B | B2 | B - A | B2 - A | B2 - B | A - A' typical |
|---|---|---|---|---|---|---|---|---|---|
| 1 (A, A', B, B2) | literal | 733.9 | 731.4 | 761.9 | 734.6 | +27.9 | +2.4 | -25.5 | 1.7 |
| 1 (A, A', B, B2) | prepared | 722.4 | 731.9 | 753.2 | 724.0 | +31.7 | +1.6 | -32.3 | 7.5 |
| 2 (B2, B, A', A) | literal | 734.4 | 745.2 | 764.9 | 753.1 | +23.4 | +8.9 | -14.5 | 18.5 |
| 2 (B2, B, A', A) | prepared | 743.2 | 740.3 | 771.3 | 744.2 | +28.0 | +3.2 | -19.5 | 2.9 |
| 3 (B, B2, A, A') | literal | 740.5 | 753.0 | 787.8 | 757.3 | +43.0 | +16.9 | -26.1 | 16.0 |
| 3 (B, B2, A, A') | prepared | 740.8 | 748.4 | 775.4 | 753.1 | +38.8 | +14.8 | -17.2 | 7.6 |

Batch 1 is the planned cell of the brief (A, B, B2 interleaved, 5 runs; A' added as the in-run control); batches 2 and 3 were run
because the time budget allowed them and are reported whole. **Batch 1 alone reads B2 - A = +2.4 µs literal [-3.4, +10.0] and +1.6
µs prepared [-3.2, +3.1], B2 slower than A in 4 and 3 of 5 runs, B - A = +27.9 and +31.7 in 5 of 5.** Batches 2 and 3 are noisier (the
A-against-A' typical gap is 16-18 µs on the literal path in both) and B2 - A grows with B - A: B2 - A is 9 %, 38 % and 39 % of B - A
on the literal path in batches 1, 2, 3. Three batches do not say whether that is a real small residual that scales with host
state or drift; the pooled median is the figure to carry.

Per run, µs, `B - A / B2 - A / A - A'`:

| batch / run | literal | prepared |
|---|---|---|
| 1 / 0 | +21.1 / +10.0 / +9.3 | +35.4 / +1.6 / -7.5 |
| 1 / 1 | +5.1 / +8.4 / +1.7 | +34.2 / +1.9 / -27.5 |
| 1 / 2 | +39.4 / -3.4 / +1.7 | +25.3 / -3.2 / -3.6 |
| 1 / 3 | +43.5 / +0.6 / -22.2 | +31.7 / +3.1 / -22.2 |
| 1 / 4 | +27.9 / +2.4 / +0.6 | +29.5 / -2.9 / -3.9 |
| 2 / 0 | +27.8 / -1.6 / -18.5 | +34.0 / +7.4 / -1.8 |
| 2 / 1 | +15.8 / +12.4 / +7.6 | -17.8 / -34.8 / +28.9 |
| 2 / 2 | +23.4 / +8.9 / -41.5 | +22.8 / +3.2 / -0.2 |
| 2 / 3 | +7.2 / -13.7 / +34.4 | +35.1 / -15.3 / +25.0 |
| 2 / 4 | +30.5 / +18.7 / -4.0 | +28.0 / +21.7 / +2.9 |
| 3 / 0 | +61.8 / +29.3 / -41.8 | +47.8 / +14.8 / -21.5 |
| 3 / 1 | +43.0 / +16.9 / -16.0 | +27.7 / +9.2 / -7.6 |
| 3 / 2 | +16.8 / +8.5 / +14.7 | +48.3 / +31.2 / -13.5 |
| 3 / 3 | +27.4 / +64.4 / -23.3 | +38.8 / +60.5 / -4.5 |
| 3 / 4 | +75.0 / +16.6 / -1.4 | +20.9 / +9.1 / +6.9 |

## 3. The point `SELECT`: within the spread; the scan row is 30.5 ns

**Point `SELECT`** (`select-pk` on the 1,000-row table, batch 1: 5 runs x 3,000 statements per path per arm). Server CPU per
statement, rate derived:

| path | A | A' | B | B2 | A statements/s | B statements/s | B2 statements/s | B - A | B2 - A | B2 - B | A - A' (S; typical) | B2 slower than A |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| literal | 29.54 [27.21, 30.51] µs | 29.73 [27.24, 30.16] µs | 29.09 [27.35, 30.19] µs | 29.54 [27.36, 30.47] µs | 33,847 /s | 34,379 /s | 33,858 /s | +0.14 µs (+0.5 %) | +0.15 [-0.39, +0.93] µs (+0.5 %) | +0.01 µs | -0.01 (S 0.35; 0.08) | 3 of 5 |
| prepared | 27.07 [26.72, 28.90] µs | 27.64 [26.24, 29.07] µs | 28.12 [25.97, 29.55] µs | 27.50 [26.43, 29.50] µs | 36,943 /s | 35,559 /s | 36,364 /s | +0.34 µs (+1.3 %) | +0.43 [-0.29, +0.60] µs (+1.6 %) | +0.09 µs | -0.16 (S 0.58; 0.17) | 4 of 5 |

B2 - A is above the typical gap and under S on both paths, as B - A is. B2 is not measurably different from B (+0.01 and +0.09
µs). The remedy changed a path a point `SELECT` shares (`row_codec`, `step_vm`), and nothing here says it cost or saved it anything.

**The scan row** (`tools/bm_scan.py census --cells bn`: `SELECT * FROM s WHERE w = ?`, 10,000 rows, 192 scans per path per run, 5
runs, A and B2 only as the brief said; net row = (scan CPU - point CPU on the same relation, arm, path and run) / rows):

| path | A net row | B2 net row | A rows/s (derived) | B2 rows/s (derived) | B2 - A (paired) | B2 slower than A |
|---|---|---|---|---|---|---|
| literal | 66.0 [65.4, 67.4] ns | 30.5 [30.3, 30.9] ns | 15,140,061 /s | 32,805,159 /s | -35.5 [-37.1, -35.0] ns (-53.7 %) | 0 of 5 |
| prepared | 66.2 [65.3, 67.6] ns | 30.5 [30.2, 30.8] ns | 15,114,011 /s | 32,769,873 /s | -35.7 [-37.5, -34.8] ns (-54.0 %) | 0 of 5 |

The row digest and `examined=`, `matched=`, `sel=`, `pages=` of `ANALYZE` are identical between A and B2 (run 0). B was not in this
cell. The close measured B at 31.0 ns and A at 70.0 ns in another session; A moved 4 ns between the sessions, so the 0.5 ns
between that B figure and this B2 figure carries no weight, and the paired B2 - A above is the only figure this run offers.

## 4. Latency distributions

Wall latency per statement as the driver sees it (client to reply, one connection). Percentiles are the median over runs of each run's
percentile; `n` is the statements pooled. A statement of `update-range` is one `UPDATE` of 1,000 rows. Statements/s at p50 is derived
(1e6 / p50).

**`update-range`, 15 runs (3 batches)**

| path | arm | n | p0 | p25 | p50 | p90 | p99 | statements/s at p50 |
|---|---|---|---|---|---|---|---|---|
| literal | A | 3,600 | 692.2 µs | 732.1 µs | 751.9 µs | 881.5 µs | 1801.4 µs | 1,330 /s |
| literal | A' | 3,600 | 701.4 µs | 735.8 µs | 754.5 µs | 874.8 µs | 1561.9 µs | 1,325 /s |
| literal | B | 3,600 | 726.4 µs | 765.4 µs | 782.4 µs | 910.7 µs | 1912.3 µs | 1,278 /s |
| literal | B2 | 3,600 | 703.1 µs | 745.2 µs | 759.8 µs | 890.9 µs | 1609.7 µs | 1,316 /s |
| prepared | A | 3,600 | 689.8 µs | 726.8 µs | 745.6 µs | 868.5 µs | 1676.5 µs | 1,341 /s |
| prepared | A' | 3,600 | 692.6 µs | 730.5 µs | 751.1 µs | 879.2 µs | 1223.1 µs | 1,331 /s |
| prepared | B | 3,600 | 718.8 µs | 760.5 µs | 776.1 µs | 884.8 µs | 1238.5 µs | 1,289 /s |
| prepared | B2 | 3,600 | 701.7 µs | 740.9 µs | 755.9 µs | 884.8 µs | 1270.7 µs | 1,323 /s |

B's whole body sits 30-34 µs above A (p0 +34, p25 +33, p50 +30 literal); B2's sits 11-13 µs above A at p0-p25 and 8 µs at p50. The
shape is a shift, not a tail: p90 and p99 are not ordered by arm (B2's p99 is under A's on both paths and under B's on the literal one). The p0 columns are
the better read of fixed cost (the host contributes least there), and they say B2 retains about 11 µs on the literal path and
12 µs on the prepared path at p0, against 34 and 29 for B.

**`select-pk`, batch 1 (5 runs)**

| path | arm | n | p0 | p25 | p50 | p90 | p99 | statements/s at p50 |
|---|---|---|---|---|---|---|---|---|
| literal | A | 15,000 | 53.0 µs | 55.1 µs | 57.6 µs | 69.5 µs | 89.3 µs | 17,347 /s |
| literal | A' | 15,000 | 52.8 µs | 55.1 µs | 57.7 µs | 69.6 µs | 100.3 µs | 17,338 /s |
| literal | B | 15,000 | 52.4 µs | 55.3 µs | 57.0 µs | 69.3 µs | 91.1 µs | 17,551 /s |
| literal | B2 | 15,000 | 52.2 µs | 55.4 µs | 58.3 µs | 70.7 µs | 92.2 µs | 17,147 /s |
| prepared | A | 15,000 | 49.8 µs | 51.9 µs | 53.2 µs | 63.4 µs | 81.7 µs | 18,811 /s |
| prepared | A' | 15,000 | 49.8 µs | 52.1 µs | 53.7 µs | 64.7 µs | 83.3 µs | 18,632 /s |
| prepared | B | 15,000 | 48.5 µs | 52.5 µs | 54.5 µs | 65.7 µs | 87.0 µs | 18,361 /s |
| prepared | B2 | 15,000 | 49.6 µs | 52.3 µs | 53.5 µs | 64.5 µs | 83.5 µs | 18,698 /s |

**The scan statement (`bn`, 10,000 rows, 5 runs, 960 scans per path and arm)**

| path | arm | n | p0 | p25 | p50 | p90 | p99 | scans/s at p50 |
|---|---|---|---|---|---|---|---|---|
| literal | A | 960 | 693.6 µs | 713.1 µs | 721.9 µs | 749.1 µs | 1069.3 µs | 1,385 /s |
| literal | B2 | 960 | 349.5 µs | 356.7 µs | 368.2 µs | 388.6 µs | 477.6 µs | 2,716 /s |
| prepared | A | 960 | 690.6 µs | 709.9 µs | 717.7 µs | 742.8 µs | 1069.9 µs | 1,393 /s |
| prepared | B2 | 960 | 346.1 µs | 353.5 µs | 363.7 µs | 383.1 µs | 533.9 µs | 2,750 /s |

## 5. Waits

A statement is a sum of waits. At this configuration the sum is nearly one term.

| shape | path | arm | wall p50 | server CPU | driver CPU |
|---|---|---|---|---|---|
| select-pk | literal | A / B / B2 | 57.6 / 57.0 / 58.3 µs | 29.5 / 29.1 / 29.5 µs (51 %) | 32.3 / 32.5 / 32.4 µs (56 %) |
| select-pk | prepared | A / B / B2 | 53.2 / 54.5 / 53.5 µs | 27.1 / 28.1 / 27.5 µs (51 %) | 29.8 / 29.9 / 29.8 µs (56 %) |
| update-range | literal | A / B / B2 | 745.3 / 771.2 / 745.8 µs | 733.9 / 761.9 / 734.6 µs (98 %) | 38.0 / 38.2 / 38.2 µs (5 %) |
| update-range | prepared | A / B / B2 | 737.9 / 767.7 / 739.5 µs | 722.4 / 753.2 / 724.0 µs (98 %) | 36.2 / 35.6 / 35.3 µs (5 %) |

(Batch 1 only; medians over 5 runs. Shares are of wall p50.) For the every-row `UPDATE` the server's own CPU is 98 % of the wall
time: the statement is write work, and the driver's 5 % overlaps the server's rather than adding to it (the sum is 103 %).
**Durability wait: none by configuration** (`relaxed`; `group` and `strict` were not measured). **Lock or conflict wait: none**
(one connection, one writer). For the point `SELECT`
the server and driver CPU shares sum to 107 %: the two overlap, because the server finishes its post-reply work while the driver
parses the reply, so the table does not close to 100 % and does not decompose the statement further. A wait type this run cannot
price on today's engine: the split of the 740 µs between the walk, the per-row write and the MVCC bookkeeping, which needs a
profile and was not taken.

## 6. The noise floor, and the host

**Noise floor, inside the run.** A and A' are one file started as two servers. Their per-run difference, in µs of server CPU on the
every-row `UPDATE`, over 15 runs: literal median -1.4, typical (median of the absolute value) 14.7, largest 41.8; prepared median
-3.9, typical 7.5, largest 28.9. On the point `SELECT` (5 runs): typical 0.08 and 0.17 µs, largest 0.35 and 0.58. **A delta under the
typical gap is not a finding by itself**: B2 - A is +8.9 and +3.2 µs on the UPDATE and +0.15 and +0.43 µs on the point `SELECT`.
B - A is +27.8 and +31.7 µs: above the typical gap (14.7 and 7.5) on both paths, above the largest gap on the prepared path (28.9)
and **below it on the literal path (41.8)**, and positive in 29 of 30 runs. The bar of BM-Q12 as written (largest gap) is set by one
outlying run, so on the literal path it cannot tell B from B2; the paired B2 - B (negative in 13 and 14 of 15 runs) and the typical gap
do.

**Host.** One quiet window per run was waited for (load gate 2.0 on the 1-min average, bounded at 240 s; the longest wait was 55 s,
batch 1 run 0, because this run's own build was still in the average). Every cell-run was clean by the driver's test (siblings at most
20 % busy, no other session's task above 15 % of the pinned CPUs): batch 1 20 of 20, batch 2 10 of 10, batch 3 10 of 10, `bn` 20 of 20.
Cell-runs that started above the load gate (2.0): batch 1 one, batch 2 two (2.06, 2.20), batch 3 two (both prepared, up to 2.20), `bn`
none; one batch 3 cell needed a second attempt (a 6 s quiet wait). No competing build or test run was recorded in any cell-run; no bad
replies. Run-queue wait is 13-24 % of on-CPU time on the `UPDATE` cells in batches 1 and 2 (the server's own threads share CPU 2
with it), 17-29 % in batch 3 (maximum 48.9 %), and 4 % on the point `SELECT`: batch 3 was the busiest of the three.

Batch 1 runs: load at the gate 1.96, 1.57, 1.08, 1.11, 1.73; batch 2: 1.12, 1.44, 1.89, 1.94, 1.67; batch 3: 1.02, 1.85, 1.79, 1.44,
1.74; `bn`: 1.85, 1.52, 1.37, 1.06, 0.97. Idle CPU per second per arm and run (µs) is in the JSON (`idle_cpu_us_per_s`): 3.7-5.1 ms
per second in batch 1, no arm separated from the others.

## 7. What the run teaches

- **The `UPDATE` regression is mostly in what the remedy touched.** B2 is 21-27 µs under B in the median and under B in 13 and 14
  of 15 runs; B2 is within a few µs of A on the prepared path and about 9 µs above on the literal. That the remedy changes `UPDATE`'s
  per-row path (its commit message: "a passing row builds no `Status`") is consistent with 21-27 µs over 1,000 rows, which is
  21-27 ns per updated row. **This run has one row count and cannot say whether the cost is per row or fixed.** The 200- and
  10,000-row sizes of `bn_census` were not run (section 8); a fixed cost would have shown as the same µs at 200 rows.
- **The remedy did not cost the scan or the point statement anything visible.** The scan row is 30.5 ns (B2) against 66.0 ns (A) in
  the same run; the point `SELECT` shows B2 - B of +0.01 and +0.09 µs.
- **A 10 µs question on a 740 µs statement needs the pooled median.** One batch of five runs read B2 - A as +2.4 µs, three batches as
  +8.9 µs. The same-bytes control moves the cell by 15 µs typically and 42 µs at worst, so a 5-run cell can neither confirm nor
  exclude a 10 µs residual; the conclusion above rests on 15 paired runs and still carries "not proven zero".
- **Server start order is visible.** Batch 1 (A started first) has the smallest B2 - A and the smallest literal-path A-against-A' gap;
  batches 2 and 3 (A started last and third) are noisier. The BM close (`v2.7.0-946-g6b771ec1`, section 4) found that the instance
  moves a cell; this run shows the order moving it too and does not identify a cause.
- No open decision in `CLAUDE.md` acquired a data point. BM-Q12's guard (the every-row `UPDATE`) reads "within the spread" by its bar
  at B2 and above the typical gap in four of six cell-batches; the bar would have read B "within" on the literal path too.

## 8. What was cut, and what was not run

Cut to the operator's 20-minute budget for a stage's measurement (this run: 06:10 to 06:21, build included):

- **`bn_census` at 200 and 10,000 rows**: not run. Only 1,000 rows, as the brief set. The every-row `UPDATE` therefore has one size, and
  the point `SELECT` (which does not scale with rows) has one size as evidence of that; rule 9's sweep is not satisfied here.
  Mapping: `--rows 1000 --scan-rows 1000` with `--range-ops 240` (240 every-row updates per path per arm per run) and `--ops 3000`.
- **The point `SELECT` ran in batch 1 only** (5 runs); batches 2 and 3 ran `--only update-range`.
- **The scan cell** ran A and B2 only (B was not in it, per the brief), 5 runs, one size (10,000 rows), `bn` only: not the 20-cell
  census.
- **`scan_kernel = off`, the layout probe (A-al64) and PostgreSQL 18**: not run. The PostgreSQL floor was not measured for this
  shape; the close carries it (29.1 ns per row).
- **`durability = group` and `strict`, `cores > 1`, `bn_census`'s `update-lit` and `scan` shapes**: not run.
- **The test suite**: not executed in this session.
- Batches 2 and 3 were added after batch 1 read; they are reported with batch 1, none discarded.

## 9. Baseline for the next run

The baselines for this shape are this file's figures at B2 (`v2.7.0-948-g952c67e5`): every-row `UPDATE`, 1,000 rows, `relaxed`, one core,
server CPU 753.07 µs literal and 744.21 µs prepared per statement (pooled median, 15 runs; 1,328 and 1,344 statements/s derived),
with A at 736.99 and 738.33 µs in the same runs; point `SELECT` 29.54 and 27.50 µs (33,858 and 36,364 statements/s); the scan row
30.5 ns on `bn` (32.8 million rows/s). The in-run control A' must run beside any arm compared against these.

## Appendix. Reproducing

All from `/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open`, copies in `/home/cdkbs/bench-runs/bm-s6r/bin/` (A, A' the same file, B
from `/home/cdkbs/bench-runs/bm-s6/bin/`; B2 from `git archive 952c67e5` built with
`cmake -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF` and `cmake --build --target kds_server -j8`).
`GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"`:

    python3 tools/bn_census.py --arm A=<A> --arm A2=<A copy> --arm B=<B> --arm B2=<B2> --workdir <dir> --port 32601 --runs 5 \
        --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
        --only select-pk,update-range --quiet-load 2 --quiet-wait 240 $GATE --out bn1000.json          # batch 1
    python3 tools/bn_census.py --arm B2=<B2> --arm B=<B> --arm A2=<A copy> --arm A=<A> ... --only update-range ...   # batch 2
    python3 tools/bn_census.py --arm B=<B> --arm B2=<B2> --arm A=<A> --arm A2=<A copy> ... --only update-range ...   # batch 3
    python3 tools/bm_scan.py census --arm A=<A> --arm B2=<B2> --workdir <dir> --port 32611 --runs 5 --pin-server 2 --pin-driver 4 \
        --cells bn --scan-ops 192 --point-ops 3000 --quiet-load 2 --quiet-wait 240 $GATE --out bmscan-bn.json

Tables: `scripts/s6r_table.py bn1000.json --section guard|runs|pct|hosts|waits`, `scripts/s6r_pool.py a.json,b.json,c.json --section
guard|batches|pct`, `scripts/s6r_scan_table.py bmscan-bn.json --section net|pct|hosts`, `scripts/s6r_digest.py bmscan-bn.json`.

Raw output: `bench/v3.0.0/archive/bm-s6-remedy-v2.7.0-948-g952c67e5/` - the four JSON files and logs (gzip), `binaries.txt`, `host.txt`
(device, toolchain, build cache, driver hashes, tree status), `hostwatch.log` (load average every 10 s), `progress.log`, `build.log.gz`
and the run scripts and table scripts.
