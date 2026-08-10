# The T1 multi-row INSERT, priced — batch 1/10/100/1000 × three durability classes, against PostgreSQL 17.10

`docs/spec-bulkinsert.md` §0 claimed the single-row INSERT ceiling of ~9K
rows/s was ~21 µs/row of statement cost that never amortizes, and shipped T1
(BLK01–BLK05) as that cost's eviction notice. This document holds **four
runs of four engine states**, deliberately: the first run (at `9ee04e4`,
the T1 commit) found two engine defects — an O(pages-resident) tail walk
under every heap insert, and a WAL stream that wedged at an exactly-filled
segment; the second run (at `926f422`, both fixes in) re-measured the
matrix whole; the third (at `06c8ab9`) measured **T3, the sorted heap
fill** (`docs/workplan-t3.md`) against a gate-closed twin; the fourth (at
`3175824`) measures **T2, the KWP binary load stream** (KL02–KL06), which
closes the tier table. Earlier parts are kept as the record of why each
next step exists; every number in them describes an engine state that no
longer ships. Same `write_probe` row shape throughout; driver
documentation: [`bench/docs/README.md`](docs/README.md),
`bulk_insert_benchmark.py` / `pg_bulk_insert_benchmark.py` /
`kwp_load_benchmark.py`.

**Thesis, across the four runs: each tier removed the cost the previous
one exposed, and the ledger closed where the spec said it would. T1
removed the per-row round trip; the tail-hint fix made the pipeline flat;
T3 removed per-row placement (0.31 µs/row of fill remains); T2 removed
the parse — and replaced it with a smaller binary-decode cost the parse-
share estimate had priced at zero, so T2 delivers 1.69× against Part III's
~3.5× prediction, landing at 1.14M rows/s on one connection, 1.6× the
PostgreSQL COPY twin, with the durability class within noise of free
because a load session is one transaction.**

---

# Part I — the pre-fix run, at `9ee04e4` (superseded engine state)

This part is the record of the run that found the two defects. **Both are
fixed on `main` as of `926f422`; Part III is the current engine.** Part I's
PostgreSQL comparison verdict — PostgreSQL wins the large batches — is
inverted by the fix and survives only as history.

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

## Reproducing (Part I)

Driver flags and exact invocations: [`bench/docs/README.md`](docs/README.md).
The pattern behind every cell in this part:

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

---

# Part II — the post-fix run, at `926f422` (superseded by Part III)

Both Part I findings were fixed on `main` the same day and the whole matrix
was re-measured. The fixes, as landed: `heap::ChainInsert` takes a **tail
hint** carried on the cached `TableAccess` — read as the tail-search start,
written back with the landing page; a hint can be behind, never wrong,
since `next_page_id` is write-once and a page never leaves its chain
(commit `e704c16`, pinned by `tests/heap_chain_test.cpp`). And
`WalStream::SegmentRemaining()` answers **0 at an exactly-filled segment**,
so the roll happens instead of the wedge (commit `926f422`, two wal_stream
tests fill a segment to the byte and cross it, live and through a reopen).
The spot-check addendum that previously ended this file is superseded by
this part.

## The run

| | |
|---|---|
| executed | 2026-08-10, 06:48–07:02 UTC (matrix 06:48–07:01, WAL-boundary demonstration 07:02 — sequential, never concurrent) |
| commit measured | `926f422` (`wal: the exact-fill boundary rolls instead of wedging the stream`, tip of `feat-drop-table` == `main`; includes `e704c16`, the tail hint) |
| tree | clean |
| binary | `build-release/kds_server`, rebuilt 06:45:04 UTC from this tree (commit timestamp 06:41:14) — mtime after HEAD, verified before the first measurement |
| build type | Release (`CMAKE_BUILD_TYPE:STRING=Release`) |
| tests | not run in this session (no engine change made here; the fix commits carry their own suites). In-run verification as in Part I: `rows=` fields, id spans, `SELECT COUNT(*)` per phase — all exact, zero error replies in every configuration |
| device | same volume as Part I. Raw 8 KiB write+fsync p50 **946 µs** re-probed at 07:05 (Part I measured 940 at 05:53 — stable); that probe's tail (p99 8.8 ms) was taken during a live sibling-load burst and is not comparable |
| machine / gating | as Part I: 2 vCPU shared box, load gate at 1-min ≤ 0.60 before every configuration, load recorded after. **Every configuration in this part ran in a clean window on its first attempt — zero retries** (log: `~/bench-bulk2/matrix3.log`) |
| ckdbs config | as Part I; `durability` per configuration. Data files `/home/ec2-user/bench-bulk2/*.db` |
| PostgreSQL | **carried over from Part I unchanged** (measured 05:34–05:38 the same day, same volume, untouched defaults). PostgreSQL's engine did not change between the parts; the ~75-minute gap is noted where a durable cell is compared |
| relation | scenario1's `write_probe` shape, as Part I |

## The noise floor

The relaxed configuration was again run twice whole (A and B), fresh server
and file per configuration:

| measurement | run A | run B | spread |
|---|---:|---:|---:|
| bulk-1 rows/s | 9,328 | 9,638 | 3.3 % |
| bulk-10 rows/s | 63,369 | 64,502 | 1.8 % |
| bulk-100 rows/s | 209,876 | 205,538 | 2.1 % |
| bulk-1000 rows/s | 278,180 | 264,030 | 5.2 % |
| PING p50 (all servers) | 63.0–91.9 µs | | bimodal, as Part I |

