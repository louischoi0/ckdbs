# Cross-Core Execution

How a single statement that references relations owned by different cores
executes. This is the concept spec for the mechanism `docs/protocol.md` D3
reserved ("server-side forwarding — clients are core-topology-unaware") and
`docs/sched.md` §5 provides transport for. Consistent with `docs/rules.md`
(thread-per-core, shared-nothing, no exceptions, deterministic testability).

Scope boundary: this spec covers cross-core **reads**. Cross-core **commit**
(a transaction writing relations owned by more than one core) remains
`[OPEN]` per `docs/wal.md` §3 — reserved for a later 2PC design, not designed
here. §6 defines the v1 restriction that keeps commit single-stream.

`[PROPOSED]` marks a default to confirm or amend before the affected part is
built. `[OPEN]` marks a deferred decision that must not be assumed.

## 1. Decisions

| # | Decision | Resolution |
|---|----------|-----------|
| CC1 | Execution model | **Step pipeline (function shipping)** — each step runs on the core owning its relation; output flows to the next step's core; final rows to the session core |
| CC2 | Intermediate transfer | **KWP binary row batches (protocol D5 encoding) in chunked ring messages + credit-based flow control** |
| CC3 | Write scope | **v1 is read-only cross-core.** A transaction's writes bind to one home core; a write targeting another core's relation is a retryable error. 2PC write support reserved `[OPEN]` |
| CC4 | Remote-read isolation | A remote step reads the owning core's **latest committed snapshot**; no cross-core ReadView. RC-equivalent; RR weakening documented (§5) |
| CC5 | Cancellation & errors | Cancel/error messages propagate both directions; every message tagged `(session_core, request_id, step_id)`; stale batches discarded by tag |
| CC6 | Scheduling | Remote step tasks run in the **foreground** group on their core (step chains are the OLTP path) |
| CC7 | Page-ownership reconciliation (the P6 blocker; operator-decided 2026-08-10) | **Page ownership is a function of the catalog**: a relation's pages belong to the core `sys.tables.owner_core` names, whatever lease allocated them. Realized at DDL publish by a **flush-then-grant handoff** — core 0 flushes the relation's pages, then grants the owner fault rights at extent granularity over the ring, and the owner faults fresh frames: the same discipline P6's catalog half already uses for catalog pages. The alternative (CREATE TABLE allocating from the owner's lease) was rejected as a new cross-core allocation protocol inside DDL that still needs a creation-time write exception. Two consequences stated now: the store's debug `MayFault` check stays extent-granular, so a granted extent may carry pages of other core-0 relations — a **superset assertion**, acceptable because the enforced mechanism is statement dispatch to the owning core, never the assertion; and a catalog-derived ownership fact is one of the two candidate fixes `feat-physical-optimizer.md` §6 gate 3 names, so this decision serves both. Ownership **rebalancing** after creation stays out of v1 with M3. |

## 2. Execution Model

The session core owns the statement end to end: it parses, resolves the step
chain (the written-order contract of `docs/parser-v2.md` — *the statement is
the chain*, never silently reordered), and looks up each
step's owner core from its catalog cache (`owner_core`, multicore-workplan
M1). Two paths:

- **Local fast path.** Every referenced relation is owned by the session
  core. Execution is exactly today's single-core code — the cross-core layer
  must add zero work here. This is an invariant, not an optimization note:
  the single-core path must not regress in instructions or allocations.
- **Pipeline path.** At least one step's relation lives on another core.
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

## 3. Messages

All pipeline traffic rides the per-core-pair SPSC rings (`docs/sched.md` §5).
Message kinds:

| Kind | Direction | Payload |
|------|-----------|---------|
| `STEP_OPEN` | downstream stage → its upstream's core (the session opens the final stage; amended 2026-08-14, §2) | step descriptor: relation oid, bindings, projection set, downstream core+step; optional upstream section (forwarded-row layout, enclosed upstream open) |
| `STEP_BATCH` | step k → step k+1 (or session) | chunk of KWP-encoded rows (§4) |
| `STEP_EOF` | upstream → downstream | no more batches for this step |
| `STEP_CREDIT` | downstream → upstream | grants N batch credits (§4) |
| `STEP_CANCEL` | any → any in pipeline | stop producing/consuming; discard tagged state |
| `STEP_ERROR` | failing core → downstream chain + session | Status code + retryable flag (protocol D9 mapping) |

Every message carries the tag `(session_core, request_id, step_id)`.
`request_id` is allocated per statement by the session core, sequential per
core (never pointer-derived — `docs/sched.md` §7 determinism rules). A core
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

## 5. Isolation Semantics

There is no cross-core ReadView. A remote step reads whatever is committed
on its core at the moment it produces each batch (`docs/txn.md` visibility
with an empty live-set view, trx-id domain is global so ids compare cleanly).

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

## 6. Writes (v1 Restriction)

- A single DML statement is shipped **whole** to the core owning its target
  relation and executes there under that core's transaction machinery —
  this is statement shipping, already implied by protocol D3, and involves
  no pipeline.
- An explicit transaction acquires a **home core** at its first write (the
  owner of the written relation). Any later write targeting a relation owned
  by a different core fails with a retryable conflict error (protocol D9;
  same client contract as first-updater-wins aborts in `docs/txn.md`).
  Reads inside the transaction remain free to pipeline cross-core under §5.
- Every rejected cross-core write increments a per-core observability
  counter keyed by (home core, target core, relation) — the input the
  future placement/2PC decision will be made from. Counters are metrics,
  not stored state.
- Write-coupled auxiliaries must therefore be co-located with their base
  relation: unique indexes, Cabin, Waystone pages, and the var-heap of a
  relation live on the relation's owner core, always (assignment rule in
  multicore-workplan M1). Read-only join partners may live anywhere.
- FK (`docs/impl-foreign-keys.md`) stays co-located in v1: RESTRICT validation is a
  read, but its validation-to-commit window is only sound against the local
  latest-committed state; cross-core FK inherits the §5 weakening and is
  deferred with 2PC `[OPEN]`.

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
  same hook that rolls back an open transaction, `docs/txn.md` tests).

## 8. Determinism and Testing

All of this must run under the simulated ring seam (`docs/sched.md` §6):
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

## 9. Open Items

- Cross-core commit protocol (2PC) and with it: cross-core FK, RR snapshot
  forwarding, write shipping inside explicit transactions (§6 counters feed
  this design).
- Batch size and initial credit tuning (`[PROPOSED]` values above).
- Pattern/Waystone-driven relation placement to reduce cross-core traffic —
  placement is an optimization concern, out of scope here by decision:
  cross-core execution is the correctness path regardless of placement.
