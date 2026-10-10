# Work order BA — parallelism against the PostgreSQL floor: twelve serialisation points

Written 2026-10-02 on `worktree-parallelism-workorder` from `d3d90b5`
(`v2.7.0-588-gd3d90b5`), on the operator's *"we will improve where
parallelism falls short compared with postgres. Plan an improvement for each
cause analysed and write the work document"*. It plans one fix per cause, for every place where
work on one core waits for work on another, and PostgreSQL 18.6's work does
not. **Opened 2026-10-09** (below). It cuts no tag. Like AS and AZ it sits outside AR0 §8's chain: it depends on no open
order and gates none.

**Paused 2026-10-06 for its sub-milestone BB** (`raft-marks-2026-10-06.md`
§4): BA-Q14 is marked (b), and `workorder-bb-issue-under-the-leaf.md` takes
defect A in BA-S1b's place. BA resumes at BB's close (BB-S5).
**Resumed 2026-10-08**: BB closed (`raft-marks-2026-10-08.md` §6), and BD-S6
had rebased these rows (§6, "BA rebased on BD"). BA-S2 waited for its own
word (given 2026-10-09, below).
**Opened 2026-10-09** (`raft-marks-2026-10-09.md` §1), on the operator's
*"continue BA from the work that was stopped; I want to finish BA"*: BA-Q0..Q13 were reviewed
one by one and marked in §4, eight of them in wording restated to the tree
at `d43845a0` (§1.14). Stages run from BA-S2 in §5's order without a word
per stage, and a question a stage raises is settled by CLA's proposal,
recorded as adopted (`raft-marks-2026-10-09.md` §2).

**Where the items came from.** The starting point was
`bench/v3.0.0/results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md`, plus a
spec-level reading of what serialises. §1 re-reads every claim in the code
at `d3d90b5`. Two of the spec-level claims were wrong:

- minting a snapshot takes no latch;
- the read view's latch is taken per *row*, not per statement.

The code also showed points that no spec names. The most frequent is one
mutex on every page access (P1). Nothing here has been measured, so no item
is called the largest. The survey also found three defects, each with its
own entry (§0).

## 0. The items

Each row is an instance-wide point where a statement on one core waits for a
statement on another, at `cores > 1` unless the row says otherwise. Every
row waits the same way, and §1.0 says why: the reactor thread itself waits,
so every session on that core waits with it.

| # | the point | how often | PostgreSQL 18.6 | stage |
|---|---|---|---|---|
| P1 | **The frame table**: one `std::mutex` over one `unordered_map`, with every pin count under it and the free map's latch nested inside | every page access on every core: 3 holds per read fetch and 4 per write fetch, plus one free-map hold | 128 buffer-mapping partitions, each looked up under a shared lock; a pin is a compare-and-swap on the buffer's state word | BA-S8 |
| P2 | **The visibility window**: `window_latch_`, a `std::mutex` that exists even at `cores = 1` | every row a reader classifies whose writer is above the floor, which in practice means recently written rows; 2 holds per commit, plus `Reclaim`'s scan | a snapshot under `ProcArrayLock`, shared, reused while no transaction has completed; a tuple is judged from the snapshot and its hint bits, with no shared lock | BA-S9 |
| P3 | **Statistics on the read path**: catalog page 11 latched exclusive, the optimizer collector's latch, and `CabinStore`'s `stats_latch_` | page 11 on every step of every successful `SELECT`; the collector on every fingerprinted `SELECT`; `stats_latch_` on every Cabin hit | backend-local counters, flushed at most once per `PGSTAT_MIN_INTERVAL` (1,000 ms) | BA-S5 |
| P4 | **The relation borrow**: one lock-table partition per relation; `IX` asked again for every written row; a decide whose release is quadratic in the borrows | 3 holds per `SELECT`; 1 per written row; 2 per borrow at the decide | weak relation locks in per-backend fast-path slots; the shared table only once a strong lock is counted | BA-S10 |
| P5 | **The pk mark**: every relation's `sys.tables` row is in one chain starting at page 7, held exclusive while a WAL record is appended. Issue order is not placement order across cores, which a btree leaf can show (defect A) | every omitted-pk row, and every named pk at or above the mark | `nextval` writes one WAL record per `SEQ_LOG_VALS` (32) values | BA-S1b, BA-S11 |
| P6 | **The WAL append**: one `std::mutex` held over encode, copy and CRC, and also over a ring-full `pwrite`, a segment roll's preallocation, its prewrite and two `fsync`s, and every flush's `pwrite` | every record | space reserved under a spinlock; records copied in parallel under one of eight insertion locks | BA-S12 |
| P7 | **Commit durability**: core 0 syncs inline on its reactor, and `strict` there pays one sync per commit with no sharing. A `strict` commit holds its snapshot marker across its sync, which caps every core's new snapshot (defect B). A peer's `strict` blocks its reactor on a condition variable, and a peer with a parked `group` commit spins its reactor. Every sync `fdatasync`s every segment ever written. A peer's writeback runs core 0's inline sync on the peer's thread (defect C) | every commit | the backend waits for `WALWriteLock` and skips its own sync when another backend's flush already covered its record | BA-S1, BA-S1c, BA-S7 |
| P8 | **The assertion directory**: its latch on every write commit, assertions or none; on every row of every relation once one assertion exists | every write commit; every written row | — (PostgreSQL has no `CREATE ASSERTION`) | BA-S6 |
| P9 | **The rightmost leaf**: monotonic pks send every core's insert to one leaf, whose latch is held across the Cabin witness, index maintenance, the assertion reservation, the undo append and the WAL append. A stale descent is refused `TXN_CONFLICT retryable=1` to the client | every insert | a B-link tree: a descent moves right past a concurrent split instead of failing | BA-S14 |
| P10 | **No yield inside a statement**: a long walk holds its core until it ends, and `C_CANCEL` has no handler | every long statement | each backend is an OS-preempted process; `CHECK_FOR_INTERRUPTS` serves a cancel | BA-S15 |
| P11 | **Session placement**: the kernel's `SO_REUSEPORT` hash decides, with no load-aware choice and no migration | every connection | the OS schedules backends across every CPU | BA-S16 |
| P12 | **Periodic stalls on a reactor**: a transaction-id carve syncs the whole pool inside `Begin`, and a checkpoint runs to completion on its core | every 4,096 transactions per core; every 5 s per core | assigning an XID writes no data page; the checkpointer is a separate process | BA-S13 |

**Defects found on the way.** Each has an entry under
`docs/inflight/bugs/`, and a stage here that is exempt from the census
(BA-Q1):

- **A, a quiet wrong answer.** `ORDER BY <pk>` can be elided over a btree
  leaf that two cores filled out of order:
  `order-by-pk-is-elided-over-a-btree-leaf-two-cores-filled-out-of-order.md`
  (BA-S1b).
- **B, a quiet wrong answer.** A `strict` commit's marker caps every core's
  snapshot across its sync:
  `a-strict-commits-marker-caps-every-cores-snapshot-across-its-sync.md`
  (BA-S1c).
- **C, a data race, fixed by BA-S1.** A peer's writeback ran core 0's WAL
  sync on the peer's thread. BA-S1 deleted the entry, which is at
  `git show e7617b2:docs/inflight/bugs/a-peers-writeback-runs-core-0s-wal-sync-on-the-peers-thread.md`.

**What the tree has measured**, each pointing at its item:

- **Scenario 0** (eight traders, four autocommit statements a transaction).
  Going from `cores = 1` to `cores = 8` takes `group` from 626.0 to 661.0
  tps (1.06x) and `strict` from 169.4 to 331.7 tps. PostgreSQL reads 710.8
  tps (`results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md` §2). At
  `cores = 1`, `strict` makes 678 durable statements a second, one every
  1.5 ms (`results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md` §5): the
  arithmetic of one sync per commit (P7).
- **Refusals under contention.** Every `s0-c8-s` run had one trade insert
  refused `TXN_CONFLICT` by the btree's bounded re-descent (same file, §6)
  (P9).
- **Serial reads, one session.** `cores = 8` reads 3–5 % below `cores = 1`
  in every scenario-3 cell (summary §4): what P1–P4 cost with nobody
  contending.
- **No measurement of what extra cores buy.** AT-S13
  (`results-at-s13-prices-v2.7.0-391-gf6f2073.md`, at `9a0525d`) found its
  Python driver saturated at 18–20k qps before the engine did. **No number
  in the tree prices what eight reactors buy**, and BA-S2..S4 close that gap
  first.

**What this order does not plan:**

- **Intra-query parallelism** (parallel scan, join, aggregate). It is a
  general-purpose feature (`CLAUDE.md`). The measured row-walk gap (70
  against 34 ns a row, summary §4) is one core's per-row cost, and whether
  PostgreSQL ran a parallel worker in those 10,000-row cells was not
  recorded.
- **Row locks in the tuple header.** Invariant 12 fixes the MVCC header at
  20 bytes with no `xmax`.
- **Per-core WAL streams.** D3 is ratified as one stream
  (`raft-marks-2026-09-28.md` §7), and D14 refuses pre-M0 volumes.
- **Per-core pk blocks.** Invariant 11 and AT-S10b forbid them
  (`heap-and-tuple.md` §4.1a, *No core caches a block*).
- **`CREATE INDEX CONCURRENTLY`.** It buys availability, not throughput, and
  a DDL takes its relation in `X` by design (AT-S5e).
- **Cabin's banking gate under concurrency** (`cabin.md` §6a). That belongs
  to AR1, whose next letters are AQ and AR.

## 1. Survey at `d3d90b5`

Read, not run. Every KDS citation is to `d3d90b5`. Every PostgreSQL
citation is to the 18.6 release tarball (`postgresql-18.6/…`), the source of
the binary the comparison ran (summary §1).

### 1.0 Every wait blocks a reactor

Every wait below is the reactor thread waiting, never one task:

- **`Latch`** is `std::mutex` (`include/kds/base/latch.hpp:52`), so a
  contended acquisition puts the reactor thread to sleep in the kernel.
- **The page latch** spins 64 turns (`kPageLatchSpinTurns`,
  `include/kds/storage/page_latch.hpp:94`), then calls
  `std::this_thread::yield()` on every turn (`:163-169`).
- **Peer reactors are pinned to CPU k** (`src/server/expeditor.cpp:1203-1206`,
  applied at `:1822`); core 0's is not. On a pinned reactor, a yield usually
  finds nothing else to run.
- **The only awaitables** are `Yield`, `WaitUntil` and `WaitFor`
  (`include/kds/sched/coro.hpp:416-475`). Each re-checks a predicate and has
  no owner or queue. No suspending mutex exists.
- **Other waits block the thread too:**
  - a second core missing the same page (`loading_done_.wait`,
    `src/storage/device_page_store.cpp:1966`);
  - `AwaitWritebackClaim`'s 50 µs `sleep_for` (`:1353`);
  - `WalWriter::EnsureDurable`'s condition variable
    (`src/wal/writer.cpp:34-38`).

**These waits cannot simply become suspensions.** A waiter usually holds
pins and page latches, and no task parks holding a pin
(`page_latch.hpp:36`, audited in debug builds by
`exec::InstallSuspendAudit`). So every row below is fixed by holding less,
less often (BA-Q13). In PostgreSQL a backend waiting on an LWLock sleeps on
its own semaphore, and only that session waits.

### 1.1 P1 — the frame table

- **The map and its latch.** `frames_` is one `std::unordered_map<PageId,
  Frame>` (`include/kds/storage/device_page_store.hpp:1456`) under
  `frames_latch_` (`:1302`). That latch is a `Latch`, armed only above one
  core (`:1303`).
- **The holds per page access:**
  - a fetch takes it (`src/storage/device_page_store.cpp:1914`);
  - an unpin takes it twice (`:2309`, `:2341`);
  - a write fetch takes it once more, to mark the frame dirty
    (`:2294-2296`);
  - `StampPageLsn` takes it on every logged mutation (`:1253`).
- **Pins are not atomic.** Each frame's pin count is a plain `uint32_t`
  under the latch: *"AM-S2 decided against making this atomic"*
  (`device_page_store.hpp:801-812`).
- **The free map's latch is nested inside.** Every fetch asks `IsAllocated`
  (`device_page_store.cpp:1225`), which takes the free map's latch (`:546`).
  On a hit that happens inside the frame-table hold. And on a hit the check
  proves nothing, because *"Nothing frees a page"* (`docs/spec/page.md:130`).
- **The sweep.** With `buffer_pool_frames` above 0, every miss on a full
  pool runs the CLOCK sweep under the same mutex (`:690-693`). The sweep
  copies and sorts every resident page id (`:2503-2506`). The default is 0,
  unbounded (`include/kds/server/expeditor.hpp:130`).
- **PostgreSQL:**
  - 128 partitions (`NUM_BUFFER_PARTITIONS`,
    `src/include/storage/lwlock.h:93`), each looked up under a shared lock
    (`src/backend/storage/buffer/bufmgr.c:582`, `:2034`);
  - `PinBuffer` (`:3090`) pins with `pg_atomic_compare_exchange_u32` on the
    state word (`:3135`).

### 1.2 P2 — the visibility window

- **Minting a view is lock-free:** atomics only (`src/txn/manager.cpp:139`;
  `src/txn/instance_visibility.cpp:94-116`).
- **Judging a row is not.** `ReadView::Visible` returns early only below the
  floor (`include/kds/txn/read_view.hpp:127`). Above it, `LookupCommit` takes
  `window_latch_` (`instance_visibility.cpp:280`), once for every row
  `Classify` reaches (`src/exec/step_vm.cpp:2088`).
- **Which rows take it.** The window is reclaimed only once it passes
  `kReclaimFloor` (1,024) entries
  (`include/kds/txn/instance_visibility.hpp:578-581`, *"Not measured"*).
  Above one core the floor is also capped by the slowest core's issue cursor
  (`instance_visibility.cpp:138-153`). So the rows that take the latch are
  the recently written ones, which in OLTP are the hot ones.
- **Commits take it too:**
  - in `BeginCommit` (`:84-85`);
  - in `PublishCommit` (`:246-271`);
  - and past a threshold, `Reclaim` scans and erases the window under it
    (`:300-333`).
- **It is a member, not a null-able pointer** (`instance_visibility.hpp:561`),
  so `cores = 1` pays it too.
- **PostgreSQL:**
  - takes a snapshot under `ProcArrayLock`, shared, and reuses it while
    `xactCompletionCount` has not moved (`GetSnapshotDataReuse`,
    `src/backend/storage/ipc/procarray.c:2095`; the bump, `:594`);
  - `HeapTupleSatisfiesMVCC` (`heapam_visibility.c:960`) judges a tuple from
    the snapshot (`XidInMVCCSnapshot`) and sets hint bits (`:144`), so a later
    reader skips the commit log.

### 1.3 P3 — statistics on the read path

**`Catalog::RecordAccess`** holds page 11 exclusive for its whole body
(`src/catalog/catalog.cpp:2950`). That body walks up to 4,096 shapes or
inserts one, and the code itself says what that costs (`:2943-2945`).

- **Callers.** It runs once per step of every successful `SELECT`
  (`src/stats/access_stats.cpp:25`, `:44`, `:66`; reached from
  `src/server/command_dispatcher.cpp:6144`, `:6241`, `:6673`, `:6688`,
  `:6703`). Writes reach it only through foreign-key checks (`:3262`).
- **Default and logging.** It is on by default (`expeditor.hpp:285`), and
  unlogged (`catalog.cpp:2988`, `:3017`).
- **Its fetch is a write fetch**, so it also pays P1's fourth hold.
- **Readers:**
  - `SHOW ACCESS` (`command_dispatcher.cpp:1634`);
  - the relayout planner behind `SHOW RELAYOUT`
    (`src/stats/relayout_planner.cpp:189`, `:232`);
  - `CREATE CABIN`'s warning (`src/exec/cabin_ddl.cpp:67`).

**`OptimizerSignals`** takes its latch (`src/stats/optimizer_signals.cpp:70`)
on every fingerprinted `SELECT` (`command_dispatcher.cpp:6814`, `:6840`).

- It is armed above one core (`expeditor.cpp:1094-1096`), whatever
  `cabin_optimizer` says.
- A full table (4,096 ids) evicts by a linear scan under the latch
  (`optimizer_signals.cpp:47-53`).

**`CabinStore`** takes a global `stats_latch_` on every Cabin hit
(`src/stats/cabin_store.cpp:138`). It also takes the partition's mutex
(`:115`, `:134`, and once per entry served at `:95`). All of these are raw
`std::mutex`es, taken even at `cores = 1`.

