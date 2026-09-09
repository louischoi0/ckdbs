# Work order AT — AR0 M3: Uniformity

Written 2026-09-09 on `m3-at` at `df8cc5f` (`v2.7.0-309-gdf8cc5f`; no v3
tag exists, AR0-M6). AT-3 below is a source read at that commit, and it is
not a copy of `ar0-5-amendment-uniformity.md` §4 — six of that list's
entries no longer describe the tree (AT-7), which is the same failure mode
AR0-5-V found in itself and the reason this order re-reads rather than
inherits.

**Status: AT-S0 lands now; no code stage starts before the operator's
word.** M2 closed on 2026-09-09 (`workorder-ao-m2-lock-family.md` AO-8) and
its closing section states the absence this document fills: *"AR0 §8 step 6
is M3, renamed 'Uniformity' and lettered AT. **No work order exists for
it**."*

**Three decisions were taken by the operator in plan mode on 2026-09-09**
and are recorded where they land:

1. **Scope** — AT is *Uniformity and the ring's tail*: AR0-5 §4's M3 list,
   §7's order, D17–D22, E8, E13, and AU-S5/S6, which
   `workorder-au-ring-retirement.md` already places inside AT. D7's Cabin
   invariant, D9(a)'s `S` fence, AR2's E3/E5/E10 and AR1's AQ/AR go to a
   following letter (AT-1, AT-0 item 6).
2. **2PC retires inside AT**, as its late stages, with AO-R8's fault net
   11 s → 1 s in the same order (AT-R6, AT-S6).
3. **The relation `IS` is taken at resolve time** — AR0-5 §2.1's first
   alternative, which that section obliges AT to pick between rather than
   inherit (AT-R1, AT-S1).

---

## AT-1 — The direction, and what it is not

**What Uniformity is.** AR0-5 §0, the operator's word of 2026-09-05:

> No page and no relation has an owner. Core 0 is not a role. Every core
> reads, writes and synchronises through the same primitives, and the
> system relations (`sys.*`, the superblock, the free map) are relations
> and pages like any other, protected by the lock family, the page latch
> and the shared pool — not by a writer's identity.

Three roles go and three synchronisation primitives replace them (AR0-5
§2): the schema version word, shared allocators with authority-free
per-core caches, and the writer thread AL-S1a/S1b already built.

**The axis.** M2's was refusal → wait. AT's is **asymmetry → primitive**:
every place the engine today answers differently depending on which core
asks, or refuses because a structure's owner is elsewhere, is either
retired with the ownership that justified it or given a lock. AT-3 B is the
census. AT is not evaluated on throughput; AT-S13 measures what M2 handed
on and claims nothing else.

**What it is not.**

- **Not D7, not D9(a), not AR1.** AR0 §8 step 6 defined M3 as *"Cabin
  invariant removal (D7), Assertion, FK (D9)"*; AR0-5's D22 renamed M3
  "Uniformity". Both definitions were live until the operator's answer of
  2026-09-09 split them. D7 with D1(b)'s gap locking, D9(a)'s `S` fence
  with the `FkPendingDeleteTable` asymmetry AO-8 re-pointed, AR2's E3, E5
  and E10, and AR1's AQ/AR are **the following letter's cargo**, named here
  so the exclusion is a handoff and not a gap (AT-0 item 6).
- **Not the lock family.** M2 built it; AT is its first large consumer, and
  the one that makes the relation `IS` load-bearing for a *result* rather
  than for a wait.
- **Not an isolation-level change.** D1(b) stays where AR0-M1 put it.
- **Not M4** (AR0 §8 step 7).

**What lands now.** This document and `index.md`'s row. No code, no spec
edit, no test. Three things are deliberately not landed with it, each a
decision rather than an omission: `docs/spec/catalog.md`, which AR0-5 §2.1
cites and which does not exist (AT-7 C) — it is created by AT-S2, the stage
whose rule it carries; `rules.md` §3's declared-shared row for the catalog,
which lands with AT-S3; and `CLAUDE.md`'s milestone rows, which the
maintenance rule flips when a decision *lands*.

---

## AT-2 — Where this sits against the D- and E-items

