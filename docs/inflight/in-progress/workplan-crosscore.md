# Multicore Workplan

Work items to take the engine from the single-core reactor (sched.md "Phase
1", the current state: one Expeditor, one Scheduler, `core_id = 0`
everywhere) to thread-per-core operation with cross-core execution
(`docs/spec/crosscore.md`). Companion to `docs/spec/sched.md`, `docs/spec/wal.md` §3,
`docs/spec/page.md` §6, `docs/spec/protocol.md` D3.

Already fixed by existing specs, not revisited here: one pinned worker per
core with no thread creation after startup and no work-stealing (sched.md);
N² per-core-pair SPSC rings; per-core WAL streams, buffer pools, and
checkpoints; server-side forwarding with topology-unaware clients (D3).

Ordering note: the transaction milestone (`docs/spec/txn.md` workplan) lands
first on the single core. Everything in txn.md is core-local by design, so
multicore adds instances, not synchronization — the reverse order would make
every txn test carry cross-core variables from day one.

## 1. Decision Record

| # | Decision | Resolution |
|---|----------|-----------|
| M1 | Ownership partition | **Relation-unit ownership recorded in the catalog** (`owner_core` on the relation row), assigned at CREATE. Write-coupled auxiliaries (unique indexes, Cabin, Waystone, var-heap) always co-located with the base relation; FK-linked relations co-located in v1 (crosscore.md §6). Page/extent hashing rejected: btree descent and heap-chain walks cannot cross cores per hop. **Amended 2026-08-24 (v2): the unit is the pk range** (`crosscore.md` CC8-CC10, §2a) — relation-unit ownership survives as the degenerate one-range case and `owner_core` as exactly that; the hashing rejection stands and is what sizes the unit; co-location becomes `crosscore.md` §6a's gates. Nothing range-granular is built (blueprint R1-R6) |
| M2 | Cross-core statements | **Cross-core read execution now** — step pipeline per `docs/spec/crosscore.md` (CC1–CC6). Writes stay single-core per transaction; 2PC `[OPEN]` |
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
  `docs/spec/foreign-keys.md` keeps those together in v1 by deferring
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
  NULL, bytes}` row batches per `docs/spec/protocol.md` §6, covering exactly the
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
- Implement `docs/spec/crosscore.md` in full: pipeline table per core,
  STEP_OPEN/BATCH/EOF/CREDIT/CANCEL/ERROR handling, KWP batch
  encoder reuse, credit accounting, teardown-by-tag.

  **Staged 2026-08-10 (P4a-P4e), every prerequisite now built:**
  - **P4a — the pipeline data plane** — **built (2026-08-10)**: the tag, the payload
    codecs for BATCH/EOF/CREDIT/CANCEL/ERROR, per-edge credit accounting
    (initial 4 `[PROPOSED]`, grant-on-drain, never send without one), and
    the batch builder over the KWP row encoder with the 32 KiB
    `[PROPOSED]` target - pure, scheduler-free, unit-tested, `wire/`'s
    build-the-seam-first method. The STEP_OPEN **step descriptor codec**
    is P4a's second half: full fidelity for a compiled `Step` (kind, oid,
    key operand, residuals, range, projection set), no
    statement-text-as-descriptor interim - the same refusal §4 makes for
    an interim batch format, for the same reason.
  - **P4b — the remote step server** — **built (2026-08-10)**: an owning core executes an opened
    step against its local state and streams batches under credit, EOF at
    end, ERROR with the D9 mapping. Single-step chains first.
  - **P4c — the session side** — **built (2026-08-10)**, and the
    end-to-end test is the engine's first cross-core statement (a star
    SELECT against an owner_core=1 relation, served remotely, reply
    byte-identical to the local path): the dispatcher (a coroutine since
    2026-08-05) awaits remote batches for a single-relation remote read
    and frames the reply; `CheckReadAffinity`'s refusal narrows to the
    shapes the pipeline cannot yet run.
  - **P4d — multi-step pipelines and the executor conversion**: step k→k+1
    wiring, join-key forwarding, and the viral `ChainRunner` coroutine
    conversion under the suspend-audit rule - the largest piece, last.
    **Started 2026-08-13**, with the survey's three facts recorded here so
    the next session does not re-derive them:
    1. **No nested-coro awaiter exists yet, and it is the enabling
       primitive.** `CoroTask::Poll` drives exactly one handle and `WaitFor`
       parks on flags; the executor's spine is *mutually recursive*
       (`Execute → RunStep → RunPoint/RunWalk → AcceptTupleAt →
       RunStep(k+1)`), so `co_await child_coro` must work first — parent
       promise records the active child, Poll resumes the deepest pending
       handle, the child's Status returns through await_resume. Built and
       tested scheduler-free before any executor line changes, per this
       workplan's own seam-first method.
    2. **The conversion order is DispatchAsync's**: spine to coroutines with
       zero suspension points first (bit-identical behavior, suite green
       unchanged), forwarding after. What made the 2026-08-05 seam
       verifiable — "when a reply is produced has not moved" — is reused at
       the executor scale.
    2a. **P4d-2's staging, decided 2026-08-13 from the survey.** `exec::
       Execute` has exactly four call sites (dispatcher ×3,
       remote_step_service ×1). The conversion keeps the synchronous
       signature as a wrapper that drives the coroutine to completion
       inline — legal precisely while nothing suspends, so all four sites
       and every test stay untouched and bit-identical. The `ExecuteAsync`
       seam the dispatcher will await arrives with its first awaiting
       consumer (P4d-4), not with the conversion — the built P4d-2
       deliberately did not add it, because an async entry nobody awaits
       is dead code (this paragraph originally claimed it as part of the
       conversion; corrected 2026-08-13). The spine (`Execute`, `RunStep`,
       `RunPointStep`, the index paths, `AcceptTupleAt`) becomes
       `sched::Coro`-returning with `co_await` at the recursion edges. At
       the two places `AcceptTupleAt` is reached from *inside a walk
       visitor callback* — which cannot await — the child coroutine is
       driven by a named inline helper (`RunToCompletionAtWalkBoundary`),
       correct while nothing suspends beneath it — a contract P4d-3 now
       *enforces* (the driver refuses an unsatisfied wait instead of
       resuming past it) and P4d-4's batching dissolves for the multi-step
       shape (the staging originally read "P4d-3 dissolves"; the terminal
       split at `95946c4` moved that to P4d-4). **Execution method, so the flip is mechanical**: change
       the seven signatures (`Run`, `RunStep`, `RunPointStep`,
       `RunCabinStep`, `RunIndexStep`, `RunWalkStep`, `AcceptTupleAt`) to
       `sched::Coro` and rebuild - a `return` inside a coroutine is a hard
       compile error at every spine site, while the walk visitors' own
       `VisitControl` returns compile untouched because a lambda is a
       separate function to the compiler. The error list IS the conversion
       list, with the lambda discrimination done by the compiler rather
       than by fallible text matching; `Coro`-to-`Status` conversion
       errors then mark every recursion edge for `co_await` and the two
       visitor sites (step_vm.cpp:1031, :1042 at `2953340`) for the
       boundary helper. Sub-chain evaluation inside `EvaluateConjuncts`
       stays synchronous and drives any converted runner through the same
       helper - whether sub-chains ever await is P4d-4's decision, not
       this task's side effect.
    3. **Walk callbacks cannot await, and the answer is the page boundary.**
       The chain walks are visitor-style; a callback cannot `co_await`.
       Awaits therefore happen *between* pages at the `RunWalkStep` level —
       finish the page, drop its pin, await, continue — never inside a
       visitor. This is also what makes the suspend audit's rule
       enforceable at last: with the PageRef migration landed
       (2026-08-13), "suspending while holding a page span" is mechanically
       `DevicePageStore::live_pins() != 0`, and the audit should assert
       exactly that.

    **P4d status.** P4d-1 built (`2953340`: the nested awaiter,
    scheduler-free tests first). P4d-2 built (`0fd7fc3`: the spine is
    coroutines with zero suspension points; `5ec61da` fixed three
    pre-existing wrong-answer bugs the conversion made conspicuous;
    `95946c4`'s terminal split took the per-tuple path back to zero
    coroutine frames; measured in `bench/results-p4d-executor.md`).
    **P4d-3 built 2026-08-13**, all three halves of fact 3: (i) the walk's
    page loop is owned by the coroutine — `heap::ChainVisitOnePage` /
    `btree::BtreeVisitLeafPage` visit exactly one page under the existing
    visitor contract and return the next page, the whole-chain forms are
    loops over them, and `RunWalkStep` steps pages itself with the pin
    dropped at each boundary, so the between-pages gap is a real
    suspension point awaiting only its first await (nothing suspends yet —
    bit-identical by construction); (ii) `RunToCompletionAtWalkBoundary`
    is gated — `Coro::TryResumeDeepest` consumes a satisfied wait exactly
    as `CoroTask::Poll` does and *refuses* an unsatisfied one, so a wait
    beneath a synchronous boundary is a hard `InvalidArgument`, never a
    fabricated resume (the `5ec61da` review's loudest flag); (iii) the
    suspend audit takes the installing core's store and asserts
    `live_pins() == 0` at every suspension, the pin half of R1's rule.
    One scope note the review insisted on: **only the outermost walk's
    boundary is awaitable today.** A step below the first is reached from
    inside the parent's visitor through the gated driver, under the
    parent's pin - so its page boundary cannot await until P4d-4c moves
    the descent outside the visitor, and the audit is what proves that
    ordering rather than trusting it.
    **P4d-4a built 2026-08-14** — the engine's first genuine suspension.
    `exec::ExecuteAsync` exists with its first awaiting consumer: the
    remote step server streams under credit instead of collecting the
    relation. A `resume_gate` predicate on the runner is consulted at the
    outermost walk's page boundary (`WaitUntil` semantics - no coroutine
    frame per page, one predicate call per poll), and the producer
    coroutine parks there while sealed batches wait on credit, so
    buffering is bounded by the credit ceiling plus one page's seals.
    CANCEL reaches a parked producer by mark-and-self-teardown (the
    handler must not erase state a parked walk re-finds). Proven by
    `remote_step_service_test.cpp`'s streaming trio: the park is real
    (task suspends, idle polls send nothing), the suspend audit stays
    quiet with the real store armed - no pin, no span, at the first real
    park - traffic is byte-identical to collect-then-stream, and a cancel
    never EOFs. The reactorless collect-then-stream shape survives as the
    no-`SubmitFn` fallback the older tests still pin.
    **Open hazard, and it is live, not theoretical**: a producer parked
    mid-walk borrows its `TableAccess` across wall time exactly as the
    executor's `Bind` does, and **any** DDL anywhere in the system
    invalidates it. `Catalog::BumpVersion` broadcasts `kCatalogInvalidate`
    to every core, whose handler runs `CatalogCache::Invalidate()` -
    `table_access_.clear()`, which *destroys* every entry, not just the
    DDL'd relation's. So it takes no cross-core DDL shape and no DDL on
    the pipelined relation: one `CREATE TABLE` on any core while a
    producer is parked is enough. Reproduced under ASan at
    `31319c8`+review: `InvalidateFromPeer()` between two `Pump()`s of a
    parked producer gives `heap-use-after-free` in
    `CheckKeystoneColumn` ← `DecodeRowInto` ← `AcceptTupleAt`
    (`step_vm.cpp:1345`) on the next resume - so it is the *executor's*
    `bound_` borrow that dies first, and copying the producer's own
    `Schema` alone would not fix it.
    **Answered 2026-08-14, the re-Bind option, both halves**: `RunWalkStep`
    re-runs `Bind` (and re-opens the frame) after every *actual* park -
    the only points another task can interleave - so a cleared cache is
    re-filled from catalog storage before any borrow is read, and a
    relation dropped across the park surfaces as the clean retryable
    error §5 promises a stale plan, never a freed read; the producer's
    `Schema` is a frame-owned **copy**, which also makes "trusts the
    descriptor, does not re-resolve" structural. Refuse-DDL and
    defer-invalidate were rejected: the first refuses unrelated DDL for a
    window sized by someone else's scan, the second serves dropped tables
    from a stale cache on the DDL's own core - a wrong answer where
    re-Bind gives the right one. Pinned by the two regression tests in
    `remote_step_service_test.cpp` (invalidation across a park changes
    nothing; DROP across a park answers STEP_ERROR and never EOF). The
    frame re-open is legal precisely because the gated shape is
    single-step - no outer row is live at a boundary - and **P4d-4c
    inherits that caveat by name** when deeper steps learn to await.
    **P4d-4b staged 2026-08-14, the survey's four facts recorded here so
    no session re-derives them:**
    1. **Open ordering is lossy today, and the answer is chained opens.**
       §3's teardown rule silently discards a batch whose tag matches no
       live pipeline, so a session opening step k and step k+1 on
       different cores races k's first batch against k+1's `STEP_OPEN` -
       a lost-rows race, not an error. Decided: **opens propagate
       upstream through the pipeline itself.** The session opens only the
       *final* stage's core; each stage's open handler, finding an
       upstream edge described in its envelope, forwards the upstream
       stage's open once its own state exists. Readiness ordering holds
       by construction, no new message kind exists, and the credit
       protocol is untouched (where the initial credit lives stays §9's
       open knob). Errors route to the session by tag exactly as today;
       CANCEL still goes point-to-point from the session, which knows
       every stage's core from its own plan.
    2. **The wire grows two fields, both in the envelope, neither in the
       step descriptor.** `StepOpenHead` gains the downstream *step id*
       (it has only the core) and the envelope gains the **forwarded-row
       layout** of the upstream edge: the ordered upstream columns each
       batch row carries, shipped as full `SysColumnRow`s - KWP fields
       are views, not self-describing, and decode and re-encode both
       need the width/scale semantics only the catalog row carries (the
       survey first said "(col_pos, type) pairs"; the built form is the
       rows, per 4b-1). The step descriptor codec and its version are
       untouched - it already ships the full compiled step, key operand
       and column refs included.
    3. **The forwarded set is already computed.** Step k forwards the
       join key step k+1 consumes plus what the projection and later
       residuals need of k's relation (crosscore.md §2) - which is the
       compiler's `read_columns` of step k minus k's own filter-only
       columns; the session derives it at plan time and both edge ends
       receive the same list, the upstream to encode, the downstream to
       decode.
    4. **The mid-chain stage runs on the executor's own parent-frame
       machinery, not a second evaluator.** The consuming stage opens an
       outer `ChainFrame` over the forwarded layout's pseudo-schema,
       fills slot 0 per upstream row through `SlotsFor(0)`, rewrites the
       shipped step's upstream references from (up=0, slot=k) to
       (up=1, slot=0) - the same re-slotting P4b already does for its
       one-slot chain - and runs the step as a one-step chain whose
       parent link is that outer frame, exactly `EvaluateSubChain`'s
       shape. One row in, a probe or filtered walk against local state,
       forwarded columns out.
    Staging: **4b-1 built 2026-08-14** (`e354c4d`): `downstream_step` in
    the head (zero, the historical value, still means the session's own
    read), the optional upstream section - core, forwarded layout as
    full `SysColumnRow`s (KWP fields are views, not self-describing),
    the enclosed open - and `DecodeStepOpenEnvelope`.
    **4b-2 built 2026-08-14** - the consuming stage. The envelope's
    upstream section gained the **output spec** (`StepOutputColumn`: a
    pass-through of an input-layout column or a local column, in the
    order the downstream decodes - a stage never guesses what its
    downstream needs). `OpenConsumingStage` validates the normalized
    references (own columns at up=0/slot=0, upstream at up=1/slot=0
    with col_pos into the forwarded layout), opens the pipeline,
    submits `RunConsumer`, and forwards the enclosed open **last** -
    state first, upstream last, the chained-open contract in code.
    `RunConsumer` parks between input batches, fills a one-slot outer
    `ChainFrame` per upstream row, runs the local step through the
    executor's new `parent` frame parameter (fact 4 exactly; both
    entries grew it, nesting depth 1), seals mixed output rows through
    the same Seal/Drain machinery the producer uses, and grants the
    upstream one credit per consumed batch - buffering bounded by one
    input batch's seals plus the credit ceiling. `wire::FieldToValue`
    (hoisted from the dispatcher: one inverse beside the one encoder)
    turns input fields into frame values; peers register
    kStepBatch/kStepEof to the step server; errors route to
    `tag.session_core`, never the message source, because a chained
    open's sender is a stage and errors belong to the session. Proven
    by `ConsumingStageTest`: the chained forward precedes any batch, a
    parked consumer, a three-key join emitting (key, qty) rows
    byte-decodable downstream, grant-on-drain, EOF riding the
    producing=false drain, cancel reaching the park, and the
    forwards-nothing refusal - ASan-clean beside the sim suites.
    **4b-3 built 2026-08-15** - the session side, and with it **the
    engine's first multi-step cross-core statement executes**: a
    two-step join, scan feeding probe, planned by the session and served
    as a chained pipeline (`CoreRuntimeTest.
    ATwoStepJoinAgainstRotatedRelationsIsServedAsAPipeline`, both stages
    on one peer - self-sends are the transport's degenerate case). What
    landed, exactly the surveyed recipe: the envelope's **output section
    stands alone** beside the upstream half (absent = whole row, so
    every pre-4b-3 envelope means what it meant) and the **leaf honors
    it** - both producer shapes seal the spec'd columns; the forwarded
    layout of edge 0→1 is the unique step-0 columns among step 1's
    key/residual refs and the projection's slot-0 refs, ascending;
    `BuildTwoStepPipeline` (session_step_client.cpp) computes it,
    normalizes the shipped step's refs (own → (0,0), upstream → (1,0)
    at the forwarded index - the residual's *lhs* may be upstream too,
    an ON clause is written in either orientation, so the stage-side
    validation admits both sides either way), builds both output specs
    and both opens, and hands `OpenPipeline` the stage list plus the
    decode/render facts (`output_layout`, `column_names`,
    `projection_types` - stashed in the read because the chain dies
    with the statement frame); the dispatcher's eligible class is two
    steps, no aggregate/sort/limit/offset/hoisted/sub-chains/
    emit_in_key_order, **inner kind kProbe only** (at most one row per
    input row keeps the consumer's buffering bounded; an inner scan
    stays refused by name until it can park mid-walk, P4d-4c), at
    least one owner remote, refusals falling through to the affinity
    refusal; the typed session decode renders the projected reply.
    The expeditor grew its own RemoteStepServer (core 0 serves stages
    like any core) with **one fan-by-tag lambda per shared kind** -
    a scheduler holds one handler per kind, and both consumers discard
    unmatched tags silently, so the tag is the demultiplexer.
    Teardown grew the multi-stage halves: `OnStepError` matches **by
    statement** (request_id + session_core - a failing stage answers
    under its own tag while the read is registered under the final
    stage's), and `Close` cancels **every stage** unless a clean EOF
    ended the read - a leaf's failure leaves its consumer parked on
    input forever, and only the session holds the whole stage list.
    Two of the three carried debts closed with it: the per-field width
    refusal is `wire::FieldToValueChecked` (invariant 13 one level up),
    applied at both edge decodes - the consumer's input fill and the
    session's typed decode; and **`downstream_step` is now read**: the
    consuming stage refuses an enclosed open whose downstream core/step
    do not address the stage forwarding it - a plan wired to the wrong
    consumer would stream rows to a tag that never opened. Still
    carried, by name: **the per-input-row runner cost** (each input row
    pays an ExecuteAsync frame, a Bind and a frame Open before touching
    a tuple - the shape 95946c4 removed locally, reintroduced one level
    up; measure at P4e's benchmark before building the per-batch runner
    handle, and note the current shape is why RunConsumer needs no
    schema-copy ceremony: no borrow survives a park).
    **The 4b-3 review gate (2026-08-15)** found one CRITICAL and priced
    two more: **a shipped conjunct with the upstream relation's `uint64`
    column on the left answered orderings differently across cores** -
    the comparison's type comes from the lhs alone, `SchemaFor` answers
    nullptr for any `up != 0` ref, and `CompareValues` reads `type_val`
    in exactly its `kTypeValUint64` arm, so `a.u > b.u` with `a.u`
    above `INT64_MAX` compared signed remotely and unsigned locally.
    Refused at plan (`BuildTwoStepPipeline`; equality stays shippable,
    the int64 bit patterns are order-isomorphic under `=`). The real
    fix, a decision not an edit: **`StepPredicate` carries the lhs
    `type_val`, resolved at compile** exactly as `projection_types`
    already is, `EvaluateAll` stops asking `SchemaFor`, and the refusal
    is deleted - it also closes the same accepted hole
    `chain_frame.cpp` documents for correlated sub-chains. Second:
    **every shipped stage read with every writer visible**
    (`snapshot=nullptr` → `kSeesEverything`) - pre-existing from
    P4b/P4c, doubled by 4b-3, and **closed the same day**: the server
    takes its host's `TransactionManager` and mints the
    autocommit-shaped view itself, once per stage, held by value across
    every park (a `ReadView` is a POD; the undo pointer outlives the
    reactor). CC4's "the owning core's latest committed snapshot" is
    now literal - no view crosses a core, each stage mints its own on
    the core owning the relation, which is the per-core RR weakening
    `docs/inflight/known-gaps.md` records. P4e's equivalence pass can now pin
    the right behaviour. Third, applied with the review: the session's two
    rendering loops folded into one (which put the width-checked decode
    on the P4c edge too - the third of three edge decodes, previously
    bare), one send lambda for core 0's two pipeline endpoints,
    `NarrowTo` as the one home for "empty spec = whole row" plus its
    re-fetch bound, `Open` as a one-stage `OpenPipeline`, and the
    shared input-edge lookup.
    **P4d-4c, the gated inner walk: built 2026-08-15.** The consuming
    stage's per-row run became `co_await ExecuteAsync(..., &output_ok,
    &outer)` - awaited and gated instead of synchronous - so a **walked**
    inner parks at its own page boundaries whenever the sealed output
    cannot ship. That is the bound the dispatcher was waiting on, and
    the eligible class widened with it: an inner `kScan`/`kFilterScan`
    **with at least one residual reaching the outer row** now ships,
    which is what a join on a *non-pk* column compiles to. Two facts
    the survey corrected: the compiler reserves `kFilterScan` for an
    unindexed equality against a **literal**, so a join predicate leaves
    the kind `kScan` - admitting only `kFilterScan` would have shipped
    nothing new; and the "reaches the outer row" half is load-bearing,
    because without it the class admits a walk that ignores its input -
    a cross product, correct but quadratic. Safe across the park
    because `ChainFrame::Open` touches only its own frame's storage, so
    the inner re-open after a park cannot disturb the parent frame
    holding the input row. Proven by three non-pk join shapes in P4e's
    equivalence test, over data where one outer row hits two inner
    rows, two outer rows hit the same inner row, and two hit none.
    **The 4c review gate (2026-08-15)** confirmed the park is safe and
    measured the bound real - **194 rows high-water, one 8 KiB page's
    worth, against a 1600-row relation** - and found one bug and one
    coverage hole. The bug: the stage handed `ExecuteAsync` a **fresh
    budget per input row**, and the runner seeds its counter from the
    limit it is given, so the row-touch count restarted every row and a
    walked inner had no statement-wide bound at all - the n² shape
    `exec/budget.hpp` exists to refuse, and which the *local* path does
    refuse, so the pipeline was answering a statement its local twin
    errors on. Fixed by accumulating into one `ExecStats` per stage and
    handing each row only what is left, refusing *before* the call when
    nothing is (`Budget(0)` is the **unlimited** sentinel, so subtracting
    to zero would remove the bound rather than enforce it). The ceiling
    is now this core's **configured** budget, which the server ignored
    entirely before - `RunProducer` and the reactorless fallback each
    built a fresh default. Carrying the *session's* limit across a
    heterogeneous deployment is an envelope field, still open. The
    coverage hole: the `(gate != null && parent != null)` combination
    was new with 4c and **no test reached it** - every consuming-stage
    test used a one-page relation, so the gate was never consulted, and
    P4e's equivalence test grants credit synchronously inside the send,
    so its buffer is empty at every check. Closed with a parked-walk
    test over eight pages and a cancel-inside-a-parked-row test. Also
    applied: the eligible class moved out of the dispatcher into
    `BuildTwoStepPipeline` (one home for what may ship, ~45 lines out of
    the dispatcher), `txn::AutocommitSnapshot` replaced two copies of
    the same six lines, and the snapshot rule stopped being restated in
    five places. Left as a known latent: `Drain` holds a `Pipeline&`
    across `send_`, which a synchronous send reaching `OnStepOpen` would
    invalidate - unreachable today, and the one place in that file not
    following its own re-find-by-tag discipline.
    **Measured 2026-08-15** (`bench/results-p4d-executor.md` §10, 38
    runs, `f2f101d` vs `53fd2ce`, Release, 2 vCPU, interleaved): **null**
    - the local statement's added cost is ≤ ~0.3 µs and measured
    negative, indistinguishable from control arms that cannot reach the
    code. Two things the run established beyond the null. First, a
    **same-binary control** is now part of this series: it showed the
    harness manufacturing +55 µs of apparent p50 delta on the 10k arms
    with one build on both servers, which retires those arms as evidence
    below ~50 µs and puts §9's unattributed −17.9/−21.3 µs inside the
    floor. Second, the review's own simplification had **widened** the
    local two-step path - folding the shape rules into the planner made
    every two-step chain pay two `InitTableAccess` lookups before any
    shape test - closed on that finding by splitting the chain-only
    `TwoStepPipelineEligible` out, so the cheap question is asked first
    and the rule still has one home. **Not measured, by construction**:
    the pipeline itself (no statement shipped - every relation sat on the
    session's core), so the per-input-row runner cost is still P4e's;
    and the local two-step projected join, which no documented driver can
    A/B inside one run.
    **What remains of 4c**: the per-batch runner handle, the sub-chain
    await decision, and the frame re-open caveat 4b inherits from the
    re-Bind fix.

    **The ship-time downgrade — added 2026-08-18.** An index or Cabin
    probe carries core-local structure state the descriptor cannot ship,
    and refusing those kinds outright meant a peer-owned join stopped
    answering the day its join column gained an index (opened by equality
    propagation, widened by IX17 — `docs/inflight/known-gaps.md`'s closed entry).
    `ShippedForm` (`step_descriptor.cpp`) now sends such a
    step as the walk it would fall back to anyway — `kScan`, aux dropped,
    residual intact, sound by the residual property — at all three encode
    seams (the single-step open, the pipeline's leaf, its consuming
    stage), and `TwoStepPipelineEligible` admits the kinds into the
    walked class under the same outer-row requirement. The peer pays the
    walk, never an error; re-deriving the structure from the peer's own
    catalog is the recorded improvement.

    **The per-batch runner handle: justified, designed, and blocked on a
    catalog change - surveyed 2026-08-15.** P4e priced it at 0.626 µs
    per forwarded row, 1.5x the whole local per-row cost of the same
    join, so it is worth building. What each input row pays today, from
    `ExecuteAsync` down: a coroutine frame for `ExecuteAsync`, a second
    for `ChainRunner::Run`, a `Budget` copy, a `ChainRunner`
    construction (four vectors - `bound_`, `schemas_`, and the frame's
    `bases_`/`values_`), a `Bind` that calls `InitTableAccess` per step,
    and a `frame_.Open`. The shape: hold one runner per stage in
    `RunConsumer` and re-enter it per row, so `Bind` and `Open` happen
    once per *batch* rather than once per row.
    **What blocks it, and it is not effort.** A held runner caches
    `TableAccess*` borrows, which is exactly the use-after-free P4d-4a
    fixed by re-Binding after every park - so re-entry must re-Bind
    whenever the catalog cache turned over. The obvious guard is
    `Catalog::catalog_version()`, and it **does not work here**:
    `InvalidateFromPeer()` - the `kCatalogInvalidate` handler, which is
    the *only* invalidation a peer ever sees, and peers are where stages
    run - deliberately clears every cached fact **without bumping the
    version**, because that counter is per-instance and means nothing
    across cores (catalog.hpp says so at its declaration). An epoch
    check on it would therefore miss precisely the invalidation that
    matters and hand a stage a freed schema.
    So the prerequisite is a **cache-generation counter that every
    invalidation path bumps, `InvalidateFromPeer` included** - a change
    to the catalog's documented invalidation contract, in a subsystem
    this milestone does not own. Left as a stated prerequisite rather
    than taken: it is a correctness contract, and the failure mode if it
    is wrong is the silent one 4a already caught once. Target for the
    work when it happens: beat 0.626 µs/row
    (`bench/results-crosscore-pipeline.md` is the measurement, and
    `bench/crosscore_pipeline_bench.cpp` re-runs in seconds).
  - **P4e — equivalence + the benchmark**. **The equivalence half landed
    2026-08-15**: `CoreRuntimeTest.
    EveryShippableShapeAnswersExactlyWhatLocalExecutionAnswers` proves
    the pipeline's reply is the local reply byte for byte, over nine
    shapes - the P4c star read of each relation, the 4b-3 join, that
    join with its projection reversed and with the inner column alone
    (the output spec is what carries order, so a wrong spec scrambles
    exactly these), a residual on the leaf, a residual on the consuming
    stage, both at once, and an empty answer - against data holding a
    key that matches nothing and a key two outer rows share, so a miss
    and a fan-in are both covered. The design is what makes it a proof
    rather than a typed-string check: **one dataset, two dispatchers
    differing only in `core_id`**, both over the same catalog and the
    same pages, so the relations' `owner_core=1` makes one side run
    locally and the other ship. A counter of stages actually opened on
    the far core is asserted to rise per statement - without it the
    test could degrade into comparing two local runs and still pass,
    which is the one way an equivalence test lies.
    **The benchmark half landed 2026-08-15, and P4e is complete.** Two
    results, because the obvious driver could not reach the code.
    *First*, the isolation re-run this workplan asked for was attempted
    with `tools/multicore_benchmark.py` extended to take `--placement`,
    and it **cannot run**: rotation does place the relations on core 1,
    and then nothing can write them - cross-core writes are refused
    (CC3), DML shipping is unbuilt, and core 0 alone listens, so **a
    peer-owned relation has no writer**. The driver now probes with one
    INSERT and reports that, which turns the constraint into something
    reproducible in ten seconds (`bench/results-multicore.md`). The old
    prediction there named the pipeline and placement but not a writer,
    which is the binding constraint now that the other two exist.
    *Second*, the pipeline was therefore priced where it **can** be
    reached - in process, one dataset through two dispatchers differing
    only in `core_id` (`bench/crosscore_pipeline_bench.cpp`,
    `bench/results-crosscore-pipeline.md`, Release, 400 interleaved reps
    per size, five sizes, both paths gated on byte-identical replies):

        shipped - local  =  2.52 us  +  0.626 us per forwarded row

    **That is the carried per-input-row runner cost, and it settles 4c's
    open question: the per-batch runner handle is worth building.** The
    slope is flat from 128 rows up (0.652 / 0.623 / 0.628 across a 16x
    span), so it is a genuine per-row charge and not an amortized fixed
    cost; and at 0.626 µs it exceeds the entire local per-row cost of the
    same join (0.417 µs), leaving the shipped path at **2.50x local**
    with 60% of a shipped row's cost being pipeline overhead rather than
    join work. Not measured, by construction: the ring, the socket, and
    the *benefit* side - a loopback harness on a 2-vCPU box prices the
    cost of shipping with the parallelism removed, and the benefit needs
    the writer named above.
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
representation is an explicitly open decision (`docs/spec/sched.md` §3 and §10).
Rewriting the executor into a state machine would settle that decision by
precedent, at the largest possible scale, without anybody deciding it.

**The decision landed 2026-08-05: C++20 stackless coroutines**
(`include/kds/sched/coro.hpp`, `docs/spec/sched.md` §3). `co_await WaitFor{&flag}`
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

**Decided 2026-08-10, operator-ratified: page ownership becomes a function
of the catalog (crosscore.md CC7).** The lease answers "who may allocate
this id"; the catalog answers "whose pages are these now" — and the second
fact wins, realized at DDL publish by the flush-then-grant handoff CC7
specifies. The remaining P6 tasks:

- **P6b — the publish handoff — built (2026-08-10)**:
  `DevicePageStore::GrantFaultPages` holds CC7 fault grants (consulted by
  `MayFault`, never `MayWrite`), `kRelationFaultGrant` (21) carries an
  `ExtentGrantPayload`, `CoreRuntime::GrantRelationFault` is the receive
  side, and `RelationFaultExtentOf` computes the extent-aligned cover of a
  relation's roots. The pinned negative became the positive contract
  (`CoreRuntimeTest.AGrantedPeerFaultsARelationsDataPagesReadOnly`): after
  the grant the peer faults the root read-only and `InitTableAccess`
  resolves.
- **P6c — placement re-enable — built (2026-08-10)**:
  `AssignOwnerCore(policy, ...)` takes a `PlacementPolicy` — `kCreatingCore`
  (default, unchanged behavior) or `kRotate` (M1's rotation over non-system
  cores) — selected by the **`placement` config key** (`creating` |
  `rotate`, default `creating`). `Catalog::SetRelationPublishHook` fires at
  the end of any CreateTable whose owner is not the system core, and the
  Expeditor's installed hook flushes the relation's extent range and sends
  the `kRelationFaultGrant` — the production send P6b deferred. Rotate is
  deliberately **not** the default: statements still all run on core 0, so
  a rotated relation is refused retryably by the affinity check until
  dispatch (P4's remaining half) lands — the key exists to exercise the
  handoff end to end, and the placement tests pin both the rotation math
  and the default staying put.

The second, independent blocker behind it — **a peer cannot INSERT**,
because `AllocateRowId()` bumps `next_id` on a catalog page — is **built
(2026-08-10), P5's shape**: `catalog::RowIdLeaseTable`
(`catalog/row_id_lease.hpp`) holds per-relation leased blocks; a non-zero
core's catalog has one installed, so its `AllocateRowId()` issues from the
block with no catalog write and answers **retryable exhaustion** when
spent — never OutOfRange, which stays the truthful spelling for the 40-bit
ceiling itself. `kRowIdLease` (22) carries the request/grant both ways
(`row_id_lease_service.hpp`, the extent service's shape): core 0's handler
carves blocks with `AllocateRowIdRange()` — the bulk-INSERT primitive,
already ceiling-checked — and a zero-count grant means "none", so a waiter
fails honestly instead of hanging. Default block 4096, K-M2's measured
floor reused rather than re-decided. Blocks are disjoint from core 0's own
ids by construction (one sequence, one writer), which is K1's issue-once
contract across cores, and the contract test pins it. What a peer still
cannot do is *run* the INSERT — that is the pipeline.

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
   ownership from page ids, hashes, or topology. (v2: range resolution
   likewise — `sys.ranges` is a catalog relation, `crosscore.md` CC9.)
5. All cross-core messages are POD, tagged, and processable after the
   originating request is gone (discard-by-tag is normal operation).
6. Every new mechanism lands with its simulated-transport test in the same
   change; a cross-core code path without a deterministic test does not
   merge.
