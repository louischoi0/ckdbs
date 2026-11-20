# DI close: the statement binding against the engine it opened from

`instructions/v3.2.0/workorder-di-shape-identity.md` DI-S5, DI-Q8 (a): no cell slower outside its own spread; no factor proposed, and a delta inside the spread is "not resolved", never "no cost". DI-R4 and DI-R5 (the statement path gains no latch, no cross-core write; one core pays nothing outside the floor) are the rules the bar enforces.

**Finding.** DI-Q8's bar is met as written and resolves little. In no cell is B (`v3.4.15-12-g871845e4`) slower than A (`v3.4.15-4-g1c7bb774`) outside the cell's own spread, either over all 12 paired lifetimes or over the pairs that ran on a quiet host, and both Waystone arms behave: `replays=` reads 0 where nothing is opted and 200 on the opted `scanprobe` on **both** binaries, so the opt-in keyed by what the example bound takes on B. On a quiet host the point reads are level (`select1` -0.12 us, `select8` -0.16 us of server CPU a statement, B against A) and the three write cells lean the other way: keyed `INSERT` +0.42 us at `cores = 1` (B dearer in 5 of 5 quiet pairs, spread 0.67 us) and +2.36 us at `cores = 8` (8 of 8, spread 4.23 us), the FK insert +0.47 us (2 of 4, spread 1.23 us). Over all 12 lifetimes B is the dearer in 10 of 12 on both `insert` cells and 9 of 12 on `fk1`, against 7 of 12 and 4 of 12 on the two reads. None leaves its spread, so none is a finding; **the write path is where a cost, if one exists, is hiding, and this host cannot say it is not there** (DI-S0 never measured a write). The host was shared: two sessions' builds and a measurement driver loaded it for much of the run, so a first pass was set aside whole and the cells carry quiet-pair statistics beside the as-measured ones (Method).

## Stamp

