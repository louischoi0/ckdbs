# Cross-Core Execution

> **AT-S9 (2026-09-24): ownership and placement are retired**, and with them
> the routes this spec was written for. No relation or range is owned by a
> core (`owner_core`'s bytes are reserved), every statement runs on the
> core its session is on, no range is opened (insert spreading retired),
> and the single-step fan-in and two-step pipeline are gone. **AT-S10
> deleted the remote-step protocol** those two routes ran on: the remote
> step server, the session's step client, the step descriptor codec, and
> ring kinds 1-6 and 40. §2's pipeline path, §3's step kinds, §4, §4a, §5
> and §7 are collapsed to a record of it; the full text is
> `git show 91111e3:docs/spec/crosscore.md`. **The decision rows below
> state what is true now; §2a, §6's multi-owner rules and §6b still
> describe the retired ownership mechanism** and are AT-S12's to rewrite
> (`instructions/v3.0.0/workorder-at-m3-uniformity.md`); the row-id leases
> §6b builds on are gone too since AT-S10b (`heap-and-tuple.md` §4.1a).
> Where a section and a row disagree, the row wins.

How a single statement that references relations owned by different cores
executes. This is the concept spec for the mechanism `docs/spec/protocol.md` D3
reserved ("server-side forwarding — clients are core-topology-unaware") and
`docs/spec/sched.md` §5 provides transport for. Consistent with `docs/rules/rules.md`
(thread-per-core, core-local by default with what is shared declared,
no exceptions, deterministic testability).

**There is no ownership unit since AT-S9.** It was the primary-key range
(CC8), with `sys.tables.owner_core` the one-range case; both are retired,
and a range is now only a sub-structure of a pre-AT split relation.

Scope boundary: this spec covers cross-core **reads** and the routing every
statement takes. Cross-core **commit** — a transaction touching relations
owned by more than one core — was specified in
`docs/spec/cross-owner-txn.md`, which is retired: a transaction is one
core's, whole, since AT-S6;
where the two disagree, that one wins on the protocol and this one on
routing. "Cross-core write" includes a statement or transaction writing two
ranges owned by different cores, *even ranges of one relation* (CC3); a
multi-*range* write statement or transaction is refused (§6).

## 1. Decisions

| # | Decision | Resolution |
|---|----------|-----------|
| CC1 | Execution model | **Every statement runs on the core its session is on** (AT-S5 writes, AT-S6 reads, AT-S9 the last two routes), **and no part of one runs anywhere else** (AT-S10). The step pipeline this row named - each step on the core owning its range, output flowing to the next step's core - lost its last producers at AT-S9, the single-step fan-in over a split relation and the two-step join pipeline, both of which chose their cores by ownership; AT-S10 deleted the protocol itself (AT-0 item 4, answered *struck*). **The ring carries nothing the engine sends since AT-S10b**: its last two kinds, the id leases `kTrxIdLease` (18) and `kRowIdLease` (22), lost their users when every core began issuing its own ids, and stay enumerated only as the transport cells' stand-ins (`include/kds/sched/ring_message.hpp`, §3) |
| CC2 | Intermediate transfer | **None since AT-S10**: no row crosses a core. It was KWP binary row batches (protocol D5) in chunked ring messages under credit-based flow control (§4). What stands is the rule it imposed: **one row encoding**, `wire/row_codec.hpp`, whose one consumer is now the KWP wire - result batches and the load stream's chunks (`protocol.md` §6, `bulkinsert.md` BI6) |
| CC3 | Write scope | A transaction's writes run on the core its session is on (AT-S5), and a transaction is one core's, whole (AT-S6). **Nothing is refused for touching two ranges or two relations** since AT-S9: the multi-owner refusals (a write naming no pk, a join, `LIMIT`/`OFFSET`, a sort, DDL on a relation with two or more ranges) went with the owners they described |
| CC4 | Remote-read isolation | **There is no remote read since AT-S10.** Every read takes the snapshot its own statement or transaction holds on the session's core (`txn.md` §4.1), so the per-stage view this row described - a remote step's latest-committed snapshot, minted per stage and outside any asking transaction - has nothing left to describe (§5) |
| CC5 | Cancellation & errors | **Nothing cross-core to cancel since AT-S10**: a statement's errors are its own core's, framed by its session. The `STEP_CANCEL`/`STEP_ERROR` propagation this row named went with the protocol (§7). The tag `(session_core, request_id, step_id)` is still a field of every ring message's header (`ring_message.hpp`'s `MessageHeader`); nothing reads it since the lease services went at AT-S10b (§3) |
| CC6 | Scheduling | A ring message's task runs in the scheduling group its **sender designates** on the header (`sched.md` §5). The rule that stood here - remote step tasks in `foreground`, because step chains are the OLTP path - retired with the protocol at AT-S10 |
| CC7 | Page ownership | **None, since AT-S9** (AR0-5 D17). Pages were the core's that `sys.tables.owner_core` named; that column's four bytes are reserved now - written 0, never read, so a pre-AT volume's value is ignored where it lies and the volume mounts unchanged. Every core reads and writes every page under the page latch (CC11, `page.md` §6). The history of how ownership was realized - the flush-then-grant handoff AW-S1b deleted, the owner-built index and assertion paths AT-S5d/S5e retired - is git's |
| CC8 | Ranges | **A pk range `[lo, hi)` is a sub-structure of one relation, and owned by nothing** (AT-S9). A heap range is its own chain with its own head, the entry page riding the directory row (CC9); a btree relation does not split (§6a). Ranges exist only on relations split before AT-S9 retired spreading (and, for a relation created since SUS-1, not even then). **Every walk covers every range** (`TableAccess::WalkHeads`), and a row lands in the range its id falls in (`HeapChainFor`, which refuses an id in no range) |
| CC9 | Range directory | `sys.ranges` (rel oid, lo, a reserved word where the owner core was, entry page; hi is the next row's lo, and a **non-empty directory carries a row at lo = 0**, so the rows partition the whole id space) records every split a pre-AT volume made; a relation with no rows there is one range headed by `sys.tables.desc_page_id`. Resolved from the executing core's catalog cache. A directory write bumps the schema version word as DDL does (`catalog.md` CT2) |
| CC10 | Range split | **No range is opened since AT-S9**, which retired insert spreading - the only path that created one - with range ownership, on the operator's ruling. `range_size_ids` is refused by name. What a split relation already has is read and written whole; `Catalog::OpenRangeRows` survives with no production caller, as the way a cell builds the state a pre-AT volume can hold. The rules this row carried for opening a range (a page-boundary split, the durable row before the grant, the pre-grant Cabin discard) are git's |
| CC11 | Shared-structure access | **Every core reads and writes with the same authority** (AT-S5; AR0-5 §0). The rule that stood here until then — *core 0 alone writes the superblock, the free map and the catalog pages, and one writer is the serialization mechanism* — is retired with the store's `MayWrite` arm that enforced it and the `catalog_read_only_` flag that routed around it. What serialises each of the three now is named where it is written: the **page latch** across cores for the bytes of any page (`page.md` §6), and within one core the rule that no task parks under a page span, which is what makes a catalog row's read-modify-write atomic (`catalog.md` CT5, `AdmitExplicitRowId`'s hook); the free map's own `map_latch_` for allocation (AM-S3, `page.md` §5); the **schema version word** for every core's memo of the catalog (`catalog.md` CT2), which replaced the invalidation broadcast at AT-S2; and page 0's persisted transaction-id ceiling, which every core's `TrxIdSequence::Carve` reads and raises in one step under the superblock latch since AT-S10b (`txn.md` §4.2) — core 0 alone carved until then, by the lease's routing. A relation's row-id mark is a catalog row like any other, bumped in place under its page latch by whichever core issues (`heap-and-tuple.md` §4.1a). DDL runs where the session is, under the DDL's own relation `X` (`txn.md` §5). A peer's fill path was already the shared frame (AM-S2 step 3) and is unchanged. Not covered: a *relation's* shared structure — the btree's top levels under a split relation (CC8) |
| CC12 | Catalog page placement | **CR1 — a catalog relation's root page stays in the reserved range; its var-heap does not.** `kCatalogPageTypes = 4` through `kCatalogPageRanges = 15` (`include/kds/catalog/well_known.hpp`) sit below `kFirstUserPageId = 128` because they must be findable at bootstrap *without* a catalog read. A catalog relation's **var-heap** root is allocated through `CreateNew()` and recorded in `sys.tables`, binding on any catalog relation with a var-heap. The cost this carried — a page outside the range was not peer-readable, so the one peer-readable catalog var-heap, `sys.assertions`, was reached by granting the individual pages a row names — **went with the fault grants at AW-S1b**. Every core faults every page, so a var-heap root's id no longer decides who can read it. `exec::CatalogSpillPages` survives with its other consumer, the mount's var-heap sweep. **CR2 is retired at AT-S5, and its sentence stood here until AT-S5b** — *DDL executes on core 0; a peer sends and waits*, shipping the request and falling back on `PeerDdlRefused` for what the ship did not cover. DDL runs where the session is (CC11, CC13's CR5), both the ship and the refusal are gone, and a peer that has to **grow** a catalog chain places the page itself since AT-S5b (`catalog.md` CT5). **CR3 — a catalog page may leave the reserved range once grown.** Catalog pages are allocated and initialised inside the range at bootstrap; beyond bootstrap a catalog page **may be managed outside it** in either of two cases — the relation grows past what the reserved range holds, or every peer must read *and write* the page equally. Such a page takes the ordinary relation rules: allocation from the general supply, free-map accounting, and the WAL logging catalog change already has. No catalog page has left the range; a var-heap root outside it is found through `sys.tables` — the indirection `varheap_page_id` uses, DDL-immutable and therefore cacheable per `rows.hpp` |
| CC13 | DDL's route, and where a core's statistics are written | **CR5 is retired at AT-S5**: a peer routed DDL to core 0 by shipping the statement, detected at dispatch on `catalog_read_only_` and the `CREATE`/`ALTER`/`DROP` tokens, with `PeerDdlRefused` for what the ship did not cover; DDL runs where the session is now (CC11). **CR6 — a catalog relation whose content is only a statistic is written unlogged**, the sole exception to the rule that catalog writes are WAL-logged and replayed (`docs/spec/ddl-transactional.md` §7; the rule text is `docs/rules/rules.md` §5). **CR7 and CR8 are retired at AT-S7.** CR7 had a peer batch its access shapes locally and flush them to core 0 over `kAccessStatsBatch`, because `sys.access_stats` sits at page 11 inside the reserved range and only core 0 could write it; CR8 made that send the engine's one **droppable** message, invariant 8 pricing a lost statistic as performance and never a result. Every core writes every page since AT-S5, so **a core writes its own access statistics where the statement ran** - the batch, the ring kind, the drop and the five `SHOW META` counters are gone, and a peer's count is in the row before its statement returns rather than on the next tick. What stands from CR7 is the part that was never about the wire: **`sys.access_stats` is one relation at page 11 (`kCatalogPageAccessStats`)** and there are no per-core statistics relations - `SysAccessStatRow` is a 33-byte fixed row with no `core_id` field (`catalog/rows.hpp`), so every core's counts **fold into the one `(kind, rel_id, column_mask)` shape**. `Catalog::RecordAccess` holds that relation's root page exclusive for its whole body, which is what makes the fold and the admission of a shape nobody has seen one act across cores. `RecordAccess` costs +1-2% on a point lookup (`docs/spec/heap-and-tuple.md` §7) |

## 2. Execution Model

The session core owns the statement end to end: it parses, compiles the
step chain (the written-order contract of `docs/spec/parser-v2.md` — *the
statement is the chain*, never silently reordered) from its catalog cache,
and executes every step of it itself, reading every page through the
instance's one frame table (CC7, CC11). A walk of a split relation covers
every range (CC8, `TableAccess::WalkHeads`). There is **one path**, the
single-core code; what was the "local fast path" is all there is.

Trail and statistics recording is the executing core's: a core writes
`sys.access_stats` and `sys.patterns` itself, under those relations' root
page latches (AT-S7, CC13).

**The pipeline path was retired in two steps.** AT-S9 removed the last
statements that opened one - the single-step fan-in over a split relation
(k sibling stages, bounded by a 255-upstream ceiling) and the two-step join
pipeline (chained `STEP_OPEN`s, each stage forwarding the join key and the
projected columns to the next step's core) - and AT-S10 deleted the
protocol they ran on. The full description is
`git show 91111e3:docs/spec/crosscore.md` §2.

## 2a. Ranges — the Unit, the Directory, the Routing

**The unit** is CC8's; **the directory** is CC9's `sys.ranges`, resolved
as CC9 states (plan time, the session core's cache, staleness a retryable
step error). Two rules beyond CC9's cell:

- **The directory is read-mostly.** A split is a rare, DDL-frequency
  event; the per-statement path reads the per-core cached copy. A split
  bumps the schema version word as DDL does (`catalog.md` CT2). **A
  resolved range is never cached across a suspension**: a peer's cache is
  dropped by the word's `Revalidate()` at a task boundary, which never
  advances `catalog_version()` — that counter is one core's (`catalog.md`
  CT1) — so a range fact guarded by it would be wrong on every peer.
- **The fast-path invariant binds here hardest**: a one-range relation
  on its owner core must add zero instructions over the single-core code
  (CC1, §2).

**Routing a predicate to ranges.**

- A pk equality or pk range names its range(s) arithmetically against
  the directory — no structure consulted, no broadcast.
- A non-pk *read* predicate names none; the default is every range of
  the relation, and that fan-out is cut only by the engine's own
  structures under their existing authority rules, never by a new one:
  Cabin answers "which range holds value V" for an observed key under its
  own banked-authority rules, for every range of the relation
  (`docs/spec/cabin.md` §4a, §4b, §6a); a Waystone trail names pages, a
  page names a range, a range names a core — advisory per invariant 9, and a trail replayed on the
  wrong core misses on the epoch/owner check and falls through, the
  ordinary miss discipline. A split relation carries no secondary index
  (§6a), so no index probe routes on one.
- **Advisory structures never narrow a write's target set.** Invariant
  9's license is *where to look*; for a write, executing on fewer
  ranges than hold matching rows is a missed write, not a slower one.
  DML target resolution is pk arithmetic against the directory alone
  (§6).

## 3. Messages

**No engine message crosses the per-core-pair SPSC rings since AT-S10b**
(`docs/spec/sched.md` §5). The last two kinds, `kTrxIdLease` (18) and
`kRowIdLease` (22) - a peer's request to core 0 for a block of transaction
ids or of one relation's row ids, and core 0's grant on the same kind -
lost their users when every core began carving its own transaction-id
window and bumping a relation's row-id mark in place (`txn.md` §4.2,
`heap-and-tuple.md` §4.1a). They stay enumerated in
`include/kds/sched/ring_message.hpp`, the census AR0-6's D25 freezes at 2,
only as the transport cells' stand-in kinds, until the transport and that
file go whole. Every other value is struck and never reused. The seven this
section tabulated until AT-S10 - `STEP_OPEN`, `STEP_BATCH`, `STEP_EOF`,
`STEP_CREDIT`, `STEP_CANCEL`, `STEP_ERROR` (1-6) and `SHIPPED_ROW_DESC`
(40) - went with the protocol.

Every message's header still carries the tag `(session_core, request_id,
step_id)` (`MessageHeader`). A `request_id` is sequential per core, never
pointer-derived (`docs/spec/sched.md` §8 determinism rules), and zero names
no request. The rule the tag was written for - a batch whose tag matches no
live pipeline state is discarded silently, the teardown correctness rule -
has no batch left to apply to.

## 4. Transfer Format and Flow Control

**No rows cross a core since AT-S10**, so this section has no format and
no flow control left to state: the step batch, its 32 KiB target and
ring-slot ceiling, and the per-edge credit protocol (4 initial credits,
preallocated at `STEP_OPEN`) went with the protocol. The one-encoding rule
they obeyed stands as CC2's.

What stands is the transport's own rule, which every send keeps:

- **A successful send wakes a sleeping destination; a refused one wakes
  nobody** (`docs/spec/sched.md` §7 and its invariant 7). The send stays
  non-blocking and fallible, and the wake follows the push, so a message
  never waits out the destination's idle block. The refused case is
  deliberate: waking a core for a message that is not in the ring is the
  spin the wake exists to remove, moved to the sender. Ring-full on send
  follows the global rule: the sending task yields and retries; it never
  blocks the reactor and never drops.

### 4a. The answer edge

**Retired at AT-S10.** The answer edge was how a stage delivered rows to
the core that asked for them - a fan-in over a split relation, or a
two-step join's consuming stage, and until AT-S6 a shipped read - over
`STEP_BATCH` under credit, with the row description crossing first as
ordered `SHIPPED_ROW_DESC` chunks. AT-S9 retired the last read that opened
one and AT-S10 deleted the edge with the protocol. A result row reaches a
client from the core the statement ran on, through the session's
`ResultSink` (`protocol.md` §12). The full text, including what the edge
refused and why the description was its own kind, is
`git show 91111e3:docs/spec/crosscore.md` §4a.

## 5. Isolation Semantics

**A statement reads under one view, on one core.** Every step of it runs
where the session is (CC1), so the snapshot it reads under - its own, or
its transaction's (`docs/spec/txn.md` §4.1) - is the only one it has, and
no part of its answer is read at another instant.

- **READ COMMITTED** statements: each statement observes the latest
  committed state as of its own view.
- **REPEATABLE READ** transactions: one view, on one core. The
  transaction pins its snapshot at `BEGIN` and every statement of it runs
  where the session is, so "one instant on every core" is true by having
  only one core to be true on.

  **It was a protocol until AT-S6** (AN-S3): a cross-owner RR transaction
  carried its coordinator's snapshot on every shipped statement and each
  participant adopted it when it opened its context. Nothing ships now, so
  nothing carries and nothing adopts.

**The per-stage weakening this section stated went with the protocol at
AT-S10.** A remote step minted its own latest-committed view when its
stage first ran, so two stages of one statement could disagree about a
concurrent commit, a stage could see a commit a local view would have
excluded, and inside an explicit transaction a stage could not see the
transaction's own uncommitted rows - a wrong answer, which is why the
remote-read routes were skipped inside a transaction and why the two-step
pipeline was also wrong there. None of it has a stage to apply to; the
record is `git show 91111e3:docs/spec/crosscore.md` §5.

- Catalog: the plan is resolved and executed on the session core from
  its catalog cache, under the relation `IS` every bound relation holds
  for the statement (`docs/spec/txn.md` §5).

## 6. Writes

**No statement crosses.** A read and a write both run on the core their
session is on, and the pages they touch are faulted through the one frame
table AM-S2 step 3 made the instance's. What a relation's `owner_core`
still decides is where a range is opened and where a placement-bound task
runs, never where a statement executes (D18).

**Statement shipping was how a statement reached a relation another core
owned**, and it is retired in two halves:

- **Writes, at AT-S5.** A single DML statement was shipped whole to the
  core owning its target range and executed there under that core's
  transaction machinery. `MayWrite`'s last arm, the three write ship forks
  and the cross-owner refusals went with it.
- **Reads, at AT-S6.** An autocommit read was carried to the owner as
  text and answered from there; a read *inside an explicit transaction*
  shipped and **enrolled**, which is what gave a transaction a half on
  another core at all. It went because it was wrong rather than merely
  unnecessary: the write it was written beside had stopped shipping, so
  the two halves of one transaction ran under two transaction ids and the
  read could not see its own uncommitted row
  (`tests/shipped_read_own_write_test.cpp` is the cell that measured it and now pins the fix).
  The two-phase commit protocol lost its last traffic with it
  (`cross-owner-txn.md`, retired).

What the mechanism cost is recorded rather than lost: the refusals it
converted, the `UNKNOWN_OUTCOME` a lost answer had to be, and the
per-(arrival core, session) dedup record that kept a duplicate from
executing twice. A citation to any of it resolves against
`git show c63e49f:docs/spec/crosscore.md`.

**Nothing is refused for where it reads since AT-S9.** The one shape left
until then - a read of a split relation this core did not wholly hold, in
a statement the fan-in could not take (`CheckReadAffinity`) - went with
ownership: every walk covers every range (CC8), so no walk answers short.

**Target resolution is pk arithmetic against the directory alone** (§2a):
a DML on a split relation whose predicate does not bound its rows to one
owned range is a cross-core write, refused retryably.

- A write to a range this core does not own, reaching `CheckWriteAffinity`
  without having been shipped, is refused `CrossCoreWriteRefused`
  (retryable, protocol D9; the same client contract as first-updater-wins
  aborts in `docs/spec/txn.md`) and recorded. Writes to any ranges the home
  core owns — of one relation or several — are legal: they are
  single-owner, and nothing 2PC-shaped is in them (§5's shared statement
  view is the reader-side half of that claim).
- Every rejected cross-core write increments a per-core observability
  counter keyed by (home core, target core, relation). The counter counts
  the *residue* — the writes shipping does not convert, which is the
  multi-owner-statement and cannot-park population; what shipping converts
  is counted separately by `SHOW META`'s `shipped_*` fields. Counters are
  metrics, not stored state. `SHOW META` prints
  `cross_core_write_refusals`, `cross_core_write_refusal_keys` and a
  capped `cross_core_write_refusal_detail` of `home>target:oid=count`. The
  counter is **core-local**, so a total is one reading per core. What it
  cannot see, stated at the print site as well: anything refused before
  resolution at all. **DDL on a peer** was the named case until AT-S5
  (`PeerDdlRefused`, refused by verb before any relation was resolved);
  there is no such refusal now, and a peer's DDL is a statement like any
  other. The owner-core refusals —
  `IndexBuildPending` until AT-S5e, and `RelationWriteRightsPending` beside
  it until AW-S1b struck the grants it waited on — were excluded **by
  decision**: the write was not cross-core, it was this core's own write
  waiting on a build window. Neither exists now.
- Write-coupled auxiliary placement is §6a's. On a one-range relation
  unique indexes, Cabin, Waystone pages, and the var-heap live on the
  relation's owner core, always. Read-only join partners may live anywhere.
- FK (`docs/spec/foreign-keys.md`): parent and child on two *relations* on
  two cores is **not a crossing at all since AT-S5f**. The forward check
  still hoists to the dispatch fork - for deduplication, and because the
  wait has to happen where nothing has been written yet - and descends
  every parent here; the reverse check walks every chain of every child
  here. Nothing is probed, nothing is left behind, and the
  validation-to-commit window closes on the child row's own header: an
  uncommitted child answers a parent's `DELETE` busy because the walk
  reads it, where a reference intent on the parent's owner used to say so
  (`foreign-keys.md` §2a/§3a). Nothing inside a `WriteScope` waits. A
  split parent or child would make the validation range-granular, which
  the engine does not do: §6a's FK gate.

### 6a. Write-Coupled Auxiliaries — What May Split

A split relation has no single owner core, so the one-range co-location
rule does not survive a split as written. **The rule is that a relation
carrying such an auxiliary does not split.** There is no user-facing range
DDL — a range is information the user does not have; the system allocates
and enforces it. The gates bind wherever a range would be created — the
range allocator's admission check, `RangeEligible`. A gated relation is
declined as a logged engine decision naming the gate — no statement asks
for the range, so no offending token and no byte position: the caller
logs the line and increments per-core decline counters in §6's
`SHOW META` refusal-counter form, owner-core-local because the decline is
the owner's refill tick's (the assertion gate reads the instance's
registry since AT-S5d, which every core answers alike).

- **Btree relations** — a btree relation does not split (CC8's
  top-of-tree hop has no owner rule).
- **Secondary indexes** — an indexed relation does not split. A secondary
  index is btree-only (`docs/spec/index.md`), so every indexed relation is
  already declined by the btree arm, and `RangeEligible` carries no index
  arm of its own.
- **Cabin** — an **Observational** Cabin does not gate split: its entry
  set is authoritative for the observed value across every range, and
  every walk covers every range, so a boundary changes nothing a set
  speaks for (`docs/spec/cabin.md` §4b). A **Bound**
  Cabin gates split, asked as the assertion arm below, where a live Bound
  Cabin is visible; `RangeEligible` carries no `kCabin` arm.
- **Var-heap** — one `kVarHeap` page may hold spilled values referenced
  from tuples on both sides of a boundary, a core faults only pages it
  owns, and invariant 14 stands (values immutable per version, pages
  never relocated). A relation whose schema can spill (`SchemaCanSpill`)
  does not split.
- **Foreign keys** — an FK parent or child does not split
  (`docs/spec/foreign-keys.md`; the §6 bullet above says why).
- **Assertions** — an asserted relation — one the registry holds live
  (`AnyOn`) *or* knows and cannot enforce (`CannotEnforce`) — does not
  split. **The gate is unchanged and two of its three reasons are gone.**
  It was written for a Bound Cabin that lived on the owner alone: a second
  owner core's appends hit `MayWrite`'s refusal (gone at AT-S5, which
  retired the arm), and a core whose registry never heard of the assertion
  admitted the write unchecked (gone at AT-S5d, where the registry became
  the instance's, `assertion.md` §6.1). What stands is that the aggregate
  is keyed on group columns that need not include the pk
  (`ResolveAssertionColumns` refuses none), so a group's rows straddle any
  range boundary; whether that alone should keep a relation whole is not
  re-decided here, and spreading ships off in any case. The fact
  deliberately does not ride `TableAccess`:
  `CREATE`/`DROP ASSERTION` do no version bump (`assertion_catalog.cpp`'s
  publish comment — nothing cached is derived from a `sys.assertions`
  row), so a cached bit would be stale by construction, and the gate
  function takes the enforcer, not the access struct alone.
- **Waystone and statistics** — advisory (invariant 8): a stale trail
  misses and falls through, and **no gate is needed** — worst case the
  trail is deleted, which invariant 8 prices as performance, never a
  result. Both are written **where the statement ran** since AT-S7, into
  the instance's one `sys.patterns` and one `sys.access_stats`; there are
  no per-core statistics relations, and there never were.

What is splittable — non-spilling (`SchemaCanSpill` false; invariant 13
makes *every* relation fixed-length, so the spill is the gate), heap,
un-cabined-by-a-Bound-class, FK-free, un-asserted relations — is narrow
and real.

### 6b. Inserts and the Tail — Id-Block-Aligned Spreading

> Retired: spreading at AT-S9 (`range_size_ids` is refused by name), and
> the row-id leases it was built from at AT-S10b - every core bumps the
> relation's one mark (`docs/spec/heap-and-tuple.md` §4.1a). The text
> below is the retired mechanism, left for AT-S12 (see the header).

**`range_size_ids` ships `kRangeSizeOff`** (`include/kds/server/range_alloc.hpp`):
by default no range ever opens, a relation is one range for its life, and
the pk is an identity *and a sequence* — monotonic in issue order
(`docs/spec/heap-and-tuple.md` §4.1). An operator who sets
`range_size_ids` turns spreading on instance-wide; `kRangeSizeIdsDefault`
(65,536) is what a range measures once it is on. There is no per-relation
spreading flag and no syntax for one.

With spreading on: a core that does not own a relation records row-id
lease demand before giving a foreign `INSERT` away, which is what makes
core 0 open a range owned by that core, and **a write is routed by the id
it will issue, not by which relation the statement names.** A range opens
only where a core that does not own the relation asks for a block, and
`OpenRangeOnSystemCore` declines whenever the asking core already owns the
top range — so a **single-core instance opens no range at any size**, and
neither does a relation only one peer ever writes. A write naming no
primary key on a *multi-owner* relation was refused.

**The read surface is a local walk** of every range (CC8). It was a fan-in
of stages over the remote-step protocol, with its own stage ceiling and
refusals (`LIMIT`/`OFFSET`, any sort but the pk ascending, a join, any read
inside an explicit transaction), until AT-S9 retired the route and AT-S10
the protocol (`git show 91111e3:docs/spec/crosscore.md` §6b).

**The mechanism.** When an `INSERT` omits its key the engine issues an
ascending one, so every such `INSERT` targets the relation's maximum id —
the tail range — and naive range ownership would spread reads while
leaving inserts single-core. The answer is built from the row-id block
leases (`catalog::RowIdLeaseTable`, demand-driven): each core inserts from
its own leased id block, and **ranges align to block boundaries**, so
every core appends to its own range's tail, fully locally. Invariant 3 is
satisfied per range because each range is **its own chain** with its own
tail — the per-range sub-structures CC8 names; `ChainInsert` refuses an id
below the tail page's `min_key`, so interleaved id blocks on one shared
chain would fail on the first insert. The leases supply the ids; the
per-range chains supply the tails. Consequences:

- Per-relation id monotonicity becomes per-range monotonicity
  (`docs/spec/heap-and-tuple.md` §4.1a): ids ascend per range and no
  longer in issue order across a relation. `sys.tables.next_id` stays one
  high-water mark, so ids remain globally unique across cores (K1) and
  `key_order` is untouched — every block is carved *above* the mark, so
  spreading never produces a below-mark key.
- A **btree** relation whose caller names its keys spreads naturally —
  those ids need not ascend — and needs none of this. A **heap** relation
  does not get that for free even when the caller names its keys: they
  must still be at or above the mark (`docs/spec/heap-and-tuple.md` §4.1),
  which is the same tail this section is about. The spreading problem is
  the chain's, not the issuer's.
- With spreading on, interleaved blocks are not opt-in: a range is
  information the user does not have, and opt-in would be a spelling the
  user would have to write.

## 7. Cancellation, Errors, Early Termination

**Nothing crosses to cancel since AT-S10.** A statement runs on its
session's core, so an early stop (`LIMIT`) is the walk's own quota, an
error is framed once by the session that ran the statement, and a
connection close tears down only that core's state (the same hook that
rolls back an open transaction). The cross-core rules that stood here -
`STEP_CANCEL` upstream at the `LIMIT`-th row, `STEP_ERROR` downstream and
`STEP_CANCEL` upstream on a step failure, a cancel for every live pipeline
at session teardown, and a failed batch send cancelling rather than
leaking its pipeline entry - went with the protocol; the record is
`git show 91111e3:docs/spec/crosscore.md` §7.

## 8. Determinism and Testing

Nothing the engine sends crosses a core since AT-S10b; the transport
itself is still exercised under the simulated ring seam
(`docs/spec/sched.md` §8): message delay and reorder injection, reactors
stepped round-robin on one thread. The list keeps its numbering;
the items that tested the remote-step protocol are retired with it at
AT-S10, their cells deleted with the services they pinned.

1. *(Retired at AT-S10: pipeline equivalence against single-core.)*
2. *(Retired at AT-S10: credit-bounded flow control.)*
3. *(Retired at AT-S10: cancel mid-stream and pipeline teardown.)*
4. *(Retired at AT-S10: one terminal error per request across stages.)*
5. **Write restriction:** a transaction writing ranges owned by two cores
   receives the retryable conflict error at the second write; the first
   write rolls back cleanly; the observability counter increments.
6. *(Retired at AT-S10: tag isolation between two pipelines.)*
7. *(Retired at AT-S10: the pipeline layer's zero-message fast path; there
   is no pipeline layer.)*
8. *(Retired at AT-S10: the per-stage RR weakening, which had no stage to
   apply to.)*
9. **Range equivalence:** every shape over a split relation returns
   byte-identical result sets to the same statement over the same rows
   unsplit — the split as the only variable, over data where matching
   rows straddle the boundary.
10. **Shared statement view:** a transaction writing two relations *or*
   two ranges of one relation commits while a statement reads them; the
   statement's answer contains all of that transaction's writes or none
   (§5's one view per statement, pinned against the torn read in both
   shapes).
11. *(No test: the engine does not migrate a range.)*
12. **Insert spreading:** k cores inserting concurrently each land in
   their own range's tail; ids ascend per range; ids stay globally
   unique (K1's issue-once contract across cores); invariant 3 holds
   per range.
13. **Split gates:** the allocator's admission check declines a btree,
   indexed, Bound-cabined, spilling, FK-linked or asserted relation, names
   the gate, and the relation stays one range (§6a — an engine decision,
   not a statement refusal: no token, no byte); on an eligible relation the
   allocator opens the second range and the directory rows appear (the
   lo = 0 row included, CC9), and an unaffected relation's fast path is
   byte-identical to before.

## 9. Open Items

The open decisions of this subsystem are unrecorded here.
