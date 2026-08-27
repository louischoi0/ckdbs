# Workplan — cross-owner transactions (R6)

The task rows, findings and retractions of `instructions/v2.4.0/2pc.md`.
The work order is the operator's input and is not edited here; this file is
what building it produces. **D1–D7 are not ratified** — only Finding 1's
option is (§2, confirmed 2026-08-27) — so nothing below may be quoted as a
settled design decision except where it says the operator confirmed it.

The deliverable §7 names a spec under `docs/spec/` as the end state. It does
not exist yet and must not be written until the decisions it would state are
ratified; a spec is what is confirmed and implemented (`CLAUDE.md`'s rule for
that directory), and D1–D7 are today proposals with reasoning.

## Status

| # | Task | State |
|---|---|---|
| R6-0 | The retry bit (§2) | **Built 2026-08-27**, `40e9220` |
| R6-1 | Wire and sizing (D6) | **Built 2026-08-27**, this worktree |
| R6-2 | Participant transaction context (D2) | Next; see the sizing note below |
| R6-3 | Prepare and decide (D4) | — |
| R6-4 | Recovery (D4) | — |
| R6-5 | In-doubt handling (D5) | — |
| R6-6 | PW3b extension | — |
| R6-7 | PL-A revisit | — |
| R6-8 | Dispatch | — |
| R6-9 | Docs | — |

## R6-0 — the retry bit

Built on `v2.4.0-2pc-0` at `40e9220`, which is on that branch only — `main`
does not contain it. The operator confirmed §2's proposed option (a retry bit
in the request) rather than the two alternatives the Part A write-up listed.

`ShippedStatementRequestPayload::retry` takes one of the five `reserved0`
bytes — offset 27, `reserved0[4]` after it, `sizeof` unchanged at 1,024 — and
crosses to `StatementShipServer::ShippedStatement::retry`, which
`ShippedStatementExecutor::Execute` reads. A first attempt (`retry=0`)
meeting an absent dedup record still executes; a resend (`retry=1`) meeting
one answers `UnknownOutcome`.

**The branch's placement is load-bearing and is guarded by its own test.**
It sits *after* the `answered_` lookup and the `running_` in-flight check, so
a retry that meets a live record is still answered from it. Hoisting it above
the lookup — the reading "a marked retry never executes" invites — passes the
acceptance test and is caught only by
`ARetryThatMeetsAPresentRecordIsStillAnsweredFromIt`. Both facts were
established by mutation, not by inspection: disabling the branch reproduces
`executed()` 4,097 → 4,098, the exact signature the Part A finding measured.

**The `>` fall-through refuses, and that is not over-caution.** When
`answered_` holds the key at a *lower* sequence, a marked retry still gets
`UnknownOutcome`. The reachable case: sequence 6 runs and records 6, the key
is evicted, a stale unmarked sequence-5 request executes and re-records 5,
and a retry of 6 then meets a record reading 5 even though 6 ran.

**What R6-0 does not close.** The guarantee is a **contract on the sender,
not a property of the owner** — an absent record plus an unset bit is
indistinguishable from a first attempt by construction, so every retry path
built from R6-3 on has to set the bit. `early_evictions()` stops being a
correctness signal and becomes an availability one.

**Deferred with reasons** (reviewed, not overlooked):

- The three `UnknownOutcome` arms in `Execute` are one shape written three
  times. Collapsing them into a helper would make `++unanswerable_`
  impossible for a fourth arm to forget — which matters, because **R6-5 adds
  that fourth arm**. Not done at R6-0: the messages reach clients verbatim
  through `Status::FromWire`, so the collapse must preserve bytes exactly,
  and it belongs where the fourth caller appears.
- A dedicated counter for the new refusal was folded into `unanswerable_`
  instead. A separate `SHOW META` field would read structurally 0 until R6-3
  sets the bit, against the "absent rather than zeroed" rule the shipping
  block already keeps. Split it out at R6-3, when it can be non-zero.

## R6-1 — wire and sizing

Built on `v2.4.0-2pc-0`. Four ring kinds (33–36:
`kTxnPrepareRequest`/`Reply`, `kTxnDecideRequest`/`Reply`), three payloads in
`include/kds/server/txn_2pc_service.hpp`, and the two decodes that bound
bytes the receiving core did not compute.

### D6's answer: they fit, and it is not close

D6 required R6-1 to **confirm** the sizing rather than assume it, and to stop
and report if any message needed more than the slot — never to shrink a field
to make one fit. It did not come to that:

| message | bytes | share of the 1,024-byte slot |
|---|---|---|
| `TxnPrepareRequestPayload` | 24 | 2.3% |
| `TxnDecideRequestPayload` | 24 | 2.3% |
| `TxnParticipantReplyPayload` | 256 | 25% |

So **`crosscore.md` §9's payload-sizing decision is R6's neighbour, not its
gate**, which is what D6 asked to be established here. The assertions are
written against `sched::kCoreRingPayloadBytes` rather than against 1,024, so
a future slot resize re-checks them instead of leaving a stale number behind.

