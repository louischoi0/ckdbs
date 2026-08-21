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

This is the same shape as row ids, and **that half is already built** —
`catalog::RowIdLeaseTable` is installed into every non-zero core's catalog
(`src/server/core_runtime.cpp:125`), `AllocateRowId()` draws from the block,
and `kRowIdLease` refills over the ring
(`include/kds/server/row_id_lease_service.hpp`, `kRowIdLeasePerGrant = 4096`).
The trx-id lease is that service again with a different payload.

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

### PW-B3. A peer has no checkpointer

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
| PW2 | Route the two root-move catalog writes. **Needs a decision — §7** | PW1 |
| PW3 | Wire a peer checkpointer through `RemoteCheckpointAnchor` | PW1 |
| PW4 | Name the peer DDL refusal, and hang §5d's purge gate off it | none |
| PW5 | `SO_REUSEPORT` per-core listeners; share the credential store and TLS context without sharing them mutably | PW1, PW4 |
| PW6 | The benchmark: `placement = rotate`, one writer connection per core, per-core relations. The first cross-core number that is a speedup and not a cost | PW1-PW5 |

PW1 alone makes a peer write a heap relation with no secondary index, so
PW1 + PW3 + PW5 is a shippable slice with a real number at the end of it and
a stated shape restriction. PW2 is what removes the restriction.

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