**PostgreSQL** keeps pending statistics in the backend and flushes them no
more often than `PGSTAT_MIN_INTERVAL`, 1,000 ms
(`src/backend/utils/activity/pgstat.c:126`).

### 1.4 P4 — the relation borrow

**One relation is one partition.**

- The lock table has 64 × cores partitions
  (`include/kds/txn/lock_table.hpp:396`; `src/txn/lock_table.cpp:59`),
  keyed by a hash of `(rel_oid, unit, lo)` (`lock_table.cpp:94-97`).
- A relation's key is `{rel, kRelation, 0, 0}` (`lock_table.hpp:352-354`),
  so every borrow of one relation lands in the same partition.
- Each partition has a `Latch`, null at `cores = 1` (`lock_table.cpp:68`).
- The modes are `IS`, `IX`, `S` and `X` (`lock_table.cpp:44-56`). The only
  strong relation ask is DDL's `X` (`command_dispatcher.cpp:8013`).

**Statements take it often.**

- A `SELECT` asks `IS` at bind under a fresh holder
  (`command_dispatcher.cpp:6410`; `include/kds/server/read_borrow.hpp:97`),
  then releases and wakes at its end: three holds.
- `BorrowChain` asks `IX` again for every written row
  (`command_dispatcher.cpp:8104`).
- The coverage and conflict tests scan the partition's entries, and each
  entry's holders, linearly under the latch (`lock_table.cpp:174-219`).

**The decide releases borrow by borrow** (`lock_table.cpp:588`). That is
O(K²/P) for K borrows (`docs/inflight/known-gaps.md:493-496`: 14 to 49 ms at
K = 16,384).

- Each `ReleaseHeld` (`:529-567`) scans its partition and erases.
- `WakeWaiters` then takes the partition again and scans again (`:443-451`).
- AZ-Q4 accepted this as priced and struck AZ-S6's keyed partition
  (`workorder-az-ay-carry-forward.md` §3-§4).
- No latch spans the loop, so the cross-core cost is latch traffic, not one
  long hold.

**No fast path exists for weak modes.**

**PostgreSQL:**

- gives each backend fast-path slots: `FP_LOCK_SLOTS_PER_GROUP` (16) per
  group, `FastPathLockGroupsPerBackend` groups
  (`src/include/storage/proc.h:87`, `:98`);
- takes a weak lock to the shared table only when
  `FastPathStrongRelationLocks` counts a strong holder
  (`src/backend/storage/lmgr/lock.c:999`);
- keeps 16 partitions in that table (`LOG2_NUM_LOCK_PARTITIONS`,
  `lwlock.h:96`);
- stores row locks in the tuple header, which this engine cannot
  (invariant 12).

### 1.5 P5 — the pk mark

**One chain for every relation.**

- `AllocateRowId` finds the relation's row with `ForFirstRow<SysTableRow>`
  starting at page 7 (`catalog.cpp:2456-2457`; `kCatalogPageTables`,
  `include/kds/catalog/well_known.hpp:329`).
- It holds each page of the walk exclusive across the callback
  (`catalog.cpp:176`, `:190`).
- Every relation's 106-byte row (`include/kds/catalog/rows.hpp:150`) is in
  that one chain, so inserts into different relations meet on page 7.

**One WAL record per bump.** Each bump appends a record and stamps the page
under the hold (`OverwriteLogged`, `catalog.cpp:2484` → `:242`, `:244`):

- once per row on the per-row path (`command_dispatcher.cpp:5051`);
- once per statement for a sorted fill (`AllocateRowIdRange`, `:4766`).

A named key at or above the mark takes the same path
(`catalog.cpp:2593-2598`). Its `before_mark` hook takes two lock-table
partition latches while page 7 is held (`command_dispatcher.cpp:5032-5041`),
against `lock_table.hpp:136-139`, which requires *"no page latch held"*.

**Issue order is not placement order (defect A).**

- An id is issued under page 7 (`command_dispatcher.cpp:5051`) and placed
  later under its leaf (`:5079`).
- The spec states the heap half of this window, and says *"A btree relation
  has none, the descent placing each id where it sorts"*
  (`heap-and-tuple.md:250-258`). The descent picks the leaf where the id
  sorts, but the leaf appends at its slot count (`btree.cpp:930`;
  `heap_page.cpp:162-163`). The comment at `btree.cpp:943-945` assumes only
  a caller-supplied id can sort inside a leaf.
- While `key_order` is `kAscending`, `ORDER BY <pk>` is elided and the walk
  emits slot order (`src/exec/step_compiler.cpp:1863-1891`).
- So two cores can place 101 before 100 in one leaf, and that query returns
  them in that order. Under `LIMIT` it can return a different row set.
- The rules this sits under:
  - invariant 11 and `heap-and-tuple.md` §4.1a: one sequence in issue order,
    bumped under the page latch, no per-core block;
  - `docs/rules/keystoneid-invariant.md` K3 already counts *"bump-ahead
    recovery"* among the gaps it licenses (`:23-25`);
  - §2 there: the mark is *"persisted before the row is placed"*
    (`:130-133`), and every core issues while *"none caches"* (`:134-139`).
- **PostgreSQL:** `SEQ_LOG_VALS` is 32 (`src/backend/commands/sequence.c:58`).
  `nextval` writes one WAL record per 32 values, and a crash skips the
  unlogged remainder.

### 1.6 P6 — the WAL append

`WalStream::Append` takes the stream latch (`src/wal/stream.cpp:210`) and
holds it across:

- the segment-fit check and any seal and roll (`:212-219`);
- the ring-full check (`:221-227`);
- the LSN assignment, the encode, the payload `memcpy` and the CRC32C
  (`src/wal/record.cpp:101`, `:111`);
- the cursor bump (`stream.cpp:229-235`).

**I/O also runs under that latch.**

- **A full ring (1 MiB)** is drained by the appender with a `pwrite`
  (`src/wal/manager.cpp:317-324`).
- **A full segment (64 MiB)** is sealed, and then `CreateSegment` runs:
  `posix_fallocate` (`src/wal/file_log_device.cpp:260`), a 64 × 1 MiB zero
  prewrite and an `fsync` (`:105`, `:117`), and a directory `fsync`
  (`:287`).
- **Every `Flush` and `Sync` caller** holds the latch across its `pwrite`
  (`stream.cpp:239-263`). Only the `fdatasync` is outside it (`:278`).

**The page latch is the outer one**
(`include/kds/storage/device_page_store.hpp:147-151`). An insert appends
while it holds its leaf (P9), so a roll lands inside every page hold that is
waiting on the append.

**Why it is one latch.** AL-R1 chose a latch because a bare `fetch_add`
cannot express a roll, an oversized refusal or `OutOfSpace`. It also named
the follow-on: *"staging into a second buffer and writing it unlatched is
the follow-on if it shows"* (`workorder-al-m0-single-wal.md` AL-R1).
`wal.md` §6-1 records the reserve/copy/publish split as *"tried and
abandoned"*.

**PostgreSQL:**

- `ReserveXLogInsertLocation` reserves under `insertpos_lck`
  (`src/backend/access/transam/xlog.c:1111`, `:1134`);
- the copy happens under one of `NUM_XLOGINSERT_LOCKS` (8, `:151`);
- a flush waits only for in-progress copies below it
  (`WaitXLogInsertionsToFinish`, `:1507`).

### 1.7 P7 — commit durability

**`strict` (D1)** syncs inside `Commit` (`manager.cpp:357-369`), and `Commit`
is called from inside the synchronous statement body:
`txn/manager.cpp:320`, called from `EndWrite` (`command_dispatcher.cpp:8522`).

**`group` (D2)** stages the commit, and the statement then parks on
`IsDurable(lsn)` (`command_dispatcher.cpp:743-745`). The post-task hook
drains once per reactor pass (`src/server/core_runtime.cpp:474-494`;
`src/sched/scheduler.cpp:429`). There is no batching delay;
`wal_drain_interval_us` (1,000 µs) is a backstop.

**Core 0 syncs on its reactor**, by a recorded decision
(`manager.cpp:158-168`): a hand-off *"doubled `group`'s p99"* on a 2-core
host. AL-S8 later found that a peer's commit tail, which does hand off, was
*"indistinguishable from core 0's at both p50 and p99"* on this 8-CPU host
(`workorder-al-m0-single-wal.md`, AL-S8's row). Core 0's D1 calls `Sync()` on
every commit, so each D1 commit pays its own `fdatasync`, whatever else is
in flight.

**A peer blocks or spins.**

- **A peer's D1** flushes, then waits on the writer's condition variable
  (`manager.cpp:190-198`; `writer.cpp:34-38`). Its whole reactor blocks.
- **A peer's D2** asks the writer for a sync (`manager.cpp:427`), and its
  parked statement polls:
  - the drain hook reports work for as long as a commit is pending
    (`core_runtime.cpp:481-487`), so the scheduler never blocks
    (`scheduler.cpp:193`);
  - every pass takes the stream latch and the writer's mutex again
    (`manager.cpp:249`; `writer.cpp:25`) until the sync lands.
- **The writer coalesces** (`writer.cpp:15-19`, `:72`), so the peers' syncs
  are already shared. Core 0's inline ones are not.

**A `strict` commit caps every core's snapshot across its sync (defect B).**

- A commit sets its snapshot marker before its append
  (`src/txn/manager.cpp:309-318`) and lifts it after its publish
  (`:351-361`).
- Meanwhile `SnapshotCeiling()` caps every core's new snapshot below that
  marker (`instance_visibility.cpp:93-115`).
- For D2 the span is the append. For D1 it includes the `fdatasync`.
- So for the length of every D1 sync, no new snapshot on any core covers a
  later commit, even one already acknowledged to its client.
  `txn.md:92` says a `READ COMMITTED` statement *"sees everything committed
  before it began"*.

**Every sync `fdatasync`s every segment ever written**
(`file_log_device.cpp:386-405`), and the log is never recycled
(`docs/inflight/bugs/wal-segment-descriptors-exhaust-the-open-file-limit.md`).
So a commit's sync costs more the older the log. The loop over every
segment is a correctness dependency, not an oversight: *"Do not 'optimise'
it to the tail"* (`:366-377`).

**Defect C:** a peer's writeback runs core 0's inline sync on the peer's
thread, against `manager.hpp:29-31`. BA-S1 fixed it and deleted its entry,
which is at
`git show e7617b2:docs/inflight/bugs/a-peers-writeback-runs-core-0s-wal-sync-on-the-peers-thread.md`.

**PostgreSQL:** `XLogFlush` takes `WALWriteLock` by `LWLockAcquireOrWait`
(`xlog.c:2854`). A backend whose record another backend's flush covered
returns without a sync of its own, and only that backend waits.

### 1.8 P8 — the assertion directory

- **Every write commit takes the latch.** `CommitTxn` and `AbortTxn` take
  the directory latch unconditionally (`src/exec/assertion_check.cpp:703`,
  `:766`). Their callers:
  - every autocommit write's end (`command_dispatcher.cpp:8424-8425`,
    `:8519`);
  - `COMMIT` (`:7702`) and `ROLLBACK` (`:7781`);
  - a failed statement's abort (`:8550`).

  The latch exists above one core (`expeditor.cpp:1021`).
- **One assertion anywhere makes every row pay.** The per-row checks skip
  the latch only while no assertion exists in the instance
  (`include/kds/exec/assertion_check.hpp:404`). Once one does, all of these
  take it for every relation's rows:
  - `AdmitInsert` (`assertion_check.cpp:430-431`);
  - `ReserveInsert` (`:535-538`);
  - `AnyOn` (`:97-98`);
  - `CannotEnforce` (`:103-104`).
- **Reservations enter in one place.** `pending_` is written only by
  `ReserveOne` (`:517`), which inserts and deletes both reach
  (`ReserveDelete`, `:675`).

### 1.9 P9 — the rightmost leaf

**The insert holds its leaf throughout.**

- The descent takes the leaf exclusive (`src/storage/btree/btree.cpp:190-191`)
  and hands it back held, with any parents a split secured (`:935`,
  `:957-962`).
- Under that hold the insert runs:
  - the Cabin witness, index maintenance, the assertion reservation and the
    undo append (`command_dispatcher.cpp:5101-5180`);
  - then the `kHeapInsert` append (`:4257`), and only then the release
    (`:4264`).
- AT-S21 put the append under the hold on purpose
  (`records-appended-after-their-page-is-released.md`). Nothing records
  whether the steps before it need the hold.

**A stale descent is refused, not retried.**

- A descent restarts at most `kMaxDescentRestarts` (4) times
  (`include/kds/storage/insert_placement.hpp:67`; the loop at
  `btree.cpp:163`, the refusal at `:232`).
- A stale parent path is re-descended a bounded number of times, then
  refused (`:499`, `:524`).
- A root another core grew over is refused with no retry at all (`:549`).
- The index tree has the same three refusals
  (`src/storage/index/index_tree.cpp:195`, `:254`, `:279`).
- Each reaches the client as a retryable `TxnConflict`
  (`command_dispatcher.cpp:5088`; `src/wire/error_registry.cpp:89`), and
  nothing in the engine retries it.

**PostgreSQL:**

- `_bt_moveright` (`src/backend/access/nbtree/nbtsearch.c:246`) moves right
  past a concurrent split instead of failing;
- the rightmost-leaf fast path caches the target block once the tree is
  `BTREE_FASTPATH_MIN_LEVEL` (2) deep (`nbtinsert.c:30`, `:1426`).

### 1.10 P10 — no yield inside a statement

- **A statement's body runs synchronously.** `DispatchAsync` runs it to the
  end (`command_dispatcher.cpp:694`, and the comment at `:685-687`). The
  executor also runs to completion (`src/exec/step_vm.cpp:2821`), and it
  refuses any park beneath it (`:168-172`).
- **The one suspension point is unused.** The bottom of the page loop is
  *"the executor's one legal suspension point"*, and nothing has suspended
  there since AT-S10 (`step_vm.cpp:1926-1933`). `sched::Yield` is used
  nowhere in `src/`.
- **So a walk holds its core** until it ends or hits `max_rows_touched`
  (100M by default, `include/kds/exec/budget.hpp:56`). Every session, every
  timer and the WAL hook on that core wait. That breaks `sched.md:42`
  (*"Cooperative yielding is mandatory"*).
- **Cancel.** `C_CANCEL` has no handler, as `protocol.md:131` says.
- **Scheduling groups do not help.** They arbitrate between groups, not
  sessions. Every client statement is `kForeground`
  (`src/server/tcp_server.cpp:585`), served first-in-first-out
  (`scheduler.cpp:264-286`).
- **What a yield would have to survive:**
  - The dispatcher keeps per-statement state in members (`may_park_`,
    `pending_commit_lsn_`, `blocking_writer_`, `statement_trail_mark_`;
    `command_dispatcher.hpp:2044`, `:2193`, `:2208`, `:2223`).
  - Each statement's first act is `catalog_.Revalidate()`
    (`command_dispatcher.cpp:755`). That frees every `const TableAccess*` a
    running statement holds whenever the schema word has moved
    (`include/kds/catalog/catalog.hpp:231-236`).

### 1.11 P11 — session placement

- **The kernel decides.** Above one core every core binds with
  `SO_REUSEPORT` (`tcp_server.cpp:90`; `expeditor.cpp:1470`;
  `core_runtime.cpp:585`), and the kernel picks one.
  `force_listener_handoff` (`expeditor.cpp:1466-1468`) takes the fallback
  instead, which hands connections out round-robin
  (`src/server/connection_handoff.cpp:20`).
- **A session never moves** after `AdoptConnection`
  (`tcp_server.cpp:368-370`). There is no `SO_ATTACH_REUSEPORT_*BPF` and no
  `SO_INCOMING_CPU`.
- **Inferred, not measured.** On a loopback listener only the client's
  ephemeral port varies, so the choice is effectively random. Eight sessions
  on eight cores occupy about 5.25 cores on average, and all eight are
  distinct about 0.24 % of the time.
- **The tree already works around it.** `tools/multicore_benchmark.py` reads
  each session's `core=` from `SHOW META` and opens connections until every
  core has its share.

### 1.12 P12 — periodic stalls on a reactor

**The transaction-id carve.**

- Ids come in blocks of `kTrxIdBlockSize`, 4,096
  (`include/kds/txn/trx_id.hpp:74`).
