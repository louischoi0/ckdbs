# Where a business transaction's time goes, by layer

Measured 2026-08-05 against `tools/stress_business.py`, decomposing **one
business transaction** — the four statements the scenario counts as its unit:

```
INSERT INTO trades ...    buy leg     (heap relation, WAL-logged)
INSERT INTO trades ...    sell leg
UPDATE accounts SET ...   buyer       (clustered btree, WAL-logged since docs/txn.md)
UPDATE accounts SET ...   seller
```

`bench/results-business-stress.md` measured this workload end to end and named
the WAL fsync as the ceiling. This document answers the next question — **how
many microseconds each layer actually spends** — and it revises that one on two
points: `UPDATE` is logged now, so the transaction is four logged statements
rather than two, and the fsync share is larger than the earlier document could
see.

---

## Method

Release build at `feat-fk` (uncommitted FK work included), xfs root volume on
an EBS gp3 device, AMD EPYC 7571, 2 cores, one `kds_server`. Dataset: 2,000
users / 4,015 accounts / 500 assets, 45 s measured phase, 1 trader process
unless stated.

The layer numbers come from **temporary instrumentation** added to
`CommandDispatcher` for this measurement and then reverted: a thread-local
nanosecond accumulator marked at each layer boundary and appended to the
existing debug `[query]` line as `ph=scope:…,parse:…,cat:…,id:…,enc:…,store:…,
wal:…,cmt:…,tail:…`. The patch is kept at `~/scratch/phase-instrumentation.patch`
and is ~10 clock reads per statement, i.e. under 0.3 µs against statements of
30 µs and up. Buckets:

| bucket | what it covers |
|---|---|
| `scope` | command routing + `BeginWrite` (transaction begin, read view) |
| `parse` | `parser::Parse` — SQL text to AST |
| `cat` | name → oid, `InitTableAccess` (cached), affinity, `CompileWhere` |
| `id` | `Catalog::AllocateRowId` |
| `enc` | `exec::EncodeRow` |
| `store` | heap tail append / btree descent + in-place overwrite, decode, predicate |
| `wal` | undo page allocate+init+write, WAL record encode + append |
| `cmt` | `EndWrite` — commit record, `DrainOnce`, `EnsureDurable` (**the fsync wait**) |
| `tail` | trail + access-stats recording (SELECT only) |

Client-side latencies are the tool's own per-request wall clock, so
`client − server` is the Python driver plus two syscalls plus the reactor.
Sub-microsecond differences are noise on both sides.

---

## 1. The budget, `durability = group` (the default)

**182 TPS**, transaction mean 5,473 µs. Per transaction, summing the four
statements' means:

| layer | µs | share |
|---|---|---|
| **WAL durability wait** (`cmt`, ×4) | **4,644** | **84.9 %** |
| client + socket + reactor (4 round trips) | 642 | 11.7 % |
| **engine work, everything else** | **158** | **2.9 %** |
| client-side driver loop | 29 | 0.5 % |
| total | 5,473 | |

The 158 µs of engine work, itemized:

| component | µs/txn | what it is |
|---|---|---|
| storage access | 46.8 | 2 heap tail appends + 2 btree descents with in-place overwrite |
| WAL + undo records | 32.8 | record encode/append; **31 of it is the two UPDATEs** (see §3) |
| parse | 30.8 | 4 statements of SQL text |
| transaction scope | 29.4 | 4 × (begin, read view, routing) |
| catalog bind | 9.8 | cached; 4 lookups |
| Keystone id allocation | 3.6 | 2 inserts |
| row encode | 2.8 | 2 inserts |
| residual | 2.4 | |

Per statement, server-side means: `INSERT trades` 1,188 µs of which 1,149 is
the commit wait; `UPDATE accounts` 1,213 µs of which 1,173 is the commit wait.

**Nothing above the durability wait is worth optimizing at this ratio.** Parse,
catalog, encode and id allocation together are 47 µs — 0.9 % of a transaction.

## 2. Group commit amortizes nothing

| | TPS | commit p50 (server) | txn p50 (client) |
|---|---|---|---|
| 1 trader | 182.0 | 926 µs | 5,304 µs |
| 4 traders | 177.2 | 932 µs | — |

Four concurrent committers produce the **same** per-commit wait as one, and
slightly lower throughput. `docs/client-manual.md` says `group` matching
`strict` on one connection is expected; matching it on four is not.

