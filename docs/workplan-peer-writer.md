# Workplan — a writer for peer-owned relations

Owning specs: `docs/crosscore.md` (M3, CC3, §6), `docs/workplan-crosscore.md`
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

`docs/known-gaps.md` states it: cross-core writes are refused (CC3), DML
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
mount replays it whole, and `docs/known-gaps.md`'s already-open mount-latency
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
| PW3b | The **shutdown** checkpoint, which PW3 did not ship — core 0 has three checkpoint points and a peer now has two. A graceful restart replays up to one `checkpoint_interval_ms` of every peer's stream. **Needs a decision**: after the worker join both reactors are stopped, so a queued anchor send is never polled; it wants either one more core-0 ring drain after the join, or a different anchor on the shutdown path | PW3 |
| PW4 | **Built 2026-08-24** (`r1-peer-ddl-refusal`). CREATE/ALTER/DROP refused whole at dispatch wherever the catalog is read-only (`SetCatalogReadOnly`, set by `CoreRuntime` for every non-system core; `PeerDdlRefused` names the core and where DDL runs). Predicated on the incapacity rather than `core_id_`, so the P4e harness's core-1 stand-ins over a writable store keep building fixtures. §5d's purge gate cites the guard and stays as defense in depth | none |
| PW5 | **Built 2026-08-24 with a named restriction** (`r1-peer-ddl-refusal`): `peer_listeners = on` binds every core's listener with `SO_REUSEPORT` (core 0's socket carries the flag too - the first binder must), `CoreRuntime::ListenAndAttach` wires each to its own reactor and dispatcher on the startup thread. **The tls/auth half is not built**: the combination is refused at boot (`CheckPeerListenerConfig`), because the credential store and TLS context are constructed on core 0's stack and sharing them immutably is still open. Off by default | PW1, PW4 |
| PW6 | ~~The benchmark: `placement = rotate`, one writer connection per core, per-core relations. The first cross-core number that is a speedup and not a cost~~ **Measured 2026-08-25 with the host's bound** (worktree `pw6-rotate-benchmark`, `v2.0.0-48-g314a06d`, `bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md`). The client half: `SHOW META` grew `core=` (a session cannot choose its core under SO_REUSEPORT, so it must be able to see it), and `tools/multicore_benchmark.py --placement rotate --peer-listeners` hunts sessions per owner core by asking, retries retryable refusals with the whole wait as the latency, and `--verify`s the surviving row count. **The number this row asked for — a speedup from two writer cores — is unmeasurable on this host**: the server refuses `cores` above `hardware_concurrency()` (two here), and at `cores = 2` rotation skips the system core so every rotated relation is core 1's; the runnable cell is the peer write path against core 0's at equal parallelism — **two writers 0.977× against a control that measured 0.982×, every per-statement median within 2% (INSERT 1,863 vs 1,862 µs, point-SELECT 25 vs 26 µs)** — the peer path costs nothing this harness resolves, flat at 200/2,000/10,000 rows; PostgreSQL 16.14 is 7% behind on statements and 2× on pk lookups. A ≥3-CPU host runs `--cores 3 --tables 2`; the results file's §7 fdatasync-on-a-second-file probe decides first whether two writer cores can overlap their syncs on one ext4 volume. **Three findings, each recorded in `docs/known-gaps.md`, the first being the next job**: (1) **every lease refill lags by hundreds of milliseconds to seconds under four active sessions on one peer** — relations 3 and 4 wait 0.5–1.75 s for their first INSERT on the row-id refill (the servers' logs hold only `row-id lease ... is spent` refusals there and never `RelationWriteRightsPending`, so PW1c-7's request latch is off the path — the results file's first draft attributed it there from the source and was corrected from the logs), the trx-id lease is spent mid-run with a quarter-window of headroom, and the 64-page extent lease is spent so **1, 13 and 51 INSERTs per run were lost**; an idle refill is 2–7 ms, core 0 logged no failed grant, and the mechanism is untraced (`PickNextGroup` picks the ready group with the lowest consumed-time/share ratio, so starvation of the parked refill by the query group is not the obvious reading) — the trace is `SHOW META` counters per lease kind (requests, grants, longest wait in ticks) and a debug-level cell C at 200 rows; (2) **three refusals promise a retry without the wire's bit** — the row-id, trx-id and extent leases' `ResourceExhausted` prints as bare `ERR` because only `TxnConflict` is `IsRetryable`, and the extent one lost rows because a client retrying on the bit did not retry it; (3) **a point-SELECT on a core with a committing session waits out that session's fdatasync** (973 µs beside a writer, 37 alone — the drain runs inline on the reactor), the first number for the open I/O-backend decision. Overhead not measured beyond the cells (the v2 amendment) | PW1-PW5 |

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
**EXPLICIT stays refused on a peer** (the id-ceiling catalog write);
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

## 8. The PW1c decision — decided 2026-08-24 (operator-delegated)

**The write handoff, riding PL-B.** A peer gains write rights over a
rotated relation's creation pages through the handoff contract
`docs/spec-page-lsn-cross-stream.md` §9 ratifies, upgrading CC7's
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
blueprint's R5 mover and `feat-physical-optimizer.md` §6 gate 3 both need
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
| PW1c-6 | **Built 2026-08-24 as the refusal half** (worktree `pw1c1-handoff-record`): `CREATE INDEX` on a relation this core does not own is refused at dispatch, before the DDL scope draws a transaction id, naming the task and the offending table token's byte — the tree would be built from core 0's free map into pages the owner's fault grant covers only by the 64-page-extent accident, with no handoff and no write grant. The **grant-extension half is deliberately deferred to PW2**: the owner's write path refuses indexed relations anyway (PW1c-5's shape gate), so granting index pages today funds nothing; when PW2 routes the root-move catalog writes, extend the publish grant and the handoff to the index pages and delete this refusal. `DROP INDEX` stays admitted — it shrinks the unsound set | PW1c-4 |
| PW1c-7 | **Built 2026-08-24** (worktree `pw1c7-restart-ownership`): **ownership survives a restart, and the stamp is what carries it.** The PW1c-4r debt asked for "re-establish grants at mount from `sys.tables.owner_core`"; the probe that scoped it found the debt understated. A peer's *extent lease* is carved fresh at every mount — `LeasedIdSource` remembers only this run's grants, `Expeditor::Serve` reserves a new extent per peer at startup, and nothing persists which extents a core held — so a restart loses not only the creation-page grants but fault rights (Debug) and write rights (every build) over **every page the peer allocated itself**: a heap chain's second page, a btree's leaves, a var-heap's growth. No catalog-derived grant can name those pages; only a walk could, and core 0 cannot soundly walk a peer's pages at mount. What already names them, durably, is the fact PL §9 rule 4 made: every page a stream writes carries that stream's stamp, and rule 6 lets no page leave a stream unrestamped — **the stamp is the durable form of ownership**. So `DevicePageStore::ResidentBytes` **claims from the stamp**: a leased store faulting a page outside its lease, its fault grants and its write rights reads the stamp — off the resident frame redo left at mount, else off the device, checksum-verified, the read handed to the miss path rather than repeated — and admits the page to read and write when the stamp names this core; a foreign stamp or 0 leaves every refusal exactly as it was. Attempted only where a check would refuse, so a leased or granted page pays nothing and core 0 its one pointer compare. The write-rights set became a page-sized bitmap (the headerless map's precedent: same addressing, different meaning) because its population grew from a handful of creation pages to every page touched since mount, and `MayFault` consults it — a write-granted or claimed page is readable by that alone, which also makes the 95b45e8 review's C2 structural. **The other half is re-delivery**, for what the stamp cannot claim: a creation page this core never acquired (stamp 0, core 0's `page_lsn`) after a crash before the acquisition restamp, a grant lost to a full ring, or `GrantRelationWrite`'s mid-grant abandon (the 25059bf review's C-5) — and only core 0 can hand it off, since rule 1 puts the record in the giver's stream. `CheckWriteAffinity` gained a **rights probe** after the shape gate: all three creation pages must be writable after a claim attempt (a crash between the restamp flush and the admission can leave them split), else the demand is recorded (`RelationGrantDemand`, unique per tick) and the statement refused retryably **by name** — `RelationWriteRightsPending`, `TXN_CONFLICT`, naming this task — where the store's every-build backstop named a page id. The drain tick sends `kRelationGrantRequest` (24) per demanded relation with no in-flight state and no reply: the answer is the ordinary grant pair, produced by core 0's `RegisterRelationGrantHandler` running **the same publish hook** CREATE TABLE runs (a named `publish` in `Expeditor::Serve`, two callers, so nothing else ever hands a relation off) after checking `sys.tables.owner_core` names the requester — a request for a foreign relation is dropped, since granting it would be the two-writer route. Idempotent by construction: a repeat PAGE_HANDOFF is analysis's no-op (PW1c-4's one-liner), and the receive side takes no second acquisition on a page whose stamp already names it (`GrantRelationWrite` now asks the stamp as well as the rights, and asks after the fault). **No mount-time re-grant loop, by decision**: with the claim it would only re-deliver what every page already states, and the demand path covers the one case it cannot — which also keeps mount cost independent of the relation count. `PageStore` grew a virtual `MayWrite` (default true) so the dispatcher can ask its interface. Pinned by: the store claiming an own-stamped page on a write fault and on a read fault while refusing foreign and unstamped ones in every build; a 600-row peer relation surviving a restart with a fresh lease and nothing granted — read whole, then written again — once with its pages flushed and once with them living only in the log (redo's replay leaves them resident without rights, stamped as it applied them); and, over a real ring, an unacquired relation refused by name, asked for on the tick, re-published exactly once by core 0, granted, and written on the retry, with a request for a foreign relation dropped. **Residuals, named**: a page formatted and flushed before its first content record is unstamped (`LogPageInit` does not stamp, by design) and unclaimable until redo or a write stamps it — unreachable while a page's format and first write share one statement on one reactor, recorded so nobody relies on it; and nothing *revokes* lease ownership at a handoff, so the R5 mover must drop a departing page from the giver's `LeasedIdSource` as well as restamp it, or the giver keeps write rights the stamp no longer allows — `GrantFaultPages`' "nothing revokes" rule now has a second reader. **Decided under §8's delegation precedent, not asked**: the alternative — a persisted per-core extent directory — is a new system page and a format bump that the future page-granular mover would outgrow, while the stamp is already durable, already exact per page, and already what redo trusts. **Two findings the restart test made on the way, both pre-existing and both fixed here.** (1) **A peer that crashed with one unflushed new page could not remount.** An extent reserved for a peer is allocated *whole* in the free map core 0 flushes at startup, while the peer writes those pages lazily — so a page whose PAGE_INIT was logged but never written back reads, at the next mount, as *allocated* by the map and *all zero* on the device; `ResidentBytes` called that a checksum `Corruption`, redo's checksum arm poisoned it and waited for a `FULL_PAGE_IMAGE` that never comes (its PAGE_INIT arm creates a page only from `NotFound`), and the mount refused with "the log cannot heal this page". Reachable in production the same way since PW1c-5 (`Expeditor` flushes the reserved extents before any peer runs); no test had restarted a peer with data. The store now answers `NotFound` ("allocated but was never written") for an allocated page the device holds as zeros or cannot address — the convention `Open()` already used for the free map — and `CreateAt` accepts such an id after proving the device holds nothing (`DeviceHoldsOnlyZeros`; a resident frame, the two map pages, or one nonzero byte still refuse), so redo's PAGE_INIT arm creates the page. Pinned at the store (`AnAllocatedPageNeverWrittenIsNotFoundNotCorrupt`, and the beyond-capacity test re-expected from Corruption to NotFound with its reason) and by the restart test's second iteration, whose mount replays a stream whose last page was never flushed. (2) **INSERT, UPDATE and DELETE spelled the affinity refusal as a bare `ERR <message>`**, dropping the `TXN_CONFLICT retryable=1` spelling `ErrorReply` exists to keep uniform — so `CrossCoreWriteRefused`, a `TxnConflict` since CC3, never carried the retryable bit on the wire from those three sites; all three go through `ErrorReply` now. **The review, applied**: C1 — the resident-frame branch of the claim read a stamp off a headerless page (the device branch refused them; the asymmetry could hand a peer write rights over a core-0 Waystone page on a byte coincidence) — the headerless test now precedes both branches; C2 — the never-written test rode the claim's checksum verification, i.e. on `CRC32C(8192 zeros) ≠ 0` (true, `0x90444623`, but stated nowhere) — it runs unconditionally now; C3 — a re-delivered grant appended duplicate extents to a vector `MayFault` scanned per fault, which S1 then removed outright: **both rights sets are page-sized bitmaps** now (`fault_rights_`, `write_rights_`; `HasFaultRight`/`HasWriteRight` state the range check once), so a repeated grant sets what is set and `MayFault` is two bit tests; C4 — a re-delivery request costs core 0 a catalog scan, an extent flush, three appends and an fsync, and the first form sent one per demanded relation per tick — **one request in flight per core** now (`grant_request_in_flight_`), released when a write grant is admitted or after `kRelationGrantRequestTicks` (1000 ticks, ~1 s at the 1 ms cadence) so a dropped request is asked again, pinned in the ring test (a refusal during the flight sends nothing; the waiting demand goes out after the grant lands); S3 — `RelationGrantDemand` moved to `core_affinity.hpp` so the dispatcher header stops pulling the scheduler and transport into every translation unit that includes it; S4 — the three peer refills and the grant request share one drain-tick timer instead of four; S5/S6 — the bit tests and the all-zero scan each have one home, and the claim guard asks one predicate per fault instead of two; S7 — the probe's null-guard comment named a caller that does not exist, corrected; S8 — the memory-resident premise, written out seven times, now lives in `relation_grant_service.hpp` with citations elsewhere, and `RelationGrantDemand::records()` (one test's convenience) is gone. **Declined, S2**: one send helper for the ring's eleven `MakeSendRetryTask` sites — they differ in header provenance (replies copy `request_id`/`session_core` from the request, broadcasts iterate the destination, two carry no payload), so the helper would take a prebuilt header and save four lines a site, and the workplan's recorded dedup target for these services is the receiver/request coroutine pair (§6's `lease_refill.hpp`), untouched here. **Noted, C5**: a page flushed, then zeroed by device damage, whose first in-range record is ordinary and whose FPI follows, now refuses at redo where it used to poison and heal — reachable only by damage between a checkpoint and a crash, and every never-written page was refused before, so a strict improvement in the reachable case; `docs/page.md` §10 carries it. Overhead not measured (the v2 amendment); structurally, a peer's *read* fault now evaluates `MayFault` in release where it did not before, on the frame-load path only, never per row, and `CreateAt` on an allocated id costs one device read it did not before (recovery and bootstrap paths only) | PW1c-4, PL §9 |

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
It closes the release-build two-writer route `docs/known-gaps.md` names —
`MayWrite`'s enforcement is Debug-only, so without this a peer-accepted
INSERT into a rotated relation silently dirtied core 0's page — which
became reachable the day PW5 landed. PW6's write benchmark waits on
PW1c-4, not on more listener work.
