# Work order BA — parallelism against the PostgreSQL floor: twelve serialisation points

Written 2026-10-02 on `worktree-parallelism-workorder` from `d3d90b5`
(`v2.7.0-588-gd3d90b5`), on the operator's *"postgres와 비교해서 병렬성이
부족한 부분에 대해 개선을 할거야. 원인을 분석한 항목 별로 개선점을 플래닝하고
작업문서를 작성해야해"*. This order plans one fix per cause, for every place
where work on one core waits for work on another and PostgreSQL 18.6's work
does not. **Not opened.** The letter, the scope and every §4 item wait for
the operator's mark, and each stage then waits for its own word. It cuts no
tag. Like AS and AZ it sits outside AR0 §8's chain: it depends on no open
order and gates none.

**Where the items came from.** The starting point was
`bench/v3.0.0/results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md` plus a
spec-level reading of what serialises. §1 re-read every claim in the code
at `d3d90b5`. Two of the spec-level claims were wrong:

- minting a snapshot takes no latch;
- the read view's latch is taken per *row*, not per statement.

The code also showed points that no spec names. The largest is one mutex on
every page access (P1).

## 0. The items

Each row is an instance-wide point where a statement on one core waits for a
statement on another, at `cores > 1` unless the row says otherwise. Every
row waits the same way, and §1.0 says why: the reactor thread itself waits,
so every session on that core waits with it.

| # | the point | how often | PostgreSQL 18.6 | stage |
|---|---|---|---|---|
| P1 | **The frame table**: one `std::mutex` over one `unordered_map`, with every pin count under it, and the free map's latch nested inside | every page access on every core: 3 holds per read fetch, 4 per write fetch, plus 1 free-map hold | 128 buffer-mapping partitions, each looked up under a shared lock; a pin is a compare-and-swap on the buffer's state word | BA-S8 |
| P2 | **The visibility window**: `window_latch_`, a `std::mutex` that exists even at `cores = 1` | every row a reader classifies whose writer is above the floor, which in practice means recently written rows; 2 holds per commit, plus `Reclaim`'s scan | a snapshot under `ProcArrayLock` (shared), reused while no transaction has completed; a tuple is judged from the snapshot and its hint bits, with no shared lock | BA-S9 |
| P3 | **Statistics on the read path**: catalog page 11 latched exclusive; the optimizer collector's latch; `CabinStore`'s `stats_latch_` | page 11: every step of every successful `SELECT`. Collector: every fingerprinted `SELECT`. `stats_latch_`: every Cabin hit | backend-local counters, flushed at most once per `PGSTAT_MIN_INTERVAL` (1,000 ms) | BA-S5 |
| P4 | **The relation borrow**: one lock-table partition per relation; `IX` asked again for every written row; the decide's release quadratic in the number of borrows | 3 holds per `SELECT`, 1 per written row, 2 per borrow at the decide | weak relation locks go in per-backend fast-path slots; the shared table is used only once a strong lock is counted | BA-S10 |
| P5 | **The pk mark**: every relation's `sys.tables` row lives in one chain starting at page 7, held exclusive while a WAL record is appended | every row whose pk is omitted, and every named pk at or above the mark | `nextval` writes one WAL record per `SEQ_LOG_VALS` (32) values | BA-S11 |
| P6 | **The WAL append**: one `std::mutex` held over encode, copy and CRC, and also over a ring-full `pwrite`, a segment roll's preallocation, prewrite and two `fsync`s, and every flush's `pwrite` | every record | space is reserved under a spinlock and records are copied in parallel under one of eight insertion locks | BA-S12 |
| P7 | **Commit durability**: core 0 syncs inline on its reactor, and `strict` there syncs once per commit with no sharing. A peer's `strict` blocks its reactor on a condition variable. A peer with a parked `group` commit spins its reactor. Every sync `fdatasync`s every segment ever written. A peer's writeback runs core 0's inline sync on the peer's thread (a defect) | every commit | the backend waits for `WALWriteLock` and skips its own sync when another backend's flush already covered its record | BA-S1, BA-S7 |
| P8 | **The assertion directory**: its latch is taken on every write commit, with or without assertions; once one assertion exists anywhere, on every row of every relation | every write commit; every written row | — (no `CREATE ASSERTION`) | BA-S6 |
| P9 | **The rightmost leaf**: monotonic pks send every core's insert to one leaf. Its latch is held across the Cabin witness, index maintenance, the assertion reservation, the undo append and the WAL append. A descent that goes stale is refused `TXN_CONFLICT retryable=1` to the client | every insert | B-link tree: a descent moves right past a concurrent split instead of failing | BA-S14 |
| P10 | **No yield inside a statement**: a long walk holds its core until it ends; `C_CANCEL` has no handler | every long statement | each backend is a process the OS preempts; `CHECK_FOR_INTERRUPTS` serves cancels | BA-S15 |
| P11 | **Session placement**: the kernel's `SO_REUSEPORT` hash decides, with no load-aware choice and no migration | every connection | the OS schedules backends across every CPU | BA-S16 |
| P12 | **Periodic stalls on a reactor**: a transaction-id carve syncs the whole pool inside `Begin`; a checkpoint runs to completion on its core | carve: every 4,096 transactions per core. Checkpoint: every 5 s per core | assigning an XID writes no data page; the checkpointer is a separate process | BA-S13 |

**What the tree has measured**, with each item it points at:

- **Scenario 0** (eight traders, four autocommit statements a transaction).
  Going from `cores = 1` to `cores = 8` takes `group` from 626.0 to 661.0 tps
  (1.06x) and `strict` from 169.4 to 331.7 tps. PostgreSQL reads 710.8 tps
  (`results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md` §2). At
  `cores = 1`, `strict` makes 678 durable statements a second, one every
  1.5 ms (`results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md` §5), which
  is the arithmetic of one sync per commit (P7).
- **Refusals under contention.** Every `s0-c8-s` run had one trade insert
  refused `TXN_CONFLICT` by the btree's bounded re-descent (same file, §6)
  (P9).
- **Serial reads, one session.** `cores = 8` reads 3–5 % below `cores = 1`
  in every scenario-3 cell (summary §4): this is what P1–P4 cost with nobody
  contending.
- **No measurement of what extra cores buy.** AT-S13
  (`results-at-s13-prices-v2.7.0-391-gf6f2073.md`, at `9a0525d`) found that
  its Python driver saturated at 18–20k qps before the engine did. **No
  number in the tree prices what eight reactors buy**, and BA-S2..S4 close
  that gap first.

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

Read, not run. Every KDS citation is to `d3d90b5`. Every PostgreSQL citation
is to the 18.6 release tarball (`postgresql-18.6/…`), the source of the
binary the comparison ran (summary §1).

