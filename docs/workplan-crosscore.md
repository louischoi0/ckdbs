# Multicore Workplan

Work items to take the engine from the single-core reactor (sched.md "Phase
1", the current state: one Expeditor, one Scheduler, `core_id = 0`
everywhere) to thread-per-core operation with cross-core execution
(`docs/crosscore.md`). Companion to `docs/sched.md`, `docs/wal.md` §3,
`docs/page.md` §6, `docs/protocol.md` D3.

Already fixed by existing specs, not revisited here: one pinned worker per
core with no thread creation after startup and no work-stealing (sched.md);
N² per-core-pair SPSC rings; per-core WAL streams, buffer pools, and
checkpoints; server-side forwarding with topology-unaware clients (D3).

Ordering note: the transaction milestone (`docs/txn.md` workplan) lands
first on the single core. Everything in txn.md is core-local by design, so
multicore adds instances, not synchronization — the reverse order would make
every txn test carry cross-core variables from day one.

## 1. Decision Record

| # | Decision | Resolution |
|---|----------|-----------|
| M1 | Ownership partition | **Relation-unit ownership recorded in the catalog** (`owner_core` on the relation row), assigned at CREATE. Write-coupled auxiliaries (unique indexes, Cabin, Waystone, var-heap) always co-located with the base relation; FK-linked relations co-located in v1 (crosscore.md §6). Page/extent hashing rejected: btree descent and heap-chain walks cannot cross cores per hop |
| M2 | Cross-core statements | **Cross-core read execution now** — step pipeline per `docs/crosscore.md` (CC1–CC6). Writes stay single-core per transaction; 2PC `[OPEN]` |
| M3 | Accept distribution | **SO_REUSEPORT per-core listeners**; the kernel distributes connections; a session lives on the core that accepted it (protocol D3). No fd handoff path. Session/data skew is observed via metrics, never rebalanced in v1 |
| M4 | trx-id allocation | **Single superblock counter, per-core block leases** requested from the system core over the ring; a crash burns each core's unissued remainder (extends txn workplan T3). Ids stay globally unique with no core bits in the format |
| M5 | Shared-resource ownership | **Core 0 is the system core**: owns the superblock (page 0), free map, file growth, extent leasing, and catalog pages. Other cores hold extent leases and catalog caches; checkpoint anchors are written via message to core 0 (`SuperBlock::SetWalAnchor(core_id, …)` already takes the id) |
| M6 | Core-count changes | `cores` config key; the count is **recorded in the superblock; a mismatch at startup refuses to boot**. Stream reassignment stays `[OPEN]` (wal.md §3) |
| M7 | Ring send failure | Sends are non-blocking and fallible (sched.md §7); on ring-full the sending **task yields and retries**. Never an error to the client, never a reactor block |
| M8 | Idle policy default | **busy-poll** default (appliance deployment, sched.md §6); epoll mode selectable via config |
| M9 | Deterministic simulation | **In scope for v1**: real SPSC rings and a simulated ring behind one seam; reactors stepped round-robin on a single thread with message delay/reorder injection. Without it every cross-core test is nondeterministic |

## 2. Phases

Each phase ends green: build + full test suite at `cores ∈ {1}` until P8
adds multi-core runs. The single-core configuration must behave identically
throughout — that regression check is part of every phase, not a final step.

### P0 — Config and superblock plumbing — **built (2026-08-04)**
- Add `cores` to `ConfigFile`/`Expeditor::Config` (default 1); validate
  against `std::thread::hardware_concurrency()` at startup.
- Superblock: record `core_count` at bootstrap; refuse to start on mismatch
  (M6). Fresh-format bump per the superblock versioning rules.
- Catalog: add `owner_core` to the relation row (sys.tables); bootstrap
  assigns all system relations to core 0. Development-stage row-format
  change is permitted (no compatibility shim, per the CREATE PATTERN
  precedent).
- Assignment policy at CREATE: `[PROPOSED]` round-robin over non-system
  cores, overridden by co-location (M1): an index/Cabin/var-heap/FK-linked
  relation inherits its base relation's core.

**As built.** `cores` is an `Expeditor::Config` key, pinned into the
superblock at bootstrap and validated at every mount, naming both numbers on
a mismatch — the arrangement `inline_cell_width` already had, and the check
lives beside it in `bootstrap.cpp`. Superblock **9 → 10** and the
`sys.tables` row grew `owner_core`; every pre-existing data file stops
mounting, which is the documented development-stage policy. Two findings
worth carrying forward:

- **`cores ≤ kMaxWalCores` is a hard ceiling, not a preference.** The
  superblock's WAL anchor table is indexed directly by `core_id` and has 64
  slots, so a core above it has nowhere to publish a checkpoint from.
  `server::CheckCoreCount()` is the single test, shared by the config
  overlay, bootstrap and `SuperBlock::Decode` so the three cannot disagree.
- **Co-location needed no encoding.** A relation's unique indexes, Cabin,
  Waystone pages and var-heap hang off its own catalog row and have no owner
  field of their own, so M1's co-location rule is structural — there is no
  way to spell a relation whose var-heap is on another core. The only
  co-location that will need expressing is FK-linked relations, and
  `docs/impl-foreign-keys.md` keeps those together in v1 by deferring
  cross-core FK entirely.

The placement policy is `catalog::AssignOwnerCore()`
(`include/kds/catalog/core_placement.hpp`), deliberately a free function
outside `Catalog`: the catalog *records* ownership and does not decide it,
so the policy can be replaced without the catalog acquiring a reason to know
how many cores exist. `DESCRIBE` carries `owner_core=`.

**Correction (2026-08-05): the round-robin was wrong and is now disabled.**
M1's `[PROPOSED]` rotation was performed from P0 onward, and it violated the
invariant placement actually has to satisfy — **a relation's owner must be
the core that allocates its pages.** DDL runs on the system core and
allocates from the system core's free map, so a relation the catalog placed
on core 1 was built entirely out of core 0's pages and no core could reach
it: core 1 may not fault them, and core 0 does not own the relation.

Nothing detected this for two phases, because no code compared the two
facts. P4's `CheckReadAffinity` is what asked, and every statement on a
two-core instance immediately failed. `AssignOwnerCore` now returns the
creating core; the rotation is written out in the header as what it becomes
once CREATE TABLE can allocate a relation's pages from its *owner's* lease.
The lesson is worth keeping: a `[PROPOSED]` policy that nothing consumes is
not inert if something else already depends on the fact it sets.

### P1 — Rings and reactor phase 3 — **built (2026-08-04)**
- SPSC ring implementation: preallocated at startup, per core pair, fixed
  max message size, indices as the only atomics (sched.md §5).
- One `RingTransport` seam with two implementations: real rings and the
  simulated transport (M9). Engine code sees only the seam.
- Message header: `(src_core, dst_core, kind, session_core, request_id,
  step_id)` + POD payload. Kinds enumerated centrally (crosscore.md §3 plus
  system kinds: anchor write, extent lease, trx-id lease, catalog
  invalidation).
- Scheduler: implement reactor phase 3 (cross-core inbox drain — currently
  an explicit no-op) wrapping received messages as tasks in the
  sender-designated group.
- Send-retry helper implementing M7 (yield + retry as a task state, no
  spinning inside a task).

**As built.** `sched/spsc_ring.hpp` (the ring), `sched/ring_transport.hpp`
(the seam plus `RealRingTransport`, the N² matrix),
`sched/sim_ring_transport.hpp` (M9's delay/reorder injection over a seeded
`SplitMix64` and the injected clock), `sched/ring_message.hpp` (the header
and the central kind enum — crosscore.md §3's six step kinds plus the four
system kinds, all declared, none sent yet), and `sched/send_retry.hpp` (M7).
Phase 3 in `Scheduler::RunOnce()` is no longer a comment. Four things the
work settled or found:

- **The two transports agree per *edge*, not per inbox.** The real one
  sweeps its peers in rotation so none starves; the simulation delivers by
  deadline. Two messages sent from *different* cores therefore arrive in an
  order the two do not share — and nothing above this layer may depend on
  that order, which is exactly what the reorder injection exists to prove.
  What both guarantee is per-edge send order and no invention, loss or
  duplication. The equivalence test asserts the real property rather than
  the tidier false one.
- **A handler runs inside a task, never in the drain.** Phase 3's job is to
  move messages off the ring; doing the work there would put an unbounded
  amount of it in a phase that has to stay bounded — the contract phase 1's
  io handlers are already under. The drain has its own loop budget
  (`max_messages_per_iteration`) for the same reason phase 4 has one.
- **The scheduling group travels in the message**, designated by the sender
  (sched.md §5), rather than being derived from the kind: the same kind can
  be foreground or maintenance work depending on what asked for it.
