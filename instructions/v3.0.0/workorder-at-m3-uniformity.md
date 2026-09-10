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
| D25 (AR0-6) | count **34** since AU-S3 — and the `static_assert` freezing it **was a mark's obligation, not a fact of the tree** until AT-S2b: `ring_message.hpp` carried two asserts, both on `MessageHeader`'s layout | AT-3 C: four of the 34 have no engine user. **AT-S2b wrote the assert AU-R4 owes, at 29** — the four and `kCatalogInvalidate` — and its review gave it its form: the count is derived from the default-less switch, so a new enumerator warns and a moved count fails |
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
writes it at the number the strike leaves rather than renumbering a 34 that
was never written down — **29**, since AT-S2b struck `kCatalogInvalidate`
with the four (this ruling said 30 counting the four alone). AU-S0 owed it;
AT pays it where the number changes. **Its form is the review's**: the count
is computed at compile time from the default-less switch that is the census,
so a hand-kept list cannot drift from the enum.

**AT-R11 — What stays special about system pages is two things, neither an
authority.** Their frames are pinned, and page 0's address with the fixed
catalog page numbers is bootstrap layout. When `MayWrite`'s last arm goes
(AT-S5), what stands in its place is named in the same commit: for the
catalog pages, **the page latch across cores and, within one, the rule
that no task parks under a page span** - the latch is re-entrant for the
owning core, so it serialises cores and not tasks, and every catalog
read-modify-write is atomic only because nothing suspends inside it
(`page.md` §6, `AdmitExplicitRowId`'s `before_mark` contract; corrected at
AT-S3's review, which found this ruling naming a tuple `X` on the
`sys.tables` row that AT-S3 showed has no contender); for allocation the
free map's own `map_latch_`; and for page 0 its ceiling advancing by CAS
from whichever task crosses the threshold (AT-S4). **A stage that deletes the arm without naming its
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
already be sound. ~~The borrow before the route (S5), because a named-pk
`INSERT` must have something to wait on before it stops shipping.~~ **Struck
at AT-S3**: the admission waits on the page latch it already takes, not on a
row borrow, so S5 needs S2 only - and S4 needs S5, since its persist half is a
page-0 write the arm S5 retires still forbids (AT-7 items 11, 12). The route before
everything that dies with it (S6–S9).

**AT-R15 — AT-S5's seven defects are five sub-stages, and each one's shape
is decided rather than left to its stage.** *Operator's rulings,
2026-09-09, taken in plan mode.* The review's D0-D7 are one finding stated
seven times - **the route removed the routing that made four per-core
structures sound, and unified none of them** - so they are not seven
patches. D0, D2 and D7 are the mechanical half and land together as
**AT-S5b**; the other four each replace a structure and are ordered by
exposure rather than by the letter the review gave them:

| # | shape ruled | stage |
|---|---|---|
| D3 | the descent re-validates after its re-fetch and restarts from the root on a mismatch | **AT-S5c**, first, because any two sessions on two cores inserting into one btree relation reach it and nothing else is needed |
| D1 | **one `AssertionEnforcer` for the instance**, `LockTable`'s idiom - `Expeditor` owns, `Config` hands, a fixture builds its own - with the group delta taken *with* the admission under a latch that never spans page work | **AT-S5d** |
| D6 | **`CREATE INDEX`/`DROP INDEX` take the relation `X`**; `PendingIndexBuilds`, `AwaitIndexWindow` and `IndexBuildPending` retire. AO-S6e-a declined this same `X` because the window's close, the cache drop and the next admitted write were one ordered event a lock could not reproduce - **AT-S2a's word is that ordering** (CT1, CT2), so the objection has expired | **AT-S5e** |
| D5 | **the foreign-key checks go local**: no parent is deferred by `owner_core`, an in-flight parent is AO-S3's wait across cores through the instance lock table, both tables and the probe protocol retire. This is AT-S5's own uncell **built** | **AT-S5f** |
| D4 | two halves: the Cabin arm may *find* a child and may not *clear* one while the store is per core (rides S5b or S5f); the store becomes the instance's at **AT-S7**, which restores the fast path rather than removing it | AT-S5f, AT-S7 |

