# DH-S1 spike: what a packed image would buy in throughput, priced with a stand-in

**Finding.** A packed image that logs 0.38 of a page at 12 us (the LZ4 class of
DH-S0's census) or 0.21 of a page at 32 us (zstd-1 class) recovers **44 to 55 % of the
image's throughput cost where the cost is large** (one core, a table three times the
pool: B is 28.3 % under A0, P1 is 15.8 % and P2 12.8 % under it, outside the floor; **5 of that cell's 8 runs had another session's `kds_server_base` running beside them** (section 1), kept, and the two rounds still agree within the stated floor and B's -28.3 % matches DH-S0's -28.1 %, which had no such neighbour in its scans),
**about a third to a half at eight cores and at one core** (cell 3 is a single run; cell 7
is outside its 0.5 % floor), and **an amount that cannot be told from nothing at four
cores / 100K rows**, where B's own loss (3 to 8 %) is the size of the run-to-run floor.
**No P arm reaches A0 anywhere**: the log-volume half of the price is bought, the half
that is not log bytes (a fixed copy per image, the exclusive hold, the CPU of a packer)
is not, and the 10 to 26 us spin lands on it. **P is a stand-in**: it prices a shorter
sync and a fixed CPU spin. It is not a packer, not the unpack at redo, and not a log
anyone can recover (redo refuses the short payload), so its numbers bound what the format
change can buy and say nothing about whether it is correct.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-20, 01:12 to 01:30 UTC |
| Branch / commit | `worktree-dh-page-image-volume`, `f4138860`, `git describe --tags` = `v3.4.15-1-gf4138860`; the worktree's tools (`tools/dh_s0.py` extended with `envx=` per-run environment and `--keep-stderr`, new `tools/dh_s1_report.py`) are uncommitted; no engine file touched |
| Source measured | the worktree's `git archive f4138860` extracted to `/home/louis/bench-runs/dh-s1/src` **plus both measurement-only patches**, archived beside this file: `dh-s0-images-switch.patch` (`KDS_DH_NO_IMAGES`, read once at mount) and `dh-s1-truncated-image-spike.patch` (`KDS_DH_IMAGE_BYTES=N`: the image record carries only the page's first N bytes; `KDS_DH_IMAGE_SPIN_US=M`: M us burned under the page's exclusive hold before the append). `<cstdlib>` and `<chrono>` were added to `manager.hpp` and `<cstdlib>` to `expeditor.cpp` by `build.sh`. Neither arm is main's exact binary; with nothing set it behaves as main |
| Binary | Release (`-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`), built 01:13:49 UTC (87 s); the copy run, `bin/kds_server_dh1`, mtime 01:13:50 (newer than the commit, committed 00:32:04), `sha256 7f2acdea01a487dc83d5b9b7ed1f1ba3cc6254bb7c3ba1327dfac4fedb61a796`; every server started from the copy |
| Device | `/home/louis/bench-runs/dh-s1/` on `/dev/nvme0n1p1`, ext4 (data files, WAL, the copies of DH-S0's snapshots); never tmpfs |
| Snapshots | DH-S0's snapshots (`/home/louis/bench-runs/dh-s0/wd/snap`, loaded by an A0 run of the earlier binary, stopped cleanly) are read-only and `cp -r`'d for each run; **every run's directory, which a P arm leaves unrecoverable, was deleted after the run and none was mounted again**; the server's stderr was copied out first |
| Server | `cores` as per cell, `durability = group`, `buffer_pool_frames = 65536` (2,048 in the pressure cell), `log_level = warn`, everything else default; port 17432; 4 clients (8 in cell 3), 3 s warm-up, **15 s** window |
| Arms | **A0** `KDS_DH_NO_IMAGES=1`; **B** nothing set; **P1** `KDS_DH_IMAGE_BYTES=3104`, `KDS_DH_IMAGE_SPIN_US=12` (pressure cell: 2592 bytes, 10 us); **P2** 1744 bytes, 32 us (pressure cell: 1048 bytes, 26 us). The figures are DH-S0's census means, **taken from cell 1 for every 100K-row cell**: P1's 3,104 B and 12 us are cell 1's weighted LZ4 mean (3,095 B, 11.7 us), P2's 1,744 B and 32 us its zstd-1 (1,743 B, 32.2 us). They were used unchanged in **cell 3**, whose own census says LZ4 3,433 B and 13.3 us, zstd-1 2,121 B and 33.9 us, so cell 3's P arms log 10 % and 18 % fewer bytes an image than their own mix would (about 43 and 50 B/stmt of 657 and 483 B/stmt: flattering to P) and spin 1 to 2 us less; and in **cell 7**, which has no census at all (the same table, the same page types, unmeasured). The pressure cell's figures are cell 5's (LZ4 2,585 B, 9.5 us; zstd-1 1,042 B, 25.8 us) and cell 2's (2,586 B, 9.4 us; 1,045 B, 26.1 us) |
| Quiet gate | load 0.24 at the start of DH-S0; this stage's kept runs read `loadavg_before` 1.55 to 7.15 (the first cell-5 round 7.15, 6.11, 5.09, 4.25 right after the build; the rest 1.55 to 3.42). **Foreign processes beside kept runs** (the top-5 `ps` scan taken just before each run, `procscan_before` in `runs.jsonl`; `ps` %CPU is a lifetime average, so whether a neighbour persisted through the 15 s window is not known): **another session's `kds_server_base`** (pids 3237011 at 70.9 and 73.5 %, 3238400 at 49.9 to 68.1 %) was present before **d5r1 A0 and B, d5r2 B, P1 and P2 (5 of cell 5's 8 runs; B in both rounds, A0 and P1 and P2 each in one) and d1r1 A0 and B (2 of cell 1's 12 runs)**, and an `ld` at 99.8 % (a link finishing) before d1r3 P2. **These runs were kept and not re-run.** What it does to the claims: B had the neighbour in both its cell-5 runs and A0 in one of two, so if the neighbour costs throughput B's loss (-28.3 %) and the recoveries are overstated; against that, within the arms the with-neighbour and without-neighbour runs differ in no consistent direction (A0 3,611 with against 3,498 without; P1 2,950 with against 3,039 without; P2 3,058 with against 3,141 without), the two-round spread (2.2 to 3.2 %) holds, and DH-S0's cell 5 (3,582 and 2,576 stmt/s, its scans showing no foreign process) agrees with this file's A0 and B within 1 %. In cell 1, d1r1 B (4,334 stmt/s) is the lowest of its three B runs and d1r1 A0 (5,006 stmt/s) agrees with d1r3's (5,068 stmt/s), so a neighbour's cost, if any, is at most a few percent there and adds to B's spread. **A concurrent compile by another session then started inside the cell-1 round-2 block (about 01:17 to 01:21)** and contaminated `d1r2` (B, P1, P2), `d3` and `d7r1`: those 10 runs are discarded, archived in `contaminated-runs.jsonl`, and re-run once `pgrep -x` showed no compiler for 30 consecutive seconds (`quiet.sh`). **Hold: 01:21:01 to 01:24:25 (3.4 min), outside the budget.** **No per-run `pgrep` scan was taken** (house rule 3 asks `pgrep cc1plus` before a measurement): the only scan per run is the `ps` top-5 above, and the compiler was found by reading the load average and the `ps` scans (`cc1plus` in the scans from d1r2 P1 on) after the contaminated runs had already been taken |
| Budget | 20 minutes for build plus runs. Spent: build 1.4 min; runs about 14 min including 3 min of discarded runs; plus the gate hold above. One extra round of cell 1 (`d1r3`, 1.3 min) was added because round 2's A0 was an outlier |
| Tests | no engine code changed (the patches live only in the extraction); the suite was not run for this measurement |
| Raw | `bench/v3.4.15/archive/dh-s1-packed-image-spike-v3.4.15-1-gf4138860/` (`runs.jsonl`, `contaminated-runs.jsonl`, `stderr/`, `plan.txt`, patches, `sha256`, `progress.log`, `tables.md`). The server's own warn-level log was in each run directory and was **not** copied out (`tools/dh_s0.py` now copies it); `stderr/` holds only the start-up and shutdown lines |

## 2. Throughput and the gap each arm recovers

`gap recovered = (arm - B) / (A0 - B)`: 0 % is B, 100 % is A0. The floor is the largest
two-or-three-run spread of any arm in the cell; "outside" means the arm's difference from B is
larger than that floor. Statements a second, client side, four clients unless the cell says.

| cell | A0 | B | P1 | P2 | B vs A0 | P1 gap recovered | P2 gap recovered | floor (spread of repeated runs) | recovery outside the floor? |
|---|---|---|---|---|---|---|---|---|---|
| **5**: 1M rows, pool 2,048 frames, 1 core | 3,554 stmt/s (3,611; 3,498 stmt/s) | 2,550 stmt/s (2,577; 2,522 stmt/s) | 2,995 stmt/s (3,039; 2,950 stmt/s) | 3,099 stmt/s (3,141; 3,058 stmt/s) | -28.3 % | **44 %** of the gap (+17.4 % vs B) | **55 %** of the gap (+21.6 % vs B) | A0 3.2 %, B 2.2 %, P1 3.0 %, P2 2.7 % | **yes, both** (5 times the floor), with the foreign-load caveat of section 1 |
| **1**: 100K rows, 4 cores, group | 4,739 stmt/s (5,006; 4,142; 5,068 stmt/s) | 4,492 stmt/s (4,334; 4,505; 4,635 stmt/s) | 4,693 stmt/s (4,656; 4,634; 4,789 stmt/s) | 4,638 stmt/s (4,741; 4,608; 4,566 stmt/s) | -5.2 % | 82 % of the gap (+4.5 % vs B) | 59 % of the gap (+3.3 % vs B) | A0 19.5 % (one outlier), B 6.7 %, P1 3.3 %, P2 3.8 % | **no** (B's own loss is inside A0's floor) |
| 1, A0's outlier (4,142 stmt/s, only 7 checkpoint runs, started the second after the gate released) left out | 5,037 stmt/s | 4,492 stmt/s | 4,693 stmt/s | 4,638 stmt/s | -10.8 % | 37 % of the gap | 27 % of the gap | B 6.7 % | no: +4.5 % and +3.3 % against B's 6.7 % |
| **3**: 100K rows, 8 cores x 8 clients | 8,961 stmt/s | 7,713 stmt/s | 8,246 stmt/s | 8,129 stmt/s | -13.9 % | **43 %** of the gap (+6.9 % vs B) | **33 %** of the gap (+5.4 % vs B) | single run per arm: none | not established (single run) |
| **7**: 100K rows, 1 core | 4,140 stmt/s (4,129; 4,150 stmt/s) | 3,896 stmt/s (3,899; 3,892 stmt/s) | 4,002 stmt/s (4,010; 3,994 stmt/s) | not run | **-5.9 %** | **44 %** of the gap (+2.7 % vs B) | n/a | A0 0.5 %, B 0.2 %, P1 0.4 % | **yes** (P1 +2.7 % against 0.5 %) |

Reading it:

- **Cell 5, the pressure cell, is the clearest result, not a clean one.** Two rounds, floors
  of 2 to 3 %, every arm ordered the same way in both rounds (A0 > P2 > P1 > B); but 5 of
  its 8 runs (d5r1 A0 and B, d5r2 B, P1, P2) had another session's `kds_server_base` beside
  them (section 1). B had it in both of its runs, so B's -28.3 % and the 44 % and 55 %
  recoveries are not shielded from it; the arms' own with and without pairs show no
  consistent direction, and B's -28.3 % equals DH-S0's -28.1 % taken without such a
  neighbour. The ordering and the size are therefore believed; the third digit is not. P1 cuts the log from 6,910 to 2,369
  B a statement (66 %) and recovers 44 % of the throughput; P2 cuts it to 1,122 B (84 %)
  and recovers 55 %. The remaining 45 to 56 % is not log volume: P2's log is within 873 B of
  A0's yet its statement is 207 us (mean p50 1,205 against 998) slower.
- **Cell 1's A0 is not a usable reference.** Three A0 runs read 5,006, 4,142 and 5,068; the
  second had 7 checkpoint runs in the window against 11 in every other cell-1 run and
  started within a second of the quiet gate releasing, and I do not know why it is low. With
  it, cell 1's floor is 19.5 % and no conclusion survives; without it, B is 10.8 % under A0
  (DH-S0 read 8.2 % at this cell) and P1 and P2 recover 37 % and 27 %, which are +4.5 % and
  +3.3 % over B against B's own 6.7 % spread across three rounds. **At four cores and 100K
  rows the recovery is not outside the floor.**
- **Cell 3 is a single run per arm** and has no floor. The order (A0 > P1 > P2 > B) and
  sizes (43 % and 33 %) are those of cell 1's unambiguous rounds, and its A0 and B (8,961;
  7,713) match DH-S0's (8,980; 7,836) within 1.6 %, which is evidence that the host
  was the same; it is not a floor.