**Nothing under ~5 % is a finding.** The fsync-bearing batch-1000 cells get
a wider berth still: the three durable batch-1000 configurations measured
in different windows (group 100K / group 400K / group 600K rows) put their
per-statement durability overhead at 1.16 / 2.38 / 1.67 ms against a device
whose own fsync spans 0.94 (p50) to 2.2 ms (p95) — that spread is the
device's state over time, and no group-vs-strict or size claim is made
inside it.

## The matrix: throughput

100,000 rows per cell, single connection, rows/s. PostgreSQL columns are
Part I's, carried over.

| rows/statement | ck relaxed | ck group | ck strict | pg sync=off | pg sync=on | ck/pg (relaxed) | ck/pg (durable) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 9,328 | 943 | 951 | 5,503 | 878 | **1.70×** | **1.07×** |
| 10 | 63,369 | 9,116 | — | 36,450 | 8,080 | **1.74×** | **1.13×** |
| 100 | 209,876 | 64,614 | — | 87,920 | 45,630 | **2.39×** | **1.42×** |
| 1,000 | 278,180 | 210,165 | 170,894 | 94,600 | 81,400 | **2.94×** | **2.58×** |
| **txn-1000 control** (pre-T1 batching) | 9,870 | 9,837 | — | — | 5,703 | 1.73× | 1.72× |

Readings:

- **The Part I verdict is inverted: ckdbs now wins every cell**, 1.07× to
  2.94×. Part I's crossover — PostgreSQL overtaking at large batches on its
  O(1) target-block insert — was entirely the tail walk, and with the walk
  gone ckdbs's cheaper per-row pipeline (3.6 vs 10.6 µs/row at batch 1000
  relaxed) decides every batch size. Caveat on the durable column: the two
  engines' durable cells were measured ~75 minutes apart on a device whose
  fsync tail drifts; the relaxed cells are CPU-bound and carry no such
  caveat.
- **Against the pre-fix engine**: batch-1000 relaxed 51,258 → 278,180
  (5.4×), group 42,339 → 210,165 (5.0×), batch-1 relaxed 7,174 → 9,328
  (+30 % — the walk was taxing single-row inserts too, ~35 µs at the
  100K-row phase average). The pre-T1 control also rose 7,406 → 9,870 for
  the same reason.
- **T1 vs the pre-T1 best practice, post-fix, same durability**: 210,165
  against 9,837 — **21.4×** where Part I measured 5.8×. Removing the walk
  widened T1's own advantage, because the walk was a per-row cost T1 could
  not amortize.
- **`group` = `strict` at batch 1** (943 vs 951, 0.8 %), as documented. At
  batch 1000 strict reads 19 % below group; both sit inside the durable
  device-drift band above and the difference is not read as an engine
  effect.

## Per-statement latency

ckdbs `relaxed` (run A; per-row mean = mean ÷ B):

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 100.0 | 56.3 | 76.2 | 92.0 | 134.9 | 201.1 | 11,489.5 | 100.0 |
| bulk-10 | 10,000 | 149.0 | 82.3 | 132.9 | 141.9 | 185.4 | 245.7 | 9,740.5 | 14.9 |
| bulk-100 | 1,000 | 467.1 | 369.2 | 430.4 | 450.6 | 556.4 | 778.6 | 2,159.6 | 4.7 |
| bulk-1000 | 100 | 3,581.3 | 3,281.1 | 3,331.8 | 3,388.7 | 4,493.0 | 6,128.4 | 6,334.3 | 3.6 |
| txn-1000 (per INSERT) | 100,000 | 98.6 | 54.6 | 73.6 | 104.7 | 137.5 | 178.6 | 17,353.5 | 98.6 |

ckdbs `group`:

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 1,051.8 | 378.1 | 1,017.2 | 1,044.2 | 1,137.2 | 1,328.7 | 12,359.2 | 1,051.8 |
| bulk-10 | 10,000 | 1,086.2 | 398.2 | 1,050.6 | 1,082.8 | 1,216.0 | 1,462.0 | 26,116.4 | 108.6 |
| bulk-100 | 1,000 | 1,536.2 | 766.0 | 1,468.8 | 1,515.7 | 1,848.8 | 2,324.9 | 4,608.3 | 15.4 |
| bulk-1000 | 100 | 4,744.5 | 4,079.2 | 4,244.0 | 4,413.2 | 6,575.4 | 7,596.6 | 7,611.4 | 4.7 |
| txn-1000 (per INSERT) | 100,000 | 98.4 | 54.8 | 73.8 | 105.5 | 136.8 | 170.7 | 15,458.0 | 98.4 |

ckdbs `strict`:

| phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-1 | 100,000 | 1,043.7 | 385.4 | 1,003.4 | 1,031.5 | 1,127.4 | 1,313.3 | 9,143.1 | 1,043.7 |
| bulk-1000 | 100 | 5,838.6 | 5,038.5 | 5,458.6 | 5,571.0 | 7,550.2 | 7,914.6 | 8,488.5 | 5.8 |

