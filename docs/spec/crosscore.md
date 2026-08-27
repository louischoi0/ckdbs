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

Scope boundary: this spec covers cross-core **reads**. Cross-core **commit**
(a transaction writing relations owned by more than one core) remains
`[OPEN]` per `docs/spec/wal.md` §3 — reserved for a later 2PC design, not designed
here. §6 defines the v1 restriction that keeps commit single-stream; since
v2 "cross-core write" includes a statement or transaction writing two
ranges owned by different cores, *even ranges of one relation* (CC3).

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
| CC8 | Ownership unit (v2, operator-directed 2026-08-24) | **The pk range `[lo, hi)` of one relation.** Not the relation — too coarse: it caps a hot relation at one core permanently, and one dominant relation is the stated workload's ordinary case. Not the page — too fine: M1's rejection of page/extent hashing stands, and its argument is what sizes the unit — btree descents and heap-chain walks cannot cross cores per hop. Both clustering modes are key-ordered structures and `min_key` is immutable (invariant 2), so a boundary is stable for a page's life and a descent under range ownership crosses **at most one boundary, at the top**. Two qualifications the code forces (added at the 2026-08-24 review): the one-boundary claim is the **btree's**, and its top levels belong to whoever owns the root — that hop is the shared-structure access mechanism, still `[OPEN]` (blueprint §8; indexed in §9). The heap chain has no descent at all — every walk enters at the head and follows `next` — so **a split relation's ranges are per-range sub-structures**: a heap range is its own chain with its own head, a btree range its own subtree entry, and the entry page rides the directory row (CC9). Per-range chains are R3's largest piece, named here so they are built, not assumed. A relation starts as one range owned by its creating core (§2a) |
| CC9 | Range directory | **Ownership stays a function of the catalog** — workplan guideline 4 kept, not amended. A catalog relation (working name `sys.ranges`: rel oid, lo, owner core, entry page; hi is the next row's lo, and a **non-empty directory carries a row at lo = 0**, so the rows partition the whole id space) records every split; a relation with no rows there is one range owned by `sys.tables.owner_core`. Resolved at plan time from the session core's catalog cache; a remote stage trusts its descriptor and does not re-resolve; staleness is a retryable step error (§5's rule, unchanged). Split/migration broadcasts ride `kCatalogInvalidate` as DDL does (§2a names the cache-generation prerequisite) |
| CC10 | Range split & migration | **Split at a page boundary only** — `min_key` is the split key, so no page is ever divided and invariants 2/3 hold by construction; §6b's block-aligned insert split satisfies this vacuously, because the new range starts as its own empty sub-structure (CC8) and no existing page straddles it. **Migration**: the page-level contract is the ratified PL-B handoff (`page-lsn-cross-stream.md` §9); the range-level sequence *around* it is this spec's, and its ordering is a correctness statement — (0) the outgoing owner quiesces the range (in-flight stages finish or cancel; new plans against it surface as §5's stale retryable step error), (1) flush, (2) durable handoff record, (3) **durable directory row before any grant** — the row is a catalog write in core 0's stream, synced before step 4 — (4) grant, (5) invalidation broadcast; the incoming core's first write stamps its stream per PL-C. **A crash before step 4 aborts the migration to the outgoing owner at mount**, sound precisely because the grant is last: the incoming core has written nothing. PL §9 governs the record/redo/stamp halves; steps 0, 3 and 5 are outside its five rules and are owned here, not cited to it. Advisory-reference retirement is priced, not free: a Waystone trail replayed against a moved range misses on the epoch/owner check and falls through, but bumping `relayout_epoch` costs one write per moved page — whether the bump or the owner check alone retires stale trails is R5's to settle against the mover's flush-per-move cost. **Cabin does not self-heal**: its hint miss resolves through the pk *on the same core*, and its entry sets are memory-resident where they were observed — so a cabined relation is gated for migration exactly as for split (§6a). The mover is the physical optimizer's Part III and inherits Part I's discipline (observe, decide, report through SHOW, enact through named gates); its policy and constants stay `[OPEN]` (§9). Until the owning docs decide auxiliary placement, split is **gated** (§6a) |

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
its **local** trail/statistics recording — no statistics cross cores.

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
- **REPEATABLE READ** transactions issuing cross-core reads: the remote
  relation is read at latest-committed, not at the transaction's ReadView.
  This is a **documented weakening**: RR guarantees hold per core, not
  across cores. The client manual must state it; the server does not error.
  Escalating to an error (or to snapshot forwarding) is `[OPEN]` alongside
  the 2PC milestone.
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

  Three things stay refused, and each is a scope statement rather than a
  gap: a statement **inside an explicit transaction** (nothing crosses
  transaction state), a statement **spanning two owners** (R6), and a
  statement on a path that **cannot park** — the synchronous dispatch
  entry, because sending from a path that cannot wait would leave a
  statement the owner may have committed with nowhere to deliver its
  answer. Shipping is **unconditional** where it applies: whether to ship
  or to refuse by load is placement policy, which is §9's open decision
  and does not ride along.

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
**the conservative gate is that the relation does not split**. A
`SPLIT RANGE` (working name, blueprint R3) targeting a gated relation is
refused with the gate's name — `Unsupported`, understood and declined,
carrying the offending token's byte position per the standing refusal
rule — which keeps every listed option viable. The gates below bind
**split**; whole-relation **migration** moves everything together and
preserves co-location, so only the Cabin gate (its state is
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
- **Waystone and statistics** — advisory (invariant 8): recording stays
  per owning core, a stale trail misses and falls through, and **no
  gate is needed** — worst case the trail is deleted, which invariant 8
  prices as performance, never a result. Per-core statistics relations
  are R1's item and a prerequisite of the mover (R5), not of
  correctness.

What remains splittable in the first build — non-spilling
(`SchemaCanSpill` false; invariant 13 makes *every* relation
fixed-length, so the spill is the gate), unindexed, un-cabined, FK-free
relations — is narrow and real. The gates are lifted by the owning
decisions, never by relaxing the refusal.

### 6b. Inserts and the Tail — Id-Block-Aligned Spreading (v2, R4)

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
  invariant 11's 2026-08-11 amendment (`docs/spec/heap-and-tuple.md` §4.1:
  "monotonicity is now per-relation, never engine-wide") one level
  down, and it needs the same loud documentation when built (R4).
- A **btree** relation whose caller names its keys spreads naturally —
  those ids need not ascend — and needs none of this. A **heap**
  relation does not get that for free even when the caller names its
  keys: since 2026-08-25 they must still be at or above the mark
  (`docs/spec/heap-and-tuple.md` §4.1), which is the same tail this section
  is about. The spreading problem is the chain's, not the issuer's.
- Whether interleaved blocks are the default or opt-in is `[OPEN]`
  (§9): a single-writer relation gains nothing from them.

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
13. **Split gates:** `SPLIT RANGE` on an indexed, cabined, spilling, or
   FK-linked relation is refused with the gate's name and the offending
   token's byte position (§6a); on an eligible relation the directory
   rows appear (the lo = 0 row included, CC9) and an unaffected
   relation's fast path is byte-identical to before.

## 9. Open Items

- Cross-core commit protocol (2PC) and with it: multi-range write
  statements and transactions, cross-core FK, RR snapshot forwarding,
  write shipping inside explicit transactions (§6 counters feed this
  design, with the undercount §6 names; PL-A's revisit clause arms with
  2PC, `page-lsn-cross-stream.md` §9).
- Split/migrate policy and its constants — triggers, thresholds,
  cadence, and merge (the mover is the physical optimizer's Part III;
  blueprint R5 owns the phase, the Part III spec owns the policy when
  drafted).
- Auxiliary placement under a split relation — each lifts its §6a gate:
  per-range local vs global secondary indexes (`docs/spec/index.md`
  §13), Cabin (`docs/spec/cabin.md` §11), var-heap partition
  (`docs/spec/heap-and-tuple.md`), FK (`docs/spec/foreign-keys.md`).
- Id-block interleave default (§6b): default or opt-in.
- The shared-structure access mechanism (blueprint §8) — CC8's
  one-boundary btree hop lands on it, so it gates R3's btree ranges,
  not only "every core equivalent".
- Batch size and initial credit tuning (`[PROPOSED]` values above).
- ~~Pattern/Waystone-driven relation placement~~ — the *dynamic* half
  subsumed 2026-08-24 by the mover (CC10): re-placement is range
  placement, decided by statistics. **Initial placement stays open**:
  `PlacementPolicy` (`creating` | `rotate`) is built, configured and
  unsettled, and where a new relation's one range starts is not the
  mover's question. Either way placement stays an optimization
  concern — cross-core execution is the correctness path regardless.