| item | mark at `df8cc5f` | bearing on AT |
|---|---|---|
| D3 (log appender) | **answered in practice, against the proposal** — AL-R1 built every-core-appends under one latch with a single writer thread (`known-gaps.md`, "Decisions the revision has not taken") | AR0-5 §2.3 strikes "log core = core 0" as a sentence with no mechanism. Nothing for AT to build |
| D4 (free map / superblock rule) | struck by AR0-5 §2.2 | **AT-S4, AT-S5**: the free map is a shared frame under its page latch; page 0's ceilings advance by CAS |
| D10 (affinity weight) | provisional 0 | unchanged; after D18 it is the only place affinity is read |
| D11 (`sys.range_affinity`) | AR0-M5: the R5 mover is retired; the relation **does not exist and stays absent** | nothing to build; AT-S9 states it |
| D14 (format migration) | pending | AT has **its own format event** (D17, AT-R7) and it is not AM-S4's |
| D17 | **marked 2026-09-05**: columns dropped at M3 on AT's own event; a pre-M3 value ignored on read | **AT-S9**; `rows.hpp:978`'s `static_assert` moves there |
| D18 | **marked**: affinity kept as a statistic and optimizer hint, D10 weight 0 until AS-E; placement NS10 deleted | **AT-S9**; and AT-0 item 4, because a weight-0 hint leaves the remote-step protocol with a consumer that never fires |
| D19 | **marked**: every core listens (`SO_REUSEPORT`); a session lives where it was accepted; no handoff | **AT-S8**, and smaller than the mark assumes — the mechanism is in the tree behind `peer_listeners` (AT-3 F) |
| D20 | **marked as constants carried, not decided**: 4,096 for trx-id and row-id caches, one extent | **AT-S4**; any change from them needs a measurement |
| D21 | **marked**: schema word in `Expeditor`, memory only, bumped **before** the DDL's relation `X` releases | **AT-S2**; AR0-5 §8's read-order hazard is AT-S1's cell |
| D22 | **marked**: M3 is "Uniformity", letter **AT** | this document; `index.md`'s row |
| D25 (AR0-6) | kind enum frozen by `static_assert`, count **34** since AU-S3 | AT-3 C: **four of the 34 are already dead** and are struck, not converted |
| E7 (execution default) | measurement-gated; C1/C2 measured, **C3 answered that its shape does not exist** (AO-S7) | AT-R5 settles the *correctness* half — routing is not a correctness condition. The default is AT-0 item 2 and AT-S13's cell |
| E8 (NS10's verb) | user-visible | **AT-S9**, and the verb goes with the policy rather than surviving it |
| E9 (`core_count` pinning) | **withdrawn by the operator 2026-09-08** — it argued from two dead premises | AT-S9 decides the count's fate with the rest of core specialization, which is what the withdrawal hands it |
| E13 (catalog rows borrowable) | OPEN, M3 | **AT-S3**; AR0-5 §3 already reads it as closed-yes and AT builds it |
| E1, E4, E11, E12 | ratified (AR2-A) | built in M2; AT changes none of them |
| E3, E5, E10 | M3 | **not here** — the following letter's, per the scope answer |
| AO-R8 (the fault net) | 11 s while 2PC is in the tree, 1 s once M3 retires it | **AT-S6** is the stage that earns the change |
| AO-R14 (guard in M2, route in M3) | the split | **AT-S5** is the route half |

---

## AT-3 — The survey at `df8cc5f`

### A. What AR0-5's retire list names that is already gone

`ar0-5-amendment-uniformity.md` §4's M3 list was written at `410377e`, one
day before `workorder-aw-m1-close.md` AW-S1b (`af86026`) deleted eight
files. At `df8cc5f`:

| named for retirement at M3 | state at `df8cc5f` |
|---|---|
| `LeasedIdSource`, `ExtentRefill`, `MaybeRefillLease`, the extent spent-lease `TxnConflict` | **gone.** `include/kds/storage/extent_lease.{hpp,cpp}` and `include/kds/server/extent_lease_service.{hpp,cpp}` are not in the tree |
| `MayFault`, `GrantFaultPages`, `GrantWritePages`, `RelationGrantDemand`, `MaybeRequestRelationGrants` | **gone**; `MayFault` and `RelationWriteRightsPending` survive as one prose mention each, in comments |
| `MayWrite`'s "remaining arms — the lease arm and the system-range arm" | **one arm.** See B |
| the `(M5)` comment census, "twelve sites" | **eight**, in five files: `include/kds/catalog/core_placement.hpp`, `include/kds/server/range_alloc.hpp`, `include/kds/server/core_runtime.hpp`, `src/server/expeditor.cpp`, `src/server/core_runtime.cpp` |
| `core_runtime.hpp` asymmetries 1–3 | asymmetry **2 has already been rewritten** by AW-S1b — it now reads "allocation reaches the one free map, under the structure latch … it came from a per-core extent lease until AW-S1b". AT strikes 1 and 3 and edits one sentence, not three asymmetries |

Three lease families were named; **two remain** (trx ids, row ids). This is
the single largest correction to AT's size and it is downward.

### B. `MayWrite` is one arm and one call site

`src/storage/device_page_store.cpp:980` declares it; the whole predicate is
`src/storage/device_page_store.cpp:1015`:

```
if (page_id < first_evictable_page_id_) return CurrentCore() == 0;
return true;
```

The lease arm, the write-grant bitmap and the stamp claim went at AW-S1b;
AW-a collapsed the two boundary members into one and made the surviving arm
ask `CurrentCore()`. It has **one caller**, the store's own `mark_dirty`
gate at `src/storage/device_page_store.cpp:738`; the four external callers
AW-a's text names are gone. `Session::MayWriteOn`
(`include/kds/server/session.hpp:343`, called at
`src/server/command_dispatcher.cpp:6846`) is a different predicate — the
session's core check — and is **not** retired by this row.

So AT-S5's deletion is small. What it costs is not: the arm is the last
enforcement that only core 0 writes the superblock, the free map and the
catalog pages, and AT-R11 states what replaces it.

### C. Four ring kinds are already dead

`include/kds/sched/ring_message.hpp`'s enum carries 34 kinds beside
`kUnset` (D25's count). **Four have no producer and no handler** outside
the enum and `spsc_ring.cpp`'s name table: `kExtentLease` (17),
`kRelationFaultGrant` (21), `kRelationWriteGrant` (23),
`kRelationGrantRequest` (24) — AW-S1b removed their consumers and AU-R4's
freeze meant nobody struck them. **30 kinds are live**, not the 31
`workorder-au-ring-retirement.md`'s AU-S5 row assumes.

Grouped by what retires them:

| group | kinds | retired by |
|---|---|---|
| already dead | 4 (above) | **AT-S0's successor**, struck on sight (AT-R10) |
| catalog broadcast | `kCatalogInvalidate` | AT-S2 |
| allocators | `kTrxIdLease`, `kRowIdLease` | AT-S4 |
| shipped statements | `kShippedStatementRequest/Reply`, `kShippedRowDesc` | AT-S5 (writes); reads per AT-0 item 4 |
| foreign-key probes | `kFkProbeRequest/Reply`, `kFkReverseProbeRequest/Reply` | AT-S5 |
| index and assertion builds | `kIndexBuild{Request,Reply,Done}`, `kAssertionBuild{Request,Reply,Done}` | AT-S5 |
| 2PC | `kTxn{Prepare,Decide,Resolve}{Request,Reply}` | AT-S6 |
| statistics | `kAccessStatsBatch` | AT-S7 |
| checkpoint | `kAnchorWrite` | AT-S8 |
| remote steps | `kStep{Open,Batch,Eof,Credit,Cancel,Error}` | **undecided** — AT-0 item 4 |

That last row is why AU-S6 may not be reachable inside AT: the transport
cannot be deleted while one protocol still uses it, and whether the
remote-step protocol survives is a decision, not a consequence.

### D. The catalog has two invalidation paths and they disagree by design

- `Catalog::InvalidateFromPeer()` (`src/catalog/catalog.cpp:951`), the
  `kCatalogInvalidate` handler and the only invalidation a peer ever
  receives, clears every cached fact **without bumping the version
  counter**, deliberately — the counter is per-instance and means nothing
  across cores.
- `Catalog::InvalidateAfterCompensation()` (`src/catalog/catalog.cpp:847`),
  called from `CommandDispatcher::EndDdlScope`
  (`src/server/command_dispatcher.cpp:9054`) on **either ending** of any
  transaction that wrote catalog rows, **does** bump it, because the rows
  really changed on this instance.

The broadcast reaches peers through
`CoreRuntime::InvalidateCatalog()` (`src/server/core_runtime.cpp:922`,
declared `include/kds/server/core_runtime.hpp:437`), published at
`src/server/expeditor.cpp:1274`.

**AT-S2's word replaces both.** That is why AT-R2 requires it to be a
*generation* counter that every invalidation path bumps: today's counter is
not one, and a cache validated against it is correct on core 0 and wrong on
every peer.

### E. The `IS` gap, and where the resolve happens

`known-gaps.md`'s `## Locks` entry, verified at `cf3d0d0`: the relation `IS`
is declared only by a statement's outermost walk. `src/exec/step_vm.cpp:1960`
and `:2030` guard the position report on `index == 0`, so a nested walk — a
join's inner relation — declares nothing; and
`src/server/remote_step_service.cpp` passes no `PositionSink` at any of its
three execution sites (`:458`, `:886`, `:1080`).

The resolve AT-R1 must precede is `Catalog::InitTableAccess(Oid)`
(`include/kds/catalog/catalog.hpp:684`), whose result is the
reference-stable `const TableAccess*` a statement holds for its own
duration (`include/kds/catalog/catalog_cache.hpp:98-102`).

### F. D19 is half built

`TcpServer::Listen(port, reuse_port)` sets `SO_REUSEPORT`
(`src/server/tcp_server.cpp:36`, `:44-51`), a peer's listener already opens
with it (`src/server/core_runtime.cpp:1298`), and the arrangement is a
config flag, `peer_listeners`, defaulting off
(`include/kds/server/expeditor.hpp:173`; the listener opened at
`src/server/expeditor.cpp:1534`).

**The blocker is not the socket.** `CheckPeerListenerConfig`
(`src/server/expeditor.cpp:127-145`) refuses `peer_listeners = on` combined
with TLS or SCRAM because *"the credential state is core-local"* — which is
itself an ownership residue, and therefore AT's to remove rather than to
inherit (AT-0 item 8).

### G. Two Cabin stores

`std::optional<stats::CabinStore>` is a member of **both**
`include/kds/server/core_runtime.hpp:629` and
`include/kds/server/expeditor.hpp:779`. AT-S7's "one instance store
partitioned by `expr_id`" (AR1 §11) is a real unification, not a move.

### H. What is left of the leases

Two families, each a spent-block `TxnConflict`:
`include/kds/txn/trx_id_lease.hpp:66-67` and
`include/kds/catalog/row_id_lease.hpp:116`. The refill paths are
`CoreRuntime::MaybeRefillTrxIds` (`src/server/core_runtime.cpp:1193`) and
`MaybeRefillRowIds` (`:1080`), both driven from `:1025-1026`, with
`src/server/trx_id_lease_service.cpp` and `src/server/row_id_lease_service.cpp`
carrying the ring protocol. The block constants D20 carries are
`txn::kTrxIdBlockSize` (defined `include/kds/txn/trx_id.hpp:74`, aliased
`include/kds/server/trx_id_lease_service.hpp:51`) and `kRowIdLeasePerGrant`
(`include/kds/server/row_id_lease_service.hpp:32`).

### I. `docs/spec/catalog.md` does not exist

AR0-5 §2.1 requires the word-then-lock order to be *"written in that order
in `catalog.md` so nobody later removes the lock because the word 'covers
it'"*. `docs/spec/` has no such file. AT-S2 creates it or names the spec
that takes the rule; AT-S0 does neither (AT-1, "what lands now").

---

## AT-4 — Rulings (CLA's proposals except where marked)

**AT-R1 — The relation `IS` is taken at resolve time.** *Operator's ruling,
2026-09-09.* Before the catalog read, held for the statement, released at
its end. AR0-5 §2.1 obliges AT to pick this or a re-read of the word after
the first `IS`; the first is taken because it closes the window for **every**
statement shape at one seam, including the nested walks and remote steps
that declare nothing today (AT-3 E), where the second alternative would
make the word load-bearing for correctness in the same breath §2.1 says it
must not be. The position `IS` on the slice, which AO-S6e-b built, is
unchanged and keeps its own scope.

**AT-R2 — The schema version word is a generation counter every
invalidation path bumps.** D21 as marked: one `std::atomic<uint64_t>` on
`Expeditor`, memory-resident (every cache is empty at mount), bumped
**before** the DDL's relation `X` is released. It replaces both of AT-3 D's
paths, and the peer path bumps nothing today — so "keep the existing
counter" is not available to AT-S2, and a cache validated against a
non-bumping path is the failure this ruling exists to prevent.

**AT-R3 — Correctness never rests on the word.** The lock is the argument,
the word is the fast path, and they are written in that order. AT-S1's cell
is AR0-5 §8's inverted case — the word deliberately not bumped, asserting
the DDL is still blocked — and it belongs to **S1, not S2**, because S1 is
the stage that makes it true.

**AT-R4 — An allocator is a `fetch_add` with an authority-free cache.** A
per-core cache takes a block by one `fetch_add(block)`; a miss is another
`fetch_add`, so there is no refill *protocol*, no grant, and no refusal. A
cached block is lost at crash, which is today's behaviour. **AN-R13 is
kept**, re-scoped from "lease" to "cache" in wording only: the floor is
pinned by the block, not by the lease protocol, so an idle core still
freezes inside its cache and `MaybeBurnIdleTrxIdBlock` stays load-bearing.

**AT-R5 — A write runs where the session is.** R12 and AO-R14's other half.
Routing survives only as D18's statistic and the optimizer's weight-0 hint;
it is no longer a condition of correctness, and no code path may consult it
for admission or for the right to write.

**AT-R6 — 2PC retires with the ownership that justified it.** *Operator's
ruling, 2026-09-09.* Once a write runs where the session is, a transaction
has no participant on another core, so the coordinator/participant protocol
loses its traffic rather than its callers — AR0 §4.5 already says *"2PC no
longer exists inside a single node."* AO-R8's fault net falls **11 s → 1 s**
in the same stage, which is the only place that change has its stated
precondition, and AO-0 item 7 (`in_doubt_ceiling_ms`, inert since AO-S3) is
settled there rather than carried again.

**AT-R7 — AT has its own format event.** D17 as marked: `sys.tables.owner_core`
(`include/kds/catalog/rows.hpp:104`) and `sys.ranges.owner_core` (`:955`)
are dropped, `rows.hpp:978`'s `static_assert` moves with the layout, and a
pre-AT volume's value is **ignored on read** — no mount refusal. It is not
AM-S4's event, which is the page-header stamp.

**AT-R8 — Every retired refusal is struck from AO-3's census with the
mechanism that replaces it, not deleted.** AR0-5 §5 names this method as one
of the things M3 does not change, and AT is where most of the census's
premises expire.

**AT-R9 — G2 holds at every stage.** Every primitive of AR0-5 §2 is null or
a plain increment at `cores = 1`. A stage that cannot state this for its own
mechanism has not finished.

**AT-R10 — A dead kind is struck, never reused.** AU-R4's freeze. AT-3 C's
four already-dead kinds are struck by the first stage that opens
`ring_message.hpp`, with the `static_assert` count moving 34 → 30, rather
than being left for AU-S5 to "replace" — there is nothing to replace.

**AT-R11 — What stays special about system pages is two things, neither an
authority.** Their frames are pinned, and page 0's address with the fixed
catalog page numbers is bootstrap layout. When `MayWrite`'s last arm goes
(AT-S5), what stands in its place is named in the same commit: the free
map's own page latch for allocation, the catalog row's tuple `X` for
`sys.*` (AT-S3), and page 0's ceiling advancing by CAS from whichever task
crosses the threshold. **A stage that deletes the arm without naming its
replacement in the same change is not done** — this is the one place AT can
turn a refusal into corruption rather than into a wait.

**AT-R12 — The listener is the marked D19 and its blocker is AT's.**
`peer_listeners` becomes the arrangement rather than an option; the
TLS/SCRAM pairing refusal it carries today is an ownership residue (AT-3 F),
and AT either removes it or states in AT-S8's row why it is a networking
item and whose it becomes.

**AT-R13 — The stage order is AR0-5 §7's, and the reason is stated.** The
defence (S1) before the word (S2), because §8's quiet-wrong surface opens at
S2 and its only guard is S1. The word before the catalog borrow (S3),
because E13 makes `sys.tables` a relation like any other and the cache must
already be sound. The borrow before the route (S5), because a named-pk
`INSERT` must have something to wait on before it stops shipping. The route
before everything that dies with it (S6–S9).

**AT-R14 — No stage claims an overhead number it did not measure.** The
interleaved A/B is suspended by operator decision; a landed stage carries
"overhead not measured" as a stated fact, never an implied pass. AT-S13 is
the one stage that measures, and it says what it could not.

---

## AT-5 — Stages

Every code stage is gated on the operator's word in addition to the
dependencies below. Sub-stages are opened by the stage that needs them, as
AO-S6 did.

| stage | what it is | cells | size | gate |
|---|---|---|---|---|
| **AT-S0** | This document; `index.md`'s row. No code, no spec edit, no test | — | S | — |
| **AT-S1** | **The relation `IS` at resolve time** (AT-R1). The borrow is taken at the dispatcher's resolve, before `InitTableAccess`, and released at statement end; the nested-walk and remote-step sites of AT-3 E are covered by construction rather than by widening the position report | AR0-5 §8's **inverted case**: the word deliberately not bumped, the DDL still blocked. A nested join's inner relation blocks a `DROP TABLE`. A remote step blocks one. `cores = 1` unchanged | M | word |
| **AT-S2** | **The schema version word** (D21, AT-R2). `Expeditor` holds it; DDL bumps before releasing `X`; relation resolution compares one relaxed load and re-parses on mismatch. Retires `kCatalogInvalidate`, `InvalidateCatalog()`, `InvalidateFromPeer()` and crosscore.md §5's retryable table-not-found clause. Creates `docs/spec/catalog.md` or names the spec that takes AR0-5 §2.1's order (AT-3 I) | a peer resolves a post-DDL schema with no broadcast. A rollback and a commit both bump (AT-3 D's two paths). The retryable not-found is unreachable — call-site grep | M | S1 |
| **AT-S3** | **Catalog rows borrowable** (E13). Tuple `X` on the `sys.tables` row, so a named-pk `INSERT` **waits** instead of shipping; AR2 R5 struck. `rules.md` §3's declared-shared row for the catalog lands here | two named-pk inserts to one relation from two cores: one waits, neither refuses. `next_id`'s bump still logs and replays (keystone invariant) | M | S2 |
| **AT-S4** | **Shared allocators, per-core caches** (§2.2, D20, AT-R4). Trx ids and row ids to one `fetch_add` each with a cached block; `TrxIdLease`, `TrxIdRefill`, `MaybeRefillTrxIds`, `RowIdLeaseTable`, `RowIdRefill`, `MaybeRefillRowIds`, both service files and both spent `TxnConflict`s retire. AN-R13 kept and re-worded | the two spent refusals are unreachable — call-site grep. Ids stay unique across cores under load. An idle core's floor still burns (AN-R13's own cell, re-pointed) | M | S0 |
| **AT-S5** | **The route** (R12, AT-R5). A write runs where the session is; `ShipStatement`'s write arms retire with the FK probes, the index build and the assertion build; `MayWrite`'s last arm goes **with its replacement named in the same change** (AT-R11) | a cross-relation write mix from every core, byte-identical results to today. A peer writes a catalog page and the free map. The FK forward check meets a parent being written on another core and waits — AO-S5(b)'s cell, re-pointed at a local wait | **L** | S3, S4 |
| **AT-S6** | **The cross-owner transaction and 2PC retired** (AT-R6). `txn_2pc_service.hpp` and the participant protocol go; `cross-owner-txn.md` is rewritten or struck; AO-R8's net **11 s → 1 s**; AO-0 item 7 settled | an undecided prepare cannot be constructed. Recovery's undecided-prepare arm is unreachable — the mount scan's own cell states what replaced it. The net's new value is the only clock-ended exit | **L** | S5 |
| **AT-S7** | **Statistics and Cabin local.** `AccessBatch` fold-and-flush becomes a local write under lock (CC13), `kAccessStatsBatch` struck; the two `CabinStore`s (AT-3 G) become one instance store partitioned by `expr_id`; `cabin.md` §4b's scope rule struck; Waystone's peer-recording default lifted | `SHOW ACCESS` totals match the pre-change engine for the same workload. A peer's Cabin observation is banked and served. The contract suites for waystone and cabin stay byte-identical across configurations | M | S5 |
| **AT-S8** | **Checkpoint and the listener.** `RemoteCheckpointAnchor` retires; `Checkpoint()` becomes an instance task under an at-most-one-running flag; `kAnchorWrite` struck. D19 as marked and as AT-R12 scopes it: `peer_listeners` becomes the arrangement, and its TLS/SCRAM refusal is removed or handed on by name | two cores cannot run a checkpoint at once. A session accepted on any core runs to completion there. TLS with per-core listeners, or the row says whose it is | M | S5 |
| **AT-S9** | **Placement and `owner_core`** (D17, D18, E8, E9's withdrawal). AT's format event drops both columns and moves `rows.hpp:978`'s `static_assert`; `core_placement.hpp` and `PlacementPolicy` retire; NS10's verb becomes "declares the affinity of"; `core_count`'s pinning is decided with the rest of core specialization | a pre-AT volume mounts and its `owner_core` values are ignored — no refusal. `ns.table` still resolves; namespaces still exist as names | M | S5–S8 |
| **AT-S10** | **AU-S5**, one sub-stage per surviving group of AT-3 C's table: the replacement carries the traffic and the kind's handler is unreachable | per group: a call-site grep, and the group's own behaviour cell | **L** | S9 |
| **AT-S11** | **AU-S6**: count reaches 0; `RingTransport`, `RealRingTransport`, `SimRingTransport`, `ring_message.hpp`, phase 3, the N² preallocation, `AttachTransport` and the two transport tests removed | the suite; the golden log CRC unchanged; `SHOW META` loses its ring counters and `client-manual.md` says so | M | S10, **AT-0 item 4** |
| **AT-S12** | **The prose sweep.** CC11 and CC13 in `crosscore.md`; the eight `(M5)` comments; `core_runtime.hpp`'s asymmetries 1 and 3 and asymmetry 2's sentence; `Expeditor`'s "core 0 owns the superblock, the free map, the catalog pages and the listener"; `namespace.md`, `sched.md` §5, `rules.md` §3, `page.md` §6, `CLAUDE.md`'s rows | done-conditions written as greps, and **checked as greps** — AO-S8's two done-conditions were grep conditions that did not hold on the first pass | M | S11 |
| **AT-S13** | **The prices.** E7's cell, which AO-S7 handed on because C3's shape did not exist on an engine that serialised writes per core — it exists once AT-S5 lands. The `cores = 1` A/B. `ck-tester`, `build-release`, BTREE-only per the 2026-09-08 mark, `git describe` on every number | `bench/v3.0.0/`, one results file, with p0 and p25, a wait breakdown and a delta against this engine's own previous number | M | S5 |

**Order, and why it is not negotiable at three points** (AT-R13): S1 before
S2; S3 before S5; S5 before S6–S9. Everything else may be resequenced by
the operator without breaking an argument.

---

## AT-6 — Row status

| stage | state |
|---|---|
| AT-S0 | **landed 2026-09-09** on `m3-at` — this document and `index.md`'s row. No code, no spec edit, no test; the suite was not executed and no pass is claimed. Overhead not measured |
| AT-S1 … AT-S13 | not started; each gated on the operator's word |

---

## AT-0 — Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| 1 | **AR0-5's body.** It is still a DRAFT; `raft-marks-2026-09-05.md` §2 marks §6's D17–D22 and says so explicitly — *"this marks its §6 items, not the amendment"*. AT is built on the body | procedural | ratify it, or AT proceeds against it as a governing draft the way AO proceeded against AR2. AT-3 A is the one correction the body needs either way |
| 2 | **E7's default** now that AT-R5 settles the correctness half | measurement-gated | read it off AT-S13, not off AO-S7's C3, which measured an engine where the shape did not exist |
| 3 | **The borrow cap's unit.** `raft-marks-2026-09-08.md` §1 left it per *local* transaction "until AT's uniformity work asks the question again". With no participants there is one `Transaction` per transaction, so the cap silently narrows | constant, user-visible | state the narrowing in AT-S6's row and keep 65,536; it stops being one-cap-per-participant because there are no participants |
| 4 | **Does the remote-step protocol survive AT?** D18 keeps affinity as a weight-0 hint, so `kStep*` retains a consumer that never fires. **AU-S6 cannot reach count 0 while it lives** | design | keep it, converted by AT-S10 to shared state plus a kick (AR0-6-R1), and let AT-S11 delete the *transport* rather than the feature. If instead it is struck, say so before AT-S10 sizes its sub-stages |
| 5 | **AO-0's carried items 9, 22, 25 and 27** — the FK split's M2 half awaiting confirmation, the bound-assertion wait's own bound, `DROP TABLE` refused because readers keep arriving, and the intention-mode-on-an-interval rule that shipped with no ruling to point at | mixed | 9 confirms with D9(a) in the following letter; 22 and 25 move with it; **27 is AT's**, because AT-S1 widens what holds an `IS` and 27 is the rule that decides what such a borrow fences |
| 6 | **The following letter** for D7 with D1(b)'s gap locking, D9(a)'s `S` fence with `FkPendingDeleteTable`, E3, E5, E10 and AR1's AQ/AR | naming | one letter, opened after AT-S5 lands, since D9(a)'s fence and D7's gate both assume a write that no longer ships |
| 7 | **`in_doubt_ceiling_ms`'s manner of death** (AO-0 item 7): refused at startup naming its successor, or dropped silently with the 2PC it configured | user-visible | refuse at startup naming `kLockWaitFaultNetNs`; a config key that silently stops meaning anything is the failure the rule against second names exists to prevent |
| 8 | **Per-core listeners with TLS or SCRAM** (AT-3 F, AT-R12). The refusal's stated reason — credential state is core-local — is an ownership residue | networking | AT-S8 removes it if the credential state moves to `Expeditor` with everything else; if it is larger than that, AT-S8's row names it and it becomes its own item |

---

## AT-7 — Where the governing text and the tree disagree

Recorded at `df8cc5f`, in the form AO-7 established, because every one of
these would otherwise be inherited as an instruction.

1. **`ar0-5-amendment-uniformity.md` §4's M3 list names five things that
   left the tree at AW-S1b** (`af86026`) and one that has already been
   rewritten (AT-3 A). The extent-lease family is gone entirely, so AT
   retires **two** lease families, not three.
2. **"`MayWrite`'s remaining arms — the lease arm and the system-range
   arm"** is one arm and one call site (AT-3 B). The lease arm went with the
   arrangement it described.
3. **AR0-5 §2.1 requires a rule to be written in `docs/spec/catalog.md`,
   which does not exist** (AT-3 I). AT-S2 creates it or names its home; this
   order does neither, deliberately.
4. **The `(M5)` census is eight sites, not twelve** (AT-3 A). AR0-5-V had
   already corrected the count from five to twelve; it drifted the other way
   when AW-S1b deleted four of the files.
5. **`workorder-au-ring-retirement.md`'s AU-S5 row says "the remaining
   31"**; 30 are live and four are dead-but-unstruck (AT-3 C). AU-R4's
   freeze is why they are still enumerated, and AT-R10 strikes them.
6. **AR0 §8 step 6 and AR0-5's D22 define M3 differently** — "Cabin
   invariant removal (D7), Assertion, FK (D9)" against "Uniformity". Both
   were live until the operator's answer of 2026-09-09. AR0 §8 step 6 is not
   amended by this order; it is *split*, and AT-0 item 6 carries the other
   half.
7. **`raft-marks-2026-09-05.md`'s D19 assumes the listener is unbuilt**; the
   mechanism is in the tree behind `peer_listeners` and the real blocker is
   the credential state (AT-3 F). The mark is not wrong about what it wants,
   only about what it costs.