- **A message with no handler is dropped and logged, not fatal** — the same
  situation as a message whose tag matches no live pipeline state, which
  guideline 5 calls normal operation.

M7 is implemented in its plainest form — yield and retry, no backoff, no
ceiling, no deadline — because sched.md §10 leaves the retry protocol
`[OPEN]` and each of those would settle it. They belong with crosscore.md
§4's credit accounting, which is what actually bounds per-request buffering.

**Not built, and named so it is not assumed:** nothing constructs a
transport in production yet. `Expeditor` still builds one `Scheduler` and
never calls `AttachTransport()`, so at `cores = 1` the phase costs one null
test and the whole layer is exercised only by tests. Wiring it is P2's, with
the reactors it connects.

### P2 — Multi-reactor fan-out — **bullets 1-3 built (2026-08-05)**
- Expeditor spawns `cores` pinned workers; each owns a full per-core stack:
  Scheduler, BufferPool, WalManager + FileLogDevice(core_id) + segment
  naming `(core_id, segment_no)`, checkpointer. The existing single-core
  wiring becomes the per-core wiring, instantiated N times (page.md §6:
  "multi-core adds instances, not synchronization").
- Core 0 additionally hosts the system services (M5): extent lease service
  over messages, superblock anchor writer, catalog page ownership.
- Buffer-pool discipline: a core faults only pages it owns; an
  ownership-violation assert in the frame-load path (debug builds) enforces
  shared-nothing mechanically.
- Recovery: per-core parallel replay (wal.md §15) now over N streams;
  anchors read per core from the superblock.

**As built.** `include/kds/server/core_runtime.hpp` is one core's stack -
`Scheduler`, `FileLogDevice(core_id)` + `WalManager`, and the WAL drain
cadence. `Expeditor::Serve()` builds `cores` of them plus one
`RealRingTransport`, spawns workers 1..N-1 on `std::thread` with
`pthread_setaffinity_np`, and runs core 0's reactor on the calling thread.
Five things this settled or found:

- **The fourth bullet is vacuous and stays that way.** There is no WAL
  recovery to distribute - no `Recover()` exists anywhere in `wal/` - so
  "per-core parallel replay" has nothing to parallelize. Single-core
  recovery (wal.md §12) is a prerequisite this workplan assumes and **no
  milestone owns**; it is the largest hidden dependency here.
- **Cores above 0 come up alive and idle, deliberately.**
  `catalog::Catalog` reads the catalog's fixed pages (ids 4-12) straight
  through its `PageStore&`, and M5 gives those to core 0. Until P6 hands a
  core a catalog cache it cannot resolve a relation, so it cannot dispatch,
  so its WAL stream logs nothing and its checkpointer flushes nothing. They
  are built anyway because *this* is the change that decides their shape.
- **Allocation could not cross cores per call**, which is why the extent
  lease half of P5 moved here - see `include/kds/storage/extent_lease.hpp`.
  22 synchronous allocation sites sit deep in the storage layer and nothing
  can suspend mid-call; converting them would settle sched.md §3's open
  task-representation decision by precedent. A core leases a run of ids up
  front and allocates from it with no message instead.
- **A leased store must answer `IsAllocated` from its lease, not its free
  map.** A non-zero core reads the map at `Open()`; core 0 sets a lease's
  bits later, in *its* copy. Without the addition every page a leased core
  allocated read back `NotFound`.
