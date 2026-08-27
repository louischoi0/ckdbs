# Workplan — a writer for peer-owned relations

Owning specs: `docs/spec/crosscore.md` (M3, CC3, §6), `docs/inflight/in-progress/workplan-crosscore.md`
(P5, P6, CC7). This file owns the *task series*; every decision it depends on
belongs to those two and is cited, never restated.

Scoped 2026-08-21 on `crosscore-peer-listener` at `aa3e26c`. **PW1 built the
same day** on that tree; PW2-PW6 are not built, and every claim about them is
a read of the source with its site named rather than a measurement.

**What PW1's measurement can and cannot say.** The suite is green (2486/2486,
Debug, `KDS_WITH_TLS=OFF`). For overhead, `kds_txn_bench` was run Release and
interleaved against `aa3e26c`, six reps in both A/B orders — and then a null
control ran the *same base binary* on both sides of the same interleave and
produced a +0.040 µs whole-transaction delta of its own, with INSERT p50
spanning 2.53–2.63 µs run to run. **That harness does not resolve differences
of PW1's size**, so the honest statement is a structural one: `Next()` is
byte-for-byte the path it was, the added branch is in `ReserveBlock` and runs
once per 4096 ids, and a single-core instance gains one handler registration
at startup and nothing on the statement path. A per-statement claim finer than
±2% needs a harness this one is not.

## 1. What this closes, and why it is the binding constraint

`docs/inflight/known-gaps.md` states it: cross-core writes are refused (CC3), DML
statement shipping is unbuilt, and core 0 alone listens, so **a peer-owned
relation has no writer**. `tools/multicore_benchmark.py --placement rotate`
places relations on core 1 and then nothing can populate them.

The consequence is not a missing feature, it is a missing *number*. P4a-P4e
are complete and priced at `2.52 µs + 0.626 µs per forwarded row`
(`bench/results-crosscore-pipeline.md`) — 2.50× local — because a loopback
harness with no peer writer prices the cost of shipping with the parallelism
removed. `bench/results-multicore.md`'s 1.05× is a parity baseline and
cannot become anything else until a peer can be written to.

## 2. The premise that survived, and the one that did not

**Survived: this needs no CC3 lift and no 2PC.** CC3 binds *a transaction's
writes* to one home core because LSNs are stream-local
(`workplan-crosscore.md` guideline 3). A session accepted on core 1 writing
only core-1-owned relations is single-stream by construction — exactly what
`Session::MayWriteOn` already admits (`include/kds/server/session.hpp:139`,
`kUnbound` until the first write). The restriction half is not in the way.

**Did not survive: "the listener is the work."** The listener is the last
task in this series, not the first. Three core-0-owned write points sit on
the ordinary write path, and each is verified below.

## 3. The blockers, each with its site

### PW-B1. A peer cannot issue a transaction id at all — **built 2026-08-21**