- **Cell 7 answers the DH-R4 question for B**: the one-core 100K cell loses **5.9 %** to the
  image (3,896 against 4,140; two rounds, spreads 0.2 % and 0.5 %), outside the floor;
  DH-S0's single run read -5.5 %. P1 recovers 44 % of it, +2.7 % against 0.5 %: outside the
  floor. The one-core cell therefore does pay, by about 250 B of log and 0.03 images a
  statement.
- **P2 is not better than P1 at 4 and 8 cores** (cell 1: 4,638 against 4,693; cell 3: 8,129
  against 8,246), though its log is 25 % shorter (443 to 483 against 596 to 657 B a
  statement): the 20 us extra spin offsets the 150 to 170 B of log it saves. In the
  pressure cell, where the log is 2,369 against 1,122 B and the images are 0.81 a statement,
  the shorter log wins by 55 against 44 %.

**Previous number for each cell** (DH-S0, `results-dh-s0-premise-v3.4.15-1-gf4138860.md`,
same binary family, a different session; a cross-reading, not a delta, as the host's
absolute level drifts by a few percent between sessions):

| cell | DH-S0 A0 / B | this file A0 / B |
|---|---|---|
| 5 | 3,582 / 2,576 stmt/s (-28.1 %) | 3,554 / 2,550 stmt/s (-28.3 %) |
| 1 | 5,065 / 4,649 stmt/s (-8.2 %) | 4,739 / 4,492 stmt/s (-5.2 %); without the outlier 5,037 / 4,492 stmt/s |
| 3 | 8,980 / 7,836 stmt/s (-12.7 %) | 8,961 / 7,713 stmt/s (-13.9 %) |
| 7 | 4,298 / 4,064 stmt/s (-5.5 %) | 4,140 / 3,896 stmt/s (-5.9 %) |

