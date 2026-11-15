# Work order CS - `BEGIN READ ONLY`: a transaction that issues no id, logs nothing and is a held snapshot

Status: **OPENED 2026-11-15** on the operator's standing go-ahead of
2026-11-15 (go-ahead-achieving-milestone, argument
`cs-read-only-transaction`), on worktree `cs-read-only-transaction` from
`32354dbe` (`v3.4.11-1-g32354dbe`). **CS-Q0..Q9 are adopted as CLA
proposed** under that word (`raft-marks-2026-11-15.md` §8); none is the
operator's own per-item mark. Written on `main` from `030b5bd9`
(`v3.4.10-1-g030b5bd9`) on the operator's word of 2026-11-15
(`raft-marks-2026-11-15.md` §4), and carried onto `61917b0f`
(`v3.4.10-6-g61917b0f`, F4 landed) before its commit. Every
`[source-read]` claim below was read at `030b5bd9`. F4 changed none of
the code read, and the line numbers of `src/server/command_dispatcher.cpp`
and `docs/spec/txn.md` are given at `61917b0f`, where F4 moved them.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` marks an item where a wrong choice
turns a refusal into a wrong answer.

Related: `docs/spec/txn.md` §1, §4.1, §9; `docs/spec/protocol.md` §9
(`C_TXN_BEGIN`); `docs/spec/sched.md` (BL's moves, the reader-lease
slots); `instructions/v3.0.0/workorder-an-read-view.md` (the commit-LSN
snapshot this order builds on); `instructions/v3.0.0/workorder-ax-inflight-publication.md`
(the in-flight table a read-only transaction stays out of).

CS was checked against every file name under `instructions/`, `docs/`,
`manual/` and `CLAUDE.md`, and every commit subject reachable from the 34
remote branches, for a `CS-` token or a `workorder-cs` name at `030b5bd9`, and again at `61917b0f`:
no use found. The last letter assigned is CR
(`workorder-cr-read-committed-rerun.md`).

## 0. What CS is

### 0.1 Subject

`[operator]` Oracle's `SET TRANSACTION READ ONLY` read against this
engine, item 1 of CLA's survey of 2026-11-15: a transaction declared
read-only before it starts, which writes nothing and is therefore owed
none of a writer's machinery.

`[design]` The survey found that the half of Oracle's design that matters
most is already built. A snapshot in this engine is one number, the commit
LSN ceiling (§1.1), which is the role Oracle's SCN plays. What is not built
is the other half: today an explicit transaction is a writer from its
`BEGIN`, whether or not it ever writes (§1.2). CS adds
`BEGIN ... READ ONLY`. Under it the transaction is a read view and a
reader lease held across statements: no id is issued, no record is
appended, no in-flight slot is taken, and `COMMIT` waits on no durability
point. Every write statement inside it is refused.

### 0.2 Not in CS

- **Lazy id assignment** for an undeclared transaction, where the id is
  drawn at the first write (PostgreSQL's and InnoDB's model). It would give
  every read-only explicit transaction CS's savings without the keyword,
  but it touches every write path's reading of `Transaction::id()` and is
  `[quiet-wrong]` wherever a path reads the id before it is drawn
  (`kNoTrxId` is `0`, `include/kds/txn/read_view.hpp:54`). CS-S0 records
  the census such an order would need (CS-Q9). It is not built here.
- **A retention bound on old snapshots** (`SnapshotTooOld`). The engine has
  none by design (`include/kds/txn/manager.hpp:38`), and a long `READ ONLY`
  transaction holds the horizon exactly as a long `READ WRITE` one does
  today. CS changes nothing there.
- **A transaction that crosses cores.** A read-only transaction could in
  principle move with its session between statements, since its snapshot
  is one number. `sched.md` says "a transaction never crosses"
  (`docs/spec/sched.md:94`), and CS keeps that rule.
- **Reads on a standby.** This is the HA order's, and CS is its
  prerequisite on the primary only.
- **`AS OF` (flashback) reads.** They are a separate order on the same
  mechanism.
- **Commit-time cleanout** of `LookupCommit`'s latch
  (`src/txn/instance_visibility.cpp:322`). It is a separate performance
  matter, and CS adds no lookup.

## 1. The tree as read `[source-read]` at `030b5bd9`

### 1.1 The snapshot is already one number

- `TransactionManager::MintCheckView` sets
  `view.snapshot_lsn = visibility_->SnapshotCeiling()`, "one load, and the
  whole of what a snapshot is" (`src/txn/manager.cpp:92`).
- `InstanceVisibility::SnapshotCeiling` reads `commit_ceiling_` and then
  caps it by each attached core's `pending_commit_bound`. That costs one
  atomic load per core and copies no list of active transactions
  (`src/txn/instance_visibility.cpp:136-158`).
- `PublishCommit` enters `trx_id -> commit_lsn` in the window and raises
  the ceiling, under the window latch, in AN-R9's order
  (`src/txn/instance_visibility.cpp:290-320`).
- `ReadView::Visible` answers `commit_lsn <= snapshot_lsn` through
  `LookupCommit` (`include/kds/txn/read_view.hpp:106-114`).

The mechanism CS needs to read is therefore built and needs no change.

### 1.2 An explicit transaction is a writer from `BEGIN`

`TransactionManager::Begin` (`src/txn/manager.cpp:125-166`), in order:

1. Refuses `ResourceExhausted` when the core's in-flight table is full
   (`:131`). The table holds `kInFlightSlotsPerCore = 1024`
   (`include/kds/txn/instance_visibility.hpp:289`).
2. Issues an id from the carve: `ids_.Next()` (`:137`).
3. Mints the view (`:144`).
4. Appends `TXN_BEGIN` (`:147`).
5. Publishes the id as in flight to every core (`:162`).
6. Publishes the core's bounds (`:165`).

`CommandDispatcher::CommitLocal` calls `TransactionManager::Commit` for
every explicit transaction, whether or not it wrote anything
(`src/server/command_dispatcher.cpp:11043-11055`). `Commit` sets a pending
commit marker that caps every mint on the instance (`src/txn/manager.cpp:384`),
then calls `WalManager::Commit` (`:386`). That call appends `TXN_COMMIT`
and, under `strict`, syncs the device before it returns
(`src/wal/manager.cpp:425-440`).

The consequence `[design]`, unmeasured until CS-S0: a client that sends
`BEGIN; SELECT ...; SELECT ...; COMMIT` to get one consistent snapshot pays
an id, two records, a slot in a 1024-entry table and, under `strict`, one
device sync, for a transaction that changed nothing.

### 1.3 What a reader already uses, without a transaction

- **Autocommit reads take no id.** `AutocommitSnapshot` mints with
  `kNoTrxId` and registers the view under a `ReaderLease`
  (`include/kds/txn/manager.hpp:863-873`). `RegisterReader` stores the
  snapshot in one of `kMaxRegisteredReaders = 256` slots per core and
  republishes the core's bounds (`src/txn/manager.cpp:756-775`).
  `LocalSnapshotBound` folds the lease slots with the live transactions'
  views (`:656-672`), so a lease holds the purge horizon exactly as a
  transaction's view does.
- **A read's lock is not the transaction's.** Every read statement takes a
  `ReadBorrow` under a synthetic holder from `NextReadHolder()`, never the
  transaction id (for example `src/server/command_dispatcher.cpp:6264`,
  `:10251`, `:12496`). The lock table tells the two apart (`:329`, "a read
  borrow's id names no transaction").
- **Isolation differs in one branch.** `StartStatement` re-mints under
  `READ COMMITTED` and returns early under `REPEATABLE READ`
  (`src/txn/manager.cpp:169-186`).
- **The 256 lease slots are shared** with every suspended `SELECT` walk on
  the core. The 257th is refused `OutOfSpace` at its head
  (`docs/spec/sched.md:43`).

`[design]` A read-only transaction is therefore an autocommit reader's
lease held past one statement: kept under `REPEATABLE READ`, re-pointed at
each statement boundary under `READ COMMITTED`. The parts it needs exist.

### 1.4 The surfaces CS touches

- **The `BEGIN` text grammar** is parsed in the dispatcher, not the parser:
  `BEGIN [TRANSACTION|WORK] [ISOLATION LEVEL <name>] [DURABILITY <class>]`,
  with the clauses order-free (`src/server/command_dispatcher.cpp:10958-11000`).
  The reply carries `trx_id=` (`:11020`).
- **KWP's frame** is `C_TXN_BEGIN {durability u8}` (`docs/spec/protocol.md:126`)
  and has no field for an access mode.
- **The role gate** classes `BEGIN` and `START` at the read-only floor
  (`src/server/command_dispatcher.cpp:1298`). A `readonly` connection may
  therefore open an explicit transaction today, and every such transaction
  draws an id and logs two records.
- **`txn.md` §9** lists the open decisions of the subsystem, and an access
  mode is not among them (`docs/spec/txn.md:1194-1210`).

## 2. The mechanism `[design]`

**The session's transaction is one of two kinds.** `Session` holds today's
`txn::Transaction*` for `READ WRITE`. For `READ ONLY` it holds a
`ReadOnlyTxn`, which is a value of the dispatcher's: a `ReadView` minted
with `kNoTrxId`, the isolation level, and the `ReaderLease` that registers
the view. It has no id, no trail, no borrows and no entry in `live_`.
`in_explicit_txn()` answers true for both kinds. A new
`in_read_only_txn()` tells them apart.

**`BEGIN ... READ ONLY`** runs `AutocommitSnapshot`'s two steps,
`MintReadView(kNoTrxId)` and then `RegisterReader`, and stores the result
on the session. It appends no record and draws no id. The only refusal it
can meet is the lease registry's `OutOfSpace`.

**The statement boundary.** Under `REPEATABLE READ` nothing happens.
Under `READ COMMITTED` the view is minted again and the lease's slot is
re-pointed at the new snapshot. Re-pointing goes through a new
`TransactionManager::RefreshReader(lease, view)`, which lowers the slot
before the ceiling is read, as `MintReadView` does
(`src/txn/manager.cpp:108-122`), and republishes the bounds. Dropping the
lease and registering a fresh one would leave a window in which the core
announces no reader for this session. That window is harmless only by
AN-R1's argument, and CS does not lean on it twice (CS-R3).

**`SnapshotFor` and `MintResolveView`** read the session's view for both
kinds. A `READ ONLY` transaction never reaches `BeginWrite`.

**A write is refused at the statement head**, before any borrow, any
catalog read under the write path, and any spill. The refusal is a new
`Status::ReadOnlyTransaction` carrying the statement's first byte, and its
SQLSTATE is `25006` where the wire maps one. Refused at the head:
`INSERT`, `UPDATE`, `DELETE`, `ON CONFLICT`, every DDL spelling, every
transactionless catalog write (`CREATE PATTERN`, `WAYSTONE ON|OFF`,
renames), `PURGE`, and every route CS-S0's census lists.

**`COMMIT` and `ROLLBACK`** drop the lease and clear the session. Neither
appends anything or waits for anything. `COMMIT`'s reply carries no
`RELAXED` flag, because nothing was acknowledged at any class (CS-Q6).

**What a read-only transaction is invisible to**: the in-flight table, the
floor's candidate (`FloorCandidate` reads issue cursors and oldest
unresolved ids, `src/txn/instance_visibility.cpp:180-195`), the checkpoint's
active list (`TransactionManager::Snapshot`), recovery, the lock family's
wait-for graph, and every `sys.*` row keyed by transaction id. What it is
visible to: the horizon, through its lease, and nothing else.

## 3. Rules

- **CS-R1. A `READ ONLY` transaction writes nothing, anywhere**
  `[quiet-wrong]`. No row, index entry, spill, catalog row, assertion
  reservation, WAL record or id. Each refusal happens before the first
  byte of any of these. A write that reached a page under `kNoTrxId`
  would be stamped with id `0` and answered by every view as that id
  answers. CS-S0's census names that answer, and a test pins the refusal
  of every write route the census finds.
- **CS-R2. The horizon covers every snapshot a read-only transaction
  holds** `[quiet-wrong]`, from the mint to the lease's release, across
  every statement boundary. A purge that frees a version such a view can
  still reach returns a wrong row, not an error. A test holds a
  `REPEATABLE READ` read-only transaction across an update and a purge on
  another core, and reads the old version.
- **CS-R3. No window without a registration.** `RefreshReader` lowers
  before it reads, as `MintReadView` does. No path drops the lease and
  registers again.
- **CS-R4. `READ WRITE` is untouched**: the same id, the same records, the
  same reply. The default is `READ WRITE` everywhere CS-Q3 does not say
  otherwise. A test pins the reply text byte for byte.
- **CS-R5. Off the read-only path the cost is nil**: an autocommit
  statement and a `READ WRITE` transaction execute no new instruction
  beyond one test of the session's kind at `SnapshotFor` and at the write
  head. This is measured at the close. The rule also holds at one core,
  where nothing new is built (the standing zero-overhead rule).
- **CS-R6. The lease registry is the only cap.** A read-only transaction
  holds one of the core's 256 slots for its whole life, the time its
  client spends idle included. CS-Q5 decides whether that is enough.
- **CS-R7. The spec moves with the code**: `txn.md` §1 (the access mode
  beside the levels), §4.1 (a view with no owning transaction held across
  statements), §9; `protocol.md` §9 per CS-Q4; `sched.md`'s lease-slot
  sentence; `manual/sql/sql.md`'s `BEGIN`; `manual/client/client.md` for
  the guidance a client needs. All of these move in the stage that lands
  the behaviour, not before.
- The standing rules: each stage reviewed, the full suite per stage, the
  overhead measured once at the close in `build-release`, a stage's
  measurement inside 20 minutes under a 60 s watchdog, results named by
  `git describe --tags`, nothing pushed or merged without the operator's
  word.

## 4. Items for the operator (CS-Q), CLA's proposals

Each is **adopted on the operator's standing go-ahead of 2026-11-15
(go-ahead-achieving-milestone)**, as CLA proposed; the mark column says
so. None is the operator's own per-item mark.

| | item | options | CLA proposes | mark |
|---|---|---|---|---|
| CS-Q0 | The shape | (a) the declared mode only, as §2 builds it. (b) lazy id assignment only, no keyword. (c) both in this order | (a). (b) is the larger saving but the larger `[quiet-wrong]` surface (§0.2), and the declaration is also a contract a client can rely on. CS-S0 records (b)'s census for a later order (CS-Q9) | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q1 | Does CS-S0 gate the build? | (a) yes, with a bar: if `BEGIN READ WRITE; <k reads>; COMMIT` against the same reads in autocommit differs by less than the spread at every durability class and every `k` S0 runs, CS stops and returns to the operator. (b) built whatever S0 shows | (a). No structure without a measured premise | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q2 | The spelling | (a) `BEGIN ... READ ONLY` / `READ WRITE` as one more order-free clause, and `START TRANSACTION` the same. (b) (a) and `SET TRANSACTION READ ONLY` as the first statement of a transaction. (c) (a) and a session default, `SET TRANSACTION_MODE read only` | (a). One spelling per rung, the same shape as `ISOLATION LEVEL` and `DURABILITY`. (c) is a fourth rung with no consumer yet | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q3 | The `readonly` role | (a) a `readonly` connection's `BEGIN` is `READ ONLY` unless it says `READ WRITE`, and `READ WRITE` is refused for the role. (b) unchanged: the role's `BEGIN` stays `READ WRITE`, and its writes are refused by the role gate as today | (a). Such a connection cannot write, so its transactions only spend ids and records. The reply text changes for it, and that is its only visible change | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q4 | KWP | (a) text only in CS: a KWP client sends `BEGIN READ ONLY` as a statement, and `C_TXN_BEGIN` is unchanged. (b) `C_TXN_BEGIN` gains an access-mode byte under a capability bit | (a). A frame change is a protocol revision with its own capability, and no client asks for one yet | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q5 | The cap | (a) the shared 256-slot registry, as CS-R6 states, with the `OutOfSpace` message naming read-only transactions as a holder. (b) a separate registry for read-only transactions, sized by measurement | (a), until a measurement shows held transactions starving suspended walks | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q6 | The replies | (a) `BEGIN trx_id=none isolation=<l> access=read only`, `COMMIT` replies as today without the `RELAXED` flag, and `READ WRITE`'s reply unchanged. (b) `READ WRITE`'s reply gains `access=read write` too | (a). CS-R4 | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q7 | A refused write | (a) refused, and the transaction is not poisoned: nothing was written and the view is unchanged. (b) refused and poisoned, as every other failed statement in an explicit transaction is | (a). Poisoning protects writes a client might commit unknowingly, and a read-only transaction has none | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q8 | What is reported | (a) `read_only_begins` and `read_only_write_refusals` in the transaction counters, and no wait event, since CS adds no wait. (b) nothing new | (a) | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q9 | The lazy-id census | (a) CS-S0 lists every reading of `Transaction::id()` and every `kNoTrxId` comparison on a write path, in a `docs/pending/` entry for a later order. (b) not in CS | (a). It is a read with no code, and the census CS-R1 needs covers half of it | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q10 | A `DURABILITY` clause on a `READ ONLY` `BEGIN` (surfaced at CS-S1, written here before adoption) | (a) accepted, binding nothing: a commit that acknowledges no write keeps every class's promise, and the reply names no class. (b) refused `InvalidArgument`: the clause has nothing to bind | (a). KWP's `C_TXN_BEGIN` always sends `BEGIN [DURABILITY <c>]`, and under CS-Q3 (a) a `readonly` connection's frame `BEGIN` is `READ ONLY`, so (b) would refuse every such client that names a class; and (a) is not a spelling enforced as something else - no class is owed on a commit with nothing to make durable | **(a)**, adopted on the standing go-ahead of 2026-11-15 |
| CS-Q11 | What a read inside a `READ ONLY` transaction records `[quiet-wrong]` (surfaced by CS-S0's census, written here before adoption) | (a) what an autocommit read records, unchanged: the unlogged `sys.access_stats` fold (`Catalog::RecordAccess`, written at `kBootstrapXid` with no log), an opted pattern's Waystone trail, the in-memory statistics, and a Cabin bank under the bank's own gate. (b) suppress all of it inside a `READ ONLY` transaction | (a). None is the transaction's write: each is advisory or unlogged state every autocommit read already writes outside any transaction, and CS-R1's ground - user data stamped `kNoTrxId` - does not arise. The `[quiet-wrong]` half is the Cabin bank, which a `READ WRITE` view never reaches (`own_trx_id != 0` declines it, `step_vm.cpp:937-940`) and a `READ ONLY` view now does. **Premise re-read at `bbaff33a`**: the bank declines a view minted with anything in flight (`in_flight_at_mint`) and, after its announce, any view whose `CommitCeiling()` is not its own `snapshot_lsn` (`step_vm.cpp:994-1000`) - so a `REPEATABLE READ` view held past a commit is declined, and a view that passes both is indistinguishable from an autocommit view minted at the same instant. The premise holds | **(a)**, adopted on the standing go-ahead of 2026-11-15 |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **CS-S0** | **The premise and the census.** Measured: `BEGIN; <k> tuple key SELECTs; COMMIT` against the same `k` statements in autocommit, `k` in {1, 4, 16}, under `strict`, `group` and `relaxed`, one connection and eight, `cores = 1` and `cores = 4`: latency p50/p99, throughput, WAL bytes and ids spent per transaction. The census: every write route a statement head can reach (CS-R1), every site that reads the session's `Transaction*` on a read path, every `sys.*` view and ASH field that names a transaction id, what a write stamped `kNoTrxId` would read as, and CS-Q9's list. No behaviour change, and no engine code on the branch | a results file under `bench/v3.4.10/`, the census in §7, CS-Q1's bar read |
| **CS-S1** | **The mode.** `ReadOnlyTxn`, the `BEGIN` clause per CS-Q2, `SnapshotFor`/`MintResolveView` for both kinds, `RefreshReader`, `COMMIT`/`ROLLBACK`, CS-Q3. Tests: no id drawn, no record appended, no in-flight slot taken (each asserted on its counter); the `REPEATABLE READ` view held across another core's commit; the `READ COMMITTED` view advancing at each statement; CS-R2's purge cell; the 257th holder refused; `READ WRITE`'s reply byte for byte; a sim cell with read-only transactions mixed into the workload | `txn.md` §1, §4.1, §9; the manual's `BEGIN` |
| **CS-S2** | **The refusals.** `Status::ReadOnlyTransaction` at every head the census lists, CS-Q7's poison rule, CS-Q8's counters. Tests: one per write route, each asserting no page dirtied and no WAL byte appended; a refused write followed by a read that still sees the held snapshot | `sys-views.md` for the counters; the client manual |
| **CS-S3** | **The close.** The overhead A/B over the whole change (CS-R5), S0's arms measured again, the known gaps, the report | the operator's word on landing |

## 6. What CS does not do

- No change to what a view sees, to `Visible`, to the window or to the
  ceiling.
- No change to `READ WRITE`: its id, its records, its reply.
- No retention bound, no `SnapshotTooOld`.
- No cross-core read-only transaction, no standby reads, no `AS OF`.
- No lazy id for an undeclared transaction (CS-Q9 records its census
  only).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-15

On `main` from `030b5bd9`, on the operator's word. No stage is opened and
no code is written. Documents only.

### CS-S0 - the premise and the census, 2026-11-15

On worktree `cs-read-only-transaction`. **The bar of CS-Q1 (a) is met, so CS
continues.** Measured at `bbaff33a` (`v3.4.11-3-gbbaff33a`, no engine change
from `32354dbe`), Release from a `git archive` export:
`bench/v3.4.11/results-cs-s0-premise-v3.4.11-3-gbbaff33a.md` (committed at
`059d6b42`). `BEGIN; <k> tuple-key SELECTs; COMMIT` against the same reads in
autocommit is costlier beyond the run's A/A spread in 23 of 36 cells: every
durability class has such cells (`strict` 10 of 12, `group` 8, `relaxed` 5),
and by `k` 11 of 12 at `k = 1`, 9 at `k = 4`, 3 at `k = 16`. Throughput falls
81-91 % at `strict`, `k = 1`. The explicit arm appends 64 WAL bytes per
transaction in every cell, the autocommit arm 0. It spends one id per
transaction at `cores = 1`, the autocommit arm none. The probe could not
count ids at `cores = 4`, where each core carves its own window. **What
bounds the reading**: the host was loaded (load average 11-21, other
sessions compiling), so the spread is wide (median about 30 %). The
explicit arm's two extra round trips are not separated from the engine's
share, and under `relaxed` at `k = 1` they may be most of it. The id, the
64 bytes and the `strict` sync are the engine's share. Cut to the stage's
budget: one table size (10,000 rows), 2 repeats of 2 s per arm, no wait
decomposition, no PostgreSQL floor.

**The census** (an `Explore` read at `bbaff33a`; line numbers there):

- **One chokepoint.** Every client statement, newline and KWP alike, passes
  through `CommandDispatcher::DispatchInner` (`:1349`). KWP's
  `C_TXN_*` and `C_EXECUTE` become statement text there (`kwp_session.cpp:650-693`, `:986-1038`).
  The one bypass is the KWP load stream, which enters `ExecuteInsert`
  (`:6116`) on a session of its own whose role is `admin`, so under
  CS-Q3 (a) it is never `READ ONLY`.
- **Write routes.** Every DML and `InDdlStatement` DDL reaches
  `BeginWrite` (`:11317`), which at HEAD dereferences a null transaction
  for a session in a transaction with none (`:11331`); `CommitLocal` and
  `RollbackLocal` do the same. Five transactionless write routes never call
  `BeginWrite` and would have written: column-shape `CREATE`/`DROP CABIN`
  (`:3217`), `ALTER ... RENAME` (`:3282-3421`, drawing an id through
  `TakeDdlBuildLock`), `DROP ASSERTION` (`:4014`), `CREATE ASSERTION`
  (`:4057`) and `CREATE`/`DROP TRANSITION` (`:4101`). All of them are
  routed under `CREATE`, `DROP` or `ALTER`, so a refusal on the routing
  token covers them. `PURGE`, `REINDEX`, `WAYSTONE` and `BACKUP` (but
  `STATUS`) already refuse inside any transaction.
- **Writes a read makes** (CS-Q11): `sys.access_stats` (`Catalog::RecordAccess`,
  unlogged, `kBootstrapXid`); an opted pattern's Waystone trail
  (`trail_store.cpp:70`, unlogged); the in-memory statistics, plan memo and
  optimizer signals; and the Cabin bank, which a `READ ONLY` view reaches.
- **Read paths.** `SnapshotFor` (`:11292`), `EnsureStatementBoundary`
  (`:7689`) and `MintResolveView` (`:7711`) are the three readers of the
  session's transaction on a read path. Every subquery, query-layer walk
  and `SelectRun` reuses the snapshot `SnapshotFor` returned and takes no
  lease of its own. So the session's lease covers a suspended walk, which
  `CloseClient` defers past (`tcp_server.cpp:1090-1094`).
- **Lifetime and placement.** The sweep keyed on `transaction()` and the
  id (`tcp_server.cpp:926-975`), so a read-only transaction would have been
  unswept. BL's placer tests `in_explicit_txn()` (`tcp_server.cpp:670`), which a
  read-only transaction answers true, so it stays on its core.
- **Transaction-id observability.** `sys.lock_waits`' `waiter_trx` reads
  NULL for a holder with no id (`sys_views.cpp:127-128`, `:317-332`). The
  wait-event parameters name the other party. ASH's `trx_id` is filled by no
  writer, and `sys.session_state`/`session_waits` carry no id. Nothing else
  changes.
- **A tuple stamped `0`** is invisible to every view, its writer's
  included, until the first floor reclaim, and visible to every view, older
  snapshots included, after it. Recovery skips its records. At HEAD the
  write path would have stamped `kBootstrapXid` (`:12075`), had it not
  crashed first. This is why CS-R1's refusal sits at the head.
- **Counters.** `sys.meta` has no transaction block. CS-Q8's two counters
  go in a new per-core `txn` section.
- **CS-Q9**: `docs/pending/lazy-id-assignment-census.md`.

Overhead not measured; measured at the milestone's close.

### CS-S1 and CS-S2 - the mode and the refusals, landed together, 2026-11-15

On worktree `cs-read-only-transaction`, one commit. **Together, not in
turn**, because the census showed the mode without the head gate is a null
dereference at `BeginWrite`, `CommitLocal` and `RollbackLocal` for any
write a read-only session sends. An S1 without S2's gate would not be a
safe intermediate tree.

- **Built**:
  - `Session::ReadOnlyTxn` (a view, the level, the lease) with
    `in_read_only_txn()`;
  - `BEGIN`'s `READ ONLY`/`READ WRITE` clause, with the isolation level now
    read by its shape, because `READ` opens both;
  - `CommandDispatcher::BeginReadOnly`;
  - `COMMIT`/`ROLLBACK` dropping the lease;
  - `EnsureStatementBoundary`, `SnapshotFor` and `MintResolveView` for both
    kinds;
  - `TransactionManager::RefreshReader` (CS-R3);
  - the readonly-role default (CS-Q3);
  - KWP's RELAXED flag cleared for a read-only transaction
    (`KwpSession::TxnIsRelaxed`, CS-Q6);
  - the lifetime sweep keyed on `Session::open_txn_seq()`, with
    `Session::expired()`, so a read-only transaction is swept;
  - `StatusCode::kReadOnlyTransaction`, wire category 19,
    `READ_ONLY_TRANSACTION retryable=0`;
  - `DispatchInner`'s allowlist gate (`ReadOnlyAdmits`) and `BeginWrite`'s
    second line;
  - CS-Q8's per-core `sys.meta` `txn` section;
  - the sim's read-only transactions (shapes on only, own stream).
- **Tests**:
  - `tests/read_only_txn_test.cpp`, 10 cells on the two-core rig;
  - `PurgeWaitTest.AnOlderReadOnlyTransactionKeepsItsRowAcrossStatementsAndThePurge`;
  - `ExpeditorTest.AReadOnlyTransactionPastItsLifetimeIsEndedAndItsNextStatementIsTold`;
  - the new code in the status, kwp-error and parser-golden lists.

  Both CS-R2 cells fail under a mutation that releases the lease at
  `BEGIN`. The sim ran seeds 1-8 and 11-14 in `clean`, `crash` and
  `sync-crash`, all `ok`, with 5-19 read-only transactions per run. The
  shapes-off stream is unchanged.
- **Review** (`critics-developer`): no correctness bug. Three comment and
  spec fixes were applied by the reviewer. A wrong claim that a write past
  the gate would stamp `kNoTrxId` now reads `kBootstrapXid` in the gate's
  comment and in `txn.md`, and a non-existent function name in
  `session.hpp` was corrected. **Applied from its list**:
  - deleted `SplitBeforeClause`, dead now;
  - `Session::expired_trx_id_` is an `optional`;
  - plain counters, read and written on one core only;
  - `SnapshotFor`'s two explicit branches merged, which also keeps the
    autocommit path free of a new branch;
  - `RefreshReader`'s silent guard turned into an `assert`;
  - KWP's RELAXED expression written once;
  - the sim counts the engine's reply rather than the SQL;
  - the expired refusal no longer tells a read-only transaction its writes
    were undone;
  - the cross-core purge cell it asked for, `ItsLeaseHoldsTheHorizonAgainstAPurgeOnAnotherCore`;
  - `SYNC`'s checkpoint record and CS-Q11 stated in `txn.md`.

  **Rejected**: sharing `TrxIdText` with `tcp_server.cpp`. It is a
  dispatcher-internal helper, and exporting it for one log line adds a
  header surface for no reader.
- **Suite**: full `ctest -LE bs-nightly`, 4474/4474.
- **Docs**: `txn.md` §1 (the access mode); `sched.md` (the lease slot);
  `protocol.md` §11 (category 19) and §14 (the readonly role);
  `sys-views.md` §7 (`txn`); `manual/sql/sql.md` §5 and §8;
  `manual/client/client.md`.

Overhead not measured; measured at the milestone's close.