### 1.0 Every wait blocks a reactor

Every wait below is the reactor thread waiting, never one task:

- **`Latch`** is `std::mutex` (`include/kds/base/latch.hpp:52`), so a
  contended acquisition puts the reactor thread to sleep in the kernel.
- **The page latch** spins 64 turns (`kPageLatchSpinTurns`,
  `include/kds/storage/page_latch.hpp:94`), then calls
  `std::this_thread::yield()` on every turn (`:163-169`). Reactor threads are
  pinned to their CPUs (`src/server/expeditor.cpp:1206`), so a yield usually
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
pins and page latches, and no task parks while holding a pin
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
  - a write fetch takes it once more, to mark the frame dirty (`:2295-2296`);
  - `StampPageLsn` takes it on every logged mutation (`:1253`).
- **Pins are not atomic.** Each frame's pin count is a plain `uint32_t`
  under the latch: *"AM-S2 decided against making this atomic"*
  (`device_page_store.hpp:801-812`).
- **The free map's latch is nested inside.** Every fetch asks `IsAllocated`
  (`device_page_store.cpp:1225`), which takes the free map's latch (`:546`).
  On a hit that happens inside the frame-table hold. The check proves nothing
  on a hit: *"Nothing frees a page"* (`docs/spec/page.md:130`).
- **The sweep, when the pool is bounded.** With `buffer_pool_frames` above 0,
  every miss on a full pool runs the CLOCK sweep under the same mutex
  (`:690-693`). The sweep copies and sorts every resident page id
  (`:2503-2506`). The default is 0, which means unbounded
  (`include/kds/server/expeditor.hpp:130`).
- **PostgreSQL:** 128 partitions (`NUM_BUFFER_PARTITIONS`,
  `src/include/storage/lwlock.h:93`), each looked up under a shared lock
  (`src/backend/storage/buffer/bufmgr.c:582`, `:2034`). `PinBuffer` (`:3090`)
  pins with `pg_atomic_compare_exchange_u32` on the state word (`:3135`).

### 1.2 P2 — the visibility window

- **Minting a view is lock-free.** It uses atomics only
  (`src/txn/manager.cpp:139`; `src/txn/instance_visibility.cpp:94-116`).
- **Judging a row is not.** `ReadView::Visible` returns early only below the
  floor (`include/kds/txn/read_view.hpp:127`). Above it, `LookupCommit` takes
  `window_latch_` (`instance_visibility.cpp:280`). That happens once for each
  row that `Classify` reaches (`src/exec/step_vm.cpp:2088`).