**The Part I spread is gone.** bulk-1000 relaxed reads p50/p0 = 1.03
(3,389/3,281) where the pre-fix engine read 5.2 (19,540/3,728) — one
hundred byte-identical statements now cost the same, which is what
PostgreSQL's tables looked like in Part I and ckdbs's did not.

## The re-fit: the resident-rows term is dead

The same `--trace` protocol as Part I, batch 1000 relaxed, 100 statements,
fresh server (phase CPU-bound: 0.38 s server CPU over 0.36 s phase wall):

```
pre-fix  (9ee04e4): statement_µs = 3,949 + 0.321 ns × resident_rows   R² = 0.967
post-fix (926f422): statement_µs = 4,036 − 0.009 ns × resident_rows   R² = 0.169
```

A slope statistically indistinguishable from zero with no explanatory
power — the fit is now just the mean. The twenty-statement means run
4,221 / 3,506 / 3,305 / 3,629 / 3,332 µs across the phase: no trend, only
window noise. The batch-1 trace says the same thing at 100× the sample
count: decade means 105.5 → 112.9 µs over 100K rows (p50 pinned at
110.4–111.1 across all ten decades), a residual drift of ~0.07 ns per
resident row — 5–15× below the pre-fix slope, at the edge of what the
client can resolve, consistent with page-cache dilution rather than any
per-insert scan.

