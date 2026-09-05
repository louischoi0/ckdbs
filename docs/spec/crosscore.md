# Cross-Core Execution

> **SUS-1 (2026-09-05): heap relations are suspended, and range splitting
> is heap-gated** — `src/server/command_dispatcher.cpp:7224` keys routing on
> `heap_omitting_pk`, so **no relation created since the suspension can be
> range-split at all**. Everything below about multi-range relations,
> the directory, id-based write routing and the K-series therefore
> describes relations created *before* the suspension, and is unreachable
> for new ones until it lifts. Spreading also ships off independently
> (§6b), so this is the second of two gates, not the only one. The order
> is `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`; AS-Q5 is
> where the operator's word on this banner goes.

How a single statement that references relations owned by different cores
executes. This is the concept spec for the mechanism `docs/spec/protocol.md` D3
reserved ("server-side forwarding — clients are core-topology-unaware") and
`docs/spec/sched.md` §5 provides transport for. Consistent with `docs/rules/rules.md`
(thread-per-core, core-local by default with what is shared declared,
no exceptions, deterministic testability).

**The ownership unit is the primary-key range, not the relation.** CC8
defines the unit; a relation starts life as one range owned by its creating
core, so `sys.tables.owner_core` is the one-range case of every rule below,
not a retired concept. The unit, the directory and the routing are §2a; the
write scope is CC3 and §6; what may split is §6a; how inserts spread is §6b.

Scope boundary: this spec covers cross-core **reads** and the routing every
statement takes. Cross-core **commit** — a transaction touching relations
owned by more than one core — is specified in `docs/spec/cross-owner-txn.md`;
where the two disagree, that one wins on the protocol and this one on
routing. "Cross-core write" includes a statement or transaction writing two
ranges owned by different cores, *even ranges of one relation* (CC3); a
multi-*range* write statement or transaction is refused (§6).

## 1. Decisions