**Closed on `crosscore-peer-listener`.** `RingMessageKind::kTrxIdLease` was
declared by P1 and unsent; it now carries the service in
`include/kds/server/trx_id_lease_service.hpp`, `row_id_lease_service`'s shape
with no oid in any payload, because this sequence is per-instance rather than
per-relation. A peer's `TrxIdSequence` takes a `TrxIdLease*`
(`SetLeaseSource`, `Catalog::SetRowIdLeases`'s idiom) and draws its windows
from grants; core 0 answers from `TrxIdSequence::Carve()`, the **one** place a
block leaves the superblock, which its own windows now come through too. The
request rides the WAL drain cadence at `low_water()`, one in flight per core,
because `Next()` runs inside a statement and cannot await a grant.

Two things the build had to correct, both of them predicted in the code it
touched:

1. **The reserve arithmetic.** `ReserveBlock` computed its ceiling as
   `next_ + kTrxIdBlockSize`. With a peer's block raising the durable ceiling
   above core 0's `next_`, that computes a ceiling *below* the durable one,
   which `SetNextTrxId` refuses — so core 0's next reserve failed outright,
   and before failing it could hand a peer ids core 0 had already issued.
   `Carve` takes from `superblock_.next_trx_id()` instead, which is
   behaviour-identical on one core and the difference between correct and
   impossible on two. Three tests fail against the old arithmetic, verified;
   one of them is the reissue.
2. **The mount check compared against zero.** `CoreRuntime::Open` refuses a
   mount whose recovered stream names an id above `superblock_.next_trx_id()`
   — and a peer's `superblock_` is default-constructed, so that bound was 0.
   Harmless only while a peer's stream named no transaction of its own, which
   is exactly the state PW1 ends: the first peer that wrote and remounted
   would have refused its own mount. Core 0's ceiling now travels in
   `CoreRuntime::Config` beside the WAL anchor, for the same reason and with
   the same comment. **Not end-to-end tested** — a peer stream that names ids
   needs a peer that checkpoints, which is PW3; the config plumbing and the
   no-false-refusal direction are covered, the refusal direction is not.

**The review found one live defect, and it was a design error rather than a
slip.** `low_water()` measured only the sequence's own window, but a grant
parks in `TrxIdLease::pending_` and does not install until the window is
spent — so the mark stayed up across the whole refill and
`MaybeRefillTrxIds()` asked again on the next tick, forever. On an *idle*
peer at the default 1 ms drain cadence that is a superblock write plus a full
`Expeditor::Sync()` on core 0's reactor thread every millisecond, and 4 M
transaction ids per second burned out of a space invariant 12 forbids
wrapping. `low_water()` now counts the pending grant, with
`APendingGrantClearsTheLowWaterMark` pinning it — verified to fail against
the unfixed build. The cause is worth keeping: this is the one point where
the lease may **not** copy `LeasedIdSource`, which installs its extent inside
`Grant` and so drops its own mark when the grant lands.

Also taken from that review: the request payload's caller-supplied `count` is
gone — `Carve` clamps at `kMaxTrxId + 1` and grants what it clamped to, so a
count on the wire let one malformed message consume the instance's whole
48-bit space. The grant size is fixed at registration now, which is what
`RegisterExtentGrantHandler` already did and what the row-id service
diverged from.

The persist-after-mutate ordering named below is **not** fixed, and the
reason is now written at the site: a lost persist leaves the in-memory
ceiling *above* the durable one, so the next carve starts higher and burns
the difference, while rolling it back is what would reissue an id. The case
that made repeated advance pathological was a peer's refusing callback, and
a peer no longer reaches it.

### The original statement of PW-B1, for the record

`TrxIdSequence` constructs with `next_ == ceiling_ == superblock.next_trx_id()`
(`include/kds/txn/trx_id.hpp`), so the **first** `Next()` takes the
`next_ >= ceiling_` branch into `ReserveBlock()`, which calls `persist_()`.
On a peer that callback is `CoreRuntime::Open`'s refusal
(`src/server/core_runtime.cpp:131`):

> core N cannot raise the transaction-id ceiling; the superblock belongs to
> the system core and per-core id leases are workplan P5

So a peer's write dies at its first id, before it reaches a page. Reads are
unaffected: a read view is minted from `peek()`, which issues nothing.

This is the same shape as row ids — and **the claim that "that half is
already built" was wrong, found at PW1's review 2026-08-21.** The *receiving*
half is: `catalog::RowIdLeaseTable` is installed into every non-zero core's
catalog, `AllocateRowId()` draws from the block, and the grant handler and
receiver are both wired. The *asking* is not — `RequestRowIdLease` has **zero
callers** in the tree. Since `AllocateRowId` short-circuits to the lease
whenever one is installed (`src/catalog/catalog.cpp:1907`), and nothing ever
grants that table anything, a peer INSERT still fails at its row id with
`ResourceExhausted`, permanently.

**So PW1 does not by itself produce a writing peer**, and this workplan said
otherwise. It removes the first of two closed doors. The second is
**PW1b**, below, and it is not the wiring omission it looks like: a row-id
lease is *per relation*, so a pre-emptive low-water tick — the trick PW1 uses
for the per-instance trx-id sequence — has no relation to name. Something has
to decide which relations a peer leases ids for, and that is a design
question, not a missing call.

**One ordering constraint is a correctness statement, not a preference.**
`CoreRuntime::Open` refuses the mount when a peer's recovered stream names a
transaction id above the superblock's ceiling
(`src/server/core_runtime.cpp:88`). That refusal stays sound only if core 0
**persists the raised ceiling before granting the block**. Grant-then-persist
would let a crash produce exactly the log that refusal describes, and the
mount would fail on a database that did nothing wrong.

**A latent defect to fix while here**, unreachable today and not so once
leasing lands: `ReserveBlock` calls `superblock_.SetNextTrxId(ceiling)`
*before* `persist_()`, so on a peer every failed attempt advances the local
copy's ceiling by `kTrxIdBlockSize` while `next_` and `ceiling_` stay put.

### PW-B2. Two catalog write points ride the ordinary INSERT

A peer faults catalog pages read-only and `DevicePageStore::MayWrite` enforces
it (`src/storage/device_page_store.cpp:363`). Two write paths reach a catalog
page from inside a plain INSERT:

- **`Catalog::UpdateRelationDescPage`** — the clustered B+ tree grew a level,
  so `sys.tables.desc_page_id` must be repointed
  (`src/server/command_dispatcher.cpp:3219`).
- **`Catalog::UpdateIndexRoot`** — a secondary index's root moved
  (`src/exec/index_maintain.cpp:199`). `catalog.cpp:2816` says it in as many
  words: *"A root moves when a split grows the tree, which happens inside an
  ordinary INSERT."*

**The scope-shrinking fact is that this is not uniform.** `InsertPlacement`'s
`new_root` is *"Always kInvalidPageId for a heap chain, which has no root to
move"* (`include/kds/storage/insert_placement.hpp:76`). So:

| relation shape | catalog page written by INSERT |
|---|---|
| heap-clustered, no secondary index | **none** |
| BTREE-clustered | `sys.tables`, when the tree grows a level |
| any, with a secondary index | `sys.indexes`, on a root split |

A peer can therefore write a heap relation with no secondary index the
moment PW-B1 falls, and nothing else until PW2 decides how a root move
reaches core 0.

### PW-B3. A peer has no checkpointer — **built 2026-08-21**

**Closed.** `CoreRuntime` now owns a `PageStoreCheckpointTarget`, a
`RemoteCheckpointAnchor` and a `wal::Checkpointer`, built at
`AttachTransport()` rather than `Open()` for the one reason that placement
has: the anchor publishes over the ring, so it cannot exist before the ring
does. Two things run off it — the **completion checkpoint** (RC08's half for
a peer, at `AttachTransport`, so a mount bounds the next crash) and the
**cadence** (`wal.md` §11, in `Run()`, gated on `checkpoint_interval_ns`).
Peers only: core 0's checkpointer is `Expeditor`'s, and a core-0
`CoreRuntime` — which exists only in tests — would send its anchor to itself.

`APeersCheckpointAnchorReachesCoreZerosSuperblock` asserts the end of the
path rather than the send: core 0's superblock carries core 1's anchor, and
a second checkpoint advances it rather than republishing the first. Verified
to fail with the checkpointer removed.

**Its review found a silent-corruption route PW3 armed**, fixed with it.
`DevicePageStore::FlushMaps` is the one write path that reaches
`device_.WritePage` without asking `MayWrite`, and it writes the two map
pages a peer may read and never write. A peer acquires a dirty map bit at
mount — redo's `CreateAt` runs *before* `SetCoreOwnership` installs the
lease, which `core_runtime.cpp` orders that way deliberately — and until PW3
nothing on a peer ever called `FlushPages`, so nothing ever flushed it. The
checkpointer is the first caller. A peer's cadence checkpoint would have
written back the free map as it stood when that store opened, reverting every
allocation and extent reservation core 0 had made since: silent reuse of live
pages, not a lost bit. Guarded at `FlushMaps` with the reason at the site, and
pinned by `ALeasedStoreNeverWritesTheMapsBackToTheDevice` — the existing
`ALeasedStoreNeverMutatesTheFreeMap` group covered the *set* half and never
the *write-out* half. Verified to fail unguarded.

Two decisions inside it:

- **The assertion snapshot source is wired even though it writes nothing.**
  A peer's registry is empty — `ResumeAssertionsAfterRecovery` runs only on
  core 0 — so no group snapshots are written today. Omitting the wiring
  would be the silent kind of gap: a peer that later enforces would
  checkpoint without snapshots, and the next mount would find no base to
  fold from, which is exactly the failure RC07 exists to prevent.
- **`StartWriter()` is *not* called for a peer**, and that was checked
  rather than assumed. `WalManager::Sync()` runs on the calling thread
  always, writer or not, by an explicit decision in its own header — so
  `SyncAll`, `EnsureDurable`, D1's commit and the checkpoint's own gate
  (`Checkpointer::Complete` → `EnsureDurable`) are byte-identical with and
  without one. **The review named the one path that is not**: `DrainOnce`'s
  D3 relaxed branch hands its flush to the writer when there is one and falls
  through to `Sync()` on the reactor when there is not. `Run()` arms that
  drain both as a post-task hook and as a timer, so under
  `durability = relaxed` a peer charges that fsync to its reactor where core 0
  does not — 2,208 µs against 194 µs at p99, by the number recorded at that
  site. Free before PW3, because a peer's stream was empty; not free from
  here, because it now carries this feature's own `CHECKPOINT_BEGIN`/`END`.
  Still not PW3's to change, and now named rather than waved at.

  Two adjacent staleness findings, neither PW3's: `expeditor.cpp`'s "every
  sync moves to the WAL writer thread… and the checkpoint gate's" is wrong on
  two of its three items, and a peer's WAL opens with a default
  `WalManagerConfig`, so its `relaxed_flush_interval_ns` is the 10 ms default
  rather than the configured one — load-bearing from this commit on.

### The original statement of PW-B3, for the record

`CoreRuntime` submits a WAL drain cadence and a lease refill
(`src/server/core_runtime.cpp:377`, `:385`) and **no checkpointer** —
`checkpointer_` is an `Expeditor` member, core 0's. A peer that starts
writing would grow a stream whose anchor never advances, so every subsequent
mount replays it whole, and `docs/inflight/known-gaps.md`'s already-open mount-latency
entry gets a second multiplier.

The sending half exists and is unwired: `RemoteCheckpointAnchor` publishes a
peer's anchor through core 0, fire-and-forget, and its header explains why
that is sound (`include/kds/server/remote_checkpoint_anchor.hpp`).

## 4. In scope, secondary

- **Auth and TLS live on core 0's stack.** The credential store and TLS
  context are built inside `Expeditor::Serve`
  (`src/server/expeditor.cpp:1013`, `:1026`, `:1076`). Per-core listeners
  need them shared immutably or built per core; rules.md #3 means nothing may
  be shared mutably by default.
- ~~**DDL on a peer needs a named refusal.**~~ **Built 2026-08-24, PW4** —
  refused at dispatch by `PeerDdlRefused` wherever the catalog is read-only,
  instead of reaching `MayWrite`'s page-id failure.
- **A peer records nothing.** `waystone_recording` and `access_statistics`
  are off on a peer by construction, because `sys.patterns` and
  `sys.access_stats` are catalog pages written on the statement path
  (`include/kds/server/core_runtime.hpp`). Advisory under invariant 8, so
  rows are identical — but a peer-served benchmark measures an engine with
  its optimizer input switched off, and a results file must say so.
- **A peer takes no DDL, and one soundness argument depends on it.**
  `command_dispatcher.cpp:3546` gates the §5d delete-mark purge to the system
  core because a peer's `ReadHorizon()` answers `UINT64_MAX`. Its comment
  says the property is *"enforced nowhere"*. The DDL refusal above is what
  would enforce it.
- **FK and assertion co-location.** `crosscore.md` §6 requires write-coupled
  auxiliaries to live on the relation's owner core. A peer INSERT into a
  child whose parent is core-0-owned is a cross-core read. Whether a Bound
  Cabin's entry pages follow the CC7 grant was **not verified in this pass**;
  it must be before an assertion-carrying relation is placed on a peer.

## 5. What per-core listeners do not buy

`crosscore.md` M3 is already ratified — SO_REUSEPORT per-core listeners, the
kernel distributes connections, a session lives on the core that accepted it,
never rebalanced in v1. That means **a client cannot choose its core.** A
connection that lands on core 1 and needs to write a core-2 relation gets
CC3's retryable refusal, forever, however many times it retries the
statement.

For the benchmark this is fine and even honest: N connections, each writing
the relation its own core owns, is the shape a shared-nothing engine claims
to scale on. For a general workload it is a cliff, and the fix is the
*other* route — §6's DML statement shipping, which ships a write statement
whole to the owner core and *"involves no pipeline"*. That route is not in
this series and is not blocked by it; both need PW-B1.

`TcpServer` itself is ready: `Listen(port)` and
`Attach(scheduler, dispatcher, log)` already take the scheduler and
dispatcher as parameters (`include/kds/server/tcp_server.hpp`), so a
per-core instance is a `SO_REUSEPORT` setsockopt beside the existing
`SO_REUSEADDR` (`src/server/tcp_server.cpp:43`) plus one `TcpServer` per
`CoreRuntime`.

## 6. Task series

| # | Task | Gate |
|---|---|---|
| PW1 | **Built 2026-08-21.** Trx-id lease over the ring (`kTrxIdLease`), mirroring `row_id_lease_service`. Core 0 persists the ceiling before granting; the reserve arithmetic and the mount check's zero bound corrected with it | none |
| PW1b | **Built 2026-08-21.** A peer asks for row-id blocks: the miss records the demand, the drain tick answers it. Decision taken — **demand-driven, not pre-emptive** (§7a) | PW1 |
| PW1c | **Write rights on a peer-owned relation's pages.** Found by probe while building PW1b, not by reading: with both leases in hand a peer INSERT still fails, `core 1 may not write page 130`. `MayWrite` grants a peer only pages from its own *extent* lease, and CC7's grant is fault rights only, deliberately. ~~Needs a decision — §7b~~ **Decided 2026-08-24 (§8, the write handoff riding PL-B); PW1c-1..5 built 2026-08-24 — a funded peer INSERTs end to end — PW1c-6's grant-extension half remains; the PW1c-4r re-grant debt closed the same day as PW1c-7, ownership surviving a restart by the stamp** | PW1, PW1b |
| PW2 | Route the two root-move catalog writes. ~~Needs a decision — §7~~ **Decided and built 2026-08-24** (§7, §7a — operator-delegated): the per-relation **anchor page** makes both catalog columns CREATE-fixed, so a root move writes the mover's own granted page; PW2-1..4 built, the btree shape gate lifted. The indexed gate stays until PW1c-6's grant extension | PW1, PW1b |
| PW3 | **Built 2026-08-21.** A peer checkpointer through `RemoteCheckpointAnchor`: the completion checkpoint at `AttachTransport`, the `wal.md` §11 cadence in `Run()` | PW1 |
| PW3b | ~~The **shutdown** checkpoint, which PW3 did not ship — core 0 has three checkpoint points and a peer now has two. A graceful restart replays up to one `checkpoint_interval_ms` of every peer's stream. **Needs a decision**: after the worker join both reactors are stopped, so a queued anchor send is never polled; it wants either one more core-0 ring drain after the join, or a different anchor on the shutdown path~~ **Decided and built 2026-08-25** (worktree `pw3b-peer-shutdown-checkpoint`), under §8's delegation precedent: **the shutdown anchor goes direct.** After the worker join the startup thread owns every core — the peer's runtime, as `Sync()` already assumes, and core 0's, whose reactor ran on that thread — so `CoreRuntime::ShutdownCheckpoint(system_anchor)` flushes the peer's pages (`DevicePageStore::Sync`, the guarded map no-op included), routes its `RemoteCheckpointAnchor` to core 0's own `SuperBlockCheckpointAnchor` (`RouteDirectly`) and runs `Checkpoint()`; `Expeditor::Serve`'s tail calls it per peer right after that peer's final `Sync()`, before `cores_.clear()`. The page flush comes first for the reason core 0's final sync precedes its checkpoint — an empty dirty table makes the redo start the `CHECKPOINT_BEGIN` LSN itself. **The ring drain was rejected**, and the header of `remote_checkpoint_anchor.hpp` carries why: a peer's ready queue after `CloseListener` can hold a task whose session is gone (the PW5 review's BUG 3, still open), core 0's ring can hold any pending request whose handler would then run against peers mid-teardown, and `BroadcastShutdown`'s hand pump is bounded and best-effort by construction — right for a stop message, wrong for the one anchor no later checkpoint makes up for. Fire-and-forget's soundness argument ("a later checkpoint follows") is exactly what the last checkpoint lacks, so the direct route is the argument's own consequence rather than an exception to it. `SuperBlockCheckpointAnchor` stays the one piece of code that reaches page 0. **Also here, because the test needed it and an operator does too**: a peer's `MountRecovery` was discarded at `Open` beyond `next_trx_id`; it is kept now (`CoreRuntime::recovery()`), wired into the dispatcher so a peer's `SHOW META` carries the RC09 recovery block (`docs/spec/client-manual.md`), and the completion checkpoint is timed into it as core 0's is. Pinned by `AMountAfterAPeersCleanStopDoesNotRereadTheRunsWholeLog`: 200 rows on a rotated relation, the tail's `Sync()` + `ShutdownCheckpoint` through a real `SuperBlockCheckpointAnchor` with no reactor pumped, a remount from the anchor Expeditor copies — `recovery_records < 20`, `redo_applied = 0`, all 200 rows — beside a control iteration stopped the old way that re-reads more than 200. **And measured end to end on a real two-core server** (`placement = rotate`, `peer_listeners = on`, the cadence set past the run so nothing else could publish, 200 rows written through a core-1 session into a core-1-owned relation, `STOP`, restart): the peer's next mount reads **`recovery_records=2 recovery_redo_applied=0`** with all 200 rows present, against **`810` and `404`** — 200 transactions re-analysed — from the same binary with the one call disabled, while core 0 reads 2 either way because it has had its own since RC08. That is the core-0 property of `docs/inflight/known-gaps.md` ("2 records where it read 1205") holding on a peer for the first time. Not mirrored in `SimInstance`: the harness is single-core. **The test's first run found a pre-existing defect, fixed here** (`docs/inflight/known-gaps.md` carries the entry): the clean-stop remount answered `page id not found` because the relation's growth page sat in an extent the device's free map did not hold — `DevicePageStore::free_map_bytes()` marks the map dirty when the span is *taken*, `ExtentAllocator` held that span for its life, so every reservation after core 0's last flush went into a map the next flush skipped as clean; redo's `CreateAt` had re-created such pages at every remount, which is exactly what a checkpoint past their PAGE_INITs stops. `extent_lease.hpp`'s "durable exactly when the store next flushes" was false, and its "a crash burns the extent" has been a crash *freeing a used run* since PW1c — core 0's next-mount allocator could hand it out over a peer's committed rows. Closed on both halves: the production `ExtentAllocator` is built over the store (every reservation marks the map; the raw-span form stays for scripted-map tests), and the extent grant handler calls `ExtentAllocator::Persist()` — `DevicePageStore::PersistMaps`, the map pages and a device sync, not the frames — **before the grant leaves**, a run that cannot be made durable answered with the zero-page reply. Pinned by `AReservationAfterTheLastFlushIsLandedByPersist`; the defect's observed failure is the PW3b test's own first run. Overhead not measured (the v2 amendment): the shutdown costs one page flush, one checkpoint and one superblock sync per peer, none on any statement path; the persist costs core 0 one map write and one fsync per 64-page extent granted. **The review** (`critics-developer` over the uncommitted diff) **found two defects in this task's own evidence, both fixed and both re-verified by reverting the fix and watching the test fail**: C1 — `AReservationAfterTheLastFlushIsLandedByPersist` passed against the cached-span implementation it was written to condemn, because the store-form *constructor* takes `free_map_bytes()` and so marks the map dirty itself; the allocator is now built before the flush, production's own order, and the side effect is named at the constructor; C2 — the peer's completion checkpoint was timed into `recovery_.checkpoint_ns`, which `SHOW META` suppresses unless `timings.timed`, set only when *recovery* got a clock — so a peer printed seven fields where core 0 prints twelve and the "timed as core 0's is" claim was false; `RecoverCoreAtMount` now takes the clock, pinned by an added assertion. C3, applied here rather than by the reviewer: the direct route is **cleared again after the checkpoint** — `Expeditor` declares `cores_` above `checkpoint_anchor_`, so reverse-order destruction would leave an armed route pointing at a destroyed anchor (unreachable only because `Serve` clears `cores_` first), and an armed route on a running core would have a peer's thread write page 0. C5: the page flush moved above the no-checkpointer guard, since the flush is this core's own work. **Two findings recorded and not fixed**: C4, that one failed checkpoint disarms every later one on that core (`in_progress_` clears only on `Complete()`'s success path, `Start()` refuses while it is set, and nothing resumes a half-finished checkpoint) — pre-existing on core 0 too, load-bearing here because it now costs the graceful-restart bound, and the repair is a `wal.md` §11 behaviour decision, so it went to `docs/inflight/known-gaps.md`; and S6, that `Serve`'s per-peer call site has no test at all, since nothing builds an `Expeditor` with `cores > 1` — the property is pinned at the `CoreRuntime` level and the wiring was exercised by hand against a two-core server. S5 is closed rather than recorded: `AGrantedExtentIsOnTheDeviceBeforeTheGrantLeaves` drives a store-backed allocator through the real grant handler, crashes the device and reopens it, and was verified to fail with the persist disabled. **S4 declined**: sharing the PW3 test's transport/scheduler/receiver rig would hand the two tests one setup whose halves they use differently — the PW3b one loops twice, captures the mount anchor between the phases and funds a relation — and the fixture's rig precedent (`OpenForeignIndexRig`) exists for a rig with one shape, which this is not | PW3 |
| PW4 | **Built 2026-08-24** (`r1-peer-ddl-refusal`). CREATE/ALTER/DROP refused whole at dispatch wherever the catalog is read-only (`SetCatalogReadOnly`, set by `CoreRuntime` for every non-system core; `PeerDdlRefused` names the core and where DDL runs). Predicated on the incapacity rather than `core_id_`, so the P4e harness's core-1 stand-ins over a writable store keep building fixtures. §5d's purge gate cites the guard and stays as defense in depth | none |
| PW5 | **Built 2026-08-24 with a named restriction** (`r1-peer-ddl-refusal`): `peer_listeners = on` binds every core's listener with `SO_REUSEPORT` (core 0's socket carries the flag too - the first binder must), `CoreRuntime::ListenAndAttach` wires each to its own reactor and dispatcher on the startup thread. **The tls/auth half is not built**: the combination is refused at boot (`CheckPeerListenerConfig`), because the credential store and TLS context are constructed on core 0's stack and sharing them immutably is still open. Off by default | PW1, PW4 |
| PW7 | **Lease refills under load — traced and fixed 2026-08-25** (worktree `lease-refill-lag`), the job PW6's four-writer cell opened. **The instrument**: every refill carries `LeaseRefillStats` (`include/kds/server/lease_refill_stats.hpp`) — requests, grants, and the longest wait split into submit→sent (the scheduler queueing the request task), sent→grant received (the ring and core 0), grant received→resumed (this reactor reaching the parked coroutine), each in nanoseconds *and* in reactor iterations (`Scheduler::iterations()`), so a long-in-time-short-in-iterations leg reads as a blocked loop and long-in-both as a loop that ran and never reached the task; a peer's `SHOW META` prints them per kind (`docs/spec/client-manual.md`) and `tools/multicore_benchmark.py` reads them after every peer cell. **The trace** (four writers on core 1, 200 rows, quiet box): the row-id refill's longest wait was **547 ms, of which submit→sent 546 ms over 395 iterations, sent→grant 1.2 ms, resume 0** — the ring, core 0 and the resume were innocent; the reactor iterated 395 times without polling the freshly submitted refill task. **The mechanism**, from `RunReadyTasks`: a parked coroutine answers `kSuspended` in nanoseconds, and the loop budget re-polled the *previous* refill, parked awaiting its grant, up to 64 times an iteration while its group's ratio stayed lowest, charging every poll to the `system` group (share 50); the group then owed the `foreground` (share 1000) twenty times that debt, and because a statement's time is the drain's fdatasync — outside every group's account — the debt took hundreds of iterations to clear, during which the *next* relation's refill sat unpolled. Two floors under the share law fix it (`src/sched/scheduler.cpp`, `docs/spec/sched.md` §4): a task is polled at most once per iteration, and every group with a task ready at the start of an iteration is polled at least once; pinned by two `SchedulerTimerTest`s that fail on the old policy. **After** (measured on the tree committed as `v2.0.0-52-g2c6ae23`; the trace numbers above on the same tree without the fix): the same cell's row-id refill waits **2.7 ms** (submit 0, to-grant 1.8, resume 0.9), the four-writer cell at 200 rows runs **1.034×** the single-core configuration (0.61–0.80× before) and at 2,000 rows **0.988×** (0.802×) with **no lost rows** — the extent lease refilled twice in 2 ms, where PW6's run lost 1/13/51 INSERTs — and the peer's point-SELECT p50 is 48 µs beside three writers (934–1,139 µs), because the stalls no longer desynchronise the threads, which is exactly what PW6's §10 predicted. **Two things it also showed, recorded and not fixed here**: the trx-id refill's 39 ms spanned 108,150 iterations — a reactor with any parked coroutine spins (`IdleTimeoutMs` counts a parked task as ready), which predates this change and burns a peer's CPU while it waits on a grant; and the group accounting sees only time inside polls, so a core whose time is fdatasync charges it to nobody (`docs/spec/sched.md` §4 carries the gap). The PW6 results file stays as the record of `314a06d`; its §6a–§6b and §11 are what this row answers, and ~~the re-measurement of its matrix at the fixed engine is a `bench/v2.0.0/` file of its own, owed~~ **re-measured 2026-08-25 at `v2.0.0-67-g952bbb9`** (worktree `agent-a88b32b3e80c45166`, `bench/v2.0.0/results-multicore-writers-v2.0.0-67-g952bbb9.md`): the four-writer cell runs at 1.030× the single-core configuration with zero lost rows; the row-id refill's longest wait is 3.0–3.3 ms; the two-writer peer path runs at 0.990× against a 0.944× control of identical engines, whose 0.866 outlier that file's §3 explains as one unattributed 485 ms stall of core 0's reactor; a point-SELECT beside a committing session is 1,088/1,083 µs on core 0/core 1 against 37/35 alone. **The 2c6ae23 review, applied as a follow-up**: C1 — the receivers stamped the grant on a message's *arrival*, before the payload was validated, so a malformed or stale message could end leg 2 early and charge the rest of the wire wait to this reactor — the stamp now sits where a grant is taken, beside the coroutine's release (`NoteGrant`); C2 — the in-flight sentinel was `requested_at_ns != 0`, which a `ManualClock` starting at 0 defeats — an explicit `in_flight` flag; C3 — floor 2 held only while the loop budget was at least the group count — clamped at construction; S1 — `Scheduler::clock()` replaces the six clock parameters, six null guards and `CoreRuntime::clock_`; S2 — two dead accessors deleted; S3 — `NoteSubmit`/`NoteSent`/`NoteGrant` replace six copies of the stamp idiom; S4 — `wait_last_us`, read by nothing, dropped; S5 — the driver's post-hoc `SHOW META` read cannot lose a measured cell; C4/S6 — `docs/spec/sched.md` §4 states the floor's group order, the per-iteration `PollReady` consequence (unmeasured on the pipeline shape) and no longer says "always" | PW6 |
| PW6 | ~~The benchmark: `placement = rotate`, one writer connection per core, per-core relations. The first cross-core number that is a speedup and not a cost~~ **Measured 2026-08-25 with the host's bound** (worktree `pw6-rotate-benchmark`, `v2.0.0-48-g314a06d`, `bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md`). The client half: `SHOW META` grew `core=` (a session cannot choose its core under SO_REUSEPORT, so it must be able to see it), and `tools/multicore_benchmark.py --placement rotate --peer-listeners` hunts sessions per owner core by asking, retries retryable refusals with the whole wait as the latency, and `--verify`s the surviving row count. **The number this row asked for — a speedup from two writer cores — is unmeasurable on this host**: the server refuses `cores` above `hardware_concurrency()` (two here), and at `cores = 2` rotation skips the system core so every rotated relation is core 1's; the runnable cell is the peer write path against core 0's at equal parallelism — **two writers 0.977× against a control that measured 0.982×, every per-statement median within 2% (INSERT 1,863 vs 1,862 µs, point-SELECT 25 vs 26 µs)** — the peer path costs nothing this harness resolves, flat at 200/2,000/10,000 rows; PostgreSQL 16.14 is 7% behind on statements and 2× on pk lookups. A ≥3-CPU host runs `--cores 3 --tables 2`; the results file's §7 fdatasync-on-a-second-file probe decides first whether two writer cores can overlap their syncs on one ext4 volume. **Three findings, each recorded in `docs/inflight/known-gaps.md`, the first being the next job**: (1) **every lease refill lags by hundreds of milliseconds to seconds under four active sessions on one peer** — relations 3 and 4 wait 0.5–1.75 s for their first INSERT on the row-id refill (the servers' logs hold only `row-id lease ... is spent` refusals there and never `RelationWriteRightsPending`, so PW1c-7's request latch is off the path — the results file's first draft attributed it there from the source and was corrected from the logs), the trx-id lease is spent mid-run with a quarter-window of headroom, and the 64-page extent lease is spent so **1, 13 and 51 INSERTs per run were lost**; an idle refill is 2–7 ms, core 0 logged no failed grant, and the mechanism is untraced (`PickNextGroup` picks the ready group with the lowest consumed-time/share ratio, so starvation of the parked refill by the query group is not the obvious reading) — the trace is `SHOW META` counters per lease kind (requests, grants, longest wait in ticks) and a debug-level cell C at 200 rows; (2) **three refusals promise a retry without the wire's bit** — the row-id, trx-id and extent leases' `ResourceExhausted` prints as bare `ERR` because only `TxnConflict` is `IsRetryable`, and the extent one lost rows because a client retrying on the bit did not retry it — **closed 2026-08-25** (worktree `lease-refusal-retryable`: the three re-coded `TxnConflict`, rendered through `ErrorReply`, pinned by `ASpentLeaseRefusesWithTheWiresRetryableBit`; `docs/inflight/known-gaps.md` carries the reasoning); (3) **a point-SELECT on a core with a committing session waits out that session's fdatasync** (973 µs beside a writer, 37 alone — the drain runs inline on the reactor), the first number for the open I/O-backend decision. Overhead not measured beyond the cells (the v2 amendment) | PW1-PW5 |

PW1 + PW1b + **PW1c** make a peer write a heap relation with no secondary
index by single-row INSERT — PW1c is the door the probe found behind PW1b, and it was not in this
workplan's original three because it is not on the *id* path at all. PW1c +
PW3 + PW5 is then a shippable slice with a real number at the end of it and a
stated shape restriction; PW2 removes the restriction. Taking §7b's option
(c) would collapse PW1c and PW5 into one task and is the current
recommendation.

**Named at the PW5 review, pre-existing, not fixed there** (BUG 3): a
shutdown with a statement in flight leaks the deferred fd — `CloseClient`
defers when `conn.in_flight`, and `Detach()`'s `clients_.clear()` then
destroys the `Connection` without closing it, with a queued `CoroTask`
still pointing at the dead session. True of core 0 since the coroutine
conversion; N listeners give it N chances per shutdown. Its own item,
not PW5's.

**Deferred cleanup, with a name** (PW1's review, rejected for PW1 itself):
`trx_id_lease_service.cpp`'s receiver and request coroutine are a third
near-identical copy of `row_id_lease_service.cpp`'s, which are themselves
`extent_lease_service.cpp`'s — the two functions differ in six lines. A
shared `server/lease_refill.hpp` templated on payload and an `apply` callable
would delete ~70 lines now and ~70 more when the older two adopt it. Declined
inside PW1 because it refactors two already-shipped services under a change
that had not landed; the right moment is when a change next touches those
funnels. **Not** to be merged with it: `TrxIdLease`, `catalog::RowIdLease`
and `storage::Extent` are three id domains with three exhaustion contracts,
and they resemble each other more than they share.

## 7a. The decision PW1b took, and why

A row-id lease is per **relation**. PW1's transaction-id lease is per
*instance*, so a peer can pre-empt for it from its first tick — there is
exactly one subject and it always exists. Nothing on a peer knows that a
relation needs ids until a statement names one, so the three options were:
ask on first exhaustion, ask at first write, or ask at the CC7 fault grant.

**Taken: demand-driven.** `RowIdLeaseTable::Next` inserts the spent entry on
a miss, which turns the failure into a recorded request; `MaybeRefillRowIds`
on the drain tick asks for the neediest relation, one in flight per core;
`RowIdLease::window` gives it PW1's quarter-window low-water mark, so a
relation is topped up before it is spent and asked for exactly once.

The client-visible consequence, stated because it is a contract and not an
implementation detail: **the first single-row INSERT into a relation on a
given peer fails retryably, and no later one does.** *Single-row* is exact
and was corrected at review: the multi-row `VALUES` path calls
`Catalog::AllocateRowIdRange`, which never consults the lease table at all,
so a peer's bulk INSERT bypasses the lease and dies at the catalog page
write. That is a fifth thing PW1c or PW4 has to reach, not something PW1b
left half-done. That is exactly what that lease's
`ResourceExhausted` message has always promised — "retry after the refill
grant lands" — and it is the same retryable shape CC3's cross-core write
refusal already uses, so a client that retries on those needs no new code.

Rejected: **at the CC7 fault grant**, which would avoid the first failure.
`ExtentGrantPayload` carries no oid, so it needs a wire change; and it leases
ids for every relation placed on a peer whether or not the peer ever writes
one. The saving is one retry per relation per mount.

## 7b. The decision PW1c needs — do not assume

**A peer with both leases still cannot INSERT.** Probed rather than reasoned:
a rotated relation, a peer holding a transaction-id block, a row-id block and
a CC7 fault grant, and `INSERT INTO rotated VALUES (7)` answers

> ERR DevicePageStore: core 1 may not write page 130

`MayWrite` allows a peer only the pages its own **extent lease** owns. The
relation's pages were allocated from core 0's free map at `CREATE TABLE`, so
the tail page an INSERT appends to is core 0's, and the write is refused.

**And the refusal above is a Debug-only refusal.** Found at PW1b's review
2026-08-21 and stated nowhere else: the whole `MayFault`/`MayWrite` check
sits inside `#ifndef NDEBUG` (`device_page_store.cpp`, "the shared-nothing
check … debug builds only"). In `build-release` — where this project's
measurement rule says every number is taken — that same peer INSERT does
**not** refuse. It dirties core 0's page in the peer's own store and the last
flush wins.

So PW1c is not a door to open. It is a **silent two-writer corruption route
that opens the moment PW5 gives a peer a listener**, and it would have been
invisible in exactly the build PW6 measures. That makes **PW1c before PW5 an
ordering requirement, not a preference** — the Debug check is an assertion
of a shared-nothing invariant whose actual enforcement is statement dispatch,
and a peer listener is what removes the dispatch.

**This is not an oversight to patch.** `device_page_store.hpp` says it in as
many words — *"MayWrite deliberately never [consults the granted list] - a
grant is [read rights only]"* — and CC7 explains why: a grant is
**extent-granular**, so a granted extent may carry pages of *other* core-0
relations. CC7 calls that the "superset assertion" and accepts it precisely
because the enforced mechanism is statement dispatch, never the assertion.
A superset is safe to *fault* and not safe to *write*: it would let a peer
write another relation's pages.

So closing it means picking one, and each is a real design commitment:

- **(a) Per-relation write grants.** Make the grant carry the relation and
  its exact page range rather than an extent, and let `MayWrite` consult it.
  Ends the superset for writes; costs a wire change and a grant that has to
  be re-sent as a relation grows.
- **(b) Allocate a peer-owned relation's pages from the owner's *free-map*
  allocation at DDL.** CC7 records this as considered and rejected — "a new cross-core
  allocation protocol inside DDL that still needs a creation-time write
  exception" — but it was rejected when no peer could write at all, and the
  premise it was rejected under is the one PW1 removed.
- **(c) Ship the DML statement to the owner core** (`crosscore.md` §6's first
  bullet, "involves no pipeline"). Then no peer ever writes a page core 0
  allocated, because the *owner* executes the write, and PW1c disappears
  rather than being solved. This is the route that also fixes PW5's
  no-steering problem.
- **(d) Grant a peer-owned relation a fresh *write* extent at DDL** — core 0
  reserves an extent per peer-owned relation and grants it through the
  existing `kExtentLease` path into `lease_`, rather than as a CC7 fault
  grant, and that relation's pages are allocated only from inside it. Added
  at PW1b's review, and it is the cheapest of the four: **no wire change and
  no new protocol**, because both halves already exist. It also makes CC7's
  superset **empty by construction**, which is the precise objection that
  rules out granting write rights over an arbitrary extent. Costs one extent
  (64 pages, 512 KiB) minimum per peer-owned relation. A strictly cheaper
  cousin of (b) that avoids (b)'s stated defect — no cross-core allocation
  protocol inside DDL.

CLA's recommendation is **(d) then (c)**: (d) closes the corruption route on
its own and unblocks PW5, and (c) remains the right end state because it
makes steering a non-question. (a) and (b) are dominated by (d).

## 7. The decision PW2 needs — do not assume

A root move must reach `sys.tables` / `sys.indexes`, which only core 0 may
write. Three options, and this workplan picks none:

- **(a) Ship the write to core 0 and wait.** A request/reply on the ring
  inside an INSERT. Newly *possible* — the executor is coroutines since P4d,
  so a statement can park — and newly *expensive to reason about*: the
  statement suspends holding pins mid-insert, and the peer's INSERT is
  already logged at that point (`command_dispatcher.cpp:3219` persists the
  root only after the pages under it are logged, deliberately). A failed or
  lost reply leaves a logged tree whose root the catalog does not name.
- **(b) Make the root indirect.** A fixed per-relation page holds the current
  root, so a growth writes a relation page and never a catalog page. Removes
  the cross-core problem instead of routing it, costs one indirection on
  every descent, and is a format change to two catalog columns' meaning.
- **(c) Restrict the benchmark to shapes that never move a root.** Heap
  relations, no secondary index. Cheapest, and it must be *stated in the
  results file* — a scaling number measured only on the one shape that
  avoids the blocker is a number with an asterisk, and burying the asterisk
  is the failure mode this project's bench discipline exists to prevent.

CLA's reading was that **(c) is the right first move and (b) is the right
end state** — and events resolved it in that order. **Decided 2026-08-24
(operator-delegated, PW1c §8's precedent): (b), the indirect root, is
taken as the build.** (c) already stands *structurally*, stronger than
the benchmark restriction it proposed: PW1c-5's shape gate refuses peer
writes to btree-clustered and indexed relations at `CheckWriteAffinity`,
so no measurable shape can move a root a peer cannot write. (a) keeps
its rejection — a cross-core request/reply inside a half-logged INSERT
is the most failure-reasoning for the least structure.

### 7a. The PW2 build — the anchor page

One **anchor page per relation**, allocated at `CREATE TABLE` beside the
root and handed off with the creation pages (the write-grant set grows
to three; capacity six holds). It carries the relation's entry points -
the clustered root, and each secondary index's root keyed by index oid -
as an ordinary logged, headered, authoritative page class: losing it
loses the relation's entry points, the var-heap's argument for logging.

- `sys.tables.desc_page_id` and `sys.indexes.root` become **fixed at
  CREATE**: they name the anchor (respectively: the initial root, kept
  for diagnostics), and no growth ever writes a catalog page again -
  the cross-core problem removed, not routed. A format event: the
  columns' meaning changes, superblock version bumps, pre-existing files
  stop mounting (the development-stage policy, P0's precedent).
- A root move writes the mover's own anchor page - on a peer, a page it
  holds by grant, in its own stream, PL-stamped like any write.
- The descent reads the anchor once per bind (a resident-frame hit);
  caching the root beside `heap_tail_hint` as advisory-self-healing is
  the recorded later optimization, not the first build.
- **What it lifts and what it does not**: the btree shape gate lifts
  (btree growth writes the anchor, never `sys.tables`); the indexed
  gate stays until PW1c-6's grant extension covers index *pages*.

Staging: **PW2-1 built 2026-08-24** (worktree `pw1c1-handoff-record`):
`PageType::kAnchor` (14) with `storage/anchor_page.hpp` (clustered root
+ per-index-oid entry table, swap-remove, capacity-refusing);
`SysTableRow.anchor_page_id` + `TableAccess` (superblock 14 → 15,
key_mode's precedent; a **system** relation carries `kInvalidPageId` -
its fixed-page root never moves - and PW2-2 reads that as
"desc_page_id is the root"); `CREATE TABLE` allocates, formats and logs
it - and the durable story is PAGE_INIT **plus `kAnchorUpdate` (26)**,
the record a root move will write, because PAGE_INIT rebuilds only the
common header and the roots are body content; the publish hook and
`RelationFaultExtentOf` carry the anchor, so the write-grant set is
three pages. Nothing reads the anchor yet - behavior-identical by
construction, suite 2610/2610 (its review then hardened the count into
checked redundancy, type-checked the applier, stamped the update, and
put the anchor on the mount audit's list). **PW2-2 built 2026-08-24**:
`InitTableAccess` resolves `desc_page_id` through the anchor at fill -
the anchor is the durable truth, the row CREATE-fixed from PW2-3 on;
between fills the cached access keeps today's in-place-update license; a
system relation (no anchor) keeps the row's value, and a *foreign*
relation's unfaultable anchor falls through to the row deliberately
(its root is never walked here - execution ships to the owner, whose
own fill resolves), Corruption alone loud. Pinned by the
moved-anchor-vs-fixed-row test. **The first build of the read alone
failed fourteen suites** - a grown btree served its CREATE-time root to
every fresh fill, the partial-version trap by the book - so PW2-2
carries the **transition dual-write**: both movers
(`UpdateRelationDescPage`, `UpdateIndexRoot`) land the new root in the
anchor beside the row, logged and stamped through `LogCatAnchorUpdate`.
**PW2-3 built 2026-08-24**: the rows are CREATE-fixed - both movers
write the **anchor alone** through `WriteAnchorRoot`, the one write
path (the f5686f8 review's S1), with `UpdateIndexRoot` taking the
anchor id from the caller's own access instead of scanning sys.tables
inside an index split (its S2); `CREATE INDEX` seeds its slot so the
anchor is whole truth from birth; `InitTableAccess` resolves index
roots through the anchor too, and this core's *own* relation with an
unresolvable anchor is now loud (`Catalog` learned its core id - the
review's C1, which would have gone wrong the day PW2-4 lifts the btree
gate), with the anchor's owner stamp checked as redundancy (C5).
DESCRIBE's root/height/leaves and SHOW INDEXES resolve through the
anchor (C7). **Named debts, each the review's**: the anchor slot
removal waits for DDL resolution (DROP INDEX is transactional, a
rollback must keep the slot), so entries accumulate across
create-then-drop cycles toward the 679 cap - `anchor_page.hpp` states
it; a failed root repoint still leaves a grown tree nobody points at
(pre-existing, every ordering); `exec/catalog_view.cpp`'s
`desc_page_id` column and the mount audit's "descriptor" entry now
mean the CREATE-time root, said here so nobody rediscovers it; and
**C3, a decision for PW2-4**: the anchor is authoritative but lives
above `kCatalogOverflowLimit`, outside `kEveryCatalogPage`'s
invalidation set - a *diagnostic* cross-core reader (core 0 faulting a
peer's anchor for DESCRIBE) can cache a frame nothing refreshes;
options: extend the invalidation set with cached relations' anchors,
fault-bypass the frame at fill, or declare the anchor owner-readable
only. **PW2-4 built 2026-08-24** (worktree `pw1c1-handoff-record`): the
btree shape gate lifted — a peer grows its own btree writing only its
own pages, proven by the e2e (600 rows through the peer dispatcher,
leaves divided, the sys.tables row never written, COUNT whole). What
made the lift sound, each recorded: **root moves are not DDL** — both
movers are `WriteAnchorRoot` + an in-place cache update
(`CatalogCache::UpdateDescPage` joins the index root's deliberate
exception; the old `BumpVersion` destroyed the entry the running
INSERT held), and both take rel-oid/anchor from the caller's access —
the 96b0343 review's C1 found `UpdateIndexRoot` still write-fetching
`sys.indexes` (half of PW-B2 surviving the retirement), now gone;
**C3 decided: owner-readable anchors** — the fill resolves anchors
only for this core's own relations (build-invariant; foreign
diagnostics show the CREATE-time root, by statement), which also
closes the cross-core anchor-faulting hazard the C7 diagnostics had
opened; **the pre-grant window closes at the grant** — an own
pre-grant fill falls back to the row (P6's resolve-before-grant kept;
provably safe, a peer cannot have moved a root it could not write) and
`GrantRelationWrite` drops the catalog cache when rights land;
**EXPLICIT stays refused on a peer** (the id-ceiling catalog write) —
**superseded 2026-08-25**: the key mode is gone
(`docs/spec/heap-and-tuple.md` §4.1), so `funded_shape` lost this arm and
the refusal moved into `InsertOneRow`, where the supplied id is in
hand. It refuses *the row that names a key*, which is the row whose
admission writes `sys.tables`; a row that omits its key draws from
this core's lease and writes no catalog page, so a peer now takes
writes to relations this gate used to refuse whole;
**`WriteAnchorRoot` validates page type and owner stamp** (the
review's C3 — the write is where damage is created); the fill holds
**one anchor ref across the whole fill** (C2 — the N+1 re-fetch could
leave a half-anchored access under a sized pool, maintenance appending
into a stale subtree); `CheckIndexDef` refuses seeding a foreign
anchor when a publisher is installed (C4, keyed on the hook so the
P4e harness's hook-less fixtures keep building); DESCRIBE reads the
anchor directly rather than filling the shared cache from a
view-filtered resolve (C5/DT3). Declined with reasons: S1's shared
resolution helper (C2+C5 cover the sites), S2's seed parameter (cold
path; the owner check removed the coupling risk), S3's
derive-core-id-from-store (a base-class change with a named
interaction to examine first).

### 7c. The decision PW1c-6's grant extension needs — do not assume

Scoped 2026-08-25 on `pw1c6-index-grant` at `2c6ae23`. The PW1c-6 row
says "extend the publish grant and the handoff to the index pages and
delete this refusal". Read against `exec::CreateIndex`
(`src/exec/index_ddl.cpp:217`), that is **unsound for a populated
relation**: the function formats the root with the *builder's* store,
runs `Backfill` over the relation through that store, and logs the built
tree as full page images into the builder's stream — so on core 0 against
a peer-owned relation, `Backfill` reads what the device holds (the owner's
last checkpoint, up to `checkpoint_interval` stale, PW3) and misses every
row the owner inserts while the build runs, since the owner cannot
maintain an index its catalog does not yet show. The grant machinery —
handoff records, exact-page write grants — is sound for the *pages*; the
*rows* are what core 0 cannot reach. CC7's record rejected "owner
allocates at DDL" for `CREATE TABLE`, where the pages were empty and core
0 could format them; that premise does not hold here. Three shapes:

- **(a) The owner builds.** Core 0 keeps the catalog half — the
  `sys.indexes` oid, `CheckIndexDef`, the row, the commit — and sends the
  owner a build request (index oid, key and covered columns, widths); the
  owner allocates the tree from its own lease, backfills from its own pool
  in one synchronous poll (`CreateIndex` never parks, so no statement
  interleaves), logs the full page images into its own stream, seeds its
  own anchor slot (owner-writable, PW2-4's C3) and replies with the root;
  core 0 writes the row with that root (diagnostics — the anchor is the
  live truth, PW2-3) and commits. No page crosses a stream, so PL's
  handoff is not needed at all, and the `sys.indexes` write follows the
  reply, so no half-logged state waits on the ring — the objection §7
  raised against a wait inside INSERT. What it costs: the first
  cross-core wait inside a DDL statement (`HandleIndex` becomes a parking
  step with a timeout that refuses); a rollback after the owner's build
  orphans the owner's tree, as a dropped index's pages orphan today; and
  `DROP INDEX` on a peer relation wants a message to retire the owner's
  anchor slot (or leaves it, PW2-3's named debt). The shape gate's
  `indexed` arm then lifts on the footing PW2-4 lifted the btree arm:
  every index page is the owner's.
- **(b) Freeze, flush, build on core 0, hand off.** A quiesce request the
  owner honours (flush the relation, refuse its writes), the build over the
  device image, the tree handed off page by page — the six-slot
  `RelationWriteGrantPayload` cannot carry a tree, so a multi-message grant
  or a range grant against §8's exact-page rule — the acquisition restamp
  of every page on the owner, then the thaw. Heavier in every part, refuses
  the owner's writes for the build's duration, and buys only what the R5
  mover needs later anyway — CC7's argument, which (a) sidesteps by never
  moving a page.
- **(c) Admit `CREATE INDEX` only on an empty peer relation** — rejected:
  "empty" is the owner's fact, so it needs (a)'s request anyway, and it
  races the owner's inserts between the check and the commit.

**CLA's reading: (a).** It is the one shape in which the rows and the
pages stay in one stream. It reverses CC7's owner-allocates-at-DDL stance
for this one DDL, on the ground that the stance assumed core 0 could
produce the pages. **Not taken**: it introduces the first cross-core
request/reply inside a DDL statement, which is the operator's to ratify.
Until decided, the PW1c-6 refusal stands and the shape gate keeps refusing
writes to indexed relations on a peer.

**Decided 2026-08-25 (operator): (a), the owner builds.** The shape, as
built in worktree `pw1c6-index-grant`:

- **Two phases on core 0, parked between them.** Phase 1 (`HandleIndex`,
  the foreign-relation arm): resolve the relation and columns, compute the
  definition and layout, pre-issue the index oid (the `sys.indexes`
  `next_id` bump is non-transactional under RV3; an oid burned by a failed
  build is never reissued, the ids' standing rule), `CheckIndexDef`, send
  the owner a build request, and return a `DispatchOutcome` carrying a
  pending build — `DispatchAsync` parks on it with a deadline, the way it
  parks on a remote read. Phase 2, on the reply: a DDL scope, the
  `sys.indexes` row with the owner's root and **no anchor seed** (the
  owner seeded its own), the commit, the catalog invalidation broadcast,
  then `done(committed)` to the owner. A timeout or a refused reply ends
  the statement with an error and `done(aborted)`. **Inside an explicit
  transaction the statement is refused by name**: the owner's write
  refusal below would last until the client's `COMMIT`.
- **The owner's handler** (a `system`-group task on its reactor): its own
  `InitTableAccess`, a root from its own lease, `Backfill` from its own
  pool in the one synchronous poll `CreateIndex` always was, the tree's
  full page images logged under `kNoTxnId` into its own stream (a
  transaction of core 0's would be a phantom in this stream's analysis),
  its anchor slot seeded, the reply. From the build's first page until
  `done` arrives, **writes to that relation on the owner are refused
  retryably** — rows inserted in that window would never be indexed,
  since the owner's catalog does not show the index until core 0's
  commit invalidates its cache — with a generous timeout as the backstop
  against a core 0 that never says `done`. On `done(aborted)` the tree
  orphans and the anchor slot stays (PW2-3's named debt, one more
  occupant).
- **Then the shape gate's `indexed` arm lifts** for the owner: every
  index page is the owner's, allocated from its lease and stamped by its
  stream, so maintenance on INSERT/UPDATE/DELETE is a local write, and a
  root split's `UpdateIndexRoot` writes the owner's anchor (PW2-4).
  `DROP INDEX` stays core 0's catalog write; the owner's slot stays.

| # | Task | Gate |
|---|---|---|
| PW1c-6b-1 | **Built 2026-08-25** (worktree `pw1c6-index-grant`). `PrepareIndexDef(catalog, stmt, view, seed)` resolves the relation under the view and every column, computes the widths, runs `CheckIndexDef`, *then* issues the oid — so a refused declaration burns no oid and leaks no root page, where the old order burned and leaked both; it touches no page of the relation, only catalog pages (the oid bump among them, which a leased store refuses — core 0's half by construction). `BuildIndexTree(store, access, def, trx_id, wal)` re-derives the layout from the def's widths (`covered = entry - key - pk`, guarded, plus the column-count caps — the review's finding: the exported entry point assembled an `IndexRef` from an unbounded def, one element past its fixed array at five keys, unreachable today and reachable the moment 6b-2's payload arrives), formats the root, drops its pin before the walk, `Backfill`s, `LogBuiltTree`s, returns the final root. `Catalog::CreateIndex` and `CheckIndexDef` take `AnchorSeed {kHere, kByOwner}`: `kByOwner` skips the anchor seed and the owner refusal that guards exactly that seed, and asserts the owner seeded (unchecked). `CreateIndex` is the three back to back; the dispatcher is untouched. Suite 2620/2620 at both the split and the review's edits; overhead not measured (the v2 amendment). Review applied: the array guard, the header's false "touches no page", `seed` on `PrepareIndexDef` (6b-3 cannot call it otherwise), ~30 lines of prose restating the header cut, the enum's precondition stated; declined: the pre-existing three-way copy of the index shape (`IndexDef`/`SysIndexRow`/`IndexRef`), which the split brushes against and 6b-2 does not add to | §7c |
| PW1c-6b-2 | **Built 2026-08-25** (worktree `pw1c6-index-grant`). Kinds 25-27 and their POD payloads (`server/index_build_service.hpp`: the request is the `IndexDef` verbatim, name included so the owner's `CheckIndexDef` refuses by the same rules; the reply a root or a `StatusCode` plus 112 bytes of message; `done` the oid and a committed bit). **The owner's `IndexBuildServer`**: `OnRequest` bounds the column counts before reading the arrays, reads the owner from the row rather than the requester, refuses a second build on a relation whose window is open, then opens the window *before* the build and hands it to a `system` task - `CheckIndexDef`, `exec::BuildIndexTree` under `kNoTxnId` into its own stream, `WriteAnchorRoot` on its own anchor, `SyncAll` (the first cut used `EnsureDurable(appended_lsn())`, which names the append point and is refused - the two new tests caught it), reply. `OnDone` closes the window and on `committed` drops the catalog cache itself, so the invalidation broadcast and `done` may arrive in either order; `Expire` closes windows older than `kIndexBuildPendingCeilingNs` (180 s) and drops the cache too, in case the lost `done` was a commit. **The window** is `PendingIndexBuilds` (core_affinity.hpp, the dispatcher's sole dependency, `RelationGrantDemand`'s pattern) and `IndexBuildPending`, a `TXN_CONFLICT retryable=1` at the top of `CheckWriteAffinity`'s peer branch - before the shape gate, since the relation looks funded until the commit lands. The ceiling exceeds core 0's reply deadline (60 s, 6b-3's) by a `static_assert` whose text is the argument: a release before core 0 gives up admits rows the published index would miss. **Core 0's half**: `IndexBuildWaiters` (a map, for the stable address a parked coroutine holds), the reply receiver (an unmatched request id is §3's silent discard), the two sends. `CoreRuntime` arms the server on peers only at `AttachTransport`, wires the window into the dispatcher at `Open`, ages it on the lease tick. Two tests over a real ring in `core_runtime_test.cpp`: the happy path with core 0's two phases driven by hand (rows the owner wrote and core 0 never faulted are in the tree; the anchor slot carries the replied root; the window refuses by name; after the row and `done(committed)` the owner's view shows the index with that root and a keyed read answers through it; what refuses a write then is the shape gate, not the window), and the endings (foreign relation refused with no window; counts past the caps refused before the arrays; `done(aborted)` closes the window, publishes nothing, keeps the slot; a `done` for nothing is ignored; a window nobody closes releases at the ceiling and not before). Suite 2622/2622; overhead not measured (the v2 amendment). **Review** (`critics-developer` over the uncommitted diff; it delivered after the push landed at `dd77ce2`, made no edits, and nothing of it is applied yet - **6b-3 opens with it**): correct as built - every peer write path passes the gate, `kNoTxnId` survives analysis and redo, the lifetimes hold. Owed: (1) the header's "whatever order" claim holds only if core 0 flushes the catalog *before* sending `done` - `InvalidateCatalog` re-reads off the device - which is 6b-3's ordering requirement and must be written; (2) the `static_assert` bounds the reply leg only: the true invariant is ceiling ≥ deadline + a bound on the commit, and no such bound exists, so 6b-4's gate lift must reckon with a late commit after an expiry; (3) `done(aborted)` overtaking the request (two independent send-retry tasks) costs a 180 s write outage on the relation - the no-waiter branch of the reply receiver should send `done(aborted)` back; (4) the window is sound only because `Backfill` indexes every version, uncommitted and delete-marked included (rollback writes pages without the gate) - to be stated in the header; (5) `IndexBuildRequestOf` truncates an over-cap definition silently, the inverse of `CheckIndexDef`'s refuse-never-truncate - refuse instead; C1 lift `StatusOfWire` into `status.hpp` (the session client's switch already disagrees - `kAlreadyExists` reads as IoError there); C2 collapse the runtime's send lambda into `SendPod` (it sets `session_core = dst` where every other reply echoes the request's); C6 the happy-path test should assert `IndexProbe` through `ANALYZE`, and its closing assertion also passes on a *successful* INSERT; (10) `SHOW INDEXES` on core 0 now walks pages that live only in the peer's pool (degrades to `-`, but parks a stale frame), and no `SHOW META` line shows an open window's age | 6b-1 |
| PW1c-6b-3 | **Built 2026-08-25** (worktree `feat-PW1C-6B-3`). Core 0's two phases in `HandleIndex`'s foreign arm, **gated on `SetIndexBuilds`** - armed by the tests and not yet by the Expeditor, so the PW1c-6 refusal still stands in production (its message now names 6b-4): an index published before the owner's shape gate lifts would leave the relation unwritable on its owner. Phase 1 (`BeginForeignIndexBuild`): the explicit-transaction refusal by name before anything is sent (the window would last until the client's `COMMIT`; not poisoning, no oid burned - tested), `PrepareIndexDef` under the session's view with `kByOwner`, the oid issued, the request sent through `IndexBuildClient::Request`, the outcome returned as `pending_index_build`. `DispatchAsync` parks on `IndexBuildClient::Settled` - arrived, deadline passed (60 s, one clock read per reactor turn), or waiter gone - between the remote-read park and the durability wait, holding nothing across the suspension (the view is a POD, the access dies in phase 1, the pending record is an owning copy); the synchronous `Dispatch()` abandons at once and tells the owner, the remote read's stance. Phase 2 (`FinishIndexBuild`): the owner's root, `Catalog::CreateIndex(def, trx_id, row, kByOwner)` under a DDL scope of its own (`DdlScopeFor`/`NoteDdlRows`/`InDdlStatement`, the local arm's shape minus the build and the seed - a same-named index created while parked, or the relation dropped, aborts the scope and orphans the tree), `done(committed\|aborted)` sent right after the commit record's append and *before* the durability wait, the staged LSN read out as `DispatchAndStage` does. A timeout is `TXN_CONFLICT retryable=1` naming the deadline; the owner's refusal passes through with its code and context. **The 6b-2 review, applied**: (1) the ordering is the row write's own invalidation hook - `BumpVersion` runs `BroadcastCatalogInvalidation`'s flush inside phase 2, before the commit and before `done`, so either message may reach the owner first - and the header states it, plus why `done` before durability is sound (a crash between makes the DDL a recovery loser; the `kNoTxnId` tree redoes into an orphan); (2) the static_assert's text now says it bounds the reply leg only, and the commit leg - one reactor turn on core 0 plus the ring - is stated as the margin the design assumes, not one the code proves; (3) the reply receiver's no-waiter branch sends `done(aborted)` for a successful build, so a request the ring held past the deadline closes its window at once, not at the 180 s ceiling - reached by a test that drops the waiter without a word; (4) the Backfill-every-version argument is in the header; (5) `IndexBuildRequestOf` refuses an over-cap definition rather than truncating (and a name the wire cannot hold - stricter than the catalog, which truncates through `SetName`; the divergence is named in the header, closing it belongs in `CheckIndexDef`); C1 `Status::FromWire` in `status.hpp`, one decode for the remote-step and index-build wires (the session client's switch read `kAlreadyExists`, `kFkViolation`, `kAssertionViolation` and two more as IoError - they now decode faithfully, a compatibility-surface move named in the commit); C2 the runtime's send lambda is `SendIndexBuildMessage`, `session_core` the constant 0 on every leg; C6 the happy path asserts `IndexProbe` through `ANALYZE` on the owner and its closing assertion names the shape gate; (10) `SHOW META` on a peer prints `index_build_windows=` and `index_build_window_age_max_us=` (tested inside and after a window). **The 6b-3 review** (`critics-developer`, no correctness bug found): applied - `IndexBuildServer` takes the scheduler and the ring directly, its three one-caller `std::function` seams and the never-called inline-build branch gone (~30 lines); the Cabin warning has one home, `exec::IndexCreationWarnings`, and the `CREATED INDEX` line one builder for both arms; `ErrorReply` on the create paths (a `TxnConflict` out of the catalog kept its spelling on neither arm before); the dead null guard on the sync path; the rig's client outlives its dispatcher (it was declared the other way, a dangling pointer through destruction); two header claims corrected (the reply does not echo a session core, nothing does; the name-length refusal is not `CheckIndexDef`'s rule). Declined, as client-visible and the operator's: the local arm's `entries=0` literal, false since the backfill (named at the site; the foreign arm prints `built_by_core=<n>` instead); the index-name length divergence between the arms. Owed to 6b-4/6b-5: `SHOW INDEXES` on core 0 parks a stale frame of a peer's tree; the durability leg is untested (the rig has no WAL); the broadcast publishes a page carrying an uncommitted row (sound only because the window holds writes and an abort re-broadcasts through `InvalidateAfterCompensation`). Seven tests over a real ring on a shared `ForeignIndexRig` (core 0 with a transaction stack, a peer owning a populated relation): the happy path through `DispatchAsync` with the window observed mid-park; the explicit-transaction refusal; the timeout under a manual clock, then the late request built and orphaned at once; a second statement refused by the owner while the first builds (two parked statements, the second's `done(aborted)` ignored by the owner); the no-waiter reply closing the window; the sync path; the owner's endings over raw payloads. Suite 2627/2627; overhead not measured (the v2 amendment) | 6b-2 |
| PW1c-6b-4 | **Built 2026-08-25** (worktree `pw1c6b4-gate-lift`). `CheckWriteAffinity`'s `indexed` arm lifted: `access.indexes.empty()` dropped from `funded_shape` and its refusal branch deleted, so a peer takes writes to a relation it owns that has a secondary index. **The soundness argument, reviewed and confirmed**: a peer-owned relation's index is *only* ever owner-built (a peer refuses all DDL - `PeerDdlRefused`; core 0's `HandleIndex` foreign arm reaches only `BeginForeignIndexBuild`; `CheckIndexDef` refuses a `kHere` seed on a foreign relation; `owner_core` is written once by `CreateTable` and there is no mover), so every index page is the owner's own-stamped and maintenance is a local write - `AppendIndexEntry`'s existing leaves and a split's new page pass `MayWrite` on the lease/own-stamp, `UpdateIndexRoot` writes the granted anchor (PW2-4, the same page `UpdateRelationDescPage` already wrote). The `IndexBuildClient` is **wired into the Expeditor** on every multi-core instance (beside `remote_reads_`, receiver registered before `SetIndexBuilds`), so the foreign arm's two phases are production, and the null-client fallback is a socket-free fixture - refused by name and byte. **The review found the lift opens a cross-core hole on the *sibling* statement**, closed here: `DROP INDEX` on a peer-owned relation *inside a transaction* marks the row and `BumpVersion` broadcasts at the mark, so the owner - whose DT9 in-flight predicate is core-local and cannot see core 0's deleter - drops the index from its view and maintains nothing before COMMIT; a ROLLBACK would then restore an index missing the owner's meanwhile-writes. Refused by name (Unsupported, byte, symmetric with the CREATE-in-a-transaction refusal); autocommit keeps only the commit-failure window every DDL has and stays admitted; core-0-owned stays isolated by DT9, untouched. Four stale contract claims the lift outdated were corrected in place (the `SetIndexBuilds` header, the DDL refused-shapes list, the whitelist opener, the exhaustiveness comment - each now names the shapes actually refused: EXPLICIT, FK-linked, cabined, assertion-covered). **Owed to 6b-5** (documentation, no code): (a) a window that *expires* on the owner before core 0's late commit lands admits writes an index the commit then publishes would miss - pre-existing (the pre-lift gate keyed on the same stale own-catalog view), unchanged in severity, now reachable in production; (b) `SHOW INDEXES` on core 0 for a peer relation reads the build-time root from the row (foreign `InitTableAccess` skips the anchor), which a maintenance split can move, so a stale root now walks a subtree and prints a plausible-wrong `entries=` - diagnostics only, cross-core reads downgrade `kIndexProbe` to `kScan` before shipping (`step_descriptor.cpp`) so query answers are unaffected. E2E over the `ForeignIndexRig`: the happy path now INSERTs after publish (admitted, the gate lifted) and `AnOwnerMaintainsInsertsIntoAPeerBuiltIndexAndReadsAnswerWhole` runs four maintained INSERTs, asserts `SHOW INDEXES` `entries=3`->`7` and reads every value whole through the index (`ANALYZE`=`IndexProbe`), an absent one none; plus the DROP refusal/admit test. The pending-window refusal-and-release and the abort path are 6b-3's tests, kept. Suite 2629/2629; overhead not measured (the v2 amendment). Review (`critics-developer`): the tree-side lift is sound (no route to a non-owner-stamped page found; `MayWrite` passes on leaves, splits, anchor), the DROP hole was its finding and is fixed, four contract claims corrected, two tests trimmed of overlap and an absent-read hardened; declined nothing of substance | 6b-3 |
| PW1c-6b-5 | **Built 2026-08-25** (worktree `pw1c6b5-docs`), docs only. `docs/spec/ddl-transactional.md` gains **§5e** (a relation another core owns: `CREATE INDEX` built by the owner) - atomic (one publishing event, core 0's commit; a rollback/timeout/abort orphans the owner's `kNoTxnId` tree as a dropped index's pages orphan; a crash between the commit append and durability makes the DDL a recovery loser and the tree an orphan), isolated (the owner's refusal window during the build, the row DT9-filtered until commit, the owner's cache holding no index until `done`), and the two gaps a single-core build lacks (an expiry window before a late commit; `SHOW INDEXES` on core 0 reading a maintenance-moved peer root) - plus the `DROP INDEX`-on-a-peer-relation refusal inside a transaction; §5b's core-0-scope paragraph and §6's oracle bullet note the meeting arrived and is closed conservatively; the header points at §5e. `docs/spec/crosscore.md` CC7 gains the **owner-builds exception** (core 0 cannot produce a populated relation's index pages, so the owner builds and no page crosses a stream - CC7's rejected "owner allocates at DDL" taken for this one DDL, the premise having failed). `docs/inflight/known-gaps.md`'s peer-writer entry records the PW1c-6b series complete and §5e's two named gaps. `CLAUDE.md`'s Open Decisions parenthetical flipped to decided-and-built. No code, no measurement | 6b-4 |

### 7d. The assertion's cabin — decided and built 2026-08-26 (PW1c-6c)

Opened by `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`
**Finding 2**, on the engine at `v2.2.0-11-g925f483`: a shipped write to an
assertion-covered, peer-owned relation was **admitted and not enforced** — a
second row landed in a group under `CHECK COUNT(*) <= 1`. The measured cause
was that the shape gate's assertion arm read `enforcer_.AnyOn(oid)`, a
per-core memory registry that only core 0's mount ever filled, so on a peer
it answered "no assertion" and the gate did not fire. The operator's
direction, 2026-08-26: **fix by ownership, not refresh** — the owner
allocates the cabin from its own lease, own-stamped, no handoff, PW3
checkpoints it, 6b's pattern; the remote-reservation alternative pends 2PC.

**Why a refresh could not have worked, and why this is stronger than 6b's
case.** Teaching the peer's registry about core 0's cabin would have turned
an unenforced write into a *refused* one, never into an enforced one: the
Bound Cabin is appended to by **every write to the relation**
(`AssertionEnforcer::ReserveInsert`), so its pages must be writable by the
relation's owner for as long as the assertion exists, and a chain core 0
allocated never is — `MayWrite` refuses a page carrying neither this core's
lease, a grant, nor its own stream's stamp. The index's premise was that
core 0 could not *read* the rows; this one is that core 0 cannot own a
structure the owner must keep writing.

**What was built.**

- **The split**, `exec::PrepareAssertionDef` / `BuildAssertionCabin` /
  `InsertAssertion` (6b-1's shape). Core 0 keeps §3.1's checks and the id —
  both catalog writes — and the owner runs the page half against its own
  catalog view.
- **Kinds 30-32** and `server/assertion_build_service.hpp`: the request
  carries the declaration *verbatim* (§8.2's canon, so the owner parses it
  exactly as a mount's `ReviveAssertion` does, and the `GROUP BY` list stays
  uncapped in the catalog while the wire caps bytes), the reply the root and
  the two numbers, `done` the statement's end.
- **No refusal window**, where the index build has one, and the reason is
  the adoption point: the owner adopts the directory at the end of its own
  synchronous build task, so there is no interval between the last scanned
  row and the publish in which a write could go uncounted. A window would
  also have been the worse trade for this structure — an index missing a row
  answers wrongly, a cabin missing a row **under-counts its group forever**,
  since nothing rebuilds it.
- **The gate reads the right question.** `funded_shape` drops
  `!enforcer_.AnyOn(oid)` — a relation whose assertions this core holds is
  funded, and refusing it would refuse writes to a constraint the core is
  enforcing correctly — and gains `!enforcer_.CannotEnforce(oid)`, the new
  fail-closed record for an assertion this core knows of and may not
  maintain.
- **RC07 per core.** `ResumeAssertionsAfterRecovery` runs in
  `CoreRuntime::Open` too and takes on **only the relations that core owns**;
  every other declaration is counted `assertions_foreign` and skipped, so no
  two cores hold the same directory. Every way a directory can fail to come
  back on a relation this core owns — the declaration unreadable, the revive
  refused, no base in range — now also records `NoteUnenforceable`, which is
  what makes the owner refuse the relation's writes instead of admitting
  unchecked ones.
- **`DROP ASSERTION` reaches the owner**, through the `done(aborted)` leg.
  Found by self-review, not by a test that existed: with the directory moved
  to the owner, a drop that only retired core 0's row would have left the
  owner refusing writes for a constraint that no longer exists — the mirror
  of the finding, and just as wrong.

**Two things the build had to learn from a failing test.** A peer could not
read the declarations at all: `sys.assertions`' *heap* pages are below
`kFirstUserPageId` by construction, exactly so a peer can read the catalog
(`catalog/well_known.hpp`), but a spilled `source_text` lives in a var-heap
page from the general supply, which a peer may not fault. The mount now
grants itself read rights over **exactly the pages the rows name**
(`exec::AssertionSpillPages`) — and exactly those, page by page, because the
first cut granted the extent around them and broke
`APeersOwnPagesSurviveARestartByTheirStamp`: a page that answers `MayFault`
from a grant never reaches `TryClaimByStamp`, so an extent covering pages the
core owns costs it PW1c-7's restored write rights. The second: the
writability probe must run *after* the revive has walked the chain, since
that walk is what lets the store claim its own stamp.

**Open, and named rather than left to be found.**

- **The admission straddle.** A write that passes `AdmitInsert` and then
  parks (a lease refill) can reserve after an adoption that happened in
  between, so one row can be reserved without an admission check. The
  aggregate stays exact, and the index build has the identical class of hole
  against its window; a fix belongs to whatever makes a statement's
  gate-to-write span atomic.
- **A lost `done`.** No acknowledgement exists in either direction, so a lost
  `done(committed)` leaves the owner's catalog cache stale until the next
  DDL, and a lost `done(aborted)` — including a `DROP`'s — leaves it
  over-enforcing until its next mount. Fail-closed both ways, and a remount
  clears it.
- **The catalog var-heap is not in the peer-readable range.** The mount's
  page-exact grant closes it for `sys.assertions`; `sys.pattern_defs` has the
  same shape and no reader on a peer today.

| # | Task | Gate |
|---|---|---|
| PW1c-6c | **Built 2026-08-26** (worktree `ss-check-findings2`). Everything above, with seven tests over the `ForeignIndexRig`: the happy path (built on core 1, the violating shipped write refused by the assertion and not by a gate, **and a legal write still admitted** — a fix that refused everything would pass a test that only checked the refusal); the owner's restart (revived, folded from its own stream, enforcing with the *recovered* aggregate, plus the two orderings above pinned separately); the unenforceable record refusing by name and its repair by eviction without a remount; the owner's refusal (`ASSERTION_VIOLATION` raised where the rows are, nothing published, the relation still writable — proof there is no leftover window); core 0 abandoning the statement on the synchronous path and the owner evicting; the explicit-transaction refusal; and the DROP reaching the owner. `SHOW META` gains `recovery_assertions_foreign=`, `SHOW ASSERTIONS` gains `enforced_by_core=` for a relation another core owns (this core's registry cannot answer for another's, so the owner is named rather than the claim guessed), and the foreign `CREATED ASSERTION` line carries `built_by_core=`. Suite **2729/2730**, the one failure `TlsChannelTest.PlaintextGarbageIsFatal`, pre-existing on this host and recorded as a bug report at the time — **fixed 2026-08-26** on worktree `tls-alert-bytes`, the test having pinned OpenSSL's byte count rather than the channel's contract, so the report is deleted and the contract is `docs/spec/protocol.md` §1; one disabled test remains and is Finding 1's, untouched. **The `critics-developer` review was not run** — this session forbids agent invocation — so the code was self-reviewed, which is a stated gap and not an implied pass; that self-review is what found the DROP hole and the `Evict` repair path. Overhead not measured (the v2 amendment) | §7d, PW1c-6b |

## 8. The PW1c decision — decided 2026-08-24 (operator-delegated)

**The write handoff, riding PL-B.** A peer gains write rights over a
rotated relation's creation pages through the handoff contract
`docs/spec/page-lsn-cross-stream.md` §9 ratifies, upgrading CC7's
DDL-publish sequence from flush-then-fault-grant to **flush → handoff
record → grant-with-write-rights**. Decided on `r1-peer-ddl-refusal` at
`7c5432c`; nothing below is built except the interim guard.

The fact that forced the shape: `Catalog::CreateTable` formats the root
and RV3 logs it in **core 0's stream** (`src/catalog/catalog.cpp:1155-1171`, `LogCatPageInit`),
so a rotated relation's creation pages carry a core-0-stream `page_lsn`
and a peer's first write is a cross-stream transition — the §3 failure of
the PL spec. Per-relation write grants alone are therefore unsound; they
need the handoff, and coupling PW1c to PL-B makes it PL-B's **first
consumer**, on its easiest case: pages that are quiescent and freshly
flushed at DDL publish. The machinery is owed anyway — the range
blueprint's R5 mover and `physical-optimizer.md` §6 gate 3 both need
it — so nothing here is throwaway.

The other two known-gaps options, resolved:

- **Owner allocates at DDL — rejected.** CC7's decision record already
  rejected it ("a new cross-core allocation protocol inside DDL that
  still needs a creation-time write exception"), and it dodges PL only at
  creation while the mover still needs the general mechanism.
- **DML statement shipping — reframed, not rejected.** It answers
  *session* placement (M3's cliff), not page ownership: a shipped write
  executes on the owner core against the same core-0-formatted pages.
  Complementary; stays its own future item.

Two rules that are correctness statements:

1. **Write rights are exact-page, never extent.** The superset that is
   safe to fault is not safe to write. The set is small by construction:
   **the pages core 0 formatted for this relation** — the root, the
   var-heap root when the schema can spill (eager, `SchemaCanSpill`,
   `catalog.cpp:1185`), **and every index page built on core 0** (amended
   at the f878f4d review: `HandleIndex` has no owner check, so
   `CREATE INDEX` on a peer-owned relation runs on core 0 and allocates
   from core 0's map — and a peer's *read* through such an index is
   already outside CC7's grant, working today only because a just-created
   index lands in the same 64-page extent; that is **PW1c-6**). Growth
   pages come from the owner's own lease and are its stream's from birth,
   so they need no handoff.
2. **The handoff record is durable before the grant leaves core 0** —
   PL §9 rule 1's ordering, restated because DDL publish is where it will
   be implemented.

| # | Task | Gate |
|---|---|---|
| PW1c-1 | **Built 2026-08-24** (worktree `pw1c1-handoff-record`): `kPageHandoff` (25), the four-byte `PageHandoffPayload{incoming_core}` — the page is the envelope's, the handoff LSN the record's own — and `LogPageHandoff` as the one emitter (`log_page_init.hpp`'s shape, PL §9 rule 1's ordering stated as the caller's), named and ceiling-derived. An ordinary record type, no format bump. Provenance stated: drafted in a prior session and left uncommitted (`git log -S kPageHandoff` answered nothing), committed, verified and extended here — the append-only type-registry test had not moved its ceiling to 25 and failed until it did. Nothing emits it until PW1c-4 | PL §9 (ratified) |
| PW1c-2 | **Built 2026-08-24** (same tree): analysis *removes* a handed-off page from the outgoing stream's dirty page table — a checkpoint-seeded entry included, and a page that returns (A→B→A) re-enters at its post-return recLSN by erase-then-first-wins. **The redo half was the finding**: `Redo` consumed only `redo_start_lsn` and never consulted the dirty page table, so rule 3's "redo never touches it" was unenforced — a departed page's records would have replayed through an RV5 gate comparing incomparable LSN spaces, the PL spec's §3 failure. Redo now applies a page record only when the page is in the table and the record's LSN is at or above its recLSN (the ARIES filter), decided **before** the page load so the page is never faulted, counted by `skipped_not_dirty` — zero on any stream with no handoff, since an ordinary scanned record never sits below its own page's recLSN. Pinned by four tests: removal, the return recLSN, the seeded entry, and redo skipping a departed page unfaulted with the store never holding it. **Two residuals its review named, neither closed here**: the returning page's (A→B→A) post-return records still pass an RV5 gate that may compare against the *other* stream's `page_lsn` stamp — resolved by §9 **rule 6**, the durable acquisition restamp (PW1c-3's row tells the story: a first answer, rule 5a, was retracted the same day); and the erase is positional, so a later `CHECKPOINT_BEGIN` still listing the page re-seeds it — PW1c-4's rule-1a flush must clear the pool's dirty entry, not merely write the bytes | PW1c-1 |
| PW1c-3 | **Built 2026-08-24, reworked the same day at its review** (worktree `pw1c1-handoff-record`): `page_flags` carries `core_id + 1` (`StreamStampFor`, the convention's one home), stamped wherever `page_lsn` is — `DevicePageStore::StampPageLsn`, the funnel every logged mutation rides, and redo's apply; 0 stays "never stamped", no backfill, unstamped pages take today's RV5 comparison unchanged. **A reachable foreign stamp is `Corruption` at mount, unconditionally** — the first form shipped a rule 5a keying a bypass on the scan window's `handed_off` set, and the review's C2 retracted it in place (a durable fact keyed to one log window falsely refused the healthy *receiving* core); §9 rule 6, the durable acquisition restamp, replaced it — the redo/store enforcement halves are built here, the emitting half is PW1c-4's grant path. The review's C1 also fixed a live defect: a peer's mount-time undo stamped core 0's id (`core_id_` default until `SetCoreOwnership`), so `SetStreamCoreId` now installs the identity before recovery runs. `page_mgr`'s Frame is not production-wired and carries a named debt comment instead of a stamp | PW1c-1 |
| PW1c-4 | **Built 2026-08-24** (worktree `pw1c1-handoff-record`): exact-page write grants (`GrantWritePages`, a sorted vector `MayWrite` consults after the lease; `kRelationWriteGrant` = 23, `RelationWriteGrantPayload` with six slots — root, var-heap root, PW1c-6 headroom, never truncated). The publish hook flushes, appends a `PAGE_HANDOFF` per formatted page into core 0's stream, makes them durable, then sends the fault grant and the write grant on one FIFO edge — and **withholds the write grant when the handoffs are not durable**: the relation stays fault-readable, its writes refused retryably, never served unsound. The receive side is rule 6's home, and the build corrected the rule's letter: the restamp LSN must name a **logged record** (the WAL gate refuses a page claiming the bare append point), so the receiver appends its own acquisition `PAGE_HANDOFF` (incoming_core = itself) and restamps with that record's LSN — the acquisition is durable in the receiver's stream for free, and analysis's rule-3 erase reads either direction correctly. Grant admitted only after the restamp flush. The three deferred debts closed: `WriteBack` clears the per-frame dirty entry (the checkpoint-reseed precondition, verified at the site); `redo_skipped_not_dirty` lifted into `MountRecovery`; the two analysis one-liners (double handoff idempotent; a transactional handoff is `Corruption` — it would mint a phantom loser). `SHOW PAGE` now prints `page_lsn` and `stream_stamp`, the review's observability gap. What remains of the series: PW1c-5 (drop the interim guard, the e2e peer INSERT) and PW1c-6 (index pages) | PW1c-1..3 |
| PW1c-4r | The 95b45e8 review's findings, applied 2026-08-24: **C1** (blocking) — a peer's free-map snapshot predates any post-startup relation, so every granted page answered "page id not found" and the grant was dropped forever, with the shipped test green only on a fixture ordering production never has; both grant receivers now `RefreshFreeMapFromDevice` (soundly ordered — core 0 flushes maps before any grant leaves), which also fixes the pre-existing CC7 read half. **C2** — the "same FIFO edge" ordering claim was false under send-retry re-queueing on a full ring; the write grant now installs its own exact-page fault rights, so it survives arriving first. **C3** — a repeat grant is a no-op: a page already writable takes no second acquisition record, and §9 rule 6 carries the stated precondition its erase rests on. **C4** — recorded, not moved: the publish hook runs *inside* the DDL transaction, so a rollback retracts neither the durable handoffs nor the grants — benign only while nothing reissues page ids; stated here so 2PC and free-map-reclamation work re-check it. Core 0 now `EvictClean`s departed pages. **C7** — `PrepareRelationHandoff` (the S1 extraction, testable at last) refuses more pages than the payload carries, never truncates. **Named debt, its own future task: nothing re-grants after a restart** — both grant sets are memory-resident, so a peer that could write before a restart cannot after; wants "re-establish grants at mount from `sys.tables.owner_core`", it gates the e2e INSERT surviving a restart, and (the 25059bf review's C-5) it must also cover **mid-grant failure** — `GrantRelationWrite`'s uniform abandon leaves the relation unwritable until a re-delivery exists. **Closed 2026-08-24 as PW1c-7, and not the way this sentence asked**: the probe found the debt understated (a restart loses the *lease-owned* pages too, which no catalog-derived grant can name), and the stamp carries ownership instead. That review also hardened the receive path: the free-map refresh is scratch-validate-**union** (a torn concurrent read keeps the old copy; redo's mount-time bits survive), and the publish-side root pin now drops before the hook fires, so the departed-page eviction actually runs | PW1c-4 |
| PW1c-5 | **Built 2026-08-24** (worktree `pw1c1-handoff-record`): the interim guard is gone, and its duties moved rather than lapsed — `CheckWriteAffinity` gained the **shape gate** (on a peer: btree-clustered refused naming PW2, indexed naming PW2/PW1c-6, FK-linked and cabined naming §4's unverified co-locations — `cabin_ids` tested by live id, it is per-column-parallel; none poison the session), the multi-row `VALUES` path was first refused on a peer, then **revised at the 25059bf review's S-1**: the sorted fill is merely *ineligible* there (its id block is `AllocateRowIdRange`'s, off the catalog page) and the ordinary per-row path serves a peer through the lease — multi-row INSERT works, and the shape gate grew the assertion arm and a whitelist tail after the same review's C-3 caught admission-by-omission; and the store's `MayWrite` is enforced for leased stores in **every** build (one pointer compare on core 0's frame-load path), so an unfunded write is refused retryably instead of surfacing as a rule-5 stamp mismatch at the next mount. `PeerWriteRefused` deleted; a foreign write on a peer flows to `CheckWriteAffinity` again — retryable `TXN_CONFLICT`, and the §6 counters see it, reversing PW5's recorded undercount. **The e2e test passes**: a funded peer (fault + write grants, row-id and trx-id blocks) single-row-INSERTs into its own heap relation through its dispatcher and reads the row back; the bulk refusal and the btree gate are pinned beside it, and C1's production ordering (peer opened before the DDL) is its own test. The e2e surviving a *restart* is PW1c-7's | PW1c-4 |
| PW1c-6 | **Built 2026-08-24 as the refusal half** (worktree `pw1c1-handoff-record`): `CREATE INDEX` on a relation this core does not own is refused at dispatch, before the DDL scope draws a transaction id, naming the task and the offending table token's byte — the tree would be built from core 0's free map into pages the owner's fault grant covers only by the 64-page-extent accident, with no handoff and no write grant. The **grant-extension half is deliberately deferred to PW2**: the owner's write path refuses indexed relations anyway (PW1c-5's shape gate), so granting index pages today funds nothing; when PW2 routes the root-move catalog writes, extend the publish grant and the handoff to the index pages and delete this refusal. `DROP INDEX` stays admitted — it shrinks the unsound set. **The grant-extension half needs a decision (2026-08-25, §7c)**: that stated approach is unsound for a populated relation — `Backfill` on core 0 cannot see the owner's rows — and the sound shapes are the owner building the tree in its own stream (CLA's reading) or a freeze-flush-handoff; the refusal stands until the operator decides | PW1c-4 |
| PW1c-7 | **Built 2026-08-24** (worktree `pw1c7-restart-ownership`): **ownership survives a restart, and the stamp is what carries it.** The PW1c-4r debt asked for "re-establish grants at mount from `sys.tables.owner_core`"; the probe that scoped it found the debt understated. A peer's *extent lease* is carved fresh at every mount — `LeasedIdSource` remembers only this run's grants, `Expeditor::Serve` reserves a new extent per peer at startup, and nothing persists which extents a core held — so a restart loses not only the creation-page grants but fault rights (Debug) and write rights (every build) over **every page the peer allocated itself**: a heap chain's second page, a btree's leaves, a var-heap's growth. No catalog-derived grant can name those pages; only a walk could, and core 0 cannot soundly walk a peer's pages at mount. What already names them, durably, is the fact PL §9 rule 4 made: every page a stream writes carries that stream's stamp, and rule 6 lets no page leave a stream unrestamped — **the stamp is the durable form of ownership**. So `DevicePageStore::ResidentBytes` **claims from the stamp**: a leased store faulting a page outside its lease, its fault grants and its write rights reads the stamp — off the resident frame redo left at mount, else off the device, checksum-verified, the read handed to the miss path rather than repeated — and admits the page to read and write when the stamp names this core; a foreign stamp or 0 leaves every refusal exactly as it was. Attempted only where a check would refuse, so a leased or granted page pays nothing and core 0 its one pointer compare. The write-rights set became a page-sized bitmap (the headerless map's precedent: same addressing, different meaning) because its population grew from a handful of creation pages to every page touched since mount, and `MayFault` consults it — a write-granted or claimed page is readable by that alone, which also makes the 95b45e8 review's C2 structural. **The other half is re-delivery**, for what the stamp cannot claim: a creation page this core never acquired (stamp 0, core 0's `page_lsn`) after a crash before the acquisition restamp, a grant lost to a full ring, or `GrantRelationWrite`'s mid-grant abandon (the 25059bf review's C-5) — and only core 0 can hand it off, since rule 1 puts the record in the giver's stream. `CheckWriteAffinity` gained a **rights probe** after the shape gate: all three creation pages must be writable after a claim attempt (a crash between the restamp flush and the admission can leave them split), else the demand is recorded (`RelationGrantDemand`, unique per tick) and the statement refused retryably **by name** — `RelationWriteRightsPending`, `TXN_CONFLICT`, naming this task — where the store's every-build backstop named a page id. The drain tick sends `kRelationGrantRequest` (24) per demanded relation with no in-flight state and no reply: the answer is the ordinary grant pair, produced by core 0's `RegisterRelationGrantHandler` running **the same publish hook** CREATE TABLE runs (a named `publish` in `Expeditor::Serve`, two callers, so nothing else ever hands a relation off) after checking `sys.tables.owner_core` names the requester — a request for a foreign relation is dropped, since granting it would be the two-writer route. Idempotent by construction: a repeat PAGE_HANDOFF is analysis's no-op (PW1c-4's one-liner), and the receive side takes no second acquisition on a page whose stamp already names it (`GrantRelationWrite` now asks the stamp as well as the rights, and asks after the fault). **No mount-time re-grant loop, by decision**: with the claim it would only re-deliver what every page already states, and the demand path covers the one case it cannot — which also keeps mount cost independent of the relation count. `PageStore` grew a virtual `MayWrite` (default true) so the dispatcher can ask its interface. Pinned by: the store claiming an own-stamped page on a write fault and on a read fault while refusing foreign and unstamped ones in every build; a 600-row peer relation surviving a restart with a fresh lease and nothing granted — read whole, then written again — once with its pages flushed and once with them living only in the log (redo's replay leaves them resident without rights, stamped as it applied them); and, over a real ring, an unacquired relation refused by name, asked for on the tick, re-published exactly once by core 0, granted, and written on the retry, with a request for a foreign relation dropped. **Residuals, named**: a page formatted and flushed before its first content record is unstamped (`LogPageInit` does not stamp, by design) and unclaimable until redo or a write stamps it — unreachable while a page's format and first write share one statement on one reactor, recorded so nobody relies on it; and nothing *revokes* lease ownership at a handoff, so the R5 mover must drop a departing page from the giver's `LeasedIdSource` as well as restamp it, or the giver keeps write rights the stamp no longer allows — `GrantFaultPages`' "nothing revokes" rule now has a second reader. **Decided under §8's delegation precedent, not asked**: the alternative — a persisted per-core extent directory — is a new system page and a format bump that the future page-granular mover would outgrow, while the stamp is already durable, already exact per page, and already what redo trusts. **Two findings the restart test made on the way, both pre-existing and both fixed here.** (1) **A peer that crashed with one unflushed new page could not remount.** An extent reserved for a peer is allocated *whole* in the free map core 0 flushes at startup, while the peer writes those pages lazily — so a page whose PAGE_INIT was logged but never written back reads, at the next mount, as *allocated* by the map and *all zero* on the device; `ResidentBytes` called that a checksum `Corruption`, redo's checksum arm poisoned it and waited for a `FULL_PAGE_IMAGE` that never comes (its PAGE_INIT arm creates a page only from `NotFound`), and the mount refused with "the log cannot heal this page". Reachable in production the same way since PW1c-5 (`Expeditor` flushes the reserved extents before any peer runs); no test had restarted a peer with data. The store now answers `NotFound` ("allocated but was never written") for an allocated page the device holds as zeros or cannot address — the convention `Open()` already used for the free map — and `CreateAt` accepts such an id after proving the device holds nothing (`DeviceHoldsOnlyZeros`; a resident frame, the two map pages, or one nonzero byte still refuse), so redo's PAGE_INIT arm creates the page. Pinned at the store (`AnAllocatedPageNeverWrittenIsNotFoundNotCorrupt`, and the beyond-capacity test re-expected from Corruption to NotFound with its reason) and by the restart test's second iteration, whose mount replays a stream whose last page was never flushed. (2) **INSERT, UPDATE and DELETE spelled the affinity refusal as a bare `ERR <message>`**, dropping the `TXN_CONFLICT retryable=1` spelling `ErrorReply` exists to keep uniform — so `CrossCoreWriteRefused`, a `TxnConflict` since CC3, never carried the retryable bit on the wire from those three sites; all three go through `ErrorReply` now. **The review, applied**: C1 — the resident-frame branch of the claim read a stamp off a headerless page (the device branch refused them; the asymmetry could hand a peer write rights over a core-0 Waystone page on a byte coincidence) — the headerless test now precedes both branches; C2 — the never-written test rode the claim's checksum verification, i.e. on `CRC32C(8192 zeros) ≠ 0` (true, `0x90444623`, but stated nowhere) — it runs unconditionally now; C3 — a re-delivered grant appended duplicate extents to a vector `MayFault` scanned per fault, which S1 then removed outright: **both rights sets are page-sized bitmaps** now (`fault_rights_`, `write_rights_`; `HasFaultRight`/`HasWriteRight` state the range check once), so a repeated grant sets what is set and `MayFault` is two bit tests; C4 — a re-delivery request costs core 0 a catalog scan, an extent flush, three appends and an fsync, and the first form sent one per demanded relation per tick — **one request in flight per core** now (`grant_request_in_flight_`), released when a write grant is admitted or after `kRelationGrantRequestTicks` (1000 ticks, ~1 s at the 1 ms cadence) so a dropped request is asked again, pinned in the ring test (a refusal during the flight sends nothing; the waiting demand goes out after the grant lands); S3 — `RelationGrantDemand` moved to `core_affinity.hpp` so the dispatcher header stops pulling the scheduler and transport into every translation unit that includes it; S4 — the three peer refills and the grant request share one drain-tick timer instead of four; S5/S6 — the bit tests and the all-zero scan each have one home, and the claim guard asks one predicate per fault instead of two; S7 — the probe's null-guard comment named a caller that does not exist, corrected; S8 — the memory-resident premise, written out seven times, now lives in `relation_grant_service.hpp` with citations elsewhere, and `RelationGrantDemand::records()` (one test's convenience) is gone. **Declined, S2**: one send helper for the ring's eleven `MakeSendRetryTask` sites — they differ in header provenance (replies copy `request_id`/`session_core` from the request, broadcasts iterate the destination, two carry no payload), so the helper would take a prebuilt header and save four lines a site, and the workplan's recorded dedup target for these services is the receiver/request coroutine pair (§6's `lease_refill.hpp`), untouched here. **Noted, C5**: a page flushed, then zeroed by device damage, whose first in-range record is ordinary and whose FPI follows, now refuses at redo where it used to poison and heal — reachable only by damage between a checkpoint and a crash, and every never-written page was refused before, so a strict improvement in the reachable case; `docs/spec/page.md` §10 carries it. Overhead not measured (the v2 amendment); structurally, a peer's *read* fault now evaluates `MayFault` in release where it did not before, on the frame-load path only, never per row, and `CreateAt` on an allocated id costs one device read it did not before (recovery and bootstrap paths only) | PW1c-4, PL §9 |
| PW1c-8 | **Built 2026-08-26** (worktree `g1-peer-page-id-not-found`), the statement-shipping work order's **G1** gate: PW1c-4r's C1 had a second half nobody had reached. C1 refreshes a peer's free-map copy when a *relation grant* arrives; **nothing refreshes it when core 0 grows the system range between grants** - and a peer must read core 0's catalog pages to resolve any relation of its own. A catalog page allocated after a peer's last grant is therefore invisible to it forever, and the fault seam turned that staleness into a permanent, non-retryable `NotFound`: every write to that relation answered `ERR page id not found` for the rest of the mount. **measured** at `cores = 2` on the 2-CPU host: 58 `CREATE INDEX`/`DROP INDEX` pairs against a peer-owned relation, then page **15** - a catalog heap page whose bit is set on the device and absent from the peer's copy - refuses every later INSERT; and a `CREATE TABLE` that rotation places on the peer **heals** it, which is the proof rather than the argument. The two probes that did *not* reproduce are evidence too: `placement = rotate` skips the system core, so relation churn kept healing what it broke, and only DDL that grows the catalog without placing a relation on the peer reaches the defect. Fixed at the one seam where "stale" and "absent" are indistinguishable - a leased store adopts the device's map once before calling a page absent (`AdoptDeviceMapOnMiss`, the grant receivers' own operation), and the refusal names the id it withheld. **400 builds clean afterwards**, 32 MB -> 70 MB, the `cores = 1` shape. Costs one device read per resident region **on a miss** (an error path; core 0 never pays it, and a test pins that). Two residues stated rather than fixed: a page core 0 has allocated but not yet flushed still refuses - non-retryably, though the next statement re-adopts, so it clears itself - and a peer still reads core 0's catalog **bytes** off the device, so an unflushed catalog page is a stale read this fix does not reach. Overhead not measured (the v2 amendment) | PW1c-4r |
| PW1c-9 | **Built 2026-08-26** (worktree `g1-peer-page-id-not-found`), the statement-shipping work order's **G2** gate and its D5 invariant - *a refusal allocates nothing*: `CREATE INDEX` built the whole tree and seeded the relation's anchor slot **afterwards** (`exec/index_ddl.cpp` for a core-0 relation, `server/index_build_service.cpp` for a peer's), so once the entry table filled, every attempt allocated an index tree and discarded it - and nothing in this engine frees a page. **measured** on this 2-CPU host at `cores = 2` with `bench/index_refusal_storm_probe.py`, three phases with a clean stop between so only the refusing window is counted: before, 3,259 refusals in 30 s consumed **104,257 pages** - exactly **32.0 per refused attempt**, and past the 65,280 ids one free-map region covers, so it was FM's growth and nothing else that kept the instance alive where the pretasks' storm had died at "free map full". After: **27,267 refusals, marginal cost 0**, with a same-length control window pricing the per-mount extent reservation (64 pages) that both windows carry and neither earns. The fix is the check hoisted, not a new one: `storage::CheckAnchorRoomForIndex` is the predicate `SetAnchorIndexRoot` already applied, extracted so both share one bound and one message (a test asserts the two answer byte-identically). Its review moved it from the two call sites to `Catalog::CheckIndexDef` under `seed == kHere` - the door both build paths already came through - which deleted both blocks and, because `PrepareIndexDef` issues the index oid *after* that check returns, **stopped the local arm burning an oid on a refusal** as well. `kByOwner` asks nothing and must not: the anchor is then a page core 0 may neither seed nor fault, and the owner asks for itself when it comes through with its own `kHere`. Refusals also got 8.4× cheaper to issue, which is a consequence and not a goal. **Four things this does not close, all stated rather than implied and all in `docs/inflight/known-gaps.md`**: a build refused by a *spent extent lease* mid-way still keeps its pages (D5's other half - the check and the allocation are the same act there, so hoisting cannot reach it); a build that **succeeds** and is then dropped still orphans its tree, the gated reclamation leak and not this row's; `UpdateIndexRoot` is the same violation in a smaller costume, a root split allocating before it writes the slot; and the **write-rights** class, which the pre-check's `GetForRead` is deliberately too permissive to catch - so "a refusal allocates nothing" is true of the entry-table cap and must not be read wider. Overhead not measured (the v2 amendment); structurally, a successful `CREATE INDEX` now faults its anchor page once more than before, on a DDL path, never per row | PW1c-8 |

**The interim guard, built with this decision (2026-08-24):** a peer
dispatcher refuses INSERT/UPDATE/DELETE by name, beside PW4's DDL guard —
and the same review found and closed the **third** write route: `SHOW
PAGE` fetched through the mutating accessor, so a peer diagnostic dirtied
a core-0 page in release and could wedge that peer's catalog invalidation
permanently (`EvictClean` refuses on a dirty frame before
`InvalidateFromPeer` runs); it reads through `GetForRead` now. "A peer
listener is read-only" is true *because of* these three, not by nature.
Two recorded costs: a peer's refused foreign writes no longer reach
`cross_core_writes_.Record` (the §6 counters see nothing from peer
listeners — the guard fires before the relation is parsed), and the
foreign-write reply changed from retryable `TXN_CONFLICT` to
`Unsupported`, which is the honest bit on a session that can never
succeed by retrying.
It closes the release-build two-writer route `docs/inflight/known-gaps.md` names —
`MayWrite`'s enforcement is Debug-only, so without this a peer-accepted
INSERT into a rotated relation silently dirtied core 0's page — which
became reachable the day PW5 landed. PW6's write benchmark waits on
PW1c-4, not on more listener work.
