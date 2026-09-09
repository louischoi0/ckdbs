# Work order AT — AR0 M3: Uniformity

Written 2026-09-09 on `m3-at` at `df8cc5f` (`v2.7.0-309-gdf8cc5f`; no v3
tag exists, AR0-M6). AT-3 below is a source read at that commit, and it is
not a copy of `ar0-5-amendment-uniformity.md` §4 — four of that list's M3
entries no longer describe the tree (AT-7), which is the same failure mode
AR0-5-V found in itself and the reason this order re-reads rather than
inherits. **AT-3 was then reviewed against the tree in turn, and three of
its own corrections were wrong** (AT-8); they are fixed above the line and
recorded below it, because a survey that corrects a draft is not thereby
exempt from being checked.

**Status: AT-S0 lands now; no code stage starts before the operator's
word.** M2 closed on 2026-09-09 (`workorder-ao-m2-lock-family.md` AO-8) and
its closing section states the absence this document fills: *"AR0 §8 step 6
is M3, renamed 'Uniformity' and lettered AT by `ar0-5-amendment-uniformity.md`
(D22). No work order exists for it."*

**Three operator decisions of 2026-09-09**, taken in plan mode and recorded
where they land: the **scope split** (AT-1, AT-0 item 6), **2PC inside AT**
(AT-R6), and the **relation `IS` at resolve time** (AT-R1). The reasoning
for each is in the section that carries it, not here.

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
retired with the ownership that justified it or given a lock. AT-3 is the
census. AT is not evaluated on throughput; AT-S13 measures what M2 handed
on and claims nothing else.

**What it is not.**

- **Not D7, not D9(a), not AR1.** AR0 §8 step 6 defined M3 as *"Cabin
  invariant removal (D7), Assertion, FK (D9)"*; AR0-5's D22 renamed M3
  "Uniformity". Both definitions were live until the operator's answer of
  2026-09-09 split them. D7 with D1(b)'s gap locking, D9(a)'s `S` fence
  with the `FkPendingDeleteTable` asymmetry AO-8 re-pointed, AR2's E3, E5
  and E10, and AR1's AQ/AR are **the following letter's cargo** — AT-0
  item 6 is the handoff.
- **Not the lock family.** M2 built it; AT is its first large consumer, and
  the one that makes the relation `IS` load-bearing for a *result* rather
  than for a wait.
- **Not an isolation-level change.** D1(b) stays where AR0-M1 put it.
- **Not M4** (AR0 §8 step 7).

