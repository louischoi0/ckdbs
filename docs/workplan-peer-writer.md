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
- **DDL on a peer needs a named refusal.** The catalog is read-only there, so
  a `CREATE TABLE` on a peer-accepted connection would reach `MayWrite` and
  fail with a page id — the exact failure `core_affinity.hpp` says the
  affinity check exists to prevent.
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
| PW1c | **Write rights on a peer-owned relation's pages.** Found by probe while building PW1b, not by reading: with both leases in hand a peer INSERT still fails, `core 1 may not write page 130`. `MayWrite` grants a peer only pages from its own *extent* lease, and CC7's grant is fault rights only, deliberately. **Needs a decision — §7b** | PW1, PW1b |
| PW2 | Route the two root-move catalog writes. **Needs a decision — §7** | PW1, PW1b |
| PW3 | **Built 2026-08-21.** A peer checkpointer through `RemoteCheckpointAnchor`: the completion checkpoint at `AttachTransport`, the `wal.md` §11 cadence in `Run()` | PW1 |
| PW3b | The **shutdown** checkpoint, which PW3 did not ship — core 0 has three checkpoint points and a peer now has two. A graceful restart replays up to one `checkpoint_interval_ms` of every peer's stream. **Needs a decision**: after the worker join both reactors are stopped, so a queued anchor send is never polled; it wants either one more core-0 ring drain after the join, or a different anchor on the shutdown path | PW3 |
| PW4 | Name the peer DDL refusal, and hang §5d's purge gate off it | none |
| PW5 | `SO_REUSEPORT` per-core listeners; share the credential store and TLS context without sharing them mutably | PW1, PW4 |
| PW6 | The benchmark: `placement = rotate`, one writer connection per core, per-core relations. The first cross-core number that is a speedup and not a cost | PW1-PW5 |

PW1 + PW1b + **PW1c** make a peer write a heap relation with no secondary
index — PW1c is the door the probe found behind PW1b, and it was not in this
workplan's original three because it is not on the *id* path at all. PW1c +
PW3 + PW5 is then a shippable slice with a real number at the end of it and a
stated shape restriction; PW2 removes the restriction. Taking §7b's option
(c) would collapse PW1c and PW5 into one task and is the current
recommendation.

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
implementation detail: **the first INSERT into a relation on a given peer
fails retryably, and no later one does.** That is exactly what that lease's
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
- **(b) Allocate a peer-owned relation's pages from the owner's lease at
  DDL.** CC7 records this as considered and rejected — "a new cross-core
  allocation protocol inside DDL that still needs a creation-time write
  exception" — but it was rejected when no peer could write at all, and the
  premise it was rejected under is the one PW1 removed.
- **(c) Ship the DML statement to the owner core** (`crosscore.md` §6's first
  bullet, "involves no pipeline"). Then no peer ever writes a page core 0
  allocated, because the *owner* executes the write, and PW1c disappears
  rather than being solved. This is the route that also fixes PW5's
  no-steering problem, and it is the one CLA would pick.

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

CLA's reading is that **(c) is the right first move and (b) is the right
end state** — (c) gets an honest number under a named restriction without
spending a decision, and (b) removes the constraint for every core rather
than building a cross-core protocol inside the insert path. (a) buys the
widest scope for the most reasoning about failure. But this is `crosscore.md`
§9's kind of decision and belongs to whoever owns it.
