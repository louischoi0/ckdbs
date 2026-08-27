# Statement shipping, Part A — correctness before throughput

`instructions/v2.2.0/checklists-post-ss5.md` Part A, run on worktree
`partA-shipping-correctness` over `main` at **`v2.2.0-11-g925f483`** — the
merge that landed SS1–SS5. The engine is that commit unmodified: everything
this file reports is a test, a probe or a source-read added beside it, and no
engine line was changed to make a cell pass.

**Part A is not clean.** Five of seven items pass outright; two carry a
finding that a test in the tree now fails on, and both are recorded below
with the mechanism rather than the symptom. Neither is reachable from a
client today, and one of them is not shipping's to fix.

Discipline: `build-release` for every number, Debug for the suite; each claim
is **measured** with its invocation or **source-read** with `path:line`.

---

## What Part A found

| Item | Verdict | Evidence |
|---|---|---|
| A1 duplicate delivery | **pass** | 3 tests; row count is the verdict |
| A1 dedup record evicted | **finding 1** | the duplicate re-executes; test in the tree, disabled |
| A2 disconnect / timeout | **pass** | 3 tests; contract written into `docs/spec/client-manual.md` |
| A3 ring saturation | **pass** | 40 statements over a 16-slot ring, none dropped; storm map delta 0 |
| A4 per-session ordering | **pass** | 2 tests, one with a retried first statement |
| A5 shape gates | **finding 2** | FK and Cabin hold; the **assertion** gate does not fire on a peer |
| A6 G1 at DML volume | **pass** | 2,510 shipped statements at `cores = 4`, clean |
| A7 crash recovery | **pass** | two kill points; SS3's stream claim measured, not asserted |
| A7 `cores = 1` floor | **pass** | 1.030x/1.032x, opposite signs, inside the noise |

---

## 1. A1 — the dedup record under adversarial delivery

Three cases pass, each asserting **the row count** rather than the returned
status, because a right status over two rows is the failure this record
exists to prevent.

- **The same identity arriving twice** runs once and answers from the record
  (`TheSameShippedIdentityArrivingTwiceRunsOnceAndAnswersFromTheRecord`,
  over the real ring). `deduped = 1`, one row.
- **A reply lost after the owner committed**: the owner is pumped alone until
  it has executed and committed, the waiter is closed under the answer, and
  the retry then finds the record
  (`AReplyLostAfterTheOwnerCommittedLeavesOneRowAndTheRetryFindsTheRecord`).
  The lost answer is counted as `late_executed_replies = 1`; one row.
- **Session ids are not reused within a mount**
  (`AReconnectingClientTakesAFreshShipIdSoNoStaleSequenceMatchesIt`).
  `next_ship_session_id_` is per core and monotonic from 1
  (`include/kds/server/command_dispatcher.hpp:1387`, source-read), a
  `Session` mints from it once, and an owner's record dies with the process —
  so no stale sequence can match a reconnecting client's.

**No live path produces a duplicate at all.** `SendRetryTask` retries only a
send `TrySend` refused, which by definition never arrived
(`include/kds/sched/send_retry.hpp`, source-read). The record is the guard
for the retry paths a routing layer will bring; these tests drive it
directly, which is why they can exist before that layer does.

### Finding 1 — an evicted record re-executes its duplicate (blocking, latent)

`kShippedDedupMaxRecords` is 4,096. Fill it with 4,097 distinct sessions and
the oldest record is dropped **before its retention expires**; a duplicate of
that statement then finds no record and is **executed again** — against an
engine-issued pk, a second row. Measured: `executed` 4,097 → 4,098 with the
same (session, sequence) delivered twice
(`DISABLED_ADuplicateWhoseRecordWasEvictedEarlyIsNotReExecuted`).

This is the behaviour `shipped_statement_executor.hpp` already states — "an
early eviction is the one condition under which a duplicate could reach an
empty record and be re-executed" — and `early_evictions()` counts it. What
A1 asks for is a change to that behaviour: `UnknownOutcome`, non-retryable.

Every way of getting there is a policy decision, which is why none was taken
here:

1. **Refuse rather than evict.** Never drop a record inside its retention;
   refuse the 4,097th shipping session's statement retryably instead. Safe by
   construction — the refusal precedes execution, so a retry can win — and it
   makes the cap what its own header claims (memory, never correctness). Cost:
   an availability cliff at 4,096 concurrent shipping sessions per owner.