| item | value |
|---|---|
| executed | 2026-11-20 05:06-06:42 UTC (build 05:06:42-05:14:22; the set-aside first pass of driver 1 05:28-05:37; driver 2 06:02-06:12 and its extension 06:26-06:32; driver 1 06:13-06:22 and its extension 06:33-06:42) |
| branch | `worktree-di-shape-identity` |
| A | commit `1c7bb774`, `v3.4.15-4-g1c7bb774` (commit time 2026-11-20 01:30 UTC), the commit DI opened from |
| B | commit `871845e4`, `v3.4.15-12-g871845e4` (commit time 2026-11-20 05:05 UTC), DI-S4, DI's closing code commit |
| tree | each binary built from its own clean `git archive` of its commit (so clean by construction); the DI worktree carries uncommitted document edits and a changed `statement_stats.hpp` and was **not built** |
| binaries | both Release (`-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`), built after their commits: A at 05:10:49 UTC, B at 05:14:22 UTC. Copies `/home/louis/bench-runs/di-close/bin/kds_A` sha256 `b743a60ce0d685061ee63d73ed71c2ae0bb7667782ff453a03938471f1ac42cd` and `kds_B` sha256 `ff7d26cc2dec3cd9dcb92738fe9b40885c80443143a434a7ceafadb4dabf86fe`; every server started from a copy. **A's hash is identical to DI-S0's `kds_A`** (and to DA's close `kds_B`), so A is the engine those runs measured |
| device | data files and WAL under `/home/louis/bench-runs/di-close/` on `/dev/nvme0n1p1` (ext4, `df -T`), never tmpfs; host Intel Xeon Platinum 8581C, 8 vCPUs |
| server config, driver 1 | `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `access_statistics = on` (the shipped default), `cores` 1 (server pinned to CPU 2, one serial client) or 8 (unpinned, 16 closed-loop client processes); a fresh data file per lifetime |
| server config, driver 2 | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `waystone_recording = true`, `waystone_replay = true`; server pinned to CPU 2, driver to CPU 3; a fresh data file per run |
| quiet gate | `quietwait.py` before each driver (no process whose argv names a compiler, build or suite, and load average below 4, for 60 s running), plus driver 2's own per-run gate. Every gate waited for another session's builds and suites (worktrees `dl-statistics-queue-no-statement`, `audit-dead-code-g43-g55`, `dh-page-image-volume`). A resident `build-sandbox/kds_server` of another project ran throughout on both arms alike. **The gates sample at a driver's start only**; see Method for what loaded the lifetimes anyway |
| hold | outside the budget: gate 1 841 s, gate 2 1,521 s, gates 3-5 50 s, 210 s and 50 s, and driver 2's own per-run waits (450 s + 60 s in its first six reps, 250 s + 10 s in the extension), about 57 minutes in all |
| budget | build 7.7 min (two Release builds in sequence, themselves slowed by two other sessions' builds on the host); run time about 31 minutes active, of which the set-aside first pass of driver 1 was 9; about 39 minutes against the hour. Cut: nothing the order named. Added: driver 1 ran 12 rounds, not 6, and driver 2 12 reps, not 6 (below) |

## Method

**Driver 1, the statement cells** (`di_arms.py`, the DI-S0 driver with a `--start-round` flag added to resume; archived beside this file). One server lifetime per arm per round, arm order alternating (A first in odd rounds), 2 s warm then 6 s measured, `SELECT * FROM t WHERE id = <random>` over 10,000 rows, or a keyed `INSERT INTO t VALUES (<key>, ...)` into an empty relation, or DA's `fk1` (an `INSERT` into a child relation with a foreign key into a 1,000-row parent). Server CPU is `utime + stime` over the measured window divided by completed statements. The cells are `cores = 1` and `cores = 8` for the point `SELECT` (`select1`, `select8`) and the keyed `INSERT` (`insert1`, `insert8`), and `fk1`. Only 10,000 rows were run: a point lookup and a keyed insert do not scale with rows (bench rule 9, stated rather than hidden).

**Driver 2, CF's Waystone arms** (`cf/cf_di.py` on `cf/cf_s0.py`, archived). CF-S2's harness with four arms: `A`, `A-opt` (binary A, `scanprobe` opted by `WAYSTONE ON <its example>`), `B`, `B-opt`; the order rotates by one each rep. Changes against `bench/v3.3.0/archive/cf-harness/`: `cf_di.py` replaces `cf_s2.py` (arms and rotation; `--bins A=..,B=..`; the opt-in applied only to cells that are run); `cf_s0.py` reads its tools directory from `KDS_TOOLS` (one line); `chain4` and `control` are not run to fit the budget; `compare.py` and `run.sh` are unused, the fold is `analyze.py`. Cells `pk` (`SELECT * FROM a WHERE id = k`), `chain3` (a three-relation tuple-key join, the one cell whose statement binds more than one relation) and `scanprobe` (a filter scan then two tuple-key probes, 100 rows and 200 trail entries a statement), 1,000 keys (50 sessions for `scanprobe`), 22 passes of which the first two are warm-up, so 20,000 measured statements a run for `pk` and `chain3` and 1,000 for `scanprobe`. `replays=` is read from one `ANALYZE` of the cell's first key after the sweep, as in CF-S0. The reply digest of the last pass is identical across all 48 runs in every cell (invariant 8).

**What was set aside, what was added.**
- *The first pass of driver 1* (`raw1-contaminated-first-pass.jsonl`, 05:28-05:37 UTC, 6 rounds) ran while another session's measurement driver (`dl_s0.py`, a server and many client processes) was on the host: load average 2.7 at the gate, 15-22 from the second cell on, server CPU of 34-40 us a statement in the `cores = 1` cells against 22 us here on a quiet host. It is set aside, not used for any number below.
- *The second pass* (`raw1.jsonl`) ran 12 rounds in two launches (rounds 1-6 at 06:13-06:22, rounds 7-12 at 06:33-06:42). The gate was quiet at each start, but other sessions' builds loaded the host from about round 5 (06:19) on and again from about 06:35: the loads at the end of the lifetimes are in `analysis.txt`, 1.3-8 in rounds 1-4 (the `select8` and `insert8` figures include the driver's own 16 clients) and 9-14 in rounds 6 and 8-12. **Rounds 5-12 overlapped another session's build in part**: round 7 was quiet in `select8` (both arms 34.6 and 34.4 us) and loaded in `insert1` (31 and 38 us). A loaded lifetime reads 31-41 us in the `cores = 1` cells and 40 us in `select8`, against 22-23 and 34, so it is not subtle.
- *Two views of every driver-1 and driver-2 number.* The **as-measured** one uses every lifetime and is the record: its spreads (max - min over lifetimes) are 10-18 us in the `cores = 1` cells because they contain loaded lifetimes, which makes "outside the spread" nearly unattainable. The **quiet-pair** one drops, symmetrically, every pair in which either lifetime exceeds 1.10 x the lowest lifetime of its own arm and cell (the one-binary floor on this host is about 0.3 us in 22, so 10 % is wide of the floor and narrow of the spikes). The rule was chosen after seeing the data and is disclosed here; it uses nothing but the CPU figure it filters. It leaves 5, 7, 5, 8 and 4 pairs of 12 for `select1`, `select8`, `insert1`, `insert8` and `fk1`, and 8-12 of 12 for driver 2.
- *Reps beyond six.* Driver 2's first six reps held two single-run spikes (`B-opt-r1`, `B-r2`, at 20-30 us in a 22 us cell) that no mean over six can absorb; reps 7-12 were run as a second launch. Driver 1 was extended the same way.

The decomposition of the statement into waits (bench rule 3) does not apply: the unit measured is a server CPU cost compared between two binaries, not a latency to be broken down. The driver gives no wait breakdown.

## Driver 1: the statement cells

### As measured (12 lifetimes an arm, every one)

Spread = max - min over the 12 lifetimes. QPS is client-side completed statements a second. Quiet pairs below are where the comparison is read.

| cell | arm | CPU us/stmt, mean | CPU us/stmt, spread | QPS mean | QPS spread | statements |
|---|---|---|---|---|---|---|
| select1 | A | 29.95 | 17.64 | 18,043 | 16,073 | 1,300,311 |
| select1 | B | 30.86 | 18.67 | 17,981 | 11,263 | 1,295,737 |
| select8 | A | 36.68 | 8.26 | 81,342 | 67,488 | 5,877,382 |
| select8 | B | 36.45 | 6.95 | 80,110 | 62,892 | 5,788,930 |
| insert1 | A | 28.63 | 16.78 | 18,313 | 14,169 | 1,325,813 |
| insert1 | B | 30.44 | 17.09 | 18,168 | 10,293 | 1,317,776 |
| insert8 | A | 55.94 | 8.80 | 45,116 | 33,668 | 3,262,471 |
| insert8 | B | 57.73 | 5.65 | 44,427 | 31,500 | 3,210,948 |
| fk1 | A | 31.76 | 16.91 | 16,932 | 11,214 | 1,221,453 |
| fk1 | B | 33.20 | 18.71 | 16,034 | 16,465 | 1,155,462 |

### Quiet pairs

| cell | kept rounds | arm | CPU us/stmt, mean | CPU spread | QPS mean | QPS spread | statements |
|---|---|---|---|---|---|---|---|
| select1 | 1-5 | A | 22.80 | 1.13 | 23,326 | 1,521 | 700,318 |
| select1 | 1-5 | B | 22.68 | 1.44 | 23,535 | 1,445 | 706,622 |
| select8 | 1-5, 7, 10 | A | 34.29 | 1.21 | 106,624 | 15,090 | 4,491,268 |
| select8 | 1-5, 7, 10 | B | 34.13 | 0.89 | 103,535 | 20,498 | 4,362,178 |
| insert1 | 1-4, 10 | A | 22.11 | 0.63 | 23,371 | 489 | 706,543 |
| insert1 | 1-4, 10 | B | 22.53 | 0.67 | 23,189 | 566 | 704,772 |
| insert8 | 1-3, 5, 6, 9, 10, 12 | A | 54.79 | 4.23 | 47,460 | 33,668 | 2,288,537 |
| insert8 | 1-3, 5, 6, 9, 10, 12 | B | 57.15 | 3.97 | 45,358 | 28,870 | 2,185,831 |
| fk1 | 1-4 | A | 23.31 | 1.03 | 22,929 | 1,200 | 550,729 |
| fk1 | 1-4 | B | 23.77 | 1.23 | 22,860 | 762 | 549,068 |

`insert8` never has a quiet floor: its 16 clients contend for one tree's right edge and its own lifetimes span 51.9-60.7 us on A, so its "quiet" pairs are only the ones that did not also meet a build.

### Paired B - A, per round

CPU us/stmt, B minus A, round 1 to 12. A positive value is B dearer. "Outside" compares the mean with the larger of the two arms' spreads over the same lifetimes.

| cell | per round (us/stmt) | mean, all 12 | B dearer in | outside spread | quiet-pair mean | spread | quiet B dearer in | outside |
|---|---|---|---|---|---|---|---|---|
| select1 | +0.44 -0.16 +0.13 -0.28 -0.71 +3.16 -5.52 +1.16 -1.71 +9.22 +5.00 +0.19 | +0.91 (+3.0 %) | 7 / 12 | no | -0.12 (-0.5 %) | 1.44 | 2 / 5 | no |
| select8 | -0.54 +0.24 -0.99 -0.11 +0.33 -1.09 -0.27 -0.41 -0.89 +0.20 +1.33 -0.63 | -0.24 (-0.6 %) | 4 / 12 | no | -0.16 (-0.5 %) | 1.21 | 3 / 7 | no |
| insert1 | +0.16 +0.68 +0.08 +0.69 +9.13 -1.69 +7.07 +1.99 +2.00 +0.52 +2.26 -1.12 | +1.81 (+6.3 %) | 10 / 12 | no | +0.42 (+1.9 %) | 0.67 | 5 / 5 | no |
| insert8 | +2.22 +0.79 +0.92 -1.79 +0.97 +4.73 +5.96 -2.74 +2.28 +6.61 +1.20 +0.36 | +1.79 (+3.2 %) | 10 / 12 | no | +2.36 (+4.3 %) | 4.23 | 8 / 8 | no |
| fk1 | -0.69 -0.04 +1.02 +1.58 +1.20 +3.78 +2.12 +3.93 +0.63 -0.61 +2.46 +1.94 | +1.44 (+4.5 %) | 9 / 12 | no | +0.47 (+2.0 %) | 1.23 | 2 / 4 | no |

The same pairs in QPS, B against A (all 12 rounds, loaded ones included): `select1` +3.4 % mean (B slower in 7 of 12), `select8` -0.4 % (5 of 12), `insert1` +3.4 % (7 of 12), `insert8` -1.0 % (6 of 12), `fk1` -6.2 % (8 of 12); every one within a per-round swing of -50 to +76 %, so throughput says nothing here that the CPU column does not say better.

### Client latency (round trip, us; median of the per-lifetime percentiles, quiet pairs)

Includes the Python client and the socket; it is not server time.

| cell | arm | n statements | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|
| select1 | A | 700,318 | 33 | 39 | 40 | 48 | 59 |
| select1 | B | 706,622 | 33 | 39 | 40 | 46 | 58 |
| select8 | A | 4,491,268 | 30 | 83 | 126 | 245 | 470 |
| select8 | B | 4,362,178 | 29 | 86 | 125 | 248 | 470 |
| insert1 | A | 706,543 | 31 | 35 | 36 | 43 | 63 |
| insert1 | B | 704,772 | 31 | 36 | 37 | 44 | 65 |
| insert8 | A | 2,288,537 | 27.5 | 85 | 147.5 | 641 | 4,021 |
| insert8 | B | 2,185,831 | 29 | 87 | 153.5 | 783.5 | 4,125.5 |
| fk1 | A | 550,729 | 32.5 | 37 | 38 | 46 | 70.5 |
| fk1 | B | 549,068 | 32.5 | 38 | 38.5 | 46.5 | 70.5 |

## Driver 2: CF's Waystone arms

Twelve reps an arm, `cores = 1`. Spread is max - min over the 12 runs. `replays=` is the `ANALYZE` count after the sweep; `scanprobe`'s 1,000 statements are 50 sessions swept 20 times.

| cell | arm | opted | CPU us/stmt, mean | spread | median | QPS mean | QPS spread | p50 us | p99 us | statements a run | `replays=` |
|---|---|---|---|---|---|---|---|---|---|---|---|
| pk | A | no | 22.36 | 1.55 | 22.25 | 19,586 | 1,502 | 48.8 | 66.6 | 20,000 | 0 |
| pk | A-opt | `scanprobe` | 24.04 | 20.32 | 22.40 | 18,977 | 8,660 | 49.0 | 66.3 | 20,000 | 0 |
| pk | B | no | 24.07 | 14.29 | 22.85 | 18,641 | 9,270 | 49.2 | 67.6 | 20,000 | 0 |
| pk | B-opt | `scanprobe` | 24.51 | 21.55 | 22.72 | 18,753 | 9,130 | 49.4 | 67.7 | 20,000 | 0 |
| chain3 | A | no | 32.46 | 1.75 | 32.37 | 16,248 | 960 | 58.8 | 79.6 | 20,000 | 0 |
| chain3 | A-opt | `scanprobe` | 35.50 | 33.53 | 32.76 | 15,570 | 8,740 | 59.2 | 81.6 | 20,000 | 0 |
| chain3 | B | no | 32.85 | 1.88 | 32.67 | 16,216 | 1,185 | 59.0 | 80.0 | 20,000 | 0 |
| chain3 | B-opt | `scanprobe` | 35.66 | 33.42 | 32.81 | 15,563 | 8,295 | 59.0 | 80.6 | 20,000 | 0 |
| scanprobe | A | no | 410.29 | 50.27 | 403.01 | 1,732 | 232 | 539.7 | 835.7 | 1,000 | 0 |
| scanprobe | A-opt | yes | 398.16 | 427.20 | 364.82 | 1,811 | 1,059 | 498.0 | 779.7 | 1,000 | **200** |
| scanprobe | B | no | 448.79 | 385.83 | 412.55 | 1,642 | 948 | 540.2 | 865.5 | 1,000 | 0 |
| scanprobe | B-opt | yes | 397.53 | 451.09 | 363.09 | 1,817 | 1,150 | 504.1 | 782.2 | 1,000 | **200** |

The opt-in takes on both binaries: 200 replays on `scanprobe` in all 12 `A-opt` runs and all 12 `B-opt` runs, 0 everywhere else, `WAYSTONE ON` answered `OK WAYSTONE ON` on B for all 12 runs. A spread of 20-33 us on a 22-32 us cell is one or two loaded runs (`B-opt-r1`, `B-r2`, `A-opt-r8`; their pair partners are the big numbers in the per-rep rows below); medians are beside the means for that reason.

### Paired deltas, per rep

CPU us/stmt, first arm minus second, rep 1 to 12. The mean and the dearer count are over all 12; "quiet" is over the pairs left by the 1.10 x rule (the count kept is in the last column), and "outside" is that quiet mean against the larger of the two arms' spreads over the kept runs.

| cell | pair | per rep (us/stmt) | mean, all | first dearer in | quiet mean | quiet spread | quiet first dearer in | outside |
|---|---|---|---|---|---|---|---|---|
| pk | B - A | -0.64 +14.38 +1.08 -0.12 +0.18 -0.24 +2.19 +0.77 +0.79 +0.16 +0.88 +1.05 | +1.71 | 9 / 12 | +0.39 (+1.7 %) | 1.55 | 7 / 10 | no |
| pk | B-opt - A-opt | +21.91 +0.65 +0.69 +0.28 +0.71 +0.70 -0.63 -19.93 -0.10 +0.70 +0.40 +0.26 | +0.47 | 9 / 12 | +0.37 (+1.6 %) | 1.11 | 8 / 10 | no |
| pk | A-opt - A | -1.23 +0.16 +0.30 -0.02 +0.22 -1.09 +0.50 +20.58 +0.47 -0.22 +0.00 +0.53 | +1.68 | 8 / 12 | -0.03 | 1.55 | 7 / 11 | no |
| pk | B-opt - B | +21.33 -13.57 -0.08 +0.37 +0.75 -0.15 -2.32 -0.13 -0.42 +0.32 -0.47 -0.26 | +0.45 | 4 / 12 | -0.01 | 0.82 | 3 / 9 | no |
| chain3 | B - A | -1.54 +1.42 +0.29 -0.40 +0.28 +0.81 +1.63 +1.03 -0.29 +0.21 +0.41 +0.78 | +0.39 (+1.2 %) | 9 / 12 | +0.39 (all 12 kept) | 1.88 | 9 / 12 | no |
| chain3 | B-opt - A-opt | +33.47 +0.60 +0.33 +0.67 -0.10 -0.11 +0.38 -33.18 -0.21 -0.47 +0.71 -0.13 | +0.16 | 6 / 12 | +0.17 (+0.5 %) | 1.40 | 5 / 10 | no |
| chain3 | A-opt - A | -1.28 +0.27 +0.40 -0.68 +0.99 +0.58 +1.07 +33.54 +0.15 +1.00 -0.26 +0.64 | +3.04 | 9 / 12 | +0.26 (+0.8 %) | 1.75 | 8 / 11 | no |
| chain3 | B-opt - B | +33.73 -0.55 +0.45 +0.40 +0.61 -0.34 -0.17 -0.67 +0.23 +0.31 +0.05 -0.28 | +2.81 | 7 / 12 | +0.00 | 1.80 | 6 / 11 | no |
| scanprobe | B - A | -44.15 +371.73 +11.31 -2.81 -19.78 +31.28 +101.97 +6.02 -22.09 +3.37 +11.84 +13.31 | +38.50 | 8 / 12 | +6.82 (+1.7 %) | 33.10 | 6 / 8 | no |
| scanprobe | B-opt - A-opt | +431.08 +4.02 -17.37 +6.10 -11.76 -8.75 -0.83 -420.97 -4.25 +18.42 -4.85 +1.60 | -0.63 | 5 / 12 | -1.77 (-0.5 %) | 26.33 | 4 / 10 | no |
| scanprobe | A-opt - A | -80.92 -49.71 -23.59 -33.54 -44.97 -42.49 -43.01 +375.14 -71.57 -49.55 -40.00 -41.32 | -12.13 | 1 / 12 | **-40.91 (-10.1 %)** | 26.33 | **0 / 9** | **yes** |
| scanprobe | B-opt - B | +394.32 -417.42 -52.27 -24.62 -36.95 -82.53 -145.81 -51.84 -53.73 -34.51 -56.69 -53.02 | -51.26 | 1 / 12 | **-49.57 (-12.1 %)** | 33.10 | **0 / 9** | **yes** |

In QPS the opted `scanprobe` runs faster than the not-opted one in 11 of 12 paired reps on A (median +9.2 %) and in 11 of 12 on B (median +10.3 %); `pk` and `chain3` do not move (medians -0.1 % and -0.7 % on A, +0.4 % and -0.1 % on B). B-opt against A-opt in QPS is +0.3 % (median) on `scanprobe`.

## Against this engine's own previous numbers

A (`1c7bb774`) is the same binary as DA's close B and DI-S0's A (sha256 identical), so its quiet-pair numbers should agree with theirs, and they do, inside the stated spreads:

| cell | this run, A, quiet pairs (CPU us/stmt, QPS) | DI-S0, A (`results-di-s0-premise-v3.4.15-4-g1c7bb774.md`) | DA close, B (`results-da-close-v3.4.14-32-g89c26252.md`) | this run, B, quiet pairs |
|---|---|---|---|---|
| select1 | 22.80 us, 23,326 /s | 22.39 us (spread 1.93), 23,765 /s | 22.30 us, 23,957 /s | 22.68 us, 23,535 /s |
| select8 | 34.29 us, 106,624 /s | 34.04 us (1.20), 109,984 /s | 34.39 us, 110,509 /s | 34.13 us, 103,535 /s |
| fk1 | 23.31 us, 22,929 /s | not run | 22.76 us, 23,440 /s | 23.77 us, 22,860 /s |
| insert1, insert8 | 22.11 us, 23,371 /s; 54.79 us, 47,460 /s | not run | not run | 22.53 us, 23,189 /s; 57.15 us, 45,358 /s |

DI-S0's spike B1 (the binding in the key, 16 relations, before DI-S1's code as landed) measured 22.41 us at `select1` and 34.47 us at `select8`; this run's B is 22.68 and 34.13 us. B1 against this B: +0.27 us and -0.34 us, inside the spreads of both (1.44 and 0.89 here, 0.33 and 0.59 there); the landed code reads no dearer than the spike on the point read.

For Waystone, CF-S2's close (`results-cf-s2-close-v3.4.0-9-g46c632aa.md`, an AMD host at 40.5, 55.3 and 434.0 us for `pk`, `chain3` and a not-opted `scanprobe`) and this run's A are on different hosts, so absolute levels do not compare (this host is the faster, 22.4, 32.5 and 403 us). What carries over is the shape: CF-S2 found opting `scanprobe` worth -5.7 % server CPU and +4.3 % throughput, `pk` and `chain3` unmoved (inside 1.4 %); here A gives -10.1 % CPU (quiet pairs, 0 of 9 dearer) and +9.2 % throughput (median), `pk` -0.2 % and `chain3` +0.8 %. A agrees with CF-S2 in direction and in the cost of opting the other cells (none), and shows a larger gain on this host.

## What the numbers say about the engine

- **The read path pays nothing the host can see.** `select1` and `select8` are level on quiet pairs (-0.12 and -0.16 us, B cheaper in 3 of 5 and 4 of 7 pairs), and the same as DI-S0 measured on the spike. The statement binding is recorded at the bind and copied once onto the outcome, and for a one-relation point read the whole of it is under 0.3 us, which is this host's one-binary floor. DI-R5 (one core pays nothing outside the floor) holds for the point read at `cores = 1`; it is **not demonstrated for the write path**, below.
- **If DI costs anything, it lands on the write path, and the data cannot say.** All three write cells have B dearer: `insert1` +0.42 us, `fk1` +0.47 us, `insert8` +2.36 us on quiet pairs, and 10, 9 and 10 of 12 lifetimes over all of them (a fair coin gives 10 or more of 12 with probability about 0.02 one-sided for each cell, and the three share a code path, so they are not three independent draws). Each is inside its spread (0.67, 1.23, 4.23 us), so DI-Q8 reads "not resolved", not "no cost". `insert1`'s +0.42 us is the one that sits above the 0.27 us floor with five of five pairs agreeing, but five pairs is five pairs. The binding is taken for a write as for a read (every relation a statement names is declared at the bind), but no one split a write's cost into binding, outcome copy and the wider statistics key. The DI-S0 order's "not measured: a write" now has a first number, and it is of the order of one floor at `cores = 1`.
- **The three-relation join is within a floor of A.** `chain3`, the cell whose statement binds three relations, is +0.39 us (+1.2 %) with B dearer in 9 of 12 and all 12 pairs kept; the `pk` cell on the same harness is +0.39 us as well (7 of 10). The binding's width does not show: a three-relation binding costs no more than a one-relation one at this resolution, which is DI-S0's finding for 16 against 1 carried to the landed code.
- **`cores = 8` does not differ from `cores = 1` in kind.** `select8` is level; `insert8` leans the same way as `insert1` with a larger number (+2.36 against +0.42 us) and a wider floor (a spread of 4 us on 55 against 0.7 on 22), because at 8 cores the right edge of one tree is the contended resource and its CPU a statement moves 10 % between identical lifetimes. The size of the `insert8` delta is not a reason to think the cost grows with cores: the cell cannot resolve 2.4 us.
- **The opted Waystone path is intact on B, with the gate that asks with the binding.** `WAYSTONE ON` took on B for every run; `replays=` is 200 on both opted arms and 0 on the not-opted ones; opting `scanprobe` is worth -40.9 us (-10.1 %) of server CPU on A and -49.6 us (-12.1 %) on B, with `B-opt` against `A-opt` at -1.77 us (-0.5 %), inside the spread. `pk` and `chain3` cost the same with an opted map beside them as without. DI-S3's per-binding opted map (one entry a binding) does not show on the cells that are not opted. This is the only place the closing code's change to the Waystone gate was exercised; CF-S2's `chain4` and `control`, the refill after root growth and the mount's cost were not.
- **What this host can and cannot resolve.** Two servers of one binary differ by up to about 0.27 us in 22 over many rounds; the quiet-pair spreads here (0.6-1.4 us at `cores = 1`) say 5 pairs resolve about 1 us, so a delta of +0.4 us is one floor and not distinguishable from zero in spread, whatever its sign count. There is no PMU, so timing is all there is, and other sessions' builds and measurement drivers loaded the host for most of the 96 minutes, which cost two passes and about 57 minutes of hold. The gate samples at a driver's start only; a load that begins mid-driver is found afterwards in the lifetimes' loads, not stopped.

## Not measured

- A write's cost split into the binding, the outcome copy and the wider statistics key (no profile, no PMU); the order of DI's cost between statement kinds beyond the five cells above.
- `chain4`, `control`, `scanprobe` at `cores > 1`, the Waystone refill after root growth and the mount's cost with many opted rows (CF-S2 cut them, DI-S5's row named only the opted and not-opted arms).
- Row counts other than 10,000 (driver 1) and 20,000 / 2,000 (driver 2); a point lookup and a keyed insert do not scale with rows, the claim is argued not measured.
- `sys.statement_stats`'s read cost with the wider key, memory of the part, and a shape whose binding is over the 16-relation cap.
- Anything on a loaded host: the first pass is archived but not analysed beyond its set-aside reason.

## Archive

`bench/v3.4.15/archive/di-close-v3.4.15-12-g871845e4/`: `build.sh`, `measure.sh`, `measure2.sh`, `measure3.sh`, `measure4.sh` (the pipeline as run), `quietwait.py`, `di_arms.py` (driver 1), `cf/` (driver 2: `cf_di.py`, `cf_s0.py` with the one-line change, and the unused `cf_s2.py`, `compare.py`, `run.sh` it came with), `raw1.jsonl` (driver 1, the 120 lifetimes), `raw1-contaminated-first-pass.jsonl` (set aside), `run2/` (48 run files), `analyze.py` and its output `analysis.txt` (run as `python3 -I analyze.py raw1.jsonl run2`), `sha256`, `logs/` (build logs, binary mtimes, gate files, `pipeline.status`, status files), `example-*.conf`.
