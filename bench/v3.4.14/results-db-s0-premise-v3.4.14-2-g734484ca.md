# DB-S0 premise census: page writes against log bytes

What DB (`instructions/v3.2.0/workorder-db-torn-page-healing.md`) will log
is one 8,192-byte `FULL_PAGE_IMAGE` per logged page at its first exclusive
fetch after the page was copied for a device write (or loaded). This census
measures the quantity that sets that volume - **pages written per second** -
beside the log bytes per second the same workload writes today. No engine
code changed; a new driver, `tools/db_s0_premise.py`, was added (its flags
and the exact command are in its docstring; `bench/docs/` does not exist and
`bench/README.md` keeps it closed).

## Stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19, 12:53 - 13:02 UTC |
| Version | `v3.4.14-2-g734484ca` |
| Branch / worktree | `worktree-db-torn-page-healing`, `/home/louis/ckdbs/.claude/worktrees/db-torn-page-healing` |
| Commit | `734484ca`; tree clean but for the untracked new driver `tools/db_s0_premise.py` |
| Binary | `build-release/kds_server`, Release, built in this worktree at the commit; source mtime 2026-11-19 12:53:25 UTC (HEAD committed 08:04 UTC, so not older than HEAD); measured copy `/home/louis/bench-runs/db-s0/kds_server`, sha256 `68fa69928a1f455335625172c5f18ab1a941bbda892fb1a2c6ef41abf7469b30`; every server started from the copy |
| Device | data file and WAL under `/home/louis/bench-runs/db-s0/` on `/dev/nvme0n1p1`, ext4 (checked with `df -T`); not tmpfs |
| Server config | `cores` 1 and 4, `durability = group`, `checkpoint_interval_ms` default 5,000, `buffer_pool_frames = 65536` (nothing evicted) except the pressure cells, `log_level = warn`; not pinned |
| Clients | 4 closed-loop connections (Python threads, one `Connection` each) at both core counts, so `cores` is the only thing varied between the two blocks |
| Host | 8 CPUs; load average 1.5-4.7 across cells (the first cell still carried my build's tail; 1.5-2.9 after). No other build, suite or benchmark was running from the first cell on (the other session's `ctest -j 6` ended while I built). `hostload.log`'s `pgrep` hit at the `cores = 4` pre-run is the driver's own shell. Per-cell loadavg is in the JSON |
| Quiet-gate hold | none after the build: the other session's suite ran during my `make -j4` and was gone when it finished (12:53:25). Time spent: build 131 s; measurement 12:53:57 - 13:02:16 (8.3 min, the pressure cells included) |
| Raw | `bench/v3.4.14/archive/db-s0-premise-v3.4.14-2-g734484ca/` (JSON lines, progress logs, host load, the summary scripts) |

## Method, and what the counters are

Per cell: a fresh server and data file, the table loaded, 4 s warm-up, then
a **25 s steady window** (5 cadence periods). Counters were read from
`SHOW META` and `sys.checkpoints` immediately before and after the window;
`statements` is the clients' count, every reply tag-checked.

| Quantity | Source | Note |
|---|---|---|
| Log bytes | `wal_append_lsn` delta | an LSN is a byte position; includes commit records and the checkpoints' own records |
| Log bytes / records of the statement | `sys.statement_stats` `log_bytes`, `log_records` for the shape | the statement's own records; within 0.1% of the LSN delta in every cell |
| Checkpoint pages written | `contention_checkpoint_pages_written` (`Tally::kCheckpointPagesWritten`) | **steps plus the publish**: the publish rewrites every page dirty at that moment (`checkpointer.cpp` ~619), so a page re-dirtied during a run is written twice in it. `sys.checkpoints.pages_flushed` is the steps alone and is lower (25 against 44 at update-lit 200, `cores = 1`). The counter is what an image volume follows, so it is the one tabulated |
| Writeback outside the checkpoint | `pool_dirty_drained` (`dirty_drained_`, the eviction drain) | `Flush()` and `PersistPage` (shutdown, `SYNC`) never run inside a steady window |
| Checkpoints | `contention_checkpoint_runs` delta | **runs of every core**: at `cores = 4` each core runs the cadence, 20 runs per 25 s, not 5 |
| Table size | `pages_allocated` at the window's end | whole-run figure (load and warm-up included) |

**Warm-up residue, subtracted.** The first run inside a window also writes
what the load and warm-up left dirty (select-pk: 71 pages in run 1, then 1
per run). The counter has no per-run split, so each cell's counter total is
reduced by an estimated excess: `pages_flushed` of the window's first run
minus the median of its other runs. Raw totals are in the JSON; the tables
use the corrected figure. This is a derivation, not a measurement; the
correction is at most 4% of any update or insert row, except the `cores = 4`
spill row (11%) and the control.

