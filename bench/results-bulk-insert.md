# The T1 multi-row INSERT, priced — batch 1/10/100/1000 × three durability classes, against PostgreSQL 17.10

`docs/spec-bulkinsert.md` §0 claimed the single-row INSERT ceiling of ~9K
rows/s was ~21 µs/row of statement cost that never amortizes, and shipped T1
(BLK01–BLK05) as that cost's eviction notice. This document holds **two
runs of two engine states**, deliberately: the first run (at `9ee04e4`, the
T1 commit) found two engine defects — an O(pages-resident) tail walk under
every heap insert, and a WAL stream that wedged at an exactly-filled
segment — and both were fixed the same day; the second run (at `926f422`,
both fixes in) is the current state of the engine, re-measured whole. The
pre-fix sections are kept as the record of why the fixes exist and what
they were worth; every number in them describes an engine that no longer
ships. Same `write_probe` row shape as the baseline
(`bench/results-scenario1-vs-pg.md`); driver documentation:
[`bench/docs/README.md`](docs/README.md), `bulk_insert_benchmark.py` /
`pg_bulk_insert_benchmark.py`.

**Thesis, across the two runs: T1 works as designed — it removes the round
trip entirely and cuts the parse to ~1.1 µs/row — and what it exposed
underneath was worth more than what it removed: with the tail walk it
surfaced now fixed, a 1000-row INSERT costs a flat ~3.4–3.6 µs/row with no
dependence on relation size, 278K rows/s at relaxed durability against
PostgreSQL's 95K on the same volume, and the durability classes decompose
to exactly one device fsync per statement. The bulk-ingestion story now
ends where T3's territory begins: per-tuple placement itself.**

---

# Part I — the pre-fix run, at `9ee04e4` (superseded engine state)

This part is the record of the run that found the two defects. **Both are
fixed on `main` as of `926f422`; Part II is the current engine.** Part I's
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

# Part II — the post-fix run, at `926f422` (current engine state)

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

