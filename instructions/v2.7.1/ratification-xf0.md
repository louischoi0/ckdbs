# Ratification ask XF0 — the typed shipped read: what carries the rows

Drafted 2026-08-31 by CLA on the worktree `xf` at `04403a1`
(`v2.7.0-22-g04403a1`), as row **XF0** of work order XF
(`instructions/v2.7.1/workorder-xf.md`). **Nothing is built under this
ask.** It is a source-read survey and the wire it implies, put to the
operator; the survey itself and every path:line behind the claims below
are `docs/inflight/blocked/workplan-shipped-read-typed.md`.

**Why it is an ask at all.** Six of the seven questions change a wire
form, and the seventh changes what a client is told it may not do. The
order's own conclusion 2 says spec and ratification precede code here, and
its conclusion 4 says no constant is chosen. XF1 is written and does not
start (`workplan-shipped-read-typed.md` §8).

## What is broken, in one paragraph

Under KWP/1 every session carries a result sink, and `ShipStatement`
refuses a shipped **read** to a session that has one
(`src/server/command_dispatcher.cpp:4257`). That is not a corner: a
projected read of a foreign relation, and **every** read inside a
cross-owner transaction, take the shipping route rather than the typed
remote-step edge (the routing gates at `:7274-7277` and `:7362`). So a
typed client cannot read foreign data inside a transaction at all — which
is the production limitation — and the engine's own scenario-2 booking,
which opens with exactly that read, cannot run under `peer_listeners = on`
— which is the measurement blocker XE hit
(`bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md` §4.1).

## The one correction the order's sketch needs, up front

The order says "the ship reply carries those bytes opaquely". **It cannot
be built that way.** The ship reply is one ring slot: 32 bytes of header
and **992 bytes** of text (`statement_ship_service.hpp:261-263`, asserted
at `:306`). Typing a reply that cannot hold a result set converts one
refusal into another — and that second refusal already exists on the text
arm, in `known-gaps.md`, as "a shipped read whose reply exceeds 992 bytes
is answered `Unsupported`".

So the rows must cross on something that batches. Everything below follows
from that.

---

## Q1 — What carries the rows?

**(a) An answer edge on the existing step wire. — CLA's recommendation.**
The arrival core mints a `PipelineTag`, registers a receiver, and sends it
on the ship request; the owner installs a batch sink on the shipped
session and streams STEP_BATCH to that tag under the credit protocol that
already exists; the ship reply POD becomes the terminator carrying the
status and the watermark. Reuses the codec, the batch builder, the credit
grant, the cancel and the ceiling unchanged — the fan-in has run this
machinery since P4b.

**(b) A second, ship-specific batch stream.** New message kinds, new
credit, new ceiling. Buys nothing (a) does not, and adds a second answer
to "how do rows cross a ring", which is what the engine's own
no-second-name rule exists to prevent.

**(c) Route the read through the remote-step edge instead of shipping it**
— teach STEP_OPEN to carry `(coordinator core, session id, join)` so the
owner runs the step inside its participant transaction. Cheapest wire, and
it closes the scenario-2 case. **It does not close the others**: the edge
refuses a projection with one stage (deliberately — it folds on the owner
and ships one row instead of every row, `:7355-7362`), a sort, a quota, a
sub-chain, and any step the descriptor codec declines. It also moves plan
binding to the arrival core for a shape the ship path binds on the owner
on purpose (`statement_ship_service.hpp:37-50`). A partial fix that leaves
the same refusal reachable from four other shapes is worse than either
whole one.

**(d) Re-parse the owner's rendered text into typed values on the arrival
core.** Priced and refused rather than omitted: it is a second formatter
for the row, which `result_sink.hpp`'s header exists to forbid, and it is
lossy on the first value containing a comma.

> **What (a) buys, stated honestly, because it is smaller than "the cap
> goes away".** A batch is one ring message, so a **row** wider than
> `StepBatchCeiling` — 1,000 bytes at the shipped slot — is still refused
> `ResourceExhausted` (`known-gaps.md`, 2026-08-27; `step_pipeline.hpp:210-219`).
> (a) moves the cap **from the whole reply to the widest row**. Thirty
> narrow rows go from refused to served; one wide row does not move.
> Closing that needs a fragmented batch or `crosscore.md` §9's ring
> sizing, and neither is XF1's.

---

## Q2 — What does the dedup record hold for a typed read? **No recommendation.**

`Remember(key, sequence, status, text)` runs on **every** shipped
statement, read included
(`src/server/shipped_statement_executor.cpp:263`), and holds the answer
for `kShippedDedupRetentionNs` = 20 s under a 4,096-record cap. Today a
read's remembered answer is ≤ 992 bytes. Typed, the answer is the result
set, and remembering it makes D4's record a result-set cache of unbounded
width.

**(a) Exempt a read from the record; a duplicate re-executes.** A read has
no side effect, and D4's rule — "guessing is the one thing forbidden" — is
written about a write against an engine-issued pk. Cost: a re-executed
read under READ COMMITTED may answer different rows than the original,
which two successive RC statements already may.