- **Shutdown is a message** (`RingMessageKind::kShutdown`, added here and
  not in P1's list). `Scheduler::Stop()` writes a plain bool owned by its
  reactor's thread, so core 0 may not call it; making the flag atomic would
  put an atomic outside the ring indices, against guideline 1.

Anchor writes from a peer reach core 0 over `kAnchorWrite` -
`server::RemoteCheckpointAnchor` sends, and core 0's handler calls the same
`SuperBlockCheckpointAnchor` a local checkpoint uses, so one piece of code
knows how an anchor reaches the page. It is **fire-and-forget on the
merits**: an anchor is published only after `CHECKPOINT_END` is durable
(wal.md §8-3), so losing one costs a longer replay and never an answer -
which is what lets it be one-way and keeps P2 clear of the suspend/resume
question.

Verified under ThreadSanitizer, which caught one data race - in the *test*,
reading a running core's `stopped_` from the main thread. That is precisely
the access the design forbids, and the test now proves liveness by making
the core serve a message instead.

### P3 — Sessions and accept
- SO_REUSEPORT listener per core (M3); connection, session state,
  statements, portals, and open transaction live on the accepting core.
- Session context object threaded through dispatch (shared prerequisite
  with txn workplan T5 — build once).

### P4 — Cross-core read execution
- ~~Prerequisite: the KWP D5 row encoder~~ — **built 2026-08-05**,
  `include/kds/wire/row_codec.hpp`. Row descriptions and `{i32 len | -1 =
  NULL, bytes}` row batches per `docs/protocol.md` §6, covering exactly the
  types the engine can store. It sits **below both consumers** and knows
  about neither frames nor cores, which is what makes CC2's "one encoder,
  two consumers; no second row format" literal rather than aspirational —
  it exists before either consumer, the only way that rule survives
  whichever is built first. `float`/`decimal` are refused rather than
  guessed (`DECIMAL`'s encoding is `[OPEN]` in §6 and settling it here would
  settle it for the type system). `DecodedField` holds views into the
  payload, and the rvalue `DecodeRowBatch` overload is deleted so decoding a
  temporary is a compile error — the mistake was made once while writing the
  tests, which is why the guard exists.
- Implement `docs/crosscore.md` in full: pipeline table per core,
  STEP_OPEN/BATCH/EOF/CREDIT/CANCEL/ERROR handling, KWP batch
  encoder reuse, credit accounting, teardown-by-tag.
- Statement planner on the session core resolves owner cores from the
  catalog cache and picks fast path vs pipeline (crosscore.md §2).
- DML statement shipping: route a write statement whole to the owner core;
  home-core binding + retryable rejection for second-core writes
  (crosscore.md §6) with the observability counter.

**The restriction half is built (2026-08-05); the pipeline is blocked.**

`include/kds/server/core_affinity.hpp` implements CC3 and §6: a
transaction's writes bind to a home core on the first write
(`Session::BindHomeCore`), a write to another core's relation is refused
with `kTxnConflict` - the retryable spelling first-updater-wins already uses,
so a client that retries on `TXN_CONFLICT` needs no new code - and every
refusal is counted by `(home core, target core, relation)`, which is §6's
stated input to the 2PC design. The planner half is `CheckReadAffinity`,
called right after `Compile`: all-local is the fast path, and a chain
spanning cores is refused with an exact reason.

**Why the pipeline itself is not here.** `CommandDispatcher::Dispatch()`
returns a finished reply synchronously, `TcpServer` calls it inline from a
read handler, and `ChainRunner` walks a step chain with **no suspension
point anywhere in it** (`grep -c 'kSuspended\|co_await\|Yield'
src/exec/step_vm.cpp` → 0). A pipeline is an asynchronous dataflow - the
session core sends `STEP_OPEN` and must then wait for batches - so building
one means making the whole statement path suspendable, and task
representation is an explicitly open decision (`docs/sched.md` §3 and §10).
Rewriting the executor into a state machine would settle that decision by
precedent, at the largest possible scale, without anybody deciding it.

**The decision landed 2026-08-05: C++20 stackless coroutines**
(`include/kds/sched/coro.hpp`, `docs/sched.md` §3). `co_await WaitFor{&flag}`
is the cross-core request/response shape, and `coro_test.cpp` demonstrates a
coroutine on core 0 sending to core 1 and resuming on its reply, in
straight-line code, with both reactors stepped round-robin. The scheduler
needed no change.

**The statement path is suspendable as of 2026-08-05.**
`CommandDispatcher::DispatchAsync()` is a coroutine, and `TcpServer` submits
it as a task and appends the reply when it completes. Nothing suspends yet -
the executor is still synchronous - which is exactly what made the change
verifiable: every one of the 1,254 tests behaves as it did, because *when* a
reply is produced has not moved.

Three properties the seam had to preserve, each now pinned by a test:

- **One statement in flight per connection.** Forced twice over: the session
  is stateful (an open transaction, the failed-txn flag), and the newline
  protocol has no request ids, so replies must leave in arrival order.
  Pipelining is unchanged - a batch is drained one command at a time and the
  replies still leave in one `write()`; only concurrency *within* a
  connection is excluded, which the protocol never offered.
- **The statement text outlives the statement.** Parser tokens are views into
  it (parser-v2.md's zero-copy tokens), so each line is copied out of the
  inbox into the connection before dispatch.
- **A connection that goes away mid-statement is deferred, not destroyed.**
  The coroutine holds a pointer to its session; tearing that down under a
  queued task is a use-after-free. It is marked and closed when the statement
  finishes. Cancelling would be better and needs cancellation the engine does
  not have.

**A pre-existing bug fell out of it.** `FlushOutbox` used `::write`, so a
client that hung up without reading raised SIGPIPE and **terminated the
server**. One write per readable event made the window narrow; a reply per
statement completion widened it enough that a pipelined client hanging up
killed the process every time. It is `::send(..., MSG_NOSIGNAL)` now, which
turns it into the EPIPE the error path already handled.

**The suspension-safety rule is mechanical as of 2026-08-05**, ahead of the
code that will need it. `exec::InstallSuspendAudit()` registers the
executor's answer to "is it safe to be parked right now?" into
`sched::SetSuspendAudit` (a hook, because `sched/` sits below `exec/` and
must not know what a page is), and `CoroTask::Poll()` consults it in debug
builds at the moment a coroutine suspends.

What it forbids: **suspending while holding a page span.** `parser-v2.md`
I15's R1 already forbids a page *fetch* under a live span, because nothing
pins the frame the span points into. Suspending under one is strictly worse
— the span stays live not for the length of a nested call but for arbitrary
wall time, across every other statement that runs on this core in between.
A store that ever evicts turns that from a latent bug into a routine one.

It is installed per core, on the thread that runs the statements, because
the guard's counters are core-local — installing once on the startup thread
would leave every worker unguarded.

**What P4 still needs** is the suspension point itself: `ChainRunner` walks
a chain start to finish, so a step cannot yet await a remote batch. That is
a viral conversion of `Execute` / `RunStep` / `RunPointStep` /
`RunWalkStep` / `AcceptTupleAt` into coroutines — the largest single change
left in this workplan, and the one the rule above exists to keep honest. The
seam above it will not have to change again.

Note the two halves age differently: the write restriction survives the
pipeline (it is what keeps commit single-stream, guideline 3), while the read
refusal is precisely what the pipeline replaces.

### P5 — Id lease services
- trx-id block leases from the system core (M4), replacing the dispatcher's
  in-memory `next_txn_id_` and aligning with txn workplan T3 (superblock
  counter, crash burns the remainder). **Note the premise still holds after
  the transaction milestone**: `CommandDispatcher::next_txn_id_` is still a
  process-local counter restarting at 1 every boot, used on the `own_txn`
  path beside the durable `txn::TrxIdSequence`. Its own comment calls it
  wrong the moment recovery reads two boots of one stream. So P5 is two
  jobs: retire that counter onto the durable sequence, *then* lease blocks
  per core.
- ~~Extent leases for file growth~~ — **built**. The lease itself moved into
  P2 (2026-08-05), because per-core page stores do not work without it; the
  **refill path landed the same day**, as the first production use of the
  coroutine decision (`include/kds/server/extent_lease_service.hpp`).

  A peer at `low_water()` submits a coroutine that sends `kExtentLease` and
  `co_await`s the grant; core 0's handler carves the extent from the free
  map it owns and replies. Three things worth keeping:

  - **The refill runs beside allocation, never inside it.** `CreateNew()` is
    called from 22 sites deep in btree splits and chain growth, none of them
    coroutines, so the request is a background task and the lease is asked
    for *before* it is spent. A core that runs dry first gets
    `ResourceExhausted`, which is retryable — the refill is already in
    flight.
  - **One request in flight per core.** Without that the low-water check
    submits a fresh request every tick until the first grant lands, and core
    0 answers every one — burning an extent per tick.
  - **An exhausted free map replies with a zero-page grant** rather than
    dropping the message. A requester that is `co_await`ing must be able to
    wake and fail; a dropped reply parks it forever.

  Extent size stays the existing `[OPEN: size]`
  (`storage::kDefaultExtentPages`, 64 pages) until measured.

### P6 — Catalog cache and DDL propagation — **partly built (2026-08-05)**
- Per-core catalog cache over core-0-owned catalog pages; version stamp per
  relation row.
- DDL executes on core 0, then broadcasts invalidation; remote stale-oid
  access surfaces as a retryable step/statement error (crosscore.md §5).
  The existing "instance-scoped coherency" caveat becomes core-scoped and
  is re-documented.

**As built: a peer reads the catalog. It still cannot read a relation.**

The catalog half works. A peer opens its own `DevicePageStore` over the
shared device, faults the catalog's fixed pages **read-only**
(`DevicePageStore::MayFault` / `MayWrite` are now separate questions, and
the fixed system range is readable by every core and writable only by core
0), and resolves a relation by name and schema. Core 0's
`Catalog::BumpVersion()` — the single DDL choke point — now flushes the
catalog pages and broadcasts `kCatalogInvalidate`; a peer receiving it
evicts the catalog page *frames* and drops its `CatalogCache`.

Four things this phase found, each of which had to be fixed or recorded:

- **The workplan's premise was wrong.** Catalog pages do not change only by
  DDL: `sys.patterns` and `sys.access_stats` are written on the **ordinary
  statement path** (`TrailRecorder::EnsurePattern`, `RecordSteps`). A peer
  may not write them, and `RegisterPattern` returns a pointer its caller
  uses immediately, so it cannot be shipped either. **Peers therefore run
  with `waystone_recording` and `access_statistics` off.** Both are
  advisory — invariant 8, and "a degraded statistic, not a degraded
  database" — so a peer returns identical rows, more slowly. The real fix
  is per-core statistics relations, which crosscore.md §2 already calls for.
- **Catalog reads dirtied the catalog.** `ScanAll` used `Get()`, which marks
  a frame dirty by convention rather than by what was written — so every
  lookup dirtied a catalog page and every checkpoint wrote all nine back
  unchanged. Now `GetForRead()`. The bug long predates multicore; the
  ownership check is what surfaced it.
- **Invalidating a cache is not enough.** Dropping derived facts while the
  stale page frames stay resident is a no-op — the next scan reads the same
  bytes and reaches the same conclusion. Hence
  `DevicePageStore::EvictClean()`, which refuses to evict a dirty frame.
- **A peer's view of the database starts at core 0's first flush.** A peer
  builds "which pages exist" from the free map on the device at `Open()`,
  so a peer started before core 0 has synced sees an empty database.

**The blocker P6 stops at**, pinned by
`CoreRuntimeTest.APeerCannotYetFaultARelationsDataPages`:

> **Relation ownership and page ownership are different facts and nothing
> reconciles them.** `sys.tables.owner_core` says which core owns a relation
> (M1); a page belongs to whichever core's lease it came from (P2). Every
> relation's pages are allocated by core 0, because DDL is core 0's — so a
> relation the catalog says core 1 owns is built entirely out of core 0's
> pages, and core 1 may not fault one.

Closing it is a design decision, not an omission: either `CREATE TABLE`
allocates a relation's root from its *owner's* lease — making DDL a
cross-core allocation — or page ownership stops being a per-core lease and
becomes a function of the catalog. A second, independent blocker sits behind
it: **a peer cannot INSERT either**, because `AllocateRowId()` bumps
`next_id` on a catalog page. That one is P5's shape — a leased range of row
ids, the same mechanism as the page-id lease and as
`docs/keystoneid-invariant.md` K-M2's bump-ahead allocator.

### P7 — Observability
- Per-core: everything wal.md §16 lists, now labeled by core.
- Cross-core: shipped batches/bytes per (edge, relation), credit-stall
  time, pipeline count, cancel/error counts, rejected cross-core writes by
  (home, target, relation) — the placement/2PC input (crosscore.md §6).

### P8 — Deterministic multicore simulation and tests
- Harness: N reactors stepped round-robin on one thread over the simulated
  transport; delay/reorder/drop injection; seed-stable (sched.md §6 rules:
  deterministic containers, sequential ids).
- Run at `cores ∈ {1, 2, 4}`: the full existing suite (storage, wal
  crash-recovery, txn.md §10) plus crosscore.md §8 tests 1–8.
- CI: single-core remains the default matrix entry; multicore sim runs are
  additive.

## 3. Guidelines (invariants for every phase)

1. No shared engine state between cores; the ring seam is the only
   cross-core channel. No atomics outside ring indices.
2. The single-core build path must not regress: with `cores = 1` the ring
   layer, pipeline layer, and lease services contribute zero messages and
   zero allocations on the hot path.
3. LSNs are stream-local and are never compared across cores. Nothing in
   this milestone may create a cross-stream ordering dependency — that is
   what keeps recovery per-core and the 2PC door safely closed.
4. Owner-core resolution comes from the catalog only; no code derives
   ownership from page ids, hashes, or topology.
5. All cross-core messages are POD, tagged, and processable after the
   originating request is gone (discard-by-tag is normal operation).
6. Every new mechanism lands with its simulated-transport test in the same
   change; a cross-core code path without a deterministic test does not
   merge.
