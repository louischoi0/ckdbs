# Cross-Core Execution

What crosses a core when a statement runs, and what does not. **Since AR0
M3 (AT-S5 to AT-S10d) the answer is almost nothing**: no relation, range or
page has an owner core; every statement runs, whole, on the core its
session is on; every core reads and writes every page under the page
latch; nothing is shipped, no transaction has a half on another core, and
there is no message between cores - one core reaches another only by
writing shared state and kicking it (`docs/spec/sched.md` §5). What still
crosses is a **connection**, once, where the port cannot be shared (CC1),
and a **kick**.

This spec keeps its decision rows because each records a rule the engine
used to enforce and says what replaced it. The mechanisms themselves -
statement shipping, the remote-step pipeline, range ownership and
id-block-aligned insert spreading, the cross-owner transaction - are
git's: `git show 2b20369:docs/spec/crosscore.md` is the last text that
described any of them, `git show 91111e3:docs/spec/crosscore.md` the last
that described the remote-step protocol whole, and
`docs/spec/cross-owner-txn.md` the retired commit protocol. Consistent
with `docs/rules/rules.md` (thread-per-core, core-local by default with
what is shared declared, no exceptions, deterministic testability).

**There is no ownership unit since AT-S9.** It was the primary-key range
(CC8), with `sys.tables.owner_core` the one-range case; both are retired,
and a range is now only a sub-structure of a pre-AT split relation.

## 1. Decisions

