# Cross-Core Execution

How a single statement that references relations owned by different cores
executes. This is the concept spec for the mechanism `docs/spec/protocol.md` D3
reserved ("server-side forwarding — clients are core-topology-unaware") and
`docs/spec/sched.md` §5 provides transport for. Consistent with `docs/rules/rules.md`
(thread-per-core, shared-nothing, no exceptions, deterministic testability).

**Revised 2026-08-24 (v2): the ownership unit is the primary-key range,
not the relation** — operator-directed, promoting
`docs/inflight/in-progress/blueprint-range-ownership.md` §1 into this spec. CC8 defines the
unit; a relation starts life as one range owned by its creating core, so
`sys.tables.owner_core` is the degenerate case, not a retired concept —
everything shipped is the one-range instance of the rules below, and
**nothing range-granular is built at this revision** (worktree
`v2-crosscore-range-rules`; the build phases are the blueprint's §11,
R1-R6). The unit, the directory and the routing are §2a; the widened
write scope is CC3 and §6; split and migration are CC10 and §6a-§6b.

Scope boundary, **revised 2026-08-28**: this spec covers cross-core
**reads** and the routing every statement takes. Cross-core **commit** — a
transaction touching relations owned by more than one core — is **built**,
and its spec is `docs/spec/cross-owner-txn.md`; it is not designed here,
and where the two disagree that one wins on the protocol and this one on
routing. §6 defined the v1 restriction that kept commit single-stream, and
§6's refusal list below now records which parts of it survive. Since v2
"cross-core write" includes a statement or transaction writing two ranges
owned by different cores, *even ranges of one relation* (CC3) — and
multi-*range* transactions are the one part of the restriction that
stands, because they inherit the commit protocol unchanged but need RD3's
resolver to discover their participants.

`[PROPOSED]` marks a default to confirm or amend before the affected part is
built. `[OPEN]` marks a deferred decision that must not be assumed.

## 1. Decisions