There is no prior number for the P arms (new) and PostgreSQL has no twin for this shape.

## 3. Where the wait goes (flush wait and log bytes a statement)

Microseconds a statement of `commit.wal_flush` (the `sys.wait_stats` total over the window
divided by its statements), the other waits beside it, and the log written a statement,
means of the kept runs:

| cell | arm | log written | images | `commit.wal_flush` | `sched.run_queue` | `cpu.on_cpu` | `io.page_read` | server CPU | stmt p50 |
|---|---|---|---|---|---|---|---|---|---|
| 5 | A0 | 249 B/stmt | 0 img/stmt | 512 us/stmt | 77.4 us/stmt | 21.4 us/stmt | 3.9 us/stmt | 60.7 us/stmt | 998 us |
| 5 | B | 6,910 B/stmt | 0.810 img/stmt | 752 us/stmt | 69.4 us/stmt | 24.4 us/stmt | 4.1 us/stmt | 75.2 us/stmt | 1,383 us |
| 5 | P1 | 2,369 B/stmt | 0.808 img/stmt | 609 us/stmt | 64.8 us/stmt | 30.4 us/stmt | 3.9 us/stmt | 73.7 us/stmt | 1,215 us |
| 5 | P2 | 1,122 B/stmt | 0.808 img/stmt | 571 us/stmt | 71.1 us/stmt | 43.1 us/stmt | 3.9 us/stmt | 84.2 us/stmt | 1,205 us |
| 1 | A0 | 250 B/stmt | 0 img/stmt | 710 us/stmt (668; 828; 635 us/stmt) | 0.9 us/stmt | 34.4 us/stmt | 0 us/stmt | 98.0 us/stmt | 759 us |
| 1 | B | 1,196 B/stmt | 0.115 img/stmt | 741 us/stmt (775; 739; 708 us/stmt) | 1.0 us/stmt | 38.9 us/stmt | 0 us/stmt | 108.7 us/stmt | 838 us |
| 1 | P1 | 612 B/stmt | 0.116 img/stmt | 700 us/stmt (705; 717; 678 us/stmt) | 0.9 us/stmt | 44.5 us/stmt | 0 us/stmt | 108.8 us/stmt | 811 us |
| 1 | P2 | 450 B/stmt | 0.113 img/stmt | 710 us/stmt (696; 722; 713 us/stmt) | 2.0 us/stmt | 41.7 us/stmt | 0 us/stmt | 108.8 us/stmt | 824 us |
| 3 | A0 / B / P1 / P2 | 250 / 1,381 / 657 / 483 B/stmt | 0 / 0.138 / 0.130 / 0.132 img/stmt | 714 / 843 / 779 / 783 us/stmt | 1.3 / 1.1 / 1.2 / 1.0 us/stmt | 36.4 / 42.4 / 42.7 / 51.0 us/stmt | 0 us/stmt | 96.3 / 106.6 / 107.1 / 112.2 us/stmt | 858 / 945 / 883 / 930 us |
| 7 | A0 / B / P1 | 249 / 510 / 346 B/stmt | 0 / 0.032 / 0.031 img/stmt | 441 / 472 / 459 us/stmt | 38.4 / 38.1 / 38.9 us/stmt | 18.8 / 18.6 / 19.1 us/stmt | 0 us/stmt | 49.2 / 49.9 / 50.1 us/stmt | 944 / 1,000 / 974 us |