- A carve takes the superblock latch twice (`src/txn/trx_id.cpp:26`;
  `expeditor.cpp:1159`), and persists before it issues (`trx_id.cpp:41-49`).
- The persist is `PersistTrxIdCeiling`, which is `store_->Sync()`
  (`expeditor.cpp:1171-1174`): every dirty page of the instance's pool is
  written behind the WAL gate, and then the data file is synced.
- All of this runs inline in `Begin` on the carving core
  (`src/txn/manager.cpp:155`). An idle core burns blocks too
  (`MaybeBurnIdleBlock`, `manager.cpp:611-630`).
- Blocks are not aligned, because recovery raises the ceiling to the highest
  id it saw plus one (`expeditor.cpp:948-963`).

**The checkpoint.**

- It runs every `checkpoint_interval_ms` (5,000) per core, staggered
  (`core_runtime.cpp:526-529`).
- The checkpointer has a slicing `Step()`, `pages_per_step` at a time
  (`src/wal/checkpointer.cpp:205-216`). But `RunToCompletion` (`:335`) drives
  it to the end in one call, which breaks `checkpointer.hpp:32-36`'s
  *"spreads across reactor iterations"*.
- It holds the assertion registry's latch across its snapshot appends
  (`checkpointer.hpp:152-157`).

**Nothing in the tree attributes the ~0.4 s stalls** that AT-S13's cell 2
and AZ-S7's cell 3 recorded. AZ-S7's sits in its `fk` arm only, on both of
its A/B builds (`results-az-s7-overhead-v2.7.0-568-g9f170b1.md:344-349`),
which argues against a periodic cause like these two. BA-S2's counters are
what would settle it.

**PostgreSQL:** `GetNewTransactionId` (`varsup.c:77`) takes `XidGenLock`
(`:105`) and extends the commit log (`:204`), and it syncs no data file. The
checkpointer is a process of its own.

### 1.13 Where the tree's own text disagrees with the code

Besides those stated in place (§1.5, §1.7, §1.10, §1.12), these are each
corrected by the stage named:

- `docs/spec/eviction.md:93` says *"per-core map, no lock"* (BA-S8).
- `include/kds/storage/device_page_store.hpp:159-173` describes a durability
  wait on the fault path that the inline sweep never reaches (BA-S8). EV8's
  missing protocol is already recorded (`known-gaps.md:15-29`).
- `src/exec/step_vm.cpp:2066-2072` says a visible writer *"pays nothing at
  all"*. Above the floor it pays `window_latch_` (BA-S9).
- These two cite `manager.cpp` lines that have moved (BA-S7):
  - `command_dispatcher.cpp:1279` cites `manager.cpp:110` for the sync
    count, which is at `:225`;
  - `:1313-1314` cites `:250`/`:256` for `interval_syncs`, which is at
    `:461`.
- `include/kds/wal/stream.hpp:27` calls the stream latch *"outermost"*. It
  is outermost only against the device's own lock (BA-S12).
- Not this order's, recorded so it is not lost: `src/txn/manager.cpp:325-329`
  says a failed autocommit commit leaks its transaction, but `EndWrite`
  aborts it now (`command_dispatcher.cpp:8522-8524`).

### 1.14 Re-read at `d43845a0`, for the marks

§1 was read at `d3d90b5`. Before §4 was marked, every item was re-read at
`c47fbeff`, by reading and not running; the citations below are
`d43845a0`'s, which adds BH (`PURGE`) on top. Where it differs, this
section wins over §0 and §1.1-§1.13 until a stage restates them:

- **P1.** One `frames_latch_` over one `unordered_map` still
  (`device_page_store.hpp:1658`, `:1828`), pins plain under it. BE rewrote
  the sweep (one slot at a time, bounded reclaim batches inline and on core
  0's 50 ms tick) and made `buffer_pool_frames` required, so §1.1's sorted
  copy and its "default 0" are gone. **New:** BE-Q11's no-refuse window
  takes the latch twice more per inserted row (`device_page_store.cpp:879`,
  `:903`). BF-S2's `FreePage`, which replaced `EvictClean`, is one of the
  table's three erasers, which an atomic pin must exclude. BG's scan queue
  (BG-S2, not built) is designed under the same latch.
- **P2, P3, P4, P8** hold as stated; line numbers moved. AZ-S5's
  `ReleaseIf` gives back only a failed foreign-key check's borrows; the
  decide's release is unchanged.
- **P5.** Defect A is closed (BB, then BD). On a btree the issue still
  holds page 7 but no leaf since BD-R6, with BD's bounded re-draw. The
  `before_mark` hook is gone. **New:** every named key on a btree, below the
  mark too, takes page 7 with a write fetch (`AdmitExplicitRowId` ->
  `ForFirstRow`, `catalog.cpp:2600-2623`), logging only at or above the
  mark. BH gives K1 its one named exception: invariant 11 now reads *"never
  rebound except by `PURGE`"* (`heap-and-tuple.md:371`), the text BA-R8's
  change lands on.
- **P6.** Wider: a roll under the stream latch now syncs every live segment
  through the sync gate (`stream.cpp:70-75`, `:92-95`), behind `sync_mutex_`, so it can
  wait out the writer's `fdatasync`. `CreateSegment` adds a temp-name build,
  a `renameat2` and a directory fsync. A ring still full after 4 drains
  fail-stops the log (BC-S4).
- **P7.** Defect C closed (BA-S1); defect B closed for the session's own
  commit (BA-S1c), the other-session remainder open. BC recycles segments
  below the durable redo start, so a sync covers live segments only.
- **P9.** Holds. BD-R6 moved issue, borrow and encode before the descent;
  BD-R3 E5's take-back needs the leaf held for every failure after
  placement, a new reason the post-placement steps stay under it.
- **P10.** Holds. **New:** BF-R9's statement epoch keeps one slot per core
  because the executor never parks (`statement_epoch.hpp:22-31`); a yield
  breaks that. BH's `PURGE` waits by polling between whole re-runs
  (`command_dispatcher.cpp:731-760`), never inside a walk.
- **P11.** Holds. Migration at a transaction boundary is
  `docs/pending/session-load-balancing.md`'s.
- **P12.** Attributed since §1.12 was written:
  `docs/inflight/bugs/a-checkpoint-run-holds-its-cores-reactor-for-its-whole-length.md`
  (`c0e0868a`) measured stalls up to 5.7 s ending at the core's own
  checkpoint anchor. `workorder-bi-checkpoint-off-the-reactor.md` (written,
  not opened) asks in BI-Q1 (a) to take the checkpoint half; BA keeps the
  carve.
- **§1.0** holds, with the sync gate's `sync_mutex_` added as a blocking
  wait. Pinning is `PinToCore` (`expeditor.cpp:1246-1256`): peer k on CPU k,
  core 0 unpinned. The reactor-k-on-CPU-2k map does not exist yet: BB-S5's
  pinned `cores = 2` cell ran core 1 on CPU 1, core 0's SMT sibling.
- **The driver.** `tools/bb_concurrent_benchmark.py` already has
  multi-process clients, the client-bound mark, per-session `core=`, and
  one relation or one per client, but no PostgreSQL twin; BA-S3 starts
  from it.

## 2. Rulings — CLA's proposals, marked in §4

**BA-R0 — measure first, with an instrument that does not saturate first.**

*The counters (BA-S2).*

- **What is counted:**
  - For every `Latch` row of `rules.md` §3 (the frame table, the free map,
    the window, the lock partitions, the stream, the assertion directory,
    the optimizer collector, the superblock) and for `CabinStore`'s raw
    mutexes: the acquisitions that found the latch held, and the nanoseconds
    spent waiting.
  - The page latch's spin turns, which `Acquire` returns and the store throws
    away (`device_page_store.cpp:2245`, `:2277`).
  - Syncs by thread: core 0 inline, or the writer.
  - Drain passes spent with a commit pending (§1.7's spin).
  - Time new snapshots spent capped below a held marker (defect B).
  - Carves and the longest one; checkpoint runs and the longest one.
  - Refusals by site, at the `TxnConflict` sites of §1.9 - seven at
    `d43845a0`, BtreeLookup's bounded re-descent being the seventh: the
    btree's descend, parent, secure and lookup, and the index tree's descend,
    parent and secure.
- **How it is counted.** Try the lock first, and count and time only the
  contended branch. Counters are per core and summed when read, so the
  instrument adds no shared cache line. `SHOW META` prints them.
- **At `cores = 1`.** Nothing new is armed. The new `SHOW META` fields are
  the one visible change, and any golden that pins `SHOW META` moves with
  them.

*The driver (BA-S3).* It is a tools stage, landing before any measurement,
as `bench/README.md` requires.

- **Clients:** N client processes, never threads (AT-S13's ceiling was one
  process). Each is pinned to CPUs disjoint from the server's, and each
  one's CPU is recorded.
- **Shapes:**
  - a pk point read;
  - scenario 0's trade;
  - a monotonic-pk insert into one relation and into N relations, which
    separates page 7 from the leaf;
  - a read of rows written within the last few hundred commits, which
    reaches P2.

  Each runs at `relaxed`, `group` and `strict`.
- **The PostgreSQL twin** runs the same shapes at the same client counts,
  with `synchronous_commit` on and off. Off is the like-for-like for
  `relaxed`, which the summary did not measure (§7).
- **Client-bound cells.** A cell whose clients run above 80 % CPU is marked
  client-bound. It reads as a floor, never as the engine's ceiling.

*The host (BA-Q2).*

- 8 logical CPUs on 4 physical cores; the SMT siblings are 0/1, 2/3, 4/5 and
  6/7 (`lscpu -e`, 2026-10-02).
- The engine pins peer reactor k to CPU k and leaves core 0 unpinned (§1.0).
  So at `cores = 2` both reactors would share a physical core, and a 1→2→4
  curve would measure SMT siblings rather than cores.
- The census therefore needs reactor k on CPU 2k. BA-S2 adds that map, by
  re-scoping an existing setting if one expresses it, and otherwise as a new
  one.
- With the reactors on CPUs 0 and 2 and the clients on 4–7, `cores = 2` is
  the largest clean cell on this host. `cores = 4` (CPUs 0, 2, 4 and 6)
  shares physical cores with the clients, and every results file says so.
- PostgreSQL's postmaster gets the same CPUs as the reactors.

*The census (BA-S4).*

- **The matrix:** sessions {1, 2, 4, 8, 16} × `cores` {1, 2, 4} × the
  shapes, on a release build, under every rule in `bench/README.md`.
- **Output:** a scaling curve per shape for KDS and for PostgreSQL, and each
  item's contended wait as a share of reactor wall time (`sched_wall_us`).
- **Materiality, which BA-Q1 asks the operator to mark:**
  - an item is material when its share reaches 5 % in any cell, or when it
    refused anything;
  - P11 is material when a balanced arm (connections opened until every core
    has its share, `multicore_benchmark.py`'s method) beats the kernel's
    placement by more than the cell's spread;
  - P12 is material when any carve or checkpoint holds a reactor longer than
    10 ms.

**BA-R1 — a peer's writeback stops running core 0's inline sync (defect C).**

- **The fix.** The store's WAL gate answers from the calling core's manager.
  The store already knows `CurrentCore()`, so it keeps one gate per core
  rather than one for the instance. A peer's `EnsureDurable` then waits on
  the writer, as its commits do, and `manager.hpp:29-31` is true as written.
- **The four peer paths.** All four reach the gate today:
  - the checkpoint flush (`src/wal/checkpointer.cpp:214`);
  - the anchor publish (`src/server/superblock_checkpoint_anchor.cpp:126`);
  - the carve (`expeditor.cpp:1171-1174`, wired at `:1752-1754`);
  - a client `SYNC` on a peer session (`command_dispatcher.cpp:1215`).

**BA-R1b — the leaf keeps issue order, or the relation stops claiming it
(defect A).** BA-Q14 is the operator's choice between:

- **(a)** A placement that lands below its leaf's highest live id flips the
  relation's `key_order` to `kUnordered`, once, as a below-mark named key
  does. It is small and sound, and it costs a relation that ever raced its
  `ORDER BY <pk>` elision for good.
- **(b)** The id is issued under the hold of the leaf it lands on. An
  omitted pk always lands in the rightmost leaf, so the descent targets that
  leaf, holds it, then issues. This keeps the elision, but adds a new latch
  order, leaf before page 7, which the stage must prove nothing inverts.

A heap relation, which can only predate SUS-1, has no `kUnordered`. For it,
(a) becomes a per-page key-order emission whenever its walk is asked for pk
order.

**BA-R1c — a snapshot never misses a commit its session was acknowledged
(defect B).**

- **The bound.** A session records the commit LSN of its last acknowledged
  commit.
- **The wait.** A statement whose session's bound sits above
  `SnapshotCeiling()` parks at the statement boundary, where a park is
  legal, until the markers below the bound lift. The wait is at most one
  sync, and it happens only in the race.
- **Why the wait cannot be skipped.** Minting at the bound without waiting
  would cover an unpublished commit, which is the AN-Q3 anomaly the marker
  exists to prevent.
- **What it fixes and does not.** It closes the session's own case. A
  commit by another session that was acknowledged earlier stays invisible
  for one sync. That is the remaining gap against `txn.md:92`, which BA-Q3
  (c) would close.

**BA-R2 — statistics fold per core (P3).**

- **Collect.** `RecordAccess`, the collector's note and `CabinStore`'s hit
  statistics accumulate into a per-core pending table. Its core is its only
  writer, so it needs no latch.
- **Fold.** Each core folds its pending table into the existing shared
  structures (page 11, the collector, the Cabin statistics):
  - once per cadence;
  - before it answers `SHOW ACCESS`, `SHOW RELAYOUT` or `CREATE CABIN`'s
    warning.
- **The controller** reads the collector as it does today.
- **What changes.** `crosscore.md` CC13's *"a peer's count is in the row
  before its statement returns"* is struck; a count reaches the row within
  one cadence.
- **What does not.**
  - The relation stays unlogged (CR6), so a crash loses at most one cadence
    of statistics: invariant 8's class.
  - `SysAccessStatRow` gains no `core_id`.
- **The cadence** is 100 ms as a constant, unless an existing setting
  expresses it, in which case that setting is re-scoped (BA-Q4).

**BA-R3 — only a transaction that reserved touches the assertion directory
(P8).**

- **Commit and abort.** `CommitTxn` and `AbortTxn` return before the latch
  when the transaction holds no pending reservation.
- **The flag.** `ReserveOne` sets a per-transaction flag under the latch it
  already holds, so inserts and deletes both set it. The test itself needs no
  latch.
- **Per-row checks** skip the latch for a relation no assertion covers, by
  reading a per-relation count with acquire.
- **Why that is safe.** `CREATE ASSERTION` raises the count under the
  relation's `X`, which waits for every writer (AT-S5e). A writer asks `IX`
  before admission (`command_dispatcher.cpp:4605-4617`), so a writer that
  read 0 finishes before the build takes its base.

**BA-R4 — commit syncs leave every reactor (P7).**

*Part 1: `group` (D2), no semantic change.*

- Core 0's D2 syncs go to the writer, as a peer's do.
- The writer kicks a parked statement once its watermark passes the
  statement's LSN: write-then-kick through the wake registry
  (`sched/waker_table.hpp`, AU-R2).
- The polling pass goes, and the drain hook stops reporting a pending
  commit as work, so an idle reactor blocks.

*Part 2: `strict` (D1), on BA-Q3's mark.* D1 syncs today inside the
synchronous body, before its publish (§1.7), so it cannot simply park. The
options:

- **(a)** Publish, then park. D1 then becomes visible before it is durable,
  as D2 already is.
- **(b)** Park before the publish. The commit's second half (publish, marker
  lift, borrow release) runs after the park. That keeps the marker across the
  park, as it is across the inline sync today, so BA-S1c lands first.
- **(c)** Snapshots carry their in-flight set, as PostgreSQL's do, so no
  commit holds a marker across its sync. That is a `txn.md` §4 redesign and
  its own order.

Under (a) or (b), D1 shares whatever sync concurrent requests ride.

*What stays blocking.* `EnsureDurable` (WAL-before-data) and `SyncAll` keep
their blocking wait, because their callers hold pages (§1.0).

*Fewer segments per sync.* A sync covers the segments with unsynced bytes,
not every segment ever written, but only if it answers `:366-377`'s two
reasons:

- A segment is unsynced from any write to it, recovery's rewrite and a late
  full-page image included, until a sync that began after that write.
- A roll between a sync's watermark capture and its device sync leaves the
  previous segment in the set.

*The premise is re-measured first.* `manager.cpp:158-168`'s argument came
from a 2-core host. The census re-measures the hand-off's p50 and p99, under
`group` and `strict`, before anything changes.

**BA-R5 — the frame table, partitioned (P1).**

- **The split.** The map splits into 128 partitions by a hash of the page
  id, each with its own `Latch`, null at `cores = 1` as today.
- **Atomic counters.** Pins and the dirty generation become atomic per
  frame.
- **The sweep** visits partitions one at a time. It evicts a frame only
  after a compare-and-swap of its pin count from 0 to a sentinel, so a
  concurrent fetch either pins first or misses. EV4 (a pinned frame is never
  a victim) then holds by that compare-and-swap rather than by the latch.
- **`IsAllocated` leaves the hit path**, because no clear bit has a
  resident frame: the one free primitive erases a page's frame in the hold
  that clears its bit (`page.md` §5, BF-R5; the reason this bullet gave,
  "nothing frees a page", BF made false). It stays on the miss and create
  paths.
- **What stays global** (BA-Q5 as marked): BE-R1's free list and CLOCK
  hand, BE-Q11's `window_promised_` and BG-R1's scan queue, under one
  remaining latch. BG-S2 lands before BA-S8.
- **This re-opens AM-S2's decision** that pin accounting lives under the
  structure latch (`device_page_store.hpp:801-805`): BA-Q5.

**BA-R6 — the visibility window, read without the latch (P2).**

- **The shape.** The window becomes an open-addressed array of atomics
  indexed by trx id, sized to the range above the floor. A lookup is atomic
  loads.
- **The fallback.** An id beyond the array's range, such as one held by a
  long transaction that pins the floor, falls back to today's latched lookup.
- **The reclaim order.** The reclaim raises the floor before it erases, with
  release. A lookup's miss is decided by a floor read after the lookup, with
  acquire. That is the "second read decides" rule `read_view.hpp:114-121`
  already states, and it keeps AN-R12: a reader is wholly before or wholly
  after a pass (`instance_visibility.cpp:276-279`).
- **The commit side** may keep its latch among commits. What leaves is the
  read side's latch, on every core count.
- **The alternative** is per-core commit tables, the in-flight tables'
  shape. That needs a block-to-core range table, because blocks are not
  aligned (§1.12), and per-core reclaim. BA-Q6.

**BA-R7 — the relation borrow (P4).**

1. **No second ask (always).** A transaction that already holds a mode
   covering `IX` does not ask again per row. `LockHoldings::Holds` answers
   at any mode (`lock_table.hpp:533-541`), and a foreign-key parent check
   puts `IS` into the same holdings (`command_dispatcher.cpp:8100`). So the
   test is "covers `IX`", not "holds".
2. **A fast path for `IS`/`IX`, only if the census measures the partition
   material after (1).**
   - Weak relation borrows go in a per-core table.
   - A strong count per relation lives in an array hashed by relation. An
     `S` or `X` asker raises it, then moves every core's fast-path entries
     for that relation into the partition.
   - The deadlock detector sees moved entries only, which is enough because
     a fast-path holder never waits on a weak mode.
   - `txn.md` §5 is restated.
3. **The decide's release.** It takes AZ-R6's keyed partition only if the
   census shows the release's latch traffic material. Otherwise AZ-Q4
   stands.

BA-Q7 marks all three.

**BA-R8 — the pk comes from an instance cursor, and the mark is logged
ahead (P5).**

- **The cursor.** Each relation's issue cursor is an atomic in an
  instance-wide table: a new `rules.md` §3 row, spec first. Issue is a
  `fetch_add` while the cursor is below the persisted ceiling.
- **The ceiling.** The issuer that reaches the ceiling raises it by
  `kRowIdLogAhead` (32, PostgreSQL's `SEQ_LOG_VALS`), under page 7 and the WAL
  append, before any id at or above the old ceiling is placed. So K1 holds
  across a crash, which burns at most 32 ids per relation: K3's *"bump-ahead
  recovery"*.
- **Named keys.** A named key at or above the cursor raises the cursor by
  compare-and-swap, and raises the ceiling with it if needed. The heap
  relation's below-mark refusal compares against the cursor, not the
  ceiling. A btree named key below the cursor takes no page 7 (BA-Q8).
- **`next_id`.** On disk it becomes the ceiling for an omitted key, as it
  already is for a named one. `DESCRIBE` and `SHOW BUDGET` read the cursor.
- **Placement order.** The cursor issues faster, and it must not widen
  defect A. BD-R2's sorted placement holds under it, and BA-S11's cells
  test placement order, not only issue order.
- **Text changes.**
  - Invariant 11's `next_id` sentence (`heap-and-tuple.md:371`, *"a
    high-water mark on what has been placed"*) becomes the persisted
    ceiling above one instance cursor every core issues from.
  - `keystoneid-invariant.md` §2's *"Every core issues, and none caches"*
    (`:180`) is restated with it.

  Both are the operator's (BA-Q8).

**BA-R9 — the WAL append's I/O moves out from under the latch (P6).**

*The first half,* whenever P6 is material:

- **The segment is prepared ahead.** The writer preallocates and prewrites
  the next segment under its temporary name ahead of need. A roll under the
  latch still syncs through the sync gate before it switches (BC,
  `stream.cpp:92-95`), so that sync stays under it (BA-Q9).
- **The flush writes outside the latch.** The flush stages into a second
  buffer; the latch swaps the buffers, and the `pwrite` runs outside it. A
  flush sequence keeps the durable watermark from passing a gap.
- **Less work under the latch.** The CRC's LSN-independent part is computed
  before the latch is taken.

*The second half* is the reserve/copy/publish split that AL-R1 abandoned
(BA-Q9). After the first half, nothing rolls under a reservation, and an
oversized record is refused before it reserves. What remains is a full ring,
which waits for space rather than unwinding; that wait replaces BC-S4's
ring-full fail-stop rather than sitting beside it (BA-Q9). Whether to build it is decided
from the census's stream-latch numbers after the first half.

**BA-R10 — the periodic stalls shrink (P12).**

*The carve persists the superblock page alone*, and stops calling
`store_->Sync()` on the whole pool:

- **Page 0's write goes through `WriteBack`'s claim** (AT-S10e).
- **The data-file `fdatasync` is unconditional.** `FlushPages` skips its sync
  when another core's writeback carried the page
  (`device_page_store.cpp:1800-1818`). A carve that skipped would return with
  its ceiling unsynced and could reissue ids after a crash.
- **The anchor's order still holds.** Every page a checkpoint vouches for is
  written before the anchor is raised in memory, and `CHECKPOINT_END` is
  durable before the publish (`checkpointer.cpp:269-278`). There is one data
  file, so the `fdatasync` covers every write issued before it.

*The carve also runs early.* The next block is carved when the current one is
three-quarters used, on the system group, so `Begin` waits only when a block
runs dry. Both still run on a reactor; what shrinks is the stall.

*The checkpoint runs through its own `Step()`*, `pages_per_step` at a time,
one per reactor iteration, instead of `RunToCompletion`. The anchor is
published after the last step. This paragraph is BI's if BI-Q1 is marked
(a) (BA-Q1).

**BA-R11 — the rightmost leaf (P9).**

- **A survey first, inside the stage.** For each step under the leaf's hold
  (§1.9), it records whether that step needs the hold, and why. AT-S21
  already argued the append. A step that can run before the descent moves
  there.
- **A structural refusal becomes a wait, at the statement level.** It joins
  the dispatcher's existing wait-and-re-run
  (`command_dispatcher.cpp:268-277`): a statement refused before it wrote
  anything parks its coroutine, which yields the reactor, and re-runs from
  the top, under the statement's one deadline, the fault net
  (`lock_wait_fault_net_ms`, 1 s).
  - **Why not inside the descent.** A statement cannot yield there (P10), so
    a retry loop under the statement would hold its core for up to the fault
    net.
  - **Why a re-run reaches a root grown over.** The re-run re-reads the root,
    because the growth moves the schema word.
- **Who it reaches:** a single-row insert with no spilled value.
  - The re-run happens only while the transaction's trail is unchanged
    (`command_dispatcher.cpp:8436-8439`).
  - A spilled value is noted before the descent (`:5066-5079`), and so is
    every earlier row of a multi-row `INSERT`. Those keep today's refusal,
    because statement-level rollback is out of scope (`txn.md` §9).
  - Scenario 0's refused trade is a single-row insert.
- **Not proposed: B-link move-right.** It needs a high key per node, a node
  format change, and BD-R3 E5's take-back still needs the leaf held after
  placement. The superblock bump it brings is routine under the standing
  order of 2026-10-07 and is not the cost. BA-Q10.

**BA-R12 — a statement yields at its walk boundary (P10).**

- **The mechanism.** `DispatchAsync` awaits the step chain instead of
  running it to completion. The walk suspends at the bottom of the page loop
  (no pin, no span) every 64 pages, and checks a cancel flag there.
  `C_CANCEL` gains a handler.
- **What the stage must prove:**
  - **No catalog memo is dropped while a statement on this core is
    suspended.** `Revalidate` frees what a suspended statement holds
    (§1.10), and every btree root growth anywhere moves the schema word
    (`catalog.cpp:2682`).
  - **No dropped relation's pages are freed under a suspended statement.**
    BF-R9's statement epoch becomes the minimum over a core's suspended
    statements, not one slot per running statement (§1.14, BA-Q11).
  - **Every per-statement `CommandDispatcher` member becomes
    statement-local** (§1.10).
  - **A walk resumed after a split neither misses nor repeats a row.** Other
    cores can already split between two of a walk's pages, so the stage
    first reads how the walk finds its next page today.
- **What stays held.** The borrow and the read view stay held across a
  suspension; both are statement-scoped and hold no page.
- **The chain runs inside the synchronous `HandleSelect`**
  (`command_dispatcher.cpp:6410-6412`), so this stage is L.

BA-Q11.

**BA-R13 — placement by load (P11), only if material.**

- **The change.** `force_listener_handoff`'s arm (`expeditor.cpp:1466-1468`)
  picks the core with the fewest live sessions instead of round-robin, and
  becomes the default above one core. That setting is re-scoped rather than
  a second one added. `SO_REUSEPORT` stays as the other mode.
- **The cost:** one hop per connection, not per statement. A session still
  never moves after adoption.
- **Not proposed:** migration, or a kernel BPF selector. BA-Q12.

## 3. Stages

Since 2026-10-09 no stage waits for its own word (`raft-marks-2026-10-09.md`
§2). BA-Q1 says which wait for the
census.

| stage | what | exit | size |
|---|---|---|---|
| BA-S0 | This order, its index row and the three bug entries | the files at the commit | S |
| BA-S1 | **Defect C, the peer-thread sync** (BA-R1) | **Red first**, on the two-core rig (`workorder-av-two-core-rig.md`): an owning-thread assertion in `WalManager::Sync()` fails today. **Green**: all four peer paths wait on the writer. **Mutation**: the per-core gate replaced by core 0's, killed. The bug entry deleted | S |
| BA-S1b | **Defect A, the leaf's order** (BA-R1b, on BA-Q14). **Superseded by BB** (`workorder-bb-issue-under-the-leaf.md`), BA-Q14 being marked (b) | **Red first**: on the two-core rig, core A is paused between issue and placement while core B issues and places. `ORDER BY <pk>` then returns 101 before 100 today. **Green**: the chosen fix; the `LIMIT` shape of the same cell. `heap-and-tuple.md:250-258` and `btree.cpp:943-945` restated. The bug entry deleted | M |
| BA-S1c | **Defect B, the snapshot that misses its own commit** (BA-R1c) | **Red first**: on the two-core rig, core 0's `strict` sync is paused while core 1's session commits `relaxed`, is acknowledged and reads its row. The row is missing today. **Green**: the read waits and finds it. **Mutation**: the bound ignored, killed. The bug entry restated to the other-session remainder, or deleted under BA-Q3 (c) | M |
| BA-S2 | **The counters** (BA-R0) | each counter proved by a cell on the two-core rig that forces its contention. `cores = 1` unchanged in behaviour. The reactor-to-CPU map. The new `SHOW META` fields in `manual/` | M |
| BA-S3 | **The driver and its PostgreSQL twin** (BA-R0), a tools stage | `--help` documents the shapes, the pinning and the client-bound mark. A dry run at `cores = 1`, with no number claimed | M |
| BA-S4 | **The census** (BA-R0) | `bench/v3.0.0/results-ba-s4-census-<describe>.md` with: scaling curves per shape for KDS and PostgreSQL; each item's share and its material or immaterial reading; BA-R4's premise (the hand-off's p50 and p99) | M |
| BA-S5 | **Statistics fold per core** (BA-R2) | `SHOW ACCESS` on the same core shows its own statement. A two-core cell shows the other core's count after one cadence. The page-11 and collector waits at 0 in the census shape. CC13 restated | S |
| BA-S6 | **The assertion directory** (BA-R3) | a commit that reserved nothing takes no latch (counter at 0). A delete-only transaction settles its departures. A relation without assertions writes beside one with them. The AT-S5e cells green. **Mutation**: the flag ignored, killed | S |
| BA-S7 | **Commit durability** (BA-R4), part 1, and part 2 on BA-Q3 | the premise re-measured first. The spin counter at 0. **Mutation**: the kick removed, with the cell asserting the latency bound, not just liveness. Under part 2, two `strict` commits staged in one pass take one `fdatasync`. A sync of an old log covers its unsynced segments, with a roll between capture and sync covered. `wal.md` §1 and §3 restated | M |
| BA-S8 | **The frame table** (BA-R5) | the store's suites green, and `cores = 1` byte-identical. A sweep racing a fetch of the same page, forced by a barrier and repeated. **Mutation**: the compare-and-swap made a plain store, killed. `page.md` §6 and `eviction.md` restated | L |
| BA-S9 | **The visibility window** (BA-R6) | AN's visibility cells green. A two-core cell interleaving a reclaim with a lookup of a winner reclaimed in that pass answers visible. **Mutation**: the reclaim's floor-first order swapped, killed | M |
| BA-S10 | **The relation borrow** (BA-R7) | for (1): an N-row insert takes the partition once, not N times. For (2), if marked: the fast-path and strong-ask race forced and repeated, and a DDL `X` waiting on a fast-path `IX` held on another core. The lock suites green | M, or L with (2) |
| BA-S11 | **The pk cursor** (BA-R8) | the Keystone suites green. A sim crash between a ceiling raise and the placement reissues no id and burns at most 32. Two cores issuing into one relation: one sequence, and every leaf in key order. An issue that draws an id a named key inside [cursor, ceiling) placed first is re-drawn, never placed. A btree named key below the cursor takes no page 7 (BA-Q8). **Mutation**: the ceiling logged after the placement, killed | M |
| BA-S12 | **The WAL append** (BA-R9), first half | sim crash cells across a prepared roll and across a buffer swap. The stream latch's wait in the census shape. The second half only on BA-Q9's mark | M |
| BA-S13 | **The periodic stalls** (BA-R10; the checkpoint half BI's if BI-Q1 is marked (a), BA-Q1) | the stall counters under 10 ms in the census shape. A crash between a carve's persist and its first issue reissues no trx id, with another core's writeback racing the page-0 write. A crash mid-steps replays from the previous anchor | M |
| BA-S14 | **The rightmost leaf** (BA-R11) | the survey's table, with a reason per step. Scenario 0's `c8-s` cell with no refusal. A forced root grown over is re-run and succeeds. **Mutation**: the re-run removed, killed | M |
| BA-S15 | **The walk-boundary yield** (BA-R12) | `sched.md:42` true. A point read sharing a core with a long walk is bounded by one slice. A root growth on another core during a suspension frees no memo the walk holds. A split between two slices misses and repeats no row. Cancel cells | L |
| BA-S16 | **Placement by load** (BA-R13) | 8 connections at `cores = 4` land 2 per core. The census's per-core session counts | S |
| BA-S17 | **BA's close** | a row per stage. The census re-run at the closing commit, against BA-S4's. The overhead A/B at `cores = 1` over the whole change (`raft-marks-2026-09-30.md` §8). What BA carries | S |

## 4. Items for the operator

| # | item | class | CLA proposal | mark (2026-10-09, `raft-marks-2026-10-09.md` §1) |
|---|---|---|---|---|
| BA-Q0 | **The letter and the scope**: one letter; twelve items and three defects; and the not-planned list of §0 | scope | Yes. BA is the next free letter: AQ and AR are held for AR1 (`raft-marks-2026-10-02.md` §4) | **as proposed**; defects A and C and B's own-session case are closed (§1.14) |
| BA-Q1 | **The premise gate**: no fix stage before the census, and only for an item BA-R0's materiality test marks. BA-S1, BA-S1b, BA-S1c (defects) and BA-S15 (a spec promise the code breaks) are exempt | process | Yes. It is `CLAUDE.md`'s *"re-measure a premise before building the fix"*, applied per item | **as restated**: BA-S1b is struck (BB, BD); if BI-Q1 is marked (a), BI-S1's gate replaces the census for P12's checkpoint half and BA-S13 keeps the carve |
| BA-Q2 | **The host and method**: this host, reactor k on CPU 2k and the clients on CPUs 4–7, so `cores = 2` is the largest clean cell; multi-process clients with the client-bound mark. Or a second host for the clients | method | This host, with its limit stated in every results file | **as proposed**; the reactor-k-on-CPU-2k map is BA-S2's to build (§1.14) |
| BA-Q3 | **`strict`'s shape** (BA-R4 part 2, BA-R1c): (a) publish then park, visible before durable like D2; (b) park before the publish, the marker held across the park, with BA-S1c first - and the synchronous `Dispatch`'s BA-R1c wait made a park before it, or it spins on its own core's marker (BA-S1c's review, C2); (c) snapshots carry their in-flight set, as their own order | user-visible | (b) now: it keeps D1's meaning and takes D1 off the reactor. (c) as a later order if the census finds the marker's cap material | **(b)**, (c) a later order if the census finds the marker's cap material |
| BA-Q4 | **Statistics freshness**: another core's counts arrive up to one cadence (100 ms) late, and CC13's sentence is struck (BA-R2) | user-visible | Yes | **as proposed** |
| BA-Q5 | **AM-S2's pin decision re-opened**: a partitioned frame table with atomic pins (BA-R5) | architecture | Yes | **as restated**: the hit path (lookup and pin) partitioned over BE's slot array, 128 partitions, pins and the dirty generation atomic; BE-R1's free list and hand, BE-Q11's `window_promised_` and BG-R1's scan queue stay under one remaining global latch; `IsAllocated` leaves the hit path on BF-R5's invariant; BG-S2 lands before BA-S8 |
| BA-Q6 | **The window's read side**: (a) per-core commit tables with a block-to-core range table, or (b) an open-addressed atomic array with the latched fallback (BA-R6) | architecture | (b): it needs no block table and no per-core reclaim | **(b)** |
| BA-Q7 | **The relation borrow**: (1) always; (2) the fast path only if material after (1); (3) AZ-Q4 re-opened only if the decide's release is material (BA-R7) | architecture | As stated | **as proposed** |
| BA-Q8 | **The pk cursor**: the text of invariant 11 and of `keystoneid-invariant.md` §2, the meaning of `next_id`, and `kRowIdLogAhead` = 32 (BA-R8) | invariant | Yes. K3 already licenses the burned ids | **as restated, on BD-R6**: issue a `fetch_add` under no page hold with BD's bounded re-draw; the ceiling raised under page 7 and the WAL append; invariant 11's `next_id` sentence (`heap-and-tuple.md` §4.1) and `keystoneid-invariant.md` §2 restated, `next_id` the persisted ceiling, `kRowIdLogAhead` = 32; **and a btree named key below the cursor takes no page 7** |
| BA-Q9 | **The WAL split** that AL-R1 abandoned (BA-R9's second half) | architecture | Not now. Decide on the first half's numbers | **as proposed: not now**; the first half restated to BC: a roll still syncs the left segment through the sync gate, so only preallocation and the prewrite under the temporary name move ahead, and a ring-full wait replaces BC-S4's fail-stop rather than sitting beside it |
| BA-Q10 | **Structural refusals re-run at the statement level, with no move-right** (BA-R11) | user-visible | Yes | **as proposed**; the cost cited against a B-link tree is the node format and BD-R3 E5's take-back under the hold, not a superblock bump, which is routine under the standing order of 2026-10-07 |
| BA-Q11 | **A yield every 64 pages, and a `C_CANCEL` handler** (BA-R12) | user-visible | Yes, with 64 re-measured in the stage | **as proposed, with** BF-R9's statement epoch made the minimum over a core's suspended statements |
| BA-Q12 | **Placement**: the least-loaded hand-off as the default above one core, only if material (BA-R13) | user-visible | As stated | **as proposed**; migration at a transaction boundary is not decided here and stays with `docs/pending/session-load-balancing.md` |
| BA-Q13 | **Waits stay blocking**: no suspending latch primitive (§1.0); every fix holds less, less often | architecture | Yes | **as proposed**; §1.0's list restated with `sync_mutex_` (§1.14); BA-S1c's and BI-R2's parks wait where nothing is held and are not a suspending latch |
| BA-Q14 | **Defect A's fix**: (a) a placement below the leaf's highest id flips `key_order`; (b) issue under the leaf's hold (BA-R1b) | user-visible | (a) now: small and sound. (b) with BA-S11, if the census asks for the elision back. **Marked (b) by the operator, 2026-10-06** (`raft-marks-2026-10-06.md` §4), as its own sub-milestone BB | (b), 2026-10-06 (above) |

## 5. Sequencing

1. **The defects first**, independently of everything else and of each
   other: BA-S1 and BA-S1c, both built; BA-S1b superseded by BB.
2. **Then BA-S2, BA-S3 and BA-S4, in that order.** BA-Q1 says what waits for
   BA-S4.
3. **Then the fix stages, in the order the census ranks them.** CLA's prior,
   by expected share and isolation, is: S5, S6, S7, S8, S9, S10, S11, S12,
   S13, S14, S16.
4. **File overlaps force these orders:**
   - S1 before S7: both touch `wal/manager.cpp` and the store's gate.
   - S1c before S7's part 2.
   - S7 before S12: `stream.cpp` and `manager.cpp`.
   - BG-S2 before S8 (BA-Q5).
   - S8 before S13: both touch the store's writeback paths.
   - S11 before S14: both touch the insert path in `command_dispatcher.cpp`.
   - S15 after S10: the read borrow's scope across a suspension.
5. **BA-S17 last.** The overhead is measured once over the whole change,
   from the commit BA opened at, `d43845a0` (`raft-marks-2026-10-09.md` §2),
   to the commit that closes it. BA-S1's and BA-S1c's code is outside it.

## 6. Row status

### BA-S0 — written 2026-10-02

On `worktree-parallelism-workorder`, from `d3d90b5`, this stage wrote:

- this file;
- its index row;
- the bug entry for defect C.

§1 was read against `d3d90b5`, not run, and the PostgreSQL citations were
read from the 18.6 release tarball. No code, spec or test changed. The
letter belongs to BA-Q0.

**The review** (`critics-developer`, on `264cb30`) found the survey's
citations right within a line or two except those below. Its findings,
most severe first:

- **Two engine defects, both now entries.**
  - Defect A: an issued id placed out of slot order in a btree leaf, under
    an elided `ORDER BY <pk>`.
  - Defect B: a `strict` commit's marker capping snapshots across its sync.

  CLA verified both by reading at `d3d90b5`, and each now has a fix stage
  (BA-S1b, BA-S1c) and an operator item (BA-Q14, BA-Q3).
- **Five rulings had hazards, all applied:**
  - BA-R12 ignored that `Revalidate` frees a suspended statement's catalog
    memo, and the dispatcher's per-statement members.
  - BA-R6's floor-before-and-after test missed a winner erased before the
    floor rose. The reclaim now raises the floor first.
  - BA-R4 had D1 "park like D2", which would publish before durable. D1 is
    now BA-Q3's three options.
  - BA-R3's flag sat in `ReserveInsert` and would strand a delete's
    reservation. It is now in `ReserveOne`.
  - BA-R11's root re-descent could not reach a grown root from a stale memo.
    That bullet is struck, and the statement-level re-run covers it.
- **Accuracy fixes, all applied.**
  - Defect C has four peer paths, not two.
  - Core 0's reactor is not pinned, and SMT siblings share CPUs 0/1, so
    BA-R0's host plan now maps reactor k to CPU 2k.
  - The sync-every-segment loop is a correctness dependency.
  - There is no `SIX` mode.
  - The refusal sites are six.
  - Five citations moved.
  - The AZ-S7 stall is a weak match, now said so.
  - "The largest" became "the most frequent", since nothing is measured.
- **Its trims, taken in part.**
  - Taken:
    - the premise gate now lives in BA-Q1, with pointers elsewhere;
    - §1.7's defect bullets became one sentence;
    - §1.13 no longer repeats what §1.5, §1.7, §1.10 and §1.12 state;
    - BA-R2 lost its second tier;
    - BA-Q6's proposal moved to (b);
    - BA-R13 re-scopes `force_listener_handoff`.
  - Kept: BA-R0 keeps the materiality test itself, because BA-Q1 asks for a
    mark on a definition that has to live somewhere.

**Rejected: none.** The "only unsynced segments" item stays in BA-R4, with
the warning's two reasons (`file_log_device.cpp:366-377`) as its conditions,
which is what the finding asked.

### BA-S1 — built 2026-10-02

Started on the operator's word (`raft-marks-2026-10-02.md` §6) and built on
`worktree-ba-s1-peer-writeback-gate` from `0c3268f`. Defect C is fixed: each
core's writebacks ask that core's own WAL gate.

**Red first, at `597abf6`.** The new file `peer_writeback_gate_rig_test.cpp`
has eight two-core rig cells. Seven covered the peer paths, and on
`0c3268f` all seven failed for the same reason: core 0's owning manager
synced and flushed for the peer, the stream's own watermark moved, and the
writer was never asked. The peer paths are:

- a writeback as core 1, once on the rig thread and once on core 1's own
  reactor;
- the checkpoint flush;
- the anchor publish;
- the trx-id carve;
- a client `SYNC` on a peer session;
- the shutdown checkpoint, a fifth peer path the bug entry had not listed.

The eighth cell checks that core 0's own writeback still syncs inline, and
it was green there.

**The fix, at `858ddc0`.**

- **The store** keeps the default gate (`SetWalGate`) and one atomic slot
  per core id up to `kPageLatchMaxCoreId`. Each `WriteBack` picks the
  calling core's gate once (`GateForCaller`); the batch's call and every
  run's use that one.
- **`CoreRuntime::Open`** registers a peer's attached manager before the
  peer's first writeback (the completion checkpoint). `~CoreRuntime` takes it
  back before `wal_` is destroyed.
- **Core 0 keeps the default**: its owning manager, which syncs inline.
- **`WalManager::Sync()`'s owning arm** aborts in debug builds when it runs as
  a core other than its own.
- **Specs:** `wal.md` §8-1, `page.md` §8, and a new `rules.md` §3 row for the
  slots.

**The checkpointer cells, at `a8e68cd1`.** The first full suite failed 11
`CheckpointerTest` cells on the new check. Their fixture drives an owning
manager for core 3 from a thread running as core 0. The fixture now runs as
core 3, and the two cells that use other managers declare their own cores.

**The review**, of `858ddc0` by `critics-developer` plus three skeptics
(concurrency and lifetime, durability, tests). It confirmed:

- the slot lifetime on every teardown path;
- that the attached manager's gate is as strong as the owner's;
- that the gate is chosen once per writeback;
- that `cores = 1` and the simulator are unchanged.

**What the review changed:**

- **One owner per slot.** `SetCoreWalGate` now refuses a second gate for a
  live core id (`AlreadyExists`). `ClearCoreWalGate(core, gate)` clears only
  the owner's own gate. Without this, a second runtime for one core id could
  have cleared a live one's gate, and that core would have fallen back to
  core 0's inline sync: the defect again, silently.
- **The check is tested.** A death cell (`WalManagerTest.AnOwningSync…`)
  covers it; before, deleting the check passed every cell.
- **Boundary cells.** One cell for `kPageLatchMaxCoreId` itself, and one for
  a null gate.
- **Rig waits** raised to 30 s. The rig's log is a real file, and a disk
  stall under `-j8` must not fail a cell.
- **Text.**
  - The rig test's present tense became past tense.
  - A message that claimed an ordering it could not check was corrected.
  - The abort's justification no longer cites a path that never reaches it.
  - The stale "parked request", "a wait on the writer thread" and "no gate
    installed" texts were corrected.
- **Duplicates cut.** About 15 lines that repeated the rationale were
  removed. Its homes are `device_page_store.hpp` and `wal.md`.
- **The bug entry deleted**, and the citations to it now point at
  `e7617b2`.

**Rejected:**

- **Narrowing the check to shared streams.** The rule stays one rule with no
  exception, and the core-3 fixture now states its core.
- **Wrapping the rig's device in `ProbedDevice`.** The write-after-durable
  order belongs to `WriteBack`, which BA-S1 did not change, and the eviction
  cells pin it.

**Mutation: seven mutants, all killed.**

- core forced to 0;
- peer registration deleted;
- core 0 registering too;
- run gate on the default;
- slot never taken back (a segfault on the destroyed manager);
- the abort removed;
- a slot set without the ownership check.

Each was restored from a byte copy, never from git. The first, second and
fourth were killed by the debug abort before any cell could fail. At
`597abf6`, before the check existed, the same shape failed the cells
themselves.

**Suite:** 3,123 / 3,123 on the final tree (`scripts/test.sh`, Debug,
269 s); 1 disabled test did not run.

**Overhead: not measured; it is measured at the milestone's close**
(BA-S17). At `cores = 1` the change adds one thread-local read and one
acquire load per `WriteBack`.

### BA-S1c — built 2026-10-06

Started on the operator's word (*"go ahead with BA-S1c"*, 2026-10-06) and built
on `worktree-ba-s1c-strict-marker-snapshot` from `dfabae1`. BA-R1c is
built: a session's statement never misses that session's own acknowledged
commit. **BA-Q3 is not marked by it**; the other-session remainder stays in
the bug entry, restated rather than deleted (the row's "or deleted under
BA-Q3 (c)" did not apply).

**Red first, at `2ab1916`.** The new file
`strict_marker_snapshot_rig_test.cpp` holds core 0's `strict` commit inside
its `fdatasync`, through a `GatedLogDevice` the rig now offers
(`gated_log_sync`). Core 1's session commits `relaxed`, is acknowledged,
and selects its own row. On `dfabae1`'s engine the `SELECT` answered no row,
10/10. The rig also needed core 1 warmed before the gate closes: its first
transaction carves an id window and persists page 0, whose writeback waits
on the log, and a held sync stalled it there instead of on the marker.

**The fix, at `fb94070`.**

- **The bound.** `Session::acknowledged_commit_lsn` takes the commit LSN on
  both commit arms (`CommitLocal`, `EndWrite`'s autocommit).
- **The wait.** `CommandDispatcher::UncoveredCommit` returns the bound while
  `SnapshotCeiling()` sits below it. `DispatchAsync` parks on it before its
  synchronous half; the synchronous `Dispatch` yields, as its group commit
  blocks.
- **The kick.** A waiter counts itself into its core's `ceiling_waiters`
  (`TransactionManager::CeilingWait`, scoped). `EndCommit` clears its
  marker, then kicks every core whose count is non-zero - `seq_cst` on both
  sides, the store-buffer shape. `Expeditor` installs the registry at
  `cores > 1`; the rig through its sim.
- **Specs:** `txn.md` §4.1 and its level table, `rules.md` §3's visibility
  row, `instance_visibility.hpp`'s new "acknowledged-commit bound" note.

**Cells:** three - an autocommit write, an explicit `COMMIT`, a
synchronous `SELECT` - each 20/20 green at `fb94070` and again at
`7470977`.

**The review**, of `fb94070` by `critics-developer`. No correctness defect.
It confirmed the handshake between waiter and lift, that the two commit
arms are every `TransactionManager::Commit` caller, that the wait holds
nothing, that the synchronous loop cannot deadlock today, and the frame and
visibility lifetimes. **Applied at `7470977`:**

- **C1.** `EnterCeilingWait`'s no-`NoteSlot` comment named the wrong
  guarantee; it is the manager's `PublishCoreBounds` at construction.
- **C2.** The synchronous loop's premise - no marker held across a park -
  ends with BA-Q3 (b). The loop now says it must become a park before (b)
  lands. **Carried to BA-Q3's mark.**
- **C3.** The cell's first barrier now requires core 0's marker set, not
  only a parked sync, and the cell asserts the ceiling below the session's
  bound before the `SELECT`.
- **C4.** The synchronous wait counts itself in too, so its cell takes the
  same barrier and a 100 ms sleep went.
- **C5.** `CeilingWait`'s ordering comment names `await_ready`; "one sync"
  now says a peer's can queue behind the writer's; `txn.md`'s level table
  carries the other-session exception.
- **Simplifications.** `UncoveredCommit` is the one lookup and consumes a
  covered bound - the ceiling is monotone - so only the first statement
  after a commit reads the ceiling; the setter is a plain store; the
  session's comment is a pointer.

**Rejected:** cutting the dispatcher's comment at the wait to a pointer
(simplification 3's other half). It is the one place a reader of
`DispatchAsync` learns why a statement parks before it has run.

**Mutation: four mutants, each run 20 times, all killed.**

- **the bound never recorded** - all three cells miss the row;
- **`DispatchAsync`'s wait skipped** - the autocommit and `COMMIT` cells
  miss it, the synchronous cell passes;
- **`Dispatch`'s wait skipped** - the synchronous cell misses it, the two
  parked cells pass;
- **the lift kicking nobody** - the two parked cells wait out core 1's 5 s
  idle block and fail the 1 s bound; the synchronous cell, which spins
  rather than sleeps, passes.

Run at `fb94070` and again at `7470977`, 20/20 each time.

Each was restored from a byte copy, never from git.

**Suite:** 3,126 / 3,126 at `fb94070` (`ctest -LE heap-suspended -j8`,
Debug, 115 s), and again on the merge with `origin/main` at `b448d5c`
(74 s), whose engine is `7470977`'s - the merge brought documents and
bench archives only. 1 disabled test did not run. Not re-run at
`7470977` alone.

**Overhead: not measured; it is measured at the milestone's close**
(BA-S17). At `cores = 1` a statement reads its session's bound and finds it
consumed; the first statement after a commit reads `SnapshotCeiling()` once,
three loads at one core. A commit adds one null test in `EndCommit`.

### BA paused for BB — 2026-10-06

BA-S1b was about to start on BA-Q14 (a): CLA had asked, and the operator
answered (a) (`raft-marks-2026-10-06.md` §4). The operator then asked how
wide (a)'s flag reaches, which is the whole relation, for good, and which
paths produce the misorder: one, CLA answered, and it fires at every
collision of concurrent omitted-pk inserts (BB's survey then found a
second, a named key at or above the mark, BB §1.3). On those answers the
operator re-marked
BA-Q14 **(b)** and paused BA for a sub-milestone,
`workorder-bb-issue-under-the-leaf.md`. Nothing of BA-S1b was built: its
worktree `ba-s1b-leaf-order-key-order` holds no change from `bddd450c`.

BA resumes once BB closes, with BA-S1b struck; its rows are rebased on BD (below).

### BA rebased on BD - 2026-10-07

BD-Q12 (b) (`raft-marks-2026-10-07.md` §17,
`workorder-bd-sorted-leaf-named-keys.md` §5): BB closes on the measurement it
has, without rebasing BA, and BD-S6 rebases these rows once, against BD.
BB's drafted rebase (its BB-S5 row) is dropped. BA stays paused until BB's
close entry is written, which waits for its own word.

- **BA-R8's last bullet** (*"The `before_mark` hook. Its two partition
  latches move out from under page 7"*): the hook is gone - since BB-R3 the
  borrow precedes the admission, and the latches with it. That stands.
- **BA-R8's named-key bullet**: BB's draft would have added *"a btree now
  refuses below the cursor too"*. **Withdrawn**: a btree places a named key
  below the mark where it sorts (BD-R2, BD-R5), and its admission is
  advance-or-nothing (BD-R7). The heap's below-mark refusal stands, and
  compares against the cursor, as BA-R8 says.
- **BA-R8's "Placement order" bullet**: a btree's leaf keeps its key order by
  placement (BD-R1), so the cursor can issue as fast as it likes without
  widening defect A on a btree; BA-S11's cells test the walk's order, which
  placement keeps. A heap's still needs its tail's hold (BB-R7).
- **BA-R11's "who it reaches"**: BA's original text stands. The encode
  precedes the descent again on a btree (BD-R6: issue, borrow, encode,
  descend, place), so a spilled value is noted before the descent, as BA
  first wrote; BB's draft, which had the spills follow the descent, is
  dropped. A structural refusal of a statement whose trail an earlier spill
  changed keeps today's refusal.
- **BA-S11's row**: it rests on BD-R6, not BB-R1. On a btree no leaf's
  hold covers the issue, so BA-R8's cursor runs outside any leaf's hold:
  below the ceiling an issue is a `fetch_add` plus BD's bounded re-draw
  (`kMaxIssueRounds`) when a named key took the id first; the ceiling's
  raise stays under page 7 and the WAL append. *"Every leaf in key order
  (or the relation `kUnordered`, per BA-S1b)"* reads *"every leaf in key
  order"*: `kUnordered` is deleted (BB-R10) and BA-S1b struck. *"A named
  key inside [cursor, ceiling) is never issued"* reads *"an id a named key
  inside [cursor, ceiling) placed is never placed again: an issue that
  draws it is re-drawn"*.

### BA resumed - 2026-10-08

BB's close entry is written (`workorder-bb-issue-under-the-leaf.md` §6,
"BB-S5 - the close", on `worktree-bb-s5-close`; `raft-marks-2026-10-08.md`
§6), so BA is no longer paused. Its rows stand as rebased on BD above. BB-S5
hands BA one item: C4 at `cores = 2` (four relations, pinned) leans against
BB on its medians and is unresolved at ten runs
(`bench/v3.0.0/results-bb-s5-overhead-v2.7.0-640-gb76261bb.md`). BA's census
takes the wait breakdown BB did not, on an engine where BD-R6 took the issue
back out from under the leaf. BA-S2 waits for its own word.

### BA opened - 2026-10-09

On `worktree-ba-open-marks` from `d43845a0`, on the operator's *"continue BA
from the work that was stopped; I want to finish BA"* and the item-by-item review that
followed (`raft-marks-2026-10-09.md` §1). Before marking, every item was
re-read at `c47fbeff`: §1.14 records what moved. §4 carries the marks; eight
items (Q1, Q5, Q8, Q9, Q10, Q11, Q12, Q13) were marked in wording restated
to the tree. BA-S2 is the next stage.

**Run to its close** (`raft-marks-2026-10-09.md` §2): the operator let
BA-S2..S17 run without a word per stage, a stage's question settled by CLA's
proposal and recorded as adopted under that go-ahead
(go-ahead-achieving-milestone), and set the close's A to `d43845a0`.

### BA-S2 - the counters, built 2026-10-09

On `worktree-ba-open-marks` from `7b3b9732`.

- **The instrument** (`include/kds/base/contention.hpp`). Each core has one
  cache-line-aligned block, summed when read. `kds::Latch` became a class
  over `std::mutex` that carries its `rules.md` §3 row as a `LatchKind`. Its
  `lock` tries first, and only a failed try reads the clock and records the
  wait. Every `Latch` row is tagged, plus `CabinStore`'s two raw mutexes, the
  WAL sync gate (`sync_mutex_`) and the free map's flush latch. The flush
  latch has a kind of its own because it is held across device I/O. An
  uncontended acquisition costs what it did before.
- **The tallies.**
  - Page-latch waits and their spin turns (`AcquirePageLatch`).
  - Syncs inline (the owning manager's arm) against syncs by the writer
    thread.
  - Drain passes taken with a group commit staged (`DrainOnce`).
  - BA-S1c's ceiling waits and their length (`CeilingWait`).
  - Carves and checkpoint runs, each with its longest (`StallTimer`). The
    checkpoint is counted on `RunGated`, the periodic path.
  - Refusals at seven `TxnConflict` descent sites, not the six BA-R0
    listed: `BtreeLookup`'s bounded re-descent is the seventh.
- **`SHOW META`** prints a `contention_*` block summed over every core
  (`client-manual.md`). It is the one visible change at `cores = 1`, where
  only the always-armed latches can move.
- **The host map.** The new key `reactor_cpus` pins reactor k, core 0
  included, to the k-th CPU listed. It must name exactly `cores` distinct
  CPUs that the machine has, or the server is refused at boot. No existing
  setting expressed CPU placement, so this is a new key and not a re-scope.
- **Cells** (`tests/contention_counters_test.cpp`, plus the BA-S1c rig
  cells).
  - **Forced deterministically** by holding the real latch from a second
    thread: every kind on a bare `Latch`, the frame table, the free map, the
    window, a lock partition, the wait-for latch, a carve on the superblock
    latch, and a page latch's spin.
  - **On the two-core rig:** the stream latch, the writer's syncs and the
    drain passes, from both cores inserting into one relation. Rounds are
    repeated until the counts move, bounded at ten. The ceiling waits come
    from BA-S1c's rig cells, which now assert the count.
  - **Unit cells:** an owning manager's inline sync, and a checkpoint run.
- **Not forced by any cell:**
  - the seven refusal sites one by one. No cell reaches a given site
    deterministically, and each count sits on its site's final return. Since
    BA-S3, the two-core insert cell proves their sum against the refusals
    its writers retried;
  - the assertion directory, optimizer, Cabin and handoff latches (each is
    tagged at its declaration, and the bare-`Latch` cell proves the
    counting for every kind).
- **Found on the way, a census fact:** ten rounds of two cores inserting
  400 rows each into one relation never contended the window latch or a
  lock partition.

**The review** (`critics-developer`): no code bug found.
- **Applied:**
  - the free map's flush latch split into its own kind (`free_map_flush`),
    so its I/O waits do not land on the bitmap latch;
  - the handoff inbox latch tagged (`handoff`);
  - `NsSince` replacing three copies of the duration conversion;
  - `static_assert`s on the three name tables;
  - `Latch::kind()` and `Contention::kSlots` deleted as unused;
  - `RunGated`'s temporary removed;
  - the latch cells' hold raised from 50 ms to 200 ms against a contender
    that is not scheduled in time under `ctest -j8`;
  - two documentation fixes, applied by the reviewer: the sync gate moves at
    `cores = 1` too, and `HoldWhile`'s comment.
- **Kept as is:** a ceiling lift that lands between `UncoveredCommit`'s
  check and the registration counts a wait of about 0 ns. It is still the
  event BA-R0 asks about.

Overhead not measured; measured at the milestone's close (BA-S17, A =
`d43845a0`).

### BA-S3 - the driver and its PostgreSQL twin, built 2026-10-09

On `worktree-ba-open-marks` from `2f8f7c42`. The tool is
`tools/ba_census.py`, one file for both engines.

- **Shapes:** `point`, `trade` (scenario 0's two `INSERT`s and two pk
  `UPDATE`s, each client trading among its own accounts), `insert1`,
  `insertN`, and `recent`. `recent` is an `INSERT`, then a pk read up to 300
  ids behind it, with its reads and writes reported as separate
  distributions.
- **Equal work on both engines.**
  - The pk: KDS omits it, PostgreSQL uses an identity column.
  - Durability: KDS `relaxed`, `group` and `strict`; PostgreSQL
    `synchronous_commit` `off` and `on`.
- **Pinning.**
  - KDS: `--server-cpus` is written as BA-S2's `reactor_cpus`.
  - PostgreSQL: the postmaster and every process it has already forked are
    re-pinned with `taskset -pc`. The cluster is the one the driver is
    connected to, read from its `data_directory`.
- **The client-bound mark:** a client that used more than 80 % of one CPU
  over the measured span.
- **What the JSON records.** For KDS, the delta of every numeric `SHOW
  META` field over the measured span. The `*_longest_us` fields are maxima
  and are recorded as read. Each run also records the server binary's
  sha256 and the data file's device.
- **bench/README.md's rules:**
  - every server runs from one copy of `--bin`;
  - ports have no default;
  - a KDS port that something already answers on is refused.
- **`--help`** documents the shapes, the pinning and the client-bound mark.
  It also says that `sched_*` describes only the control session's core, so
  BA-S4 divides an item's summed wait by `cores` times the span.
- **The dry run** (`--dry-run`, on the Debug build): every shape at
  `cores = 1`, one session, one second, on KDS and on PostgreSQL 18.6. Every
  cell ran with no error. No number is claimed.
- **PostgreSQL's cluster.** `~/pg-bench` would not start: `pg_ctl` reports
  its control file corrupt. Its `pg.json` names PostgreSQL 17.10 and the
  binaries are 18.6, which is the likely cause, not checked. It is not this
  milestone's and is left untouched. BA runs its own cluster under
  `~/pg-bench-ba` on port 15434, made by `tools/pg_setup.sh init` with
  defaults.

**The review** (`critics-developer`).
- **Five bugs, fixed by the reviewer:**
  - a `--pg-data` default that would have pinned the broken cluster; the
    data directory now comes from the server;
  - PostgreSQL's startup-forked processes were never pinned;
  - cleanup ran on the success path only;
  - a client could fail with a `NameError` when one unit outlasted the
    span;
  - a statement still refused after its retries was not counted as an
    error.
- **The reviewer also added:** the maxima reported as read, the binary's
  hash, the device, and the port check.
- **CLA applied:**
  - every server runs from a copy of the binary;
  - ports are required;
  - per-kind distributions;
  - setup rows loaded in batches of 200, not one sync each at `strict`;
  - one error classifier per backend, with SQLSTATE matched as a prefix;
  - `core()` deleted for `meta_all`;
  - the `"RECENT"` placeholder replaced by a holder the generator reads;
  - the silent port rewrite removed;
  - a dead assignment removed.
- **Kept as is:**
  - `accounts` has three payload columns against scenario 0's five; the
    same on both engines.
  - The `df -T` parsing now repeated in five tools; one helper in
    `bench_common.py` belongs to a tools clean-up, not this stage.

**Suite.** `IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence`
timed out at its 20 s bound in two of four full runs on this tree. CLA
reproduced it under load: 9 of 80 runs failed at `2f8f7c42`, and 10 of 72
at the base `d43845a0`, built for the comparison. So BA-S2 did not cause
it. BA-S2's own two-core insert cell expired under the same load, and its
progress print showed both writers part-way (52 to 400 of 400 rows after
20 s, with `strict` against a real log). The host was slow, not stopped.
The cell now commits `relaxed` and `group` only, with 200 rows and a 60 s
bound. It also retries a retryable `TXN_CONFLICT`, which the faster runs
then met: a stale descent and a root grown over (P9). It asserts that the
seven refusal sites counted at least as many as its writers retried, so
the refusal tallies are now proved on the rig where the race fires them.
The cell then passed 32 of 32 under the same load. The finding went to
`known-gaps.md`'s existing entry for the IdAllocation cell (Testing), not
to `bugs/`: no engine defect is shown.
Overhead not measured; measured at the milestone's close.

### BA-S4 - the census, run 2026-10-09

On `worktree-ba-open-marks` at `c390a624` (`v2.7.0-725-gc390a624`):
`bench/v3.0.0/results-ba-s4-census-v2.7.0-725-gc390a624.md`, with its
archive. 1,125 runs: KDS 675, PostgreSQL 18.6 450. No cell failed, and no
run had an error.

**Material, so these fix stages open** (BA-Q1):
- **BA-S7** - P7, commit durability.
  - On the write shapes (trade, insert1, insertN), `group` at
    `cores = 2` and 16 sessions runs at 1,656-3,558 statements/s, against
    4,918-4,977 at `cores = 1`. PostgreSQL `on` runs at 4,857-5,325.
    `recent` does not fall (6,033 to 6,325).
  - `strict` stays at 650-770 statements/s on the write shapes, whatever
    the session count.
  - The sync gate's waits reach 55 % of `cores` x span. The sum includes
    any the WAL writer thread took, since it counts into core 0's slot.
  - The snapshot ceiling (defect B) reaches 39 % at `strict`.
  - The drain took 2.2 billion passes with a commit staged.
- **BA-S12** - P6, the WAL append: 10.0 % in a clean cell.
- **BA-S13** - P12: carves up to 2.2 s and checkpoints up to 3.1 s,
  maxima over each server's life, setup and warm-up included. The carve
  half only (below).
- **BA-S14** - P9: 20 refusals.
- **BA-S15** - exempt from the census (BA-Q1).

**Not material, so these stages do not open:**
- **BA-S5 (P3), BA-S6 (P8), BA-S8 (P1), BA-S9 (P2) and BA-S10 (P4):** each
  item is below 1 % of `cores` x span in every clean cell, and below 2 %
  in every cell.

**Not shown material, because the census cannot measure them, so these
stages do not open (BA-Q1). Neither is shown immaterial:**
- **BA-S11 (P5):** page 7 has no latch of its own, and the page-latch
  counters do not name a page. `insert1` and `insertN` both pay page 7,
  so their gap prices the leaf, not page 7.
- **BA-S16 (P11):** no balanced arm was run. In the proxy, the runs with a
  session on every core were faster by over 10 % in 8 of 30 cells and
  slower in 3, with one or two runs per arm and no spread to test
  against. BA-S17's re-run reads it again once BA-S7 removes the
  confound.

**BA-R4's premise does not hold in this configuration, and the census
cannot say why.**
- `group` at `cores = 2`: core 0's sessions see a p50 of 11-43 ms and a
  p99 of 1.3-1.6 s, and complete 16-44 statements in 8 s against a peer's
  2,800-2,900. A peer's sessions see about 2.4 ms and 10 ms.
- The core-0 arm holds the inline sync, the waits on the sync gate behind
  the writer's peer syncs, and any carve or checkpoint core 0 ran. The
  counters split none of them. At `cores = 1`, with no writer syncing,
  `group` matches PostgreSQL. The arm the premise names, a hand-off from
  every core, was not run.
- `strict`: the two arms cost about the same, 14-18 ms at the p50. A peer
  blocks its reactor on the writer's condition variable.

**Adopted on the operator's standing go-ahead of 2026-10-09**
(go-ahead-achieving-milestone; `raft-marks-2026-10-09.md` §2). Each is
CLA's proposal; none is the operator's own mark.
- **The matrix's runs and length:** 3 runs per cell, 8 s each after 2 s of
  warm-up. BA-R0 named the axes, not these.
- **The load bound.** Each run waits for a 1-min load at or below 6, not
  1.5. The census's own runs held the 1-min average above 1.5 between
  cells, which would have made the census about 10 hours long. A
  competing build is still recorded per run, as rule 4 asks. The first
  54 runs (18 cells), taken under the 1.5 bound, were kept: the restart
  skipped complete cells.
- **The client-bound mark** is recomputed in the analysis. It marks a run
  in which any one client CPU was over 80 % busy with the clients pinned
  to it, which the driver's per-client mark missed. Marked: `point` at 8
  and 16 sessions at `cores` 2 and 4, `recent` `relaxed` at `cores = 4`
  16 sessions, and eight PostgreSQL cells on CPUs 0,2,4,6.
- **P5 and P11**, read as above. P5 cannot be separated from the leaf by
  the `insert1`/`insertN` pair. P11 is read through the kernel's own
  placement varying between runs, since the driver opens no balanced
  arm. Both are "not shown", not "immaterial".
- **BA-S13 takes the carve, and the checkpoint half waits for BI-Q1.** The
  operator gave the checkpoint's shape in BI (BI-R1 and BI-R2,
  `raft-marks-2026-10-08.md` §3-§4). Whether BA-R10's checkpoint
  paragraph moves to BI is BI-Q1, which is unmarked and is not this
  milestone's to mark. So BA does not build a second design for it. BA's
  close carries the checkpoint half as a stop: it waits for the operator's
  mark on BI-Q1.

No code changed in this stage. The suite is unchanged from `c390a624`.

### BA-S7 part 1 - `group` commits leave every reactor, built 2026-10-09

On `worktree-ba-open-marks` from `66bb11b2` (BA-R4 part 1, no semantic
change).

- **The writer kicks a parked committer.** `WalWriter` keeps a per-core
  count of statements parked on a durability point (`WalManager::DurableWait`,
  taken by the dispatcher's group-commit park). After every sync, and after
  a failed one, it kicks each core with a waiter through the wake registry.
  The waiter counts itself in, fences, and then reads the watermark; the
  writer moves the watermark, fences, and then reads the counts. So the
  writer always sees a waiter that missed the new watermark. The kick
  itself is best-effort, as every kick in the engine is: a skipped one
  costs one idle block, which is 10 ms in production.
- **Core 0's `group` sync goes to the writer** once the instance has a wake
  registry (`cores > 1`), as a peer's always did.
- **No drain reports a staged commit as work** once the writer kicks
  (`WalManager::StagedCommitIsWork`, one rule for core 0's drain and for a
  peer's). So the polling pass is gone: a reactor with nothing else to do
  blocks.
- **At `cores = 1` nothing changed.** `Expeditor` sets no registry there,
  so the drain syncs inline and still reports the staged commit as work.
- **Text:** `wal.md` §3, the `client-manual.md` `SHOW META` note,
  `manager.hpp`'s header, `Sync()`'s comment, and `scheduler.hpp`'s
  post-task hook.

**Cells** (`tests/durable_kick_rig_test.cpp`):
- A peer's five `group` commits, each answered inside 2 s against the
  rig's 5 s idle block, so each one was kicked. Every sync was the
  writer's.
- A reactor with its commit parked behind a held log gate takes fewer than
  100 drain passes in 300 ms, and is kicked at the release.
- An owning manager's `group` drain syncs on the writer when a registry is
  set, and inline when none is.

**Mutations**, each killed:
- the writer's kick removed (every kick cell);
- the peer's drain reporting the commit (the spin cell);
- the owning manager syncing inline (the owning cell).

A first harness run restored files with `copy2`, which brought back the
old mtimes, so make kept a mutant object and reported a spin that was not
in the source. The files were touched, rebuilt and re-run, and the three
results above are from that run.

`tests/row_wait_wake_rig_test.cpp`'s two release-kick cells now give the
waiter a `relaxed` session. The sim holds every kick, the writer's
included, so a `group` re-run would wait for a writer kick the cell never
delivers. The wake those cells test is the release's.

**The work order's "spin counter at 0" is read as "no pass reported as
work".** `contention_drain_passes_pending` still counts each drain taken
with a commit staged. A blocked reactor takes one on each wake, not one
per iteration.

**The review** (`critics-developer`): the kick protocol is correct.
- **Fixed by the reviewer:** a flake in the new cells. A single test kick
  delivering the start could be skipped, leaving the reactor in its 5 s
  block, so `go` is now set before `Start()`. 100 repeats then passed.
- **Applied by CLA:**
  - `WalManager::wake_` deleted (every caller sets the registry after
    `StartWriter`, now asserted);
  - the drain hooks' rule moved into `StagedCommitIsWork`;
  - the writer's slot index taken as the core id, since `CheckCoreCount`
    bounds it;
  - the three stale comments corrected;
  - the duplicate rig core-0 cell removed;
  - `wal.md` and the `SHOW META` note updated.
- **Rejected:**
  - Kicking without the sleeping check, which would close the
    skipped-kick window. "Best-effort, a skipped kick costing one idle
    block" is the engine-wide contract for every kick (`CLAUDE.md`'s
    cross-core row, the lock table's included). Changing it for one caller
    belongs to the wake registry's owner, not to BA-S7.
  - A wake for waiters when another core's write fail-stops the log. A
    parked peer learns of the stop at its next idle block (10 ms), and
    only the speed of an already-refused commit's reply is at stake.

Overhead not measured; measured at the milestone's close.

### BA-S7 part 2 - `strict` parks before its publish, built 2026-10-09

On `worktree-ba-open-marks` from `f1a59fcd` (BA-R4 part 2, BA-Q3 (b)).

- **The commit in two halves.** `TransactionManager::CommitDeferred` sets
  the marker and stages the commit record for the writer
  (`WalManager::StageStrictCommit`, staged as a `group` commit is and
  counted as strict). The transaction stays active, in flight and holding
  its borrows. `FinishDeferredCommit` is the rest of `Commit`: the publish,
  the marker's lift, the retire and the release, in the same order. Both
  share one body (`FinishCommitBody`).
- **The dispatcher defers** a `strict` commit when the statement may park,
  the writer kicks (`cores > 1`), and the transaction wrote no catalog row
  (`DefersCommit`). This covers both an explicit `COMMIT` and an autocommit
  statement. The statement parks on the record's durability like a `group`
  commit. After the park, `FinishDeferredCommits` publishes every durable
  entry in order. A statement that left an entry parks on it even if a
  re-run's outcome no longer names it.
- **Several markers per core.** A parked commit holds its marker, so a core
  keeps its open bounds in begin order and its slot publishes the lowest
  (`InstanceVisibility::BeginCommit` and `EndCommit`). The bounds are
  non-decreasing, so `SnapshotCeiling`'s argument holds unchanged.
- **The synchronous `Dispatch` (BA-S1c's review, C2).** Its BA-R1c wait can
  be capped by a commit parked on its own core, whose statement cannot run
  until `Dispatch` returns. The wait makes the last deferred record durable
  and publishes the core's deferred commits itself.
- **A failed sync aborts the parked commit** (D1, `txn.md` §6). The commit
  is never published. Its session's acknowledged bound is cleared, so its
  next statement does not wait for a ceiling that will never cover it.
- **A checkpoint does not list a parked commit as active.** Its record is
  below the checkpoint's begin, and the anchor is published only once the
  end record is durable.
- **At `cores = 1` nothing changed**: no registry, so no deferral.
- **New for a synchronous caller on the same core.** One that meets a row
  or relation a parked commit still holds is refused `TxnConflict` for up
  to one sync (`txn.md` §4).
- **Text:** `txn.md` §4 (when a commit becomes visible, and the BA-R1c
  paragraph), `wal.md` §3, and `instance_visibility.hpp`'s snapshot-ceiling
  note.

**Cells** (`tests/strict_park_rig_test.cpp`, plus one in
`instance_visibility_test.cpp`):
- A core serves its other sessions while a `strict` commit awaits its sync.
  The commit is invisible on every core until then, and a checkpoint
  snapshot taken meanwhile does not list it.
- An explicit `COMMIT` parks and publishes at its sync.
- The synchronous wait on its own core's parked marker finishes the commit.
- A parked commit whose sync fails is aborted, never visible, and its
  session goes on.
- Four `strict` commits staged during one held sync complete in at most two
  syncs.
- Several open markers on one core cap at the lowest, ended in reverse
  order.

**Mutations**, each killed:
- no deferral (the serving and batching cells);
- the synchronous wait not finishing its own core's commits (the cell
  hangs);
- an autocommit commit published at its stage (the serving and
  synchronous cells);
- publishing on a stopped log, and keeping the refused commit's bound (the
  failure cell; run by the reviewer);
- the newest bound published (the unit cell; run by the reviewer);
- the checkpoint guard removed (the serving cell; run by the reviewer).

**The review** (`critics-developer`).
- **Two bugs, fixed by the reviewer:**
  - B1: a parked commit was listed as active in a checkpoint taken during
    its park, so a crash could undo a durable, acknowledged commit;
  - B2: a stopped log published the parked commit beside its refusal,
    which broke D1.
- **Also by the reviewer:**
  - one `DeferCommit` helper over a plain `txn*` per entry;
  - the multi-marker unit cell;
  - the header note.
- **Applied by CLA:**
  - the batching cell;
  - parking on a statement's own deferred entry whatever its outcome says;
  - `FinishCommitBody` moved into the private section;
  - the `txn.md` and `wal.md` text;
  - the new synchronous-caller refusal window stated.
- **Checked, and nothing to change:** a shutdown with a parked commit does
  not touch a freed wake registry. `EndCommit` reaches the registry only
  while a ceiling waiter is counted, and the reactors are joined first.
- **Kept as is:**
  - the four copies of the cells' stop guard;
  - the "finished already" guard in `FinishDeferredCommit`, which is cheap
    and covers a second call.

Overhead not measured; measured at the milestone's close.

### BA-S12 - the WAL append's I/O out from under the latch, built 2026-10-09

On `worktree-ba-open-marks` from `91a14db0`. This is BA-R9's first half, as
restated in BA-Q9's mark.

- **The next segment's body is built ahead.** Once the current segment is
  half full, `WalStream::Append` asks the device for the next one
  (`LogDevice::PrepareSegment`, asynchronous). `FileLogDevice` builds it on
  a thread of its own, not the WAL writer's, so a 64 MiB zeroing never
  sits in front of a parked committer's sync. The thread reserves the
  segment, zeroes it and fsyncs it under the temporary name that `Open`
  never adopts. The roll then writes the header, `fdatasync`s it, renames
  and syncs the directory, so the header is still durable before the
  name. With nothing built ahead, the creation builds the body itself, as
  before. Both paths share one body builder (`BuildBody`).
  - BC's rule stands. The roll still syncs the segment it leaves through
    the sync gate before the next one exists, so preparation and zeroing
    are all that moved.
- **A flush writes outside the latch.** Under the latch it claims the
  flush, swaps the staged ring for a second buffer and records the range.
  The `pwrite` runs unlatched, so appenders keep staging across it.
  - Only one flush is in flight at a time.
  - A flush under the latch (the seal before a roll) and a detach each
    wait the one in flight out first. So records reach a segment in LSN
    order, and the device's segment table never changes beside a write.
  - `Sync` takes the unlatched flush.
  - `flushed_lsn_` is stored with release and read with acquire.
- **Not built:**
  - The CRC's LSN-independent part, BA-R9's third bullet. A record's
    payload is about 100 B, so there is nothing material to take out from
    under the latch. Adopted on the operator's standing go-ahead.
  - The ring-full wait, which is BA-R9's second half (BA-Q9: not now).
- **Text:** `wal.md` §4.1, §6-1 and §6-5, `stream.hpp`'s concurrency
  header, `file_log_device.hpp`'s concurrency note, and `latch.hpp`.

**Cells** (`tests/wal_append_off_latch_test.cpp`):
- Past half a segment the next one is built ahead and the roll adopts it.
  A reopened device scans through it.
- A body built ahead and never adopted is not part of the log, and the
  next creation replaces it.
- An append is not held behind a flush's write.
- A roll waits out a flush in flight, and the log stays in order.
- A detach waits out a flush in flight (added by the reviewer).

**Mutations**, each killed:
- the flush writing under the latch;
- a latched flush not waiting for the one in flight;
- no build ahead asked for;
- a creation ignoring the body built ahead;
- a detach not waiting (run by the reviewer).

**The review** (`critics-developer`).
- **Two races, fixed by the reviewer:**
  - a failed unlatched write released its claim before stopping the
    stream, so a flush claimed in between could publish a durable point
    past the hole. Now the stream stops first, and the claim is tested
    before `stopped()`;
  - a detach could change the segment table beside an unlatched write.
- **Also fixed by the reviewer:** a data race on the preparer's read of
  `end_segment()`.
- **Applied by CLA:**
  - the two body builders merged, with `Prewrite` losing its header half;
  - a dead reset deleted;
  - a misplaced comment moved;
  - release/acquire on `flushed_lsn_`;
  - the text the reviewer listed as owed.
- **Kept as is:**
  - The flush waiters yield-spin through another thread's `pwrite`,
    which is one ring's worth, at most 1 MiB.
  - A clean shutdown can leave one 64 MiB temporary body. The next
    creation removes it.
  - The device's destructor can wait out a body build.
  - The simulation harness's in-memory device builds nothing ahead, so
    the adoption's crash points are covered by the cells, not by the sim.
  - The shared tail of `Flush` and `FlushLocked`, about 6 lines.

Overhead not measured; measured at the milestone's close.

### BA-S13 - the carve's stall, built 2026-10-09 (the checkpoint half waits for BI-Q1)

On `worktree-ba-open-marks` from `7fa2bd96`.

- **A carve persists page 0 alone** (`DevicePageStore::PersistPage`, used
  by `Expeditor::PersistTrxIdCeiling` and mirrored by the rig). Page 0 is
  written back through `WriteBack`'s claim (AT-S10e), and then the data
  file is synced **unconditionally**: a writeback that carried the newer
  image is covered too, because the claim is released only after its
  write. Before this, a carve synced the whole pool, so it waited for
  every dirty page's writeback and every one of their log records. BA-S4
  measured carves of up to 2.2 s. Page 0 is unlogged, so the carve now
  waits on no log record.
- **The next block is carved ahead** (`TrxIdSequence::CarveAheadIfLow`).
  Once three-quarters of a core's window is issued, the `system`-group tick
  carves the next block, on core 0's tick and on each peer's. The `Begin`
  that drains the window installs that block without carving on its own
  path. A burn drops the block carved ahead, so this core's ids never move
  backwards.
- **The anchor publish is not disturbed.** Both write page 0 under the
  superblock latch with the page held exclusive, and the claims keep the
  device writes in order, so an older image never lands after a newer one.
- **The mount's raise still syncs everything** (`PersistSuperBlock`), once
  per mount, ahead of the completion checkpoint. Its comment is restated.
- **The checkpoint half is not built.** BA-R10's third paragraph waits
  for the operator's mark on BI-Q1 (the BA-S4 entry says why). It is
  carried to BA's close as a stop.
- **Text:** `wal.md` §11a and `txn.md`'s carve paragraph.

**Cells:**
- `peer_writeback_gate_rig_test`'s carve cell is rewritten. A peer's carve
  persists page 0 and nothing else: the stamped page stays dirty, its
  record undurable, and no log sync is asked.
- `trx_id_carve_crash_rig_test`: a crash image taken right after a carve
  holds a ceiling at or above the carved window. Even rounds carve alone.
  Odd rounds race a writeback that holds page 0's claim with an older image
  across the carve, forced by `SetAfterWritebackCopyForTest` (the
  reviewer's fix).
  - The image copies through the page cache, so it checks the write and
    its order against the claim. It cannot see the `fdatasync`.
- `trx_id_test`'s two early-carve cells: carved ahead at three-quarters and
  installed without a carve, and a burn that drops the block ahead.

**Mutations**, each killed:
- the carve syncing the whole pool;
- the carve writing page 0 nowhere;
- the claim skipped (`kSkip`, run by the reviewer);
- a burn keeping the block ahead;
- a drained window ignoring the block ahead.

**The review** (`critics-developer`): no bug in the production change.
- **The crash cell's race arm did not race.** Its writeback usually
  snapshotted the dirty pages before the carve's encode, so a carve that
  skipped a claimed frame passed. The reviewer forced the interleaving.
- **The early carve.** CLA first left it out, reasoning that a carve was
  now one page write and one `fdatasync`. The reviewer pointed out that
  BA-R10 states it as part of the stage, and that the data file's
  `fdatasync` flushes everything else unsynced in the file too. A carve
  right after a large checkpoint write could still take long. CLA built
  it.
- **Applied:** the `wal.md` and `txn.md` text, and the restated
  `PersistSuperBlock` comment.
- **Not applied:** deleting `PersistSuperBlock` in favour of
  `PersistTrxIdCeiling` at the mount. That would drop the mount's log and
  pool sync at that point, which is a behaviour change the stage does not
  need.
- **The early carve's own review:** no bug. Per-core monotonicity, the
  floor, durability and threading all hold.
  - **Applied:** the carve ahead runs only on a core that issued since the
    previous tick, which avoids a carve an idle core's burn would drop and
    one at boot; `ahead_` was moved off the sequence's hot offsets; and
    `txn.md` states that core 0's tick carves at `cores = 1` too and that
    a peer's tick needs `wal_drain_interval_us` above 0.
  - **Not added:** a two-sequence cell for a carve by another core between
    a carve ahead and its install. It is safe by construction, since every
    block starts at the superblock's high-water.
- **Not measured yet:** the "< 10 ms" exit. BA-S17's census re-run reads
  `carve_longest_us`.

Overhead not measured; measured at the milestone's close.

### BA-S14 - the rightmost leaf, built 2026-10-09

On `worktree-ba-open-marks` from `18f002e1` (BA-R11, BA-Q10).

**The survey: no step moves.** Under the leaf's hold, after placement, each
step falls in one of two kinds:
- **The records AT-S21 logs under the hold:** the WAL append
  (`LogInsert`), and the new root's publish after a split.
- **Steps whose failure BD-R3 E5 takes back with the leaf still held:**
  - the Cabin witness (a row no Cabin knows of must not outlive it);
  - index maintenance;
  - the assertion reservation (`ReserveInsert`, held until it is logged);
  - the spill and undo notes (`NoteSpills`, `NoteInsert`, whose failure
    E5's take-back answers).

Issue, borrow, encode and `AdmitInsert` already run before the descent
since BD-R6. Nothing left can run before it.

**A structural refusal becomes a re-run.**
- The four clustered-tree sites that give up a stale descent (descend,
  parent, secure, lookup) note the statement through a thread-local
  (`storage::NoteStructuralRefusal`). A caller that recovers from such a
  refusal clears the note.
- `DispatchAsync` takes a statement that wrote nothing before the refusal,
  yields one turn, and re-runs it from the top under the statement's one
  deadline, the fault net. The re-run re-reads the root, which a growth
  moved through the schema word.
- Inside `BEGIN`, `EndWrite` withholds the poison for such a refusal. The
  poison is applied only if the re-runs end in a refusal. A read's refusal
  never poisons, as before.
- A statement that wrote rows first, or whose spills were noted, keeps
  today's refusal (`txn.md` §9).
- The index tree's three sites keep reaching the client. Their refusal
  comes during index maintenance, after the row is placed, and a re-run
  would leave earlier indexes' entries behind (adopted on the go-ahead,
  from the review's residual).
- Reads are re-run too, since `BtreeLookup` is one of the four sites. That
  is wider than BA-R11's "single-row insert", for the same reason.
- `SHOW META` gains `contention_structural_reruns`.
- **Text:** `heap-and-tuple.md` §5, the `client-manual.md` `SHOW META`
  note, and `btree.cpp`'s comment.

**Cells** (`tests/structural_rerun_rig_test.cpp`): two cores insert into one
relation without a client retry, in autocommit and inside `BEGIN`, repeated
until the re-run counter moves. No reply is a refusal.

**Mutations**, each killed:
- the re-run disabled;
- the poison applied regardless (inside `BEGIN`, run by the reviewer).

**Not met in this stage:**
- **"A forced root grown over is re-run and succeeds."** The cells' refusals
  are `btree_descend`. Forcing `secure` deterministically needs a test seam
  between `DescendTo` and `SecureParents`, and it is not built. The re-run
  loop is the same for every site.
- **"Scenario 0's `c8-s` cell with no refusal"** is a measurement, read at
  BA-S17.

**The review** (`critics-developer`). Four bugs, fixed by the reviewer:
- B1: a re-run inside `BEGIN` always failed. `EndWrite` keyed on
  `kTxnConflict`, but write paths hand it an `InvalidArgument` verdict, so
  the transaction was poisoned.
- B2: a read's refusal could poison a transaction at the deadline.
- B3: three callers that recover from a structural refusal left the note
  set, turning a later unrelated conflict into a re-run.
- B4: a re-run kept the lock on each burned id until commit.

**Applied by CLA:**
- the index sites stop noting;
- a redundant condition removed;
- the text.

**Not applied:** one helper for the three re-run blocks in `DispatchAsync`.
Each differs in what it re-runs on, and one helper would need a mode for
each.

Overhead not measured; measured at the milestone's close.

### BA-S15 - the walk-boundary yield and `C_CANCEL`, built 2026-10-10

On `worktree-ba-open-marks` from `6aa76ec7` (BA-R12, BA-Q11). Exempt from
the census (BA-Q1). The design is CLA's proposal, adopted under the
go-ahead.

**The walk yields.**
- A `SELECT`'s outermost walk yields every `exec::SlicePolicy::pages_per_slice`
  (64) pages, at the bottom of its page loop: no pin, no span
  (`step_vm.cpp`, `ExecuteAsync`).
- The structural-refusal note is cleared after each resume, so the note a
  walk ends with is its own (BA-S14 reads it).
- 64 was not re-measured. BA-Q11 asked for it "re-measured in the stage";
  it is read at BA-S17's A/B instead.

**`server::SelectRun` owns the statement.**
- It owns everything a `SELECT` reads: view and lease, parse, AST,
  borrow, chain, sinks, quota, and a scratch set (trail collector, replay
  index, output sort, counters) borrowed from a per-dispatcher pool.
- The pool replaces the members `trail_scratch_`, `replay_scratch_`,
  `sorter_` and `exec_stats_`.
- The plain path hands the run on in `DispatchOutcome::select_run` only
  under `DispatchAsync` (`may_park_`), untraced, with an outermost step
  that walks. A pk descent, a traced statement and every synchronous
  caller run it inline.
- `AwaitStatementWaits` completes the run at its tail, so every dispatch
  `DispatchAsync` makes is followed by it.
- While handed on, the run holds:
  - this core's statement epoch at the word its head revalidated against;
    the slot publishes the lowest of the running and the held words,
    which is BA-Q11's amendment;
  - a pin on the catalog memo (`Catalog::MemoPin`). `Invalidate` retires
    a pinned generation whole rather than freeing it, and pins are counted
    per generation.
- **Members made statement-local** (§1.10):
  - the BA-S14 poison flag rides the outcome (`poison_withheld`);
  - a deferred `strict` commit carries the `DispatchAsync` call that
    staged it, so a statement parks on its own entry and never on
    another statement's.
- The log line for a handed-on `SELECT` is written when its walk ends.

**`C_CANCEL` has a handler** (`protocol.md` §3, §10).
- `StatusCode::kCancelled` maps to the wire's existing `CANCELLED`
  category.
- `CancelRegistry` is one latched map for the instance.
- A KWP session registers at accept on a listener that has both an
  identity source and the registry, and only that listener offers
  `CANCEL`. The session unregisters at close and at `Detach`.
- A cancel connection's first frame is matched and closed unanswered.
- The session's flag (`Session::cancel_flag`) is taken at the slice
  boundary, before and after the yield, and ends the walk `Cancelled`
  with the transaction failed. Otherwise it is taken at the next frame.
- A connection closing mid-statement sets its own flag.

**Teardown.** `~CoreRuntime` discards its scheduler's queued tasks before
the dispatcher, catalog and transaction manager they borrow are destroyed
(`Scheduler::DiscardTasks`).

**Text:**
- `sched.md` §3: the yield, what it holds, and the 256-lease cap per core
  that a 257th suspended walk meets as `OutOfSpace`.
- `protocol.md` §3 and §10.
- `client-manual.md`.
- The stale comments in `statement_epoch.hpp`, `txn/manager.hpp`,
  `budget.hpp`, `status.hpp`, `handshake.hpp`, `tcp_server.hpp` and
  `step_vm.cpp`.

**Cells** (`tests/walk_slice_rig_test.cpp`, `kwp_session_test.cpp`):
- a point read on a long walk's core is answered inside the walk;
- a schema-word move during a suspension retires, and does not free, the
  memo the walk bound; the core's epoch stays at the walk's word, and
  both are given back when the walk ends;
- a split of leaves behind, at and ahead of the walk, made inside its
  suspension, leaves it missing and repeating nothing;
- a cancel ends the walk and fails its transaction;
- a walk of less than one slice never sees a cancel;
- a core torn down with a walk queued;
- the epoch hold, the per-generation pin, the registry, and a cancel
  connection hit, miss and absent registry.

**Not met in this stage:**
- **"A root growth on another core"** is exercised as a schema-word move
  on the walk's own core, a DDL. Both move the same word through the same
  `Revalidate`.
- **The teardown cell kills its mutant only under AddressSanitizer.** With
  `DiscardTasks` removed, a local `-fsanitize=address` build of
  `kds_tests` reports a heap-use-after-free in `~MemoPin`, and the fixed
  tree runs clean. Plain Debug passes the mutant, because nothing reuses
  the freed memory. The tree has no sanitizer build; the harness's
  `alloc_counter.cpp` needs `alloc_dealloc_mismatch=0`.
- No cell runs a `strict` commit beside a suspended walk. The fix is
  covered by reading only.
- No end-to-end `C_CANCEL` through `TcpServer`.
- An index walk and a join's inner walk do not yield.

**The review** (`critics-developer`), applied by CLA:
- C1: a peer torn down with a walk queued was a use-after-free. Fixed by
  `DiscardTasks`. It was latent before for any parked statement.
- C2: `deferred_at_entry` adopted other statements' `strict` commits.
  Fixed by the statement number.
- C3: the pin graveyard was unbounded under overlapping walks. Fixed by
  per-generation pins.
- C4: the reader-lease cap became reachable. Stated in `sched.md`.
- C5: registry registration and unregistration were asymmetric. Fixed by
  `Connection::cancel_registered`.
- Cuts:
  - a pk descent runs inline;
  - `sorter_` and `exec_stats_` deleted;
  - one completion site;
  - `MemoPin`'s null test gone;
  - a `CurrentCoreGuard` around the recording;
  - the stale comments.

**Not applied:**
- **`SetSelectRunStart`, kept.** Removing it means moving `SelectRun` and
  `StarDescription` above `DispatchAndStage`, a large move for one
  assignment.
- **`StatementEpochs::Hold`'s sorted insert, kept.** An append would be
  unsound the day a word arrives out of order, because the published
  minimum would then be wrong. The comment was the part to fix.
- **`select_run` as a `unique_ptr`, not tried.** It would make
  `DispatchOutcome` move-only, and every caller that copies one would need
  checking. The shared handle costs no more than one allocation, which the
  run already pays.

Overhead not measured; measured at the milestone's close.

### BA-S17 - the close, 2026-10-10

On `worktree-ba-open-marks`. The code measured is at `e5a7e1b7`
(`v2.7.0-736-ge5a7e1b7`). The branch was merged into `main` at `836b9551`
and fast-forwarded to `1a5176ae` (BJ's commits, no BA code). The suite at
`1a5176ae` passed 3411/3411, 1 disabled.

**Measured** (A = `d43845a0`, B = `e5a7e1b7`, `build-release`):
- **The overhead A/B**
  (`bench/v3.0.0/results-ba-close-overhead-v2.7.0-736-ge5a7e1b7.md`):
  - An unsynced pk `INSERT`: nothing resolvable.
  - The other pk statements: +0.1 to 1.25 µs.
  - A whole-relation walk: about 2-4.5 ns per row, +20 to +46 µs at
    10,000 rows. Not attributed to a stage.
  - A synced `INSERT` at `cores = 2`: +13 to +32 µs, 1-3 % of a commit
    that is 95 % device.
  - **At 10,000 rows, B's longest statement is 62-226 ms in 31 of 32
    runs** (A: 18-22 ms), attributed to the checkpoint.
  - Scenario 0's `c8-s` cell: no visible refusal in 3 of 3 passes.
  - The 50,000- and 100,000-row walk cell is invalid: setup timeouts under
    another session's build. Its reproduction on a quiet host ran clean.
- **The census, KDS half**
  (`bench/v3.0.0/results-ba-s17-census-v2.7.0-736-ge5a7e1b7.md`): 675 runs.
  The 32 cells holding the 76 contaminated runs were quarantined and
  re-run, and the second screen flags none.
  - **BA-S7 met.** At `cores = 2` and 16 sessions:
    - `group` runs at 6,102-6,606 statements/s, against 1,656-3,558 at
      BA-S4;
    - `strict` runs at 6,214-6,383, against 703-752;
    - PostgreSQL `on` runs at 5,035-5,325.
    - The sync gate's share fell from 55 % to 9.6 %.
    - Core 0's p99 fell from 1.3-1.6 s to the peers' 8.5-9.7 ms, so
      BA-R4's premise holds now.
  - **BA-S12 met.** P6 fell from 10.0 % to 0.38 %.
  - **BA-S14 met.** No refusal reaches a client; 29 are re-run inside the
    engine.
  - **BA-S13 not met.** The carve's median is 3.6-5.9 ms under `group`
    and `strict`, but **226-258 ms under `relaxed`**, against BA-S4's
    20-46 ms and the 10 ms exit. The unconditional data-file
    `fdatasync` flushing the whole unsynced backlog is the candidate;
    it is untested.
  - **The `relaxed` path is unchanged**, as BA-S4's verdicts (P1-P5 not
    material) predicted. `cores = 4` still falls below `cores = 2`;
    unresolved.
  - P11 is not shown material.
  - The cross-day shift: unchanged cells read +12 to +28 % over BA-S4,
    and the same-day A/B is equal on them. So ratios against BA-S4 carry
    a host shift. The `cores = 2` gains are far above it.

**BA closes on this measurement**, adopted on the operator's word of
2026-10-10, translated: *"and close that milestone"*. It carries these stops
and findings:
- **The checkpoint half of P12** waits for the operator's mark on BI-Q1.
  B's 10,000-row checkpoint stall belongs to it.
- **BA-S13's `relaxed` carve regression**:
  `known-gaps.md`, "Statement scheduling" and the carve entry.
- **The walk's per-row cost and the pk `SELECT`'s +0.5-0.95 µs**: not
  bisected to a stage.
- **BA-S15's 64-page slice**: not varied.
- **BA-S14's forced grown-over-root cell**: not built.
- **BA-S5, S6, S8, S9, S10, S11, S16**: not opened (BA-S4), and BA-Q6 (b)
  stays recorded for BA-S8.

Overhead measured at the milestone's close, above.