- **Who pays.** The window is reclaimed only once it passes `kReclaimFloor`
  (1,024) entries (`include/kds/txn/instance_visibility.hpp:578-581`, "Not
  measured"), and above one core the floor is capped by the slowest core's
  issue cursor (`instance_visibility.cpp:138-153`). So the rows that take the
  latch are the recently written ones, which in OLTP are the hot ones.
- **Commits take it too.**
  - `BeginCommit` holds it (`:84-85`).
  - `PublishCommit` holds it (`:246-271`).
  - Past a threshold, `Reclaim` scans and erases the window under it
    (`:300-333`).
- **It is a member, not a pointer that can be null**
  (`instance_visibility.hpp:561`), so `cores = 1` pays for it too.
- **PostgreSQL:**
  - It takes a snapshot under `ProcArrayLock` (shared) and reuses it while
    `xactCompletionCount` has not moved (`GetSnapshotDataReuse`,
    `src/backend/storage/ipc/procarray.c:2095`; the counter bump, `:594`).
  - `HeapTupleSatisfiesMVCC` (`heapam_visibility.c:960`) judges a tuple from
    the snapshot (`XidInMVCCSnapshot`) and sets hint bits (`:144`), so a
    later reader skips the commit log entirely.

### 1.3 P3 — statistics on the read path

**`Catalog::RecordAccess` holds page 11 exclusive for its whole body**
(`src/catalog/catalog.cpp:2950`).

- **Its body** walks up to 4,096 shapes or inserts one, and the code says
  what that costs (`:2943-2945`).
- **Its callers.** It runs once per step of every successful `SELECT`
  (`src/stats/access_stats.cpp:25`, `:44`, `:66`; reached from
  `src/server/command_dispatcher.cpp:6144`, `:6241`, `:6673`, `:6688`,
  `:6703`). Writes reach it only through foreign-key checks (`:3262`).
- **On by default** (`expeditor.hpp:285`) and unlogged (`catalog.cpp:2988`,
  `:3017`).
- **A frame-table hold besides.** The exclusive fetch also marks page 11
  dirty (`device_page_store.cpp:1212`).
- **Who reads it:**
  - `SHOW ACCESS` (`command_dispatcher.cpp:1634`);
  - the relayout planner behind `SHOW RELAYOUT`
    (`src/stats/relayout_planner.cpp:189`, `:232`);
  - `CREATE CABIN`'s warning (`src/exec/cabin_ddl.cpp:67`).

**`OptimizerSignals` takes its latch** (`src/stats/optimizer_signals.cpp:70`)
on every fingerprinted `SELECT` (`command_dispatcher.cpp:6814`, `:6840`).

- It is armed above one core (`expeditor.cpp:1094-1096`) whatever
  `cabin_optimizer` says.
- When the table is full (4,096 ids), it evicts by a linear scan under that
  latch (`optimizer_signals.cpp:47-53`).

**`CabinStore` takes a global `stats_latch_` on every Cabin hit**
(`src/stats/cabin_store.cpp:138`).

- It also takes the partition's mutex (`:115`, `:134`), plus once per entry
  served (`:95`).
- All of these are raw `std::mutex`es, taken even at `cores = 1`.

**PostgreSQL** keeps pending statistics in the backend and flushes them no
more often than `PGSTAT_MIN_INTERVAL`, 1,000 ms
(`src/backend/utils/activity/pgstat.c:126`).

### 1.4 P4 — the relation borrow

- **The partitions.** The lock table has 64 × cores partitions
  (`include/kds/txn/lock_table.hpp:396`; `src/txn/lock_table.cpp:59`), keyed
  by a hash of `(rel_oid, unit, lo)` (`lock_table.cpp:94-97`). A relation's
  key is `{rel, kRelation, 0, 0}` (`lock_table.hpp:352-354`), so every borrow
  of one relation lands in one partition. Each partition has its own `Latch`,
  null at `cores = 1` (`lock_table.cpp:68`).
- **Reads.** A `SELECT` asks `IS` at bind under a fresh holder
  (`command_dispatcher.cpp:6410`; `include/kds/server/read_borrow.hpp:97`)
  and releases and wakes at its end: three holds.
- **Writes.** `BorrowChain` asks `IX` again for every written row
  (`command_dispatcher.cpp:8104`). The coverage and conflict tests scan the
  partition's entries, and each entry's holders, linearly under the latch
  (`lock_table.cpp:174-219`).
- **The decide releases borrow by borrow** (`lock_table.cpp:588`).
  - Each `ReleaseHeld` (`:529-567`) scans its partition and erases the
    entry.
  - `WakeWaiters` then takes the partition again and scans it again
    (`:443-451`).
  - In all, the release is O(K²/P) for K borrows over P partitions:
    *"O(n²/64) a decide"*, 14 ms rising to 49 ms at K = 16,384
    (`docs/inflight/known-gaps.md:493-496`).
  - AZ-Q4 accepted that as priced and struck AZ-S6's keyed partition
    (`workorder-az-ay-carry-forward.md` §3-§4).
  - No latch spans the whole loop, so the cross-core cost is traffic on the
    latches rather than one long hold.
- **No fast path exists for weak relation modes.**
- **PostgreSQL** gives each backend fast-path slots: `FP_LOCK_SLOTS_PER_GROUP`
  (16) per group, `FastPathLockGroupsPerBackend` groups
  (`src/include/storage/proc.h:87`, `:98`). A weak lock goes to the shared
  table only when `FastPathStrongRelationLocks` counts a strong holder
  (`src/backend/storage/lmgr/lock.c:999`). That table has 16 partitions
  (`LOG2_NUM_LOCK_PARTITIONS`, `lwlock.h:96`). Its row locks live in the
  tuple header, which is not available here (invariant 12).

### 1.5 P5 — the pk mark

- **One chain for every relation.** `AllocateRowId` finds the relation's row
  with `ForFirstRow<SysTableRow>` starting at page 7
  (`catalog.cpp:2456-2457`; `kCatalogPageTables`,
  `include/kds/catalog/well_known.hpp:329`). It holds each page of the walk
  exclusive across the callback (`catalog.cpp:176`, `:190`). Every
  relation's 106-byte row (`include/kds/catalog/rows.hpp:150`) is in that one
  chain, so inserts into different relations meet on page 7.
- **A WAL record per bump.** Each bump appends a record and stamps the page
  under the hold (`OverwriteLogged`: `catalog.cpp:2484`, then `:242`,
  `:244`).
  - On the per-row path that is once per row (`command_dispatcher.cpp:5051`).
  - For a sorted fill it is once per statement (`AllocateRowIdRange`,
    `:4766`).
  - A named key at or above the mark takes the same path
    (`catalog.cpp:2593-2598`).
- **The `before_mark` hook breaks the lock table's contract.** It takes two
  lock-table partition latches while page 7 is held
  (`command_dispatcher.cpp:5032-5041`), against `lock_table.hpp:136-139`,
  which requires *"no page latch held"* (§1.13).
- **The rules it sits under.** Invariant 11 and `heap-and-tuple.md` §4.1a
  give one sequence in issue order, bumped under the page latch, with no
  per-core block. `docs/rules/keystoneid-invariant.md` K3 already counts
  *"bump-ahead recovery"* among the gaps it licenses (`:23-25`). §2 requires
  the mark *"persisted before the row is placed"* (`:130-133`).
- **PostgreSQL:** `SEQ_LOG_VALS` is 32
  (`src/backend/commands/sequence.c:58`). `nextval` writes one WAL record
  per 32 values, and a crash skips whatever was never handed out.

### 1.6 P6 — the WAL append

**The append takes the stream latch** (`src/wal/stream.cpp:210`) and holds it
across:

- the segment-fit check, and any seal and roll (`:212-219`);
- the ring-full check (`:221-227`);
- the LSN assignment, the encode, the payload `memcpy`, the CRC32C
  (`src/wal/record.cpp:101`, `:111`) and the cursor bump (`stream.cpp:229-235`).

**I/O also runs under that latch:**

- **A full ring (1 MiB)** is drained by the appender itself with a `pwrite`
  (`src/wal/manager.cpp:317-324`).
- **A full segment (64 MiB)** is sealed, then `CreateSegment` runs:
  `posix_fallocate` (`src/wal/file_log_device.cpp:260`), a zero prewrite of
  64 × 1 MiB followed by an `fsync` (`:105`, `:117`), and a directory `fsync`
  (`:287`).
- **Every `Flush` and `Sync` caller** holds the latch across its `pwrite`
  (`stream.cpp:239-263`). Only the `fdatasync` runs outside it (`:278`).

**The page latch is the outer one**
(`include/kds/storage/device_page_store.hpp:147-151`). An insert appends
while holding its leaf (P9), so a segment roll lands inside every page hold
that is waiting on the append.

**Why it is one latch.** AL-R1 chose the latch because a bare `fetch_add`
cannot express a roll, an oversized refusal or `OutOfSpace`. It also named
the follow-on: *"staging into a second buffer and writing it unlatched is
the follow-on if it shows"* (`workorder-al-m0-single-wal.md` AL-R1).
`wal.md` §6-1 records the reserve/copy/publish split as *"tried and
abandoned"*.

**PostgreSQL:**

- `ReserveXLogInsertLocation` reserves space under `insertpos_lck`
  (`src/backend/access/transam/xlog.c:1111`, `:1134`).
- Records are copied under one of `NUM_XLOGINSERT_LOCKS` (8, `:151`).
- A flush first waits for in-progress copies below its target
  (`WaitXLogInsertionsToFinish`, `:1507`).

### 1.7 P7 — commit durability

**How the two durability classes commit.**

- **`strict` (D1)** syncs inside `Commit` (`manager.cpp:357-369`).
- **`group` (D2)** stages the commit, and the statement parks on
  `IsDurable(lsn)` (`command_dispatcher.cpp:743-745`). The post-task hook
  drains once per reactor pass (`src/server/core_runtime.cpp:474-494`;
  `src/sched/scheduler.cpp:429`).
- There is no batching delay. `wal_drain_interval_us` (1,000 µs) is only a
  backstop.

**Core 0 syncs on its own reactor**, by a recorded decision
(`manager.cpp:158-168`): handing the sync to the writer *"doubled `group`'s
p99"* on a 2-core host. AL-S8 later measured the hand-off on this 8-CPU
host. A peer's commit tail, which does hand off, was *"indistinguishable
from core 0's at both p50 and p99"* (`workorder-al-m0-single-wal.md`, AL-S8's
row). Core 0's D1 calls `Sync()` on every commit, so each D1 commit pays its
own `fdatasync`, whatever else is in flight.

**A peer blocks or spins.**

- **A peer's D1** flushes, then waits on the writer's condition variable
  (`manager.cpp:190-198`; `writer.cpp:34-38`), which blocks its whole
  reactor.
