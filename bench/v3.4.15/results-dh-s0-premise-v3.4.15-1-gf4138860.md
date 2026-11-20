# DH-S0: what the owed page image costs on more workloads, and what three levers would buy back

**Finding.** The image is a cost of sync length, not of CPU, and it is paid
where pages are written often relative to the statement rate. On the
update micro-cells it is 5 to 13 % of throughput at four and eight cores
(100K rows), 28 % at one core with a table three times the pool, and
inside the noise floor on the booking path (scenario 2) at every shape
measured. **Leaving the free hole out of the image (L1) buys nothing** on
the pages these workloads image: a full leaf's longest zero run is 14 to
15 bytes (0.2 to 1 % of the page), because the page's 70 % zero bytes are
scattered high bytes of small integers, not a free region. **Compressing it
(L2) removes 58 to 69 % of the image bytes with LZ4 and 74 to 87 % with
zstd level 1 on this data**, at 9 to 13 us (LZ4) or 26 to 34 us (zstd-1) an
image against about 14 to 18 us an image costs today - but that ratio is a
property of a table of three small integers, not of the engine. **Making the
writeback rate independent of `cores` (L3, configuration upper bound)
removes 79 % (4 cores) and 89 % (8 cores) of the image count where the hot
set fits the pool, and 3.5 % where it does not** (the eviction drain, not
the cadence, makes the copies). The throughput each lever would recover
cannot be projected from these points: flush wait per extra log byte spans
34 to 112 ns across the cells, and the one cell where a lever's
configuration was run (L at four cores) recovers about half of what the
image-byte arithmetic predicts, from single control runs. The bound for
every lever is A0, the same binary with no image debt.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-20, 00:40 to 01:00 UTC (micro and scenario runs 00:40 to 00:56; census runs 00:57 to 00:59; census analysis 01:00) |
| Branch / commit | `worktree-dh-page-image-volume`, `f4138860`, `git describe --tags` = `v3.4.15-1-gf4138860` |
| Source measured | the worktree's `git archive f4138860` extracted to `/home/louis/bench-runs/dh-s0/src` (no worktree file was touched), **plus `dh-s0-images-switch.patch`** (archived beside this file): at mount, `KDS_DH_NO_IMAGES` set in the environment turns `EnableFetchImages` off, else main's behaviour; one `#include <cstdlib>` was needed and added by `build.sh`. The patch executes once, at mount |
| Binary | Release (`-DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`), built 00:40:26 UTC (78 s), mtime 2026-11-20 00:40:48 for the copy that was run; **newer than the commit** (committed 00:32:04 UTC), so it is the engine at `f4138860` and the patch. Run from a copy, `bin/kds_server_dh`, `sha256 ba7a00e1e7fbc6926912af17103d4fca511ffb0d928a79cf07feeddca6e704be`; every server of the run started from that copy |
| Substitution | The work order's A (the last commit before DB's merge, `12887a14`) is replaced by **A0 = the same binary with the image debt never paid**: main since then also carries DA, which moved `cores = 8` throughput by +13.5 %, so a pre-DB binary against main would not isolate the image. **Neither arm is main's exact binary** (one `getenv` at mount); B has the variable unset and is main's behaviour |
| Arms | **A0** `KDS_DH_NO_IMAGES=1`; **B** variable unset; **L** B plus `checkpoint_interval_ms = 5000 * cores` (cores > 1 only): the instance's writeback rate at one core's, a configuration upper bound for L3, nothing built |
| Device | data files, WAL and snapshots under `/home/louis/bench-runs/dh-s0/` on `/dev/nvme0n1p1`, ext4 (`df -T` checked); the binary under the same path; the WAL archive of the census runs is on the same device |
| Host | 8 logical cores, 31 GB; `uptime` load 0.24 at the start; `pgrep cc1plus` was empty **once, at the start of the measurement, and was not repeated per run block**; what exists per run is `/proc/loadavg` (1-minute, which still carries the previous run's tail) and a top-5 `ps` scan by CPU taken just before the run (`loadavg_before`, `procscan_before` in the JSON lines; `ps` %CPU is a process's lifetime average, so a burst can be missed): load 0.7 to 3.4 before a run, and read after the fact **no process other than this driver and its servers is above 20 % CPU in any run's scan** (the scenario runs' scans included). One unrelated idle `kds_server` (pid 560863, web sandbox) was resident and left alone. **No quiet gate hold: nothing else built or ran a suite during the measurement** |
| Server | `buffer_pool_frames = 65536` except the pressure cells (2,048), `durability = group` except cells 6 and `s4c`/`s4d` (`strict`), `log_level = warn`, every other key default (`checkpoint_interval_ms = 5000`), port 17431 |
| Load generator | `tools/dh_s0.py` (new), reusing `tools/db_close.py`'s capture and `tools/db_s0_premise.py`'s workloads; one Python process, one thread a client, closed loop; each run is a `cp -r` of a snapshot (data file and WAL directory, loaded once per table, stopped cleanly), mounted, row count checked, 3 s warm-up (L: 12.5 s, 32.5 s at 8 cores, so its first cadence tick falls inside the window), a **15 s** steady window. Scenario 2 is `tools/scenario2_freight.py` unmodified (`--sync --seed 1 --verify 100`) and loads its own data every run (no snapshot is possible without editing it). Rule 7's `bench/docs/` does not exist at this commit; the drivers' docstrings (`tools/dh_s0.py`, `tools/dh_image_census.py`, `tools/dh_s0_report.py`, `tools/dh_s0_project.py`) carry their flags and the commands |
| Tests | no engine code changed; the suite was not run for this measurement and is not claimed |
| Raw | `bench/v3.4.15/archive/dh-s0-premise-v3.4.15-1-gf4138860/` (`*-runs.jsonl`, scenario JSON and logs, `census/`, `projections.txt`, `tables.md`, plans, patch, `sha256`, `progress.log`). Raw census samples (up to 3,000 pages a cell) are outside the repo in `/home/louis/bench-runs/dh-s0/samples/` (`<cell>.pages`, `<cell>.index`). **The saved sample is not an even stride over the cell's images**: it was drawn from the per-type capped lists and took the first 3,000 by LSN, so undo pages are over-represented against the cell's mix (7 % in the sample against 4.3 % of the images in `k1`, 4.4 % against 0.8 % in `k2`, 17.7 % against 5.4 % in `k3`) and the tail of each log is cut. A later per-type pass has the same shape (below). `tools/dh_image_census.py` has since been fixed (one even stride over all images for the saved sample, an even stride over each type's kept list for the statistics); **the existing samples and census numbers were not re-run** |

**Budget and what was cut.** 20 minutes for build plus runs. Spent: build 1.3 min, runs
about 16 min (one restart after 3 runs to give L its longer warm-up, see section 3),
census re-runs 2 min (the first census attempt read recycled logs, section 5), census
analysis 9 s. The measurement ran at about 19 minutes plus the census runs; no gate hold.
**Cut or not run:** scenario 1 (`result-insert` and load phases; the micro insert cell
`insert:0` stands in); scenario 2 `--bookers 1` at `group` and 8 bookers at `cores = 1`
`strict`; the row-set sweep (200 / 1K / 10K) of rule 9; a second round of every micro
cell but cell 1; the same-warm control for cells 2 and 3; L at `strict` and at `cores = 1`.
The DB-S0 reading of `known-gaps.md` and of the source for L3's begin-record rules and the
drain's selection was **not done**: this file measures, it does not read the checkpointer.

## 2. How to read the arms and the cells

`stmt/s` is client-side statements (bookings) a second over the window. The cells:

| id | shape | rows | pool | cores x clients | durability |
|---|---|---|---|---|---|
| 1 | `update-lit` (point `UPDATE` by literal key, uniform) | 100,000 rows | 65,536 frames | 4 cores x 4 clients | group |
| 2 | `update-lit`, pressure | 1,000,000 rows | **2,048 frames** | 4 cores x 4 clients | group |
| 3 | `update-lit` | 100,000 rows | 65,536 frames | 8 cores x 8 clients | group |
| 5 | `update-lit`, pressure | 1,000,000 rows | 2,048 frames | 1 core x 4 clients | group |
| 6 | `update-lit` | 100,000 rows | 65,536 frames | 4 cores x 4 clients | **strict** |
| 7 | `update-lit` | 100,000 rows | 65,536 frames | 1 core x 4 clients | group |
| 8 | `insert` (named keys, one shared counter) | 0 growing to about 77,000 rows | 65,536 frames | 4 cores x 4 clients | group |
| s4a | scenario 2 booking path, 12,000 bookings, 2,000 organizations, 30 ships, 2,000 operations, 24,000 cargos | 12,000 bookings | 65,536 frames | 8 cores x 8 bookers | group |
| s4b | same | same | 65,536 frames | 1 core x 8 bookers | group |
| s4c | same | same | 65,536 frames | 8 cores x 8 bookers | strict |
| s4d | same | same | 65,536 frames | 1 core x 1 booker | strict |

Cell 4 of the order is `s4a` to `s4d`. Row counts of the 100K and 1M tables are the
`--rows` of the micro driver (a 100,000-row table is about 2,400 leaf pages of 8 KiB at
three `int64` columns; a 1M table about 6,060 pages by DB-S0's count, three times the
pool). A scenario cell's size is fixed by the flags above; it gives a booking window of
7.1 to 21 s (`span s` in the appendix).

## 3. The throughput the image costs

The same binary with and without the image debt, at four and eight cores, loses 5 to 13 %
at 100K rows; one core with a pressured pool loses 28 %; the booking path loses nothing
the floor can see.

| cell | A0 | B | L | B vs A0 | L vs A0 | noise floor (spread of repeated runs) |
|---|---|---|---|---|---|---|
| 1: 100K rows, 4 cores, group | 5,065 stmt/s (5,078; 5,051 stmt/s) | 4,649 stmt/s (4,679; 4,619 stmt/s) | 4,644 stmt/s (4,619; 4,668 stmt/s) | **-8.2 %** | -8.3 % | A0 0.5 %, B 1.3 %, L 1.0 % |
| 1, same-warm control (warm 12.5 s; one run each) | 4,780 stmt/s | 4,522 stmt/s | (L above) | -5.4 % | **-2.8 %** | single runs, none |
| 1, L with no checkpoint in the window (warm 3 s; one run) | - | - | 5,041 stmt/s | - | -0.5 % | single run, none |
| 2: 1M rows, pool 2,048 frames, 4 cores | 2,663 stmt/s | 2,820 stmt/s | 2,528 stmt/s | +5.9 % | -5.1 % | single runs, none |
| 3: 100K rows, 8 cores | 8,980 stmt/s | 7,836 stmt/s | 8,023 stmt/s | **-12.7 %** | -10.7 % | single runs, none |
| 5: 1M rows, pool 2,048 frames, 1 core | 3,582 stmt/s | 2,576 stmt/s | n/a (one core) | **-28.1 %** | n/a | single runs, none |
| 6: 100K rows, 4 cores, **strict** | 4,214 stmt/s | 3,743 stmt/s | not run | -11.2 % | not run | single runs, none |
| 7: 100K rows, 1 core | 4,298 stmt/s | 4,064 stmt/s | n/a (one core) | -5.5 % | n/a | single runs, none |
| 8: insert, 4 cores | 5,143 stmt/s | 5,102 stmt/s | not run | -0.8 % | not run | single runs, none |
| s4a: scenario 2, 8 cores, 8 bookers, group | 2,583 tps (2,619; 2,546 tps) | 2,535 tps (2,556; 2,515 tps) | 2,529 tps (2,537; 2,522 tps) | -1.8 % | -2.1 % | A0 2.8 %, B 1.6 %, L 0.6 % |
| s4b: scenario 2, 1 core, 8 bookers, group | 1,125 tps (1,121; 1,129 tps) | 1,123 tps (1,128; 1,117 tps) | n/a (one core) | -0.2 % | n/a | A0 0.7 %, B 0.9 % |
| s4c: scenario 2, 8 cores, 8 bookers, strict | 2,286 tps | 2,272 tps | not run | -0.6 % | not run | single runs, none |
| s4d: scenario 2, 1 core, 1 booker, strict | 636 tps | 616 tps | not run | -3.2 % | not run | single runs, none |

Reading it:

- **Cell 1 and the floor.** The two-round spread is 0.5 to 1.3 %, so B's -8.2 % is
  six floors. L's two runs equal B's (-0.1 % against B) although L moved 79 % fewer
  images, which looked like a refutation of the lever until the control: **a 12.5 s
  warm-up itself costs A0 5.6 %** (4,780 against 5,065; cause not identified, single
  run). Against the same-warm control L loses 2.8 % and B 5.4 %, so L recovered about
  half of the same-warm gap (122 of 258 stmt/s) for 79 % of the images. The control is
  one run each, B's own two runs at warm 3 s spread 1.3 % (about 60 stmt/s), so the
  recovery is about two floors wide and **is not established**. The L run with no
  checkpoint in the window (warm 3 s, its tick at 20 s after the window ended) equals
  A0, so removing the cadence's writeback altogether costs nothing: it is a bound, not
  a measurement of a steady cadence.
- **Cells 3, 5 and 6 are single runs** and are as large as cell 1's. Cell 6 (`strict`) is
  the same cell with the commit sync made a per-statement one and loses 11.2 % where
  `group` loses 8.2 %: the sign and the size agree with `group`, nothing more is
  claimed.
- **Cell 2 reads +5.9 % in B's favour and cannot be explained from this data.** A0's
  wait per statement is already 1,319 us there (88 % of a 1,500 us statement), twice the
  100K cell's 648, with 33,600 pages written a window against 8,500; the image's extra
  log (+6.9 KB a statement) does not add to it. **The reading the table supports,
  untested:** at four cores over a table three times the pool the log's sync is already
  sharing the device with the drain's data-file writes, and the image is not what the
  statement waits on. Single run per arm, so no floor; do not read it as "images are
  free at four cores under pressure". L's run had 7 refused statements (`bad`), an
  unexplained one in A0 (1) and one in two B runs; the server's stderr was not kept.
  Same arm, same window: cell 5 (one core) pays 28 %.
- **Scenario 2** (the booking path): every difference is inside or at the edge of the
  floor (s4a -1.8 % against 2.8 % and 1.6 % floors; s4b -0.2 % against 0.7 % and 0.9 %).
  B's log is +874 B a booking (+20 %: 4,286 to 5,160) at `cores = 8` and +266 B at one,
  but a booking is 2.7 ms (8 cores) or 6.5 ms (1 core, 8 bookers) and its commit 0.83
  to 0.86 ms: the image's share is too small a part of a booking to move the aggregate
  by more than the floor. L's tick never fell inside a scenario window (40 s interval
  at 8 cores; load plus bookings finish in 24 s), so **s4a's L is the zero-checkpoint
  bound**, equal to B inside the floor. The single-run s4d (-3.2 %) and s4c (-0.6 %)
  have no floor.
