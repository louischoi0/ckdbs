# Cross-Core Execution

> **AT-S9 (2026-09-24): ownership and placement are retired**, and with them
> the routes this spec was written for. No relation or range is owned by a
> core (`owner_core`'s bytes are reserved), every statement runs on the
> core its session is on, no range is opened (insert spreading retired),
> and the single-step fan-in and two-step pipeline are gone. **The
> decision rows below state what is true now; §2, §2a, §3-§5, §6's
> multi-owner rules and §6b describe the retired mechanism** and are
> AT-S12's to rewrite (`instructions/v3.0.0/workorder-at-m3-uniformity.md`).
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
| CC1 | Execution model | **Every statement runs on the core its session is on** (AT-S5 writes, AT-S6 reads, AT-S9 the last two routes). The step pipeline this row named - each step on the core owning its range, output flowing to the next step's core - has no engine producer since AT-S9: the single-step fan-in over a split relation and the two-step join pipeline were the last two routes that opened one, and both chose their cores by ownership, which is gone. The `kStep*` services and their wire remain until AT-S10/S11 delete the transport (AT-0 item 4, answered *struck*) |
| CC2 | Intermediate transfer | **KWP binary row batches (protocol D5 encoding) in chunked ring messages + credit-based flow control** |
| CC3 | Write scope | A transaction's writes run on the core its session is on (AT-S5), and a transaction is one core's, whole (AT-S6). **Nothing is refused for touching two ranges or two relations** since AT-S9: the multi-owner refusals (a write naming no pk, a join, `LIMIT`/`OFFSET`, a sort, DDL on a relation with two or more ranges) went with the owners they described |
| CC4 | Remote-read isolation | A remote step reads the **latest committed snapshot** as of the moment its stage mints one — since AN-S2 a commit-LSN snapshot over the instance read view, which covers every core's commits; no view crosses a core. RC-equivalent; RR weakening documented (§5). Per range: each stage mints its own |
| CC5 | Cancellation & errors | Cancel/error messages propagate both directions; every message tagged `(session_core, request_id, step_id)`; stale batches discarded by tag |
| CC6 | Scheduling | Remote step tasks run in the **foreground** group on their core (step chains are the OLTP path) |
| CC7 | Page ownership | **None, since AT-S9** (AR0-5 D17). Pages were the core's that `sys.tables.owner_core` named; that column's four bytes are reserved now - written 0, never read, so a pre-AT volume's value is ignored where it lies and the volume mounts unchanged. Every core reads and writes every page under the page latch (CC11, `page.md` §6). The history of how ownership was realized - the flush-then-grant handoff AW-S1b deleted, the owner-built index and assertion paths AT-S5d/S5e retired - is git's |
| CC8 | Ranges | **A pk range `[lo, hi)` is a sub-structure of one relation, and owned by nothing** (AT-S9). A heap range is its own chain with its own head, the entry page riding the directory row (CC9); a btree relation does not split (§6a). Ranges exist only on relations split before AT-S9 retired spreading (and, for a relation created since SUS-1, not even then). **Every walk covers every range** (`TableAccess::WalkHeads`), and a row lands in the range its id falls in (`HeapChainFor`, which refuses an id in no range) |
| CC9 | Range directory | `sys.ranges` (rel oid, lo, a reserved word where the owner core was, entry page; hi is the next row's lo, and a **non-empty directory carries a row at lo = 0**, so the rows partition the whole id space) records every split a pre-AT volume made; a relation with no rows there is one range headed by `sys.tables.desc_page_id`. Resolved from the executing core's catalog cache. A directory write bumps the schema version word as DDL does (`catalog.md` CT2) |
| CC10 | Range split | **No range is opened since AT-S9**, which retired insert spreading - the only path that created one - with range ownership, on the operator's ruling. `range_size_ids` is refused by name. What a split relation already has is read and written whole; `Catalog::OpenRangeRows` survives with no production caller, as the way a cell builds the state a pre-AT volume can hold. The rules this row carried for opening a range (a page-boundary split, the durable row before the grant, the pre-grant Cabin discard) are git's |
| CC11 | Shared-structure access | **Every core reads and writes with the same authority** (AT-S5; AR0-5 §0). The rule that stood here until then — *core 0 alone writes the superblock, the free map and the catalog pages, and one writer is the serialization mechanism* — is retired with the store's `MayWrite` arm that enforced it and the `catalog_read_only_` flag that routed around it. What serialises each of the three now is named where it is written: the **page latch** across cores for the bytes of any page (`page.md` §6), and within one core the rule that no task parks under a page span, which is what makes a catalog row's read-modify-write atomic (`catalog.md` CT5, `AdmitExplicitRowId`'s hook); the free map's own `map_latch_` for allocation (AM-S3, `page.md` §5); the **schema version word** for every core's memo of the catalog (`catalog.md` CT2), which replaced the invalidation broadcast at AT-S2; and page 0's persisted ceiling, which AT-S4 advances by CAS from whichever task crosses it — until then `TrxIdSequence::Carve` runs on core 0 by the lease's routing. DDL runs where the session is, under the DDL's own relation `X` (`txn.md` §5). A peer's fill path was already the shared frame (AM-S2 step 3) and is unchanged. Not covered: a *relation's* shared structure — the btree's top levels under a split relation (CC8) |
| CC12 | Catalog page placement | **CR1 — a catalog relation's root page stays in the reserved range; its var-heap does not.** `kCatalogPageTypes = 4` through `kCatalogPageRanges = 15` (`include/kds/catalog/well_known.hpp`) sit below `kFirstUserPageId = 128` because they must be findable at bootstrap *without* a catalog read. A catalog relation's **var-heap** root is allocated through `CreateNew()` and recorded in `sys.tables`, binding on any catalog relation with a var-heap. The cost this carried — a page outside the range was not peer-readable, so the one peer-readable catalog var-heap, `sys.assertions`, was reached by granting the individual pages a row names — **went with the fault grants at AW-S1b**. Every core faults every page, so a var-heap root's id no longer decides who can read it. `exec::CatalogSpillPages` survives with its other consumer, the mount's var-heap sweep. **CR2 is retired at AT-S5, and its sentence stood here until AT-S5b** — *DDL executes on core 0; a peer sends and waits*, shipping the request and falling back on `PeerDdlRefused` for what the ship did not cover. DDL runs where the session is (CC11, CC13's CR5), both the ship and the refusal are gone, and a peer that has to **grow** a catalog chain places the page itself since AT-S5b (`catalog.md` CT5). **CR3 — a catalog page may leave the reserved range once grown.** Catalog pages are allocated and initialised inside the range at bootstrap; beyond bootstrap a catalog page **may be managed outside it** in either of two cases — the relation grows past what the reserved range holds, or every peer must read *and write* the page equally. Such a page takes the ordinary relation rules: allocation from the general supply, free-map accounting, and the WAL logging catalog change already has. No catalog page has left the range; a var-heap root outside it is found through `sys.tables` — the indirection `varheap_page_id` uses, DDL-immutable and therefore cacheable per `rows.hpp` |
| CC13 | DDL's route, and where a core's statistics are written | **CR5 is retired at AT-S5**: a peer routed DDL to core 0 by shipping the statement, detected at dispatch on `catalog_read_only_` and the `CREATE`/`ALTER`/`DROP` tokens, with `PeerDdlRefused` for what the ship did not cover; DDL runs where the session is now (CC11). **CR6 — a catalog relation whose content is only a statistic is written unlogged**, the sole exception to the rule that catalog writes are WAL-logged and replayed (`docs/spec/ddl-transactional.md` §7; the rule text is `docs/rules/rules.md` §5). **CR7 and CR8 are retired at AT-S7.** CR7 had a peer batch its access shapes locally and flush them to core 0 over `kAccessStatsBatch`, because `sys.access_stats` sits at page 11 inside the reserved range and only core 0 could write it; CR8 made that send the engine's one **droppable** message, invariant 8 pricing a lost statistic as performance and never a result. Every core writes every page since AT-S5, so **a core writes its own access statistics where the statement ran** - the batch, the ring kind, the drop and the five `SHOW META` counters are gone, and a peer's count is in the row before its statement returns rather than on the next tick. What stands from CR7 is the part that was never about the wire: **`sys.access_stats` is one relation at page 11 (`kCatalogPageAccessStats`)** and there are no per-core statistics relations - `SysAccessStatRow` is a 33-byte fixed row with no `core_id` field (`catalog/rows.hpp`), so every core's counts **fold into the one `(kind, rel_id, column_mask)` shape**. `Catalog::RecordAccess` holds that relation's root page exclusive for its whole body, which is what makes the fold and the admission of a shape nobody has seen one act across cores. `RecordAccess` costs +1-2% on a point lookup (`docs/spec/heap-and-tuple.md` §7) |

## 2. Execution Model

The session core owns the statement end to end: it parses, resolves the step
chain (the written-order contract of `docs/spec/parser-v2.md` — *the statement is
the chain*, never silently reordered), and resolves each step's range(s) and
their owner cores against the directory (§2a) from its catalog cache at plan
time.

A step spanning k ranges owned by other cores is a **fan-in** of k sibling
stages, each with its own edge and credit, bounded by `kMaxFanInUpstreams`
(255, `include/kds/server/remote_step_service.hpp`) — so a spread
relation's readable size is `kMaxFanInUpstreams × range_size_ids`.
Range-order concatenation of stage outputs is *deterministic* because
ranges are disjoint and key-ordered, but it is not free: when the statement
requires key order, later ranges buffer until earlier ranges finish.
`emit_in_key_order` is not this mechanism — it is a per-step page-local
ordering flag that is an explicit *shipping refusal* in both paths.

Two paths:

- **Local fast path.** Every referenced range is owned by the session
  core. Execution is exactly the single-core code — the cross-core layer
  must add zero work here. This is an invariant, not an optimization note:
  the single-core path must not regress in instructions or allocations.
- **Pipeline path.** At least one step's range lives on another core.
  Each remote step receives a `STEP_OPEN` describing it (relation,
  predicate bindings, projection column set, downstream target), wiring
  step k's output to step k+1's core. **The opens are chained, not fanned
  out.** The session core opens only the *final* stage; every stage's
  envelope encloses its upstream stage's complete open, which the
  receiving core forwards once its own pipeline state exists. That
  ordering is what makes "no batch before its consumer" structural —
  §3's teardown rule silently discards an unmatched batch, so two
  independently raced opens would lose rows, not fail. The last step's
  downstream is the session core, which frames rows to the client (KWP,
  protocol D6 chunked streaming); `STEP_CANCEL` stays point-to-point from
  the session, which knows every stage's core from its own plan.

What flows between steps is not whole rows: step k forwards, per row, the
join key consumed by step k+1 plus only the columns the final projection
needs from step k's relation. Step k+1 performs its lookup (pk descent,
Waystone/Cabin hint, or scan per the plan) against its **local** state with
its **local** trail/statistics recording. **Neither crosses, and neither
needs to** (AT-S7): a core writes `sys.access_stats` and `sys.patterns`
itself, under those relations' root page latches, so what a stage records
is in the instance's one relation before it replies. CR7's fold-and-flush
to core 0 and the reason it existed - a peer could not write a catalog
page - are both gone (CC13). The trail half never could have crossed
anyway: `RegisterPattern` hands the recorder a pointer it uses
immediately, so it needs an answer, and a stage cannot wait for one.

A pipeline is torn down when the session core has framed the final row,
received `STEP_ERROR`, or issued `STEP_CANCEL` (§7).

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
  own banked-authority rules, for the ranges its core owns
  (`docs/spec/cabin.md` §4a, §4b, §6a), and the rest fall through to their
  own stages; a Waystone trail names pages, a page names a range, a range
  names a core — advisory per invariant 9, and a trail replayed on the
  wrong core misses on the epoch/owner check and falls through, the
  ordinary miss discipline. A split relation carries no secondary index
  (§6a), so no index probe routes on one.
- **Advisory structures never narrow a write's target set.** Invariant
  9's license is *where to look*; for a write, executing on fewer
  ranges than hold matching rows is a missed write, not a slower one.
  DML target resolution is pk arithmetic against the directory alone
  (§6).

## 3. Messages

All pipeline traffic rides the per-core-pair SPSC rings (`docs/spec/sched.md` §5).
Message kinds:

| Kind | Direction | Payload |
|------|-----------|---------|
| `STEP_OPEN` | downstream stage → its upstream's core (the session opens the final stage, §2) | step descriptor: relation oid, bindings, projection set, downstream core+step; optional upstream section (forwarded-row layout, enclosed upstream open); optional output spec — standalone, beside the upstream section, because a *leaf* seals its consumer's input layout too; absent = whole row. The head's `downstream_step` is read at the consuming stage: the enclosed open must address the stage that forwards it, or the open is refused |
| `STEP_BATCH` | step k → step k+1 (or session) | chunk of KWP-encoded rows (§4) |
| `STEP_EOF` | upstream → downstream | no more batches for this step |
| `STEP_CREDIT` | downstream → upstream | grants N batch credits (§4) |
| `STEP_CANCEL` | any → any in pipeline | stop producing/consuming; discard tagged state |
| `STEP_ERROR` | failing core → downstream chain + session | Status code + retryable flag (protocol D9 mapping) |
| `SHIPPED_ROW_DESC` (`kShippedRowDesc`) | upstream → the core that opened the stage | one chunk of the answer's row description, ahead of the first batch on the answer edge, with its own sequence (§4a). The name is the shipped read it was written for and the kind outlived it (AT-S6) |

Every message carries the tag `(session_core, request_id, step_id)`.
`request_id` is allocated per statement by the session core, sequential per
core (never pointer-derived — `docs/spec/sched.md` §8 determinism rules). A core
receiving a batch whose tag matches no live pipeline state discards it
silently; this is the teardown correctness rule, not an error.

## 4. Transfer Format and Flow Control

- A `STEP_BATCH` payload is rows in the **KWP binary encoding (protocol
  D5)** — the same encoder the wire path uses, applied to the forwarded
  column set. `wire/row_codec.hpp` is the D5 encoder and
  `include/kds/wire/kwp.hpp` the frame codec around it. One encoder, two
  consumers; no second row format — a private batch format is refused,
  which is what lets a stage's rows reach a KWP client without
  being re-encoded (§4a).
- Batch size: `kStepBatchTargetBytes` (32 KiB) is the target, always ≤ the
  ring's max message payload (`StepBatchCeiling`). A row larger than the
  target still ships alone (var-heap spill values are re-inlined into the
  batch by the producing step — the consumer never chases a var-heap
  reference into a page it does not own).
- **Credit-based flow control**, separate from ring backpressure: a
  downstream step grants `STEP_CREDIT` as it drains; an upstream step never
  sends a batch without holding a credit. Initial credit is
  `kInitialCreditsPerEdge` (4 batches per edge). Ring-full on send follows
  the global rule: the sending task yields and retries; it never blocks
  the reactor and never drops.
- Rationale: ring backpressure protects the *transport*; credits bound the
  *per-request* buffering so one fat pipeline cannot exhaust a peer core's
  batch memory. Credit memory is preallocated per edge at `STEP_OPEN`.
- **A successful send wakes a sleeping destination; a refused one wakes
  nobody** (`docs/spec/sched.md` §7 and its invariant 7). The send stays
  non-blocking and fallible, and the wake follows the push, so a message
  never waits out the destination's idle block. The refused case is
  deliberate: waking a core for a message that is not in the ring is the
  spin the wake exists to remove, moved to the sender.

### 4a. The answer edge

A read a **stage** answers for another core - a fan-in over a split
relation, or a two-step join's consuming stage - delivers **rows on an
answer edge over this same step wire**, and nothing about that wire
changes. The asking core mints the `PipelineTag` and registers the
receiver *before* it opens the stage; the owner installs a batch sink and
sends `STEP_BATCH` under the existing credit protocol. Codec, batch
builder, credit grant, `STEP_CANCEL` and the ceiling are reused unchanged.

**A shipped read was the fifth producer on it until AT-S6**, and that is
where this section's shape comes from: the arrival core shipped the
statement with the tag on the request, the owner streamed the rows back,
and the ship reply POD arrived last as the terminator carrying the status.
Nothing ships now (§6), so the terminator arm and the producer went; the
edge is the pipeline's alone.

**Buffered on the owner and sent under credit, not streamed row by row.**
A `ResultSink` is called from inside the executor's row callback and **has
no suspension point**, so nothing on that path can park on credit. The
rows are sealed into batches as the statement runs and queued on the edge
when it finishes; what parks is the drain, which resumes on each
`STEP_CREDIT`. The consequence is visible at the other end: **the
terminator can outrun the rows**, so the arrival core waits for the edge's
own EOF as well as for the reply, under the same 10 s deadline.

**The rows reach the client's sink without being re-encoded**, which is
what one row encoding buys. A `ResultSink` answers whether its `Emit` takes
that encoding (`AcceptsEncodedRows`, **false by default**, so a sink
written without reading this gets the safe answer); a sink that says yes
is handed the edge's bytes as they arrived. The text form says no — its
`Emit` takes rendered text, and a text session's reader never asks. Row boundaries come from
`wire::DecodeRowExtents`, the same walk `DecodeRowBatch` performs, in the
file that owns the format, so nothing reads the row format twice.

*Why an edge and not a bigger reply.* Both ship PODs fill exactly one ring
slot, so the reply carries **1000 bytes** (`kShippedStatementReplyTextMax`;
992 until AN-S2 took RR0's eight-byte watermark off the reply) and cannot
hold a result set at all. The rows cross on the thing that already batches.

**The tag is minted by the arrival core, not the owner**, because
`SessionStepClient` discards a batch whose tag matches no open read. The
owner cannot name a receiver that does not exist yet, so registration
precedes the ship and the tag rides the request.

**The description crosses first, as its own message, chunked.** A
projected read's field list is not derivable on the arrival core — only
the owner compiled the statement — so it crosses ahead of the first batch.
The engine has **no column-count cap**, so a description can exceed one
ring message; it crosses as an **ordered sequence of description chunks**
(`kShippedRowDesc`, §3), reassembled before the receiver is armed. No
ceiling is named and no constant is added: chunking is what answers the
bound. *A description chunk is its own message kind and carries its own
sequence*, not a `STEP_BATCH` with a flag: `StepBatchHeader::seq` is
per-edge and contiguous — a receiver that sees a gap has lost a batch,
which the ring's FIFO makes impossible per edge, asserted, not handled
(`step_pipeline.hpp`) — so folding a differently-shaped payload into that
sequence would either break the assertion or force description chunks to be
counted as batches. Two kinds, two sequences, one tag.

**Sizing is the edge's own**: `kStepBatchTargetBytes` (32 KiB) as the
target, `StepBatchCeiling` of the transport's slot as the bound. The KWP
socket's 64 KiB batch target is a socket-side quantity and bounds nothing
on a ring.

**The request POD pays 16 bytes for the tag**, since AN-S3 8 for the
coordinator's snapshot (`cross-owner-txn.md` §1a), and since AO-S4b 8 for
the coordinator's transaction id (the wait-for edge only the owner can
record), so the longest shippable statement is **960 bytes** — 992 before
the tag, 976 before the snapshot, 968 before the id. That bound is
client-visible and is stated in `docs/spec/client-manual.md`.

**The text arm does not move a byte.** A session with no result sink ships
`form = 0`, the owner installs no sink, and the rendered-text reply is
byte-identical to the newline protocol's — including its own whole-reply
cap, which is a debug surface's limit and stays one.

**Fail closed, never misparse.** `form = 0` is rendered text and `form = 1`
is typed; **any other value is refused by name**, and an owner that cannot
serve `form = 1` answers `Unsupported` on the reply POD and opens no edge,
so the arrival core refuses the statement rather than delivering a shape
the client cannot branch on. The same posture the `role` byte on this wire
takes.

**A duplicate read re-executes rather than being answered from a record.**
The dedup record (§6) keeps running for every write; a typed read is
exempt — `docs/spec/cross-owner-txn.md` §1a says why and what it bounds.
The owner cannot tell a *text-arm* read from a write, because nothing on
the request says so, so a text read keeps its record, bounded at
`kShippedStatementReplyTextMax` either way.

**What is refused**, by name, so a client sees one rule rather than a
surprise:

1. **a row wider than `StepBatchCeiling`** — 1,000 bytes at the 1,024-byte
   slot. The cap does not vanish; it is on the **widest row** rather than
   on the whole reply;
2. **a result that misses the 10 s deadline** — `UnknownOutcome`, and for
   a read it still says the read returned nothing and changed nothing;
3. **`ANALYZE` of a foreign relation**, which would describe a run this
   core did not perform;
4. **a join over a spread relation** (§6b).

**Cancellation has no new case.** The deadline closes the registered tag
(`STEP_CANCEL` where the read is still open remotely), a mid-result owner
failure means the reply carries a non-OK status and the arrival core
forwards **nothing** — a partial result set must not reach a client as a
whole one — and a client disconnect closes the tag on the
session's own teardown path.

## 5. Isolation Semantics

A remote step reads what is committed at the moment its stage mints its
view. Since AN-S2 that view is a commit-LSN snapshot over the instance read
view (`docs/spec/txn.md` §4.1): it would answer the same on any core, and
what keeps it core-local is the one-view-per-stage rule below, not the
view's shape. (Until AN-S2 the sentence here read "the trx-id domain is
global, so ids compare cleanly" — a per-core view over per-core id blocks,
which `workorder-an-read-view.md` AN-3 E shows compared nothing across
cores.)

**One view per stage**, minted when the stage's coroutine first runs and
held for that stage's whole life. Re-minting per batch would not be a
weaker promise but a wrong one: a stage parks mid-relation, and a view
that moved across the park could show the same row twice or skip it
entirely, depending on which side of the boundary a concurrent commit
landed. One view per stage is what makes a stage's output one statement's
answer. Two consequences: the window between `STEP_OPEN` and that first
poll is not covered, so a transaction committing inside it *is* visible
where a local statement's view would have excluded it; and each stage of a
multi-stage pipeline mints its own, so two stages of one statement on two
cores can disagree about a concurrent commit — the per-core weakening
below.

Two stages of one statement land on the *same* core whenever that core
owns both relations, or two ranges of one relation, and each mints its
own view. A transaction on that core writing both and committing between
the two mints is observed **torn** — a wrong answer, not a weakening. The
rule is **one view per (statement, core)**: all execution of one
statement on one core — every stage *and* the session core's own local
half of a mixed plan — shares that core's view, minted at the first
execution to poll. Core-local, so no view crosses a core and CC4's
sentence stays true. The engine does not share the view: each stage mints its
own, and §8's test 10 is the requirement it does not meet.
Across cores nothing is observed torn: CC3 binds a transaction's writes to
one **owner**, so no cross-core pair of views can split them. (Under the
old topology that owner was also a distinct WAL stream, which is how this
sentence used to read; with one stream per instance — `wal.md` §3 — the
binding that matters is the owning core, which is unchanged.)

One level down, a cross-range read of one relation is a cross-core read,
and it never meets REPEATABLE READ at all: a multi-owner relation refuses
every read inside an explicit transaction (`CLAUDE.md`'s range row), so the
per-stage weakening above is an autocommit statement's and no RR
transaction's.

- **READ COMMITTED** statements: semantically equivalent to local execution —
  RC already permits each statement (and each lookup within it) to observe
  the latest committed state.
- **REPEATABLE READ** transactions: one view, on one core. The
  transaction pins its snapshot at `BEGIN` and every statement of it runs
  where the session is, so "one instant on every core" is true by having
  only one core to be true on.

  **It was a protocol until AT-S6** (AN-S3): a cross-owner RR transaction
  carried its coordinator's snapshot on every shipped statement and each
  participant adopted it when it opened its context. Before AN-S2 the
  promise was consistent *per core* - each participant minted its own view
  at its own BEGIN - and two such transactions could disagree about the
  order of two commits on two cores; AN-S2 made the view instance-wide and
  AN-S3 carried the snapshot across. Nothing ships now, so nothing carries
  and nothing adopts.

  **The remote-step pipeline does not run inside such a transaction.** It
  reads each core's latest-committed snapshot outside the asking
  transaction, so a stage cannot show that transaction's own uncommitted
  rows - which inside an explicit transaction is a **wrong answer**, not a
  weakening. **So both remote-read fast paths were skipped for a session
  that could enrol, and the read shipped instead** - which is the arm
  AT-S6 removed, because the shipped read could not show those rows
  either. What a session inside a transaction gets now is a local walk,
  which is the one reader that can see them.
- Catalog: the plan is resolved entirely on the session core from its
  catalog cache; a remote step trusts the descriptor in `STEP_OPEN` and does
  not re-resolve. DDL invalidation between resolve and execute surfaces as a
  normal step error (stale oid → `STEP_ERROR`, retryable).

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

**What is still refused, and it is the only shape left**: a read of a
**split** relation this core does not wholly hold, in a statement the
fan-in cannot take (`CheckReadAffinity`). A walk here would cover this
core's ranges and answer short, which is the one ending that section may
not leave open.

**Target resolution is pk arithmetic against the directory alone** (§2a):
a DML on a split relation whose predicate does not bound its rows to one
owned range is a cross-core write, refused retryably.

- A write to a range this core does not own, reaching `CheckWriteAffinity`
  without having been shipped, is refused `CrossCoreWriteRefused`
  (retryable, protocol D9; the same client contract as first-updater-wins
  aborts in `docs/spec/txn.md`) and recorded. Writes to any ranges the home
  core owns — of one relation or several — are legal: they are
  single-owner, and nothing 2PC-shaped is in them (§5's shared statement
  view is the reader-side half of that claim). Reads inside the
  transaction remain free to pipeline cross-core under §5.
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
  set is authoritative for **(observed value × the ranges its core
  owns)**, so a boundary narrows what a set speaks for instead of
  falsifying it (`docs/spec/cabin.md` §4b); ranges the serving core does
  not own fall through to their own stage, and the sets banked under the
  whole-relation claim are dropped in CC10's pre-grant window. A **Bound**
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
neither does a relation only one peer ever writes. Two limits: a read of a
spread relation is bounded by the fan-in's stage ceiling
(`kMaxFanInUpstreams`, 255; §2), and a write naming no primary key on a
*multi-owner* relation is refused.

**The read surface.** Every core constructs a `SessionStepClient`, and the
route asks `TableAccess::ServableBy(core)` — *can a walk on this core alone
answer this relation whole* — so a run of ranges the reader owns becomes a
**self-directed stage**: the ordinary protocol with the ring hop being a
self-send, costing an upstream slot like any other stage. **Served from
every core**: `SELECT *` with an optional `WHERE`, `BETWEEN`, a free
`ORDER BY <pk> ASC`, any projection, and every aggregate. **Refused**:
`LIMIT`/`OFFSET` and any `ORDER BY` but the pk ascending, both because a
quota and a sort apply at **emission** while the remote side emits
everything in its own order; a **join** over a spread relation (the
two-step pipeline does not plan a spread relation as a stage); and the
route **inside an explicit transaction**, because each stage mints its own
latest-committed view and no snapshot is forwarded to the pipeline.

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
  out of the drain loop.** Breaking alone leaks one `pipelines_` entry per
  failed send — `SendFn` takes its payload by value, so the queue head is
  left a moved-from vector that nothing pops and the EOF-and-erase arm
  never runs — which §8's test 3 forbids. Cancelling routes teardown
  through the path a cancel already takes: the producer erases if one is
  live, the drain itself otherwise. Pinned by
  `RemoteStepServiceTest.ABatchSendThatFailsTearsThePipelineDownInsteadOfLeakingIt`.

## 8. Determinism and Testing

All of this must run under the simulated ring seam (`docs/spec/sched.md` §8):
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
5. **Write restriction:** a transaction writing ranges owned by two cores
   receives the retryable conflict error at the second write; the first
   write rolls back cleanly; the observability counter increments.
6. **Tag isolation:** two concurrent pipelines between the same core pair
   never cross batches (tag discipline), under injected reordering.
7. **Fast path:** with all relations on one core, the pipeline layer
   contributes zero messages and the execution trace is identical to the
   single-core build.
8. **RR weakening:** an RR transaction's cross-core read observes a commit
   made after the transaction began (documented behavior pinned by test).
9. **Range equivalence:** every shippable shape over a split relation
   returns byte-identical result sets to the same statement over the
   same rows unsplit on one core — test 1's discipline with the split as
   the only variable, over data where matching rows straddle the
   boundary.
10. **Shared statement view:** a transaction writing two relations *or*
   two ranges owned by one core commits between two of a statement's
   view mints on that core; the statement's answer contains all of that
   transaction's writes or none (§5's per-(statement, core) rule,
   pinned against the torn read in both shapes).
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