**The answer covers the prepare and decide legs only.** D5 needs a third
exchange — the in-doubt participant's ask — and that kind is R6-5's to
declare and to size. It carries ids and a bit, so it will fit; nothing here
has checked it, and the header says so rather than letting "two phases, four
kinds" read as the whole protocol.

**The reply cap was wrong on the first cut, and the review caught it.** 104
bytes was chosen so `sizeof` landed on 128 — three bytes under
`extent_lease.cpp`'s spent-lease refusal, which measures **107** at a
two-digit page count and is the single most likely prepare failure this
engine has. The tell was in R6-1's own test, which used a *shortened* copy of
that message: a cap sized to a round number, and a test written around it.
That is the inverse of D6's warning — nothing was shrunk to fit the slot, but
a field was cut to fit a self-chosen total while 896 bytes of confirmed
headroom went unspent. The cap is now derived from the measured population
(157 longest, then 144, 109, 51) at 232, `sizeof` 256, and the test asserts
the real strings and the derivation rather than the round number.

### Decisions taken inside R6-1

The reasoning for each is in `txn_2pc_service.hpp`, beside the field or the
struct it governs, which is where a reader of the code meets it. Listed here
only so the series has one index of what R6-1 *decided* as against what the
work order handed it:

- **The decide leg is acknowledged**, which D4 does not name. One enum value,
  no new payload. The tree's own precedent argues for it: `kIndexBuildDone`
  is fire-and-forget and pays for that with a 180 s ceiling its header calls
  a margin "not one the code proves" — an unacked decide reproduces exactly
  that, on a leg where the unacked state is a participant holding locks in
  doubt.
- **One reply payload for both reply legs**, since they differ in the message
  kind and in no field. Pinned by `OneReplyPayloadServesBothReplyLegs`.
- **The coordinator's core is not a payload field** — it is
  `MessageHeader::src_core`.
- **`message_len` bounds the reply text, not a NUL** (SS1's discipline, not
  `IndexBuildReplyPayload`'s).
- **An unreadable decision byte refuses into doubt**, because neither commit
  nor abort is the fail-closed reading.
- **The decide leg's `retry` bit is not R6-0's meaning.** There is nothing to
  re-execute; what it separates is a benign resend from a decide for a
  transaction this core never prepared.
- **No encoders and no deadline constants.** Both need a sender to keep them
  honest, and R6-1 has none. The `Utf8PrefixLen` hoist is R6-3's, when it
  gains its second caller.

### A sizing note R6-2 inherits — found here, not a gate

D6's confirmation covers the prepare/decide messages. It does **not** cover
the path R6-2 actually changes, and that path has far less room:
`ShippedStatementRequestPayload` is full at 1,024 bytes with **4 reserved
bytes left** after R6-0 took one, the rest being 992 bytes of statement text.

R6-2 must associate a shipped statement with the coordinator's transaction,
and D3 would additionally have it carry a per-participant watermark. Carrying
both as fields costs a transaction id (8) plus a watermark (8) against 4
bytes available — so **that** branch of R6-2 does reach `crosscore.md` §9's
sizing decision, and would reach it by shrinking `kShippedStatementTextMax`,
which D6 forbids doing quietly.

The other branch stays inside the 4 bytes, but **not at zero cost — the
first draft of this note claimed "no new wire field at all" and that is
wrong**. A participant already knows `(src_core, session_id)` for every
shipped statement, so enrollment state keyed on that pair needs no *id* on
the wire. But `ShippedStatementExecutor::Execute` mints a fresh session per
statement and `Finish` refuses any statement that left a transaction open, so
the participant cannot tell "enrolled, hold the transaction open" from
"autocommit" without being told — and there is no earlier message to enroll
with, because the *first* shipped statement is the enrollment. That costs one
byte against the 4 available, so the conclusion survives: this branch does
not reach `crosscore.md` §9's sizing decision.

A second input R6-2 should carry: keying enrollment on `(src_core,
session_id)` alone is stale-prone. Ring FIFO ordering covers the normal case,
but a session whose T1 decide was lost leaves an in-doubt enrollment that
T2's first statement would silently join. A 4-byte enrollment epoch fits the
remaining reserved bytes and makes that detectable — which spends the rest of
them, and is the point at which R6-2's branch stops having room to spare.

## Open, carried from the work order

- **D1–D7 await ratification.** R6-0's §2 option is confirmed; the rest are
  proposals.
- **D3's `[OPEN]`**: whether READ COMMITTED cross-owner transactions skip the
  watermark entirely. Proposal in the order is yes.
- **D5's `[OPEN]`**: whether an in-doubt participant blocks writers of the
  same rows or refuses them retryably. Proposal is block, with a bounded wait
  ending in a named refusal.
- **R6-4 must answer `wal.md` §3's second `[OPEN]`** — recovery across a
  core-count change. A refusal to mount is an answer, and is recorded as one.