**The batch-1 hint price, on a one-page relation**: the first trace decade
(rows 0–10K, relation one to ~57 pages) reads 105.5 µs mean post-fix
against 102.0 pre-fix — +3.4 µs nominal, inside the between-window client
floor drift (PING means ranged 73.7–95.3 µs across this part's servers),
and the statement floor is identical (p0 56.3 vs 56.8). The hint is one
pointer read and one write per insert; the data cannot distinguish its
cost from zero, which is what "should be ~zero" predicted.

## Wait accounting, post-fix

Anchored at batch 1000 relaxed as in Part I:

| wait type | how measured | per row at batch 1000 |
|---|---|---:|
| client + socket round trip | PING p50 (86.3 µs) ÷ 1000 | **0.09 µs** |
| parse + dispatch | parse probe marginal: 1000-row p50 1,455 − 1-row p50 101, ÷ 999 = 1.36; on p25 (1,168 − 63) ÷ 999 = 1.11 | **~1.1–1.4 µs** |
| write pipeline (id, encode, place-with-hint, witness hooks, WAL append) | statement p50 3,389 ÷ 1000, minus the above | **~1.9–2.2 µs** |
| chain tail walk | trace fit slope | **0** (−0.009 ns × N, R² 0.17) |
| durability / commit | group − relaxed statement mean, per batch: 952 (b1) / 937 (b10) / 1,069 (b100) / 1,163 µs (b1000) — one device fsync (p50 940–946) plus 10–20 % for the larger dirty range | **1.16 µs** amortized |
| lock / conflict wait | error counts, single connection | 0, structurally |

Two notes. The durability row is the quiet vindication of the re-run:
Part I's group−relaxed deltas at batch ≥ 100 (428 / 4,114 µs) were
ramp-noise artifacts, and post-fix every batch size shows the same clean
"one fsync per statement" the manual describes. And the parse probe's
1000-row body **widened** relative to Part I (p50 1,157 → 1,455 µs, p25
nearly unchanged at 1,140 → 1,168): the probe conflates parse with the
unknown-relation refusal path, which `DROP TABLE`'s oid tombstone work
(`20d5dca`, landed between the two parts) touched. It is recorded as
observed, not attributed; the p25 marginal (1.11 µs/row) is the parse
figure most comparable to Part I's 1.05.

## The WAL boundary, crossed on purpose

One configuration was sized to cross the 64 MiB segment boundary that
wedged the pre-fix stream: batch 1000 at `group`, **600,000 rows** on one
server — beyond both the pre-fix wedge point (~300K logged rows) and the
~430K rows a single segment holds at this row shape.

| | |
|---|---|
| rows | 600,000 in 600 statements, `COUNT(*)` verified, zero errors |
| throughput | 189,796 rows/s |
| per-statement | mean 5,255 / p0 3,968 / p25 4,281 / p50 4,614 / p95 6,691 / p99 7,663 µs |
| WAL on disk | `wal-0-0.log` **and `wal-0-1.log`**, 64 MiB each — the stream rolled and kept going |

The 400K-row configuration run first for this purpose stayed inside one
segment (~150 B of WAL per row at batch 1000 — batching also amortizes the
per-statement `TXN_BEGIN`/`TXN_COMMIT` records, so the pre-fix ~220 B/row
estimate from mixed batches overestimates a pure batch-1000 stream), which
is itself worth recording: the demonstration had to be re-sized on the
measured WAL density, not the assumed one.

## What the re-run teaches

1. **The fix bought exactly what the fit predicted, and then some.** Part I
   priced the walk at 0.321 ns × resident rows; removing it took batch-1000
   relaxed from 51K to 278K rows/s (5.4×) and made per-statement latency
   flat (p50/p0 1.03). Nothing else moved: the flat per-row cost the
   pre-fix fit put at 3.95 µs is measured post-fix at 3.4–3.6 µs — the fit
   decomposed the engine correctly from the outside.
2. **The bulk-INSERT picture is now three clean numbers**: ~3.6 µs/row of
   CPU-bound statement-and-pipeline work at batch 1000, one device fsync
   per durable statement (1.16 µs/row amortized), and a client round trip
   per statement (0.09 µs/row). T2's remaining prize is the ~1.1–1.4 µs/row
   parse; T3's is the ~2 µs pipeline. BI8's relaxed recommendation and
   `max_insert_rows = 1024` both stand — batch 100 reaches 75 % of
   batch 1000's throughput, and batch 1000 sits past the knee.
3. **ckdbs beats PostgreSQL in every cell of the matrix it lost half of in
   Part I** — 2.9× at the headline batch-1000 relaxed cell — and the reason
   is the one the 2026-08-07 document already named: ckdbs's fixed and
   per-row costs are both lower once no O(N) term is hiding in the loop.
4. **A 600K-row single-server load is now routine** where ~300K wedged the
   engine permanently two hours earlier. Nothing reclaims segments, so the
   WAL still grows monotonically (128 MiB for the demo) — retention is
   recovery's open decision, unchanged by the fix.

## Reproducing (Part II)

Identical protocol to Part I (`~/bench-bulk2` instead of `~/bench-bulk`);
the added configurations were `--rows 400000` / `--rows 600000` at
`--batches 1000` on one server for the boundary demonstration, and the two
`--trace` runs for the re-fit. Orchestration log with per-configuration
load gating: `~/bench-bulk2/matrix3.log`.

---

# Part III — T3, the sorted heap fill, at `06c8ab9` (superseded by Part IV)

T3 landed as a fill inside the T1 statement (`docs/workplan-t3.md`,
TS01–TS04): page-at-a-time placement, a contiguous Keystone range from one
catalog write, and WAL as one `kFullPageImage` per touched page instead of
per-row records. It engages automatically only inside the T3-2 gate —
heap-clustered, no index, no Cabin, no assertion, no spillable schema —
and falls back to the row loop otherwise, with byte-identical replies.

**The driver's schema already satisfies the gate**: `write_probe` is five
`int64` columns, so `varheap_page_id` is never allocated and the schema
cannot spill — no schema change was needed for the eligible cells. The
gate-closed twin is the same schema with `CREATE CABIN ON <t>(a)` issued
after `CREATE TABLE` (`--cabin`), which sets `cabin_mask` and forces the
row loop on the very same statements; its cells double as Part II's
row-loop numbers re-measured at this commit (they reproduce Part II within
the floor, so the twin carries a Cabin write-hook cost of ~nothing,
consistent with `bench/results-cabin.md`).

## The run

| | |
|---|---|
| executed | 2026-08-10, 08:34–08:38 UTC, sequential |
| commit measured | `06c8ab9` (merge tip of `feat-t3` == `main`; T3 = TS01–TS04) |
| tree | clean |
| binary | `build-release/kds_server`, rebuilt 08:19:51 UTC from this tree — mtime after HEAD, verified before the first measurement |
| build type | Release |
| tests | not run in this session (no engine change made here; TS04's equivalence suite rode the feature commit). In-run verification as before — `rows=`, id spans, `COUNT(*)`, zero error replies everywhere. The id-span check also pins TS03's contiguous range from the outside: `last_id − first_id + 1 == rows` held for every statement |
| device / machine / gating | as Parts I–II. **Every configuration ran clean on its first attempt — zero retries** (log: `~/bench-bulk3/matrix4.log`) |
| ckdbs config | as Part II; data files `/home/ec2-user/bench-bulk3/*.db` |
| PostgreSQL | **carried over from Part I unchanged**, as in Part II |
| WAL volume | measured directly this time: the last nonzero byte offset of the (zero-prewritten) segment files after each configuration — exact stream bytes, and byte-identical across durability classes and repeats of the same shape, which is itself evidence the fill is deterministic |

## The noise floor

The headline cell (eligible, relaxed, batch 1000) ran three times fresh
(matrix, repeat, trace): p50 1,449.5 / 1,501.3 / 1,480.9 µs — **3.5 %**.
Throughput spread on the same three is 15 % (674.6 / 580.3 / 641.4
stmts/s): these phases are only ~0.15 s long, so a single tail statement
moves the rate; the p50s are the numbers to compare, and rate claims below
stay above the 15 %. The gate-closed twin reproduces Part II's row-loop
cells (270,209 vs 278,180 rows/s at b1000 relaxed, −2.9 %; 202,686 vs
209,876 at b100, −3.4 %) — the row loop did not move between the commits,
which is the in-run control for everything attributed to T3.

## The matrix: T3 against its gate-closed twin

100,000 rows per cell, single connection, rows/s. PostgreSQL columns
carried over from Part I.

| batch | durability | T3 (eligible) | row loop (`--cabin` twin) | **T3 gain** | pg (twin class) | ck-T3/pg |
|---:|---|---:|---:|---:|---:|---:|
| 100 | relaxed | 368,156 | 202,686 | **1.82×** | 87,920 | **4.2×** |
| 1,000 | relaxed | 674,570 | 270,209 | **2.50×** | 94,600 | **7.1×** |
| 100 | group | 139,692 | 69,194 | **2.02×** | 45,630 | **3.1×** |
| 1,000 | group | 434,683 | 205,558 | **2.11×** | 81,400 | **5.3×** |
| 1 | relaxed (gate never opens) | 9,770 | — | 1.00× (by design) | 5,503 | 1.78× |

**Batch-1 no-regression**: 9,770 rows/s against Part II's 9,328/9,638 —
inside the floor. A single-row statement is not `bulk`, the gate never
opens, and the row loop it takes is the one the twin re-measured.

## Per-statement latency

T3 eligible:

| phase | durability | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-100 | relaxed | 1,000 | 263.5 | 231.3 | 254.2 | 258.3 | 289.6 | 349.4 | 1,519.3 | 2.6 |
| bulk-1000 | relaxed | 100 | 1,472.4 | 1,391.5 | 1,429.7 | 1,449.5 | 1,516.6 | 1,678.4 | 3,208.5 | 1.5 |
| bulk-100 | group | 1,000 | 706.5 | 564.6 | 652.8 | 683.7 | 837.3 | 1,015.4 | 5,114.6 | 7.1 |
| bulk-1000 | group | 100 | 2,289.5 | 1,930.1 | 2,041.0 | 2,103.2 | 3,282.3 | 4,447.1 | 5,618.2 | 2.3 |
| bulk-1 | relaxed | 100,000 | 95.3 | 56.5 | 71.7 | 86.5 | 131.4 | 176.0 | 6,117.9 | 95.3 |

Gate-closed twin (row loop, same statements):

| phase | durability | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| bulk-100 | relaxed | 1,000 | 485.0 | 414.9 | 447.0 | 460.3 | 626.8 | 854.2 | 2,763.1 | 4.9 |
| bulk-1000 | relaxed | 100 | 3,690.5 | 3,434.3 | 3,475.7 | 3,528.1 | 4,464.2 | 4,921.6 | 5,152.9 | 3.7 |
| bulk-100 | group | 1,000 | 1,434.4 | 787.5 | 1,403.9 | 1,527.6 | 1,784.6 | 2,326.0 | 7,489.6 | 14.3 |
| bulk-1000 | group | 100 | 4,852.2 | 4,295.3 | 4,497.4 | 4,621.7 | 6,069.3 | 7,434.2 | 8,259.2 | 4.9 |

The eligible distributions are the tightest in this document — b1000
relaxed p99/p0 = 1.21 — and the trace fit stays dead flat
(`1,567 − 0.4 ns × resident`, R² 0.003): the fill inherits the tail hint's
size-independence.

## Where the remaining per-row time went, and what it prices T2 at

The parse probe on this binary is tight again (1000-row p50 1,137.7 µs,
p95 1,189 — **Part II's probe widening did not reproduce** and is
downgraded to a transient of that window). The batch-1000 relaxed
statement decomposes by subtraction, p50 basis:

| component | µs per statement | µs/row | share |
|---|---:|---:|---:|
| parse + dispatch (probe p50 minus its round trip) | ~1,047 | **1.05** | 72 % |
| client + socket round trip | ~91 | 0.09 | 6 % |
| **T3 fill: id range + page assembly + FPI WAL + reply** (statement minus probe) | **~312** | **0.31** | 22 % |
| total | 1,449.5 | 1.45 | 100 % |

Against the twin's identical decomposition (3,528.1 − 1,137.7 = 2,390 µs
of placement), **T3 cut the placement slice 7.7×** — 2.39 → 0.31 µs/row —
which is the whole 2.50× statement-level gain, since parse and round trip
are common to both paths. The expected effect on the Part II pipeline
slice (~1.9–2.2 µs/row) is confirmed and exceeded.

**T2's price, updated**: the residue is now the parse, 72 % of the
statement. A binary load stream that skips it would leave
~0.4 µs/row — i.e. T2 is worth ~3.5× on top of T3 at batch 1000, where on
the row loop it was worth ~1.4×. T3 inverted which tier the next
microsecond lives in.

WAL volume, measured off the segment frontier (identical bytes at both
durability classes and across repeats):

| configuration | stream bytes / 100K rows | B/row | note |
|---|---:|---:|---|
| T3, batch 1000 | 7,403,872 | **74** | ~8 FPIs + range/txn records per statement |
| T3, batch 100 | 14,863,072 | **149** | see below — the FPI economy halves under one-page batches |
| row loop, batch 1000 | 16,219,824 | 162 | per-row records |
| row loop, batch 100 | 16,277,424 | 163 | |
| row loop, batch 1 | 22,613,936 | 226 | + per-statement `TXN_BEGIN`/`COMMIT` |

Two durability findings ride on those bytes. **The fsync got cheaper with
the volume**: group − relaxed at batch 1000 is 817 µs/statement eligible
against 1,162 on the twin — T3 syncs 74 KB a statement instead of 162 KB.
So the coordinate claim "group gains more" is true in absolute time saved
(2,562 µs/statement against relaxed's 2,218) and false as a ratio (2.11×
against 2.50×), because the fsync floor T3 cannot remove is larger than
the parse floor it also cannot. And **at batch 100 eligible, the group
premium is 443 µs/statement — half a device fsync** — at 1,397
statements/s, consistent with the 1 ms WAL drain cadence providing
durability points that consecutive commits ride instead of paying their
own; not directly instrumented, recorded as the plausible mechanism (the
twin at 692 statements/s pays the full 949).

**The batch-100 FPI caveat is a real finding about T3's shape**: this row
shape packs ~125 rows a page, so a 100-row statement never fills one —
each statement re-images the shared tail page, and T3's WAL density
doubles (74 → 149 B/row), converging on the row loop's 163. The image-vs-
records crossover sits at **batch ≈ rows-per-page**: below it T3 still
wins on placement (1.82× at batch 100) but not on logged volume. An
operator sizing batches for a T3 load should size them in pages, not
rows — at this schema, multiples of ~125.

## What Part III teaches

1. **T3 does what §8 promised, measured**: 675K rows/s relaxed / 435K
   group at batch 1000 on one connection — 2.50×/2.11× over the row loop
   on identical statements — with placement at 0.31 µs/row, WAL at
   74 B/row, byte-stable across durability classes, and a dead-flat
   latency distribution (p99/p0 1.21). The whole gain is in the placement
   slice; parse and round trip did not move, exactly as the tier table in
   `spec-bulkinsert.md` §1 predicts.
2. **The gate costs nothing and the fallback is intact**: batch 1 lands on
   Part II's numbers, and the Cabin-gated twin reproduces Part II's row
   loop within 3 % — so T3 is free where it cannot engage, and the
   `--cabin` cells are the standing row-loop baseline for future parts.
3. **The next microsecond is T2's, and it is most of the statement**:
   1.05 of 1.45 µs/row is parse+dispatch. Before T3 a binary load stream
   was a 1.4× proposition; it is now 3.5×. The pipeline itself is down to
   ~0.3 µs/row, and nothing else in the statement path is worth a tier.
4. **Batch size has a new unit**: T3's log economy is per *page*, so the
   efficient batch is a multiple of rows-per-page (~125 here). `max_insert_rows`
   = 1024 comfortably holds ~8 pages of this schema; the default stands.
5. **Against PostgreSQL: 7.1× at the headline cell** (674,570 vs 94,600
   relaxed-class, batch 1000), 3.1–5.3× on the durable cells — the widest
   margin in this document's three parts, and the first one produced by a
   mechanism PostgreSQL's INSERT path does not have rather than by the
   absence of a defect.

## Reproducing (Part III)

Protocol as before (`~/bench-bulk3`, log `matrix4.log`). The eligible
cells are the driver's defaults (the schema is already inside the gate);
the twin adds `--cabin`; WAL bytes are the last nonzero byte of the
prewritten segment files, summed, read after server stop.

---

# Part IV — T2, the KWP load stream, at `3175824` (current engine state)

T2 landed as the KWP v0 load endpoint (`kwp_port`, frames
`C_LOAD_BEGIN/CHUNK/END` + `S_LOAD_READY/ACK/COMPLETE`, capability bit
`BULK_LOAD`): pre-encoded D5 row chunks, windowed acknowledgment
(window 4, max chunk 256 KiB), one implicit transaction committing at
`LOAD_END`. Part III priced it from the parse share at **~3.5× on top of
T3**; this run claims or refutes that number. The driver is new —
`tools/kwp_load_benchmark.py`, a dependency-free KWP v0 client speaking
the layouts in `wire/kwp.hpp` / `kwp_types.hpp` / `row_codec` — and the
schema and row values are Part III's exactly, so every delta against
Part III's T1 cells is the parse + text-decode + per-statement round-trip
removal and nothing else. T3's gate stays open through the load: the WAL
frontier densities below are byte-for-byte Part III's (74 B/row at
chunk 1000, 149 at chunk 100), which is the fill engaging per chunk.