2. **Carry a retry bit on the request.** Only the client knows it is
   retrying; a retry that finds no record answers `UnknownOutcome`, a first
   attempt runs. No availability cost; five reserved bytes are already there
   (`ShippedStatementRequestPayload::reserved0`). Cost: the future retry path
   must set it, so the guarantee is only as good as that discipline.
3. **A tombstone under a second, larger bound.** Cheaper per entry, and it
   moves the window rather than closing it.

The acceptance test is in the tree, disabled and failing, so whichever lands
has it already written.

## 2. A2 — the client stops listening

- **The deadline** answers `ERR UNKNOWN_OUTCOME retryable=0` and reclaims the
  waiter at once; the owner then **applies the statement anyway**, its answer
  is counted `late_executed_replies = 1`, and the row exists exactly once
  (`AShippedStatementsDeadlineIsUnknownOutcomeAndTheOwnerStillAppliesIt`).
- **A dropped connection** does not destroy the statement:
  `TcpServer::CloseClient` defers the whole teardown while `conn.in_flight`
  and lets `OnStatementComplete` do it (`src/server/tcp_server.cpp:512-533`,
  source-read), so the parked coroutine completes and the waiter is closed on
  the ordinary path
  (`ADroppedConnectionsShippedStatementFinishesAndReclaimsItsWaiter`).
- **The invariant under that**, pinned from the other side: nothing reclaims
  a waiter whose statement is *destroyed* rather than completed — `Close` is
  reached only from `FinishShippedStatement`
  (`AParkedShippedStatementDestroyedUnderItsWaiterLeaksTheWaiter`). Harmless
  today and the thing that breaks the day a cancellation path is added, so it
  is asserted rather than assumed.

Two consequences are now stated in `docs/spec/client-manual.md` rather than left
to be discovered: `UNKNOWN_OUTCOME` means read the data back, never retry;
and a connection that drops mid-statement may find the statement applied.

**A2's literal sub-clause "must not leave the owner executing a statement
whose waiter is gone" cannot be satisfied and should be retired.** There is
no cancellation in this engine and a request already in the ring cannot be
recalled, so the owner *does* finish it. The design's answer is the
documented contract above, and the test asserts the effect lands exactly
once rather than pretending it does not land.

## 3. A3 — the ring at capacity

**Nothing is dropped and nothing is invented.** 40 statements shipped over
the rig's 16-slot ring before the owner runs once: all 40 answer, all 40
execute, `deduped = 0`, `unanswerable = 0`, `identity_mismatches = 0`, no
late replies (`EveryStatementSurvivesARingFilledPastItsCapacity`). A full
ring is never reported upward — `TrySend`'s `ResourceExhausted` is
backpressure, and `SendRetryTask` re-queues.

**The spin signature, measured**: over 32 core-0 iterations with 40
statements outstanding, the `system` group took **784 polls for 345 µs** —
about 24 re-polls per iteration, which is the ~24-message backlog re-polled
once each, and 0.44 µs of real `TrySend` work per poll. So it is not the
pathological shape A3 names (polls climbing while `polled_us` stays flat):
`polled_us` tracks polls, because each poll attempts a send. It is
nonetheless unbounded re-polling with no backoff and no ceiling, which is
`docs/spec/sched.md` §10's open `ring_full` retry protocol, named there and
unchanged here.

**Nothing allocates on the refusal path.** The G2 storm in its DML form: 180
shipped refusals across the three shapes a client can produce — a rank that
forbids the write, a relation that does not exist, a statement the parser
rejects — leave the owner's `map_pages_resident` and `map_regions`
**unchanged**
(`AStormOfRefusedShippedDmlCostsTheOwnerNoPageAndNoMapGrowth`), beside the
existing seam-level page-count storm.

## 4. A4 — per-session ordering

A session's write is visible to its own next read across the ship boundary
(`AShippedSessionReadsItsOwnWriteBack`), **including when the write was
retried** and answered from the dedup record rather than executed
(`AShippedSessionReadsItsOwnWriteBackWhenThatWriteWasRetried`) — one row,
and the read sees it.

## 5. A5 — the shape gates

The fork is a `return` placed after the relation is resolved and before
`CheckWriteAffinity` (`src/server/command_dispatcher.cpp:3588`), so
everything above it has already run and everything below it is what the
owner runs instead, through its own dispatcher. What a shipped write newly
reaches is therefore the **peer-side** shape gates
(`workplan-peer-writer.md` §4), which core 0 never had to satisfy.

- **FK-linked**: refused, and the line is byte-identical to the one the owner
  writes itself
  (`AShippedWriteToAnFkLinkedPeerRelationIsRefusedByTheOwnersShapeGate`).