- **In the pressure cell the flush wait is 60 % of the loss and it moves with the log:** B's
  extra wait over A0 is 240 us (512 to 752); P1 returns 143 us of it (59 %), P2 181 us
  (75 %). Log bytes cut 66 % and 84 %. The throughput recovery (44 % and 55 %) is less than the
  wait recovery because the CPU grows: server CPU a statement goes 60.7 (A0), 75.2 (B), 73.7
  (P1), 84.2 (P2), i.e. P2's 26 us spin on 0.81 images a statement is 21 us a statement that
  the smaller copy only partly returns (B to P1 CPU is **down** 1.5 us although P1 spins
  8 us an image-statement: the shorter record saves about 10 us of copy and checksum work).
  A real packer's CPU is the spin plus its own copy, not the spin alone. **The spin is the
  stand-in's weakest part**: it does not model memory traffic or the packer's effect on the
  page's cache lines.
- **Waits at four and eight cores** are 82 to 90 % `commit.wal_flush` as in DH-S0; the P arms
  return 0 to 64 us/stmt of the +31 to +129 us/stmt B adds over A0, within cell 1's run-to-run range of
  the A0 wait itself (635 to 828 us across three runs). `sched.run_queue`, `io.page_read`
  and `wal_ring_full` (0 in every run) do not move.
