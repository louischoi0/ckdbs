# The T1 multi-row INSERT, priced — batch 1/10/100/1000 × three durability classes, against PostgreSQL 17.10

`docs/spec-bulkinsert.md` §0 claimed the single-row INSERT ceiling of ~9K
rows/s was ~21 µs/row of statement cost that never amortizes, and shipped T1
(BLK01–BLK05) as that cost's eviction notice. This run measures what T1
actually evicted, on the same `write_probe` row shape the baseline
(`bench/results-scenario1-vs-pg.md`) used. Driver documentation:
[`bench/docs/README.md`](docs/README.md), `bulk_insert_benchmark.py` /
`pg_bulk_insert_benchmark.py`.

**Thesis: T1 works as designed — it removes the round trip entirely and cuts
the parse to ~1.1 µs/row, leaving a flat per-row cost of ~4 µs against the
old path's ~113 µs — and in doing so it exposes the next wall, which is not
statement cost at all: every heap insert walks the page chain from the root
to find the tail (`heap::ChainInsert` → `ChainTail`), an O(pages-resident)
cost of ~0.32 ns × resident rows per row that comes to dominate a bulk load
beyond ~12K resident rows and is the single reason PostgreSQL wins the large
batches.** Two engine defects were found and are reported below: the tail
walk, and a WAL stream that wedges permanently when an append exactly fills
a 64 MiB segment.

---

## The run