- **Cabined**: refused, same shape
  (`AShippedWriteToACabinedPeerRelationIsRefusedByTheOwnersShapeGate`).
- **Spanning two owners**: not shipped at all — `SoleForeignOwner` refuses a
  chain whose steps are not all one foreign core's, and the statement keeps
  its affinity refusal with nothing sent
  (`AStatementSpanningTwoOwnersIsNotShippedAndKeepsItsRefusal`).
- **Inside a transaction**: already covered, unchanged
  (`AStatementInsideATransactionIsNotShippedAndKeepsItsRefusal`).

**A wire-visible change, asserted rather than left to be discovered**: for
these relations the answer used to be core 0's `TXN_CONFLICT retryable=1`
("not mine, try elsewhere") and is now the owner's bare `ERR`, which carries
no retryable bit and is terminal by the client manual's rule. More truthful —
no core can take this write today — and different, so a client's retry loop
stops spinning where it used to.

### Finding 2 — the assertion gate does not fire on a peer (blocking)

> **Closed 2026-08-26** (PW1c-6c, worktree `ss-check-findings2`,
> `docs/inflight/in-progress/workplan-peer-writer.md` §7d). The measurement
> below stands as it was taken at `v2.2.0-11-g925f483` and nothing in it is
> restated; what changed is the engine. The operator's direction was to fix
> by **ownership** rather than by refreshing the peer's registry, and the
> paragraph below that reads "the fix is not 'let the peer enforce' — it
> cannot" is the part that was superseded: it *can*, once the Bound Cabin is
> built from the owner's own lease and held by the owner, which is what
> PW1c-6c does. The disabled test named here is enabled and its successors
> assert the enforcement as well as the refusal.

**A shipped write to an assertion-covered, peer-owned relation is admitted
and the assertion is not enforced.** Measured: `CHECK COUNT(*) <= 1` declared
on a peer-owned relation, then a shipped `INSERT` puts a **second row in the
same group** (`DISABLED_AShippedWriteToAnAssertionCoveredPeerRelationIsRefused`).

The mechanism, and why this arm alone: the FK and Cabin arms read
`access.fkeys_*` and `access.cabin_ids`, which are catalog-derived and which
`CoreRuntime::InvalidateCatalog` refreshes on every DDL broadcast. The
assertion arm reads `enforcer_.AnyOn(oid)` — a per-core, memory-resident
registry populated **at mount** (recovery RC07) that `InvalidateCatalog` does
not touch: it refreshes the free map, evicts the catalog frames and
invalidates the catalog cache, and nothing else
(`src/server/core_runtime.cpp`, source-read). An assertion declared while a
peer is running is therefore invisible to that peer — its gate does not
refuse, and it cannot enforce either, because the assertion's entry pages are
the system core's and carry no write grant, which is the very reason the gate
exists.

**Not created by shipping, and made ordinary by it.** A client on the peer's
own listener could already reach this. Shipping routes every core-0 client's
write for that relation down the same path. The bound is a remount: a peer
that mounts after the assertion exists rebuilds its registry and refuses
correctly.

The fix is not "let the peer enforce" — it cannot. It is to make the gate
read what the FK and Cabin arms read. That crosses `docs/spec/assertion.md`'s
"complete and enforcing" claim and `docs/spec/crosscore.md`'s peer contract, so it
is reported rather than taken inside a verification task.

## 6. A6 — the G1 class at DML volume

`bench/shipped_dml_churn_probe.py`, the DML form of the DDL churn probe that
found G1. Every statement is issued from a **core-0 session against a
peer-owned relation**, so each one ships.

    python3 bench/shipped_dml_churn_probe.py --cores 4 --iterations 2000 --probe-every 50

| | |
|---|---|
| cores / owner | 4 / core 3 |
| shipped statements | **2,510** |
| poisoned_at | **None** |
| retryable refusals | 2 (the initial lease refills) |
| rows in = rows out | 1,858 = 1,858 |
| after restart | 1,858, and writable |
| `cross_core_write_refusals` | 0 |

G1 poisoned its relation permanently at build 58 of a DDL churn. 2,510
shipped DML statements later this relation is still writable, across a clean
restart, with an interleaved write probe every 50 iterations that never
refused non-retryably.

## 7. A7 — crash recovery, and the single-core floor

`bench/shipped_kill_recovery_probe.py` kills the instance with **SIGKILL**
mid-burst. Both sides of the wire are threads of one process, so what varies
is *when*; the burst runs continuously and the kill lands where it lands.