The cause is structural, not a tuning miss. `EndWrite` (and `LogInsert` for the
no-manager path) calls `DrainOnce()` and `EnsureDurable()` **inline**, on the
one thread that also accepts and dispatches every other connection's statement.
A batch can only form if a second commit is staged while the first waits — and
the thread that would stage it is the thread parked in the drain. A batch is
therefore always one commit, at every connection count.

Two supporting numbers. A raw `fsync`/`fdatasync` of a 400-byte append on this
filesystem is **1,975 / 1,983 µs p50** — more than twice the engine's 926 µs
commit wait, so the engine is *already* sharing some syncs with the 1 ms
background drain (`wal_drain_interval_us`). And 728 statements/s × 926 µs =
0.67 s of every wall-clock second is spent inside that wait.

## 3. Every UPDATE burns a fresh 8 KB undo page — **fixed 2026-08-05**

Measured file growth, same workload, nothing but the run in the instance:

| run | UPDATEs | data file |
|---|---|---|
| group, 45 s | 16,414 | 132 MB |
| relaxed, 18 s | 45,362 | 360 MB |
| relaxed, 45 s | ~65,000 | **510 MB, then failure** |

The 45 s relaxed run ended in 97,826 × `ERR DevicePageStore: no free page id at
or above 65280` — the instance-wide page ceiling — after which every UPDATE
failed for the remaining 17 s of the run.

Cause: `UndoLog::TailFor` keys the tail page by `trx_id`. In autocommit **every
statement is its own transaction**, so there is never a tail to append to and
each UPDATE calls `PageStore::CreateNew()` for a page it writes one 28-byte
record plus a ~60-byte image into. `Forget()` drops the tail at commit, nothing
purges, and the page is never touched again. That is ~292× write amplification,
one free-map allocation and one `PAGE_INIT` per UPDATE, and it is why `wal` is
15.3 µs on an UPDATE against 1.1 µs on an INSERT.

Two ceilings behind it, both worth stating plainly:

- **The free map is one page, so the whole instance is ~65,280 pages ≈ 510 MB.**
  The same class of undeclared limit as the catalog's ~62 columns
  (`docs/keystoneid-k0-findings.md`). **Still true and still undeclared** — the
  fix below stopped reaching it, it did not raise it.
- At this consumption rate an instance reached that ceiling after ~65,000
  autocommitted UPDATEs — 18 seconds of a `relaxed` run.

### The fix, and what it measured

`UndoLog` now keeps **one current page shared by every transaction**, appended
to until it fills (`include/kds/txn/undo_log.hpp`, `docs/txn.md` §3.2). The
per-transaction tail table is gone with `Forget()`, `PageCountFor(trx_id)`
became `PageCount()`, and the page header's `owner_trx_id` became
`first_trx_id` — a diagnostic that grants nothing. Same offsets, same widths,
no format version moved. Nothing was relying on the exclusivity: readers follow
`undo_ptr` to a page and offset, rollback replays the in-memory trail, and redo
names each record's offset, so interleaved writers replay onto one page in LSN
order correctly.

Same workload, same 45 s, same host, before and after:

| | before | after |
|---|---|---|
| `relaxed` TPS | 716.4 | **1,343.7** |
| committed | 32,240 | 60,454 |
| failed statements | 97,826 | **0** |
| data file | 510 MB (ceiling) | **19 MB** |
| `group` TPS | 182.0 | **199.7** |
| `group` data file | 132 MB | **5 MB** |

The `relaxed` figure is not a like-for-like throughput comparison — the old run
spent its second half failing fast, which is *cheaper* per statement than
working — so read it as "the workload now completes" rather than as 1.9×. The
`group` pair is like-for-like: **+9.7 % TPS and 26× less file** for the same
committed work, from not allocating, formatting and logging a page per UPDATE.
576/576 tests pass, `undo_log_test.cpp` carrying two new cases that pin the
sharing.

## 4. Every 4,096th statement pays a full page-store Sync

In the 18 s relaxed run, **23 statements took over 5 ms and account for 1.05 s —
5.8 % of the wall clock**. The run issued 90,724 statements; 90,724 / 4,096 =
22. `scope` p50 is 1.6 µs and its max is 76.6 ms.

`TrxIdSequence::ReserveBlock` (a block is `kTrxIdBlockSize` = 4,096 ids) calls
the persist hook, which is `Expeditor::PersistSuperBlock` → `Sync()` — and
`Sync()` flushes **every dirty page in the store**, not page 0. With tens of
thousands of dirty undo pages resident (§3), that bulk flush happens inside
whichever statement drew the first id of a block.