- **A peer's D2** asks the writer for a sync (`manager.cpp:427`), and its
  parked statement polls. Meanwhile the drain hook reports work for as long
  as a commit is pending (`core_runtime.cpp:481-487`). So the scheduler never
  blocks (`scheduler.cpp:193`), and every pass takes the stream latch and the
  writer's mutex again (`manager.cpp:249`; `writer.cpp:25`) until the sync
  lands.

**The writer coalesces** (`writer.cpp:15-19`, `:72`). So the peers' syncs are
already shared. Core 0's inline syncs are not.

**Every sync costs one `fdatasync` per segment ever written**
(`file_log_device.cpp:386-405`), and the log is never recycled
(`docs/inflight/bugs/wal-segment-descriptors-exhaust-the-open-file-limit.md`).
So a commit's sync costs more the older the log is.

**A defect, found by this survey.** The shared store's WAL gate is core 0's
manager (`expeditor.cpp:825`), and peers borrow the store unchanged
(`core_runtime.cpp:154-168`).

- Two peer paths reach that gate's `EnsureDurable`
  (`device_page_store.cpp:1327`):
  - a peer's checkpoint flush (`src/wal/checkpointer.cpp:214`);
  - a peer's carve (`expeditor.cpp:1171-1174`, wired to every peer at
    `:1752-1754`).
- When the page is not yet durable, `EnsureDurable` falls through to
  `Sync()` (`manager.cpp:303`). That is core 0's inline sync, run on the
  peer's thread.
- It writes core 0's statistics and group batch, which `manager.hpp:29-31`
  says no other thread touches.

The bug entry is
`docs/inflight/bugs/a-peers-writeback-runs-core-0s-wal-sync-on-the-peers-thread.md`.

**PostgreSQL:** `XLogFlush` takes `WALWriteLock` with `LWLockAcquireOrWait`
(`xlog.c:2854`). A backend whose record another backend's flush covered
returns without a sync of its own, and only that backend waits.

### 1.8 P8 — the assertion directory

- **Every write commit takes the latch.** `CommitTxn` and `AbortTxn` take the
  directory latch unconditionally (`src/exec/assertion_check.cpp:703`,
  `:766`). Their callers:
  - every autocommit write's end (`command_dispatcher.cpp:8424-8425`, `:8519`);
  - `COMMIT` (`:7702`) and `ROLLBACK` (`:7781`);
  - a failed statement's abort (`:8550`).

  The latch exists above one core (`expeditor.cpp:1021`).
- **One assertion makes every row pay.** The per-row checks skip the latch
  only while no assertion exists in the instance
  (`include/kds/exec/assertion_check.hpp:404`). Once one does, these take it
  for every relation's rows:
  - `AdmitInsert` (`assertion_check.cpp:430-431`);
  - `ReserveInsert` (`:535-538`);
  - `AnyOn` (`:97-98`);
  - `CannotEnforce` (`:103-104`).

### 1.9 P9 — the rightmost leaf

**The leaf is held across the whole insert.**

- The descent takes the leaf exclusive (`src/storage/btree/btree.cpp:190-191`)
  and hands it back still held, together with any parents a split secured
  (`:935`, `:957-962`).
- Under that hold the insert runs the Cabin witness, index maintenance, the
  assertion reservation and the undo append
  (`command_dispatcher.cpp:5101-5180`). It then appends `kHeapInsert`
  (`:4257`) and only then releases (`:4264`).
- AT-S21 put the append under the hold on purpose
  (`records-appended-after-their-page-is-released.md`). Nothing records
  whether the steps before the append also need the hold.

**A stale descent is refused, not retried.**

- A descent restarts at most `kMaxDescentRestarts` (4) times
  (`include/kds/storage/insert_placement.hpp:67`; `btree.cpp:205-206`,
  `:232`).
- A stale parent path is re-descended a bounded number of times (`:499`,
  `:524`).
- A root that another core grew over is refused with no retry at all
  (`:545-549`).
- The index tree mirrors all three
  (`src/storage/index/index_tree.cpp:133`, `:197`, `:235`, `:262`).
- Each refusal reaches the client as a retryable `TxnConflict`
  (`command_dispatcher.cpp:5088`; `src/wire/error_registry.cpp:89`). Nothing
  in the engine retries it.

**PostgreSQL:**

- `_bt_moveright` (`src/backend/access/nbtree/nbtsearch.c:246`) moves right
  past a concurrent split instead of failing.
- Once the tree is `BTREE_FASTPATH_MIN_LEVEL` (2) deep, the rightmost-leaf
  fast path caches the target block (`nbtinsert.c:30`, `:1426`).

### 1.10 P10 — no yield inside a statement

- **A statement's body runs synchronously.** `DispatchAsync` calls the body
  directly (`command_dispatcher.cpp:694`, and the comment at `:685-687`). The
  executor runs to completion (`src/exec/step_vm.cpp:2821`) and refuses any
  park beneath it (`:168-172`).
- **The one suspension point is unused.** The bottom of the page loop is
  *"the executor's one legal suspension point"*, and nothing has suspended
  there since AT-S10 (`step_vm.cpp:1926-1933`). `sched::Yield` is used
  nowhere in `src/`.
- **So a walk holds its core** until it ends or hits `max_rows_touched`
  (100M by default, `include/kds/exec/budget.hpp:56`): every session, every
  timer and the WAL hook on that core wait. That contradicts `sched.md:42`
  (*"Cooperative yielding is mandatory"*). `C_CANCEL` has no handler
  (`protocol.md:131`).
- **Scheduling groups do not help.** They arbitrate between groups, not
  between sessions. Every client statement is `kForeground`
  (`src/server/tcp_server.cpp:585`), served first-in-first-out
  (`scheduler.cpp:264-286`).

### 1.11 P11 — session placement

- **The kernel decides.** Above one core, every core binds with
  `SO_REUSEPORT` (`tcp_server.cpp:90`; `expeditor.cpp:1470`;
  `core_runtime.cpp:585`) and the kernel picks. The fallback hands
  connections out round-robin (`src/server/connection_handoff.cpp:20`).
- **A session never moves** after `AdoptConnection`
  (`tcp_server.cpp:368-370`). There is no `SO_ATTACH_REUSEPORT_*BPF` and no
  `SO_INCOMING_CPU`.
- **What that implies (inferred, not measured).** On a loopback listener
  only the client's ephemeral port varies, so the choice is effectively
  random. Eight sessions on eight cores occupy about 5.25 cores on average,
  and all eight are distinct only about 0.24 % of the time.
- **The tree already works around it.** `tools/multicore_benchmark.py` reads
  each session's `core=` from `SHOW META` and opens connections until every
  core has its share.