| kill at | acked | rows after restart | surplus | last acked present | redo core 0 | redo owner |
|---|---|---|---|---|---|---|
| 3.0 s | 2,702 | 2,702 | 0 | yes | 1 | **5,525** |
| 1.2 s | 871 | 871 | 0 | yes | 1 | **1,781** |

An ack implies durability — `DispatchAsync` parks on `IsDurable` — so every
acked statement must survive, and every one did. Surplus 0 at both kill
points: nothing ran that nobody asked for. `shipped_running = 0` on the owner
after recovery, so no shipped statement is half-alive, and the relation takes
writes immediately.

**SS3's claim is now measured rather than asserted.** A shipped statement's
redo lives wholly in the owner's stream: `recovery_redo_applied` reads **1**
on core 0 and **5,525** on the owner that ran the statements. Core 0 accepted
every one of those 2,702 statements from its client and redid one record.

### The `cores = 1` floor

Source-read first: at `cores = 1` the whole fan-out block is skipped
(`src/server/expeditor.cpp:1203`, `if (config_.cores > 1)`), so no transport
is built, `statement_ship_client_` is never constructed, and
`SetStatementShip` is never called — `MayShip`'s first conjunct is a null
pointer test that fails. The single-core path cannot reach the fork.

Then measured, because a source-read is an argument and Guideline 2 asks for
a number. Five reps, **interleaved** rather than blocked — pre, ship, pre,
ship — so a machine that drifts does not charge the drift to whichever arm
ran second. Both arms `build-release`, 4 relations × 2,000 rows, `cores = 1`,
box at 0.87 one-minute load with no compiler running. The baseline is
`29b71ef`, the commit before SS1 — the last tree with no shipping code in it.

| rep | pre agg | ship agg | pre insert p50 | ship insert p50 |
|---|---|---|---|---|
| 0 | 2,535 | 2,874 | 1,601 µs | 2,063 µs |
| 1 | 3,135 | 2,962 | 1,824 µs | 1,822 µs |
| 2 | 2,541 | 2,572 | 1,616 µs | 1,578 µs |
| 3 | 2,608 | 2,554 | 1,700 µs | 1,698 µs |
| 4 | 2,531 | 2,617 | 1,645 µs | 1,628 µs |
| **median** | **2,541** | **2,617** | **1,645 µs** | **1,698 µs** |

**Aggregate 1.030×, insert p50 1.032× — and they disagree in sign.**
Shipping is nominally 3% *faster* on throughput and 3% *slower* on insert
p50, which is what an unresolvable difference looks like: the within-arm
spread is 2,531–3,135 (1.24×) on the pre arm and 2,554–2,962 (1.16×) on the
ship arm, so both between-arm figures sit well inside one arm's own noise.

**The `cores = 1` floor does not move**, which is what the source-read
predicts: the path cannot reach the fork, so there is nothing for it to
cost. Stated as unresolvable rather than as parity — five reps on a shared
box cannot resolve 3%, and claiming 1.000× would be claiming a precision
this run does not have.

---

## The suite

**2,720 of 2,721 Debug**, with 2 disabled — the two findings' tests. The one
failure is `TlsChannelTest.PlaintextGarbageIsFatal`, and it is not this
change's: `docs/inflight/bugs/tls-plaintext-garbage-alert-bytes.md` traces it
to **OpenSSL 3.5.5 on this host**, which queues an alert for a first record
that was never TLS where the test's pinned claim says it queues none. The
fatal path itself is intact. Thirteen tests were added (2,708 → 2,721).

> **The report named above was closed 2026-08-26** (worktree
> `tls-alert-bytes`) and, per `docs/inflight/bugs/README.md`, deleted — the
> path no longer resolves and points at git history. The count in this
> section is what was measured at `v2.2.0-11-g925f483` and stands; what
> changed afterwards is the test, which now pins the channel's contract
> (`docs/spec/protocol.md` §1) instead of the library's byte count.

## What Part A did not do

The `critics-developer` review the session workflow requires per step was
**not run**: this session forbids agent invocation, so the code was
self-reviewed instead. That is a stated gap, not an implied pass.

It did catch one thing worth recording, because it is the failure mode a
verification pass is most likely to ship: `AStatementSpanningTwoOwners...`
first passed on an `ERR` that was a **parse error** — `FROM a, b` is not this
engine's join syntax — so it asserted nothing about the fork. It now asserts
the affinity refusal's own words (`is owned by core`) over a real `JOIN ...
ON`, which is what makes it a test of A5 rather than of the parser.

Part B is untouched, per the order: no throughput cell was run, and no claim
of the memo's three is judged here.
