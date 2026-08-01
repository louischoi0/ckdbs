# Waystone read path — pk point lookup and point update, 2026-07-30

What a Waystone probe is worth on the two statement shapes that address a
single row by pk — `SELECT * FROM t WHERE id = <n>` and
`UPDATE t SET c_int = <n> WHERE id = <n>` — measured against the same
statements on the same engine with Waystone off. Raw output in
`bench/results/waystone/u_<rows>_<on|off>.json` (both phases) and
`r_<rows>_<on|off>.json` (the earlier select-only run).

The comparison is deliberately narrow: **two query shapes, one variable.** Same
binary, same host, same data file layout, same durability class, same row
shape. The only difference between an `off` row and an `on` row below is
whether `WAYSTONE ENABLE` ran before the load phase.

## 1. Setup

| | |
|---|---|
| Host | AWS EC2, AMD EPYC 7571, 2 vCPU, 7 GiB RAM |
| Data file | `/` — `nvme0n1p1`, xfs, EBS gp3. **Not tmpfs** (see §5) |
| Build | `cmake -DCMAKE_BUILD_TYPE=Release` |
| Table | 5 columns: Keystone pk + `int64`, `int32`, `bool`, `varchar(16)` |
| Queries | `SELECT * FROM t WHERE id = <uniform random 1..rows>` and `UPDATE t SET c_int = <n> WHERE id = <same>`, 1,000 ops each |
| `durability` | `relaxed` |
| Connections | 1 (the server accepts one at a time) |

`durability = relaxed` is a load-phase choice, not a read-path one: `INSERT` is
WAL-logged and `strict`/`group` cost an fsync per row, which would have made
loading 50,000 rows a benchmark of the disk rather than a setup step. It has no
bearing on the two measured phases: `SELECT` appends nothing to the log, and
`UPDATE` is not logged at all yet.

## 2. Result

Server-side p50 is the engine's own measurement of the whole statement,
excluding the client round trip. It is the column to read.

**point-select** — `SELECT * FROM t WHERE id = <n>`

| rows | scan qps | scan p50 | **probe qps** | **probe p50** | engine speedup |
|---|---|---|---|---|---|
| 1,000 | 1,331 | 562 µs | **8,547** | **11 µs** | **51×** |
| 10,000 | 167 | 5,177 µs | **8,784** | **12 µs** | **431×** |
| 50,000 | 33 | 28,044 µs | **8,261** | **12 µs** | **2,337×** |

**point-update** — `UPDATE t SET c_int = <n> WHERE id = <n>`

| rows | scan qps | scan p50 | **probe qps** | **probe p50** | engine speedup |
|---|---|---|---|---|---|
| 1,000 | 1,268 | 551 µs | **8,647** | **11 µs** | **50×** |
| 10,000 | 165 | 5,168 µs | **8,812** | **12 µs** | **431×** |
| 50,000 | 34 | 27,899 µs | **8,202** | **12 µs** | **2,325×** |

The two phases track each other to within run-to-run noise at every size,
which is the expected result: both are one directory walk plus one slot
access, and the in-place overwrite adds nothing measurable to the read.
`probe_misses=0` in every `on` run — nothing fell through.

An UPDATE is unlogged today, so its `on` numbers are not a durable-write
figure. When UPDATE starts logging, the fsync will dominate it exactly as it
already dominates INSERT (§6).

## 3. The shape is the point, not the multiple

The multiple is a property of the row count, so quoting "2,337×" without "at
50,000 rows" says nothing. What is scale-free is the shape of the two curves:

**The scan is linear in relation size**, at ~0.55 µs per row, and identically
so for both statements:

    rows        select p50    update p50    µs/row
    1,000          562 µs        551 µs      0.56
    10,000       5,177 µs      5,168 µs      0.52
    50,000      28,044 µs     27,899 µs      0.56

**The probe is constant.** 11-12 µs at every row count for both phases, p95
14→17 µs. Ten and then fifty times the data, same latency — which is what a directory walk of at most
`kMaxDirDepth` hops plus one entry read has to look like if the addressing
arithmetic is right (`pk >> 8` for the page, `pk & 0xFF` for the slot).

Extrapolating the scan is straightforward and unflattering: at 1M rows it is
~0.56 s per point lookup. The probe is still 12 µs.

## 4. The client is now the bottleneck

Probe qps flattens at ~8,700–8,800 across all three row counts while server-side
time stays at 12 µs. That ceiling is not the engine — it is ~110 µs of Python
socket overhead and newline framing per request. The engine is doing ~83,000
lookups/s worth of work and the wire is delivering 8,700 of them.

Two consequences worth being explicit about:

- **These qps numbers understate the read path by roughly 9×.** Any further
  read-path optimization is invisible in this column and visible only in the
  server-side one.
- **This is the case for KWP/1** (`docs/protocol.md`) and for multi-connection
  serving, not for more index work. The scan-vs-probe gap is closed; the
  remaining gap is transport.

## 5. Do not run this on tmpfs

An earlier attempt at the durability benchmark put the data file under `/tmp`,
which is tmpfs on this host. `fsync` is free there, all three durability
classes measured identically, and `strict` reported 7,254 inserts/s against its
real 802. The same trap applies to any page-store measurement. `df -T` the data
directory before believing a number.

## 6. What this does *not* measure

- **Only `WHERE id = <n>`, on SELECT and UPDATE.** An extra `AND`, a non-pk
  column, a range predicate, or no WHERE at all falls through to the full scan
  by design — the probe answers "where is pk N" and nothing else. Both
  statements share one `PkEqualityTarget()` so they cannot disagree about which
  predicates qualify. Range access needs `min_key` page pruning or the B+ tree.
- **DELETE has no probe** because there is no DELETE statement yet.
- **Full-table scan is unchanged**, as it must be: `SELECT * FROM t` reads every
  page either way.
- **`INSERT` is unaffected.** Measured separately at 716 qps (off) vs 719 qps
  (on), server-side 950 µs vs 944 µs. The extra 32-byte entry write is O(1) and
  invisible next to a commit fsync. Under `relaxed` — where an insert is ~12 µs
  of engine time — the entry write is the only thing that could show, and it
  does not exceed run-to-run variance.
- **Correctness is not benchmarked, it is tested.** `tests/waystone_select_test.cpp`
  asserts byte-identical results with Waystone on and off, including a
  hand-poisoned entry pointing at another row's slot.

## 7. Note on `bench/results.md`

That file (the PostgreSQL comparison) states "ckdbs has no index and appends
nothing to its WAL yet". Both claims are stale as of 2026-07-30: `INSERT` is
logged, and a Waystone-enabled relation has an O(1) pk lookup. Its point-select
and insert numbers should be re-run before being quoted again.