- **Client round trip and socket wait** are inside the p50 and were not separated; lock waits
  were nil.

## 4. What the run teaches

- **The format change's ceiling, where it matters, is about half of the price, and it is
  paid for in CPU.** Cell 5: 44 to 55 %; cell 7: 44 %; cell 3: 33 to 43 % (single). In every
  cell P1 and P2 land between B and A0 and never reach A0. The image's cost is therefore
  roughly half sync length (what packing buys) and half the fixed work of producing, copying
  and holding the record (what it does not). That is an inference from five cells with a
  spin stand-in; it assumes the fixed work does not shrink with a real packer, which the CPU
  rows above show it partly does.
- **On the update workloads at four cores the gain cannot be separated from noise at this
  sample size**, and B's own loss there is 5 to 11 % depending on whether A0's outlier
  counts. A decision to build on that cell would need more rounds. The cell where a packed
  image earns its format event, the pressure cell, is also the cell where DH-S0 found only
  L2 acts at all.
- **A heavier packer does not pay at four or eight cores** (P2 does not beat P1 there): the
  value of squeezing from 0.38 to 0.21 of a page is gone at 20 us of extra spin when the
  log's share of the statement is small; it pays where the log per statement is 2 to 7 KB.
  This weakly supports LZ4-class over zstd-class for a build, on evidence that is the spin's.
- **DH-R4 for B itself:** one core at 100K rows pays 5.9 % (two rounds, outside the floor);
  L3 cannot touch it, a packed image would recover about 44 % of it.
- **Whether a packed image changes the p99** was not measured beyond the table's percentiles;
  the exclusive hold is longer by the spin, yet P2's p99 is within or below B's range
  (cell 1: 1,427 to 1,572 us against B's 1,401 to 1,745; cell 5: 2,135 and 2,182 against
  2,310 and 2,381), so no tail growth shows at these rates.

## 5. Caveats and what was not measured

- **P is a stand-in**: a shortened record plus a fixed busy-wait under the page's exclusive
  hold. It prices the sync volume and a constant CPU, not a real packer (whose time varies
  with the page), not the **unpack at redo** (recovery time with packed images is not
  priced), not the format event (DH-R3), and not a recoverable log: the spike logs were
  deleted and never mounted. The byte and microsecond figures are DH-S0's census means for
  one table of three small integers, cell 1's figures reused for cells 3 and 7 (section 1); a
  table whose leaves compress worse gets less.
- Cell 3 and cell 7's P2 arm are single or absent; cell 3 has no floor of its own.
- `bad` statements (tag mismatch) were 1 in `d1r1` A0 and B, 1 in `d3` B and P1, 0 elsewhere.
  **They are not explained**: the servers' stderr (kept this time) holds only the four
  start-up and shutdown lines, and the server's own warn-level log was in each run's
  directory, which the driver deletes; it was not copied out. This repeats DH-S0's unexplained
  `bad`.
- The extra round `d1r3` was added after seeing round 2; all three rounds are reported.
- Cell 5's first round ran at a host load of 4 to 7 right after the build, and 5 of the cell's
  8 runs had another session's `kds_server_base` beside them (section 1); its A0 (3,611 stmt/s)
  is 3 % above round 2's (3,498 stmt/s), inside the cell's floor. They were not re-run.
- Not run: the row-set sweep (200, 1K, 10K), `strict`, scenario 2, recovery time, a
  P2 arm in cell 7, rounds beyond the above.

## Appendix: every kept run

Every cell carries its unit. Columns as in DH-S0's appendix (client-side latencies; this
driver records p90, not p95). **`ckpt runs` is the server's counter
`contention_checkpoint_runs` over the window**, recomputed from the raw JSON (the driver's
first version kept `sys.checkpoints` rows above the window-start maximum `seq`, which
counts per core; for every run in this file that row count equals the counter, so no
number above changed). `bad` is the count of statements whose reply did not carry the
expected tag; the reasons are not recoverable (the server's own log was not copied out of
the deleted run directories).