Projected image bytes = (checkpoint pages + writeback pages) x 8,192; the
32-byte record envelope (under 0.4%) is left out. Growth factor =
(log B/s + image B/s) / log B/s. **It is an upper bound for placement F** at
the standard cells: F logs an image once per page per copy only if the page is
modified again afterwards, and the count here assumes every written page is.

## Result: the whole table

`stmt/s` is client-side. Row counts: 200 / 1K / 10K / 100K are the loaded
table's rows (the driver's `shape:rows`); the insert cell starts empty. `log
B/stmt` is the LSN delta over statements, commit record included. `ckpt
pages/run` is the corrected counter over `ckpt runs`.

| Cell | cores | stmt/s | log B/s | log B/stmt | rec/stmt | ckpt runs | ckpt pages/run | ckpt pages/s | writeback pages/s | image B/s | growth factor | image B/stmt |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk, 10K rows (control) | 1 | 33,091 | 53 | 0 | 0 | 5 | 3.0 | 0.6 | 0 | 4,915 | n/a (no log) | 0.1 |
| update-lit, 200 rows | 1 | 4,225 | 1,049,939 | 248.5 | 4.01 | 5 | 7.8 | 1.6 | 0 | 12,779 | 1.01 | 3.0 |
| update-lit, 1K rows | 1 | 4,070 | 1,011,294 | 248.5 | 4.01 | 5 | 14.8 | 3.0 | 0 | 24,247 | 1.02 | 6.0 |
| update-lit, 10K rows | 1 | 4,107 | 1,020,761 | 248.5 | 4.01 | 5 | 69.2 | 13.8 | 0 | 113,370 | 1.11 | 27.6 |
| update-lit, 100K rows | 1 | 4,064 | 1,011,484 | 248.9 | 4.01 | 5 | 621.4 | 124.3 | 0 | 1,018,023 | 2.01 | 250.5 |
| update-spill-assign, 10K rows | 1 | 3,675 | 4,058,261 | 1,104.3 | 6.10 | 5 | 863.6 | 172.7 | 0 | 1,414,861 | 1.35 | 385.0 |
| insert (named keys, one shared counter) | 1 | 4,227 | 2,248,697 | 532.0 | 5.00 | 5 | 135.6 | 27.1 | 0 | 222,154 | 1.10 | 52.6 |
| select-pk, 10K rows (control) | 4 | 29,759 | 135 | 0 | 0 | 20 | 3.0 | 2.4 | 0 | 19,660 | n/a (no log) | 0.7 |
| update-lit, 200 rows | 4 | 5,237 | 1,301,595 | 248.5 | 4.01 | 20 | 50.0 | 40.0 | 0 | 327,988 | 1.25 | 62.6 |
| update-lit, 1K rows | 4 | 5,211 | 1,295,264 | 248.6 | 4.01 | 20 | 60.1 | 48.1 | 0 | 393,852 | 1.30 | 75.6 |
| update-lit, 10K rows | 4 | 5,134 | 1,276,792 | 248.7 | 4.01 | 20 | 139.3 | 111.5 | 0 | 913,195 | 1.72 | 177.9 |
| update-lit, 100K rows | 4 | 5,237 | 1,307,700 | 249.7 | 4.01 | 20 | 678.9 | 543.0 | 0 | 4,448,507 | 4.40 | 849.4 |
| update-spill-assign, 10K rows | 4 | 4,618 | 5,101,824 | 1,104.7 | 6.10 | 20 | 492.2 | 393.8 | 0 | 3,225,838 | 1.63 | 698.5 |
| insert (named keys, one shared counter) | 4 | 4,822 | 2,566,680 | 532.3 | 4.99 | 20 | 72.8 | 58.2 | 0 | 476,748 | 1.19 | 98.9 |

The extreme, where the pool is the limit (`cores = 1`; the 1M-row table is
6,060 pages by `pages_allocated`, the pool 2,048 frames, so about a third of
it fits):

| Cell | cores | stmt/s | log B/s | log B/stmt | ckpt runs | ckpt pages/s | writeback pages/s | image B/s | growth factor | image B/stmt |
|---|---|---|---|---|---|---|---|---|---|---|
| update-lit, 1M rows, pool 2,048 frames | 1 | 3,440 | 857,451 | 249.3 | 5 | 237.8 | **2,533.8** | **22,704,780** | **27.5** | 6,601 |
| update-spill-assign, 30K rows, pool 3,072 frames | 1 | 3,679 | 4,063,736 | 1,104.7 | 5 | 227.9 | 3.4 | 1,894,716 | 1.47 | 515 |