Invisible under `group`, where a 2 ms fsync per statement hides it. It is a
measurable 5.8 % as soon as the fsync goes away.

## 5. Without the durability wait

`durability = relaxed`, same workload, 18 s (short enough to finish before §3's
ceiling): **1,260 TPS, 6.9× the `group` run.** Transaction mean 776 µs:

| layer | µs | share |
|---|---|---|
| client + socket + reactor | 554 | 71.4 % |
| engine work | 190 | 24.4 % |
| commit | 9 | 1.1 % |
| residual | 23 | 3.0 % |

Server-side per statement: `INSERT trades` 62.5 µs mean / 37 µs p50,
`UPDATE accounts` 36.7 / 32.

**The client round trip is 97 µs and the server's share of it is 1 µs**: 3,000
`PING`s measure 97.5 µs p50 client-side against 1 µs server-side. So once
durability stops dominating, the newline protocol's one-statement-per-round-trip
shape is the bottleneck, not the engine.

## 6. The read path

The reporting job's `SELECT * FROM accounts WHERE user_id = <n>` over 4,015
accounts, server-side mean 1,660 µs:

| layer | µs | share |
|---|---|---|
| execute + format (`store`) | 1,633 | 98.3 % |
| compile (`cat`) | 13.3 | 0.8 % |
| parse | 8.6 | 0.5 % |
| trail + access stats (`tail`) | 1.3 | 0.1 % |

**0.41 µs per row walked.** For reads the lever is fewer rows, never less
per-statement overhead — which is what the Cabin (`--cabin`) and, eventually,
an index on the column are for. Note the row formatting (`ostringstream` +
`FormatValue` per value, ~24,000 appends here) is inside the execute bucket and
is not separately measured; `include/kds/wire/row_codec.hpp` would remove it.

With this dataset the reporter costs almost nothing (178.5 TPS with it against
182.0 without) because a 1.6 ms scan is small beside a 5.5 ms transaction. At
`results-business-stress.md`'s 20,012 accounts the same scan was 28.7 ms and
cost 48 % of throughput — the cost of this job is a function of the relation's
size, and this run does not contradict that one.

---

## What to change, in measured order

1. **Make the commit wait a suspension point** (largest, structural).
   `CommandDispatcher::DispatchAsync` is already a coroutine and
   `sched/coro.hpp` has `WaitFor{&flag}`; `EndWrite` should `co_await`
   durability instead of calling `DrainOnce`/`EnsureDurable` inline. Then N
   connections stage N commits into one drain and one fsync covers all of them,
   which is what `group` was specified to do and today cannot. The suspension
   audit permits it: the wait is after every page span is released. This is
   `docs/workplan-crosscore.md` P4's machinery applied to the local path, and it
   is the only change here that lets throughput scale with connections.
2. ~~**Stop allocating an undo page per transaction**~~ — **done 2026-08-05**,
   see §3. One core-local current page shared by every transaction: +9.7 % TPS
   under `group`, 26× smaller data file, and the 510 MB-in-18-seconds failure
   mode gone.
3. **Persist the superblock alone** (§4). `PersistSuperBlock` should write page
   0 and sync page 0, not flush the store. Worth 5.8 % once §1 lands, ~0 before.
4. **Wrap the four statements in one transaction — client-side, no engine work.**
   The tool sends four autocommits deliberately (its docstring says why), so
   this is a scenario to add rather than a change to make: four commits become
   one, two undo pages become one, four transaction ids become one. At a 926 µs
   commit that predicts ~2.0 ms per transaction against 5.5 ms, ≈2.7×, and it is
   what a real client would do. Worth measuring before doing engine work,
   because it moves the same fsync count that item 1 does.
5. **Then the protocol** (§5). At 97 µs per round trip and 1 µs of server time
   in it, KWP/1's pipelining and binary rows are the next ceiling — but only
   after 1 and 4, which are worth 3-7× between them.

**Not worth touching**: parse (6-9 µs), catalog bind (0.7-4 µs), row encode
(1.4 µs), Keystone id allocation (1.8 µs). Together 0.9 % of a transaction.

## Open

- Whether item 1 delivers the amortization it predicts is unmeasured; the
  prediction rests on the raw fsync being 2.0 ms while a commit waits 0.93 ms,
  i.e. on syncs already being partially shared.
- The row-formatting share of §6's 1,633 µs was not separated.
- 4-trader numbers were taken only under `group`, where the fsync hides
  everything; the scaling curve under `relaxed` is not measured.