| cell | arm | runs | mean | spread of runs | vs A0 | gap recovered, (arm - B) / (A0 - B) | outside the floor? |
|---|---|---|---|---|---|---|---|
| d5 | A0 | 3,611 stmt/s, 3,498 stmt/s | 3,554 stmt/s | 3.2 % | +0.0 % | - | - |
| d5 | B | 2,577 stmt/s, 2,522 stmt/s | 2,550 stmt/s | 2.2 % | -28.3 % | - | - |
| d5 | P1 | 3,039 stmt/s, 2,950 stmt/s | 2,995 stmt/s | 3.0 % | -15.8 % | 44 % of the gap | yes (delta vs B +17.4 %, floor 3.2 %) |
| d5 | P2 | 3,141 stmt/s, 3,058 stmt/s | 3,099 stmt/s | 2.7 % | -12.8 % | 55 % of the gap | yes (delta vs B +21.6 %, floor 3.2 %) |
| d1 | A0 | 5,006 stmt/s, 4,142 stmt/s, 5,068 stmt/s | 4,739 stmt/s | 19.5 % | +0.0 % | - | - |
| d1 | B | 4,334 stmt/s, 4,505 stmt/s, 4,635 stmt/s | 4,492 stmt/s | 6.7 % | -5.2 % | - | - |
| d1 | P1 | 4,656 stmt/s, 4,634 stmt/s, 4,789 stmt/s | 4,693 stmt/s | 3.3 % | -1.0 % | 82 % of the gap | no (delta vs B +4.5 %, floor 19.5 %) |
| d1 | P2 | 4,741 stmt/s, 4,608 stmt/s, 4,566 stmt/s | 4,638 stmt/s | 3.8 % | -2.1 % | 59 % of the gap | no (delta vs B +3.3 %, floor 19.5 %) |
| d3 | A0 | 8,961 stmt/s | 8,961 stmt/s | single run | +0.0 % | - | - |
| d3 | B | 7,713 stmt/s | 7,713 stmt/s | single run | -13.9 % | - | - |
| d3 | P1 | 8,246 stmt/s | 8,246 stmt/s | single run | -8.0 % | 43 % of the gap | single run (delta vs B +6.9 %, floor 0.0 %) |
| d3 | P2 | 8,129 stmt/s | 8,129 stmt/s | single run | -9.3 % | 33 % of the gap | single run (delta vs B +5.4 %, floor 0.0 %) |
| d7 | A0 | 4,129 stmt/s, 4,150 stmt/s | 4,140 stmt/s | 0.5 % | +0.0 % | - | - |
| d7 | B | 3,899 stmt/s, 3,892 stmt/s | 3,896 stmt/s | 0.2 % | -5.9 % | - | - |
| d7 | P1 | 4,010 stmt/s, 3,994 stmt/s | 4,002 stmt/s | 0.4 % | -3.3 % | 44 % of the gap | yes (delta vs B +2.7 %, floor 0.5 %) |