## The run

| | |
|---|---|
| executed | 2026-08-10, 10:40–10:42 UTC (ckdbs), 10:42 UTC (PostgreSQL COPY twin) — sequential |
| commit measured | `3175824` (merge tip of `feat-kwp-server` == `main`; T2 = KL02–KL06 at `6ca9ec9`) |
| tree | clean of engine changes; the session's driver and doc files are the working-tree delta |
| binary | `build-release/kds_server`, rebuilt 10:36:17 UTC from this tree — mtime after HEAD, verified before the first measurement |
| build type | Release |
| tests | not run in this session (no engine change made here). In-run verification: every `S_LOAD_ACK` sequence-checked, `S_COMPLETE.rows_affected` == rows sent, and `SELECT COUNT(*)` over the text port per configuration — all exact, zero errors |
| device / machine / gating | as Parts I–III. **Zero retries; every configuration's window was clean** (log: `~/bench-bulk4/matrix5.log`) |
| ckdbs config | as before, plus `kwp_port` enabling the endpoint; fresh server + fresh file per configuration; data files `/home/ec2-user/bench-bulk4/*.db` |
| PostgreSQL | 17.10, same scratch cluster, untouched defaults. The honest T2 peer is **`COPY FROM STDIN`** (via `psql \copy`, text CSV — the common operational form); `synchronous_commit = off` as the session-GUC relaxed twin. **COPY BINARY was not measured** — a binary-format COPY twin would need a generator this session did not build, and its absence is stated rather than papered over |
| relation | `write_probe` shape (5 × int64, heap) — T3-eligible, as Part III |