| # | Decision | Resolution |
|---|----------|-----------|
| CC1 | Execution model | **Step pipeline (function shipping)** — each step runs on the core owning its range(s) (v2; "its relation" until 2026-08-24, the one-range case); output flows to the next step's core; final rows to the session core |
| CC2 | Intermediate transfer | **KWP binary row batches (protocol D5 encoding) in chunked ring messages + credit-based flow control** |
| CC3 | Write scope | **v1 is read-only cross-core.** A transaction's writes bind to one home core; a write targeting another core's relation is a retryable error. 2PC write support reserved `[OPEN]`. **Widened 2026-08-24 (v2):** the binding stays per *core* (stream) and ownership is per *range*, so the retryable refusal is a write targeting a range owned by another core — which now includes a statement or transaction writing two ranges of *one* relation that live on two cores. Two ranges the home core co-owns stay legal: single-stream, nothing 2PC-shaped in them (§6, and the reader-side consequence in §5) |
| CC4 | Remote-read isolation | A remote step reads the owning core's **latest committed snapshot**; no cross-core ReadView. RC-equivalent; RR weakening documented (§5). v2: per range — each stage reads its *range* owner's view, under the one-view-per-(statement, core) rule §5 adds |
| CC5 | Cancellation & errors | Cancel/error messages propagate both directions; every message tagged `(session_core, request_id, step_id)`; stale batches discarded by tag |
| CC6 | Scheduling | Remote step tasks run in the **foreground** group on their core (step chains are the OLTP path) |
| CC7 | Page-ownership reconciliation (the P6 blocker; operator-decided 2026-08-10) | **Page ownership is a function of the catalog**: a relation's pages belong to the core `sys.tables.owner_core` names, whatever lease allocated them. Realized at DDL publish by a **flush-then-grant handoff** — core 0 flushes the relation's pages, then grants the owner fault rights at extent granularity over the ring, and the owner faults fresh frames: the same discipline P6's catalog half already uses for catalog pages. The alternative (CREATE TABLE allocating from the owner's lease) was rejected as a new cross-core allocation protocol inside DDL that still needs a creation-time write exception. Two consequences stated now: the store's debug `MayFault` check stays extent-granular, so a granted extent may carry pages of other core-0 relations — a **superset assertion**, acceptable because the enforced mechanism is statement dispatch to the owning core, never the assertion; and a catalog-derived ownership fact is one of the two candidate fixes `physical-optimizer.md` §6 gate 3 names, so this decision serves both. Ownership **rebalancing** after creation stays out of v1 with M3. **Generalized 2026-08-24 (v2, CC10):** every page movement between streams is a PL-B logged handoff (`docs/spec/page-lsn-cross-stream.md` §9); the DDL-publish flush-then-grant survives as its easiest case (pages quiescent, no peer ever logged), and `docs/inflight/in-progress/workplan-peer-writer.md` §8 (PW1c: flush → durable handoff record → grant-with-write-rights, exact-page) is the contract's first consumer. **The durable form of the fact is the page's PL-C stamp (2026-08-24, PW1c-7)**: leases and grants are memory-resident, so a core's ownership after a restart is re-derived from the stamp on each page it faults — the catalog still says which core *should* own a relation, the stamp says which stream *does* own each page, and the two agree by construction until the mover exists, which must keep them agreeing by restamping *and* revoking. **The owner-builds exception (2026-08-25, PW1c-6b, `docs/inflight/in-progress/workplan-peer-writer.md` §7c):** this cell's flush-then-grant realizes ownership by having core 0 *produce* the pages (format them, then hand fault/write rights over), and for a `CREATE INDEX` on a peer-owned relation that premise fails — core 0's `Backfill` reads the device's last checkpoint and misses every row the owner holds uncommitted or never checkpointed, so a tree built on core 0 would be wrong, not merely mis-owned. For that one DDL the **owner builds** the tree in its own stream from its own lease, and no page crosses a stream at all: PL's handoff (CC10) is not invoked, and CC7's own rejection of "owner allocates at DDL" (recorded for CREATE TABLE, where the pages were empty and core 0 could format them) is *taken* here, on the ground that the stance assumed core 0 could produce the pages. Core 0 keeps only the catalog half (the `sys.indexes` row and the commit); `docs/spec/ddl-transactional.md` §5e is the atomic/isolated account. **Widened to assertions 2026-08-26 (PW1c-6c, §7d)** on a *stronger* premise than the index's: an assertion's Bound Cabin is not merely built from rows core 0 cannot see, it is **appended to by every write to the relation**, so its pages must be writable by the owner forever after — which a core-0-allocated chain never is (`MayWrite` refuses it). So the owner builds the cabin from its own lease, own-stamped, no handoff, and **holds its live directory**: the enforcing core for an assertion is the core that owns the relation, on every core and at every mount. Core 0 keeps the `sys.assertions` row, and a `DROP` on core 0 tells the owner to forget the directory |
| CC8 | Ownership unit (v2, operator-directed 2026-08-24) | **The pk range `[lo, hi)` of one relation.** Not the relation — too coarse: it caps a hot relation at one core permanently, and one dominant relation is the stated workload's ordinary case. Not the page — too fine: M1's rejection of page/extent hashing stands, and its argument is what sizes the unit — btree descents and heap-chain walks cannot cross cores per hop. Both clustering modes are key-ordered structures and `min_key` is immutable (invariant 2), so a boundary is stable for a page's life and a descent under range ownership crosses **at most one boundary, at the top**. Two qualifications the code forces (added at the 2026-08-24 review): the one-boundary claim is the **btree's**, and its top levels belong to whoever owns the root — that hop is the one shared structure **CC11's rule does not reach**, because its writer is the root's owner core and never core 0, so it stays `[OPEN]` on its own terms (indexed in §9; blueprint §8's own bullet closed 2026-08-30). The heap chain has no descent at all — every walk enters at the head and follows `next` — so **a split relation's ranges are per-range sub-structures**: a heap range is its own chain with its own head, a btree range its own subtree entry, and the entry page rides the directory row (CC9). Per-range chains are R3's largest piece, named here so they are built, not assumed. A relation starts as one range owned by its creating core (§2a) |
| CC9 | Range directory | **Ownership stays a function of the catalog** — workplan guideline 4 kept, not amended. A catalog relation (working name `sys.ranges`: rel oid, lo, owner core, entry page; hi is the next row's lo, and a **non-empty directory carries a row at lo = 0**, so the rows partition the whole id space) records every split; a relation with no rows there is one range owned by `sys.tables.owner_core`. Resolved at plan time from the session core's catalog cache; a remote stage trusts its descriptor and does not re-resolve; staleness is a retryable step error (§5's rule, unchanged). Split/migration broadcasts ride `kCatalogInvalidate` as DDL does (§2a names the cache-generation prerequisite) |
| CC10 | Range split & migration | **Split at a page boundary only** — `min_key` is the split key, so no page is ever divided and invariants 2/3 hold by construction; §6b's block-aligned insert split satisfies this vacuously, because the new range starts as its own empty sub-structure (CC8) and no existing page straddles it. **Migration**: the page-level contract is the ratified PL-B handoff (`page-lsn-cross-stream.md` §9); the range-level sequence *around* it is this spec's, and its ordering is a correctness statement — (0) the outgoing owner quiesces the range (in-flight stages finish or cancel; new plans against it surface as §5's stale retryable step error), (1) flush, (2) durable handoff record, (3) **durable directory row before any grant** — the row is a catalog write in core 0's stream, synced before step 4 — (4) grant, (5) invalidation broadcast; the incoming core's first write stamps its stream per PL-C. **A crash before step 4 aborts the migration to the outgoing owner at mount**, sound precisely because the grant is last: the incoming core has written nothing. PL §9 governs the record/redo/stamp halves; steps 0, 3 and 5 are outside its five rules and are owned here, not cited to it. Advisory-reference retirement is priced, not free: a Waystone trail replayed against a moved range misses on the epoch/owner check and falls through, but bumping `relayout_epoch` costs one write per moved page — whether the bump or the owner check alone retires stale trails is R5's to settle against the mover's flush-per-move cost. **Cabin does not self-heal**: its hint miss resolves through the pk *on the same core*, and its entry sets are memory-resident where they were observed — so a cabined relation is gated for migration exactly as for split (§6a). The mover is the physical optimizer's Part III and inherits Part I's discipline (observe, decide, report through SHOW, enact through named gates); its policy and constants stay `[OPEN]` (§9). Until the owning docs decide auxiliary placement, split is **gated** (§6a) |
| CC11 | Shared-structure access (operator-decided 2026-08-30) | **Every core reads with the same authority; core 0 alone writes.** The rule covers the three fixed system structures blueprint §8 named as one question — the **superblock** (page 0), the **free map** bitmap pages of every region, and the **catalog** pages. §8's two candidates are **both declined**: no partition-boundary lock is taken on any of them (`rules.md` §3's last-resort clause is not invoked, and no subsystem header gains an acquisition order), and no rotating coordinator is built. **Why one writer rather than equal writers**: the single writer *is* the serialization mechanism and it costs nothing — total order over catalog change falls out of one stream, which is what makes `BumpVersion` plus an invalidation broadcast sufficient; N writers would need a lock ordering, a coordinator (core 0 again) or version vectors, none of which exists; and equal write authority would let one DDL span two WAL streams, the shape `cross-owner-txn.md` exists to handle and which nothing should acquire by accident. The argument equality would have won on — read scalability — is answered by the read half: a peer's fill path is the **device**, not a message to core 0 (catalog frames are the authority and the cache is the memo, `src/server/core_runtime.cpp:928-932`, so a peer's cache miss re-reads the pages off the device rather than asking core 0; the free map is re-read by `RefreshFreeMapFromDevice`, which `InvalidateCatalog` runs **first**, at `:919-920`, so a peer can reach a page core 0 has just allocated). **The rule is already the store's enforced contract, not a thing to build**: `DevicePageStore::MayFault` admits the whole system range on a leased store — *"the fixed system range is readable by every core: the catalog lives there, and a core that cannot read it cannot serve a statement"* (`src/storage/device_page_store.cpp:651-656`) — `MayWrite` refuses that range on a leased store in **every** build (`:804-810`), `ResidentBytes` answers it `InvalidArgument` rather than a retryable status because a peer asking for a system page is wrong on every retry (`:479-489`), and `FlushMaps` refuses the map write-back on a leased store, being the one write path that never asks `MayWrite` (`:286-299`). Core 0 holds no lease, so nothing gates it. **What stays core 0's by the same decision, stated so "every core equivalent" is not read wider than it is true**: file growth, extent leasing, and free-map region creation — a leased store may not place a page at a chosen id at all (`CreateAtUnpinned`), because that is a claim on the free map. **What follows for DDL**: a peer runs DDL by **shipping the statement to core 0**, never by writing catalog pages, so PW4's refusal is to become unreachable rather than removed (blueprint §11's R1). **What this does not decide**: a *relation's* shared structure — the btree's top levels under a split relation, whose writer is the root's owner core and never core 0 — which keeps its own open item in §9 |
| CC12 | Catalog page placement, and DDL's route (operator-ratified 2026-08-31, `instructions/v2.7.0/r1-catalog-placement-ratification.md`) | **A catalog relation's root page stays in the reserved range; its var-heap does not; and a grown catalog page may leave the range on the ordinary relation rules.** Three parts, and CC11 is what makes them necessary rather than optional. **CR1 — the roots stay reserved.** `kCatalogPageTypes = 4` through `kCatalogPageRanges = 15` (`include/kds/catalog/well_known.hpp:271-299`) sit below `kFirstUserPageId = 128` for the reason those constants already give: they must be findable at bootstrap *without* a catalog read. Their **var-heap** roots do not: `sys.assertions` allocates its through `CreateNew()` and records it in `sys.tables` (`sys.pattern_defs` did the same until 2026-08-31), and CR1 ratifies that as intended rather than incidental, binding on any catalog relation that later gains a var-heap. The cost is stated rather than discovered: a page outside the range is not peer-readable through `MayFault`'s `page_id < system_page_limit_` arm (`src/storage/device_page_store.cpp:656`), so today's one peer-readable catalog var-heap — `sys.assertions` — is reached by granting the individual pages a row names (`exec::CatalogSpillPages`, `src/server/core_runtime.cpp`), deliberately **not** an extent grant, which would cover pages that core owns and cost it PW1c-7's stamp-claimed write rights. That page-at-a-time grant is **not** ratified as the general mechanism; CR3 is. **Amended 2026-08-31**: `sys.pattern_defs` was the second such relation and is where CB0-CB3 built the grant — over a named list, `exec::kVarHeapCatalogRelations`, with the walk merged into `exec::CatalogSpillPages` so the sweep and the grant share it — and the operator then withdrew user-declared patterns, which removed the relation and left the list holding `sys.assertions` alone. So CR1 is built and exercised, the mechanism is a list of one and stays a list, and what remains unexercised is CR3's **format change**: CB1 established that every catalog var-heap read is DDL- or `SHOW`-frequency rather than per statement, so a reserved sub-range was never needed and stays available. **CR2 — DDL executes on core 0; a peer sends and waits.** A peer does not execute DDL: it ships the request to core 0 and waits for the outcome, so **PW4's refusal stays necessary** (`PeerDdlRefused`, `src/server/command_dispatcher.cpp:918`) and is not retired. This is the deliberate opposite of blueprint §8's second item, whose wording had the refusal *"become unnecessary rather than unbuilt"* — CR2 keeps the refusal and gives DDL a route around it. Two things it does **not** decide, and neither may be assumed: the **wire form** (whether DDL rides DML's `session_step_client`/`remote_step_service` path or gets its own request kind), and the **reply's failure mode** (a shipped reply over 992 bytes reports `UNKNOWN_OUTCOME`, which `known-gaps.md` already calls the wrong thing to tell a client about a statement with no effect — a DDL *error message* carries a byte position and a relation name and is not obviously bounded, so size it before building). Both are indexed in §9. **CR3 — a catalog page may leave the reserved range once grown.** Catalog pages are allocated and initialised inside the range at bootstrap; beyond bootstrap a catalog page **may be managed outside it** in either of two cases — the relation grows past what the reserved range holds, or every peer must read *and write* the page equally. Such a page then takes the ordinary relation rules: allocation from the general supply, free-map accounting, extent leases, and the WAL logging RV3 already established for catalog change. **The problem CR3 was raised against**: `sys.access_stats` is pinned at `kCatalogPageAccessStats = 11`, inside the range, so under CR2's one-writer rule a peer could not record an access at all — and `Catalog::RecordAccess` (`src/catalog/catalog.cpp:2816`) runs per *statement*, not per DDL. **CR3 is not the route taken for that one** (see CC13/CR7: the peer batches and core 0 applies, page 11 unmoved); CR3 is the standing permission, and it has **no** exercise: CB0-CB3 answered `sys.pattern_defs`' var-heap with CR1's page-at-a-time grant instead, and that relation is gone. **Owed by whoever builds it**: the roots' justification is bootstrap findability, and a migrated page is found through `sys.tables` instead — the indirection `varheap_page_id` already uses and which `rows.hpp` calls DDL-immutable and therefore cacheable. Whether that holds for a **heap** page rather than a var-heap root is *not* established here; establish it from source before the first relation moves |
| CC13 | DDL's route, and how a peer's statistics reach core 0 (operator-ratified 2026-08-31, CR5-CR8, `instructions/v2.7.0/cb-catalog-placement-buildout.md`) | The addendum to CC12, and the half that changes behaviour a client can see. **CR5 — a peer routes DDL to core 0 rather than refusing it.** The detection half already exists at dispatch (`src/server/command_dispatcher.cpp:909`, keyed on `catalog_read_only_` and the `CREATE`/`ALTER`/`DROP` tokens, deliberately the same token the routing below it reads *"so the two cannot disagree about what a verb is"*); CR5 changes what follows the match from `ErrorReply(PeerDdlRefused)` to a ship to core 0, and the peer waits for the outcome. `PeerDdlRefused` **stays reachable**, because the refusal survives for the cases the build does not cover — and its own comment's warning that it is the only guard under NDEBUG stands for those. **CR6 — a catalog relation whose content is only a statistic is written unlogged**, the sole exception to RV3's rule that catalog writes are WAL-logged and replayed (`ddl-transactional.md` §7); the rule text lives in `docs/rules/rules.md` §5 rather than in the code that takes the exception. **CR7 — a peer batches statistics locally and flushes periodically**, never one message per statement, and **`sys.access_stats` stays one relation at page 11, core-0-written**: per-core *relations* are declined, and with them the oid allocation, row migration and core-count questions such a relation would open. **What per-core attribution means here, stated so it is not read wider than it is true**: the batch carries `core_id` on the wire, and `SHOW META` can account by it, but `SysAccessStatRow` is a 33-byte fixed row with no field for it and no spare (`catalog/rows.hpp`), so a peer's counts **fold into the existing `(kind, rel_id, column_mask)` shape** at rest. Storing the core would be an on-disk format change to a catalog relation, which CB's scope excludes; what R5 is owed and now gets is that a peer's accesses are counted **at all**, which is what §6a asks for (that last one would have added a dependant to `wal.md`'s open "how" for a changed core count, which this route does not). **CR8 — a full ring drops the batch**, which invariant 8 already prices as performance and never a result, so a drop is a permitted outcome rather than an error to report; it increments an observability counter so that it is visible in `SHOW META` rather than silent. **Constants are not ratified here** — batch size and flush interval are measured (CB7), and the ceiling to beat is `RecordAccess`'s present +1-2% on a point lookup (`heap-and-tuple.md` §7), with what the batch adds to *core 0's* load reported beside what it saves the peer |

## 2. Execution Model

The session core owns the statement end to end: it parses, resolves the step
chain (the written-order contract of `docs/spec/parser-v2.md` — *the statement is
the chain*, never silently reordered), and looks up each
step's owner core from its catalog cache (`owner_core`, multicore-workplan
M1).