The second row is barely pressured (84 pages drained; the table grew to
5,716 pages by new var-heap values) and is shown as what it is. A 100K-row
spill table with a 2,048-frame pool did not load (`buffer pool full ...
another mutation's 64 frames`, the load statement's own size) and was not
retried.

Cells are equal-time (25 s), not equal-count; every ratio above is per
second or per statement, so the sample size does not enter it. No repeat was
run, so there is no noise floor from inside the run: these are volume
counters, one run per cell, and the checkpoint-boundary effect (a window
holds 5 runs at `cores = 1`, so one run starting or ending at the edge is up
to a fifth of that column) is the uncertainty to read the `cores = 1`
checkpoint columns with. The 200/1K/10K/100K sweep moves them by far more.

## Findings

**1. At the standard cells the image volume is a minority of the log, and
what sets it is the hot page set, not the statement rate.** At `cores = 1`
the point UPDATE writes ~1.0 MB/s of log (248.5 bytes and four records a
statement) at ~4,100 statements/s, and the checkpoint writes only the pages
those statements dirtied in the period: 7.8 pages a run at 200 rows, 14.8 at
1K, 69 at 10K, 621 at 100K - the table's own leaf count (`pages_allocated`
26, 33, 87, 629), since ~20,000 statements a period far exceed the leaves.
Image volume is `min(pages dirtied per period, table pages) / 5 s x 8,192`:
0.01, 0.02, 0.11 and 1.02 MB/s, growth 1.01x to 2.0x. The spilled-row UPDATE
(a var-heap value written: 6.1 records, 1,104 bytes a statement) dirties
~860 pages a period at 10K rows, 1.4 MB/s of images over 4.1 MB/s of log,
1.35x; its table grew from the loaded pages to 4,646 (new var-heap values).
The INSERT appends at one tail leaf: 5 records, 532 bytes a statement, ~136
pages a period, 0.22 MB/s, 1.10x. Its 52.6 image bytes a statement is a
fill rate - each leaf is written once or twice as it fills - not an
overwrite rate.

**2. At `cores = 4` the checkpoint writes the hot set about four times as
often.** The cadence is per core: 20 runs in 25 s, 5 from each of cores 0-3
(`sys.checkpoints.core`), not 5. Each run writes what is dirty then, and at
~5,200 updates a second over a few hundred leaves every leaf is re-dirtied
between two runs ~1.25 s apart. update-lit 100K goes from 124 to 543
pages/s (4.4x) with the same table and nearly the same statement rate (4,064
to 5,237), and the growth factor from 2.0x to 4.4x (4.45 MB/s of images over
1.31 MB/s of log); at 10K rows 1.11x to 1.72x. The small tables rise most
(200 rows: 1.6 to 40 pages/s) because a run with a handful of dirty pages
is mostly the publish's rewrite of what the steps just wrote (by the code;
the counters show steps of 4-5 pages against ~7.8 counted at `cores = 1`).
**The image volume of an F placement scales with `cores / checkpoint_interval`
times the hot set, not with the statement rate**: the 100K row at four cores
is a whole-table rewrite per run (679 pages a run, against the 629 pages the
`cores = 1` run of the same table allocated).

**3. Eviction writeback is the shape that changes the answer: 27x.** With a
table three times the pool and uniform updates, the checkpoint still writes
~238 pages/s but the eviction drain writes **2,534 pages/s** (63,347 in 25
s against 118,426 pool misses), 10.7 times the checkpoint's rate. That is
22.7 MB/s of projected images over 0.86 MB/s of log, **27.5x**, 6.6 KB of
image per 249-byte statement. In this regime a statement faults a leaf in,
modifies it, and the eviction writes it out a moment later, so every
modification is "the first exclusive fetch after a load or a copy": F's image
count approaches the statement count here (3,440 a second, against 2,534
pages written a second), not the pages-per-period bound of finding 1, and
the page-write rate is bounded by the pool and the device, not the cadence.
(Statement rate here, 3,440 a second against ~4,100 in the other update
cells, is a different table and pool, not a measured cost of anything.) A
log carrying 22.7 MB/s of images turns the 1 MiB ring over ~22 times a
second. This is the regime DB-Q1's "cost on the statement that first touches
a page after it was written" is priced against.