**What lands now.** This document and `index.md`'s row. No code, no spec
edit, no test. Three things are deliberately not landed with it, each a
decision rather than an omission: `docs/spec/catalog.md`, which AR0-5 §2.1
cites and which does not exist (AT-3 I) — it is created by AT-S2, the stage
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
| D11 (`sys.range_affinity`) | AR0-M5: the R5 mover is retired; the relation **does not exist and stays absent** | nothing to build, and nothing to state — AT touches no part of it |
| D14 (format migration) | pending | AT has **its own format event** (D17, AT-R7) and it is not AM-S4's |
| D17 | **marked 2026-09-05**: columns dropped at M3 on AT's own event; a pre-M3 value ignored on read | **AT-S9**; the layout anchors AT-R7 names move there. **D17 and E8 disagree** and D17 wins (AT-7 item 6) |
| D18 | **marked**: affinity kept as a statistic and optimizer hint, D10 weight 0 until AS-E; placement NS10 deleted. The mark also obliges *"`physical-optimizer.md` keeps its consumer"* | **AT-S9** and **AT-S12**, which carries that spec; and AT-0 item 4, because a weight-0 hint leaves the remote-step protocol with a consumer that never fires |
| D19 | **marked**: every core listens (`SO_REUSEPORT`); a session lives where it was accepted; no handoff — **and, on a platform without it, core 0 accepts and hands off, a fallback whose handoff is "a ring consumer AU-S5 must list, not one it may strike"** | **AT-S8** for the listener, **AT-S10** for the fallback's kind (AT-R12). Smaller than the mark assumes on the socket and larger on the fallback (AT-3 F) |
| D20 | **marked as constants carried, not decided**: 4,096 for trx-id and row-id caches, one extent | **AT-S4**; any change from them needs a measurement |
| D21 | **marked**: schema word in `Expeditor`, memory only, bumped **before** the DDL's relation `X` releases | **AT-S2**; AR0-5 §8's read-order hazard is AT-S1's cell |
| D22 | **marked**: M3 is "Uniformity", letter **AT** | this document; `index.md`'s row |
| D25 (AR0-6) | count **34** since AU-S3 — and the `static_assert` freezing it **is a mark's obligation, not a fact of the tree**: `ring_message.hpp` carries two asserts, both on `MessageHeader`'s layout | AT-3 C: four of the 34 have no engine user. **AT-R10 writes the assert AU-R4 owes, at 30** |
| E7 (execution default) | measurement-gated; C1/C2 measured, **C3 answered that its shape does not exist** (AO-S7) | AT-R5 settles the *correctness* half — routing is not a correctness condition. The default is AT-0 item 2 and AT-S13's cell |
| E8 (NS10's verb) | user-visible: *"selects the core that owns" → "declares the affinity of"*, **take it** | **AT-S9** takes the verb. E8's companion clause — `owner_core` "keeps its bytes" — is dead, D17 having dropped the columns (AT-7 item 6) |
| E9 (`core_count` pinning) | **withdrawn by the operator 2026-09-08** — it argued from two dead premises | AT-S9 decides the count's fate with the rest of core specialization, which is what the withdrawal hands it |
| E13 (catalog rows borrowable) | OPEN, M3 | **AT-S3**; AR0-5 §3 already reads it as closed-yes and AT builds it |
| E1, E4, E11, E12 | ratified (AR2-A) | built in M2; AT changes none of them |
| E3, E5, E10 | M3 | not here — AT-0 item 6 |
| AO-R8 (the fault net) | 11 s while 2PC is in the tree, 1 s once M3 retires it, **the key re-scoped and renamed rather than deleted** | **AT-S6** is the stage that earns both (AT-0 item 7) |
| AO-R14 (guard in M2, route in M3) | the split | **AT-S5** is the route half |

---

## AT-3 — The survey at `df8cc5f`

### A. What AR0-5's retire list names that is already gone

`ar0-5-amendment-uniformity.md` §4 was written at `410377e` (2026-09-04);
`workorder-aw-m1-close.md` AW-S1b (`af86026`, 2026-09-07) then deleted eight
files. Four of §4's **M3** entries no longer describe the tree:

| named for retirement at M3 | state at `df8cc5f` |
|---|---|
| `LeasedIdSource`, `ExtentRefill`, `MaybeRefillLease`, the extent spent-lease `TxnConflict` | **gone.** `include/kds/storage/extent_lease.{hpp,cpp}` and `include/kds/server/extent_lease_service.{hpp,cpp}` are not in the tree |
| `MayWrite`'s "remaining arms — the lease arm and the system-range arm" | **one arm.** See B |
| the `(M5)` comment census, "twelve sites" | **eight** under `grep -rn '(M5)' include/ src/`, which is §3's own stated command, in five files: `include/kds/catalog/core_placement.hpp`, `include/kds/server/range_alloc.hpp`, `include/kds/server/core_runtime.hpp`, `src/server/expeditor.cpp`, `src/server/core_runtime.cpp`. **The command is narrower than the rule**: `tests/mount_recovery_test.cpp:221` carries a ninth, `src/server/core_runtime.cpp:1284` an unparenthesised tenth, and bare-`M5` ownership claims survive in nine more files. AT-S12's grep is written to the rule, not to §3's command |
| `core_runtime.hpp` asymmetries 1–3 | asymmetry **2 has already been rewritten** by AW-S1b — `include/kds/server/core_runtime.hpp:67-69` now reads "allocation reaches the one free map, under the structure latch … it came from a per-core extent lease until AW-S1b". AT strikes 1 and 3 and edits one sentence, not three asymmetries |

Three lease families were named; **two remain** (trx ids, row ids). This is
the single largest correction to AT's size and it is downward.

**§4's other stale entries are not M3's.** `MayFault`, `GrantFaultPages`,
`GrantWritePages`, `RelationGrantDemand` and `MaybeRequestRelationGrants`
are also gone, and they sit under §4's **M1 (AM)** and **M2 (AO-S5)**
headings — retired on schedule, by the milestone that owned them. They are
recorded here so a reader of §4 is not surprised, not as a correction to
AT's list.

### B. `MayWrite` is one arm and one engine call site

`src/storage/device_page_store.cpp:980` declares it; the whole predicate is
`src/storage/device_page_store.cpp:1015`:

```
if (page_id < first_evictable_page_id_) return CurrentCore() == 0;
return true;
```

The lease arm, the write-grant bitmap and the stamp claim went at AW-S1b;
AW-a collapsed the two boundary members into one and made the surviving arm
ask `CurrentCore()`. The function's own comment carries how it got here.

**What AT-S5 has to touch is wider than the one predicate**, and the count
matters because AR0-5 §3 re-points the AO-S5 cell at *"no `MayWrite` call
site exists"*: one engine call, the store's own `mark_dirty` gate at
`:738`; a **base virtual** at `include/kds/storage/page_store.hpp:403`
defaulting to `true`, overridden at
`include/kds/storage/device_page_store.hpp:445`; and **17 call sites in
tests**, among them `tests/device_page_store_test.cpp:589-603` and
`tests/core_runtime_test.cpp:525-533`, `:1185`, `:3209`, `:3367`, `:3462`,
`:3779`. `Session::MayWriteOn` (`include/kds/server/session.hpp:343`, called
`src/server/command_dispatcher.cpp:6846`) is a different predicate — the
session's core check — and is **not** retired by this row.

### C. Four ring kinds have no engine user

`include/kds/sched/ring_message.hpp`'s enum carries 34 kinds beside
`kUnset` (D25's count). **Four have no engine producer and no engine
handler**, their consumers having gone at AW-S1b: `kExtentLease` (17),
`kRelationFaultGrant` (21), `kRelationWriteGrant` (23),
`kRelationGrantRequest` (24). **`kExtentLease` is not unused, though**:
`tests/coro_test.cpp` uses it as its stand-in kind for a whole
`TrySend`/`WaitFor`/reply round trip, registering handlers at `:263` and
`:281` and sending at `:269` and `:296`. Striking it is therefore a test
edit as well as an enum edit, and AT-R10 says so; the other three are
enum-and-name-table only. `IsKnownRingMessageKind`
(`ring_message.hpp:262-298`) names all 34, so every strike touches it too.

**30 kinds have an engine user**, not the 31
`workorder-au-ring-retirement.md`'s AU-S5 row assumes. Grouped by what
retires them:

| group | kinds | retired by |
|---|---|---|
| no engine user | 4 (above) | **AT-S2**, the first stage that opens `ring_message.hpp` (AT-R10) |
| catalog broadcast | `kCatalogInvalidate` | AT-S2 |
| allocators | `kTrxIdLease`, `kRowIdLease` | AT-S4 |
| shipped statements | `kShippedStatementRequest/Reply`, `kShippedRowDesc` | AT-S5 (writes); reads per AT-0 item 4 |
| foreign-key probes | `kFkProbeRequest/Reply`, `kFkReverseProbeRequest/Reply` | AT-S5 |
| index and assertion builds | `kIndexBuild{Request,Reply,Done}`, `kAssertionBuild{Request,Reply,Done}` | AT-S5 |
| 2PC | `kTxn{Prepare,Decide,Resolve}{Request,Reply}` | AT-S6 |
| statistics | `kAccessStatsBatch` | AT-S7 |
| checkpoint | `kAnchorWrite` | AT-S8 |
| remote steps | `kStep{Open,Batch,Eof,Credit,Cancel,Error}` | **undecided** — AT-0 item 4 |

Two things follow. **AU-S6 may not be reachable inside AT**: the transport
cannot be deleted while one protocol still uses it, and whether the
remote-step protocol survives is a decision, not a consequence. And **D19's
fallback adds a kind rather than removing one** — the mark obliges AU-S5 to
*list* the handoff, not strike it — so AT-S10 acquires a consumer on the
platforms that need it (AT-R12).

### D. The catalog has two invalidation paths, and the version counter has no engine reader

- `Catalog::InvalidateFromPeer()` (`src/catalog/catalog.cpp:951`), the
  `kCatalogInvalidate` handler and the only invalidation a peer ever
  receives, clears every cached fact **without bumping the version
  counter**, deliberately — the counter is per-instance and means nothing
  across cores.
- `Catalog::InvalidateAfterCompensation()` (`src/catalog/catalog.cpp:847`),
  called from `EndDdlScopeById` (`src/server/command_dispatcher.cpp:9054`;
  the wrapper is at `:8994`) on **either ending** of any transaction that
  wrote catalog rows, calls `BumpVersion`.

The broadcast reaches peers through `CoreRuntime::InvalidateCatalog()`
(`src/server/core_runtime.cpp:922`, declared
`include/kds/server/core_runtime.hpp:437`), published at
`src/server/expeditor.cpp:1274`.

**`catalog_version()` has no engine reader.** The accessor
(`include/kds/catalog/catalog.hpp:1296`) is read only by tests, and
`include/kds/catalog/range_directory.hpp:36-47` states the rule that keeps
it that way: a resolved range set is a plan-time value, re-resolved after
any park, *"never re-validated with `catalog_version()`"* — because of the
peer non-bump. So AT-S2 inherits **no consumer to preserve**, which makes
the stage smaller than AR0-5 §2.1 implies; what it must not do is give the
new word the old one's shape. AT-R2 states that as a requirement rather
than as a repair.

### E. The `IS` gap, and where the resolve happens

`known-gaps.md`'s `## Locks` entry, verified at `cf3d0d0`: the relation `IS`
is declared only by a statement's outermost walk. `src/exec/step_vm.cpp:1960`
and `:2030` guard the position report on `index == 0`, so a nested walk — a
join's inner relation — declares nothing; and
`src/server/remote_step_service.cpp` mentions `PositionSink` nowhere, at any
of its three execution sites (`:458`, `:886`, `:1080`).

The resolve AT-R1 must precede is `Catalog::InitTableAccess(Oid)`
(`include/kds/catalog/catalog.hpp:684`), whose result is the
reference-stable `const TableAccess*` a statement holds for its own
duration (`include/kds/catalog/catalog_cache.hpp:98-102`).

### F. D19's socket is built; its fallback is not

`TcpServer::Listen(port, reuse_port)` sets `SO_REUSEPORT`
(`src/server/tcp_server.cpp:36`, `:44-51`), a peer's listener already opens
with it (`src/server/core_runtime.cpp:1298`), and the arrangement is a
config flag, `peer_listeners`, defaulting off
(`include/kds/server/expeditor.hpp:173`; the listener opened at
`src/server/expeditor.cpp:1534`).

**Two things are not built.** `CheckPeerListenerConfig`
(`src/server/expeditor.cpp:127-145`) refuses `peer_listeners = on` combined
with TLS or SCRAM, because *"the credential store and TLS context live on
core 0's stack"* (`:129-136`) — itself an ownership residue. And the mark's
**fallback** — core 0 accepts and hands off, on a platform without
`SO_REUSEPORT` — has no handoff path at all; it is a kind AT-S10 must add.

### G. Two Cabin stores, and AR1 does not choose between the shapes

`std::optional<stats::CabinStore>` is a member of **both**
`include/kds/server/core_runtime.hpp:629` and
`include/kds/server/expeditor.hpp:779`.

**AR1 §11 leaves the topology to M3 and says so**
(`ar1-architecture-revision-cabin-function.md:359-368`): the witness must
reach *"one store, **or** a store partitioned so that every write to a given
key reaches the same partition. `expr_id` is the natural partition prefix
and AR1 fixes nothing further; the store's topology is M3's."* So AT-S7
chooses between two shapes rather than implementing one — AT-0 item 9.

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
**before** the DDL's relation `X` is released. The requirement is stated
against today's counter, which has the opposite property: `InvalidateFromPeer`
bumps nothing, which is why `range_directory.hpp:36-47` forbids validating
against it (AT-3 D). AT-S2 inherits no consumer, so the rule is a
constraint on what it builds, not a migration.

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
precondition.

**AT-R7 — AT has its own format event, and it moves four layout anchors.**
D17 as marked: `sys.tables.owner_core` (`include/kds/catalog/rows.hpp:104`)
and `sys.ranges.owner_core` (`:955`) are dropped, and a pre-AT volume's
value is **ignored on read** — no mount refusal. It is not AM-S4's event,
which is the page-header stamp. **The mark names one anchor and there are
more**: `rows.hpp:978`'s `static_assert` on `kOwnerCoreOffset` and `:980`'s
`kOnDiskSize == 32` (which becomes 28), `kEntryPageOffset` (`:967-969`), and
on the `sys.tables` side `kOwnerCoreOffset` (`:145`) with the
`kKeyOrderOffset` derived from it (`:146`). **`SysTableRow` carries no offset
`static_assert` at all**, so the check that would catch a mistake is absent
on that half; AT-S9 adds one rather than working without it.

**AT-R8 — Every retired refusal is struck from AO-3's census with the
mechanism that replaces it, not deleted.** AR0-5 §5 names this method as one
of the things M3 does not change, and AT is where most of the census's
premises expire.

**AT-R9 — G2 holds at every stage.** Every primitive of AR0-5 §2 is null or
a plain increment at `cores = 1`. A stage that cannot state this for its own
mechanism has not finished.

**AT-R10 — A dead kind is struck, never reused, and the freeze is written
at the number the strike leaves.** AU-R4's rule. AT-3 C's four kinds are
struck by AT-S2, the first stage that opens `ring_message.hpp`, together
with their `IsKnownRingMessageKind` entries and — for `kExtentLease` alone —
`tests/coro_test.cpp:263`, `:269`, `:281` and `:296`, which must move to a
surviving kind first. **The `static_assert` D25 obliges does not exist yet**
(`ring_message.hpp` has two, both on `MessageHeader`'s layout), so AT-S2
writes it at **30** rather than renumbering a 34 that was never written
down. AU-S0 owed it; AT pays it where the number changes.

**AT-R11 — What stays special about system pages is two things, neither an
authority.** Their frames are pinned, and page 0's address with the fixed
catalog page numbers is bootstrap layout. When `MayWrite`'s last arm goes
(AT-S5), what stands in its place is named in the same commit: the free
map's own page latch for allocation, the catalog row's tuple `X` for
`sys.*` (AT-S3), and page 0's ceiling advancing by CAS from whichever task
crosses the threshold. **A stage that deletes the arm without naming its
replacement in the same change is not done** — this is the one place AT can
turn a refusal into corruption rather than into a wait.

**AT-R12 — D19 is taken whole, fallback included.** `peer_listeners`
becomes the arrangement rather than an option (AT-S8), and the TLS/SCRAM
pairing refusal is removed with the core-local credential store it names or
handed on by name in AT-S8's row. **The mark's second half is an obligation
on the ring, not on the listener**: on a platform without `SO_REUSEPORT`,
core 0 accepts and hands off, and that handoff is *"a ring consumer AU-S5
must list, not one it may strike"* — so AT-S10 builds it and AT-S11's count
must reach 0 with it counted, not by ignoring it.

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
| **AT-S1** | **The relation `IS` at resolve time** (AT-R1). The borrow is taken at the bind, before the schema is read, and released at statement end; the nested-walk and remote-step sites of AT-3 E are covered by construction rather than by widening the position report | AR0-5 §8's **inverted case**: the word deliberately not bumped, the DDL still blocked. A nested join's inner relation blocks a `DROP TABLE`. A remote step blocks one. `cores = 1` unchanged | M | word |
| **AT-S2** | **The schema version word** (D21, AT-R2). `Expeditor` holds it; DDL bumps before releasing `X`; relation resolution compares one acquire load at its statement's boundary and re-parses on mismatch. Retires `kCatalogInvalidate`, `InvalidateCatalog()`, `InvalidateFromPeer()` and the retryable table-not-found clause — which lives at `include/kds/server/core_runtime.hpp:64-67`, **not** in `crosscore.md` §5 (AT-7 item 7). Strikes AT-3 C's four kinds and writes D25's `static_assert` at 30 (AT-R10). Creates `docs/spec/catalog.md` or names the spec that takes AR0-5 §2.1's order | a peer resolves a post-DDL schema with no broadcast. A rollback and a commit both bump (AT-3 D's two paths). The retryable not-found is unreachable — call-site grep. `tests/coro_test.cpp` passes on its new kind | M | S1 |
| **AT-S3** | **Catalog rows borrowable** (E13). Tuple `X` on the `sys.tables` row, so a named-pk `INSERT` **waits** instead of shipping; AR2 R5 struck. `rules.md` §3's declared-shared row for the catalog lands here | two named-pk inserts to one relation from two cores: one waits, neither refuses. `next_id`'s bump still logs and replays (keystone invariant) | M | S2 |
| **AT-S4** | **Shared allocators, per-core caches** (§2.2, D20, AT-R4). Trx ids and row ids to one `fetch_add` each with a cached block; `TrxIdLease`, `TrxIdRefill`, `MaybeRefillTrxIds`, `RowIdLeaseTable`, `RowIdRefill`, `MaybeRefillRowIds`, both service files and both spent `TxnConflict`s retire. AN-R13 kept and re-worded | the two spent refusals are unreachable — call-site grep. Ids stay unique across cores under load. An idle core's floor still burns (AN-R13's own cell, re-pointed) | M | S0 |
| **AT-S5** | **The route** (R12, AT-R5). A write runs where the session is; `ShipStatement`'s write arms retire with the FK probes, the index build and the assertion build; `MayWrite`'s last arm goes **with its replacement named in the same change** (AT-R11), taking the base virtual, the override and AT-3 B's 17 test sites with it | a cross-relation write mix from every core, byte-identical results to today. A peer writes a catalog page and the free map. The FK forward check meets a parent being written on another core and waits — AO-S5(b)'s cell, re-pointed at a local wait | **L** | S3, S4 |
| **AT-S6** | **The cross-owner transaction and 2PC retired** (AT-R6). `txn_2pc_service.hpp` and the participant protocol go; `cross-owner-txn.md` is rewritten or struck; AO-R8's net **11 s → 1 s**, and `in_doubt_ceiling_ms` is **re-scoped and renamed** to the net rather than deleted, the old spelling refused at `expeditor.cpp`'s known-key check naming its successor (AT-0 item 7) | an undecided prepare cannot be constructed. Recovery's undecided-prepare arm is unreachable — the mount scan's own cell states what replaced it. The old key name is refused at startup with its successor named | **L** | S5 |
| **AT-S7** | **Statistics and Cabin local.** `AccessBatch` fold-and-flush becomes a local write under lock (CC13), `kAccessStatsBatch` struck; the two `CabinStore`s (AT-3 G) become one store under whichever of AR1 §11's two shapes AT-0 item 9 names; `cabin.md` §4b's scope rule struck; Waystone's peer-recording default lifted | `SHOW ACCESS` totals match the pre-change engine for the same workload. A peer's Cabin observation is banked and served. The contract suites for waystone and cabin stay byte-identical across configurations | M | S5 |
| **AT-S8** | **Checkpoint and the listener.** `RemoteCheckpointAnchor` retires; `Checkpoint()` becomes an instance task under an at-most-one-running flag; `kAnchorWrite` struck. D19's listener half as AT-R12 scopes it: `peer_listeners` becomes the arrangement, and its TLS/SCRAM refusal is removed with the core-local credential store or handed on by name | two cores cannot run a checkpoint at once. A session accepted on any core runs to completion there. TLS with per-core listeners, or the row says whose it is | M | S5 |
| **AT-S9** | **Placement and `owner_core`** (D17, D18, E8's verb, E9's withdrawal). AT's format event drops both columns and moves AT-R7's four anchors, adding the `SysTableRow` assert that is missing; `core_placement.hpp` and `PlacementPolicy` retire; NS10's verb becomes "declares the affinity of"; `core_count`'s pinning is decided with the rest of core specialization | a pre-AT volume mounts and its `owner_core` values are ignored — no refusal. `ns.table` still resolves; namespaces still exist as names | M | S5–S8 |
| **AT-S10** | **AU-S5**, one sub-stage per surviving group of AT-3 C's table, **plus D19's fallback handoff**, which the mark obliges this stage to list rather than strike (AT-R12) | per group: a call-site grep, and the group's own behaviour cell | **L** | S9 |
| **AT-S11** | **AU-S6**: count reaches 0; `RingTransport`, `RealRingTransport`, `SimRingTransport`, `ring_message.hpp`, phase 3, the N² preallocation, `AttachTransport` and the two transport tests removed; **G1's sentence** (`ar0-architecture-revision.md:45`, as AR0-6 revised it) rewritten with them | the suite; the golden log CRC unchanged; `SHOW META` loses its ring counters and `client-manual.md` says so | M | S10, **AT-0 item 4** |
| **AT-S12** | **The prose sweep.** CC11 and CC13 in `crosscore.md`; **`physical-optimizer.md`, which D18's mark names**; the `(M5)` comments **to the rule and not to §3's command** — `tests/` and the bare-`M5` claims of AT-3 A included; `core_runtime.hpp`'s asymmetries 1 and 3 and asymmetry 2's sentence; `Expeditor`'s "core 0 owns the superblock, the free map, the catalog pages and the listener"; `namespace.md`, `sched.md` §5, `rules.md` §3, `page.md` §6, `CLAUDE.md`'s rows | done-conditions written as greps, and **checked as greps** — AO-S8's two done-conditions were grep conditions that did not hold on the first pass, and a grep scoped to `include/ src/` is how this one would repeat that | M | S11 |
| **AT-S13** | **The prices.** E7's cell, which AO-S7 handed on because C3's shape did not exist on an engine that serialised writes per core — it exists once AT-S5 lands. The `cores = 1` A/B. `ck-tester`, `build-release`, BTREE-only per the 2026-09-08 mark, `git describe` on every number | `bench/v3.0.0/`, one results file, with p0 and p25, a wait breakdown and a delta against this engine's own previous number | M | S5 |

**Order, and why it is not negotiable at three points** (AT-R13): S1 before
S2; S3 before S5; S5 before S6–S9. Everything else may be resequenced by
the operator without breaking an argument.

---

## AT-6 — Row status

| stage | state |
|---|---|
| AT-S0 | **landed 2026-09-09** on `m3-at` — this document and `index.md`'s row. No code, no spec edit, no test; the suite was not executed and no pass is claimed. Overhead not measured. **Reviewed after landing** and corrected in the same branch: AT-8 |
| AT-S1 | **built 2026-09-09** on `m3-at`. The compiler declares every relation it binds into the statement's read borrow, between the name and the schema (`exec::Compile`/`CompileWhere` take a `PositionSink`; `read_borrow.hpp` is the borrow's home now); the three write verbs declare at resolve; a remote-step producer takes its own `IS` in its frame, because the coordinator's borrow dies at its `pending` return before the remote walk runs. Cells: a join declares both relations, a subquery declares at its bind, `INSERT`/`UPDATE` declare at resolve, a parked producer refuses a DDL's `X` and grants it after EOF. Three mutants killed (the compiler's line, the dispatcher's write-path line, the producer's). **Its review corrected the stage's own claim**: the bind's ask is a non-blocking `TryAcquire`, so a refused ask leaves the DDL-wins direction to DT1, catalog MVCC and catalog-only DDL - AT-7 item 10, AT-0 item 10 - and found two gaps the first draft had, `INSERT` and the remote step, both closed in the same change; `drop-table.md` DT7 now states the widened wait surface. **A second review found the fixes' own defects**: two minters in one holder-id space on one core (the dispatcher's and the step server's counters both fed `ReadHolderId(core, seq)` - latent only because no dispatcher borrow spans a suspension; bit 62 now names the minter), the consuming stage dropping its `IS` for a reactor turn between the open and its first resume (the borrow is handed down now, one holder id per stage, a cell pins it), and the owning spec - `txn.md` §5 - still stating the pre-AT-S1 contract with one bullet that denied what this stage built; rewritten, with `CLAUDE.md`'s row. **A third review verified the fixes** and found what lagged them: the borrow's owning comment on the dispatcher's sequence still argued the pre-D1 way; `CoreRuntime` declared its owned lock table below the scheduler, so a parked frame's release at teardown would have reached a freed table - unreachable today, fixed by declaration order; `RunConsumer` re-declared a step the open had already declared; and a consuming stage's inner walk, `index == 0` on its own core, re-took a slice per input row - the M2 rule that a nested walk declares no slice is restored with a `parent_` gate. **Stated omission**: the foreign-key check family - an `INSERT`'s or `UPDATE`'s parent, a `DELETE`'s children, the probe server's relation - declares nothing, because those checks are not steps; the following letter's, with D9(a). **Suite 3385/3385 in 281.90 s** on the tree committed, the baseline's 3379 plus six. Overhead not measured |
| AT-S2a | **built 2026-09-09** on `m3-at`: the schema version word. `Expeditor` owns one atomic; every core's catalog reads it; `BumpVersion()` bumps it at the catalog write, before the DDL's `X` is released (D21). **Its review found the first draft revalidating at every cached read, which frees a live `TableAccess*` inside a peer's statement on any core-0 DDL** - a join's second bind, an FK loop, a `SHOW` - where the broadcast it replaced dropped only between tasks. Fixed as AT-S2's own row states: revalidate once, at the task boundary, never inside a read - `DispatchAndStage`'s head, five named handlers (`OnStepOpen`, the two FK probe requests, the two build requests), the two `system` ticks (the refill, the Cabin optimizer) and the refill's completion; and `BumpWord` adopts a bump only when the cache was current, so a bump from another core is never swallowed - latent until AT-S3 lets a peer write, as is every drop the change makes: **`Revalidate()` is a no-op on core 0 today**, the single writer, whose `cache_built_at_` always tracks. **A second review found the same class once more** - `kwp_load_server.cpp` held a `Schema&` across the `Dispatch("BEGIN")` that now drops - fixed by copying out before the boundary, and `Dispatch`'s declaration now carries the contract. Retired: the hook, the broadcast, `InvalidateCatalog()` and its three remaining callers, both build services' `on_committed` seams, eighteen fixture calls. **A third review found the fixes correct** and what lagged them - five comments and declarations stating the opposite of the code beside them, a cell that would have crashed where it should fail, test-local word atomics outliving their pointers, the Cabin tick asking above its own off-gate - each fixed; and it named the premise the memo-only drop rests on, one frame table for every core, which `Revalidate()`'s declaration now states. `InvalidateFromPeer` → `DropCache`, kept for the two post-redo drops alone. **Re-scoped**: the four dead kinds, `kCatalogInvalidate`'s enumerator and D25's `static_assert` are AT-S2b's; `docs/spec/catalog.md` and the spec mentions are AT-S2c's. Nine cells; three mutants - `Revalidate` a no-op kills the reader and peer cells, `DispatchAndStage`'s boundary removed kills the rename cell (whose first draft used `CREATE TABLE`, which no memo ever holds, and survived), and `BumpWord`'s guard made unconditional kills the behind-cache cell. **Suite 3393/3393 in 292.45 s** on the tree committed, AT-S1's 3385 plus eight. Overhead not measured |
| AT-S2b | **built 2026-09-09** on `m3-at`: the strikes. `kExtentLease` (17), `kRelationFaultGrant` (21), `kRelationWriteGrant` (23), `kRelationGrantRequest` (24) - dead since AW-S1b - and `kCatalogInvalidate` (19), whose last producer and handler went at AT-S2a, are struck from the enum, from `IsKnownRingMessageKind` and from the name table, each value left as the record that it was spent (AU-R4, the form AU-S3 set for 20). **D25's freeze is written**: `kRingMessageKinds`, the one list of every kind this build sends or handles, and a `static_assert` on its size at **29** - not the 30 AT-R10 named, because AT-R10 counted the four and this stage strikes the fifth with them. `IsKnownRingMessageKind` searches that list rather than restating it. `tests/coro_test.cpp`'s stand-in kind is `kIndexBuildRequest`. **Suite 3393/3393 in 286.02 s**, AT-S2a's count. **Review owed**: the `critics-developer` pass was terminated by an API rate limit before it reported, and is re-run when the limit resets; landed on the operator's word to commit and push, with the gap named. Overhead not measured |
| AT-S2c, AT-S3 … AT-S13 | not started; each gated on the operator's word |

---

## AT-0 — Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| 1 | **AR0-5's body.** It is still a DRAFT; `raft-marks-2026-09-05.md` §2 marks §6's D17–D22 and says so explicitly — *"this marks its §6 items, not the amendment"*. AT is built on the body | procedural | ratify it, or AT proceeds against it as a governing draft the way AO proceeded against AR2. AT-3 A and AT-7 are the corrections the body needs either way |
| 2 | **E7's default** now that AT-R5 settles the correctness half | measurement-gated | read it off AT-S13, not off AO-S7's C3, which measured an engine where the shape did not exist |
| 3 | **The borrow cap's unit.** `raft-marks-2026-09-08.md` §1 left it per *local* transaction "until AT's uniformity work asks the question again". With no participants there is one `Transaction` per transaction, so the cap silently narrows | constant, user-visible | state the narrowing in AT-S6's row and keep 65,536; it stops being one-cap-per-participant because there are no participants |
| 4 | **Does the remote-step protocol survive AT?** D18 keeps affinity as a weight-0 hint, so `kStep*` retains a consumer that never fires. **AU-S6 cannot reach count 0 while it lives** | design | keep it, converted by AT-S10 to shared state plus a kick (AR0-6-R1), and let AT-S11 delete the *transport* rather than the feature. If instead it is struck, say so before AT-S10 sizes its sub-stages |
| 5 | **AO-0's carried items 9, 22, 25 and 27** — the FK split's M2 half awaiting confirmation, the bound-assertion wait's own bound, `DROP TABLE` refused because readers keep arriving, and the intention-mode-on-an-interval rule that shipped with no ruling to point at | mixed | 9 confirms with D9(a) in the following letter; 22 and 25 move with it; **27 is AT's**, because AT-S1 widens what holds an `IS` and 27 is the rule that decides what such a borrow fences |
| 6 | **The following letter** for D7 with D1(b)'s gap locking, D9(a)'s `S` fence with `FkPendingDeleteTable`, E3, E5, E10 and AR1's AQ/AR | naming | one letter, opened after AT-S5 lands, since D9(a)'s fence and D7's gate both assume a write that no longer ships |
| 7 | **`in_doubt_ceiling_ms`** (AO-0 item 7): refused at startup naming its successor, or kept inert until M3 re-scopes it to the fault net | user-visible | **AO-R8's own plan**: re-scope and rename the key to the net, and refuse the old spelling at the known-key check naming its successor. Deleting it would leave the net with no config key at all, against `CLAUDE.md`'s rule to re-scope rather than re-name |
| 8 | **Per-core listeners with TLS or SCRAM** (AT-3 F, AT-R12). The refusal's stated reason — the credential store and TLS context live on core 0's stack — is an ownership residue | networking | AT-S8 removes it if the credential state moves to `Expeditor` with everything else; if it is larger than that, AT-S8's row names it and it becomes its own item |
| 9 | **The Cabin store's topology.** AR1 §11 offers two shapes — one store, or one partitioned so every write to a key reaches the same partition — and fixes neither, saying *"the store's topology is M3's"* (AT-3 G) | design | **one store, partitioned by `expr_id`**: the partition is what keeps a peer's observation off a mutex the owner holds, and `expr_id` is AR1's own named prefix. Raised as an item rather than taken as a ruling because AT-1 sends AR1's AQ/AR to a following letter, and this is the one AR1 decision AT cannot avoid |
| 10 | **Should a bind's `IS` wait for an in-flight DDL?** AT-S1's ask is non-blocking (AT-7 item 10). A waiting ask would close the DDL-wins direction with the lock rather than with DT1 + MVCC + catalog-only DDL, at the price of putting readers into the wait-for graph - AO-S6e-b kept them out so a DDL waiting for a reader can never cycle, and a reader holding `IS` on A while waiting for B's `X` against a DDL holding B and waiting for A's readers is a cycle no detector sees, ended only by the 11 s net | quiet-wrong class, design | **keep it non-blocking.** The three facts hold today and every DDL the engine has is catalog-only; the day a DDL moves data is the day this item reopens, and `read_borrow.hpp` names that condition. Taken as CLA's proposal on the operator's word of 2026-09-09 that CLA's proposals stand |

---

## AT-7 — Where the governing text and the tree disagree

Recorded at `df8cc5f`, in the form AO-7 established, because every one of
these would otherwise be inherited as an instruction. Each states the
consequence; the evidence is in AT-3.

1. **AR0-5 §4's M3 list is stale in four entries** (AT-3 A). The
   extent-lease family is gone, so AT retires **two** lease families, not
   three. §4's `MayFault` and grant entries are *not* in this class — they
   are its M1 and M2 lists, retired on schedule.
2. **"`MayWrite`'s remaining arms" is one arm** (AT-3 B) — and one *engine*
   call site beside a base virtual, an override and 17 test sites, which is
   what AR0-5 §3's re-pointed AO-S5 cell has to cover.
3. **AR0-5 §2.1 requires a rule in `docs/spec/catalog.md`, which does not
   exist** (AT-3 I). AT-S2 creates it or names its home.
4. **The `(M5)` census is eight sites under §3's own command and more under
   §3's own rule** (AT-3 A). AR0-5-V had corrected the count from five to
   twelve; `af86026` then deleted three of those sites with two files and
   edited a fourth away in a file that survives, which is 12 → 8.
5. **`workorder-au-ring-retirement.md`'s AU-S5 row says "the remaining
   31"**; 30 have an engine user and four are dead-but-unstruck, one of them
   with a live *test* producer (AT-3 C).
6. **AR2's E8 and AR0-5's D17 disagree, and D17 wins.** E8 says
   `owner_core` "keeps its bytes and changes its meaning to affinity"
   (`ar2-architecture-revision-borrow-model.md:525-527`, `:627`); D17's mark
   of 2026-09-05 drops the columns. E9's withdrawal text already concedes it
   in passing. AT-S9 takes E8's **verb** and D17's **bytes**.
7. **`crosscore.md` §5 carries no "retryable table-not-found clause".** The
   clause AR0-5 §2.1 retires is `include/kds/server/core_runtime.hpp:64-67`'s;
   §5's own closing bullet is a different rule — a stale oid on a *remote
   step* surfaces as `STEP_ERROR` — whose fate is coupled to AT-0 item 4.
   AT-S2 is pointed at the header, not at the spec.
8. **AR0 §8 step 6 and AR0-5's D22 define M3 differently** — "Cabin
   invariant removal (D7), Assertion, FK (D9)" against "Uniformity". Both
   were live until the operator's answer of 2026-09-09. AR0 §8 step 6 is not
   amended by this order; it is *split*, and AT-0 item 6 carries the other
   half.
9. **`raft-marks-2026-09-05.md`'s D19 assumes the listener is unbuilt**; the
   socket is in the tree behind `peer_listeners`, and what is missing is the
   credential store's home and the fallback handoff the mark obliges AU-S5
   to list (AT-3 F, AT-R12).
10. **AR0-5 §8 overstates what the lock alone gives.** *"DDL's `X` cannot be
    granted while it is held, so a stale parse cannot be executed"* is true
    of a reader that holds the `IS` - the reader-wins direction, which AT-S1
    now extends to every bound relation. A reader arriving while the DDL
    holds `X` is **refused and reads on**, because a read borrow never
    refuses a read and never waits (AO-S6e-b, ratified). What makes that
    reader's answer right is DT1 (pages stay, oids are never reissued),
    catalog MVCC (the DDL's rows are invisible to its view) and the absence
    of data-moving DDL (`alter.md`) - three facts about the engine, not the
    lock. AT-R3's "the lock is the argument" is therefore one direction's
    argument, and `read_borrow.hpp` says so. Found by AT-S1's review; the
    alternative - a bind that waits for the DDL - is AT-0 item 10.

---

## AT-8 — What the review of AT-S0 found

A `critics-developer` pass over this document, run against the tree at
`4e6ab7c` on `m3-at`, checked every `path:line` and every claim in AT-3.
**Eleven of the fourteen it was asked to verify were exact to the line, the
stage list covers AR0-5 §4's M3 list completely, and AT-3 C's grouping table
is exhaustive** — 4 + 1 + 2 + 3 + 4 + 6 + 6 + 1 + 1 + 6 = 34, every live
kind in exactly one group. What it found is recorded here rather than
silently fixed, because AT-3's whole claim is that it re-read rather than
inherited, and a survey making that claim has to show its own corrections.

**Three of the seven corrections AT-3 advertised were themselves wrong.**

1. **`kExtentLease` is not unused.** `tests/coro_test.cpp` produces and
   handles it at four lines. The first draft told AT-S2 to strike four kinds
   "on sight", which would not have compiled. AT-3 C and AT-R10 now carry
   the test edit.
2. **The `static_assert` freezing the kind count does not exist.** The draft
   read D25's *obligation* as a fact of the tree and told a stage to move a
   number "34 → 30" that is written nowhere. AT-R10 now writes it at 30.
3. **AT-7's cause for the `(M5)` drift was false** — 12 → 8 is three sites
   deleted with two files and a fourth edited away in a file that survives,
   not "four files deleted". This is the failure AR0-5-V recorded about
   itself (*"gave a false cause for two rows"*) repeated in form, which is
   why it is written out rather than quietly corrected.

**Six further defects, each fixed above**: the interval between `410377e`
and `af86026` is three days, not one; two of §4's stale entries were counted
against the M3 list and belong to M1 and M2, which is why the headline is now
four rather than six; `MayWrite`'s call-site count omitted the base virtual
and 17 test sites; `catalog_version()` has no engine reader at all, so AT-S2
inherits nothing to preserve and AT-R2 is a constraint rather than a repair;
AT-0 item 7 offered a straw alternative and contradicted AO-R8, which rules
the key **re-scoped and renamed**; and AT-R7 named one layout anchor where
the format event moves four and adds one that is missing.

**Four omissions**: D19's fallback handoff, which the mark obliges AU-S5 to
*list* and AT had nowhere (AT-R12, AT-S10); `physical-optimizer.md`, the one
document D18's mark names, absent from AT-S12; G1's sentence, dropped from
AT-S11's copy of AU-S6's list; and AR1 §11's topology choice, which AT-S7
was making silently and is now AT-0 item 9.

**Two inexact quotations** are corrected: the header's elision of AO-8 now
carries the clause it dropped, and AT-3 F quotes
`src/server/expeditor.cpp:129-136` as it reads.

**The trims it proposed were taken**, and they are why this document is
shorter than the draft: `index.md`'s row reproduced five of this order's
sections and now points at them; the scope split was stated four times and
is now stated twice, in AT-1 and AT-0 item 6; the header's decision block
re-argued three rulings and is now a provenance line; and AT-7 restated
AT-3 rather than stating its consequence.