**Amended 2026-08-24 (v2, range granularity)** — "relation" became
"range(s)" in both paths below (the session core resolves each step's
ranges against the directory, §2a, at plan time), and one honesty note:
a step spanning k remote ranges is a **fan-in the built message set
cannot yet express**. The tag `(session_core, request_id, step_id)`
cannot name k sibling stages of one step; the session ends a read at
the *first* `STEP_EOF`; the per-edge `seq` gap check assumes one
producer per tag; and the chained-open rule below has no fan-in form —
so R3's pipeline-over-ranges is new message work (sibling identity in
the tag, k-EOF accounting, per-sibling edges and credits, a fan-in
open), not a loop over today's opens. Range-order concatenation of
stage outputs is *deterministic* because ranges are disjoint and
key-ordered, but it is not free: when the statement requires key order,
later ranges buffer until earlier ranges finish. (`emit_in_key_order`
is not this mechanism — it is a per-step page-local ordering flag that
is an explicit *shipping refusal* in both built paths today.)

Two paths:

- **Local fast path.** Every referenced range is owned by the session
  core. Execution is exactly today's single-core code — the cross-core layer
  must add zero work here. This is an invariant, not an optimization note:
  the single-core path must not regress in instructions or allocations.
- **Pipeline path.** At least one step's range lives on another core.
  Each remote step receives a `STEP_OPEN` describing it (relation,
  predicate bindings, projection column set, downstream target), wiring
  step k's output to step k+1's core. **Amended 2026-08-14 (P4d-4b fact
  1): the opens are chained, not fanned out.** The session core opens
  only the *final* stage; every stage's envelope encloses its upstream
  stage's complete open, which the receiving core forwards once its own
  pipeline state exists. That ordering is what makes "no batch before
  its consumer" structural - §3's teardown rule silently discards an
  unmatched batch, so two independently raced opens would lose rows, not
  fail. The last step's downstream is the session core, which frames
  rows to the client (KWP, protocol D6 chunked streaming); CANCEL stays
  point-to-point from the session, which knows every stage's core from
  its own plan.

What flows between steps is not whole rows: step k forwards, per row, the
join key consumed by step k+1 plus only the columns the final projection
needs from step k's relation. Step k+1 performs its lookup (pk descent,
Waystone/Cabin hint, or scan per the plan) against its **local** state with
its **local** trail/statistics recording. **Waystone trails do not cross
cores; access statistics have since 2026-08-31** (CC13/CR7) — a peer folds
its access shapes locally and flushes them to core 0, which owns the one
`sys.access_stats`. The sentence used to read "no statistics cross cores"
and is narrowed rather than deleted, because the trail half of it still
holds and for its own reason: `RegisterPattern` hands the recorder a
pointer it uses immediately, so it needs an answer, and a stage cannot wait
for one.

A pipeline is torn down when the session core has framed the final row,
received `STEP_ERROR`, or issued `STEP_CANCEL` (§7).

## 2a. Ranges — the Unit, the Directory, the Routing (v2, 2026-08-24)

**The unit** is CC8's; **the directory** is CC9's `sys.ranges`,
resolved as CC9 states (plan time, the session core's cache, staleness
a retryable step error). Two rules beyond CC9's cell:

- **The directory is read-mostly.** Splits and migrations are rare,
  DDL-frequency events; the per-statement path reads the per-core cached
  copy. A split/migration broadcast rides `kCatalogInvalidate` as DDL
  does. **Prerequisite, named**: `InvalidateFromPeer()` clears a peer's
  cache without bumping `catalog_version()` — deliberately, that counter
  is per-instance — so any range fact cached across a suspension and
  guarded by that counter is wrong on every peer. The cache-generation
  counter every invalidation path bumps (`docs/inflight/known-gaps.md`, named
  2026-08-15) must exist before any code caches a resolved range across
  a park.
- **The fast-path invariant binds here hardest**: a one-range relation
  on its owner core must add zero instructions over today (CC1, §2).

**Routing a predicate to ranges.**

- A pk equality or pk range names its range(s) arithmetically against
  the directory — no structure consulted, no broadcast.
- A non-pk *read* predicate names none; the default is every range of
  the relation, and that fan-out is cut only by the engine's own
  structures under their existing authority rules, never by a new one:
  a secondary-index probe's answer names its own destination — the
  entry is `key || pk || covered`, so the pk it returns is the routing
  key (whether the index itself is per-range or global is `[OPEN]`,
  `docs/spec/index.md` §13); Cabin answers "which range holds value V"
  for an observed key under its own banked-authority rules
  (`docs/spec/cabin.md` §4a, §6a); a Waystone trail names pages, a page
  names a range, a range names a core — advisory per invariant 9, and a
  trail replayed on the wrong core after a migration misses on the
  epoch/owner check and falls through, the ordinary miss discipline.
- **Advisory structures never narrow a write's target set.** Invariant
  9's license is *where to look*; for a write, executing on fewer
  ranges than hold matching rows is a missed write, not a slower one.
  DML target resolution is pk arithmetic against the directory alone
  (§6).
- **In the first build the cutters are absent by construction.** §6a
  lets only unindexed, un-cabined relations split, so until those gates
  lift, every non-pk read predicate on a split relation broadcasts to
  all k ranges. R3's measurement will find exactly that; it is the
  stated cost of the gating discipline, not a surprise.

## 3. Messages

All pipeline traffic rides the per-core-pair SPSC rings (`docs/spec/sched.md` §5).
Message kinds:

| Kind | Direction | Payload |
|------|-----------|---------|
| `STEP_OPEN` | downstream stage → its upstream's core (the session opens the final stage; amended 2026-08-14, §2) | step descriptor: relation oid, bindings, projection set, downstream core+step; optional upstream section (forwarded-row layout, enclosed upstream open); optional output spec — **standalone, beside the upstream section, since 2026-08-15 (P4d-4b-3)**, because a *leaf* seals its consumer's input layout too; absent = whole row. The head's `downstream_step` is read at the consuming stage: the enclosed open must address the stage that forwards it, or the open is refused |
| `STEP_BATCH` | step k → step k+1 (or session) | chunk of KWP-encoded rows (§4) |
| `STEP_EOF` | upstream → downstream | no more batches for this step |
| `STEP_CREDIT` | downstream → upstream | grants N batch credits (§4) |
| `STEP_CANCEL` | any → any in pipeline | stop producing/consuming; discard tagged state |
| `STEP_ERROR` | failing core → downstream chain + session | Status code + retryable flag (protocol D9 mapping) |

Every message carries the tag `(session_core, request_id, step_id)`.
`request_id` is allocated per statement by the session core, sequential per
core (never pointer-derived — `docs/spec/sched.md` §7 determinism rules). A core
receiving a batch whose tag matches no live pipeline state discards it
silently; this is the teardown correctness rule, not an error.

## 4. Transfer Format and Flow Control

- A `STEP_BATCH` payload is rows in the **KWP binary encoding (protocol
  D5)** — the same encoder the wire path uses, applied to the forwarded
  column set. One encoder, two consumers; no second row format.
  **That encoder does not exist yet**: `include/kds/wire/kwp.hpp` is the
  frame codec alone, and the server still speaks the newline text protocol.
  Settled 2026-08-04: the D5 row encoder is a **prerequisite of P4**, built
  in `wire/` where both consumers reach it. An interim private batch format
  is refused — it would be exactly the second row format this bullet
  forbids, and the one that is hardest to remove later because a pipeline
  would be written against it.
- Batch size: `[PROPOSED]` 32 KiB target, always ≤ the ring's max message
  payload. A row larger than the target still ships alone (var-heap spill
  values are re-inlined into the batch by the producing step — the consumer
  never chases a var-heap reference into a page it does not own).
- **Credit-based flow control**, separate from ring backpressure: a
  downstream step grants `STEP_CREDIT` as it drains; an upstream step never
  sends a batch without holding a credit. Initial credit `[PROPOSED]` 4
  batches per edge. Ring-full on send follows the global rule
  (multicore-workplan M7): the sending task yields and retries; it never
  blocks the reactor and never drops.
- Rationale: ring backpressure protects the *transport*; credits bound the
  *per-request* buffering so one fat pipeline cannot exhaust a peer core's
  batch memory. Credit memory is preallocated per edge at `STEP_OPEN`.
- **A successful send wakes a sleeping destination; a refused one wakes
  nobody** (2026-08-26, `docs/spec/sched.md` §7 and its invariant 7). This
  is the half of backpressure that used to be missing rather than a new
  rule: the send stays non-blocking and fallible, and the wake follows the
  push, so a message never waits out the destination's idle block. The
  refused case is deliberate and tested — waking a core for a message that
  is not in the ring is the spin the wake exists to remove, moved to the
  sender. What it was worth is in `bench/v2.3.0/`: a cross-core round trip
  on an idle peer cost a flat ~1.06 ms before it and **20.0 µs** after,
  independent of `wal_drain_interval_us` over a 50× range.

## 5. Isolation Semantics

There is no cross-core ReadView. A remote step reads what is committed on
its own core (`docs/spec/txn.md` visibility with an empty live-set view; the
trx-id domain is global, so ids compare cleanly).