**4. The control is clean.** Point SELECT: 33,091 stmt/s (29,759 at four
cores; bound by the Python driver), log 53 and 135 B/s (the checkpoints'
own records), checkpoint pages 3 a run in steady state (0.6 and 2.4 pages/s,
5-20 KB/s of projected images). Those ~3 pages a run are not table pages -
a read dirties none - and the counters cannot name them (page 0, the anchor
page and the maps are the candidates, the classes DB-Q2/DB-Q3 treat
separately). No growth factor exists because there is almost no log.

**5. What bounded the cells.** At `cores = 1`, `group` commit at 4 closed-loop
clients: ~2,030-2,110 log syncs a second, 2 commits a batch, so ~4,100
statements a second whatever the shape (the spill cell is lower, 3,675, its
statement is dearer). At `cores = 4` the clients (four Python threads under
the GIL) and the single log stream bound it, 4.6-5.2K/s. Neither the
device's page-write bandwidth nor the ring was approached in the standard
cells (`wal_ring_full` 0 in every cell). `wal_syncs` and the batch counters
are core 0's alone, so they are not tabulated for `cores = 4`. The
pool-pressure cell is the one that reached the device.

## The ring against one statement's pages

The WAL ring is 1 MiB (`include/kds/wal/stream.hpp` ~146): 127 images of
8,224 bytes. Pages a statement dirties in these cells are not counted by any
counter and not measured here; the nearest bound is the records it appends
(4.0 update-lit, 5.0 insert, 6.1 spilled update, 249 to 1,104 bytes), each
naming at most one page and some of them commit or undo records. So at most
4-6 pages, 33-49 KB of images at F's placement, 3-5% of the ring, and
probably fewer. The largest a statement writes anywhere is a **split**, up
to `kMaxBtreeSplitPages` = 34 pages, which `BTREE_SPLIT` already carries as
full images in one 278,840-byte record (26.6% of the ring, `wal.md` §4.1).
The checkpoint's 64-page step is 526 KB of images (half the ring) if it were
all imaged at once; F places images on the later statements that touch the
pages, not at the copy, so that burst is spread over statements.

## What the numbers say about the engine

- The 5 s cadence sets the volume as the age of the dirty set: a page is
  written at most once per run in the steps and once in the publish, so
  `pages/s <= min(dirty set, table pages) x runs/s` (~0.2 runs/s per core,
  ~0.8/s at four). That follows from the per-core cadence, not from any DB
  choice, and DB-Q1's F and W both inherit it.
- Today's log is statement-dominated (0.25-1.1 KB a statement). Images add
  1.0x-4.4x at the standard cells, rising with `cores` and the hot set and
  with nothing else, and 27x once the pool is smaller than the working set.
- Under pool pressure the eviction path dominates (10.7x the checkpoint) and
  couples to the statement rate. DB-Q1 (c) ("neither until DB-S0 has priced
  F's statement-path cost") now has its first data point on volume: small at
  the standard cells, more than an order of magnitude at a working set larger
  than the pool. F against W matters most exactly there: F's images follow
  first modifications after a load, W's the device writes. The counters do
  not distinguish the two volumes in that regime beyond "both are large".
- The throughput cost of an image on a statement was not measured (no
  code), and DB-Q5 sets no bar; these are volume counters only.

## What the counters could not answer

- Pages a statement dirties: no counter; bounded above from records per
  statement.
- A per-run split of `contention_checkpoint_pages_written`: only the window
  total exists (`sys.checkpoints.pages_flushed` is the steps alone), so the
  warm-up excess is estimated.
- What the control's ~3 pages a run are: no counter by page class.
- DB-Q2's class split (how many written pages are logged-headered): not
  answerable from counters; every page written is counted.
- `wal_syncs` and group-batch counters above one core (core 0's only).
- Statements whose reply tag was not `UPDATED 1`: 31 of 130,925 at
  update-lit 200 / `cores = 4` (0.02%), 6, 1 and 5 in three other
  `cores = 4` cells, 0 elsewhere. The driver did not keep the error text; they
  stay in the statement counts.
- No prior number: this is the first census of this shape, so there is no
  delta against this engine's own history, and the PostgreSQL floor was not
  measured (the question is a page-write count, which has no twin).

## What was cut

One run per cell (no repeat, no in-run noise floor); servers unpinned; no
`strict` or `relaxed` cell (DB-Q5 (a) names `group`); the update-lit shape
carries the 200 / 1K / 10K sweep plus a 100K extreme, while select-pk,
spill-assign and insert have one row count each (10K, 10K, growing); no
100K-row spill table (the load was refused by the pool); the pool-pressure
cells are `cores = 1` only and the second of them was not pressured enough to
reach the eviction path; a `cores = 4` pressure cell was not run.