| # | Decision | Resolution |
|---|----------|-----------|
| CC1 | Execution model | **Step pipeline (function shipping)** — each step runs on the core owning its range(s); output flows to the next step's core; final rows to the session core |
| CC2 | Intermediate transfer | **KWP binary row batches (protocol D5 encoding) in chunked ring messages + credit-based flow control** |
| CC3 | Write scope | A transaction's writes bind to one **owner core**; ownership is per **range**. (Under per-core streams that owner was also a distinct WAL stream, which is how this read before AR0 M0; §4a carries the reading.) A transaction writing relations owned by two cores commits through `docs/spec/cross-owner-txn.md`'s protocol. A statement or transaction writing two *ranges* owned by different cores — including two ranges of one relation — is refused with a retryable error (§6). Two ranges the home core co-owns stay legal: single-owner, nothing 2PC-shaped in them (§6, and the reader-side consequence in §5) |
| CC4 | Remote-read isolation | A remote step reads the owning core's **latest committed snapshot**; no cross-core ReadView. RC-equivalent; RR weakening documented (§5). Per range: each stage reads its *range* owner's view |
| CC5 | Cancellation & errors | Cancel/error messages propagate both directions; every message tagged `(session_core, request_id, step_id)`; stale batches discarded by tag |
| CC6 | Scheduling | Remote step tasks run in the **foreground** group on their core (step chains are the OLTP path) |
| CC7 | Page ownership | **Page ownership is a function of the catalog**: a relation's pages belong to the core `sys.tables.owner_core` names, whatever lease allocated them. Realized at DDL publish by a **flush-then-grant handoff** — core 0 flushes the relation's pages, then grants the owner fault rights at extent granularity over the ring, and the owner faults fresh frames: the same discipline the catalog half uses for catalog pages. `CREATE TABLE` does not allocate from the owner's lease (rejected: a cross-core allocation protocol inside DDL that still needs a creation-time write exception). The store's debug `MayFault` check stays extent-granular, so a granted extent may carry pages of other core-0 relations — a **superset assertion**, acceptable because the enforced mechanism is statement dispatch to the owning core, never the assertion. The engine does not rebalance ownership after creation. Every page movement between owners is a PL-B logged handoff (`docs/spec/page-lsn-cross-stream.md` §9); the DDL-publish flush-then-grant is its easiest case (pages quiescent, no peer ever logged). The handoff survived AR0 M0 because **ownership is still per core** — what M0 removed is the second stream, not the second owner. **The durable form of the fact is the page's PL-C stamp**: leases and grants are memory-resident, so a core's ownership after a restart is re-derived from the stamp on each page it faults — the catalog says which core *should* own a relation, the stamp says which core *does* own each page, and the two agree by construction because nothing moves a page after creation. Under one stream the stamp is exactly this claim and nothing more: redo neither refuses a foreign value nor rewrites one, because there is no other log it could have come from (`wal.md` §3). **The owner-builds exception**: for `CREATE INDEX` on a peer-owned relation core 0 cannot produce the pages — its `Backfill` reads the device's last checkpoint and misses every row the owner holds uncommitted or never checkpointed — so the **owner builds** the tree from its own lease, own-stamped, and no page crosses an owner; core 0 keeps only the catalog half (the `sys.indexes` row and the commit; `docs/spec/ddl-transactional.md` §5e). The same holds for an assertion's Bound Cabin on a stronger premise: it is **appended to by every write to the relation**, so its pages must be writable by the owner forever after, which a core-0-allocated chain never is (`MayWrite` refuses it). The owner builds the cabin from its own lease, own-stamped, no handoff, and **holds its live directory**: the enforcing core for an assertion is the core that owns the relation, on every core and at every mount. Core 0 keeps the `sys.assertions` row, and a `DROP` on core 0 tells the owner to forget the directory |
| CC8 | Ownership unit | **The pk range `[lo, hi)` of one relation.** Not the relation — too coarse: it caps a hot relation at one core permanently. Not the page — too fine: btree descents and heap-chain walks cannot cross cores per hop. Both clustering modes are key-ordered structures and `min_key` is immutable (invariant 2), so a boundary is stable for a page's life. A btree descent under range ownership would cross **at most one boundary, at the top** — a shared structure whose writer is the root's owner core and never core 0, which CC11's rule does not reach; **a btree relation does not split** (§6a). The heap chain has no descent — every walk enters at the head and follows `next` — so **a split relation's ranges are per-range sub-structures**: a heap range is its own chain with its own head, and the entry page rides the directory row (CC9). A relation starts as one range owned by its creating core (§2a) |
| CC9 | Range directory | **Ownership stays a function of the catalog.** `sys.ranges` (rel oid, lo, owner core, entry page; hi is the next row's lo, and a **non-empty directory carries a row at lo = 0**, so the rows partition the whole id space) records every split; a relation with no rows there is one range owned by `sys.tables.owner_core`. Resolved at plan time from the session core's catalog cache; a remote stage trusts its descriptor and does not re-resolve; staleness is a retryable step error (§5). A split broadcast rides `kCatalogInvalidate` as DDL does |
| CC10 | Range split | **Split at a page boundary only** — `min_key` is the split key, so no page is ever divided and invariants 2/3 hold by construction. The only path that creates a range is the range allocator's block-aligned insert split (§6b), which satisfies this vacuously: the new range starts as its own empty sub-structure (CC8) and no existing page straddles it. **The engine has no mover**: no range is migrated, merged or re-placed after it opens. A range opens in one task on core 0's reactor, and the order inside it is a correctness statement: the directory row is core 0's catalog write, durable before the grant; **the relation's Observational Cabin entry sets are discarded between the durable row and the grant**, because a set banked while the relation was whole was banked under the whole-relation claim and nothing in a set records which claim it was made under (`docs/spec/cabin.md` §4b) — a discard after the boundary was published would be dropping sets that were already subsets. The discard is a direct core-local `CabinStore::Forget` on **core 0's** store alone; every core holds a store and a peer's store is not discarded, so a range opening on a relation whose owner is a peer rests on the serve site's scope rule (`cabin.md` §4b: a store falls through on a relation not wholly its core's). A Waystone trail replayed against the wrong core misses on the epoch/owner check and falls through; a Cabin hint miss resolves through the pk *on the same core*, and its entry sets are memory-resident where they were observed |
| CC11 | Shared-structure access | **Every core reads with the same authority; core 0 alone writes.** The rule covers the three fixed system structures — the **superblock** (page 0), the **free map** bitmap pages of every region, and the **catalog** pages. No partition-boundary lock is taken on any of these three (`rules.md` §3's last-resort clause is not invoked for them, and no header names an acquisition order over them — `wal/stream.hpp` names one over the WAL, which is a different structure), and there is no rotating coordinator. One writer *is* the serialization mechanism and costs nothing: total order over catalog change falls out of one writer, which is what makes `BumpVersion` plus an invalidation broadcast sufficient. **The writer is what carries this, not the log's shape** — the instance has one WAL stream now (`wal.md` §3) and the argument is unchanged, because a second core appending to the same log would still be a second core writing catalog pages. "No DDL can span two WAL streams" was a true consequence of one writer under the old topology, never the reason for it. Read scalability comes from the read half: a peer's fill path is the **device**, not a message to core 0 — catalog frames are the authority and the cache is the memo (`src/server/core_runtime.cpp`), so a peer's cache miss re-reads the pages off the device; the free map is re-read by `RefreshFreeMapFromDevice`, which `InvalidateCatalog` runs **first**, so a peer can reach a page core 0 has just allocated. **The store enforces the rule** (`src/storage/device_page_store.cpp`): `MayFault` admits the whole system range on a leased store — the catalog lives there, and a core that cannot read it cannot serve a statement; `MayWrite` refuses that range on a leased store in **every** build; `ResidentBytes` answers a peer asking for a system page `InvalidArgument` rather than a retryable status, because it is wrong on every retry; `FlushMaps` refuses the map write-back on a leased store, being the one write path that never asks `MayWrite`. Core 0 holds no lease, so nothing gates it. **What stays core 0's**: file growth, extent leasing, and free-map region creation — a leased store may not place a page at a chosen id (`CreateAtUnpinned`), because that is a claim on the free map. A peer runs DDL by **shipping the statement to core 0** (CC13), never by writing catalog pages. Not covered: a *relation's* shared structure — the btree's top levels under a split relation (CC8) |
| CC12 | Catalog page placement | **CR1 — a catalog relation's root page stays in the reserved range; its var-heap does not.** `kCatalogPageTypes = 4` through `kCatalogPageRanges = 15` (`include/kds/catalog/well_known.hpp`) sit below `kFirstUserPageId = 128` because they must be findable at bootstrap *without* a catalog read. A catalog relation's **var-heap** root is allocated through `CreateNew()` and recorded in `sys.tables`, binding on any catalog relation with a var-heap. The cost: a page outside the range is not peer-readable through `MayFault`'s `page_id < system_page_limit_` arm, so the one peer-readable catalog var-heap — `sys.assertions`, the sole entry of `exec::kVarHeapCatalogRelations` — is reached by granting the individual pages a row names (`exec::CatalogSpillPages`, `src/server/core_runtime.cpp`), deliberately **not** an extent grant, which would cover pages that core owns and cost it its stamp-claimed write rights. That page-at-a-time grant is not the general mechanism; CR3 is. **CR2 — DDL executes on core 0; a peer sends and waits.** A peer does not execute DDL: it ships the request to core 0 and waits for the outcome (CC13); `PeerDdlRefused` (`src/server/command_dispatcher.cpp`) stays for the cases the ship does not cover. **CR3 — a catalog page may leave the reserved range once grown.** Catalog pages are allocated and initialised inside the range at bootstrap; beyond bootstrap a catalog page **may be managed outside it** in either of two cases — the relation grows past what the reserved range holds, or every peer must read *and write* the page equally. Such a page takes the ordinary relation rules: allocation from the general supply, free-map accounting, extent leases, and the WAL logging catalog change already has. No catalog page has left the range; a var-heap root outside it is found through `sys.tables` — the indirection `varheap_page_id` uses, DDL-immutable and therefore cacheable per `rows.hpp` |
| CC13 | DDL's route, and how a peer's statistics reach core 0 | **CR5 — a peer routes DDL to core 0 rather than refusing it.** Detection is at dispatch (`src/server/command_dispatcher.cpp`), keyed on `catalog_read_only_` and the `CREATE`/`ALTER`/`DROP` tokens — the same token the routing below it reads, so the two cannot disagree about what a verb is; a match ships to core 0 and the peer waits for the outcome. `PeerDdlRefused` stays reachable for the cases the ship does not cover, and is the only guard under NDEBUG for those. **CR6 — a catalog relation whose content is only a statistic is written unlogged**, the sole exception to the rule that catalog writes are WAL-logged and replayed (`docs/spec/ddl-transactional.md` §7; the rule text is `docs/rules/rules.md` §5). **CR7 — a peer batches statistics locally and flushes periodically**, never one message per statement, and **`sys.access_stats` stays one relation at page 11 (`kCatalogPageAccessStats`), core-0-written**: there are no per-core statistics relations. The batch carries `core_id` on the wire and `SHOW META` can account by it, but `SysAccessStatRow` is a 33-byte fixed row with no field for it (`catalog/rows.hpp`), so a peer's counts **fold into the existing `(kind, rel_id, column_mask)` shape** at rest. **CR8 — a full ring drops the batch**: invariant 8 prices a lost statistic as performance and never a result, so a drop is a permitted outcome rather than an error to report; it increments a counter visible in `SHOW META`. Batch size and flush interval are the code's constants, not ratified values; `RecordAccess` costs +1-2% on a point lookup (`docs/spec/heap-and-tuple.md` §7) |

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
its **local** trail/statistics recording. **Waystone trails do not cross
cores; access statistics do** (CC13/CR7): a peer folds its access shapes
locally and flushes them to core 0, which owns the one `sys.access_stats`.
The trail half holds for its own reason: `RegisterPattern` hands the
recorder a pointer it uses immediately, so it needs an answer, and a stage
cannot wait for one.

A pipeline is torn down when the session core has framed the final row,
received `STEP_ERROR`, or issued `STEP_CANCEL` (§7).

## 2a. Ranges — the Unit, the Directory, the Routing

**The unit** is CC8's; **the directory** is CC9's `sys.ranges`, resolved
as CC9 states (plan time, the session core's cache, staleness a retryable
step error). Two rules beyond CC9's cell:

- **The directory is read-mostly.** A split is a rare, DDL-frequency
  event; the per-statement path reads the per-core cached copy. A split
  broadcast rides `kCatalogInvalidate` as DDL does. **A resolved range is
  never cached across a suspension**: `InvalidateFromPeer()` clears a
  peer's cache without bumping `catalog_version()` — deliberately, that
  counter is per-instance — so a range fact guarded by that counter would
  be wrong on every peer.
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
| `SHIPPED_ROW_DESC` (`kShippedRowDesc`) | owner → arrival core | one chunk of a shipped read's row description, ahead of the first batch on the answer edge, with its own sequence (§4a) |

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
  which is what lets a shipped read's rows reach a KWP client without
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

### 4a. The shipped read's answer edge

A shipped *read* (§6) on a session that carries a result sink — every
KWP/1 session — is answered in **rows on an answer edge over this same
step wire**, and nothing about that wire changes. The arrival core mints
the `PipelineTag` and registers the receiver *before* it ships; the
request carries the tag; the owner installs a batch sink on the shipped
session and sends `STEP_BATCH` under the existing credit protocol; the
ship reply POD arrives last as the **terminator**, carrying the status and
the watermark with `text_len = 0`. Codec, batch builder, credit grant,
`STEP_CANCEL` and the ceiling are reused unchanged — this is a fifth
**producer** on the pipeline, not a second pipeline.

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
`Emit` takes rendered text — and a shipped read to a text session never
asks, because that arm ships `form = 0`. Row boundaries come from
`wire::DecodeRowExtents`, the same walk `DecodeRowBatch` performs, in the
file that owns the format, so nothing reads the row format twice.

*Why an edge and not a bigger reply.* Both ship PODs fill exactly one ring
slot, so the reply carries **992 bytes** and cannot hold a result set at
all. The rows cross on the thing that already batches.

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

**The request POD pays 16 bytes for the tag**, so the longest shippable
statement is **976 bytes**. That bound is client-visible and is stated in
`docs/spec/client-manual.md`.

**The text arm does not move a byte.** A session with no result sink ships
`form = 0`, the owner installs no sink, and the rendered-text reply is
byte-identical to the newline protocol's — including its own 992-byte
whole-reply cap, which is a debug surface's limit and stays one.

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
the request says so, so a text read keeps its record, bounded at 992 bytes
either way.

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
whole one — and a client disconnect closes the tag on the path that
closes a pending shipped statement.

## 5. Isolation Semantics

There is no cross-core ReadView. A remote step reads what is committed on
its own core (`docs/spec/txn.md` visibility with an empty live-set view; the
trx-id domain is global, so ids compare cleanly).

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

The RR weakening reads the same one level down: RR guarantees hold per
core, and a cross-range read of one relation is a cross-core read. The
client manual states the widened form beside the one-range one.

- **READ COMMITTED** statements: semantically equivalent to local execution —
  RC already permits each statement (and each lookup within it) to observe
  the latest committed state.
- **REPEATABLE READ** transactions issuing cross-core reads: a cross-owner
  RR transaction sees a **consistent-per-core** snapshot — each participant
  pins one view for the transaction's life and the coordinator carries that
  participant's watermark, which is compared with nothing on any other
  core. `docs/spec/cross-owner-txn.md` §3 owns the rule and
  `docs/spec/client-manual.md` states it in the client's words. What is
  **not** given is a single global instant: two such transactions can
  disagree about the order of two commits on two cores, and sharing the log
  did not change that (`cross-owner-txn.md` §3).

  **The remote-step pipeline does not run inside such a transaction.** It
  reads each core's latest-committed snapshot outside any enrolled
  transaction, which inside a cross-owner transaction is not a weakening
  but a **wrong answer** — that transaction's own writes on the owner live
  in the transaction the owner holds for it, and no view this leg can take
  shows them. So both remote-read fast paths are skipped for a session that
  can enrol, and the read ships instead (§6), under §4a's bounds: the
  widest row on a typed session, the 992-byte reply on a text one.
- Catalog: the plan is resolved entirely on the session core from its
  catalog cache; a remote step trusts the descriptor in `STEP_OPEN` and does
  not re-resolve. DDL invalidation between resolve and execute surfaces as a
  normal step error (stale oid → `STEP_ERROR`, retryable).

## 6. Writes

The shipping unit and the refusal are per range; on a one-range relation
every rule below reads with "relation" for "range".

- A single DML statement is shipped **whole** to the core owning its
  target range and executes there under that core's transaction
  machinery — statement shipping, implied by protocol D3, involving no
  pipeline. An **autocommit, single-relation** statement — read or write —
  whose relation another core owns is carried to that core as *text*,
  parsed and bound there against the owner's own catalog, executed under
  the owner's ordinary local implicit transaction, and committed through
  the owner's group committer, which is the whole performance argument.
  The arrival core parks a waiter under a deadline and answers with the
  owner's own reply, the `retryable` bit included. A statement **inside an
  explicit transaction** ships and *enrols*, for writes and for reads —
  `docs/spec/cross-owner-txn.md`.

  **What is refused**, each a scope statement: a statement **spanning
  two owners** (a multi-owner *statement*, which is not a multi-owner
  *transaction* and is not what the commit protocol addresses), and a
  statement on a path that **cannot park** — the synchronous dispatch
  entry, because sending from a path that cannot wait would leave a
  statement the owner may have committed with nowhere to deliver its
  answer. `cross_core_write_refusals` counts these. Shipping is
  **unconditional** where it applies: the engine does not ship or refuse
  by load.

  A lost or late answer is **not** a refusal. It is `UNKNOWN_OUTCOME`,
  non-retryable by construction, because this engine issues primary keys
  and a blind retry of a statement that may have committed inserts a
  second row. The owner keeps a bounded per-(arrival core, session)
  record of what it last answered — and of what it is still running — so
  a duplicate is answered from the record rather than executed twice
  (§4a for the typed read's exemption).

  **Target resolution is pk arithmetic against the directory alone**
  (§2a): a DML on a split relation whose predicate does not bound its rows
  to one owned range is a cross-core write, refused retryably.
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
  cannot see, stated at the print site as well: **DDL on a peer**
  (`PeerDdlRefused`, refused by verb before any relation is resolved) and
  anything refused before resolution at all. The two owner-core refusals —
  `RelationWriteRightsPending` and `IndexBuildPending` — are excluded **by
  decision**: the write is not cross-core, it is this core's own write
  waiting on a grant or a build window, and counting it would inflate the
  cross-core evidence with cases it does not address.
- Write-coupled auxiliary placement is §6a's. On a one-range relation
  unique indexes, Cabin, Waystone pages, and the var-heap live on the
  relation's owner core, always. Read-only join partners may live anywhere.
- FK (`docs/spec/foreign-keys.md`): parent and child on two *relations* on
  two cores is owner-granular and supported — the forward check hoists to
  the dispatch fork, probes one round per distinct owner, and leaves a
  row-scoped reference intent; the validation-to-commit window closes
  because a parent `DELETE` meeting a live intent answers busy rather than
  racing it, and nothing inside a `WriteScope` waits (`foreign-keys.md`
  §2a/§2b). A split parent or child would make the validation
  range-granular, which the engine does not do: §6a's FK gate.

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
`SHOW META` refusal-counter form, owner-core-local because only the
owner's registry can answer the assertion gate.

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
- **Assertions** — an assertion's Bound Cabin is owner-built from the
  owner's own lease and **appended to by every write to the relation**
  (CC7), its live directory is memory-resident on that one core, and its
  aggregate is keyed on group columns that need not include the pk
  (`ResolveAssertionColumns` refuses none) — so a group's rows straddle
  any range boundary, a second owner core's appends hit `MayWrite`'s
  refusal, and a core whose registry never heard of the assertion admits
  the write **unchecked**. An asserted relation — one the owner's registry
  holds live (`AnyOn`) *or* knows and cannot enforce (`CannotEnforce`) —
  does not split. The fact deliberately does not ride `TableAccess`:
  `CREATE`/`DROP ASSERTION` do no version bump (`assertion_catalog.cpp`'s
  publish comment — nothing cached is derived from a `sys.assertions`
  row), so a cached bit would be stale by construction, and the gate
  function takes the enforcer, not the access struct alone.
- **Waystone and statistics** — advisory (invariant 8): recording stays
  per owning core, a stale trail misses and falls through, and **no
  gate is needed** — worst case the trail is deleted, which invariant 8
  prices as performance, never a result. A peer's access statistics reach
  core 0 through CC13/CR7 as per-core *rows* in the one
  `sys.access_stats`; there are no per-core statistics relations.

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