| run | arm | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | wal_flush wait | run_queue wait | on_cpu | page_read wait | ckpt runs (counter) | ckpt pages written | pages drained | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| d5r1 | A0 | 3,611 stmt/s | 541 us | 899 us | 996 us | 1,244 us | 1,963 us | 59.8 us/stmt | 249 B/stmt | 0.000 img/stmt | 505.1 us/stmt | 76.4 us/stmt | 21.2 us/stmt | 3.9 us/stmt | 3 runs | 3,514 pages | 40,054 pages | 0 stmts |
| d5r1 | B | 2,577 stmt/s | 676 us | 1,248 us | 1,368 us | 1,669 us | 2,310 us | 74.5 us/stmt | 6,905 B/stmt | 0.809 img/stmt | 738.8 us/stmt | 69.6 us/stmt | 24.2 us/stmt | 4.0 us/stmt | 3 runs | 3,550 pages | 27,713 pages | 0 stmts |
| d5r1 | P1 | 3,039 stmt/s | 669 us | 1,095 us | 1,205 us | 1,485 us | 2,137 us | 73.3 us/stmt | 2,367 B/stmt | 0.807 img/stmt | 599.4 us/stmt | 62.7 us/stmt | 30.3 us/stmt | 3.9 us/stmt | 3 runs | 3,553 pages | 33,215 pages | 0 stmts |
| d5r1 | P2 | 3,141 stmt/s | 603 us | 1,073 us | 1,188 us | 1,496 us | 2,135 us | 83.8 us/stmt | 1,122 B/stmt | 0.808 img/stmt | 564.5 us/stmt | 71.9 us/stmt | 42.7 us/stmt | 3.9 us/stmt | 3 runs | 3,595 pages | 34,435 pages | 0 stmts |
| d5r2 | A0 | 3,498 stmt/s | 517 us | 903 us | 1,000 us | 1,246 us | 1,974 us | 61.5 us/stmt | 249 B/stmt | 0.000 img/stmt | 517.8 us/stmt | 78.3 us/stmt | 21.6 us/stmt | 3.8 us/stmt | 3 runs | 3,531 pages | 38,733 pages | 0 stmts |
| d5r2 | B | 2,522 stmt/s | 796 us | 1,271 us | 1,398 us | 1,709 us | 2,381 us | 75.9 us/stmt | 6,915 B/stmt | 0.810 img/stmt | 764.4 us/stmt | 69.2 us/stmt | 24.5 us/stmt | 4.1 us/stmt | 3 runs | 3,588 pages | 27,070 pages | 0 stmts |
| d5r2 | P1 | 2,950 stmt/s | 660 us | 1,112 us | 1,224 us | 1,521 us | 2,295 us | 74.1 us/stmt | 2,371 B/stmt | 0.808 img/stmt | 618.8 us/stmt | 66.8 us/stmt | 30.4 us/stmt | 3.9 us/stmt | 3 runs | 3,584 pages | 32,165 pages | 0 stmts |
| d5r2 | P2 | 3,058 stmt/s | 553 us | 1,111 us | 1,222 us | 1,515 us | 2,182 us | 84.6 us/stmt | 1,122 B/stmt | 0.808 img/stmt | 576.7 us/stmt | 70.3 us/stmt | 43.4 us/stmt | 3.9 us/stmt | 3 runs | 3,579 pages | 33,478 pages | 0 stmts |
| d1r1 | A0 | 5,006 stmt/s | 172 us | 675 us | 762 us | 971 us | 1,360 us | 87.0 us/stmt | 250 B/stmt | 0.000 img/stmt | 667.7 us/stmt | 1.0 us/stmt | 30.1 us/stmt | 0.0 us/stmt | 11 runs | 7,848 pages | 0 pages | 1 stmts |
| d1r1 | B | 4,334 stmt/s | 200 us | 754 us | 866 us | 1,144 us | 1,745 us | 106.3 us/stmt | 1,202 B/stmt | 0.116 img/stmt | 775.4 us/stmt | 1.0 us/stmt | 35.5 us/stmt | 0.0 us/stmt | 11 runs | 7,648 pages | 0 pages | 1 stmts |
| d1r1 | P1 | 4,656 stmt/s | 406 us | 712 us | 813 us | 1,072 us | 1,581 us | 103.4 us/stmt | 625 B/stmt | 0.120 img/stmt | 705.4 us/stmt | 1.0 us/stmt | 45.0 us/stmt | 0.0 us/stmt | 11 runs | 8,492 pages | 0 pages | 0 stmts |
| d1r1 | P2 | 4,741 stmt/s | 353 us | 713 us | 811 us | 1,049 us | 1,427 us | 104.9 us/stmt | 443 B/stmt | 0.109 img/stmt | 695.6 us/stmt | 1.1 us/stmt | 41.4 us/stmt | 0.0 us/stmt | 11 runs | 7,901 pages | 0 pages | 0 stmts |
| d1r2 | A0 | 4,142 stmt/s | 355 us | 675 us | 757 us | 963 us | 1,281 us | 99.6 us/stmt | 249 B/stmt | 0.000 img/stmt | 828.2 us/stmt | 0.8 us/stmt | 31.5 us/stmt | 0.0 us/stmt | 7 runs | 4,947 pages | 0 pages | 0 stmts |
| d1r2 | B | 4,505 stmt/s | 375 us | 730 us | 833 us | 1,089 us | 1,506 us | 106.8 us/stmt | 1,196 B/stmt | 0.115 img/stmt | 738.8 us/stmt | 1.0 us/stmt | 40.2 us/stmt | 0.0 us/stmt | 11 runs | 7,935 pages | 0 pages | 0 stmts |
| d1r2 | P1 | 4,634 stmt/s | 373 us | 706 us | 811 us | 1,071 us | 1,572 us | 106.9 us/stmt | 615 B/stmt | 0.117 img/stmt | 716.5 us/stmt | 0.8 us/stmt | 46.1 us/stmt | 0.0 us/stmt | 11 runs | 8,225 pages | 0 pages | 0 stmts |
| d1r2 | P2 | 4,608 stmt/s | 414 us | 736 us | 834 us | 1,080 us | 1,570 us | 104.5 us/stmt | 452 B/stmt | 0.114 img/stmt | 721.6 us/stmt | 1.0 us/stmt | 38.2 us/stmt | 0.0 us/stmt | 11 runs | 8,012 pages | 0 pages | 0 stmts |
| d3 | A0 | 8,961 stmt/s | 388 us | 755 us | 858 us | 1,111 us | 1,624 us | 96.3 us/stmt | 250 B/stmt | 0.000 img/stmt | 714.4 us/stmt | 1.3 us/stmt | 36.4 us/stmt | 0.0 us/stmt | 21 runs | 16,156 pages | 0 pages | 0 stmts |
| d3 | B | 7,713 stmt/s | 142 us | 816 us | 945 us | 1,309 us | 1,860 us | 106.6 us/stmt | 1,381 B/stmt | 0.138 img/stmt | 842.8 us/stmt | 1.1 us/stmt | 42.4 us/stmt | 0.0 us/stmt | 21 runs | 16,198 pages | 0 pages | 1 stmts |
| d3 | P1 | 8,246 stmt/s | 153 us | 761 us | 883 us | 1,186 us | 1,819 us | 107.1 us/stmt | 657 B/stmt | 0.130 img/stmt | 779.4 us/stmt | 1.2 us/stmt | 42.7 us/stmt | 0.0 us/stmt | 21 runs | 16,288 pages | 0 pages | 1 stmts |
| d3 | P2 | 8,129 stmt/s | 404 us | 804 us | 930 us | 1,234 us | 1,743 us | 112.2 us/stmt | 483 B/stmt | 0.132 img/stmt | 782.9 us/stmt | 1.0 us/stmt | 51.0 us/stmt | 0.0 us/stmt | 21 runs | 16,342 pages | 0 pages | 0 stmts |
| d7r1 | A0 | 4,129 stmt/s | 467 us | 842 us | 945 us | 1,181 us | 1,523 us | 49.6 us/stmt | 249 B/stmt | 0.000 img/stmt | 440.6 us/stmt | 38.3 us/stmt | 19.0 us/stmt | 0.0 us/stmt | 3 runs | 1,860 pages | 0 pages | 0 stmts |
| d7r1 | B | 3,899 stmt/s | 516 us | 897 us | 1,001 us | 1,249 us | 1,615 us | 49.9 us/stmt | 510 B/stmt | 0.032 img/stmt | 469.3 us/stmt | 37.7 us/stmt | 18.7 us/stmt | 0.0 us/stmt | 3 runs | 1,862 pages | 0 pages | 0 stmts |
| d7r1 | P1 | 4,010 stmt/s | 493 us | 867 us | 970 us | 1,224 us | 1,572 us | 49.9 us/stmt | 345 B/stmt | 0.031 img/stmt | 455.7 us/stmt | 38.3 us/stmt | 19.1 us/stmt | 0.0 us/stmt | 3 runs | 1,856 pages | 0 pages | 0 stmts |
| d7r2 | A0 | 4,150 stmt/s | 506 us | 842 us | 942 us | 1,170 us | 1,463 us | 48.8 us/stmt | 249 B/stmt | 0.000 img/stmt | 442.0 us/stmt | 38.4 us/stmt | 18.6 us/stmt | 0.0 us/stmt | 3 runs | 1,866 pages | 0 pages | 0 stmts |
| d7r2 | B | 3,892 stmt/s | 480 us | 894 us | 999 us | 1,256 us | 1,646 us | 49.8 us/stmt | 511 B/stmt | 0.032 img/stmt | 475.2 us/stmt | 38.4 us/stmt | 18.5 us/stmt | 0.0 us/stmt | 3 runs | 1,865 pages | 0 pages | 0 stmts |
| d7r2 | P1 | 3,994 stmt/s | 468 us | 871 us | 978 us | 1,228 us | 1,575 us | 50.2 us/stmt | 346 B/stmt | 0.031 img/stmt | 461.4 us/stmt | 39.4 us/stmt | 19.1 us/stmt | 0.0 us/stmt | 3 runs | 1,855 pages | 0 pages | 0 stmts |
| d1r3 | A0 | 5,068 stmt/s | 387 us | 666 us | 757 us | 974 us | 1,334 us | 107.5 us/stmt | 250 B/stmt | 0.000 img/stmt | 635.2 us/stmt | 0.9 us/stmt | 41.5 us/stmt | 0.0 us/stmt | 11 runs | 8,121 pages | 0 pages | 0 stmts |
| d1r3 | B | 4,635 stmt/s | 369 us | 708 us | 816 us | 1,072 us | 1,401 us | 112.9 us/stmt | 1,191 B/stmt | 0.114 img/stmt | 707.6 us/stmt | 0.9 us/stmt | 41.0 us/stmt | 0.0 us/stmt | 11 runs | 8,111 pages | 0 pages | 0 stmts |
| d1r3 | P1 | 4,789 stmt/s | 374 us | 709 us | 808 us | 1,033 us | 1,338 us | 116.0 us/stmt | 596 B/stmt | 0.111 img/stmt | 677.9 us/stmt | 0.8 us/stmt | 42.4 us/stmt | 0.0 us/stmt | 11 runs | 8,058 pages | 0 pages | 0 stmts |
| d1r3 | P2 | 4,566 stmt/s | 369 us | 730 us | 828 us | 1,071 us | 1,472 us | 116.9 us/stmt | 454 B/stmt | 0.115 img/stmt | 713.3 us/stmt | 3.9 us/stmt | 45.6 us/stmt | 0.0 us/stmt | 11 runs | 8,049 pages | 0 pages | 0 stmts |