### 1.12 P12 — periodic stalls on a reactor

**The transaction-id carve.**

- Ids come in blocks of `kTrxIdBlockSize` (4,096,
  `include/kds/txn/trx_id.hpp:74`).
- A carve takes the superblock latch twice (`src/txn/trx_id.cpp:26`;
  `expeditor.cpp:1159`).
- It persists the new ceiling before issuing any id (`trx_id.cpp:41-49`),
  through `PersistTrxIdCeiling`, which is `store_->Sync()`
  (`expeditor.cpp:1171-1174`). That writes every dirty page in the
  instance's pool behind the WAL gate, then syncs the data file.
- All of it runs inline in `Begin` on the carving core
  (`src/txn/manager.cpp:155`). An idle core burns blocks too
  (`MaybeBurnIdleBlock`, `manager.cpp:611-630`).

**The checkpoint.**

- It runs every `checkpoint_interval_ms` (5,000) per core, staggered
  (`core_runtime.cpp:526-529`).
- `RunToCompletion` (`src/wal/checkpointer.cpp:335`) holds its reactor for
  the whole run. That contradicts `checkpointer.hpp:32-36`, which says the
  run spreads across iterations.
- It holds the assertion registry's latch across its snapshot appends
  (`checkpointer.hpp:152-157`).