| | |
|---|---|
| executed | 2026-08-10, 04:32–05:53 UTC (ckdbs matrix 04:32–05:33 with gated re-runs, PostgreSQL 05:34–05:38, A/B 05:43–05:48, trace runs 05:49, device probe 05:53 — sequential, never concurrent) |
| commit measured | `9ee04e4` (`bulk-insert: T1 multi-row VALUES - the full pipeline per row (BLK01-BLK05)`, committed 04:16:03 UTC, tip of `feat-bulkinsert` — also on `main`) |
| binary | `build-release/kds_server`, built 04:23:59 UTC from `9ee04e4` with a clean tree, and **never rebuilt during the session** — verified by mtime against the worktree reflog in the next row |
| worktree caveat | a concurrent agent switched this worktree to `feat-drop-table` at 04:30:00 UTC (reflog), committed DT01–DT05 at 05:05 and merged to `b08e6b6` — so the *checked-out tree* moved mid-session while the *measured binary* did not. Every ckdbs number in this file is `9ee04e4`'s engine: the binary predates the switch, and `git diff 9ee04e4 b08e6b6 -- tools/` shows none of the shared driver files (`bench_common.py`, `ckdbs_cli.py`, `pg_wire.py`) changed under the runs. The session's own additions (`tools/bulk_insert_benchmark.py`, `tools/pg_bulk_insert_benchmark.py`, this file) are untracked; the session's `bench/docs/README.md` rows were swept into the other agent's commit `20d5dca` |
| build type | Release (`CMAKE_BUILD_TYPE:STRING=Release` in `build-release/CMakeCache.txt`) |
| tests | not run in this session — no engine code was changed; correctness was verified in-run (every reply's `rows=` field, the Keystone id span, and `SELECT COUNT(*)` after every phase, all exact, zero error replies in every reported configuration) |
| device | `/dev/nvme0n1p1`, xfs, non-rotational, 256 GB. Data files `/home/ec2-user/bench-bulk/*.db`, WAL alongside. Raw 8 KiB write+fsync on this volume, 300 samples at run end: p0 875, p25 918, **p50 940**, p95 2,026, p99 2,108 µs (the 2026-08-07 baseline measured 927–930 — the device did not move) |
| machine | 2 vCPU, 7 GiB RAM, **shared with other agent processes that burst unpredictably**. Every configuration below ran behind a load gate (start only at 1-min load ≤ 0.60, except one flagged case) and records load before and after; contaminated attempts were discarded and re-run, and the discards are listed in the noise-floor section |
| ckdbs config | `cores = 1`, `isolation = read committed`, `max_insert_rows = 1024` (default), all other keys default; `durability` set per configuration (`relaxed` / `group` / `strict`) via `--config`. Superblock format version 13 |
| PostgreSQL | 17.10, `tools/pg_setup.sh` cluster on port 15433, untouched defaults (`synchronous_commit = on`, `shared_buffers = 128MB`). The `relaxed` twin is `SET synchronous_commit = off` — a session GUC, i.e. the same per-transaction durability property ckdbs exposes, not cluster tuning |
| protocol | one server, one data file, one fresh relation **per (durability × batch) configuration** — 100,000 rows each, so every throughput number is equal work. Fresh-server-per-configuration is also forced by the WAL segment bug reported below |
| relation | scenario1's `write_probe` shape: `(id int64, a int64, b int64, c int64, d int64) HEAP`, four int64 values per row, engine-issued pk |

## The noise floor, established from inside the run

The whole relaxed configuration was run twice (runs A and B), each
configuration on a fresh server and file, in separate clean windows:

| measurement | run A | run B | spread |
|---|---:|---:|---:|
| bulk-1 rows/s | 7,174 | 7,170 | 0.06 % |
| bulk-10 rows/s | 31,069 | 31,102 | 0.11 % |
| bulk-100 rows/s | 48,346 | 46,688 | 3.5 % |
| bulk-1000 rows/s | 51,258 | 50,181 | 2.1 % |
| PING p50 (all servers) | 89.8–91.6 µs | | 2.0 % |

**Nothing under 4 % is reported as a finding**, and single-run
between-window comparisons (the strict rows, the group−relaxed differences
at batch ≥ 100) get a wider berth, stated where used. The client floor's
*median* is stable but its distribution is bimodal (PING p0 ≈ 42 µs against
p50 ≈ 91 µs), which caps how finely any batch-1 latency can be decomposed —
±25 µs is honest resolution there.

Discarded measurements: the first full matrix ran into a sibling process's
load burst (1-min load up to 11.4) and every configuration measured under it
was thrown away and re-run behind the load gate — among the discards,
bulk-10 relaxed read 5,397 rows/s against the clean 31,069, a 5.8× error
with nothing in the driver's own output to flag it. One retained
configuration (group-b100) started at 1-min load 0.91 decaying from an
earlier burst with both CPUs otherwise idle; its number sits consistently
between its neighbors and is retained with this note.

## The matrix: throughput

100,000 rows per cell, single connection, rows/s (statements/s × batch).
Rates, not distributions — the percentile tables follow.

| rows/statement | ck relaxed | ck group | ck strict | pg sync=off | pg sync=on |
|---:|---:|---:|---:|---:|---:|
| 1 | 7,174 | 927 | 930 | 5,503 | 878 |
| 10 | 31,069 | 7,716 | — | 36,450 | 8,080 |
| 100 | 48,346 | 40,065 | — | 87,920 | 45,630 |
| 1,000 | 51,258 | 42,339 | 48,945 | 94,600 | 81,400 |
| **txn-1000 control** (single-row INSERTs, 1000/txn — the pre-T1 batching) | 7,406 | 7,301 | — | — | 5,703 |

Three readings:

- **Against the old best practice, T1 is 5.8× at equal durability**: one
  1000-row statement at `group` moves 42,339 rows/s where 1000 single-row
  statements inside one transaction move 7,301 — same rows, same fsync
  count, same connection. Against an untuned autocommit loop it is 45.7×.
  The spec's ~9K rows/s ceiling (8,845 measured 2026-08-07) is gone.
- **`group` = `strict` at batch 1 on one connection** (927 vs 930, inside
  the floor) — the documented "a batch of one is a batch", reproduced. The
  strict/group difference at batch 1000 (48,945 vs 42,339) is a single-run
  between-window comparison over ramp-dominated phases (see below) and is
  not read as a finding; there is no mechanism by which strict could beat
  group on one connection.
- **ckdbs wins batch 1 and loses batch 1000, at both durability classes.**
  Batch 1: +30 % over PostgreSQL relaxed, +6 % at group. Batch 1000:
  −46 % relaxed, −48 % group. The crossover is not statement cost — it is
  the tail walk, quantified next.

## Per-statement latency

Latencies are per **statement** (B rows each); the per-row mean is
mean ÷ B. ckdbs first, `relaxed` (run A):

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row (mean) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 132.9 | 56.8 | 117.3 | 129.7 | 158.0 | 200.6 | 27,731.8 | 132.9 |
| bulk-10 | 10,000 | 313.7 | 87.1 | 223.0 | 309.0 | 465.6 | 621.2 | 14,347.3 | 31.4 |
| bulk-100 | 1,000 | 2,057.8 | 429.0 | 1,332.9 | 1,953.2 | 3,339.6 | 4,472.3 | 15,665.2 | 20.6 |
| bulk-1000 | 100 | 19,486.2 | 3,728.1 | 10,994.5 | 19,540.3 | 34,152.0 | 35,184.9 | 35,701.9 | 19.5 |
| txn-1000 (per INSERT) | 100,000 | 132.4 | 56.6 | 116.3 | 130.7 | 160.7 | 220.7 | 20,015.6 | 132.4 |

`group`:

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row (mean) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 1,071.7 | 380.7 | 1,033.1 | 1,059.2 | 1,152.5 | 1,295.7 | 53,405.6 | 1,071.7 |
| bulk-10 | 10,000 | 1,285.5 | 422.7 | 1,168.6 | 1,254.6 | 1,531.1 | 1,985.5 | 44,135.8 | 128.6 |
| bulk-100 | 1,000 | 2,485.4 | 811.9 | 1,670.1 | 2,437.6 | 4,020.9 | 4,721.3 | 7,552.8 | 24.9 |
| bulk-1000 | 100 | 23,600.2 | 8,141.6 | 14,179.5 | 20,907.8 | 42,155.2 | 47,084.1 | 47,420.5 | 23.6 |
| txn-1000 (per INSERT) | 100,000 | 133.4 | 56.5 | 114.1 | 128.3 | 162.5 | 248.2 | 40,244.6 | 133.4 |

`strict`:

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row (mean) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 1,068.1 | 376.8 | 1,024.2 | 1,052.1 | 1,149.3 | 1,318.3 | 54,333.9 | 1,068.1 |
| bulk-1000 | 100 | 20,415.6 | 4,645.6 | 12,266.0 | 19,120.6 | 36,543.0 | 39,210.4 | 53,059.4 | 20.4 |

PostgreSQL, `synchronous_commit = off` (relaxed twin):

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | µs/row (mean) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 178.7 | 131.0 | 173.5 | 174.3 | 197.2 | 237.8 | 178.7 |
| bulk-10 | 10,000 | 271.3 | 223.5 | 255.5 | 257.2 | 313.8 | 384.1 | 27.1 |
| bulk-100 | 1,000 | 1,134.1 | 1,032.4 | 1,081.5 | 1,095.4 | 1,513.5 | 1,755.5 | 11.3 |
| bulk-1000 | 100 | 10,568.2 | 9,904.2 | 10,050.8 | 10,185.9 | 12,349.1 | 13,542.0 | 10.6 |

PostgreSQL, defaults (`synchronous_commit = on`):

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | µs/row (mean) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 1,135.6 | 674.2 | 1,100.4 | 1,123.6 | 1,218.5 | 1,366.1 | 1,135.6 |
| bulk-10 | 10,000 | 1,234.3 | 928.8 | 1,187.9 | 1,216.8 | 1,345.4 | 1,476.6 | 123.4 |
| bulk-100 | 1,000 | 2,187.8 | 1,383.4 | 2,092.0 | 2,143.1 | 2,660.8 | 2,980.2 | 21.9 |
| bulk-1000 | 100 | 12,279.6 | 11,528.1 | 11,770.8 | 11,929.1 | 14,287.5 | 14,949.6 | 12.3 |
| txn-1000 (per INSERT) | 100,000 | 171.5 | 121.9 | 166.9 | 167.7 | 189.2 | 232.8 | 171.5 |

The shape to notice before any ratio: **PostgreSQL's bulk-1000 distribution
is tight (p99/p0 = 1.37) and ckdbs's is wide (p99/p0 = 9.4)** for 100
byte-identical statements. That spread is not noise and not the device — it
is a ramp, and it is the finding of this document.

## The ramp: every heap insert walks the chain to the tail

A `--trace` run kept the per-statement series for bulk-1000 relaxed
(fresh server, clean window, phase fully CPU-bound: 1.98 s of server CPU
over 1.99 s of phase wall — this is work, not waiting):

| statements | rows resident (mid) | statement mean µs | µs/row |
|---:|---:|---:|---:|
| 0–9 | ~5,000 | 5,513.8 | 5.5 |
| 20–29 | ~25,000 | 11,447.5 | 11.4 |
| 40–49 | ~45,000 | 17,869.7 | 17.9 |
| 60–69 | ~65,000 | 24,499.9 | 24.5 |
| 80–89 | ~85,000 | 30,028.6 | 30.0 |
| 90–99 | ~95,000 | 35,193.8 | 35.2 |

A linear fit over all 100 statements (latency vs rows resident at
statement start):

```
statement_µs = 3,949 + 0.321 ns × resident_rows × 1,000 rows      R² = 0.967
```

i.e. **inserting one row costs a flat ~3.95 µs plus ~0.32 ns for every row
already in the relation.** The mechanism is in the code:
`heap::ChainInsert` (`src/storage/heap/heap_chain.cpp`) calls
`ChainTail(store, head)` on **every insert**, which follows `next_page_id`
frame by frame from the chain root to the tail. At ~176 rows per page that
slope is ~57 ns per page hop — a page-store `Get` plus a header read — paid
once per row inserted, O(pages-resident) each time. The batch-1 series shows
the same growth at ~0.5–1.0 ns per resident row (noisier: per-statement
walks run colder), which also resolves what looked like cross-session drift:
this run's txn-1000 control (7,301–7,406 rows/s over 100K rows) against the
2026-08-07 baseline's 8,845 (over 2,000 rows) is the walk lengthening, not
the engine changing — the A/B below pins that directly.

Consequences, stated with the fit's numbers:

- The walk overtakes the whole rest of the per-row cost at
  **~12,300 resident rows** (3.95 µs ÷ 0.321 ns).
- A bulk load is **quadratic in total rows**: loading N rows costs
  ~0.16 ns × N² of tail-walking on top of the linear work. At this run's
  100K rows that surcharge is ~1.6 s of the phase's ~2.0 s; at 1M rows it
  would be ~160 s.
- This is heap-specific (`--bars-clustered btree` relations descend
  O(log N)) and pre-existing — T1 did not add it, T1 made it visible by
  removing the 113 µs/row that used to hide it.
- The batch-size sweep 1/10/100/1000 is this document's statement-size
  ladder (`--batches`), and the resident-rows axis 0→100K
  (`--rows 100000`) is its relation-size ladder; the decade table above is
  the three-sizes evidence that the growth is per resident row, not per
  statement. At 200 / 1K / 10K resident rows the fit prices the walk at
  0.06 / 0.3 / 3.2 µs per row against the flat 3.95 — invisible at the
  sizes most existing benches load, dominant at bulk-load sizes.

## Wait accounting

The clean decomposition anchors at batch 1000, where the client's share per
row is 0.09 µs and each component was measured independently:

| wait type | how measured | per row at batch 1000 |
|---|---|---:|
| client + socket round trip | PING p50 (90.7–91.6 µs) ÷ 1000 | **0.09 µs** |
| parse + dispatch | parse probe: the same statement against a nonexistent relation — full lex/parse, refused at catalog resolution before the pipeline; p50 1,156.8 µs, minus the 1-row probe's 112.7, ÷ 999 | **~1.05 µs** |
| write pipeline, flat part (FK/admission hooks, id, encode, place, WAL append) | fit intercept 3,949 µs ÷ 1000, minus parse and round trip | **~2.8 µs** |
| chain tail walk | fit slope | **0.321 ns × resident rows** (0 → 32 µs across this run) |
| durability / commit | group mean − relaxed mean at batch 1000: 4,114 µs/stmt | **~4.1 µs** amortized |
| lock / conflict wait | error counts, single connection | 0, structurally |

At batch 1, the same components measured directly: statement mean 132.9 µs
relaxed = 91.6 round trip + ~22 parse+dispatch (1-row probe p50 112.7 −
PING 90.7) + pipeline-plus-walk remainder; the client floor's bimodality
(PING p0 42 µs vs p50 91.6) caps this decomposition at ±25 µs, so the
batch-1 split is indicative while the batch-1000 split is the measured one.
The durability wait at batch 1 is clean: group − relaxed = **938.8 µs**,
against the device's own 8 KiB write+fsync p50 of **940.2 µs** measured
minutes later — the engine's commit wait is the device, with nothing left
over. The group−relaxed differences at batch 10/100 (972 / 428 µs per
statement) are single-run between-window differences; the 428 sits below
one device fsync and is recorded unexplained rather than interpreted.

Reproduced from the 2026-08-07 document: group-durability batch-1 p0 is
380.7 µs (old: 379.7) — below the device's fsync p0 of 875, so some commits
still pay less than one full device sync, still with no second committer to
share with, and still unexplained by anything the engine reports.

Not measurable today: WAL append vs WAL fsync inside the pipeline (no
per-record instrumentation), read-side I/O (nothing on the read path here),
server-side queueing (single connection, none).

## No regression at batch 1: the A/B

The single-row path is claimed to be the same code (`InsertOneRow` is
`InsertInner`'s body factored, BLK03). Verified, not assumed: the parent
commit `cf4adfb` was built Release from an exported tree and interleaved
against HEAD — alternating runs, fresh server and file each, same clean
window, 50,000 rows each, relaxed:

| run | parent rows/s | HEAD rows/s | parent CPU ticks | HEAD CPU ticks |
|---|---:|---:|---:|---:|
| bulk-1 #1 | 8,022 | 8,697 | 338 | 300 |
| bulk-1 #2 | 7,997 | 8,047 | 338 | 339 |
| bulk-1 #3 | 8,075 | 8,066 | 333 | 340 |
| bulk-1 #4 | 7,941 | 8,045 | 341 | 338 |
| txn-1000 #1 | 8,246 | 8,195 | 335 | 338 |
| txn-1000 #2 | 8,279 | 8,514 | 332 | 321 |

Mean bulk-1: parent 8,009, HEAD 8,214 (8,053 excluding the one high
outlier) — **+0.5 %, inside the floor, with identical server CPU.** The
multi-row loop cost the single-row statement nothing measurable. (These
50K-row runs are faster per row than the 100K-row matrix cells above —
7,174 — which is again the tail walk scaling with the rows resident, not an
inconsistency.)

## Against PostgreSQL, read with the fit in hand

- **Fixed statement cost: ckdbs is still the cheaper engine.** Batch-1
  relaxed 132.9 vs 178.7 µs; batch-1 durable 1,071.7 vs 1,135.6; the pre-T1
  txn control 133.4 vs 171.5 per INSERT. The 2026-08-07 finding — ckdbs
  starts a statement cheaper — survives T1 unchanged, and ckdbs's flat
  bulk per-row cost (~3.95 µs) is **2.7× cheaper than PostgreSQL's
  ~10.6**.
- **PostgreSQL has no ramp.** Its heap insert resolves the target page
  through the free-space map and a cached target block — O(1) in relation
  size — so its bulk-1000 statements are flat (p99/p0 1.37) where ckdbs's
  grow 6.4× within one phase. The fit says a ckdbs 1000-row statement is
  faster than PostgreSQL's until ~19,500 resident rows
  (3,949 + 0.321 × N ≥ 10,186 µs) — and slower ever after, which is
  exactly what the throughput matrix shows at 100K rows (51K vs 95K
  rows/s relaxed).
- **Parse cost is a wash**: 1000-row probe p50 1,157 (ckdbs) vs 1,115 µs
  (PostgreSQL) — two parsers lexing 24 KB of the same text for the same
  ~1.1 µs/row.
- The comparison is defaults-vs-defaults on the same volume; the relaxed
  twin is a session GUC, not cluster tuning.

## Found by this run: the WAL stream wedges at an exactly-filled segment

The first (discarded) matrix ran all batch sizes against one server and
died mid-phase with
`ERR wal: range at offset 66994016 + 1048512 runs past the segment end (67108864)`
— and every logged statement after it failed the same way. Mechanism, from
`src/wal/stream.cpp`: `WalStream::Append` rolls segments when
`SegmentRemaining() < total`, but `SegmentRemaining()` is
`segment_size_ - OffsetOf(append_lsn_)`, which reads **a full segment,
not zero, when the previous append exactly filled the segment**
(`OffsetOf(append_lsn_) == 0`). The seal/roll is skipped, appends run past
the segment end while the ring still holds bytes belonging to the old
segment, and the next `Flush()` presents the device a range spanning the
boundary — refused by `CheckSegmentRange`, and refused again forever, since
a failed flush deliberately keeps the bytes staged. The stream is wedged;
every subsequent logged statement errors. Records are 8-byte aligned, so a
workload whose stream crosses a segment boundary has roughly a
record-alignment-in-record-size chance of landing exactly on it — this run
hit it on its first crossing, at ~64 MiB of WAL (≈300K rows). Nothing in
`bench/` had ever pushed one server past 64 MiB of WAL before, which is why
it survived until a bulk benchmark. Not fixed in this session (a
measurement session does not edit engine code); the harness works around it
by keeping each server's lifetime WAL under one segment, which is why every
configuration got a fresh server. The one-line shape of a fix is making an
exactly-full segment read as zero remaining; it belongs to the WAL owner
with a regression test at the boundary.

## What this run teaches about the engine

1. **The spec's 21 µs is dead, and its decomposition was measured, not
   inferred.** Of the old 113 µs/row (91.8 round trip + 21.3 statement), T1
   removes the round trip entirely (0.09 µs/row at batch 1000) and cuts
   parse+dispatch from ~22 µs/statement-per-row to ~1.05 µs/row, leaving
   ~2.8 µs/row of pipeline — BI2's authority, correctly untouched — for a
   96 % per-row cost reduction at small relations. `max_insert_rows` = 1024
   is comfortably past the knee: batch 100 already captures ~94 % of
   batch 1000's throughput at relaxed.
2. **The next bottleneck has a name and a line number.** The
   O(pages-resident) tail walk in `heap::ChainInsert` is now the majority
   cost of any heap bulk load beyond ~12K resident rows and makes total
   load time quadratic. It is not T3's tree-build problem — it is a missing
   cached tail pointer (the analogue of the `varheap_page_id` root that
   `TableAccess` already carries, though a *mutable* tail needs a different
   invalidation story than a DDL-immutable root). Fixing it is worth more
   to bulk ingestion than T2: at 100K resident rows the walk is ~32 µs/row
   against T2's remaining ~1.05 µs/row of parse.
3. **Durability behaves exactly as documented.** BI8's relaxed
   recommendation for bulk load is right on the numbers: relaxed batch-1000
   is 45.7× a durable autocommit loop, and even at `group` a 1000-row
   statement pays exactly one device fsync — 938.8 µs measured at batch 1
   against the device's own 940.2, amortized to 4.1 µs/row at batch 1000.
   `group` = `strict` on one connection, reproduced to 0.3 %.
4. **The measurement culture earned its keep twice.** A sibling process's
   load burst produced a 5.8× wrong number that only the load gate and the
   repeat caught; and the first matrix's shared-server layout produced the
   WAL wedge that fresh-per-configuration servers then sidestepped. Neither
   failure was visible in the driver's own output.

## Reproducing

Driver flags and exact invocations: [`bench/docs/README.md`](docs/README.md).
The pattern behind every cell in this document:

```bash
# one fresh server per (durability × batch) configuration, on the block device
printf 'durability = relaxed\n' > ~/bench-bulk/relaxed.conf
./build-release/kds_server ~/bench-bulk/<tag>.db --port <p> --config ~/bench-bulk/relaxed.conf
python3 tools/bulk_insert_benchmark.py --port <p> --rows 100000 --batches <B> \
    --durability relaxed --suffix <tag> --json ~/bench-bulk/<tag>.json
# controls: --txn-control (pre-T1 batching), --parse-probe (parse cost), --trace (the ramp)

./tools/pg_setup.sh start
python3 tools/pg_bulk_insert_benchmark.py --port 15433 --database bench \
    --rows 100000 --batches 1,10,100,1000 --synchronous-commit off --json pg-off.json
```

The relaxed configuration was run twice whole for the noise floor; the A/B
built the parent commit from `git archive cf4adfb` and alternated
parent/HEAD runs in one window; the device figure is a 300-sample 8 KiB
write+fsync probe on the same volume at run end.

## Addendum: the ChainTail fix, measured (2026-08-10, same machine)

The finding above was acted on the same day: `heap::ChainInsert` now takes
a tail hint - read as the tail-search start, written back with the landing
page - carried on the cached `TableAccess`. A hint can be behind, never
wrong (next_page_id is write-once and a page never leaves its chain), so a
stale one heals by walking forward and a damaged one falls back to the
head. `tests/heap_chain_test.cpp` pins all three properties.

Spot check with this driver, Release build at the fix commit, batch 1000
at relaxed, 100K rows, fresh file, single connection:

| | pre-fix (9ee04e4) | post-fix |
|---|---:|---:|
| rows/s | 51,258 | **251,000** |
| per-statement p0 / p50 / p99 (us) | ramped 6.4x intra-run | 3,255 / 3,359 / 5,862 |
| per-row cost | 3.95 us + 0.321 ns x rows-resident | **~3.97 us flat** |

The resident-rows term is gone: latency is flat across the run (p50/p0 =
1.03 against the pre-fix ramp), throughput at batch 1000 is 4.9x, and the
remaining per-row cost is exactly the trace fit's flat part - parse,
pipeline, framing. The bulk-ingestion story now ends where T3's territory
begins (per-tuple placement itself), and PostgreSQL's batch-1000 edge
(94.6K rows/s) is overtaken at 2.7x. The WAL segment-boundary wedge
reported above is untouched and still owed.