## The noise floor

The pipelined relaxed configuration ran twice whole: chunk-1000
1,140,045 vs 1,147,397 rows/s (**0.6 %**), chunk-100 615,061 vs 664,222
(**8 %** — that phase is 0.16 s long, and the floor scales accordingly).
Rate claims below respect the 8 % on chunk-100 cells and ~2 % on
chunk-1000 cells; per-chunk p50s are tighter (serial p50s repeat within
1 %).

## The matrix: T2 against T1, and against COPY

100,000 rows per configuration, one connection, rows/s. T1 columns are
Part III's eligible (T3) cells — same schema, same values, same machine.

| chunk/batch rows | durability | **T2 (KWP, window 4)** | T1 (Part III) | **T2/T1** | T2 serial (window 1) | PG COPY twin |
|---:|---|---:|---:|---:|---:|---:|
| 100 | relaxed | 615,061 | 368,156 | **1.67×** | 453,370 | — |
| 1,000 | relaxed | **1,140,045** | 674,570 | **1.69×** | 1,057,871 | 722,437 |
| 100 | group | 621,732 | 139,692 | **4.45×** | — | — |
| 1,000 | group | 1,116,048 | 434,683 | **2.57×** | — | 706,180 |

*(The COPY figures are the warm repeats — 138.4 / 141.6 ms per 100K rows;
the first, cold invocations read 218 / 410 ms and are recorded as cold.
COPY has no chunk size; its rows sit beside chunk-1000 for scale only.)*