- **`--verify 100` failed 0 to 6 of 400 checks** in the multi-booker cells in every arm
  (A0 1 and 4 in s4a, 6 and 3 in s4b, 2 in s4c; B 1 and 2, 3 and 5, 0; L 2 and 1; one-booker
  s4d 0 and 0): the client-side lost update (`results-scenario2-freight-v3.4.14.md` section on verify), the same pattern
  at every arm, not an arm effect. `rejected-capacity` and `rejected-credit` are the
  driver's own outcomes (about 190 and 8 of 12,200 attempts); `conflicted` bookings retried were 0 to 1 at
  8 cores `group`, 0 at one core, and 23 (A0) and 24 (B) at 8 cores `strict`.

**Previous number for each shape** (rule 4; DB's close, `results-db-close-v3.4.14-5-gec057c06.md`,
pre-DB binary against `734484ca`; this file's arms are different binaries, so this is a
cross-reading, not a delta):

| cell | DB's close A -> B | DB's delta | here A0 -> B |
|---|---|---|---|
| 1 (100K rows, 4 cores) | 5,238 -> 4,625 stmt/s | -11.7 % | 5,065 -> 4,649 stmt/s, **-8.2 %** |
| 5 (1M rows, pool 2,048 frames, 1 core) | 3,518 -> 2,628 stmt/s | -25.3 % | 3,582 -> 2,576 stmt/s, **-28.1 %** |
| 7 (100K rows, 1 core) | 4,275 -> 4,188 stmt/s | -2.0 % (inside its floor of about 4 %) | 4,298 -> 4,064 stmt/s, -5.5 % (single run, no floor here) |
| scenario 2 | none at this size (the tag's file ran 3,000 bookings, a 2.5 s window) | none | no prior number exists for this size: this file is the baseline |
| cells 2, 3, 6, 8 | none | none | no prior number for the shape: this file is the baseline |

PostgreSQL has no twin for this shape (an owed-page-image comparison is another cadence
altogether, `workorder-dh-page-image-volume.md` section 6); the floor was not measured.

## 4. Where the wait goes (rule 3)

A statement at four or more cores is a log sync with a little work around it, and the
image moves the sync, not the work. Waits are `sys.wait_stats` totals divided by the
window's statements (microseconds a statement, summed over the threads that waited; a
statement's own latency is the p50 in the appendix):

| cell | arm | stmt p50 | `commit.wal_flush` | `sched.run_queue` | `cpu.on_cpu` | `io.page_read` | server CPU | log written |
|---|---|---|---|---|---|---|---|---|
| 1 | A0 | 759 / 762 us | 648 / 649 us/stmt | 0.9 / 1.0 us/stmt | 36.2 / 36.2 us/stmt | 0 us/stmt | 95.7 / 103.1 us/stmt | 250 B/stmt |
| 1 | B | 808 / 818 us | 707 / 722 us/stmt | 1.0 / 1.0 us/stmt | 35.9 / 33.1 us/stmt | 0 us/stmt | 110.5 / 100.2 us/stmt | 1,147 / 1,151 B/stmt |
| 1 | L | 840 / 835 us | 722 / 715 us/stmt | 1.0 / 1.0 us/stmt | 31.9 / 31.3 us/stmt | 0 us/stmt | 98.3 / 95.7 us/stmt | 423 / 447 B/stmt |
| 3 | A0 / B / L | 862 / 934 / 941 us | 710 / 835 / 821 us/stmt | 1.1 / 1.2 / 1.3 us/stmt | 36.7 / 39.4 / 30.6 us/stmt | 0 us/stmt | 95.9 / 103.4 / 89.0 us/stmt | 250 / 1,369 / 372 B/stmt |
| 5 | A0 | 1,001 us | 508 us/stmt | 86.6 us/stmt | 20.5 us/stmt | 3.9 us/stmt | 59.6 us/stmt | 249 B/stmt |
| 5 | B | 1,378 us | 735 us/stmt | 65.6 us/stmt | 23.4 us/stmt | 3.9 us/stmt | 74.0 us/stmt | 6,911 B/stmt |
| 7 | A0 / B | 908 / 962 us | 428 / 451 us/stmt | 35.9 / 36.0 us/stmt | 17.2 / 17.5 us/stmt | 0 us/stmt | 45.8 / 47.1 us/stmt | 249 / 500 B/stmt |

- **At one core with the pressured pool** the image costs +227 us of sync wait a statement
  (508 to 735) and +14.5 us of server CPU: **17.9 us an image** (0.81 images a statement),
  against DB's close's about 14 us. CPU is 6 % of the loss; the rest is sync length. The
  group sync carries two commits there, so each sync grows by about 13.8 KB.
- **At four and eight cores** the wait is 82 to 90 % of a statement in every arm and the
  image adds 59 to 125 us to it (+66 us for +900 B at four cores, +125 us for +1,119 B
  at eight). CPU a statement does not move outside its own run-to-run spread (cell 1 B's
  two runs: 110.5 and 100.2; A0's: 95.7 and 103.1).
- **The L runs' wait (715 to 722 us) is as long as B's at 423 to 447 B a statement
  against B's 1,150**: the wait is not a function of the log bytes alone. Section 6 uses
  this.
- Client round trip, socket and write-statement time are inside the p50; they were not
  separated (the statement is one `UPDATE` by key: its work is about 36 us of `cpu.on_cpu`
  at four cores). Lock or conflict waits were nil (`bad` aside); `wal_ring_full` was 0 in
  every run.

For scenario 2 the driver reports the booking and the commit round trip (appendix):
**commit p50 is 830 to 905 us of a 2,690 to 3,100 us booking at 8 cores (31 %)** and
about 840 us of 6,470 us at one core with eight bookers (13 %). `sys.wait_stats` was
**not** read for the scenario runs (cut with the budget).

## 5. What an image holds: the census

The census (`tools/dh_image_census.py`) walks every segment of a B run's WAL, live
directory plus a WAL archive (`archive_dir`, which stops the recycler), and extracts the
FULL_PAGE_IMAGE records. **Validation: its count equals the run's `pool_fetch_images` since
the mount exactly in all five micro cells** (8,393, 40,513, 16,538, 35,609, 306), and the
walk ended each segment at a PAD or a zero tail with no unexplained stop. The census cells
are separate B runs with the archive on (`k1`, `k2`, `k3`, `k5`, `k8`, throughput 4,557, 2,660,
7,166, 2,415, 5,137 stmt/s: 2 to 6 % under the measured B runs, so use them for page
content only); the scenario census (`k4a`) read the load's images as well, 1,737 in
the run's log against 1,224 in the booking window. The first census attempt, on the
measured B runs' own logs, found 128 of 8,260 images in cell 1: the log is recycled
below the redo start, which is why the second set exists. The walker validates no CRC
(a stale tail fails the LSN-contiguity test it uses instead).

Page types as the census read them (the `type` byte, `common.hpp`): 3 = btree leaf,
4 = undo, 1 = heap, 2 = btree internal, 10 = var-heap, 14 = anchor. Per cell the mix
and the weighted mean over it:

| cell | images (all) | mix | zero bytes (share of 8,192 B) | type-blind hole (longest zero run) | L1 | LZ4 | zstd-1 | zstd-3 | L1+LZ4 |
|---|---|---|---|---|---|---|---|---|---|
| 1 (k1) | 8,393 images | leaf 95.7 %, undo 4.3 % | 72.5 % | 0.65 % | 8,153 B/image | 3,095 B/image | 1,743 B/image | 1,540 B/image | 3,098 B/image |
| 2 (k2) | 40,513 images | leaf 99.2 %, undo 0.8 % | 70.0 % | 0.30 % | 8,181 B/image | 2,586 B/image | 1,045 B/image | 949 B/image | 2,590 B/image |
| 3 (k3) | 16,538 images | leaf 94.6 %, undo 5.4 % | 70.2 % | 1.01 % | 8,124 B/image | 3,433 B/image | 2,121 B/image | 1,861 B/image | 3,437 B/image |
| 5 (k5) | 35,609 images | leaf 100 % | 70.0 % | 0.17 % | 8,192 B/image | 2,585 B/image | 1,042 B/image | 941 B/image | 2,589 B/image |
| 8 (k8) | 306 images | undo 82.4 %, heap 7.5 %, internal 4.9 %, leaf 4.9 %, anchor 0.3 % | 87.4 % | 17.3 % | 6,776 B/image | 1,563 B/image | 569 B/image | 566 B/image | 1,554 B/image |
| s4a (k4a) | 1,737 images | leaf 66.4 %, undo 27.1 %, heap 3.7 %, internal 2.6 %, var-heap 0.1 %, anchor 0.1 % | 76.3 % | 9.7 % | 7,402 B/image | 2,547 B/image | 1,437 B/image | 1,425 B/image | 2,541 B/image |

(L1 is the page less its longest zero run if that is 32 bytes or more, plus 4 bytes of
descriptor. Bytes are the mean over the statistics sample: **the first 3,000 images by LSN of
each type's evenly thinned kept list**, not an even stride over the whole log, so the later
part of a log is under-represented for a type that has more than 3,000 images (the leaf in
cells 1, 2, 3 and 5); a type with fewer (every type in cells 8 and s4a) is read whole.
The census's own page-type names are the `type` byte of `common.hpp`, 13 and 14 included
(the first run's tool lacked them and printed `type14`). The type-blind hole is the longest
zero run, not a per-type free region.)

By page type (cells 1, 2, 3, 5 merged where they agree; per-cell values in `census/*.json`):

| type | zero share | longest zero run: mean / median / p90, start offset | LZ4 ratio median / p90, time | zstd-1 ratio median, time | zstd-3 ratio median, time | zlib-1 ratio median, time |
|---|---|---|---|---|---|---|
| btree leaf, cells 1 / 2 / 3 / 5 | 72 / 70 / 70 / 70 % | 23 / 14 / 15 / 14 B mean; 14 to 15 B median and p90; at offset 885 to 929 B (the 15 bytes after the slot array) | 0.38 / 0.32 / 0.42 / 0.31 (p90 0.42 / 0.32 / 0.48 / 0.32); 11.6 / 9.4 / 13.4 / 9.5 us/page | 0.22 / 0.13 / 0.26 / 0.13; 32.1 / 26.0 / 34.0 / 25.8 us/page | 0.19 / 0.12 / 0.23 / 0.12; 35 / 30 / 37 / 29 us/page | 0.23 / 0.15 / 0.27 / 0.15; 133 / 107 / 145 / 106 us/page |
| undo, cells 1 / 2 / 3 | 76 % | mean 722 / 1,406 / 1,259 B; **median 49 to 50 B**, p90 3,178 / 5,625 / 5,149 B; at offset 8,142 to 8,143 B (the page's end) in the common case | 0.37 / 0.32 / 0.40 (p90 0.41 / 0.48 / 0.48); 9 to 12 us/page | 0.26 / 0.21 / 0.28; 34 / 29 / 33 us/page | 0.24 / 0.20 / 0.26; 29 to 35 us/page | 0.27 / 0.22 / 0.29; 106 / 88 / 106 us/page |
| undo, insert cell (8, 252 images) | 87 % | mean 666 B, median 46 B | 0.21; 5.9 us/page | 0.07; 22.8 us/page | 0.07; 22.7 us/page | 0.12; 98 us/page |
| heap (8: 23 images; s4a: 65 images) | 97 % / 96 % | 6,147 / 5,259 B mean, at offset 121 / 156 B | 0.06 / 0.08; 4.1 / 4.0 us/page | 0.03 / 0.05; 16 us/page | 0.03 / 0.04; 19 / 18 us/page | 0.04 / 0.06; 74 / 80 us/page |
| btree internal (8: 15 images; s4a: 45 images) | 83 % / 91 % | 3,728 / 5,908 B mean, offset quartiles 3,394 to 5,710 / 1,066 to 3,010 B | 0.42 / 0.14; 9.8 / 5.8 us/page | 0.20 / 0.08; 28 / 20 us/page | 0.22 / 0.08; 36 / 23 us/page | 0.23 / 0.09; 110 / 82 us/page |
| btree leaf, insert cell (8: 15 images) and scenario (1,153 images) | 86 % / 73 % | 4,066 / 296 B mean (median 4,663 / 56 B) | 0.15 / 0.25; 6.3 / 12.4 us/page | 0.08 / 0.14; 22 / 28 us/page | 0.07 / 0.13; 24 / 30 us/page | 0.09 / 0.18; 63 / 116 us/page |

What this says:

- **The hole is not where the bytes are.** On a leaf that is full of fixed-length rows, which
  is what an `UPDATE` workload on a loaded table images (94.6 to 100 % of the images in
  cells 1, 2, 3, 5: 95.7, 99.2, 94.6 and 100 %), the longest zero run is 14 to 23 bytes mean: **L1 saves 0.5 % of the
  image in cell 1, 0.1 % in cell 2, 0.8 % in cell 3, 0 in cell 5.** Only where the images
  are of young pages (cell 8 and the load phase inside k4a: heap, leaf, internal) does a
  run exist (3 to 6 KB), and the images there number 29 B a statement (cell 8) and
  about 840 B a booking of which L1's share is 10 %, about 80 B. **A type-blind hole is
  a poor stand-in for a per-type free region**: a slotted page's `lower`/`upper` free
  region is invisible to it if the freed bytes are not zeroed, and that was not read; but
  the leaf's longest zero run bounds the *zeroed* free space at 15 bytes, and a leaf this
  full has little free space of either kind.
- **The compression ratio comes from the data, not the engine.** 70 % of a leaf's bytes
  are zero because the rows are three `int64` columns with small values and a 20-byte MVCC
  header whose high bytes are zero. LZ4 reaches 0.31 to 0.42 of the page, zstd-1 0.13 to
  0.26, zstd-3 0.12 to 0.23, zlib-1 0.15 to 0.27 (at 106 to 145 us a page, 8 to 10 times
  LZ4's). The ratio varies with the cell's key distribution: cell 3 (eight cores) compresses
  worse than cell 2 for the same table because its leaf rows are fuller of changed values
  (p90 0.48 against 0.32). A table of wide, random or already-compressed columns (a hash, a
  varchar of random text, a `decimal128`) would compress far less; this census has no such
  table, so **the compression result is an upper bound for fixed-length small-integer rows
  and is not a prediction for another schema**.
- **CPU of the levers against today's image.** Cost today, measured here: 17.9 us an image
  at cell 5 (15.6 at cell 2; DB's close, 14 us), which includes copying the page into the
  log ring. A compressor runs on the write fetch while the page is held exclusive:
  **LZ4 adds 9.4 to 13.4 us an image** (leaf, ctypes call overhead included, about
  1 us), i.e. 0.5 to 0.75 of today's cost again; **zstd-1 adds 26 to 34 us** (1.5 to 2 times
  today's cost), zstd-3 29 to 37 us, zlib-1 88 to 145 us (5 to 8 times). A zero-run
  cut (L1) adds the scan, which was not timed alone: L1+LZ4 compressed in the same time as
  LZ4 on a cut page (11.5 against 11.7 us).

## 6. What each lever would buy

Image bytes a statement (a booking for s4a) projected from the census's weighted bytes an
image and B's measured images a statement. No byte counter exists in the engine (DH-R6): bytes are
the count times 8,192 for B and the census's means for the levers; the 32-byte record header
is not included in either. L3 is **measured by configuration** (L's run), the others are
projected arithmetic.

| cell | B images | B image bytes | L3 (L run) | L3 removes | L1 | L2 LZ4 | L2 zstd-1 | L1+LZ4 |
|---|---|---|---|---|---|---|---|---|
| 1: 100K rows, 4 cores | 0.109 img/stmt | 896 B/stmt | 186 B/stmt | 79.3 % of the images | 891 B/stmt (-0.5 %) | 338 B/stmt (-62 %) | 191 B/stmt (-79 %) | 339 B/stmt (-62 %) |
| 2: 1M rows, pool 2,048 frames, 4 cores | 0.842 img/stmt | 6,894 B/stmt | 6,650 B/stmt | 3.5 % of the images | 6,885 B/stmt (-0.1 %) | 2,176 B/stmt (-68 %) | 880 B/stmt (-87 %) | 2,180 B/stmt (-68 %) |
| 3: 100K rows, 8 cores | 0.136 img/stmt | 1,115 B/stmt | 123 B/stmt | 89.0 % of the images | 1,105 B/stmt (-0.8 %) | 467 B/stmt (-58 %) | 289 B/stmt (-74 %) | 468 B/stmt (-58 %) |
| 5: 1M rows, pool 2,048 frames, 1 core | 0.810 img/stmt | 6,635 B/stmt | n/a (one core) | n/a | 6,635 B/stmt (0 %) | 2,094 B/stmt (-68 %) | 844 B/stmt (-87 %) | 2,097 B/stmt (-68 %) |
| 8: insert, 4 cores | 0.004 img/stmt | 29 B/stmt | not run | not run | 24 B/stmt (-17 %) | 5 B/stmt (-80 %) | 2 B/stmt (-93 %) | 5 B/stmt (-83 %) |
| s4a: scenario 2, 8 cores (per booking; census includes load images) | 0.102 img/booking | 840 B/booking | 1 image, 0 B/booking (L's window held no checkpoint) | bound only | 759 B/booking (-10 %) | 261 B/booking (-69 %) | 147 B/booking (-82 %) | 261 B/booking (-69 %) |

L3's bound is the **cadence's** images. At cell 2 the images are the eviction drain's
copies (22,000 pages drained against 13,600 by checkpoint in B; 28,400 drained in L) and
no cadence change touches them: **L3 helps where the hot set fits the pool and not
where it does not**, which is also where the price is largest (cells 2 and 5 at 6.6 to 6.9
KB a statement). Cells 1 and 3's L figures include the images of the pages faulted in
after the mount (1,472 to 1,800 images a window with one tick), so they are not a pure
cadence rate. A steady cadence at `cores` times the interval would write at a quarter or
an eighth of B's rate; the L runs' 1,472 to 1,800 images a window at 100K rows are what a single
tick in a 15 s window plus the post-mount faults show.

**Throughput, by lever: not projected, and why.** A projection needs the flush wait as a
function of the log bytes a sync, and the cells do not give one:

| cell | extra log (B - A0) | extra `commit.wal_flush` | extra wait per extra byte |
|---|---|---|---|
| 1 (4 cores) | +900 B/stmt | +66 us/stmt | 73 ns/B |
| 3 (8 cores) | +1,119 B/stmt | +125 us/stmt | 112 ns/B |
| 6 (4 cores, strict) | +1,052 B/stmt | +89 us/stmt | 85 ns/B |
| 7 (1 core) | +251 B/stmt | +23 us/stmt | 92 ns/B |
| 5 (1 core, pressure) | +6,662 B/stmt | +227 us/stmt | 34 ns/B |
| 2 (4 cores, pressure) | +6,921 B/stmt | -86 us/stmt | none (wrong sign) |
| 1, L (4 cores, warm 12.5 s) | +174 to +198 B/stmt | +66 to +73 us/stmt over A0 (warm 3 s) | 340 to 430 ns/B |

The slope is 34 to 112 ns a byte, falls with the bytes (the two pressure cells), has the
wrong sign in one and is 3 to 5 times higher on the one cell that tests it with a third
point. `commits/sync` (`wal_group_commits / wal_group_batches`) is in the appendix but
reads 1.0 and 2.82 for the same cell's two B runs, so a bytes-a-sync axis is not usable. A
line through each cell's own A0 and B points would fit by construction and would
mis-predict L, which is the only independent check there is. **So the relation is not
good enough to project from, and none is given.** What the data does bound: every lever
lies between B and A0 in every cell, so cell 1's ceiling is +8.9 % (A0 over B), cell 3's
+14.6 %, cell 5's +39 %, cell 6's +12.6 %, cell 7's +5.8 %, cell 8's +0.8 %, scenario
cells' +0.2 to +3.2 % (inside the floor for s4a and s4b). Cell 2's bound is below zero (A0 is
the slower arm).

## 7. What the run teaches

- **The price is sync length, so it is a function of log bytes and device, and the log
  bytes of an image are the whole of it.** CPU (17.9 us an image) is 6 % of the loss at
  the cell where the loss is largest. This agrees with DB's close, section on waits, and
  the work order's section 1.5; no stated expectation is contradicted.
- **L1 is dead on the workloads that pay.** The work order's section 1.3 expected a
  per-type free region and said the ranking was open until the hole was measured; it is
  measured: full leaves have no hole, undo pages have a bimodal one (median 50 bytes,
  p90 3 to 5.6 KB; 4 to 5 % of images in cells 1 and 3), heap and internal pages have large
  ones but are 0.3 % to 12 % of the images and rarely imaged on an update workload. **No
  lever for which a zero-run elision would be the whole design is supported by these
  cells.** Whether a *zeroed-by-construction* free region exists per type was not read.
- **L2's yield exists on this data and is cheaper in CPU than the image already costs
  when LZ4 is used**: 58 to 69 % of the bytes for about 9 to 13 us. That is the only
  lever of the three that reduces the pressure cells' volume (cells 2 and 5: 6.6 to 6.9
  KB a statement to 2.1 to 2.2 KB), where the cadence lever does nothing. **It is a
  format event** (DH-R3, DH-Q3 `[quiet-wrong]`), a dependency the tree does not have
  (DH-Q6), and it holds the page exclusive for 10 us longer on the write fetch; the
  tail (p99) of a statement was not measured for that. The yield is the one number in
  this file that depends most on the schema; it should be re-censused on a table
  with wide and random columns before it supports a build.
- **L3 recovers the cells where the hot set fits the pool, partly and unproven.** It
  removes 79 to 89 % of the images at four and eight cores and touches no format;
  measured throughput recovery is about half of the gap at cell 1 (one run per control, two
  floors wide) and is unmeasured at cell 3 (L equals B within a floor's reach and there is
  no same-warm A0 control). The warm-up length itself cost A0 5.6 % in cell 1; **a
  steady-state L run with a longer window (two to three ticks) would settle both questions
  and was not run.**
- **Scenario 2 pays almost nothing.** At the booking path's shapes (1 and 8 cores, `group`
  and `strict`) the delta is 0.2 to 3.2 %, inside the floor where a floor exists (two of
  four shapes) and unproven where it does not (s4c, s4d are single runs). A booking's
  statement-and-commit mix hides the image: B's log is 20 % larger per booking at
  8 cores, a booking is 2.7 ms, and the commit sync is 0.86 ms of it. The work order's
  DH-Q1 proposal puts scenario 2 first and the bar to be named after DH-S0 (DH-Q2): **on the
  evidence, the price on the booking path is not distinguishable from nothing at this
  size**, and the two cells where it is large are the update micro-cells the operator
  already read.
- **The one-core reference (DH-R4) is not zero in B itself**: cell 7 reads -5.5 % (single
  run; DB's close -2.0 % inside a floor of 4 %), cell 5 -28.1 %, and 250 to 6,600 B of log a
  statement. L3 does not move a one-core cell by construction; L2 and L1 would. Whether
  cell 7's -5.5 % is above the floor needs a repeat that was not run.
- **The pressure cell at four cores is unexplained.** The image costs nothing visible at
  four cores and 6.9 KB a statement (cell 2) and 28 % at one core and 6.6 KB (cell 5). The
  reading in section 3 (the sync already shares the device with the drain) is a
  hypothesis; the measurement that would test it is A0 and B at four cores with the
  drain's writes isolated, and it was not made.
- **Where the evidence does not let a lever be ranked.** L3 against L2 on the 100K cells
  cannot be ranked on throughput (section 6); on image bytes they remove 79 to 89 % (L3)
  and 58 to 68 % (LZ4), not additively (they act on different factors: how many images,
  how big). On the pressure cells only L2 acts. DH-Q3's "yes, if DH-S0 shows either
  recovers more than L3 alone" is met on bytes in cells 2 and 5 and not otherwise.

## 8. Not measured and not claimed

`strict` at more than one cell for L; `cores = 4` pressure with an interleaved repeat;
scenario 1; the row-set axis (200, 1K, 10K); recovery time over a log that replays many
images; the p99 effect of holding the page exclusive 10 us longer; the checkpointer's
begin-record and anchor rules against L3's two candidates and the drain's selection (the
reading DH-Q4 makes a prerequisite of L3 as a candidate: not done); the images of a table
with wide, random or varchar columns; backup, restore, PITR and sim readers of an image
(DH-R3's read). `p95` is not in the micro tables: the capture the driver reuses
(`db_close.drive_lat`) records p0, p25, p50, p90, p99; scenario tables carry p95. L's
extra warm-up (12.5 s, 32.5 s at 8 cores) is a confound the same-warm control tested in
cell 1 only. `bad` (statements whose reply did not carry the expected tag) was 0 except
cell 2 A0 (1) and L (7), cell 1's control B (1), cell 6 B (1); their reasons were not
kept.

## Appendix: every run

Every cell carries its unit. `commits per sync` is `wal_group_commits / wal_group_batches`
over the window and is unreliable (section 6). **`ckpt runs` is the server's counter
`contention_checkpoint_runs` over the window**, recomputed from the raw JSON for this
version of the appendix. The earlier version counted `sys.checkpoints` rows whose `seq` was
above the window-start maximum, and `seq` counts per core, so that count and the scenario
column "ckpt runs started" (a max-`seq` difference) were not run counts above one core:
the two agree for every micro run except the control `c1w` (10 rows, counter 12), and the
scenario column read 1, 1, 0, 2, 3, 1, 1, 5, 5, 2, 1, 0, 3, 3 for runs whose counter says
12, 13, 0, 2, 3, 11, 13, 5, 5, 12, 12, 0, 3, 3 (s4a to s4d, in table order). The raw
`checkpoints` lists in the JSON are the filtered rows and cannot recover a lagging core's
rows; the counter can. `ckpt pages written` is `contention_checkpoint_pages_written`;
`pages drained` is `pool_dirty_drained`. Latencies are client-side. The micro tables carry
p90, not p95 (the capture the driver reuses records p0, p25, p50, p90, p99); the scenario
table carries p95. The run `c1r1`/`L` row labelled `c1r1-notick` is the L run with warm 3 s
whose tick fell after the window. The census-run B rows (`k*`) and the control rows (`c1w`)
are in `census-runs.jsonl` and `ctl-runs.jsonl` (not tabulated here); the control is the
same-warm A0 and B of section 3.

### micro runs

| run | arm | cell | cores x clients | durability | throughput | p0 | p25 | p50 | p90 | p99 | server CPU | log written | images | image bytes (count x 8,192) | ckpt runs (counter) | ckpt pages written | pages drained | commits per sync | ring full | wal_flush wait | run_queue wait | on_cpu | page_read wait | bad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c1r1 | A0 | update-lit:100,000 rows | 4 cores x 4 clients | group | 5,078 stmt/s | 353 us | 677 us | 759 us | 956 us | 1,251 us | 95.7 us/stmt | 250 B/stmt | 0.000 img/stmt | 0 B/stmt | 11 runs | 8,461 pages | 0 pages | 1.0 commits/sync | 0 events | 648.1 us/stmt | 0.9 us/stmt | 36.2 us/stmt | 0.0 us/stmt | 0 stmts |
| c1r1 | B | update-lit:100,000 rows | 4 cores x 4 clients | group | 4,679 stmt/s | 391 us | 706 us | 808 us | 1,056 us | 1,414 us | 110.5 us/stmt | 1,147 B/stmt | 0.109 img/stmt | 894 B/stmt | 11 runs | 7,812 pages | 0 pages | 1.0 commits/sync | 0 events | 707.1 us/stmt | 1.0 us/stmt | 35.9 us/stmt | 0.0 us/stmt | 0 stmts |
| c1r1-notick | L | update-lit:100,000 rows | 4 cores x 4 clients | group | 5,041 stmt/s | 381 us | 688 us | 775 us | 975 us | 1,221 us | 91.8 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 0 runs | 0 pages | 0 pages | 1.0 commits/sync | 0 events | 658.3 us/stmt | 0.9 us/stmt | 28.6 us/stmt | 0.0 us/stmt | 0 stmts |
| c1r2 | A0 | update-lit:100,000 rows | 4 cores x 4 clients | group | 5,051 stmt/s | 357 us | 681 us | 762 us | 967 us | 1,280 us | 103.1 us/stmt | 250 B/stmt | 0.000 img/stmt | 0 B/stmt | 11 runs | 8,101 pages | 0 pages | 1.4 commits/sync | 0 events | 648.8 us/stmt | 1.0 us/stmt | 36.2 us/stmt | 0.0 us/stmt | 0 stmts |
| c1r1 | L | update-lit:100,000 rows | 4 cores x 4 clients | group | 4,619 stmt/s | 432 us | 754 us | 840 us | 1,046 us | 1,352 us | 98.3 us/stmt | 423 B/stmt | 0.021 img/stmt | 174 B/stmt | 2 runs | 1,592 pages | 0 pages | 1.0 commits/sync | 0 events | 721.7 us/stmt | 1.0 us/stmt | 31.9 us/stmt | 0.0 us/stmt | 0 stmts |
| c1r2 | B | update-lit:100,000 rows | 4 cores x 4 clients | group | 4,619 stmt/s | 391 us | 716 us | 818 us | 1,070 us | 1,422 us | 100.2 us/stmt | 1,151 B/stmt | 0.110 img/stmt | 898 B/stmt | 11 runs | 7,727 pages | 0 pages | 2.8 commits/sync | 0 events | 722.5 us/stmt | 1.0 us/stmt | 33.1 us/stmt | 0.0 us/stmt | 0 stmts |
| c1r2 | L | update-lit:100,000 rows | 4 cores x 4 clients | group | 4,668 stmt/s | 432 us | 750 us | 835 us | 1,036 us | 1,306 us | 95.7 us/stmt | 447 B/stmt | 0.024 img/stmt | 198 B/stmt | 2 runs | 1,762 pages | 0 pages | 1.0 commits/sync | 0 events | 715.4 us/stmt | 1.0 us/stmt | 31.3 us/stmt | 0.0 us/stmt | 0 stmts |
| c2 | A0 | update-lit:1,000,000 rows, pool 2,048 frames | 4 cores x 4 clients | group | 2,663 stmt/s | 386 us | 696 us | 782 us | 989 us | 2,481 us | 133.7 us/stmt | 253 B/stmt | 0.000 img/stmt | 0 B/stmt | 11 runs | 13,508 pages | 20,134 pages | 1.0 commits/sync | 0 events | 1,318.7 us/stmt | 16.7 us/stmt | 46.2 us/stmt | 4.7 us/stmt | 1 stmts |
| c2 | B | update-lit:1,000,000 rows, pool 2,048 frames | 4 cores x 4 clients | group | 2,820 stmt/s | 528 us | 1,035 us | 1,146 us | 1,406 us | 2,374 us | 146.8 us/stmt | 7,174 B/stmt | 0.842 img/stmt | 6,894 B/stmt | 11 runs | 13,582 pages | 22,331 pages | 1.0 commits/sync | 0 events | 1,232.7 us/stmt | 3.5 us/stmt | 49.9 us/stmt | 4.7 us/stmt | 0 stmts |
| c2 | L | update-lit:1,000,000 rows, pool 2,048 frames | 4 cores x 4 clients | group | 2,528 stmt/s | 580 us | 1,289 us | 1,455 us | 1,787 us | 2,355 us | 160.9 us/stmt | 6,926 B/stmt | 0.812 img/stmt | 6,650 B/stmt | 2 runs | 2,484 pages | 28,361 pages | 1.0 commits/sync | 0 events | 1,387.1 us/stmt | 1.3 us/stmt | 52.6 us/stmt | 4.8 us/stmt | 7 stmts |
| c3 | A0 | update-lit:100,000 rows | 8 cores x 8 clients | group | 8,980 stmt/s | 383 us | 755 us | 862 us | 1,119 us | 1,507 us | 95.9 us/stmt | 250 B/stmt | 0.000 img/stmt | 0 B/stmt | 21 runs | 16,283 pages | 0 pages | 1.0 commits/sync | 0 events | 709.6 us/stmt | 1.1 us/stmt | 36.7 us/stmt | 0.0 us/stmt | 0 stmts |
| c3 | B | update-lit:100,000 rows | 8 cores x 8 clients | group | 7,836 stmt/s | 392 us | 809 us | 934 us | 1,278 us | 1,767 us | 103.4 us/stmt | 1,369 B/stmt | 0.136 img/stmt | 1,115 B/stmt | 21 runs | 16,236 pages | 0 pages | 1.0 commits/sync | 0 events | 834.7 us/stmt | 1.2 us/stmt | 39.4 us/stmt | 0.0 us/stmt | 0 stmts |
| c3 | L | update-lit:100,000 rows | 8 cores x 8 clients | group | 8,023 stmt/s | 424 us | 828 us | 941 us | 1,202 us | 1,589 us | 89.0 us/stmt | 372 B/stmt | 0.015 img/stmt | 123 B/stmt | 2 runs | 2,012 pages | 0 pages | 2.0 commits/sync | 0 events | 821.2 us/stmt | 1.3 us/stmt | 30.6 us/stmt | 0.0 us/stmt | 0 stmts |
| c5 | A0 | update-lit:1,000,000 rows, pool 2,048 frames | 1 cores x 4 clients | group | 3,582 stmt/s | 468 us | 892 us | 1,001 us | 1,273 us | 1,983 us | 59.6 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | 3,566 pages | 39,770 pages | 2.0 commits/sync | 0 events | 508.1 us/stmt | 86.6 us/stmt | 20.5 us/stmt | 3.9 us/stmt | 0 stmts |
| c5 | B | update-lit:1,000,000 rows, pool 2,048 frames | 1 cores x 4 clients | group | 2,576 stmt/s | 700 us | 1,253 us | 1,378 us | 1,693 us | 2,320 us | 74.0 us/stmt | 6,911 B/stmt | 0.810 img/stmt | 6,635 B/stmt | 3 runs | 3,575 pages | 27,698 pages | 2.0 commits/sync | 0 events | 735.3 us/stmt | 65.6 us/stmt | 23.4 us/stmt | 3.9 us/stmt | 0 stmts |
| c6 | A0 | update-lit:100,000 rows | 4 cores x 4 clients | strict | 4,214 stmt/s | 366 us | 800 us | 918 us | 1,212 us | 1,581 us | 121.3 us/stmt | 250 B/stmt | 0.000 img/stmt | 0 B/stmt | 11 runs | 7,414 pages | 0 pages | 0.0 commits/sync | 0 events | 633.8 us/stmt | 0.8 us/stmt | 42.1 us/stmt | 0.0 us/stmt | 0 stmts |
| c6 | B | update-lit:100,000 rows | 4 cores x 4 clients | strict | 3,743 stmt/s | 341 us | 879 us | 1,016 us | 1,340 us | 1,854 us | 131.1 us/stmt | 1,302 B/stmt | 0.128 img/stmt | 1,048 B/stmt | 11 runs | 7,339 pages | 0 pages | 0.0 commits/sync | 0 events | 722.6 us/stmt | 0.9 us/stmt | 45.3 us/stmt | 0.0 us/stmt | 1 stmts |
| c7 | A0 | update-lit:100,000 rows | 1 cores x 4 clients | group | 4,298 stmt/s | 519 us | 813 us | 908 us | 1,128 us | 1,413 us | 45.8 us/stmt | 249 B/stmt | 0.000 img/stmt | 0 B/stmt | 3 runs | 1,861 pages | 0 pages | 2.0 commits/sync | 0 events | 427.6 us/stmt | 35.9 us/stmt | 17.2 us/stmt | 0.0 us/stmt | 0 stmts |
| c7 | B | update-lit:100,000 rows | 1 cores x 4 clients | group | 4,064 stmt/s | 483 us | 858 us | 962 us | 1,204 us | 1,522 us | 47.1 us/stmt | 500 B/stmt | 0.031 img/stmt | 250 B/stmt | 3 runs | 1,867 pages | 0 pages | 2.0 commits/sync | 0 events | 450.6 us/stmt | 36.0 us/stmt | 17.5 us/stmt | 0.0 us/stmt | 0 stmts |
| c8 | A0 | insert:0 rows | 4 cores x 4 clients | group | 5,143 stmt/s | 381 us | 664 us | 744 us | 933 us | 1,185 us | 83.7 us/stmt | 533 B/stmt | 0.000 img/stmt | 0 B/stmt | 11 runs | 935 pages | 0 pages | 3.0 commits/sync | 0 events | 651.9 us/stmt | 1.2 us/stmt | 26.4 us/stmt | 0.0 us/stmt | 0 stmts |
| c8 | B | insert:0 rows | 4 cores x 4 clients | group | 5,102 stmt/s | 351 us | 667 us | 750 us | 943 us | 1,201 us | 72.0 us/stmt | 562 B/stmt | 0.004 img/stmt | 29 B/stmt | 11 runs | 882 pages | 0 pages | 18.1 commits/sync | 0 events | 668.9 us/stmt | 1.1 us/stmt | 18.3 us/stmt | 0.0 us/stmt | 0 stmts |

### scenario 2 runs

| run | arm | cores x bookers | durability | throughput | booking p0 | p25 | p50 | p95 | p99 | commit p0 | p25 | p50 | p95 | p99 | log written | images | image bytes (count x 8,192) | ckpt runs (counter) | ckpt pages written | syncs | commits per sync | ring full | window | verify failures | committed |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| s4a | A0 | 8 cores x 8 bookers | group | 2,619 tps | 429 us | 2,426 us | 2,688 us | 3,395 us | 4,366 us | 389 us | 701 us | 836 us | 1,256 us | 1,763 us | 4,286 B/booking | 0.000 img/booking | 0 B/booking | 12 runs | 2,533 pages | 0 syncs | 1.0 commits/sync | 0 events | 7.6 s | 1 of 400 checks | 12,000 bookings |
| s4a | B | 8 cores x 8 bookers | group | 2,556 tps | 428 us | 2,449 us | 2,729 us | 3,474 us | 4,298 us | 400 us | 717 us | 859 us | 1,323 us | 1,729 us | 5,160 B/booking | 0.102 img/booking | 840 B/booking | 13 runs | 2,544 pages | 0 syncs | 1.1 commits/sync | 0 events | 7.8 s | 1 of 400 checks | 12,000 bookings |
| s4a | L | 8 cores x 8 bookers | group | 2,537 tps | 395 us | 2,474 us | 2,737 us | 3,453 us | 4,467 us | 357 us | 693 us | 821 us | 1,233 us | 1,669 us | 4,374 B/booking | 0.000 img/booking | 1 B/booking | 0 runs | 0 pages | 36,010 syncs | 1.2 commits/sync | 0 events | 7.1 s | 2 of 400 checks | 12,000 bookings |
| s4b | A0 | 1 cores x 8 bookers | group | 1,121 tps | 988 us | 5,880 us | 6,519 us | 8,571 us | 15,632 us | 416 us | 711 us | 838 us | 1,497 us | 1,882 us | 4,350 B/booking | 0.000 img/booking | 0 B/booking | 2 runs | 800 pages | 10,275 syncs | 1.2 commits/sync | 0 events | 13.2 s | 6 of 400 checks | 12,000 bookings |
| s4b | B | 1 cores x 8 bookers | group | 1,128 tps | 1,488 us | 5,801 us | 6,472 us | 8,516 us | 13,894 us | 418 us | 709 us | 830 us | 1,481 us | 1,870 us | 4,616 B/booking | 0.030 img/booking | 245 B/booking | 3 runs | 1,018 pages | 10,340 syncs | 1.2 commits/sync | 0 events | 13.4 s | 3 of 400 checks | 12,000 bookings |
| s4c | A0 | 8 cores x 8 bookers | strict | 2,286 tps | 429 us | 2,742 us | 3,062 us | 4,018 us | 6,431 us | 397 us | 737 us | 872 us | 1,333 us | 1,924 us | 4,288 B/booking | 0.000 img/booking | 0 B/booking | 11 runs | 2,517 pages | 0 syncs | 0.0 commits/sync | 0 events | 7.3 s | 2 of 400 checks | 12,000 bookings |
| s4c | B | 8 cores x 8 bookers | strict | 2,272 tps | 658 us | 2,788 us | 3,102 us | 4,044 us | 5,244 us | 391 us | 755 us | 905 us | 1,403 us | 1,853 us | 5,172 B/booking | 0.088 img/booking | 725 B/booking | 13 runs | 2,649 pages | 0 syncs | 0.0 commits/sync | 0 events | 8.3 s | 0 of 400 checks | 12,000 bookings |
| s4d | A0 | 1 cores x 1 bookers | strict | 636 tps | 369 us | 1,301 us | 1,414 us | 1,907 us | 2,351 us | 298 us | 469 us | 530 us | 736 us | 892 us | 4,377 B/booking | 0.000 img/booking | 0 B/booking | 5 runs | 1,194 pages | 12,066 syncs | - | 0 events | 20.5 s | 0 of 400 checks | 12,000 bookings |
| s4d | B | 1 cores x 1 bookers | strict | 616 tps | 369 us | 1,330 us | 1,455 us | 1,987 us | 2,578 us | 293 us | 492 us | 560 us | 793 us | 1,005 us | 4,625 B/booking | 0.034 img/booking | 275 B/booking | 5 runs | 1,187 pages | 11,981 syncs | - | 0 events | 21.1 s | 0 of 400 checks | 12,000 bookings |
| s4a2 | A0 | 8 cores x 8 bookers | group | 2,546 tps | 432 us | 2,392 us | 2,664 us | 3,405 us | 4,378 us | 394 us | 710 us | 845 us | 1,307 us | 1,905 us | 4,291 B/booking | 0.000 img/booking | 0 B/booking | 12 runs | 2,471 pages | 0 syncs | 1.0 commits/sync | 0 events | 7.6 s | 4 of 400 checks | 12,000 bookings |
| s4a2 | B | 8 cores x 8 bookers | group | 2,515 tps | 476 us | 2,448 us | 2,725 us | 3,520 us | 4,897 us | 363 us | 720 us | 862 us | 1,329 us | 1,866 us | 5,275 B/booking | 0.105 img/booking | 857 B/booking | 12 runs | 2,690 pages | 0 syncs | 1.0 commits/sync | 0 events | 7.3 s | 2 of 400 checks | 12,000 bookings |
| s4a2 | L | 8 cores x 8 bookers | group | 2,522 tps | 401 us | 2,458 us | 2,730 us | 3,434 us | 4,276 us | 391 us | 706 us | 839 us | 1,243 us | 1,567 us | 4,392 B/booking | 0.000 img/booking | 1 B/booking | 0 runs | 0 pages | 0 syncs | 1.1 commits/sync | 0 events | 7.8 s | 1 of 400 checks | 12,000 bookings |
| s4b2 | A0 | 1 cores x 8 bookers | group | 1,129 tps | 1,330 us | 5,822 us | 6,484 us | 8,500 us | 15,660 us | 388 us | 708 us | 833 us | 1,492 us | 1,899 us | 4,378 B/booking | 0.000 img/booking | 0 B/booking | 3 runs | 994 pages | 10,352 syncs | 1.2 commits/sync | 0 events | 13.2 s | 3 of 400 checks | 12,000 bookings |
| s4b2 | B | 1 cores x 8 bookers | group | 1,117 tps | 710 us | 5,853 us | 6,506 us | 8,620 us | 15,766 us | 437 us | 719 us | 846 us | 1,508 us | 1,888 us | 4,618 B/booking | 0.029 img/booking | 238 B/booking | 3 runs | 1,014 pages | 10,344 syncs | 1.2 commits/sync | 0 events | 13.2 s | 5 of 400 checks | 12,000 bookings |