| # | Decision | Resolution |
|---|----------|-----------|
| CC1 | Execution model | **Every statement runs on the core its session is on** (AT-S5 writes, AT-S6 reads, AT-S9 the last two routes), **and no part of one runs anywhere else** (AT-S10). A session's core is the one that accepted its connection (every core listens with `SO_REUSEPORT`, AT-S8), or, where the port cannot be shared, the core core 0 handed it to (D19's fallback, AT-S10c; `include/kds/server/connection_handoff.hpp`, `sched.md` §5) - a connection crosses once, at placement, and never by a ring kind. The step pipeline this row named - each step on the core owning its range, output flowing to the next step's core - lost its last producers at AT-S9, the single-step fan-in over a split relation and the two-step join pipeline, both of which chose their cores by ownership; AT-S10 deleted the protocol itself (AT-0 item 4, answered *struck*). **There is no ring since AT-S10d** (§3): its last two kinds, the id leases, lost their users at AT-S10b when every core began issuing its own ids, and the transport went with them |
| CC2 | Intermediate transfer | **None since AT-S10**: no row crosses a core. It was KWP binary row batches (protocol D5) in chunked ring messages under credit-based flow control (§4). What stands is the rule it imposed: **one row encoding**, `wire/row_codec.hpp`, whose one consumer is now the KWP wire - result batches and the load stream's chunks (`protocol.md` §6, `bulkinsert.md` BI6) |
| CC3 | Write scope | A transaction's writes run on the core its session is on (AT-S5), and a transaction is one core's, whole (AT-S6). **Nothing is refused for touching two ranges or two relations** since AT-S9: the multi-owner refusals (a write naming no pk, a join, `LIMIT`/`OFFSET`, a sort, DDL on a relation with two or more ranges) went with the owners they described |
| CC4 | Remote-read isolation | **There is no remote read since AT-S10.** Every read takes the snapshot its own statement or transaction holds on the session's core (`txn.md` §4.1), so the per-stage view this row described - a remote step's latest-committed snapshot, minted per stage and outside any asking transaction - has nothing left to describe (§5) |
| CC5 | Cancellation & errors | **Nothing cross-core to cancel since AT-S10**: a statement's errors are its own core's, framed by its session. The `STEP_CANCEL`/`STEP_ERROR` propagation this row named went with the protocol (§7), and the tag `(session_core, request_id, step_id)` it routed by went with the message header at AT-S10d (§3) |
| CC6 | Scheduling | **Nothing crosses to be scheduled since AT-S10d**: a kick carries no group, and the woken core's own parked task runs in its own group. A ring message's task ran in the scheduling group its sender designated on the header until the transport retired; the rule before that - remote step tasks in `foreground`, because step chains are the OLTP path - retired with the protocol at AT-S10 |
| CC7 | Page ownership | **None, since AT-S9** (AR0-5 D17). Pages were the core's that `sys.tables.owner_core` named; that column's four bytes are reserved now - written 0, never read, so a pre-AT volume's value is ignored where it lies and the volume mounts unchanged. Every core reads and writes every page under the page latch (CC11, `page.md` §6). The history of how ownership was realized - the flush-then-grant handoff AW-S1b deleted, the owner-built index and assertion paths AT-S5d/S5e retired - is git's |
| CC8 | Ranges | **A pk range `[lo, hi)` is a sub-structure of one relation, and owned by nothing** (AT-S9). A heap range is its own chain with its own head, the entry page riding the directory row (CC9); a btree relation does not split (§6a). Ranges exist only on relations split before AT-S9 retired spreading (and, for a relation created since SUS-1, not even then). **Every walk covers every range** (`TableAccess::WalkHeads`), and a row lands in the range its id falls in (`HeapChainFor`, which refuses an id in no range) |
| CC9 | Range directory | `sys.ranges` (rel oid, lo, a reserved word where the owner core was, entry page; hi is the next row's lo, and a **non-empty directory carries a row at lo = 0**, so the rows partition the whole id space) records every split a pre-AT volume made; a relation with no rows there is one range headed by `sys.tables.desc_page_id`. Resolved from the executing core's catalog cache. A directory write bumps the schema version word as DDL does (`catalog.md` CT2) |
| CC10 | Range split | **No range is opened since AT-S9**, which retired insert spreading - the only path that created one - with range ownership, on the operator's ruling. `range_size_ids` is refused by name. What a split relation already has is read and written whole; `Catalog::OpenRangeRows` survives with no production caller, as the way a cell builds the state a pre-AT volume can hold. The rules this row carried for opening a range (a page-boundary split, the durable row before the grant, the pre-grant Cabin discard) are git's |
| CC11 | Shared-structure access | **Every core reads and writes with the same authority** (AT-S5; AR0-5 §0). The rule that stood here until then — *core 0 alone writes the superblock, the free map and the catalog pages, and one writer is the serialization mechanism* — is retired with the store's `MayWrite` arm that enforced it (the predicate itself, answering yes from then on, deleted at AT-S18) and the `catalog_read_only_` flag that routed around it. What serialises each of the three now is named where it is written: the **page latch** across cores for the bytes of any page (`page.md` §6), and within one core the rule that no task parks under a page span, which is what makes a catalog row's read-modify-write atomic (`catalog.md` CT5, `AdmitExplicitRowId`'s hook); the free map's own `map_latch_` for allocation (AM-S3, `page.md` §5); the **schema version word** for every core's memo of the catalog (`catalog.md` CT2), which replaced the invalidation broadcast at AT-S2; and page 0's persisted transaction-id ceiling, which every core's `TrxIdSequence::Carve` reads and raises in one step under the superblock latch since AT-S10b (`txn.md` §4.2) — core 0 alone carved until then, by the lease's routing. A relation's row-id mark is a catalog row like any other, bumped in place under its page latch by whichever core issues (`heap-and-tuple.md` §4.1a). DDL runs where the session is, under the DDL's own relation `X` (`txn.md` §5) where it has one; a name-creating DDL - `CREATE TABLE`, `RENAME TO`, `CREATE NAMESPACE`, `RENAME COLUMN`, an index's or an assertion's name - checks and writes the name under one hold of its catalog relation's root page since AT-S17 (`catalog.md` CT7). A peer's fill path was already the shared frame (AM-S2 step 3) and is unchanged. A split relation's range chains are pages like any other, under the same latch; a btree relation never split (CC8) |
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

## 2a. Ranges on a Pre-AT Split Relation

A relation split before AT-S9 keeps its ranges, and nothing opens, merges
or moves one (CC10). What such a relation does now:

- **A read walks every range** (`TableAccess::WalkHeads`, CC8), on the
  session's core, so no walk answers short and none is refused for where
  it reads.
- **A write lands by its id.** A named pk lands in the range its id falls
  in (`HeapChainFor`, which refuses an id in no range); an omitted pk is
  issued from the relation's one row-id mark (`heap-and-tuple.md` §4.1a),
  which is above every range's `lo`, so it lands in the top range.
- **The directory is read-mostly.** `sys.ranges` is read from the
  executing core's catalog cache (CC9). **A resolved range is never cached
  across a suspension**: a peer's cache is dropped by the schema word's
  `Revalidate()` at a task boundary, which never advances
  `catalog_version()` - that counter is one core's (`catalog.md` CT1) -
  so a range fact guarded by it would be wrong on every peer.
- **Advisory structures never narrow a write's target set.** Invariant
  9's licence is *where to look*; for a write, executing on fewer ranges
  than hold matching rows is a missed write, not a slower one. DML target
  resolution is pk arithmetic against the directory alone. A Cabin set
  speaks for every range of its relation (`cabin.md` §4b) and a
  Waystone trail names pages, neither of which knows a core.

The routing this section carried - a range naming its owner core, a
fan-in over the owners, a trail missing on the owner check - is git's
(`git show 2b20369:docs/spec/crosscore.md` §2a).

## 3. Messages

**There are no messages since AT-S10d**: the ring transport, its header
and its kind enum are deleted (`docs/spec/sched.md` §5), and a core that
needs another to act writes shared state and kicks it. The last two kinds,
`kTrxIdLease` (18) and `kRowIdLease` (22) - a peer's request to core 0 for
a block of transaction ids or of one relation's row ids, and core 0's grant
on the same kind - lost their users at AT-S10b, when every core began
carving its own transaction-id window and bumping a relation's row-id mark
in place (`txn.md` §4.2, `heap-and-tuple.md` §4.1a); they stayed
enumerated one sub-stage as the transport cells' stand-ins and went with
the transport. The seven this section tabulated until AT-S10 -
`STEP_OPEN`, `STEP_BATCH`, `STEP_EOF`, `STEP_CREDIT`, `STEP_CANCEL`,
`STEP_ERROR` (1-6) and `SHIPPED_ROW_DESC` (40) - went with the protocol.
The enum as it last stood, every struck value recorded where it stood, is
`git show ba8c824:include/kds/sched/ring_message.hpp`.

## 4. Transfer Format and Flow Control

**No rows cross a core since AT-S10**, so this section has no format and
no flow control left to state: the step batch, its 32 KiB target and
ring-slot ceiling, and the per-edge credit protocol (4 initial credits,
preallocated at `STEP_OPEN`) went with the protocol. The one-encoding rule
they obeyed stands as CC2's. The transport's own send rule - a successful
send wakes a sleeping destination, a refused one wakes nobody, ring-full
yields and retries - retired with the transport at AT-S10d; a kick's rule
is `docs/spec/sched.md` §5 and its invariant 7.

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

**No statement crosses, and no write is refused for where it lands.** A
read and a write both run on the core their session is on, through the one
frame table, and a transaction is one core's, whole (CC3). Two writers on
two cores meet at the page latch for a page's bytes and at the lock family
for a row or relation (`txn.md` §5) - the same two places two writers on
one core meet.

**Statement shipping was how a statement reached a relation another core
owned**, and it retired in two halves:

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

**What went with ownership at AT-S9**: the cross-core write refusal
(`CheckWriteAffinity` and `CrossCoreWriteRefused`, for a write reaching a
range this core did not own), its three `SHOW META` counters, and the
multi-owner refusals CC3 lists. What the shipping mechanism cost - the
refusals it converted, the `UNKNOWN_OUTCOME` a lost answer had to be, the
per-(arrival core, session) dedup record - is
`git show c63e49f:docs/spec/crosscore.md`; the refusal and its counters are
`git show 2b20369:docs/spec/crosscore.md` §6.

**Foreign keys** (`docs/spec/foreign-keys.md`): a parent and a child on
two relations is **not a crossing at all since AT-S5f**. The forward check
still hoists to the dispatch fork - for deduplication, and because the
wait has to happen where nothing has been written yet - and descends every
parent here; the reverse check walks every chain of every child here.
Nothing is probed and nothing is left behind, and the
validation-to-commit window closes on the child row's own header: an
uncommitted child answers a parent's `DELETE` busy because the walk reads
it, where a reference intent on the parent's owner used to say so
(`foreign-keys.md` §2a/§3a). Nothing inside a `WriteScope` waits.

### 6a. What May Split

**Nothing splits since AT-S9**: no range is opened (CC10), so the split
gates this section carried - btree, secondary index, Bound Cabin,
spilling var-heap (`SchemaCanSpill`), foreign key and assertion, checked by
`RangeEligible` at the range allocator's admission - have no admission
left to guard, and went with the allocator. They existed because a
write-coupled auxiliary lived on its relation's one owner core, and a
split would have given it two; with no owner, an auxiliary is written by
whichever core runs the statement, under the latch or registry its own
spec names (`index.md`, `cabin.md`, `assertion.md` §6.1,
`foreign-keys.md`). A pre-AT split relation that carries one keeps it,
walked whole. The gates' text is `git show 2b20369:docs/spec/crosscore.md`
§6a.

### 6b. Inserts and the Tail

**Retired: id-block-aligned spreading**, at AT-S9 (`range_size_ids` is
refused by name), and the row-id leases it was built from at AT-S10b.
Every core issues an omitted pk from the relation's one row-id mark,
bumped in place under its page latch, so **the pk is an identity and a
sequence**, monotonic in issue order across every core
(`docs/spec/heap-and-tuple.md` §4.1, §4.1a; invariant 11). The mechanism -
each core inserting from its own leased block into its own range's tail,
ranges aligned to block boundaries - is
`git show 2b20369:docs/spec/crosscore.md` §6b.

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

Nothing the engine sends crosses a core since AT-S10b, and the transport
and its simulated ring seam are deleted since AT-S10d; what crosses is a
kick, whose deterministic shape is the two-core rig's seeded
`SimWakerTable` (`docs/spec/sched.md` §8). The list keeps its numbering;
the items that tested the remote-step protocol are retired with it at
AT-S10, their cells deleted with the services they pinned.

1. *(Retired at AT-S10: pipeline equivalence against single-core.)*
2. *(Retired at AT-S10: credit-bounded flow control.)*
3. *(Retired at AT-S10: cancel mid-stream and pipeline teardown.)*
4. *(Retired at AT-S10: one terminal error per request across stages.)*
5. *(Retired at AT-S9: the cross-core write restriction; no range is
   owned.)*
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
12. **One sequence across cores** (it was *insert spreading* until AT-S9
   retired spreading): k cores inserting omitted-pk rows into one
   relation at once issue ids unique across cores
   (K1's issue-once contract) and ascending in issue order (§6b).
13. *(Retired at AT-S9: the split gates; no range is opened.)*

## 9. Open Items

The open decisions of this subsystem are unrecorded here.