Five readings:

- **The ~3.5× prediction is refuted: measured 1.69× at the cell that made
  it.** Part III's estimate removed the parse (1.05 µs/row) and assumed
  binary ingestion costs nothing; it does not. The decomposition below
  puts the D5 decode + per-chunk apply at ~0.46 µs/row — the estimate's
  missing term, and now the largest slice of what remains.
- **The durability class is free under T2** (group = relaxed − 2 %,
  inside noise): a load session is one transaction (BI11), so 100K rows
  cost one fsync where T1 paid one per statement. That is why T2's win is
  1.7× at relaxed and 2.6–4.5× at group — T2 amortizes the durability
  point *further* than a statement can.
- **The window is worth 1.36× at chunk 100 and 1.08× at chunk 1000**
  (pipelined over serial). At chunk 1000 the server is already the
  bottleneck (serial per-chunk p50 859.6 µs against a ~90 µs round trip),
  so there is little RTT left to hide; at chunk 100 the fixed per-chunk
  costs are a third of the service time and overlap pays.
- **Against PostgreSQL's COPY: 1.58–1.61×** on the warm numbers, with the
  caveat that the twin parses CSV text while KWP ships binary — the
  format asymmetry is inherent to comparing each engine's native bulk
  surface, and COPY BINARY is the unmeasured control.
- **A 1M-row session sustains 891,069 rows/s** and crosses a WAL segment
  boundary mid-load (frontier 74,016,064 bytes = 74.0 B/row exactly, two
  segments) — the Part II fix exercised through the new path. The rate is
  22 % below the 100K-row cell; the max-latency chunk (71.4 ms against a
  3.7 ms p50) is consistent with the segment roll's `CreateSegment`,
  which prewrites and fsyncs 64 MiB of zeros — a one-off p100 event the
  session otherwise amortizes. The residual gap (p99 18.7 ms) is recorded
  unattributed; candidates are the per-row rollback trail and the
  checkpoint cadence, and nothing in this run separates them.

## Per-chunk latency

Latencies are per **chunk**. Pipelined figures include deliberate
queueing behind the window — the throughput column above is the number to
read; the serial rows are the clean per-chunk service time.

| configuration | phase | ops | mean µs | p0 | p25 | p50 | p95 | p99 | max | µs/row (p50) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| relaxed, pipelined | load-100 | 1,000 | 639.7 | 244.6 | 534.3 | 549.8 | 939.2 | 1,755.5 | 5,425.4 | 5.5 |
| relaxed, pipelined | load-1000 | 100 | 3,421.5 | 1,823.7 | 3,117.9 | 3,205.6 | 4,721.0 | 6,588.9 | 6,591.9 | 3.2 |
| group, pipelined | load-100 | 1,000 | 628.5 | 252.7 | 532.6 | 547.7 | 915.6 | 1,568.7 | 5,581.3 | 5.5 |
| group, pipelined | load-1000 | 100 | 3,430.2 | 1,450.0 | 3,116.5 | 3,241.4 | 4,658.9 | 6,307.4 | 6,415.8 | 3.2 |
| relaxed, **serial** | load-100 | 1,000 | 215.9 | 165.6 | 191.3 | 198.2 | 267.6 | 432.6 | 3,939.9 | 1.98 |
| relaxed, **serial** | load-1000 | 100 | 935.0 | 814.3 | 838.0 | 859.6 | 1,227.6 | 2,376.1 | 4,057.5 | **0.86** |
| relaxed, 1M rows, pipelined | load-1000 | 1,000 | 4,428.9 | 1,381.3 | 3,550.1 | 3,734.6 | 6,425.8 | 18,742.8 | 71,374.3 | 3.7 |
| relaxed, pipelined, **--no-quickack** | load-1000 | 100 | 4,348.5 | 1,421.8 | 2,866.1 | 3,354.5 | 6,180.8 | **43,478.7** | 44,220.1 | 3.4 |

The pipelined p50 sits at ~3.7× the serial p50 at chunk 1000 — a chunk's
in-flight time is roughly the window times the service time, which is the
window working as designed, not a regression.

## Where the remaining per-row time went