Each still opens on the operator's word; the ruling fixes the shape, not
the start. Every one of the five carries a `docs/inflight/bugs/` entry
written at `091be8c`, deleted by the sub-stage that closes it.

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
| **AT-S5** | **The route** (R12, AT-R5). A write runs where the session is; `ShipStatement`'s write arms retire with the FK probes, the index build and the assertion build; `MayWrite`'s last arm goes **with its replacement named in the same change** (AT-R11), taking the base virtual, the override and AT-3 B's 17 test sites with it | a cross-relation write mix from every core, byte-identical results to today. A peer writes a catalog page and the free map. The FK forward check meets a parent being written on another core and waits — AO-S5(b)'s cell, re-pointed at a local wait | **L** | S2 |
| **AT-S5b** | **The mechanical half of AT-S5's review** (AT-R15): D0's oid sequence, D2's `CreateAtUnpinned` arm and D7's mark counter, plus the three cells AT-S5 falsified and did not retire | two cores' creates issue distinct oids; a peer places a page at a chosen id in the system range and a second caller is refused `AlreadyExists`; core 0's sweep sees a peer's marks | M | S5 |
| **AT-S5c** | **D3**: the descent re-validates after its re-fetch (AT-R15) | two cores insert interleaved keys through a split, barrier plus rounds | S | S5b |
| **AT-S5d** | **D1**: one `AssertionEnforcer` for the instance (AT-R15) | `ExpeditorTest.APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt` passes; two cores insert into one group under `COUNT(*) <= 1` and exactly one is admitted | M | S5b |
| **AT-S5e** | **D6**: the index build takes the relation `X` (AT-R15) | an INSERT on core 1 waits for a `CREATE INDEX` on core 0 and its row is in the index | M | S5b |
| **AT-S5f** | **D5** and D4's walk half: the foreign-key checks go local (AT-R15) | AO-S5(b)'s cell re-pointed at a local wait; the deadlock rig's cross-core FK cycle still refused | **L** | S5b |
| **AT-S6** | **The cross-owner transaction and 2PC retired** (AT-R6). `txn_2pc_service.hpp` and the participant protocol go; `cross-owner-txn.md` is rewritten or struck; AO-R8's net **11 s → 1 s**, and `in_doubt_ceiling_ms` is **re-scoped and renamed** to the net rather than deleted, the old spelling refused at `expeditor.cpp`'s known-key check naming its successor (AT-0 item 7) | an undecided prepare cannot be constructed. Recovery's undecided-prepare arm is unreachable — the mount scan's own cell states what replaced it. The old key name is refused at startup with its successor named | **L** | S5 |
| **AT-S7** | **Statistics and Cabin local.** `AccessBatch` fold-and-flush becomes a local write under lock (CC13), `kAccessStatsBatch` struck; the two `CabinStore`s (AT-3 G) become one store under whichever of AR1 §11's two shapes AT-0 item 9 names; `cabin.md` §4b's scope rule struck; Waystone's peer-recording default lifted | `SHOW ACCESS` totals match the pre-change engine for the same workload. A peer's Cabin observation is banked and served. The contract suites for waystone and cabin stay byte-identical across configurations | M | S5 |
| **AT-S8** | **Checkpoint and the listener.** `RemoteCheckpointAnchor` retires; `Checkpoint()` becomes an instance task under an at-most-one-running flag; `kAnchorWrite` struck. D19's listener half as AT-R12 scopes it: `peer_listeners` becomes the arrangement, and its TLS/SCRAM refusal is removed with the core-local credential store or handed on by name | two cores cannot run a checkpoint at once. A session accepted on any core runs to completion there. TLS with per-core listeners, or the row says whose it is | M | S5 |
| **AT-S9** | **Placement and `owner_core`** (D17, D18, E8's verb, E9's withdrawal). AT's format event drops both columns and moves AT-R7's four anchors, adding the `SysTableRow` assert that is missing; `core_placement.hpp` and `PlacementPolicy` retire; NS10's verb becomes "declares the affinity of"; `core_count`'s pinning is decided with the rest of core specialization | a pre-AT volume mounts and its `owner_core` values are ignored — no refusal. `ns.table` still resolves; namespaces still exist as names | M | S5–S8 |
| **AT-S10** | **AU-S5**, one sub-stage per surviving group of AT-3 C's table, **plus D19's fallback handoff**, which the mark obliges this stage to list rather than strike (AT-R12) | per group: a call-site grep, and the group's own behaviour cell | **L** | S9 |
| **AT-S11** | **AU-S6**: count reaches 0; `RingTransport`, `RealRingTransport`, `SimRingTransport`, `ring_message.hpp`, phase 3, the N² preallocation, `AttachTransport` and the two transport tests removed; **G1's sentence** (`ar0-architecture-revision.md:45`, as AR0-6 revised it) rewritten with them | the suite; the golden log CRC unchanged; `SHOW META` loses its ring counters and `client-manual.md` says so | M | S10, **AT-0 item 4** |
| **AT-S12** | **The prose sweep.** CC11 and CC13 in `crosscore.md`; **`physical-optimizer.md`, which D18's mark names**; the `(M5)` comments **to the rule and not to §3's command** — `tests/` and the bare-`M5` claims of AT-3 A included; `core_runtime.hpp`'s asymmetries 1 and 3 and asymmetry 2's sentence; `Expeditor`'s "core 0 owns the superblock, the free map, the catalog pages and the listener"; `namespace.md`, `sched.md` §5, `rules.md` §3, `page.md` §6, `CLAUDE.md`'s rows | done-conditions written as greps, and **checked as greps** — AO-S8's two done-conditions were grep conditions that did not hold on the first pass, and a grep scoped to `include/ src/` is how this one would repeat that | M | S11 |
| **AT-S13** | **The prices.** E7's cell, which AO-S7 handed on because C3's shape did not exist on an engine that serialised writes per core — it exists once AT-S5 lands. The `cores = 1` A/B. `ck-tester`, `build-release`, BTREE-only per the 2026-09-08 mark, `git describe` on every number | `bench/v3.0.0/`, one results file, with p0 and p25, a wait breakdown and a delta against this engine's own previous number | M | S5 |

**Order, and why it is not negotiable at two points** (AT-R13, as AT-S3
corrected it): S1 before S2; S5 before S6–S9. Everything else may be resequenced by
the operator without breaking an argument.

---

## AT-6 — Row status