**Amended 2026-08-15 (P4d-4c's review), and the amendment is a
tightening.** This section used to say "at the moment it produces each
batch". The built form mints **one view per stage**, when the stage's
coroutine first runs, and holds it for that stage's whole life — because
re-minting per batch is not a weaker promise but a wrong one: a stage
parks mid-relation, and a view that moved across the park could show the
same row twice or skip it entirely, depending on which side of the
boundary a concurrent commit landed. One view per stage is what makes a
stage's output one statement's answer. Two consequences worth stating:
the window between `STEP_OPEN` and that first poll is not covered, so a
transaction committing inside it *is* visible where a local statement's
view would have excluded it; and each stage of a multi-stage pipeline
mints its own, so two stages of one statement can disagree about a
concurrent commit. **[Retracted in part 2026-08-24 (v2): for two stages
on *one* core that sentence understated the exposure — it is a torn
read of a same-core transaction, a wrong answer, not a documented
weakening; the amendment below states the rule and its gate. Across
cores the weakening reading stands.]** Both sit inside the per-core
weakening below.

**Amended 2026-08-24 (v2), and the defect the new rule closes is not
range-introduced — it is latent in the shipped shape.** Two stages of
one statement already land on the *same* peer core whenever that core
owns both relations (each stage's core is resolved independently from
its relation's `owner_core`), and each mints its own view; a
transaction on that core writing both relations — which CC3 permits,
one home core — and committing between the two mints is observed torn.
Unreachable today for exactly one reason: no core but the session core
can commit while a pipeline is live, because peers take no writes until
the PW1c peer writer lands. Range ownership then widens the same defect
into *single-relation* statements — a scan spanning k ranges is k
stages. Two rules:

- **No transaction is ever observed torn across cores.** CC3 binds a
  transaction's writes to one core, so every transaction's writes lie
  in one stream and no cross-core pair of views can split them.
- **One view per (statement, core)** — a v2 rule this revision adds,
  because the blueprint's "CC4 unchanged" was insufficient: **all
  execution of one statement on one core — every stage *and* the
  session core's own local half of a mixed plan — shares that core's
  view, minted at the first execution to poll.** Core-local, so no view
  crosses a core and CC4's sentence stays true. **Gate: the peer
  writer** (`docs/inflight/in-progress/workplan-peer-writer.md` PW1c-5/PW5 — the rule must
  hold before any core other than the session core can commit while a
  pipeline is live), which precedes R3; a pipeline over a split
  relation inherits it.

The RR weakening reads the same one level down: RR guarantees hold per
core, and a cross-range read of one relation is a cross-core read. The
client manual states the widened form beside the v1 one.

- **READ COMMITTED** statements: semantically equivalent to local execution —
  RC already permits each statement (and each lookup within it) to observe
  the latest committed state.
- **REPEATABLE READ** transactions issuing cross-core reads: **amended
  2026-08-28, and the `[OPEN]` closes.** The weakening is now stated
  positively rather than as an absence — a cross-owner RR transaction sees
  a **consistent-per-core** snapshot: each participant pins one view for
  the transaction's life and the coordinator carries that participant's
  watermark, which is compared with nothing on any other core.
  `docs/spec/cross-owner-txn.md` §3 owns the rule and
  `docs/spec/client-manual.md` states it in the client's words. What is
  **not** given, and never will be from a shared-nothing engine, is a
  single global instant: two such transactions can disagree about the order
  of two commits on two cores.

  **The remote-step pipeline does not run inside such a transaction.** It
  reads each core's latest-committed snapshot outside any enrolled
  transaction, which inside a cross-owner transaction is not a weakening
  but a **wrong answer** — that transaction's own writes on the owner live
  in the transaction the owner holds for it, and no view this leg can take
  shows them. So both remote-read fast paths are skipped for a session that
  can enrol, and the read ships instead. The pipeline keeps every autocommit
  route and every measurement made on one; what it costs a cross-owner
  transaction is the shipped reply's one-slot cap — 992 bytes of reply text
  — which refuses a larger answer rather than truncating it or answering it
  wrongly (§9's ring sizing is what lifts that).
- Catalog: the plan is resolved entirely on the session core from its
  catalog cache; a remote step trusts the descriptor in `STEP_OPEN` and does
  not re-resolve. DDL invalidation between resolve and execute surfaces as a
  normal step error (stale oid → `STEP_ERROR`, retryable).

## 6. Writes (v1 Restriction, range-widened in v2)

**Revised 2026-08-24 (v2): the shipping unit and the refusal are per
range.** On a one-range relation every rule below reads exactly as it
did.

- A single DML statement is shipped **whole** to the core owning its
  target range and executes there under that core's transaction
  machinery — this is statement shipping, already implied by protocol
  D3, and involves no pipeline. **Built 2026-08-26** for the
  one-range case (SS1–SS4 of the statement-shipping work order): an
  **autocommit, single-relation** statement — read or write — whose
  relation another core owns is carried to that core as *text*, parsed
  and bound there against the owner's own catalog, executed under the
  owner's ordinary local implicit transaction, and committed through the
  owner's group committer, which is the whole performance argument
  (`docs/inflight/in-progress/memo-shipping-and-group-commit.md` §3). The arrival core parks a
  waiter under a deadline and answers with the owner's own reply, the
  `retryable` bit included.

  **The refusal list, third era (2026-08-28).** It was three things, and
  the first of them is gone: a statement **inside an explicit
  transaction** now ships and *enrols*, for writes since R6-8 and for
  reads since RR1 — `docs/spec/cross-owner-txn.md`. Two stay, and each is
  a scope statement rather than a gap: a statement **spanning two owners**
  (a multi-owner *statement*, which is not the same thing as a
  multi-owner *transaction* and is not what the commit protocol
  addresses), and a statement on a path that **cannot park** — the
  synchronous dispatch entry, because sending from a path that cannot wait
  would leave a statement the owner may have committed with nowhere to
  deliver its answer. `cross_core_write_refusals` counts what remains,
  with its meaning unchanged across all three eras, which is what lets the
  series be read end to end. Shipping is **unconditional** where it
  applies: whether to ship or to refuse by load is placement policy, which
  is §9's open decision and does not ride along.

  A lost or late answer is **not** a refusal. It is `UNKNOWN_OUTCOME`,
  non-retryable by construction, because this engine issues primary keys
  and a blind retry of a statement that may have committed inserts a
  second row. The owner keeps a bounded per-(arrival core, session)
  record of what it last answered — and of what it is still running — so
  a duplicate is answered from the record rather than executed twice.

  **Target resolution is pk arithmetic against the directory alone**
  (§2a): a DML on a split relation whose predicate does not bound its rows
  to one owned range is a cross-core write, refused retryably until 2PC —
  the widened CC3 refusal, stated rather than hidden.
- An explicit transaction acquires a **home core** at its first write
  (the owner of the written range). Any later write targeting a range
  owned by a different core fails with a retryable conflict error
  (protocol D9; same client contract as first-updater-wins aborts in
  `docs/spec/txn.md`) — **from a session on the home core**. On a *peer
  listener* the shipped guard refuses every write `Unsupported` before
  the relation is parsed (`docs/inflight/in-progress/workplan-peer-writer.md` §8's recorded
  cost) — honest on a session that can never succeed by retrying, and a
  different client contract than this bullet's, stated so §6 is not
  read as promising a retryability a peer session does not get. Writes
  to any ranges the home core owns — of one
  relation or several — stay legal: they are single-stream, and
  nothing 2PC-shaped is in them (§5's shared statement view is the
  reader-side half of that claim). Reads inside the transaction remain
  free to pipeline cross-core under §5.
- Every rejected cross-core write increments a per-core observability
  counter keyed by (home core, target core, relation) — the input the
  future placement/2PC decision will be made from. **Its meaning is
  unchanged by shipping and that is deliberate** (2026-08-26): before
  shipping it counted the whole demand; after it counts the *residue* —
  the writes shipping does not convert, which is exactly the
  multi-owner and in-transaction population a 2PC decision would be made
  about. What shipping converts is counted separately, by the
  `shipped_*` fields below. Counters are metrics,
  not stored state. **Exposed since 2026-08-26** (T5 of the
  statement-shipping pretasks): `SHOW META` prints
  `cross_core_write_refusals`, `cross_core_write_refusal_keys` and a
  capped `cross_core_write_refusal_detail` of `home>target:oid=count`.
  The recording sites predate it (`CheckWriteAffinity`'s two arms); what
  was missing was any way to read them from outside the process, which is
  the whole of what a metric is for. The counter is **core-local**, so a
  total is one reading per core.
  Two v2 notes: whether the key gains the range boundary is a workplan
  detail, not a decision; and **the undercount this bullet used to claim
  is retired**. It said PW5's peer-listener guard refuses foreign writes
  before the relation is parsed, so those refusals never reach the
  counters — that guard (`PeerWriteRefused`) was **deleted at PW1c-5**
  (`docs/inflight/in-progress/workplan-peer-writer.md` PW1c-5, 2026-08-24: *"a foreign write
  on a peer flows to `CheckWriteAffinity` again … and the §6 counters see
  it, reversing PW5's recorded undercount"*), and the passage here simply
  outlived it. What the counter genuinely cannot see today, stated at the
  print site as well: **DDL on a peer** (`PeerDdlRefused`, refused by verb
  before any relation is resolved) and anything refused before resolution
  at all. The two owner-core refusals — `RelationWriteRightsPending` and
  `IndexBuildPending` — are excluded **by decision**, not by oversight:
  the write is not cross-core, it is this core's own write waiting on a
  grant or a build window, and counting it would inflate the 2PC evidence
  with cases 2PC does not address.
- Write-coupled auxiliary placement is §6a's. The v1 sentence — unique
  indexes, Cabin, Waystone pages, and the var-heap live on the
  relation's owner core, always — is a statement about a one-range
  relation and survives as §6a's degenerate case. Read-only join
  partners may live anywhere.
- FK (`docs/spec/foreign-keys.md`) stays co-located: RESTRICT validation is a
  read, but its validation-to-commit window is only sound against the local
  latest-committed state; cross-core FK inherits the §5 weakening and is
  deferred with 2PC `[OPEN]`. A split parent or child would make the
  validation cross-core, which is §6a's FK gate.

### 6a. Write-Coupled Auxiliaries — What May Split (v2)

A split relation has no single owner core, so the v1 co-location rule
does not survive a split as written. Each auxiliary's placement under a
split belongs to its owning doc; none has decided; and until one does,
**the conservative gate is that the relation does not split**. There
is no user-facing range DDL, in this phase or any later one — a range
is information the user does not have; the system allocates and
enforces it (operator direction 2026-08-27, retracting the
`SPLIT RANGE` working name blueprint R3 carried). The gates bind
wherever a range would be created — today the only such path is the
range allocator's admission check
(`docs/inflight/in-progress/workplan-range-directory.md` RD4), and any
later creation path inherits them, the mover's included (CC10). A gated
relation is declined as a logged engine decision naming the gate — no
statement asks for the range, so no offending token and no byte
position — which keeps every listed option viable. Where the decline
is *read* was decided at RA4 (2026-08-27, `workplan-range-directory.md`
§9e, made without operator input): the caller logs the line and
increments per-core decline counters in §6's `SHOW META`
refusal-counter form, both landing with RD5's first caller — nothing
lands before a caller exists (the absent-rather-than-zeroed rule), and
the counter is owner-core-local because only the owner's registry can
answer the assertion gate. The gates below bind **split**;
whole-relation **migration** moves everything together and preserves
co-location, so only the Cabin gate (its state is
memory-resident on the outgoing core, and its miss path does not
self-heal — CC10) binds both:

- **Secondary indexes** — per-range local vs global is `[OPEN]`
  (`docs/spec/index.md` §13; reading on record: local per range, cut by
  Cabin/Waystone — not ratified). Uniqueness enforcement under either
  shape is part of that decision. Gate: an indexed relation does not
  split.
- **Cabin** — entry sets are memory-resident and observed per core, and
  the hint-miss fall-back resolves through the pk on the same core
  (CC10); a split or moved relation's observation and banked-authority
  story belongs to `docs/spec/cabin.md` §11. Gate: a cabined relation
  does not split **or migrate**.
- **Var-heap** — one `kVarHeap` page may hold spilled values referenced
  from tuples on both sides of a boundary, a core faults only pages it
  owns, and invariant 14 stands (values immutable per version, pages
  never relocated). Gate: a relation whose schema can spill does not
  split until var-heap partition is designed (owner:
  `docs/spec/heap-and-tuple.md`).
- **Foreign keys** — gate: an FK parent or child does not split
  (`docs/spec/foreign-keys.md`; the §6 bullet above says why).
- **Assertions** — the fifth gate, found by RD4's C2 enumeration
  (2026-08-27, `workplan-range-directory.md` §9) rather than carried from
  v1, which is why this bullet is younger than its four siblings. An
  assertion's Bound Cabin is owner-built from the owner's own lease and
  **appended to by every write to the relation** (PW1c-6c,
  `workplan-peer-writer.md` §7d), its live directory is memory-resident
  on that one core, and its aggregate is keyed on group columns that
  need not include the pk (`ResolveAssertionColumns` refuses none) — so a
  group's rows straddle any range boundary, a second
  owner core's appends hit `MayWrite`'s refusal, and a core whose
  registry never heard of the assertion admits the write **unchecked**
  (the Finding 2 failure,
  `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md`). Gate: an
  asserted relation — one the owner's registry holds live (`AnyOn`) *or*
  knows and cannot enforce (`CannotEnforce`) — does not split **or
  migrate**; the migration half for Cabin's reason in stronger form,
  since a mid-run move leaves the incoming owner's registry empty and
  emptiness admits silently. The fact deliberately does not ride
  `TableAccess`: CREATE/DROP ASSERTION do no version bump
  (`assertion_catalog.cpp`'s publish comment — nothing cached is derived
  from a `sys.assertions` row), so a cached bit would be stale by
  construction, and the gate function therefore takes the enforcer, not
  the access struct alone.
- **Waystone and statistics** — advisory (invariant 8): recording stays
  per owning core, a stale trail misses and falls through, and **no
  gate is needed** — worst case the trail is deleted, which invariant 8
  prices as performance, never a result. Per-core statistics relations
  are a prerequisite of the mover (R5), not of correctness — and since
  2026-08-31 the requirement has a **mechanism** rather than only a
  name. `sys.access_stats` is pinned inside the reserved range, which
  CC11 makes core-0-write-only, so a peer had nowhere to record. The
  route taken is **CC13**/CR7: the peer batches locally and flushes to
  core 0, which applies the batch to the one relation at page 11, each
  entry carrying its `core_id`. **Per-core statistics *relations* were
  declined** — the attribution is what R5 needs, not the split — so
  "per-core statistics" now means per-core *rows*, not per-core
  relations. It is R5's gate (CB7), not R1's item (blueprint §11).

What remains splittable in the first build — non-spilling
(`SchemaCanSpill` false; invariant 13 makes *every* relation
fixed-length, so the spill is the gate), unindexed, un-cabined, FK-free,
**un-asserted** relations — is narrow and real. The gates are lifted by
the owning decisions, never by relaxing the decline.

### 6b. Inserts and the Tail — Id-Block-Aligned Spreading (v2, R4)

**Built 2026-08-29** (R4/IS1-IS5,
`docs/inflight/in-progress/workplan-insert-spreading.md`) and **armed by
default 2026-08-31** (**DA1**, `instructions/v2.7.0/ratification-da.md`):
`range_size_ids` was `kRangeSizeOff` while the operator took its value on
RD9(b)'s sweep, and the value taken is **65,536**
(`server/range_alloc.hpp`'s `kRangeSizeIdsDefault` carries the numbers).
`kRangeSizeOff` stays the off-switch. What R3 left and R4 supplies is a
*producer* — a core that does not own a relation now records row-id lease
demand before giving a foreign INSERT away, which is what makes core 0
open a range owned by that core — plus the routing over it: **a write is
routed by the id it will issue, not by which relation the statement
names.** Two limits ride with it and are §3 and §7b of that workplan: a
read of a spread relation is bounded by the fan-in's stage ceiling
(**255** since **DA3**, 64 before it), and a write naming no primary key
on a *multi-owner* relation is refused until R6.

**What arming by default changes is which instances meet those two
limits**, not the limits. A range opens only where a core that does not
own the relation asks for a block, and the suppression in
`OpenRangeOnSystemCore` declines whenever the asking core already owns the
top range — so a **single-core instance opens no range at any size**, and
neither does a relation only one peer ever writes (§6's HK4). The two
limits are reachable from the second contending writer on, which before
DA1 needed a configuration and now needs only a workload.

**The first of those two was corrected by measurement and has since been
fixed** — both halves are recorded here because the intermediate reading
is quoted in results files that stay as history.

**What R4-M found** (2026-08-29, worktree `v2.6.0-ksweep` at `03b815b`;
`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md` §6a,
and `workplan-insert-spreading.md` §3a): the 64-stage ceiling is real but
was **not what bounded the read**. Two limits sat in front of it. The
fan-in *client* was constructed for core 0 alone (a `CoreRuntime` peer had
`remote_steps_`, the server half, and no client), and the route
additionally required the reader not to be the relation's `owner_core` —
which under `placement = creating` is core 0 for every relation. So a
spread relation was unreadable from **every** core in **every** shape from
its second range on.

**Both are gone** (R4-R and RS, 2026-08-29, `workplan-insert-spreading.md`
§10–§11). Every core constructs a `SessionStepClient`, and the route asks
`TableAccess::ServableBy(core)` — *can a walk on this core alone answer
this relation whole* — so a run of ranges the reader owns becomes a
**self-directed stage**: the ordinary protocol with the ring hop being a
self-send, costing an upstream slot like any other stage.

**The surface that leaves** is enumerated rather than claimed
(`bench/spread_read_surface.py`; at `3446666`,
`bench/v2.6.0/results-ag3-read-surface-v2.2.1-140-g3446666.md`, `cores =
2`, `placement = creating`): **11 shapes from every core** — `SELECT *`
with an optional `WHERE`, `BETWEEN`, a free `ORDER BY <pk> ASC`, any
projection, and every aggregate, the last two since AG3
(`workplan-insert-spreading.md` §12d). **5 refused**: `LIMIT`/`OFFSET` and
any `ORDER BY` but the pk ascending, both because a quota and a sort apply
at **emission** while the remote side emits everything in its own order. A
join is not on that list: it is the two-step pipeline planning a spread
relation as a stage, which is unbuilt. The route is also refused **inside
an explicit transaction**, because each stage mints its own
latest-committed view — snapshot forwarding is §9's.

So the 64-stage ceiling in §3 is once again the binding limit on a read,
which is what it was priced as before R4-M found two limits in front of it.

When an `INSERT` omits its key the engine issues an ascending one, so
every such INSERT targets the relation's
maximum id — the tail range — and naive range ownership spreads reads
while leaving inserts single-core, which for insert-heavy OLTP concedes
the headline number. The answer is built from the row-id block leases
that already exist (`catalog::RowIdLeaseTable`, demand-driven per
PW1b): each core inserts from its own leased id block, and **ranges
align to block boundaries**, so every core appends to its own range's
tail, fully locally. Invariant 3 is satisfied per range because each
range is **its own chain** with its own tail — the per-range
sub-structures CC8 names, which are the real work here: a relation has
one chain today, and `ChainInsert` refuses an id below the tail page's
`min_key`, so interleaved id blocks on one shared chain fail on the
first insert. The leases supply the ids; the per-range chains supply
the tails; R3/R4 owns building the second. Consequences, stated now:

- Per-relation id monotonicity becomes per-range monotonicity —
  invariant 11's 2026-08-11 amendment (`docs/spec/heap-and-tuple.md` §4.1)
  one level down. **Written 2026-08-29 with R4**, as that spec's **§4.1a**:
  ids ascend per range and no longer in issue order across a relation, and
  the inference a caller loses is named there. `sys.tables.next_id` stays
  one high-water mark, so ids remain globally unique across cores (K1) and
  `key_order` is untouched — every block is carved *above* the mark, so
  spreading never produces a below-mark key.
- A **btree** relation whose caller names its keys spreads naturally —
  those ids need not ascend — and needs none of this. A **heap**
  relation does not get that for free even when the caller names its
  keys: since 2026-08-25 they must still be at or above the mark
  (`docs/spec/heap-and-tuple.md` §4.1), which is the same tail this section
  is about. The spreading problem is the chain's, not the issuer's.
- Interleaved blocks are the **default** — closed 2026-08-27 under the
  operator's range direction: a range is information the user does not
  have, and opt-in is a spelling the user would have to write. Recorded
  as CLA's reading of the direction, correctable. That a single-writer
  relation gains nothing from them bounds the default's benefit, not
  who decides it.

### 6c. Coalesce on Auxiliary DDL — the Merge, and What Bounds a Range's Walk (v2, AX)

**Ratified 2026-08-31** (`instructions/v2.7.0/ratification-ax.md`,
AX-D1 through AX-D6 and AX-D12; build order
`instructions/v2.7.0/ax-coalesce-on-auxiliary-ddl.md`). §6a stops a
relation with an auxiliary from splitting; `RefuseAuxiliaryOnSplitRelation`
(`src/catalog/catalog.cpp`) is its converse and stops a split relation
from gaining one. Both gates predate DA1, which armed
`range_size_ids = 65,536` by default — so a range now opens on workload,
an ordinary session meets the converse gate without choosing to, and
because nothing merges ranges (the mover is R5) **write-then-index was
refused for the life of the relation**. The operator ruled that a defect.
This section is the fix: the auxiliary DDL **coalesces the relation back
to one range** and then builds.

**What it is not.** No auxiliary lives on a split relation as a result of
this section. The five placement `[OPEN]`s — index per-range/global
(`docs/spec/index.md` §13), Cabin under split/migration
(`docs/spec/cabin.md` §11), assertion group-straddle
(`docs/spec/assertion.md` §6.1), cross-core FK
(`docs/spec/cross-owner-txn.md`), var-heap partition
(`docs/spec/heap-and-tuple.md`) — are **moved to R5's schedule as the
auxiliary placement decision group** (AX-D1) and none is decided or
assumed here.

#### The scope, which is narrow because §6a made it so

A split relation is a **heap** relation: D1 of
`workplan-range-directory.md` is not taken, so every btree relation is
unsplittable. And §6a's forward gates mean a split relation is
non-spilling, unindexed, un-cabined, FK-free and un-asserted — so the
merge moves **heap pages and only heap pages**. No var-heap page, no
index entry, no cabin entry set and no assertion registry can exist on
the relation being merged. That is what makes a merge a chain operation
rather than a rebuild.

#### The mechanism: concatenation, not re-placement

**A merge links the per-range chains tail-to-head in `lo` order and moves
no tuple.** Ranges partition the id space and a heap range is its own
chain with its own head (CC8); ids inside a range's chain ascend page by
page (`include/kds/storage/heap/heap_chain.hpp`, the ordering property);
and a range's head page carries `min_key = lo` by construction
(`Catalog::CreateRangeEntryPage`). So range *i*'s last page holds ids
strictly below range *i+1*'s `lo`, which is range *i+1*'s head's
`min_key`: the concatenated chain is key-ordered page by page exactly as
a never-split relation's is, `min_key` stays immutable (invariant 2), no
tuple moves to a page whose `min_key` exceeds its pk (invariant 3), and
the tail-page duplicate check stays O(1) pages.

**`next_page_id` is still written once per page.** The chain layer's
tail-hint safety argument — *a hint can be behind, never wrong*, because
`next_page_id` goes `kInvalid` → the new tail exactly once and a page
never leaves its chain — survives concatenation unchanged: the link
writes the one field that was `kInvalid`, and every page a hint could
name still reaches the current tail by walking forward.

The **fallback** page-by-page re-placement H-AX1 named is therefore not
taken, and the cost model is O(handoffs), not O(rows).

#### What concatenation breaks, and the rule that repairs it

A per-range walk used to end where the range ended, because the chain
ended there. **Concatenation removes that coincidence**, and three walks
rested on it: the step VM's page loop (`src/exec/step_vm.cpp`), the
dispatcher's per-range visit (`src/server/command_dispatcher.cpp`) and
the relayout survey (`src/stats/relayout_planner.cpp`). Each would have
walked from a range's head straight through its successors' pages and
then walked those successors again from their own heads — duplicate rows
on a read, duplicate writes under UPDATE and DELETE.

**So a range's walk is bounded by the range, never by the chain's end**:
a walk that reaches a page whose `min_key >= hi` of the range it is
walking has left that range and stops. The bound is exact — the only
page that can carry `min_key == hi` is the successor range's head — it
costs one comparison on a page already fetched, and it is asked **only
where a directory exists**, so an unsplit relation's walk is the walk it
always was and RD3's zero-cost invariant is untouched.

This is a correctness sharpening independent of the merge: a range's
extent is the directory's statement, and a walk that reads it from the
chain's shape instead was true only by accident. It is also what makes
the crash contract below hold by ordering alone.

#### The sequence (AX-D4, **proposed** until AX7's crash matrix is green)

Adapted from CC10's migration sequence. Core 0 drives, because DDL
executes on core 0 (CC12/CR2) and every catalog write is core 0's
(CC11). Per departing range, in ascending `lo` order:

0. **Quiesce.** The range's current owner stops writing it: in-flight
   work on that core finishes inside the leg's own task, the owner's
   write rights over the range's pages are **revoked**, and its frames
   are dropped. Revocation is new — CC7's cell records that the mover
   "must keep them agreeing by restamping *and* revoking", and this is
   the first caller of the revoking half.
1. **Flush.** The owner writes its dirty frames of the range out, so the
   absorber faults the rows and not an older image.
2. **Durable handoff.** Core 0 logs a PL-B `PAGE_HANDOFF` per page to the
   absorber and waits for durability (`docs/spec/page-lsn-cross-stream.md`
   §9 rule 1: the record lives in the giver's stream).
3. **Grant.** Exact-page fault and write rights to the absorber
   (PW1c-4's form, never extent-granular).
4. **Link.** The absorber writes the predecessor chain's tail
   `next_page_id` at the departing range's head, restamping per PL-C.
   **On the absorber and after the grant**, because the link is a write
   and the absorber is the only core entitled to make it.

Then once, for the relation:

5. **Contract.** Every `sys.ranges` row of the relation is deleted and
   `sys.tables.owner_core` is set to the absorber, in one catalog
   transaction in core 0's stream, made durable, and published by the
   version bump the catalog write already carries — which is step 5 of
   CC10's sequence, the invalidation broadcast, arriving as it always
   does.

**Why this ordering is the crash contract.** Every prefix of it is a
state the engine already serves:

- Crashing before step 5 leaves the directory intact and some chains
  concatenated. The bounded walk reads exactly the rows it read before
  the merge began, because the bound stops each range's walk at its own
  `hi` whether or not a link now runs past it. Inserts are unaffected:
  ids ascend and a heap relation refuses a named key below the
  high-water mark (`docs/spec/heap-and-tuple.md` §4.1), so every insert
  lands in the **top** range, whose chain is never concatenated *into*
  anything and whose tail is therefore still its own.
- Crashing after step 5 leaves the merged relation, which is a
  one-range relation and the ordinary shape.

There is no state to repair at mount and nothing to roll back: a
half-linked relation is a **legal split relation**, permanently, and a
re-run of the merge re-links idempotently (setting a `next_page_id` to
the value it already holds) and re-contracts. **This is H9's lesson
applied rather than repeated** — the structure is published before its
router, and the bound is what makes the published structure inert until
the router changes.

The **contraction is all-at-once** rather than range-at-a-time. The build
order left the choice to AX7 with a proposal for range-at-a-time on the
ground that every intermediate is then a state the engine serves; the
bounded walk makes *every* intermediate that already, including the
all-at-once one, so the argument no longer selects between them and the
tie goes to the form with one durable catalog transaction instead of *k*.

#### The absorber (AX-D3)

**The core holding the most pages of the relation**, which minimises
pages moved; on a tie the **lowest `core_id`**, for determinism and test
reproducibility (CLA's proposal, accepted under the ratification's
standing pattern; proposed, not measured). The absorber may differ from
`sys.tables.owner_core`, so step 5 updates that column — one more catalog
write inside the DDL's scope, in core 0's stream, CC11 unviolated.

The surviving chain is headed by `desc_page_id` whatever the absorber is:
the `lo = 0` directory row records `{lo = 0, owner_core, desc_page_id}`
(`Catalog::OpenRangeRows`), so `desc_page_id` is the low range's head,
and a merged relation with **zero directory rows** resolves through
`sys.tables` — `desc_page_id` for the chain, `owner_core` for the core.

#### The final directory state is zero rows

Not one row. A non-empty directory must partition the whole id space from
`lo = 0` (CC9), so a one-range relation is represented by **absence**,
which is the shape `TableAccess::ranges.empty()` reads and the branch
RD3's zero-cost invariant is taken from. A merged relation is
indistinguishable from a never-split one at every read of the catalog.

#### Two-phase, and the residue a failure leaves (AX-D5)

The merge is **synchronous and completes before the DDL transaction
begins**. It is not inside that transaction and cannot be: a page handoff
is not undoable by the catalog transaction's compensation. So the
statement is *merge, then build*, and **a DDL half that then fails leaves
the relation merged.** That is a valid state — one range is always valid
— but it is an observable side effect of a failed statement, and it is
specified here rather than discovered: a `CREATE INDEX` that fails on a
duplicate key, on a budget, or on any later refusal has still coalesced
the relation, and the relation does not re-split.

The cost that rides with it is **DDL latency proportional to pages
moved**, accepted at ratification and measured by AX8.

#### No re-split once an auxiliary exists (AX-D6)

`RangeEligible` refuses a split on a relation carrying an auxiliary,
exactly as before. So **spreading and auxiliaries are mutually exclusive
on one relation** until R5: a coalesced relation forgoes spreading's
measured gain (1.51× on the group arm at k = 5,
`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`)
for as long as its auxiliary lives. The line is drawn in the same place
twice, deliberately — changing it *is* the auxiliary-under-a-boundary
question AX-D1 moved to R5.

#### Only an explicit statement coalesces (AX-D12)

The trigger set is exactly the four explicit DDL callers of
`RefuseAuxiliaryOnSplitRelation`: `CREATE INDEX`, `CREATE CABIN`,
`CREATE ASSERTION`, and an FK naming the relation on either side. **The
Cabin optimizer's automatic path keeps the refusal**, and its decline
stays visible in §6a's decline counters. A synchronous merge is a large
physical page movement, and an unattended background controller
triggering one is against Part I's enact-through-named-gates discipline
(`docs/spec/physical-optimizer.md`): physical change happens where the
operator can see it. `RefuseAuxiliaryOnSplitRelation` therefore **stays**
— callable, and called — rather than being deleted with its four
converted callers.

## 7. Cancellation, Errors, Early Termination

- `ORDER BY pk + LIMIT`: when the session core has framed the LIMIT-th row,
  it sends `STEP_CANCEL` upstream; producers stop at the next batch
  boundary. Cancel is advisory-fast, correctness-safe: batches already in
  flight are discarded by tag (§3).
- A step failure sends `STEP_ERROR` downstream (so the session core can
  frame the error) and `STEP_CANCEL` upstream (so producers stop). The
  session core frames exactly one terminal message per request.
- Connection close with live pipelines: the session core issues
  `STEP_CANCEL` for every live request as part of session teardown (the
  same hook that rolls back an open transaction, `docs/spec/txn.md` tests).

- **A batch send that fails cancels the pipeline; it never merely breaks
  out of the drain loop** (2026-08-27, fixed at `7148343`). Breaking alone
  leaks one `pipelines_` entry per failed send — `SendFn` takes its payload
  by value, so the queue head is left a moved-from vector that nothing pops
  and the EOF-and-erase arm never runs — which is exactly what **§8's test
  3 forbids** (*"no state leaks (pipeline table empty after teardown)"*).
  Cancelling instead routes teardown through the path a cancel already
  takes: the producer erases if one is live, the drain itself otherwise.
  The arm was unreachable until the send seam learned to refuse an oversize
  payload, which is why a fault this old had never fired. Pinned by
  `RemoteStepServiceTest.ABatchSendThatFailsTearsThePipelineDownInsteadOfLeakingIt`.

## 8. Determinism and Testing

All of this must run under the simulated ring seam (`docs/spec/sched.md` §6):
message delay and reorder injection, reactors stepped round-robin on one
thread. Required tests:

1. **Equivalence:** a two-core join pipeline returns byte-identical result
   sets to the same statement executed single-core over the same data.
2. **Flow control:** a slow consumer stalls the producer at the credit
   bound; draining resumes it; peak batch memory per edge never exceeds
   initial credit × batch size.
3. **Cancel mid-stream:** LIMIT early termination stops upstream production;
   post-cancel batches are discarded; no state leaks (pipeline table empty
   after teardown).
4. **Error propagation:** an injected step failure yields exactly one
   terminal error at the client and full teardown on every participating
   core.
5. **Write restriction:** a transaction writing relations owned by two cores
   receives the retryable conflict error at the second write; the first
   write rolls back cleanly; the observability counter increments.
6. **Tag isolation:** two concurrent pipelines between the same core pair
   never cross batches (tag discipline), under injected reordering.
7. **Fast path:** with all relations on one core, the pipeline layer
   contributes zero messages and the execution trace is identical to the
   pre-multicore build.
8. **RR weakening:** an RR transaction's cross-core read observes a commit
   made after the transaction began (documented behavior pinned by test).

Added 2026-08-24 (v2) — each lands with the phase that builds its
mechanism, R3-R5:

9. **Range equivalence:** every shippable shape over a split relation
   returns byte-identical result sets to the same statement over the
   same rows unsplit on one core — test 1's discipline with the split as
   the only variable, over data where matching rows straddle the
   boundary.
10. **Shared statement view:** a transaction writing two relations *or*
   two ranges owned by one core commits between two of a statement's
   view mints on that core; the statement's answer contains all of that
   transaction's writes or none (§5's per-(statement, core) rule,
   pinned against the torn read in both shapes). **Lands with the peer
   writer (PW1c-5), not R3** — the two-relation shape is reachable the
   day a peer can commit.
11. **Migration ordering:** a crash injected between each pair of
   CC10's steps 0-5 recovers as CC10 states — before the grant the
   migration aborts to the outgoing owner at mount; after it the
   incoming owner completes; the record/redo/stamp halves recover per
   `page-lsn-cross-stream.md` §9, and a reachable stamp mismatch
   refuses the mount as `Corruption`, never a skip.
12. **Insert spreading:** k cores inserting concurrently each land in
   their own range's tail; ids ascend per range; ids stay globally
   unique (K1's issue-once contract across cores); invariant 3 holds
   per range.
13. **Split gates:** the allocator's admission check declines an
   indexed, cabined, spilling, or FK-linked relation, names the gate,
   and the relation stays one range (§6a — an engine decision, not a
   statement refusal: no token, no byte); on an eligible relation the
   allocator opens the second range and the directory rows appear (the
   lo = 0 row included, CC9), and an unaffected relation's fast path is
   byte-identical to before.

## 9. Open Items

- ~~Cross-core commit protocol (2PC)~~ — **built 2026-08-28**,
  `docs/spec/cross-owner-txn.md`, at relation granularity. Write shipping
  inside explicit transactions and read shipping inside them both landed
  with it. What was gated behind it and is **still open**: multi-*range*
  write statements and transactions (they inherit the protocol unchanged
  and need RD3's resolver — blueprint §11's R6), cross-core FK, and RR
  snapshot forwarding to the remote-step pipeline (§5's last paragraph).
  PL-A's revisit clause fired and was executed (R6-7): 2PC changes nothing
  about page identity across streams, and the operator **ruled on
  2026-08-30 — the re-decline is confirmed**, PL-A stays declined and the
  clause is spent (`page-lsn-cross-stream.md` §9).
- Split/migrate policy and its constants — triggers, thresholds,
  cadence, and merge (the mover is the physical optimizer's Part III;
  blueprint R5 owns the phase, the Part III spec owns the policy when
  drafted).
- Auxiliary placement under a split relation — each lifts its §6a gate:
  per-range local vs global secondary indexes (`docs/spec/index.md`
  §13), Cabin (`docs/spec/cabin.md` §11), var-heap partition
  (`docs/spec/heap-and-tuple.md`), FK (`docs/spec/foreign-keys.md`).
- ~~Id-block interleave default (§6b): default or opt-in~~ — closed
  2026-08-27 as **default**, CLA's reading of the operator's range
  direction, correctable; §6b carries it.
- ~~The shared-structure access mechanism (blueprint §8)~~ — **closed
  2026-08-30 by operator decision**: every core reads, core 0 alone
  writes, both of §8's candidates declined (**CC11**). What that ruling
  does not cover is renamed here rather than left pointing at a closed
  bullet: **the btree's top-of-tree hop** under a split relation. CC8's
  one-boundary claim lands on it, its writer is the root's owner core and
  never core 0, so CC11's rule does not reach it — and it still gates
  R3's btree ranges (`workplan-range-directory.md` D1). Two candidate
  answers stand: an access mechanism for the shared top, or removing the
  shared structure by giving each range its own tree
  (`instructions/v2.6.0/v2.6.0-per-range-trees.md` proposes the second).
  Undecided either way.
- **DDL's route to core 0 — the wire form and the reply's failure mode**
  (opened 2026-08-31 by **CC12**/CR2, which decided *that* a peer ships
  DDL and waits, not *how*). Two questions, neither assumable: whether the
  request rides DML's existing `session_step_client`/`remote_step_service`
  path or takes its own request kind; and what a reply that does not fit
  reports. A shipped reply over 992 bytes reports `UNKNOWN_OUTCOME` today
  — which `known-gaps.md` already names the wrong thing to tell a client
  about a statement with no effect — and while a DDL *success* reply is
  small, a DDL **error** carries a byte position and a relation name and
  is not obviously bounded. Size it from the refusal messages before
  building.
- Batch size and initial credit tuning (`[PROPOSED]` values above).
- ~~Pattern/Waystone-driven relation placement~~ — the *dynamic* half
  subsumed 2026-08-24 by the mover (CC10): re-placement is range
  placement, decided by statistics. **Initial placement closed 2026-08-31
  as `creating`** (**DA2**, `instructions/v2.7.0/ratification-da.md`), on
  `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §6:
  rotation's crossover is a step at the first core to take a second
  session, and past it rotation is negative at seven writer cores
  (0.51×). It is also the policy DA1's sweep was run under, so DA1's
  numbers are numbers for the ratified policy rather than for the other
  one. **`rotate` is not deleted** — it stays a configurable placement and
  §6a's gates are unchanged; what DA2 settles is the default. Either way
  placement stays an optimization concern — cross-core execution is the
  correctness path regardless.