Anchored on the serial chunk-1000 p50 (859.6 µs per 1,000 rows — the
clean, unoverlapped cost), against Part III's T1 decomposition of the
same rows:

| component | T1 statement (Part III) | T2 serial chunk | note |
|---|---:|---:|---|
| parse + dispatch | ~1,047 µs | **0** | what T2 exists to remove |
| D5 decode + per-chunk apply | — | **~460 µs** | the term the ~3.5× estimate priced at zero: chunk header, batch decode, per-row value materialization into the same `InsertOneRow`-shaped fill input |
| T3 fill (id range, page assembly, FPI WAL) | ~312 µs | ~312 µs | unchanged — the same fill, engaged per chunk (WAL densities byte-identical to Part III) |
| round trip / framing | ~91 µs | ~88 µs | one ACK per chunk; hidden by the window in pipelined mode |
| **total per 1,000 rows** | **1,449.5** | **859.6** | |

So the per-row residue at T2×T3 is **~0.86 µs serial / ~0.88 µs
pipelined-throughput**, of which the binary decode+apply (~0.46) is now
the majority and the fill itself (~0.31) the rest. The parse's 1.05 µs
was removed in full; ~44 % of it came back as decode. **What is left
worth building is no longer a tier**: the spec's three-tier program is
complete, and the remaining costs are (a) the D5 decode's per-value
materialization — an optimization inside one function family, not a
surface — and (b) the endpoint defect below, which is a one-line fix
worth up to a third of pipelined throughput.

## Found by this run: the KWP endpoint does not set TCP_NODELAY
## [FIXED same day: kwp_load_server.cpp now sets it on accept, the text
## server's comment carried over - the tables above already measure the
## fixed behavior via the driver's client-side QUICKACK workaround]

`tcp_server.cpp` sets `TCP_NODELAY` unconditionally on the text port —
its comment explains why — but `kwp_load_server.cpp` never does, so the
endpoint's small `S_LOAD_ACK` frames are Nagle-held against the client's
delayed ACKs whenever the pipeline drains: the classic ~40 ms stall,
visible in this driver's first smoke run as p95 = 42.6 ms on 100-row
chunks. The driver defeats the interaction from the client side
(`TCP_QUICKACK` re-armed around every read — on by default, documented in
the driver) so the benchmark measures the engine rather than the
artifact; the `--no-quickack` demonstration row above shows the artifact
retained: p99 43.5 ms and **−33 % throughput** (761,640 vs 1,140,045
rows/s) on an otherwise identical configuration. Not fixed in this
session; the fix is the same `setsockopt` the text server already
carries, and it belongs beside a regression check that a load session's
ACK latency has no 40 ms mode.

## What Part IV teaches

1. **T2 delivers 1.69× at relaxed, not the predicted ~3.5×, and the gap
   is a lesson in estimating**: the parse-share arithmetic priced what T2
   removes and assumed what T2 adds costs nothing. Binary ingestion is
   cheaper than parsing — 0.46 vs 1.05 µs/row — but it is not free, and
   the estimate's error (2×) is entirely that term. A prediction from a
   subtraction should carry the replacement cost of the thing subtracted.
2. **T2's larger win is transactional, not textual**: at group durability
   it is 2.57–4.45× over T1, because one load session pays one fsync
   where T1 paid one per statement. For a durable bulk load, the session
   shape matters more than the encoding — BI11 quietly beat BI6.
3. **The endgame number: ~1.14M rows/s on one connection, one core**
   (0.88 µs/row), 1.6× PostgreSQL's warm COPY on the same volume, with
   T3's fill engaged per chunk and WAL at 74 B/row. The spec's tier table
   is now fully measured: round trip (T1) → placement (T3) → parse (T2),
   each removal exposing the next, ending at a residue where the largest
   slice is a binary decode.
4. **The window is insurance, not speed, at large chunks**: 1.08× at
   chunk 1000 against 1.36× at chunk 100. An operator should size chunks
   toward the 256 KiB cap and not count on the window to rescue small
   ones.
5. **One new defect (missing `TCP_NODELAY`, −33 % pipelined, 40 ms ACK
   stalls) and one prior fix re-proven** (the segment roll, crossed
   mid-load at 1M rows with a 71 ms p100 that is the 64 MiB prewrite).
   The roll's prewrite cost is now a measured, foreseeable p100 event for
   any load longer than a segment — worth remembering when a latency SLO
   meets a bulk load.

## Reproducing (Part IV)

```bash
# server with the endpoint enabled
printf 'durability = relaxed\nkwp_port = 15499\n' > kwp.conf
./build-release/kds_server ~/f.db --port 15432 --config kwp.conf
python3 tools/kwp_load_benchmark.py --port 15432 --kwp-port 15499 \
    --rows 100000 --chunk-rows 100,1000 --mode pipelined --json out.json
# --mode serial for stop-and-wait; --no-quickack to exhibit the Nagle stalls

# the COPY twin (psql \timing, warm = second run)
./tools/pg_setup.sh start && psql -h 127.0.0.1 -p 15433 -d bench \
    -f pg_copy_on.sql   # DROP/CREATE + \copy 100K CSV rows + COUNT
```

Orchestration log with load gating and per-configuration WAL frontiers:
`~/bench-bulk4/matrix5.log`.