| stage | state |
|---|---|
| AT-S0 | **landed 2026-09-09** on `m3-at` — this document and `index.md`'s row. No code, no spec edit, no test; the suite was not executed and no pass is claimed. Overhead not measured. **Reviewed after landing** and corrected in the same branch: AT-8 |
| AT-S1 | **built 2026-09-09** on `m3-at`. The compiler declares every relation it binds into the statement's read borrow, between the name and the schema (`exec::Compile`/`CompileWhere` take a `PositionSink`; `read_borrow.hpp` is the borrow's home now); the three write verbs declare at resolve; a remote-step producer takes its own `IS` in its frame, because the coordinator's borrow dies at its `pending` return before the remote walk runs. Cells: a join declares both relations, a subquery declares at its bind, `INSERT`/`UPDATE` declare at resolve, a parked producer refuses a DDL's `X` and grants it after EOF. Three mutants killed (the compiler's line, the dispatcher's write-path line, the producer's). **Its review corrected the stage's own claim**: the bind's ask is a non-blocking `TryAcquire`, so a refused ask leaves the DDL-wins direction to DT1, catalog MVCC and catalog-only DDL - AT-7 item 10, AT-0 item 10 - and found two gaps the first draft had, `INSERT` and the remote step, both closed in the same change; `drop-table.md` DT7 now states the widened wait surface. **A second review found the fixes' own defects**: two minters in one holder-id space on one core (the dispatcher's and the step server's counters both fed `ReadHolderId(core, seq)` - latent only because no dispatcher borrow spans a suspension; bit 62 now names the minter), the consuming stage dropping its `IS` for a reactor turn between the open and its first resume (the borrow is handed down now, one holder id per stage, a cell pins it), and the owning spec - `txn.md` §5 - still stating the pre-AT-S1 contract with one bullet that denied what this stage built; rewritten, with `CLAUDE.md`'s row. **A third review verified the fixes** and found what lagged them: the borrow's owning comment on the dispatcher's sequence still argued the pre-D1 way; `CoreRuntime` declared its owned lock table below the scheduler, so a parked frame's release at teardown would have reached a freed table - unreachable today, fixed by declaration order; `RunConsumer` re-declared a step the open had already declared; and a consuming stage's inner walk, `index == 0` on its own core, re-took a slice per input row - the M2 rule that a nested walk declares no slice is restored with a `parent_` gate. **Stated omission**: the foreign-key check family - an `INSERT`'s or `UPDATE`'s parent, a `DELETE`'s children, the probe server's relation - declares nothing, because those checks are not steps; the following letter's, with D9(a). **Suite 3385/3385 in 281.90 s** on the tree committed, the baseline's 3379 plus six. Overhead not measured |
| AT-S2a | **built 2026-09-09** on `m3-at`: the schema version word. `Expeditor` owns one atomic; every core's catalog reads it; `BumpVersion()` bumps it at the catalog write, before the DDL's `X` is released (D21). **Its review found the first draft revalidating at every cached read, which frees a live `TableAccess*` inside a peer's statement on any core-0 DDL** - a join's second bind, an FK loop, a `SHOW` - where the broadcast it replaced dropped only between tasks. Fixed as AT-S2's own row states: revalidate once, at the task boundary, never inside a read - `DispatchAndStage`'s head, five named handlers (`OnStepOpen`, the two FK probe requests, the two build requests), the two `system` ticks (the refill, the Cabin optimizer) and the refill's completion; and `BumpWord` adopts a bump only when the cache was current, so a bump from another core is never swallowed - latent until AT-S3 lets a peer write, as is every drop the change makes: **`Revalidate()` is a no-op on core 0 today**, the single writer, whose `cache_built_at_` always tracks. **A second review found the same class once more** - `kwp_load_server.cpp` held a `Schema&` across the `Dispatch("BEGIN")` that now drops - fixed by copying out before the boundary, and `Dispatch`'s declaration now carries the contract. Retired: the hook, the broadcast, `InvalidateCatalog()` and its three remaining callers, both build services' `on_committed` seams, eighteen fixture calls. **A third review found the fixes correct** and what lagged them - five comments and declarations stating the opposite of the code beside them, a cell that would have crashed where it should fail, test-local word atomics outliving their pointers, the Cabin tick asking above its own off-gate - each fixed; and it named the premise the memo-only drop rests on, one frame table for every core, which `Revalidate()`'s declaration now states. `InvalidateFromPeer` → `DropCache`, kept for the two post-redo drops alone. **Re-scoped**: the four dead kinds, `kCatalogInvalidate`'s enumerator and D25's `static_assert` are AT-S2b's; `docs/spec/catalog.md` and the spec mentions are AT-S2c's. Nine cells; three mutants - `Revalidate` a no-op kills the reader and peer cells, `DispatchAndStage`'s boundary removed kills the rename cell (whose first draft used `CREATE TABLE`, which no memo ever holds, and survived), and `BumpWord`'s guard made unconditional kills the behind-cache cell. **Suite 3393/3393 in 292.45 s** on the tree committed, AT-S1's 3385 plus eight. Overhead not measured |
| AT-S2b | **built 2026-09-09** on `m3-at`: the strikes. `kExtentLease` (17), `kRelationFaultGrant` (21), `kRelationWriteGrant` (23), `kRelationGrantRequest` (24) - dead since AW-S1b - and `kCatalogInvalidate` (19), whose last producer and handler went at AT-S2a, are struck from the enum, from `IsKnownRingMessageKind` and from the name table, each value left as the record that it was spent (AU-R4, the form AU-S3 set for 20). **D25's freeze is written**: `kRingMessageKinds`, the one list of every kind this build sends or handles, and a `static_assert` on its size at **29** - not the 30 AT-R10 named, because AT-R10 counted the four and this stage strikes the fifth with them. **Its review found the first form wrong**: a hand-written `std::array<…, 29>` beside the switch let a dropped entry pad to `kUnset`, so the freeze restated its own template argument and `kUnset` counted as known; the census is the default-less `IsKnownRingMessageKind` switch again, and `CountKnownRingMessageKinds()` derives the frozen number from it at compile time — a new enumerator warns, a moved count fails. Three stale sentences fixed with it. `tests/coro_test.cpp`'s stand-in kinds are `kIndexBuildRequest`/`kIndexBuildReply`. **Suite 3393/3393 in 286.02 s**, AT-S2a's count. **Review owed**: the `critics-developer` pass was terminated by an API rate limit before it reported, and is re-run when the limit resets; landed on the operator's word to commit and push, with the gap named. Overhead not measured |
| AT-S2c | **built 2026-09-09** on `m3-at`: the prose. `docs/spec/catalog.md` opened, on AR0-5 §2.1's instruction, with the lock-then-word order as CT1, the bump-at-the-write / ask-at-the-boundary rule as CT2, what an unasking task serves as CT3, the one-pool premise as CT4 and what stays special about the pages as CT5. The six spec mentions of the broadcast rewritten - `ddl-transactional.md` (three), `crosscore.md` CC9 and §3, `alter.md` AL5, `drop-table.md` DT5, `keystoneid-k0-findings.md` - and `CLAUDE.md`'s Transactional DDL row names the file. No code. **Reviewed after landing**, and the spec claimed more than the code has: "DDL takes the relation `X`" where only `DROP TABLE` does; "the catalog pages are still unlogged" where every catalog write has been WAL-logged since 2026-08-19; `page.md` §3 for §6; a retired label for `ddl-transactional.md` §5b; eight boundary sites for nine; "per-instance" for one core's counter; "every invalidation bumps" for every one that can stale another core; "at most one DDL stale" for a bound nothing enforces - each fixed, and four arguments written in three to five places folded to one home each (`txn.md` §5 owns the borrow's reach, CT1 the counter rule, `catalog.cpp` `BumpWord`'s rule, CT2-CT4 what `Revalidate()`'s header now only points at) |
| AT-S3 | **built 2026-09-09** on `m3-at`, and it built no unit: **E13 is answered no.** A named key's admission writes the relation's `sys.tables` row outside the caller's transaction, in place under the page latch (`heap-and-tuple.md` §4.1), so a tuple `X` on the row has no contender the latch does not serialise, and a transaction-length one would serialise every named-key `INSERT` into a relation for nothing - AO-S6e's shape, found by reading the write. What a peer waits on to stop shipping is the page write, `MayWrite`'s arm, AT-S5's. Landed: the declared-shared row for the catalog pages in `rules.md` §3, `catalog.md` CT5's paragraph, one cell pinning that two open transactions' named keys into one relation admit without waiting on each other and leave no catalog-row entry in the ledger. **AT-R13's "the borrow before the route" is corrected** (AT-7 item 11): S3 gates nothing, and S5 may follow S2 directly. `keystone` unchanged: `next_id`'s bump logs and replays as before. **Suite 3394/3394 in 284.05 s.** **Reviewed after landing**: the "no unit" verdict holds and the stated mechanism was wrong - the page latch is re-entrant for the owning core, so within one core the two admissions are serialised by the rule that no task parks under a page span, not by the latch; that two-part sentence is now AT-R11's replacement for `MayWrite`'s arm (which this stage had struck without replacing) and `before_mark`'s declared contract. Item 11 and the S5 row's gate agreed with item 12 only after the review; CT5 now says the flip's problem is staleness, which the word fixed, and the mark is uncached; `rules.md`'s row names CC11 as the contradiction AT-S12 owns; AR0-5 §3's "closed: yes" carries the correction inline; the cell probes the catalog relation rather than only counting. Overhead not measured |
| AT-S4 | **resequenced after AT-S5** (2026-09-09, on the survey below; AT-7 item 12), not started. The shared allocator's *issue* half is a `fetch_add` any core can make, but its *persist* half is a page-0 write - `TrxIdSequence::Carve` makes the raised ceiling durable before it returns a block, and `Catalog::AllocateRowId` bumps `sys.tables.next_id` in place - and `MayWrite`'s last arm forbids that write to every core but 0 until S5 retires it. An S4 built before S5 would keep the persist on core 0 behind a new pre-raise protocol, which is the lease under another name; AR0-5 §2.2's "advance by CAS from whichever task crosses the threshold" assumes S5. The two lease families and their spent refusals stay until then; the peer's first `INSERT` per relation stays retryable exactly as today |
| AT-S5 | **built 2026-09-09** on `m3-at`: the route. A write runs where the session is. `DevicePageStore::MayWrite`'s last arm and `ResidentBytes`' gate are gone with their replacement named in the same change (AT-R11 as corrected at AT-S3: the page latch across cores, no park under a span within one, `map_latch_` for allocation, the word for memos, page 0's CAS at S4); `catalog_read_only_` and its four gates - the DDL refusal and ship, `CheckWriteAffinity`'s cross-core refusal and its shape-gate wrapper (the index-window wait and `CannotEnforce` now run on every core), the sorted-fill gate, the named-key refusal - are retired, as are the three write ship forks, the CC3 arm, `PeerDdlRefused` and `CrossCoreWriteRefused`; `WriteTargetCore` and `owner_core` are read for the counter only (D18). The purge stays on core 0 as a placement, not an authority. CC11 rewritten whole, CC13's CR5 and CC3's first sentence with it; `heap-and-tuple.md` §4.1's peer refusal is history. **Reads' placement is untouched** (D18, AT-0 item 4): `MayShip`, `ShipStatement` and `CheckReadAffinity` survive for reads. **The row said the index and assertion build request handlers were left as dead protocol and they are not** (AT-7 item 13, found by AT-S5b's source read): four `owner_core != core_id_` branches still ship a build or refuse one - `command_dispatcher.cpp:3277`, `:3310`, `:4020`, `:4047` - so AT-3 C's rows for those kinds are AT-S5d's and AT-S5e's, not this stage's. Five more owner tests the first pass missed, lifted on the fixtures' word: `CheckRangePlacement`'s "routed to the wrong core" backstop, `CheckNoChildReferences`' "cannot see its rows" refusal, the FK probe server's two fail-closed owner tests, and `VisitRelation`'s "cannot read locally" refusal of a foreign range on a write's walk - true of a per-core pool, false since AM-S2 step 3, and not the read-placement fork (D18), which stays. Cells: the seven store cells that pinned the refusal flip to the admission; the range insert that was refused by name lands in its range's chain and so does the next omitted key; a pk-named write into another core's range runs and the unnamed spanning one is still refused; a cross-core write is counted and runs; the reverse FK check walks a foreign child; on the rig a peer creates a relation core 0 resolves at its boundary, and a peer admits a named key that moves the shared mark. **Forty-one cells retired**, each with a record of the premise that went: thirty-seven in `core_runtime_test.cpp` (shipped writes, the cross-owner transaction, a peer's DDL refusals, the foreign build requests, the row-id demand a shipped insert left), one each in the deadlock and FK-probe rigs, two in the probe server's own suite - the cross-owner transaction's cells go here because S5 removes its traffic, though its code is S6's. `SHOW META`'s `cross_core_write_refusals` now counts cross-core writes that ran; the token is renamed at S12. **The stage is landed but not closed**, on the operator's word to commit and push, with what is owed named rather than implied. **Suite 3350/3354 in 259.17 s** — four cells fail, each a claim this stage falsified and did not retire or flip: `FkReverseProbeTest.AChildThisCoreDoesNotOwnIsRefusedRatherThanAnswered`, `AffinityDispatchTest.AWriteToAnotherCoresRelationIsRefusedRetryably`, `ADeleteIsAWriteAndIsCheckedAsOne`, and `ExpeditorTest.APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt`. **Its review returned seven defects, none fixed in the landed commit**, all of one shape — the route removed the routing that made four other per-core structures sound, and unified none of them: **D0** the object-oid sequence is one in-memory counter *per catalog*, so two cores running `CREATE TABLE` issue one oid (landing-blocking; the fix is written and not applied); **D1** an assertion's Bound Cabin is the owner's, so a write on a core whose enforcer holds no directory runs unenforced — the failing `Expeditor` cell (landing-blocking); **D2** `CreateAtUnpinned` still refuses the system range to a peer, so a peer's `CREATE TABLE` fails permanently once a catalog page fills; **D3** a btree descent is not re-validated after a refetch; **D4** the FK reverse check takes `kPass` from an exhausted per-core Cabin set; **D5** the FK intent and pending-delete tables are per-core; **D6** a local `CREATE INDEX` opens no window; **D7** `pending_marks_` is per-catalog and a peer's marks are never purged. D0, D1, D2, D7 and the four cells are the next pass; **D3–D6 are design decisions** — each unifies a per-core structure or restores a routing — and are taken as sub-stages before S6, their shapes ruled at AT-R15. Overhead not measured |
| AT-S5b | **built 2026-09-09** on `m3-at`: the mechanical half of AT-S5's review (AT-R15). **D0** - the object-oid sequence is the instance's, one atomic on `Expeditor` seeded by CAS from `HighestIssuedUserOid()` and issued by `fetch_add`; **D2** - `CreateAtUnpinned`'s system-range arm goes with its replacement named in the same change (AT-R11), the claim under both latches answering every loser `AlreadyExists`, which is the one code `AllocateCatalogPage`'s probe loop walks on from, so a peer's catalog chain can grow at all; **D7** - the delete-mark count is the instance's, because the purge runs on one core and its gate could not see another core's marks, and the resettle is a **delta** rather than a store so a mark added while the sweep walked survives. Three cells, three mutants killed - and **the first draft of D0's cell did not kill its**: one create per core does not collide, because a catalog seeds lazily and the second to run scans the first's rows; the collision is a core's *second* create, issuing from a counter seeded before the other's rows existed, and the rewritten cell reads 4003 twice under the mutant. Three cells retired in AT-S5's own one-line form, the two `AffinityDispatchTest` refusals and the reverse probe's owner test. Prose: `catalog.md` CT5 takes the allocation and **CT6** the three instance words with the reason they are three and not one struct; `crosscore.md` CC12's **CR2 was still "DDL executes on core 0; a peer sends and waits"** and CC10's counter note still named `PeerDdlRefused`, both AT-S5's misses; `ddl-transactional.md` §5d gave two dead premises for the sweep's placement, `MayWrite` and `PeerDdlRefused`, and now gives AN-S2's; `rules.md` §3's row; and the store cell AT-S5 flipped without renaming, `APeerMayNotWriteTheSystemRangeOnASharedStore`, whose name denied its body. **Five `docs/inflight/bugs/` entries opened** at `091be8c`, one per defect this pass does not close. **Suite 3353/3354 in 92.85 s**, the one failure `ExpeditorTest.APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt` - which is D1 and AT-S5d's, named rather than retired because it is a defect and not a stale claim. **Its `critics-developer` pass found no correctness defect in the three fixes** - it traced the delta's interleavings and reports it cannot undershoot, the CAS seed cannot duplicate or skip, and deleting the store's arm is safe because `ClaimNamedIdLocked` really is the one admitter - and found eight things beside them, all applied. The two worth the row: **three code comments still asserted `PeerDdlRefused` and `MayWrite`'s arm as live guards**, one of them in the present tense asserting the very premise this stage rewrote §5d to retire, which is AT-7 item 14's own lesson unapplied to the item that states it - the grep was run over `docs/` and not over `src/`; and one fixture, `OpenForeignIndexRig`'s second catalog, took the schema word and not the other two, a latent duplicate-oid trap for two lines. Also: the D7 argument was written twice in one header and now lives where the gate is read, `CreateAtUnpinned`'s replacement comment lost sixteen lines of history CT5 already carries, `Expeditor::pending_marks_` is `delete_mark_count_` because it shared a name with the local fallback it replaces, and the "eight cores" claim was two cells' worth stated as one - `free_map_race_test.cpp`'s races eight cores in the *user* range and never touched the retired arm. **One finding declined**: the review costed the three words into one struct and is right about the cost, and right that both of CT6's original reasons were wrong; the change re-opens AT-S2's landed wiring, which this stage may not do on its own account, so CT6 is re-stated on the honest ground - a threshold, not a principle - and it becomes AT-0 item 11. Overhead not measured |
| AT-S5c | **built 2026-09-10** on `m3-at`: the descent re-validates (D3, AT-R15). **The window was wider than the review named it.** D3 was reported as the re-fetch - `DescendTo` drops the leaf's shared hold before taking it exclusive, the page latch never being upgraded - and a first draft compared the leaf's right link across exactly that, which built, passed the suite and **still failed its own cell**: the descent has no latch coupling, so it releases the *parent* before asking for the child, and the child's range can shrink in that gap too, before this descent has touched the leaf at all. What it checks now is coverage itself, asked of the chain as it stands: a leaf covers `[min_key, right sibling's min_key)`, both ends immutable, so the one question is whether the sibling's bound still sits above the key. **Free on the shape the engine inserts** - a monotonic pk lands on the rightmost leaf, which has no sibling to ask - and one resident-page read on a caller-named key mid-chain. Restarts are bounded at 4 and **make progress by construction**: a splitter holds the old leaf across its own `PromoteSeparator`, so a re-descent is granted the leaf only once the parent carries the new separator. **The read path has the same gap and is fixed with it**: a divide *moves* keys, so a lookup routed to the old leaf answers `NotFound` for a row that exists - the one answer `btree.hpp` says a descent may never give - and `BtreeLookup` now checks coverage **on the miss and not on the hit**, a hit being authoritative however the chain has moved, so the hot path pays nothing. `BtreeSeekLeaf` checks nothing and the reason is directional, stated at the site: a split moves keys only right, so an outrun seek lands *left* of where the key went and the scan it feeds walks onto that page next. **Then the suite found two more of AT-S5's family, and they are why this stage is larger than its ruling.** A btree root repoint (`Catalog::UpdateRelationDescPage`) updated **only the repointing core's** cache in place - *"no invalidation broadcast, no catalog write"*, sound while one core did every write - and worse, `InitTableAccess` consulted the **anchor** that carries the current root only `if (access.owner_core == core_id_)`, on the stated ground that *"a foreign relation's root is never walked here anyway, execution ships to the owner"*. AT-S5 deleted that ship. So any core that did not "own" a relation descended from its **CREATE-time** root, which after a level growth is the new root's leftmost child, and wrote rows into a leaf that could not hold them - silently, until this stage's check refused it. Both halves fixed: the anchor is read by every core, and the repoint calls `BumpWord` so a stale memo drops at the next task boundary while the descent's retryable refusal covers the window between. One test fixture was found doing the same thing on its own account - `exec_budget_test.cpp` never applied `new_root` at all - and now persists it the way the dispatcher does. Three cells in `tests/btree_race_test.cpp`, all on real threads with a **rendezvous before every insert**: the chain ascends and every tuple sits inside its leaf's interval; a scan returns every row in order; a lookup does not miss a row a divide moved. Two mutants, each run ten times and killed ten times - and the numbers are the stage's own correction, because the first shape of the cells (started together, then left to run) killed only **three in six**, and the first cell drafted, which asserted a descent still *finds* a misplaced row, survived its mutation **six times in six**: the splitter promotes the separator that routes to its new page, so the tree half stays self-consistent and it is the **chain** that breaks. That cell was replaced rather than kept. Specs: `btree.hpp`'s concurrency paragraph, which still said *"there is no concurrent mutation to protect against yet"*; `heap-and-tuple.md` §5 takes the descent's cross-core authority and §6 loses one more AT-S5 miss, *"a write routes to the relation's owner"*. `docs/inflight/bugs/btree-descent-refetches-without-revalidating.md` deleted, the bucket's rule for an entry its stage closes. **Suite 3356/3357 in 94.08 s**, the one failure `ExpeditorTest.APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt`, which is D1 and AT-S5d's. **Its `critics-developer` pass could not break the coverage check** - it verified sufficiency and necessity against both split shapes, walked every `next_page_id` consumer in the tree to establish that **nothing anywhere holds a page and asks for the one to its left**, and confirmed the progress argument against the code rather than the row - and found five things around it, of which the row's own *"both halves fixed"* was one: **there are three**. `Catalog::UpdateIndexRoot` is `UpdateRelationDescPage`'s self-declared exact mirror and did not get the bump, so a peer's index insert descended a stale index root and landed entries in a subtree no later probe reaches - **and unlike the clustered root there is no coverage check underneath**, so it is silent; the reviewer applied the one line and this stage carries it. Two more taken here: `LeafStillCoversKey` parsed the sibling's `min_key` without a type check, where `min_key` lives in the heap page *body* and on a non-leaf is another view's field - the one fetch in the file that skipped `RequireType`; and removing the anchor's `owner_core` gate left the *fall-through* beneath it standing, so a relation whose anchor could not be faulted silently reinstated the CREATE-time root, its stated justification (*"the pre-grant window ... nothing can have moved a root the owner could not yet write"*) resting on grants AW-S1b deleted - it refuses now. The exhaustion message named churn as the cause where a **stale root** is the everyday other and looks identical from there, so it names what it knows instead. **One finding is not fixed and is AT-0 item 12**: the stage made a lookup's *status* authoritative and left its *payload* a hint - `BtreeLookup` returns a `(page, slot)` and drops the leaf, and all seven callers re-fetch by id, so a divide that compacts slots gives them a different row. `docs/inflight/bugs/a-lookups-location-is-stale-before-its-caller-reads-it.md` carries it. Overhead not measured |
| AT-S5d | **built 2026-09-10** on `m3-at`: one `AssertionEnforcer` for the instance (D1, AT-R15). **The registry** - `Expeditor` owns it, armed at `cores > 1`; `CoreRuntime::Config::assertions` hands it to every peer; a dispatcher never handed one builds its own, `LockTable`'s idiom. **Core 0's mount resumes it for every relation** and a peer handed it resumes nothing - `ResumeAssertionsAfterRecovery` loses its `owner_core` filter, and `MountRecovery::assertions_foreign` goes with `SHOW META`'s `recovery_assertions_foreign`, a counter nothing could move. **Two latches, both null unarmed** (`assertion_check.hpp`, `rules.md` §3's new row, `sched.md` §9-2): the ruled **directory latch**, which never spans page work, and a **chain latch** per assertion, which does - the ruling named one latch and the stage needed two, because a cabin chain's tail and growth are one writer's state and the growth is page work. **The admission holds** what it admitted (`AssertionEnforcer::Hold`): a hold counts in every later admission and in no header, `ReserveInsert` converts it one step with its `ASSERT_RESERVE`, and every exit that does not reach the reservation gives it back - a refusal, a failed placement, a borrow that parks and re-runs. A negative contribution is held as zero, and an `UPDATE` holds its arrival's **full** contribution whatever it checked, because its departure lands first and would otherwise read as room to another core in between. An assertion adopted after a row's admission is admitted at its reservation instead. **What the ruling did not name, and the stage found by reading the recovery pass**: a checkpoint's `ASSERT_SNAPSHOT` is a base only if it equals the fold of every record before it, and with a reserver on another thread a snapshot could land between a header change and its record. So the directory latch also spans the WAL append that describes a header change, and the snapshot is taken and appended under it through a new seam, `AssertionSnapshotSource::VisitSnapshots` (a single-threaded source keeps the default); `BoundCabinChainWriter::Append` splits into `Place` and `Log` so the record can be appended inside the latch while the page work stays outside it. **The ship is retired** - AT-7 item 13's `:4020` and `:4047` branches: `CREATE ASSERTION` builds where its session is and adopts into the registry, `DROP` evicts from it, `server/assertion_build_service.{hpp,cpp}` are deleted, ring kinds 30-32 are struck and D25's freeze moves 29 → 26, and the success line's `built_by_core=` and `SHOW ASSERTIONS`' `enforced_by_core=` go with it. **Found and not closed**: the build takes no relation lock, so a writer on another core can land a row the scan already passed before the directory is adopted - a quiet understatement that predates this stage (the owner's build had the same window since AT-S5). `docs/inflight/bugs/create-assertion-build-is-not-fenced-against-writers.md` and AT-0 item 13; `assertion.md` §8.1 now says so instead of *"no write can interleave with the build"*. Cells: five in the new `tests/assertion_race_test.cpp`, on real threads over an armed store - two cores admitting into one group admit exactly one **and refuse the other at its admission**, an assertion adopted between admission and reservation is checked there, a hold counts in every admission and in no snapshot and is room again once given back, a snapshot holds the directory against a reservation on another core, and eight cores reserving into one cabin lose no entry. **Mutations, measured**: the admission taking no hold, killed 5 in 5; the reservation skipping its uncovered check, 1 in 1; the snapshot without the latch, 3 in 3; a hold never given back, 1 in 1; the directory latch never armed, 5 in 5 against the eight-core cell. **The chain latch removed survived 10 in 10, at two writers and again at eight**, and the cell says why rather than claiming it: a growth links from the *live* tail under that page's latch, so losing a link takes a third writer inside another's growth, a window the shape does not open. The latch stays for the plain fields two threads would otherwise write, and no cell pins it. Two cells in `core_runtime_test.cpp`: `UnderOneStreamAPeerWithAnAssertionInTheCatalogStillMounts` re-pointed from `assertions_foreign` to the unrevivable declaration refusing its relation's writes, and `APeerHandedTheInstancesAssertionRegistryResumesNothing` new. **`ExpeditorTest.APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt` passes, and it was edited to pass**: its enforcement reading - a second row in group 7 refused on core 0's listener for a relation `kRotate` placed on core 1, which is D1 exactly - is unchanged, and its counter reading moved from the peer's recovery report to core 0's, because the resume moved; it now also pins that the two cores hold one registry. Three `core_runtime_test.cpp` cells retired in AT-S5's one-line form - the owner's restart folding its own cabin, a refused foreign build, an abandoned one - and the four earlier records that said the handlers stayed for AT-S10 now say they went here. Specs: `assertion.md` §6 retitled and §6.1 rewritten whole, §6.2's steps and two properties, §4.2, §4.3, §7's AS6a note, §8.1, §8.1a and AS4's row; `crosscore.md` CC7's owner-builds exception for assertions and §6a's gate, whose two of three reasons are gone and which is kept unre-decided; `client-manual.md`'s recovery block; `manual/sql/sql.md`'s multi-core bullet; `CLAUDE.md`'s Assertions row. `docs/inflight/bugs/assertion-enforced-only-where-the-directory-is.md` deleted, the bucket's rule for an entry its stage closes. **Suite 3360/3360 in 70.86 s** on the tree committed - AT-S5c's 3357, less three retired, plus six new, and D1's failure gone. **Review owed**: the `critics-developer` pass runs on the landed commit and its findings land in the next. Overhead not measured |
| AT-S4, AT-S6 … AT-S13 | not started; each gated on the operator's word (S4 after S5, AT-7 item 12) |

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

| 11 | **One struct for the instance's catalog words, or three pointers?** AT-S5b's review costed it: the three are identical in lifetime, plumbing, default and wiring site, mirrored in three fixtures, so each new word is about a dozen edits and a struct collapses them to one. It also refuted both reasons CT6 first gave for three - a struct creates no shared *rule*, and removing a field from one is a single deletion | design, mechanical | **three at three words, one at four.** The saving is two setters and two `Config` fields; the cost is re-opening AT-S2's landed schema-word seam, which this stage may not do on its own account. `catalog.md` CT6 states it as the threshold it is rather than as a principle, so the next word decides it rather than re-arguing it |
| 12 | **Marked (a) by the operator, 2026-09-10: `BtreeLookup` returns the held `PageRef`.** Not built; its stage waits for the word. **Does `BtreeLookup` hand back the leaf it read, or do its callers re-check the key?** AT-S5c made the lookup's *status* authoritative across cores and left its *payload* a hint: the `(page, slot)` it returns is unheld, and all seven callers re-fetch by id, so a concurrent divide - which rebuilds the leaf and compacts its slots - hands them a different row. In `step_vm` the residual drops it and the statement answers **zero rows for a row that exists**; in `fk_check` there is no residual and it decides a constraint verdict | quiet-wrong class, design | **(a) return the held `PageRef` beside the `Location`.** It is what `Descent` already produces, it is correct by construction, and it costs a pin held across the caller's read - which every other page access in this engine already holds. The alternative, (b) a Keystone re-check at each of seven sites, is cheaper and spreads one invariant across seven places that must each remember it. Six callers change either way |
| 13 | **How does a `CREATE ASSERTION` build fence off a writer on another core?** Found by AT-S5d's read: the build takes no relation lock, so a writer that is admitted before the directory is adopted, places its row where the scan has already passed and reaches its reservation before the adoption is in neither the scan nor the cabin (`docs/inflight/bugs/create-assertion-build-is-not-fenced-against-writers.md`). Present since AT-S5, when the owner's build stopped being the only core that wrote the relation | quiet-wrong class, design | **D6's shape, ridden with AT-S5e**: the build takes the relation `X`, which a writer's `IX` waits on. Two things D6 does not decide and this does: the build is not transactional DDL, so the `X` needs a holder - a DDL transaction around the build, or a non-transactional holder id released at the statement's end; and an `INSERT` takes its relation `IX` only at its id borrow, *after* its admission, so the `IX` moves ahead of the admission or a writer admitted before the build can still slip past it. CLA proposes the non-transactional holder, because wrapping the build in a transaction changes what a failed `CREATE ASSERTION` leaves behind, and the `IX` moved ahead of the admission |

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

11. **AR0-5 §3's "R5 struck: tuple `X` on the `sys.tables` row, uniformly"
    and AR2 E13's question both assume the row is a transactional unit.**
    It is not: the admission's catalog write is an unversioned in-place
    overwrite under the page latch, outside the caller's transaction
    (`heap-and-tuple.md` §4.1). What is uniform is the latch, which every
    core already takes, and what a peer waits on to stop shipping is the
    page write - `MayWrite`'s last arm, AT-S5's. AT-S3 built no unit and
    says so (AO-S6e's precedent); AT-R13's "the borrow before the route"
    ordering dissolves with it - S5 needs S2 only, and S4 needs S5 (item
    12). AR2 R5's third paragraph stays true until S5 retires the ship.
12. **AR0-5 §7's order puts the shared allocators (S4) before the route
    (S5), and the tree says the reverse.** A shared allocator has two
    halves: the issue, a `fetch_add` any core can make, and the persist -
    `TrxIdSequence::Carve` raising page 0's ceiling durably before it
    returns a block, `AllocateRowId` bumping `sys.tables.next_id` in place.
    The persist is a system-page write, which `MayWrite`'s last arm
    (`device_page_store.cpp`) refuses to every core but 0 until S5 retires
    the arm with the route; the function a shared row-id allocator
    replaces is `AllocateRowIdRange` as much as `AllocateRowId`. §2.2's
    "page 0's persisted ceilings advance by CAS from whichever task crosses
    the threshold" is true only after S5. **The third allocator is not
    blocked and shows what the block is**: free-map allocation is any core's
    under `map_latch_` (AM-S3) and `FlushMaps` is the one write path that
    reaches the device without asking `MayWrite` - so it is the gate, not
    durability-before-issue, that forces S4 after S5. (§2.2 says the map is
    "under their page latch"; the tree does it under `map_latch_` over
    in-memory regions, a drift S4 will meet.) AT-R13 allows the
    resequencing, and this is its reason.
13. **AT-S5's row claimed the build request handlers were dead protocol,
    and four branches still ship or refuse a build** (`command_dispatcher.cpp:3277`,
    `:3310`, `:4020`, `:4047`). Found by AT-S5b's source read. The
    consequence is a re-pointing rather than a repair: AT-3 C's rows for
    `kIndexBuild*` and `kAssertionBuild*` belong to AT-S5e and AT-S5d,
    which retire the ship with the structure that made it necessary, and
    AT-S10 inherits handlers with no producer rather than handlers with
    one.
14. **AT-S5's prose pass missed three sentences, each stating a rule that
    stage retired** - `crosscore.md` CC12's CR2 (*"DDL executes on core 0;
    a peer sends and waits"*), CC10's counter note naming `PeerDdlRefused`
    as what the counter cannot see, and `ddl-transactional.md` §5d, which
    justified the sweep's core-0 placement by `MayWrite` and
    `PeerDdlRefused` **both of which AT-S5 deleted**. Fixed at AT-S5b. The
    consequence for method: AT-S8's lesson - a done-condition written as a
    grep and *checked* as one - was not applied to AT-S5's own sweep, and
    AT-S12 is where it has to be.

15. **The review's D3 named the re-fetch window and the defect is the
    descent's staleness generally** (AT-S5c). There is no latch coupling: a
    parent is released before its child is asked for, so a leaf's range can
    shrink before the descent reaches it, with nothing to compare a later
    reading against. A fix scoped to the re-fetch builds, passes the suite
    and fails the cell. The consequence for method is the one this tree
    keeps re-learning: **the cell was written before the fix was believed**,
    and it is the only reason the narrow version did not land.
16. **Two more predicates whose justification was AT-S5's routing, found by
    AT-S5c's suite rather than by its survey** - `InitTableAccess`'s
    `access.owner_core == core_id_` gate on reading the anchor (*"a foreign
    relation's root is never walked here anyway, execution ships to the
    owner"*), and `UpdateRelationDescPage`'s in-place-only cache update.
    Together they meant a peer descended from a relation's CREATE-time
    root. Both fixed at AT-S5c. The consequence: **AT-S5's census of
    `owner_core` reads was of the *dispatcher*** (AT-S5b's AT-7 item 13
    listed four it missed there), and the catalog was never swept for the
    same shape. AT-S12's grep is owed that file.
17. **A predicate's removal is not done when the branch it gated is left
    standing** (AT-S5c's review, C3). Taking the `owner_core` gate off
    `InitTableAccess`'s anchor read left the *else* arm beneath it - a
    fall-back to the CREATE-time root - with a justification written for the
    gated world (*"the pre-grant window ... nothing can have moved a root
    the owner could not yet write"*, itself resting on grants AW-S1b had
    already deleted). The lifted gate widened that arm from "an own
    relation, briefly" to "any relation, always", which is the wrong answer
    the lift was made to remove. The consequence for method: **the stages
    that lift AT-S5's predicates owe their `else` arms the same read**, and
    AT-S5b's item 13 and AT-S5c's C1 are the same lesson in two other
    files - a census run over one function is not a census.
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
