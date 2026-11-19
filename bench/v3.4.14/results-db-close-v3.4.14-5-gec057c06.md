# DB close: what the first-write page image costs a statement

DB (`instructions/v3.2.0/workorder-db-torn-page-healing.md`) logs one 8,192-byte
`FULL_PAGE_IMAGE` at the first write fetch of a page after its fault-in or after
each writeback copy, so a torn page heals at mount. DB-Q5 asks for the close as
an interleaved A/B over the DB-S0 cells and sets **no throughput bar**: this
file reports the deltas and the operator rules on them. Finding in one
paragraph: **the image is free where the hot set is small and expensive where
pages are written often.** At 200, 1K and 10K rows a point UPDATE moves by less
than the run-to-run spread (about 4 %) in throughput, latency and server CPU;
at 100K rows and four cores it costs 11.7 % of throughput, and with a table
three times the pool (1M rows, 2,048 frames) it costs 25 % and writes 6.7 KB
of log per 249-byte statement. The size of the cost is the number of page
writes, not the statement rate: the images the engine logged equal the pages
it wrote to the device, to within 10 of 43,000.

## Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19, 18:06:46 - 18:39:13 UTC (matrix 18:06:46 - 18:30:52; pressure and recovery additions 18:31 - 18:39) |
| Version | A = `v3.4.14-2-g734484ca` (the commit DB opened from); B = `v3.4.14-5-gec057c06` (DB-S2's commit) |
| Branch / worktree | `worktree-db-torn-page-healing`, `/home/louis/ckdbs/.claude/worktrees/db-torn-page-healing` |
| Tree | Both builds are `git archive` extractions of the named commit, built fresh in `/home/louis/bench-runs/db-close/build{A,B}` (Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `kds_server` target only), so neither measures a dirty tree. The worktree itself carries uncommitted documents and `include/kds/storage/log_page_image.hpp`, `include/kds/txn/undo_log.hpp`, `sim/*` edits; **none of those is in either binary**. The only untracked file the run added to the worktree is the driver `tools/db_close.py` |
| Source diff A to B | 15 files under `src/` and `include/`, +327 / -20 lines (`device_page_store`, `page_store`, `redo`, `wal/durability`, `catalog`, `index_ddl`, `expeditor`, `command_dispatcher`'s `pool_fetch_images` field) |
| Binary provenance | built 2026-11-19 18:06:19 UTC from the extraction (commits are 08:04 UTC for A and 18:03 UTC for B, so neither binary is older than its commit). Measured copies `/home/louis/bench-runs/db-close/bin/kds_server_A` sha256 `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3` and `kds_server_B` sha256 `3e84864f7ab07b5b3bee8657cd60aaa675e4799cd41736ebfbf627bf0633651e`; every server started from a copy |
| Build type | Release (`CMAKE_BUILD_TYPE` read from each `CMakeCache.txt`) |
| Device | data files and WAL under `/home/louis/bench-runs/db-close/` on `/dev/nvme0n1p1`, ext4 (`df -T` at the start); not tmpfs |
| Server config | `cores` 1 and 4, `durability = group`, `checkpoint_interval_ms` default 5,000 (recovery variant: 600,000), `buffer_pool_frames = 65536` (nothing evicted) except the pressure cell, 2,048; `log_level = warn`; not pinned |
| Clients | 4 closed-loop connections (Python threads, one `Connection` each) at both core counts, the premise's setup; port 15611 chosen |
| Host | 8 CPUs, 31 GiB. At 18:06:40 no `cc1plus`, `ctest` or `cmake` was running (`pgrep`). One unrelated idle `kds_server` (a web sandbox under `/home/louis/ckdbs/build-sandbox`, pid 560863, 1.1 % CPU over 4.5 days) was resident throughout, on its own port and data. Per-cell `/proc/loadavg` (1 minute, which includes my own clients) ran 1.2 - 3.9 in the first cells, which still carried the builds' tail, and 1.3 - 2.8 after; each is in the JSON. A 60 s watchdog polled the progress file's age and the driver's liveness (`watch.log` in the archive); no run orphaned |
| Quiet-gate hold | none |
| Time | builds 2 min 23 s (A and B in parallel, `-j4` each); the matrix 24 min; pressure and recovery runs 8 min; about 37 min in all. One launch of the pressure/recovery runs was killed and restarted within a minute (an empty-cells argument in my driver) and left no data |
| Raw | `bench/v3.4.14/archive/db-close-v3.4.14-5-gec057c06/` (`run1/`: the matrix and the 10K-row recovery; `run2/`, `run3/`: pressure cell and 100K-row recovery; the summarising scripts; `binary.sha256`). The driver is `tools/db_close.py` (docstring has the flags and the command); it imports `tools/db_s0_premise.py`'s workloads. It was extended with `--recovery-rows` and `--recovery-intervals` between `run1` and `run2`; `run1`'s path through it is unchanged |

## Method

Cells are the DB-S0 premise's (`bench/v3.4.14/results-db-s0-premise-v3.4.14-2-g734484ca.md`):
`shape:rows`, a fresh server and a fresh data file per cell and per build, 3 s
warm-up, then a **20 s steady window** (the premise used 25 s). Order is
A, B, A, B per cell (two rounds, each cell's A and B back to back, the full
matrix once per round), so a slow minute on the host lands on both builds.
`cores` 1 and 4. Rows by flag: `update-lit:200 | :1000 | :10000 | :100000`
is the loaded table's row count; `select-pk:10000`; `update-spill-assign:10000`
(a 300-byte `varchar` assigned); `insert:0` starts empty and grows. The
pressure cell is `update-lit:1000000:2048` (a 1M-row table, a 2,048-frame
pool), `cores = 1`, two A/B pairs (run 2 and run 3).

Counters: statements are the clients' count, every reply tag-checked;
latency is the client's per-statement wall time (p0 is the minimum); server CPU
is `utime + stime` of the server's pid from `/proc` over the window, divided
by statements (it includes the WAL writer and I/O threads, so it is higher
than `cpu.on_cpu`); log bytes per statement is the `wal_append_lsn` delta over
statements; `pool_fetch_images` is the new `sys.meta` field (a build that
predates DB has none, so A shows none); checkpoint duration is
`sys.checkpoints.duration_us` of the runs finished inside the window (4 at
`cores = 1`, about 15 at `cores = 4`); waits are `sys.wait_stats` deltas.
**Equal time, not equal work**: windows are 20 s; the ratios are per statement
and per second so the sample size does not enter them, and a slower build is a
smaller sample (counts are in the latency table).

**Noise floor, from inside the run.** Every cell ran twice per build, so
`|run 1 - run 2| / mean` is the spread of one binary against itself:
throughput up to **3.9 % (A) and 4.0 % (B)** across the 15 cells, most cells
under 1.5 %. **A throughput delta below about 4 % is not a finding.** CPU
per statement at `cores = 1` moves by up to 1.1 us between two runs of one
binary; at `cores = 4` by up to 10 us. Checkpoint duration moves up to 2.3x
between two runs of one binary at `cores = 4` (spill update at `cores = 1`:
21.1 and 49.3 ms for A).

## Throughput

`stmt/s` is client-side. The first six rows of each core count sweep the
row-set size (200 / 1K / 10K / 100K, plus the spill and insert shapes) and
the pressure row is the extreme.

| Cell | cores | A run 1 | A run 2 | B run 1 | B run 2 | A mean | B mean | B vs A |
|---|---|---|---|---|---|---|---|---|
| select-pk, 10K (control) | 1 | 32,724 stmt/s | 32,914 stmt/s | 32,906 stmt/s | 32,924 stmt/s | 32,819 stmt/s | 32,915 stmt/s | +0.3 % |
| update 200 | 1 | 4,382 stmt/s | 4,464 stmt/s | 4,361 stmt/s | 4,396 stmt/s | 4,423 stmt/s | 4,378 stmt/s | -1.0 % |
| update 1K | 1 | 4,374 stmt/s | 4,363 stmt/s | 4,289 stmt/s | 4,397 stmt/s | 4,369 stmt/s | 4,343 stmt/s | -0.6 % |
| update 10K | 1 | 4,348 stmt/s | 4,360 stmt/s | 4,385 stmt/s | 4,327 stmt/s | 4,354 stmt/s | 4,356 stmt/s | +0.0 % |
| update 100K | 1 | 4,200 stmt/s | 4,351 stmt/s | 4,184 stmt/s | 4,193 stmt/s | 4,275 stmt/s | 4,188 stmt/s | -2.0 % |
| spill update 10K | 1 | 3,770 stmt/s | 3,758 stmt/s | 3,627 stmt/s | 3,776 stmt/s | 3,764 stmt/s | 3,702 stmt/s | -1.7 % |
| insert (from empty) | 1 | 4,380 stmt/s | 4,403 stmt/s | 4,367 stmt/s | 4,338 stmt/s | 4,391 stmt/s | 4,352 stmt/s | -0.9 % |
| select-pk, 10K (control) | 4 | 30,045 stmt/s | 29,516 stmt/s | 29,887 stmt/s | 29,635 stmt/s | 29,781 stmt/s | 29,761 stmt/s | -0.1 % |
| update 200 | 4 | 5,198 stmt/s | 5,405 stmt/s | 5,197 stmt/s | 5,146 stmt/s | 5,302 stmt/s | 5,171 stmt/s | -2.5 % |
| update 1K | 4 | 5,234 stmt/s | 5,202 stmt/s | 5,231 stmt/s | 5,168 stmt/s | 5,218 stmt/s | 5,200 stmt/s | -0.4 % |
| update 10K | 4 | 5,198 stmt/s | 5,232 stmt/s | 5,152 stmt/s | 5,217 stmt/s | 5,215 stmt/s | 5,185 stmt/s | -0.6 % |
| **update 100K** | 4 | 5,217 stmt/s | 5,259 stmt/s | 4,618 stmt/s | 4,632 stmt/s | 5,238 stmt/s | 4,625 stmt/s | **-11.7 %** |
| spill update 10K | 4 | 4,546 stmt/s | 4,667 stmt/s | 4,506 stmt/s | 4,549 stmt/s | 4,606 stmt/s | 4,528 stmt/s | -1.7 % |
| insert (from empty) | 4 | 5,114 stmt/s | 5,184 stmt/s | 5,271 stmt/s | 5,244 stmt/s | 5,149 stmt/s | 5,257 stmt/s | +2.1 % |
| **update 1M, pool 2,048 frames** | 1 | 3,526 stmt/s | 3,509 stmt/s | 2,652 stmt/s | 2,604 stmt/s | 3,518 stmt/s | 2,628 stmt/s | **-25.3 %** |

Only the two bold rows are outside the noise floor, and both are outside it
by a factor of three or more, with both B runs below both A runs. The other
eleven non-control rows are inside it; their signs are mostly negative
(9 of 11), a pattern the floor does not let me call a result. Two things
bound the cells. At `cores = 1`, `group` commit with 4 clients is
sync-bound (`wal_syncs` about 2,000 a second, 2 commits per sync), which is
why every update shape sits near 4,300 stmt/s whatever the table size; at
`cores = 4` the Python client and the single log stream bound it. **The
pressure cell is the only one in which the device and the pool, not the
sync cadence, were the limit** (rule 4b), and the only one in which A and B
differ by more than noise at `cores = 1`.

## Latency

Client-side per statement, the mean of the two runs' percentiles; `n` is both
runs' statements. p0 is the best case the path reaches; at `cores = 1` its
spread between two runs of one build is up to 50 us.

| Cell | cores | build | n | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|---|
| select-pk, 10K | 1 | A | 1,312,819 | 32 us | 73 us | 100 us | 203 us | 379 us |
| select-pk, 10K | 1 | B | 1,316,648 | 32 us | 74 us | 101 us | 202 us | 364 us |
| update 200 | 1 | A | 176,927 | 456 us | 792 us | 883 us | 1,100 us | 1,360 us |
| update 200 | 1 | B | 175,140 | 428 us | 798 us | 890 us | 1,111 us | 1,437 us |
| update 1K | 1 | A | 174,761 | 461 us | 804 us | 893 us | 1,110 us | 1,377 us |
| update 1K | 1 | B | 173,732 | 442 us | 808 us | 898 us | 1,115 us | 1,404 us |
| update 10K | 1 | A | 174,171 | 442 us | 805 us | 896 us | 1,116 us | 1,399 us |
| update 10K | 1 | B | 174,255 | 438 us | 806 us | 897 us | 1,114 us | 1,386 us |
| update 100K | 1 | A | 171,026 | 430 us | 820 us | 913 us | 1,135 us | 1,434 us |
| update 100K | 1 | B | 167,549 | 479 us | 832 us | 931 us | 1,167 us | 1,491 us |
| spill update 10K | 1 | A | 150,568 | 500 us | 910 us | 1,009 us | 1,240 us | 1,561 us |
| spill update 10K | 1 | B | 148,070 | 467 us | 918 us | 1,023 us | 1,274 us | 1,673 us |
| insert (from empty) | 1 | A | 175,662 | 428 us | 788 us | 877 us | 1,091 us | 1,352 us |
| insert (from empty) | 1 | B | 174,101 | 433 us | 794 us | 883 us | 1,100 us | 1,385 us |
| select-pk, 10K | 4 | A | 1,191,272 | 41 us | 94 us | 122 us | 206 us | 336 us |
| select-pk, 10K | 4 | B | 1,190,498 | 41 us | 95 us | 123 us | 205 us | 332 us |
| update 200 | 4 | A | 212,080 | 88 us | 654 us | 733 us | 919 us | 1,184 us |
| update 200 | 4 | B | 206,868 | 106 us | 662 us | 742 us | 934 us | 1,206 us |
| update 1K | 4 | A | 208,739 | 129 us | 662 us | 741 us | 931 us | 1,199 us |
| update 1K | 4 | B | 207,998 | 141 us | 658 us | 737 us | 926 us | 1,183 us |
| update 10K | 4 | A | 208,624 | 346 us | 648 us | 726 us | 917 us | 1,215 us |
| update 10K | 4 | B | 207,403 | 343 us | 647 us | 727 us | 924 us | 1,224 us |
| update 100K | 4 | A | 209,521 | 382 us | 660 us | 741 us | 931 us | 1,189 us |
| update 100K | 4 | B | 185,003 | 397 us | 719 us | 821 us | 1,069 us | 1,415 us |
| spill update 10K | 4 | A | 184,263 | 270 us | 717 us | 805 us | 1,017 us | 1,362 us |
| spill update 10K | 4 | B | 181,113 | 138 us | 742 us | 835 us | 1,052 us | 1,364 us |
| insert (from empty) | 4 | A | 205,987 | 363 us | 658 us | 740 us | 935 us | 1,214 us |
| insert (from empty) | 4 | B | 210,306 | 351 us | 648 us | 730 us | 923 us | 1,181 us |
| update 1M, pool 2,048 | 1 | A | 140,719 | 430 us | 891 us | 996 us | 1,260 us | 1,860 us |
| update 1M, pool 2,048 | 1 | B | 105,130 | 704 us | 1,206 us | 1,329 us | 1,637 us | 2,276 us |

The whole distribution moves together where it moves: at 100K / four cores
p25 to p99 rise 9 - 19 % and at the pressure cell **p0 itself rises from 430
to 704 us**, so the image is a fixed addition to every statement there, not a
tail. Elsewhere B's p50 is 1 - 18 us above A's at `cores = 1` (0.1 - 2.0 %, largest at 100K and the spill update),
inside the floor; the p0 differences in both signs at `cores = 4` (88 vs 106
us; 270 vs 138 us) are one-statement minima over a 4-client queue.

## What a statement costs besides latency

Per-statement server CPU, log volume, images and checkpoint duration. Image
bytes are `pool_fetch_images x 8,192 / statements`; the log delta B - A
equals them to within 1 % where measured (100K / `cores = 1`: 243.5 B of log
against 242.5 B of images; the pressure cell 6,674 against 6,647), so the
32-byte record envelope is not visible here. `fetch_images` is the window's
delta for each B run.

| Cell | cores | server CPU A | server CPU B | log A | log B | image bytes a statement (B) | `fetch_images` B runs 1, 2 | checkpoint mean A | checkpoint mean B |
|---|---|---|---|---|---|---|---|---|---|
| select-pk, 10K | 1 | 22.9 us | 22.7 us | 0.0 B | 0.0 B | 0.0 B | 0, 0 | 4.1 ms | 4.5 ms |
| update 200 | 1 | 42.9 us | 43.2 us | 248.5 B | 250.9 B | 2.4 B | 26, 25 | 4.3 ms | 5.1 ms |
| update 1K | 1 | 43.4 us | 43.1 us | 248.5 B | 253.5 B | 5.0 B | 54, 52 | 4.7 ms | 5.5 ms |
| update 10K | 1 | 43.6 us | 43.6 us | 248.5 B | 274.4 B | 25.8 B | 273, 275 | 5.7 ms | 6.4 ms |
| update 100K | 1 | 44.0 us | 44.6 us | 248.9 B | 492.4 B | 242.5 B | 2,480, 2,480 | 13.9 ms | 15.2 ms |
| spill update 10K | 1 | 54.3 us | 55.0 us | 1,104.3 B | 1,176.4 B | 71.9 B | 648, 651 | 35.2 ms | 25.0 ms |
| insert (from empty) | 1 | 40.5 us | 40.2 us | 532.5 B | 535.6 B | 3.0 B | 32, 32 | 8.5 ms | 8.6 ms |
| select-pk, 10K | 4 | 32.6 us | 32.8 us | 0.0 B | 0.0 B | 0.0 B | 0, 0 | 3.9 ms | 4.0 ms |
| update 200 | 4 | 78.9 us | 81.7 us | 248.5 B | 296.9 B | 48.2 B | 595, 622 | 56.3 ms | 41.1 ms |
| update 1K | 4 | 90.7 us | 90.9 us | 248.6 B | 307.8 B | 59.1 B | 729, 770 | 48.8 ms | 56.0 ms |
| update 10K | 4 | 86.0 us | 83.3 us | 248.7 B | 397.0 B | 147.8 B | 1,936, 1,804 | 73.3 ms | 61.7 ms |
| update 100K | 4 | 98.6 us | 105.9 us | 249.8 B | 1,227.9 B | 974.0 B | 11,019, 10,977 | 46.5 ms | 65.9 ms |
| spill update 10K | 4 | 112.4 us | 113.3 us | 1,104.8 B | 1,452.1 B | 345.9 B | 3,735, 3,914 | 52.5 ms | 61.5 ms |
| insert (from empty) | 4 | 85.1 us | 72.1 us | 533.6 B | 564.9 B | 32.4 B | 393, 439 | 63.8 ms | 78.1 ms |
| update 1M, pool 2,048 | 1 | 57.6 us | 69.2 us | 249.3 B | 6,923.7 B | 6,647.3 B | 43,042, 42,264 | 194.4 ms | 58.8 ms |

Reading it, with the noise floor in hand:

- **Server CPU.** At `cores = 1` the difference is -0.3 to +0.7 us in every
  cell, inside the 1.1 us spread of one binary against itself: the image
  costs no measurable CPU where it is rare. At `cores = 4` it is inside the
  10 us spread except at 100K (A 98.8 / 98.3, B 106.3 / 105.6 us, +7.5 us,
  consistent across both rounds, a hint rather than a result) and at the
  pressure cell, where A is 58.3 / 56.8 and B 69.6 / 68.9 us: **+11.6 us a
  statement** (about 0.8 of an image a statement, so about 14 us CPU an
  image, an 8 KiB copy plus the record's append and checksum).
- **Log bytes.** The log grows by exactly the image volume: +1 % at update
  200 / 1K (`cores = 1`), +10 % at 10K, **+98 % at 100K**, +6 % on the spill
  update, +1 % on the insert; at four cores +19 % to +391 % across the same
  update sizes (the checkpoint cadence is per core, so each leaf is copied
  more often - the premise's finding 2 holds); the pressure cell is **28x**
  (6,924 against 249 B a statement).
- **Checkpoint duration.** No effect is visible. B is 0.1 - 1.3 ms longer at
  `cores = 1` in five cells of seven and shorter in the spill cell; at
  `cores = 4` it differs in both signs by up to 2x between two runs of one
  binary. At the pressure cell A's mean is 194 ms and B's 59 ms, the
  opposite of the direction the image volume suggests: A's four runs in the
  window were 57, 29, 316 and 328 ms (run 2) and B's 79, 48, 67 and 28 ms;
  the per-run values are in the archive, and with four runs a window and
  that spread this is not a finding either way. I did not look for the
  cause.

## Where the time goes

A latency is a sum. In these closed-loop cells it is (4 clients' worth of)
the client's own work and socket round trip, the log sync the commit waits
for, the reactor's run queue and the statement's CPU, plus any page read.
`sys.wait_stats` names the engine's waits; the client and socket share is
the remainder of the client's mean, so **it also holds the queueing behind
the other three clients** (a closed loop of 4 has a mean of about 4 /
throughput) and is not measured on its own. Page write waits did not exist as
a wait type in either build's output for these cells (the drain and the
checkpoint's I/O are not on the statement's path); `lock` and `conflict`
waits were absent in every cell shown. Means of the two runs (run 2 and 3
for the pressure row).

| Cell | cores | build | client mean | `commit.wal_flush` | `sched.run_queue` | `cpu.on_cpu` | `io.page_read` | client, socket and queueing (remainder) | flush share of the mean |
|---|---|---|---|---|---|---|---|---|---|
| select-pk, 10K | 1 | A | 121 us | 0 us | 0 us | 9 us | 0.0 us | 111 us | 0 % |
| select-pk, 10K | 1 | B | 121 us | 0 us | 0 us | 9 us | 0.0 us | 111 us | 0 % |
| update 10K | 1 | A | 917 us | 422 us | 34 us | 16 us | 0.0 us | 445 us | 46 % |
| update 10K | 1 | B | 917 us | 424 us | 34 us | 16 us | 0.0 us | 443 us | 46 % |
| update 100K | 1 | A | 935 us | 432 us | 34 us | 16 us | 0.0 us | 453 us | 46 % |
| update 100K | 1 | B | 954 us | 440 us | 34 us | 16 us | 0.0 us | 464 us | 46 % |
| spill update 10K | 1 | A | 1,061 us | 493 us | 42 us | 21 us | 0.0 us | 506 us | 46 % |
| spill update 10K | 1 | B | 1,080 us | 496 us | 41 us | 22 us | 0.0 us | 521 us | 46 % |
| insert (from empty) | 1 | A | 910 us | 424 us | 31 us | 13 us | 0.0 us | 442 us | 47 % |
| insert (from empty) | 1 | B | 918 us | 429 us | 31 us | 13 us | 0.0 us | 445 us | 47 % |
| update 100K | 4 | A | 762 us | 627 us | 1 us | 34 us | 0.0 us | 100 us | 82 % |
| update 100K | 4 | B | 864 us | 720 us | 1 us | 38 us | 0.0 us | 105 us | 83 % |
| update 1M, pool 2,048 | 1 | A | 1,136 us | 513 us | 96 us | 20 us | 3.4 us | 504 us | 45 % |
| update 1M, pool 2,048 | 1 | B | 1,521 us | 715 us | 70 us | 22 us | 3.4 us | 710 us | 47 % |

**The wait that moves is the commit's log sync.** Where B is slower, the
increase is almost all `commit.wal_flush`: +93 us at 100K / four cores (of
the 102 us mean increase) and +202 us at the pressure cell (of 385 us). The
sync has more to write - 2 commits a sync carry about 14 KB of images at
the pressure cell, against about 0.5 KB - and the sync's duration rises with
it (the flush wait per commit 513 to 715 us), while commits per sync stay 2.0
and `wal_ring_full` stays 0 in both builds. The pressure cell's remainder
also rises by 206 us; that is not a named wait, it is the closed loop: each
of 4 clients waits behind the others' longer syncs (4 / 2,628 stmt/s = 1.52
ms against 4 / 3,518 = 1.14 ms), and the CPU rise of 11.6 us a statement is
the only server-side non-wait addition. `io.page_read` is the same 3.4 us in
both: the image does not add read misses (1.38 misses a statement in both
builds). `cpu.on_cpu` is 16 us of the 43 us a statement the process burns at
`cores = 1`; the rest is the WAL writer, the network threads and the kernel.
`sched.run_queue` falls at the pressure cell (96 to 70 us) because the longer
sync leaves the reactor less to run.

## The extreme: where the pool is the limit

Table three times the pool (6,060 pages by the premise; 1M rows, 2,048
frames), uniform updates, `cores = 1`. Two A/B pairs; the pairs agree (A
3,526 and 3,509, B 2,652 and 2,604 stmt/s).

| Quantity | A (2 runs) | B (2 runs) |
|---|---|---|
| Throughput | 3,526, 3,509 stmt/s | 2,652, 2,604 stmt/s |
| p0 / p25 / p50 / p90 / p99 (4 runs' mean) | 430 / 891 / 996 / 1,260 / 1,860 us | 704 / 1,206 / 1,329 / 1,637 / 2,276 us |
| Log per statement | 249.3 B | 6,923.7 B |
| `pool_fetch_images` per window | none | 43,042, 42,264 |
| Pages written by the eviction drain + the checkpoints | 56,824, 56,638 | 43,041, 42,254 |
| Drain per statement, misses per statement | 0.74, 1.38 | 0.72, 1.38 |
| Syncs per second | about 1,760 | about 1,330 |

**`fetch_images` equals the pages written to the device, to within 10 in
43,000**: every page the drain or the checkpoint writes is modified again
and therefore imaged once. That is the premise's upper bound (it assumed
every written page is re-modified), and in this regime the bound is met. The
A - B difference in pages written (56.8k vs 43.0k) is only the shorter
window's worth of statements (the per-statement drain rate is the same,
0.74 and 0.72). The premise projected 6,601 image bytes a statement for this
cell; the measured figure is 6,647.

## Recovery

One fixed log per variant: the table is loaded, a fixed count of point
UPDATEs run across 4 clients (equal work), the server is `kill -9`'d, and the
mount time is the wall time from exec to the first accepted connection. The
server's own startup (exec, reading the superblock and recovery) is in it:
the floor is about 0.10 s. After the restart the table's row count was
checked (all rows present in every run: 10,000 or 100,000); the updated
values were not compared. Unlike the other tables, `pool_fetch_images` was not
the subject here.

| Variant | statements before the kill | log at the kill (A / B) | runs | mount A | mount B |
|---|---|---|---|---|---|
| 10K rows, default cadence (5 s) | 40,000 | 14.63 MB / 15.22 MB | 3 | 0.115, 0.110, 0.110 s | 0.111, 0.110, 0.126 s |
| 10K rows, no checkpoint (600,000 ms) | 40,000 | 14.63 MB / 14.66 MB | 3 | 0.120, 0.116, 0.121 s | 0.116, 0.121, 0.116 s |
| 100K rows, default cadence | 160,000 | 86.65 MB / 122.34 MB | 2 | 0.105, 0.100 s | 0.116, 0.110 s |
| 100K rows, no checkpoint (600,000 ms) | 160,000 | 86.60 MB / 86.63 MB | 2 | 0.341, 0.348 s | 0.343, 0.348 s |

**Recovery time did not change measurably.** With no checkpoint the whole
log replays (86.6 MB, 0.24 s above the 0.10 s floor) and A and B are within
0.007 s of each other. With the default cadence, only the log after the last
checkpoint replays and B is 0.010 - 0.011 s slower in the 100K variant (two
runs each, the same size as A's own 0.005 s spread, so I do not call it a
finding); the 10K variant shows nothing. Two limits on what this says. In
the no-checkpoint variants nothing is written back, so B logs no images and
its log is the same size as A's: **that variant exercises B's recovery code
on a log with no images**. In the default-cadence variants B's log is 41 %
larger at 100K (122.3 against 86.7 MB), but redo starts after the last
checkpoint, so the window replayed holds at most 5 s of it. A log that
replays many images was not measured, and neither was a torn page being
healed (that is DB-S2's corpus, `sim/`). The data file's WAL directory was
64 MiB for A and 128 MiB for B at 100K / default (the segment count at the
kill, `wal_dir_bytes`).

## What this says about the engine

1. **The price of an image is the number of page writes, and the statement
   rate does not enter.** Image bytes a statement are `writes of that page
   set / statements`: 2.4 B at 200 rows, 25.8 B at 10K, 243 B at 100K, and
   6,647 B when every statement faults, modifies and evicts a page. The
   premise's projection (finding 3: 27x at the pressure cell) held to 1 %
   (6,601 projected, 6,647 measured), and 100K / `cores = 1` to 4 % (250.5,
   242.5). **The premise's upper bound is loose where pages are not modified
   again after a write**: it projected 385 B a statement for the spill update
   at 10K (measured 72) and 52.6 B for the insert (measured 3.0), because the
   spill's new var-heap pages and the insert's filled leaves are written once
   and never touched again; and loose at four cores for 10K (178 projected,
   148 measured). Where the premise was a bound and not an estimate it is
   met only by the 1M cell.
2. **Throughput follows log volume only once the sync is long enough to
   notice.** +98 % log (100K, `cores = 1`) cost nothing measurable (-2.0 %,
   inside the floor, where the sync adds 8 us of 432); +391 % log at 100K /
   four cores cost 11.7 %; +2,677 % at the pressure cell cost 25 %.
   Latency is dominated by the sync, 46 % of a one-core statement and 82 %
   at four cores, so the image enters latency through the sync's length and
   nowhere else. The log device did not become a bound in any cell (the ring
   never filled, `wal_ring_full` 0 everywhere); the cost is a longer sync,
   and the drain's own writes sharing the device.
3. **The cost sits where DB-Q1 priced it.** DB-Q1's "cost on the statement
   that first touches a page after it was written" is, in numbers, 0.81
   images a statement at the pressure cell and 0.003 - 0.03 at one core in
   the standard cells: a 1 - 2 % effect on update throughput at one core that
   the noise floor cannot confirm, an 11.7 % effect where four cores copy a
   100K-row hot set every 1.25 s, and 25 % where the pool is smaller than the
   working set. The decision it leaves for the operator is whether the
   pool-smaller-than-working-set and many-core regimes are the regimes
   financial OLTP runs in; this run cannot say.
4. **The control is clean.** The point `SELECT` (32.8K and 29.8K stmt/s):
   no change in throughput (+0.3, -0.1 %), latency, CPU or checkpoint, no
   images, as a read takes no exclusive fetch.
5. **Server CPU at four cores varies by 10 us between two runs of one
   binary**; its cause was not investigated, and it is why this file reads CPU only at one core
   and at the one cell where the effect is 11 us.

## What was cut, and what was not measured

- Two runs per cell and build (A, B, A, B); the pressure cell two A/B pairs
  (runs 2 and 3, a minute apart, not interleaved with the matrix). 20 s
  windows, so a window holds 4 checkpoints at `cores = 1`: a run starting or
  ending at the edge is a quarter of that column (the checkpoint-duration
  columns are read with that).
- No `strict` or `relaxed` cell (DB-Q5 names the standard cells, which are
  `group`); a `strict` commit, whose sync is the whole latency, would show the
  sync's lengthening most and was not run. No `cores = 4` pressure cell. The
  select-pk control is at 10K rows only (a pk point lookup does not scale
  with rows); the 200 / 1K sweep is on the update shape; the spill and
  insert shapes have one size each, as in the premise.
- The premise's 100K-row spill table, which the 2,048-frame pool refuses to
  load, was not run.
- Recovery: no log with many images replayed, no torn page healed (DB-S2's
  corpus does that), updated values not compared after the restart, the
  start-to-listen time rather than a time inside the server.
- **No prior number exists for this shape** with latency percentiles
  (the premise reported volume only), so A is this engine's baseline and
  the next run reads against B. The PostgreSQL floor was not measured (the
  question here is the price of a page image, which has no twin).
- The tests: no engine code changed in this measurement (the driver is
  `tools/db_close.py`), so the suite was not run for it; it is the DB
  milestone's own step runs that carry it. A `--verify` mode does not exist
  in this driver; every reply's tag was checked, with `bad` = tags other than
  the expected one: at `cores = 1` 0 in every cell; at `cores = 4`, 39
  across A's runs and 34 across B's, of about 2.4 million statements a build at four cores, the
  premise's own 0.02 % class of refusal at four cores, equal in the two
  builds.