**A hypothesis, not measured.** The unexplained ~0.4 s stalls have this
shape: AT-S13's cell 2, and AZ-S7's cell 3
(`results-az-s7-overhead-v2.7.0-568-g9f170b1.md`, *"The stall is on both
engines, again"*). BA-S2's counters are what would show it.

**PostgreSQL:** `GetNewTransactionId` (`varsup.c:77`) takes `XidGenLock`
(`:105`) and extends the commit log (`:204`), but syncs no data file. The
checkpointer is a process of its own.

### 1.13 Where the tree's own text disagrees with the code

Found by this survey. Each is corrected by the stage named.

- `docs/spec/eviction.md:93` says *"per-core map, no lock"* (BA-S8).
- `include/kds/storage/device_page_store.hpp:159-173` describes a durability
  wait on the fault path that the inline sweep never reaches. Also,
  `eviction.md` EV8's `evict_retry_budget` exists in no source (BA-S8).
- `src/exec/step_vm.cpp:2066-2072` says a visible writer *"pays nothing at
  all"*, but above the floor it takes `window_latch_` (BA-S9).
- `docs/spec/wal.md` §3 says *"a peer performs no device sync"*, and
  `include/kds/wal/manager.hpp:29-31` says no other thread touches a
  manager. The peer-thread sync contradicts both (BA-S1).
- `command_dispatcher.cpp:1279` cites `manager.cpp:110` for the sync count,
  which is at `:225` (BA-S7).
- `include/kds/wal/stream.hpp:27` calls the stream latch *"outermost"*. It is
  outermost only against the device's own lock (BA-S12).
- `include/kds/wal/checkpointer.hpp:32-36` says the run *"spreads across
  reactor iterations"* (BA-S13).
- `lock_table.hpp:136-139` requires no page latch, against the `before_mark`
  hook under page 7 (BA-S11).
- `docs/spec/sched.md:42` says *"Cooperative yielding is mandatory"*
  (BA-S15).
- Not this order's, recorded so it is not lost: `src/txn/manager.cpp:325-329`
  says a failed autocommit commit leaks its transaction, but `EndWrite` now
  aborts it (`command_dispatcher.cpp:8522-8524`).

## 2. Rulings — CLA's proposals, not marked

**BA-R0 — measure first, with an instrument that does not saturate first.**

*The counters (BA-S2).*

- **What is counted.** For every `Latch` row of `rules.md` §3, and for
  `CabinStore`'s raw mutexes: the acquisitions that found the latch held, and
  the nanoseconds spent waiting. The `Latch` rows are the frame table, the
  free map, the window, the lock partitions, the stream, the assertion
  directory, the optimizer collector and the superblock. Also counted:
  - the page latch's spin turns, which `Acquire` returns and the store throws
    away today (`device_page_store.cpp:2245`, `:2277`);
  - syncs by thread (core 0 inline, or the writer);
  - drain passes spent with a commit pending (the spin of §1.7);
  - carves and the longest one;
  - checkpoint runs and the longest one;
  - refusals by site, at the five `TxnConflict` sites of §1.9.
- **How it is counted.** Try the lock first, and count and time only the
  contended branch. Each counter is per core and summed when read, so the
  instrument adds no shared cache line. `SHOW META` prints them.
- **What `cores = 1` sees.** Nothing new is armed at `cores = 1`. The new
  `SHOW META` fields are the one visible change, and any golden that pins
  `SHOW META` moves with them.

*The driver (BA-S3).* This is a tools stage that lands before any
measurement, which is `bench/README.md`'s rule.

- **Clients.** N client processes, never threads: AT-S13's ceiling was one
  process. They are pinned to CPUs disjoint from the server's, and each
  process's CPU use is recorded.
- **Shapes:** a pk point read; scenario 0's trade; a monotonic-pk insert,
  into one relation and into N relations (the second separates page 7 from
  the leaf); and a read of rows written within the last few hundred commits
  (to reach P2). Each runs at `relaxed`, `group` and `strict`.
- **The PostgreSQL twin** runs the same shapes at the same client counts,
  with `synchronous_commit` on and off. Off is the like-for-like for
  `relaxed`, which the summary did not measure (§7).
- **Client-bound cells.** A cell whose clients run above 80 % CPU is marked
  client-bound. It reads as a floor, never as the engine's ceiling.

*The host (BA-Q2).*

- It has 8 logical CPUs on 4 physical cores; the siblings are 0/1, 2/3, 4/5
  and 6/7 (`lscpu -e`, 2026-10-02).
- The server gets CPUs 0–3 (two physical cores) and the clients get 4–7. So
  `cores` ∈ {1, 2, 4}, and nothing above 4 is measured on this host.
- PostgreSQL's postmaster is pinned to 0–3 the same way.

*The census (BA-S4).*

- **The matrix:** sessions {1, 2, 4, 8, 16} × `cores` {1, 2, 4} × the shapes,
  on a release build, under every rule in `bench/README.md`.
- **Output:** a scaling curve per shape for KDS and for PostgreSQL, and, for
  each item, its contended wait as a share of reactor wall time
  (`sched_wall_us`).
- **When an item is material:**
  - in general, when that share reaches 5 % in any cell, or when the item
    refused anything (BA-Q1);
  - P11, when a balanced arm (connections opened until every core has its
    share, `multicore_benchmark.py`'s method) beats the kernel's placement by
    more than the cell's spread;
  - P12, when any carve or checkpoint holds a reactor longer than 10 ms.
- **What it decides.** The census ranks the fix stages. A fix stage opens only
  for a material item.

**BA-R1 — a peer's writeback stops running core 0's inline sync (P7, the
defect).**

- **The fix.** The store's WAL gate answers from the calling core's manager.
  A peer's `EnsureDurable` becomes its own attached manager's, which waits on
  the writer as its commits do.
- **The shape.** The store already knows `CurrentCore()`, so it keeps one gate
  per core rather than one gate for the instance. This keeps
  `manager.hpp:29-31` true as written.
- **Red first.** On the two-core rig (`workorder-av-two-core-rig.md`), a peer
  flushes a page whose LSN is not yet durable while core 0 holds a parked D2
  commit. A debug assertion of the owning thread in `WalManager::Sync()`
  fails today.
- **Done.** The assertion holds, and the bug entry is deleted.

**BA-R2 — statistics are folded per core (P3).**

- **The mechanism.** `RecordAccess`, the collector's note and `CabinStore`'s
  hit statistics all accumulate into a per-core pending table. Each core is
  that table's only writer, so it needs no latch.
- **When a core folds.** Each core folds its own pending counts into the
  shared store at a cadence, and again before it answers `SHOW ACCESS`,
  `SHOW RELAYOUT` or `CREATE CABIN`'s warning.
- **The controller.** It folds every core's shard at its own tick, through a
  per-shard latch the owner takes once per fold, never once per statement.
- **What changes.** `crosscore.md` CC13's *"a peer's count is in the row
  before its statement returns"* is struck: a count reaches the row within
  one cadence.
- **What does not change.**
  - The relation stays unlogged (CR6), so a crash loses at most one cadence
    of statistics. That is invariant 8's class.
  - `SysAccessStatRow` gains no `core_id`, since nothing names a core on disk
    (AT-S9).
- **The cadence.** 100 ms, as a constant, unless an existing setting already
  expresses it, in which case that setting is re-scoped (`CLAUDE.md`,
  Working Rules). This is BA-Q4.

**BA-R3 — only a transaction that reserved touches the assertion directory
(P8).**

- **Commits.** `CommitTxn` and `AbortTxn` return before taking the latch
  when the transaction holds no pending reservation. The transaction records
  that it reserved: `ReserveInsert` sets a flag under the latch it already
  holds, so the test itself needs no latch.
- **Per-row checks.** These skip the latch for any relation no assertion
  covers, by reading a per-relation count with acquire.
- **Why the count is safe.** `CREATE ASSERTION` raises the count under the
  relation's `X`, which already waits for every writer (AT-S5e). So a writer
  that read 0 finishes before the build takes its base.

**BA-R4 — commit syncs leave every reactor (P7).**

- **Core 0 hands off too.** Core 0's commits hand their sync to the writer,
  as a peer's already do.
- **Parked commits are kicked.** The writer kicks every parked statement when
  its watermark passes it, write-then-kick through the wake registry
  (`sched/waker_table.hpp`, AU-R2). The polling pass goes, and the drain hook
  stops reporting a pending commit as work, so an idle reactor blocks.
- **D1 on any core parks too.** It requests a sync and parks like D2, instead
  of blocking its reactor, and it shares whatever sync concurrent requests
  ride.
  - D1's contract is unchanged: the record is durable on return, and D1
    never waits for a batch to form.
  - What changes is that D1 no longer refuses to share a sync (BA-Q3).
- **Blocking waits that stay.** `EnsureDurable` (WAL-before-data) and
  `SyncAll` keep their blocking wait, because their callers hold pages
  (§1.0).
- **Only unsynced segments are synced.** A sync covers the segments that hold
  unsynced bytes, not every segment ever written. The device tracks the
  oldest segment not yet synced.
- **The premise is re-measured first.** `manager.cpp:158-168`'s argument was
  measured on a 2-core host. The census re-measures the hand-off's p50 and
  p99 under `group` and `strict` before anything changes.

**BA-R5 — the frame table is partitioned (P1).**

- **The change.** The map is split into 128 partitions by a hash of the page
  id, each with its own `Latch`, null at `cores = 1` as today. Pins and the
  dirty generation become atomic per frame.
- **Eviction.** The sweep visits partitions one at a time. It evicts a frame
  only after a compare-and-swap of its pin count from 0 to a sentinel, so a
  concurrent fetch either pins first or misses. EV4 (a pinned frame is never
  a victim) then holds by that compare-and-swap instead of by the latch.
- **`IsAllocated` leaves the hit path**, because nothing frees a page
  (`page.md:130`). It stays on the miss and create paths.
- **This re-opens AM-S2's decision** that pin accounting lives under the
  structure latch (`device_page_store.hpp:801-805`). That is BA-Q5.

**BA-R6 — the visibility window is read without the latch (P2).**

- **The write side.** Commits publish into per-core commit tables. Each table
  has one writer and is read atomically by every core: the shape the
  in-flight tables already have (AX, `rules.md` §3).
- **The read side.** `LookupCommit` finds the writer's core through a
  block-to-core map, which the carve records and which does not exist
  today. A trx id names its block, and every block is one core's.
- **The reclaim.** It must keep AN-R12's guarantee: a reader sees wholly
  before or wholly after a reclaim pass (`instance_visibility.cpp:276-279`).
  The reader checks this by reading the floor before and after the lookup,
  and retries when the two differ.
- **The commit side** may keep a latch among commits. What leaves is the
  read side's latch, on every core count.
- **The alternative** is an open-addressed array of atomics indexed by trx
  id. It is BA-Q6's other option.

**BA-R7 — the relation borrow (P4).**

1. **No second ask (always).** A transaction that already holds a
   relation's `IX` does not ask again per row. `BorrowChain` checks the
   transaction's own table of relation borrows before the partition. No
   semantic change.
2. **A fast path for `IS`/`IX`** (only if the census measures the partition
   material after the first step).
   - Weak relation borrows go in a per-core table.
   - Each relation has a strong count, in an array hashed by relation. An
     asker of `S`, `SIX` or `X` raises it, then moves every core's fast-path
     entries for that relation into the partition.
   - The deadlock detector sees only moved entries. That suffices, because a
     fast-path holder never waits on a weak mode.
   - `txn.md` §5 is restated.
3. **The decide's release** gets AZ-R6's keyed partition only if the census
   shows the release's latch traffic material. Otherwise AZ-Q4 stands.

All three are BA-Q7.

**BA-R8 — the pk comes from an instance cursor, and the mark is logged
ahead (P5).**

- **Issuing.** Each relation's issue cursor is an atomic in an
  instance-wide table, which is a new `rules.md` §3 row, spec first. Issuing
  an id is a `fetch_add` while the cursor is below the persisted ceiling.
- **Raising the ceiling.** The issuer that reaches the ceiling raises it by
  `kRowIdLogAhead` (32, PostgreSQL's `SEQ_LOG_VALS`), under page 7 and the
  WAL append. It does so before any id above the old ceiling is placed.
  - So K1 holds across a crash, and a crash burns at most 32 ids per
    relation, which is K3's *"bump-ahead recovery"*.
- **Named keys.** A named key at or above the cursor raises the cursor by
  compare-and-swap, and raises the ceiling too if it must. On a heap
  relation, the below-mark refusal compares against the cursor, not the
  ceiling.
- **What `next_id` means.** On disk it becomes the ceiling for an omitted key,
  which is what it already is for a named one. `DESCRIBE` and `SHOW BUDGET`
  read the cursor instead.
- **Invariant 11's text changes.** *"Every core bumps the one mark under its
  page latch"* becomes *"every core issues from the one cursor; the mark is
  raised under its page latch"*. That is a hard-invariant edit in
  `CLAUDE.md`, and the operator's (BA-Q8).
- **The hook moves.** The `before_mark` hook's two partition latches move out
  from under page 7 (§1.5).

**BA-R9 — the WAL append's I/O moves out from under the latch (P6).**

The first half applies whenever P6 is material:

- **The next segment is ready before it is needed.** The writer thread
  creates it ahead of need, so a roll under the latch only swaps in a
  prepared segment.
- **The flush writes outside the latch.** It stages into a second buffer;
  the latch swaps the buffers, and the `pwrite` runs outside it. A flush
  sequence keeps the durable watermark from ever passing a gap.
- **Less work under the latch.** The part of the CRC that does not depend on
  the LSN is computed before the latch is taken.

The second half is the reserve/copy/publish split that AL-R1 abandoned
(BA-Q9). Once the first half is built, nothing rolls under a reservation, and
an oversized record is refused before it reserves. What remains is a full
ring, which waits for space instead of unwinding. Whether to build the
second half is decided from the census's stream-latch numbers after the
first half lands.

**BA-R10 — the periodic stalls leave the reactor (P12).**

- **The carve persists less.** It persists the superblock page alone: page
  0's write, then a data-file `fdatasync`. That replaces `store_->Sync()` of
  the whole pool.
  - The stage must prove the anchor's order still holds. A checkpoint writes
    the pages it vouches for before it raises the anchor in memory, and the
    `fdatasync` covers every write issued before it.
- **The carve runs early.** The next block is carved when the current one is
  three-quarters used, in the system group. So `Begin` waits only when a
  block runs dry.
- **The checkpoint runs in slices:** a bounded number of pages per reactor
  iteration, which is what `checkpointer.hpp:32-36` already claims. The
  anchor is published after the last slice.

**BA-R11 — the rightmost leaf (P9).**

- **A survey first, inside the stage.** For each step run under the leaf's
  hold, record whether it needs the hold, and why: the Cabin witness, index
  maintenance, the assertion reservation and the undo append. AT-S21 already
  argued the append. A step that can run before the descent moves there.
- **A structural refusal becomes a wait.** Past `kMaxDescentRestarts`, the
  descent retries with nothing held until the lock family's fault net
  (`lock_wait_fault_net_ms`, 1 s). Only then does it refuse, as today.
- **A root grown over re-descends.** `SecureParents` re-descends from the root
  instead of refusing.
- **The result:** the client stops seeing `TXN_CONFLICT` for a race that it
  did not cause and cannot avoid by retrying.
- **Not proposed: B-link move-right.** It needs a high key per node, which is
  a node-format change and a superblock bump that refuses every older volume
  (D14). This is BA-Q10.

**BA-R12 — a statement yields at its walk boundary (P10).**

- **The mechanism.** `DispatchAsync` awaits the step chain instead of
  running it to completion. The bottom of the page loop holds no pin and no
  span; there the walk suspends every 64 pages and checks a cancel flag.
  `C_CANCEL` gains a handler.
- **What stays held.** The borrow and the read view stay held across a
  suspension. They are statement-scoped, and neither holds a page.
- **What the stage must prove.** A walk that resumes after a split moved its
  next page neither misses nor repeats a row. Other cores can already split
  pages between two of a walk's pages, so the stage reads how the walk finds
  its next page today. The yield adds same-core writers to a hazard that
  already exists for other cores' writers.

This is BA-Q11.

**BA-R13 — placement by load (P11), only if material.**

- **The change.** Core 0's accept-and-hand-off path (`connection_handoff.hpp`,
  D19's fallback) picks the core with the fewest live sessions instead of
  round-robin, and becomes the default above one core. `SO_REUSEPORT` stays
  as the other mode.
- **The cost.** One hop per connection, not per statement. A session still
  never moves after adoption.
- **Not proposed.** Migrating a session, and a kernel BPF selector. This is
  BA-Q12.

## 3. Stages

No stage starts before its own word. A fix stage also needs the census to
mark its item material (BA-Q1). The exemptions are BA-S1, a defect, and
BA-S15, a spec promise the code breaks.

| stage | what | exit | size |
|---|---|---|---|
| BA-S0 | This order, its index row and the bug entry | the files at the commit | S |
| BA-S1 | **The peer-thread sync** (BA-R1) | red first on the two-core rig: an owning-thread assertion in `WalManager::Sync()` fails today. Green: a peer's checkpoint flush and its carve wait on the writer. `wal.md` §3 and `manager.hpp:29-31` true as written. Mutation: the per-core gate replaced by core 0's, killed. The bug entry deleted | S |
| BA-S2 | **The counters** (BA-R0) | each counter proved by a cell on the two-core rig that forces its contention. `cores = 1` unchanged in behaviour. The new `SHOW META` fields in `manual/` | M |
| BA-S3 | **The driver and its PostgreSQL twin** (BA-R0), a tools stage | `--help` documents the shapes, the pinning and the client-bound mark. A dry run at `cores = 1`, with no number claimed | M |
| BA-S4 | **The census** (BA-R0) | `bench/v3.0.0/results-ba-s4-census-<describe>.md`: scaling curves for KDS and PostgreSQL per shape; each item's share and its material or immaterial reading (BA-Q1); BA-R4's premise, the hand-off's p50 and p99 | M |
| BA-S5 | **Statistics folded per core** (BA-R2) | `SHOW ACCESS` on the same core shows its own statement. A cell on two cores shows the other core's count after one cadence. The page-11 and collector waits at 0 in the census shape. CC13 restated | S |
| BA-S6 | **The assertion directory** (BA-R3) | a commit with no reservation takes no latch (counter at 0). A relation without assertions writes beside one with them. `CREATE ASSERTION` racing a writer keeps the AT-S5e cells green. Mutation: the reserved flag ignored, killed | S |
| BA-S7 | **Commit durability** (BA-R4) | the premise re-measured first. Two `strict` commits staged in one pass on core 0 take one `fdatasync`. The spin counter at 0. A sync of an old log covers only its open segments. Mutation: the kick removed, with the cell asserting the latency bound, not just liveness. `wal.md` §1 and §3 restated | M |
| BA-S8 | **The frame table** (BA-R5) | the store's suites green and `cores = 1` byte-identical. A sweep racing a fetch of the same page, forced by a barrier and repeated. Mutation: the compare-and-swap made a plain store, killed. `page.md` §6 and `eviction.md` restated (§1.13) | L |
| BA-S9 | **The visibility window** (BA-R6) | AN's visibility cells green. A two-core cell interleaving a reclaim with a lookup of a winner reclaimed in that pass answers visible. Mutation: the floor re-read removed, killed | M |
| BA-S10 | **The relation borrow** (BA-R7) | (a): an N-row insert takes the partition once, not N times. (b), if marked: the fast-path and strong-ask race forced and repeated, and a DDL `X` waiting on a fast-path `IX` held on another core. The lock suites green | M, L with (b) |
| BA-S11 | **The pk cursor** (BA-R8) | the Keystone suites green. A sim crash between a ceiling raise and the placement reissues no id and burns at most 32. Two cores issuing into one relation give one ascending sequence. A named key inside [cursor, ceiling) is never issued. Mutation: the ceiling logged after the placement, killed | M |
| BA-S12 | **The WAL append** (BA-R9, first half) | sim crash cells across a prepared roll and across a buffer swap. The stream latch's wait in the census shape. The second half only on BA-Q9's mark | M |
| BA-S13 | **The periodic stalls** (BA-R10) | the stall counters under 10 ms in the census shape. A crash between a carve's persist and its first issue reissues no trx id. A crash mid-slices replays from the previous anchor | M |
| BA-S14 | **The rightmost leaf** (BA-R11) | the survey's table, a reason per step. Scenario 0's `c8-s` cell with no refusal. A forced root grown over re-descends. Mutation: the refusal restored, killed | M |
| BA-S15 | **The walk-boundary yield** (BA-R12) | `sched.md:42` true. A point read sharing a core with a long walk is bounded by one slice. A split between two slices misses and repeats no row. Cancel cells | M |
| BA-S16 | **Placement by load** (BA-R13) | 8 connections at `cores = 4` land 2 per core. The census's per-core session counts | S |
| BA-S17 | **BA's close** | a row per stage. The census re-run at the closing commit against BA-S4's. The overhead A/B at `cores = 1` over the whole change (`raft-marks-2026-09-30.md` §8). What BA carries | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| BA-Q0 | **The letter and the scope**: one letter, twelve items, and the not-planned list of §0 | scope | Yes. BA is the next free letter; AQ and AR are held for AR1 (`raft-marks-2026-10-02.md` §4) |
| BA-Q1 | **The premise gate**: no fix stage before the census, and only for a material item (5 % contended wait in any cell, or any refusal). BA-S1 and BA-S15 are exempt | process | Yes. It is `CLAUDE.md`'s *"re-measure a premise before building the fix"*, applied per item |
| BA-Q2 | **The host and method**: this host, server on CPUs 0–3 and clients on 4–7, so `cores` ≤ 4, with multi-process clients and the client-bound mark; or a second host for the clients | method | This host, with its limit stated in every results file |
| BA-Q3 | **Commit syncs leave core 0's reactor, and D1 shares a sync** (BA-R4) | user-visible | Yes, once the census has re-measured the hand-off's p99. D1's contract (durable on return, no batch window) does not change |
| BA-Q4 | **Statistics freshness**: another core's counts arrive up to one cadence (100 ms) late, and CC13's sentence is struck (BA-R2) | user-visible | Yes |
| BA-Q5 | **AM-S2's pin decision re-opened**: a partitioned frame table with atomic pins (BA-R5) | architecture | Yes |
| BA-Q6 | **The window's read side**: (a) per-core commit tables through the block-to-core map, or (b) an open-addressed atomic array (BA-R6) | architecture | (a), the in-flight tables' shape |
| BA-Q7 | **The relation borrow**: (a) always; (b) the fast path only if material after (a); (c) AZ-Q4 re-opened only if the decide's release is material (BA-R7) | architecture | As stated |
| BA-Q8 | **The pk cursor**: invariant 11's text and the meaning of `next_id`; `kRowIdLogAhead` = 32 (BA-R8) | invariant | Yes. K3 already licenses the burned ids |
| BA-Q9 | **The WAL split** that AL-R1 abandoned (BA-R9's second half) | architecture | Not now; decide on the first half's numbers |
| BA-Q10 | **Structural refusals become waits, with no move-right** (BA-R11) | user-visible | Yes |
| BA-Q11 | **A yield every 64 pages, and a `C_CANCEL` handler** (BA-R12) | user-visible | Yes, with 64 re-measured in the stage |
| BA-Q12 | **Placement**: least-loaded hand-off as the default above one core, only if material (BA-R13) | user-visible | As stated |
| BA-Q13 | **Waits stay blocking**: no suspending latch primitive (§1.0); every fix holds less, less often | architecture | Yes |

## 5. Sequencing

1. **BA-S1 first**, independently: it is a defect.
2. **Then BA-S2, BA-S3 and BA-S4, in that order.** No fix stage comes before
   BA-S4 (BA-Q1). BA-S15 is the exception.
3. **Then the fix stages, in the order the census ranks them.** CLA's prior,
   from expected share and how isolated each change is: S5, S6, S7, S8, S9,
   S10, S11, S12, S13, S14, S16.
4. **File overlaps force these orders:**
   - S1 before S7: both touch `wal/manager.cpp` and the store's gate.
   - S7 before S12: `stream.cpp` and `manager.cpp`.
   - S8 before S13: both touch the store's writeback paths.
   - S11 before S14: both touch the insert path in `command_dispatcher.cpp`.
   - S15 after S10: the read borrow's scope across a suspension.
5. **BA-S17 last.** The overhead is measured once, over the whole change,
   from the commit BA opens at to the commit that closes it.

## 6. Row status

### BA-S0 — written 2026-10-02

On `worktree-parallelism-workorder` from `d3d90b5`, this stage writes:

- this file;
- its index row;
- the bug entry
  `docs/inflight/bugs/a-peers-writeback-runs-core-0s-wal-sync-on-the-peers-thread.md`.

§1 was read against `d3d90b5`, not run. The PostgreSQL citations were read
from the 18.6 release tarball. No code, spec or test is changed. The letter
belongs to BA-Q0.