**(b) Remember the status and not the rows; a duplicate is
`UnknownOutcome`.** Safe and non-retryable; costs a client a `SELECT` it
could have re-issued itself.

**(c) Remember the rows under a stated cap.** Restores today's behaviour
exactly and needs a number, which conclusion 4 says CLA does not pick.

CLA has no basis to prefer one of these: (a) and (b) are both defensible
readings of D4 and the choice is about what the engine promises, not about
what the code can do.

---

## Q3 — How does the description cross, and what bounds it?

The owner is the only core that compiled the statement, so a projected
read's field list is not derivable on the arrival core. It has to cross.

**(a) Its own message on the answer edge, before the first batch. — CLA's
recommendation**, because it is the only one of the three that does not
put a variable-length list into a fixed 992-byte POD.

**(b) On the ship reply POD.** The reply arrives *last* (it is the
terminator), so this reverses the order the client needs, and a wide
description does not fit anyway.

**(c) Derive it on the arrival core from the relation's schema.** Correct
only for a star read, which is the one shape that already works without
any of this.

**The bound this opens, and it is real:** the engine has **no column-count
cap** — CLA searched for one and there is none. A description is
`name + type_oid + type_len + flags + type_mod` per field
(`include/kds/wire/row_codec.hpp:86-101`), so a wide relation's
description can exceed a ring slot exactly as a wide row can. Either the
description chunks, or a ceiling is named. **A ceiling is a constant, so
CLA stops here rather than choosing one.**

---

## Q4 — Does the 992-byte reply cap stay on the text arm?

After Q1(a), a typed shipped read is bounded by the row, and the text arm
is bounded by the whole reply at 992 bytes, unchanged. Two end states:

**(a) Leave it.** The debug surface is a debug surface (KW-D6), and its
limits are a debug surface's. **CLA's recommendation**, on the grounds
that spending ring budget on the arm nothing in production speaks is the
wrong place for it.

**(b) Give the text arm the same edge**, rendering into the batch stream.
One code path instead of two, at the price of touching the one suite whose
byte-identity is XF1's whole regression proof.

---

## Q5 — Which batch target governs the answer edge?

Three exist and they are not interchangeable:

| constant | value | bounds |
|---|---:|---|
| `wire::kRowBatchTargetBytes` | 64 KiB | an `S_ROW_BATCH` frame on a **socket** (`wire/kwp.hpp:81`) |
| `kStepBatchTargetBytes` | 32 KiB | a STEP_BATCH **target** (`step_pipeline.hpp:228`) |
| `StepBatchCeiling(slot)` | 1,000 B | what a **ring** message can carry (`:238-244`) |

The order cites "the 64 KiB batch target already ratified for it" — that
is KW-D2's, and it is a socket-side quantity that cannot bound a ring-side
batch three orders of magnitude smaller. **Reuse `kStepBatchTargetBytes`
and `StepBatchCeiling` unchanged (CLA's recommendation)**, or say a
ship-specific target is wanted — in which case it is a constant and CLA
stops for the number.

---

## Q6 — 16 bytes off the longest shippable statement

The answer tag is a 16-byte `PipelineTag` (`step_pipeline.hpp:39-48`) and
the request POD has exactly one spare byte (`reserved0[1]`,
`statement_ship_service.hpp:254`). So carrying it costs
`kShippedStatementTextMax` **992 → 976 bytes** — a client-visible bound
moving, which is why it is asked rather than absorbed.

**(a) Accept the shrink.** **(b)** Carry only `request_id` and derive the
rest of the tag from the requester core and a fixed step id — 8 bytes
instead of 16, at the cost of a tag whose shape differs from every other
tag in the engine. **(c)** Raise the ring slot, which is `crosscore.md`
§9's open sizing decision and not XF1's.

---

## Q7 — What stays refused after this lands, by name

The refusal at `:4257` is narrowed, not deleted, and the spec states what
survives so a client sees one rule rather than a surprise. CLA's reading
of what must survive:

1. a row wider than `StepBatchCeiling` (Q1's note);
2. a result too slow to finish inside the 10 s deadline —
   `UnknownOutcome`, unchanged;
3. `ANALYZE` of a foreign relation — the owner would describe a run this
   core did not perform;
4. a join over a spread relation — R5's.

**Is that the list?** Anything the operator adds becomes a named refusal
in `crosscore.md` §4 rather than an undocumented edge.

---

## What CLA will do with each answer

- **Q1(a) + Q3(a) + Q5 reuse + Q6(a)** is one coherent build, and XF1-a
  through XF1-e in `workplan-shipped-read-typed.md` §8 are written for it.
- **Q2** decides XF1-e entirely; nothing else depends on it.
- **Q4(b)** would add a task and put the newline byte-identity suite in
  the blast radius, which XF1 currently keeps out of it.
- **A "no" to Q1(a)** stops XF1 and reopens §4 of the workplan.

Nothing lands until this file has answers.
