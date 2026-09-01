# The typed shipped read — survey, spec draft, and the gate in front of it

Work order XF, row **XF0**, surveyed on the `xf` worktree at `04403a1`
(`git describe --tags` → `v2.7.0-22-g04403a1`). Every path:line below is
that commit's.

> **UNBLOCKED 2026-09-01.** All seven questions are answered by work
> order XG's rulings (`instructions/v2.7.2/workorder-xg.md`, XG-R1..R7),
> the design is landed as spec text in `docs/spec/crosscore.md` §4a, and
> this file moved from `blocked/` to `in-progress/`. XF1's task rows are
> XG1's; §8 is the plan they execute. Where a ruling changed what §4-§5
> proposed, the ruling wins and the difference is noted in place.
>
> The answers, in one line each: **Q1** the answer edge over the existing
> step wire (recommended, taken); **Q2** a read is exempt from the dedup
> record, so a duplicate re-executes; **Q3** the description is its own
> message, **chunked**, so no ceiling is named; **Q4** the text arm keeps
> its 992-byte cap; **Q5** `kStepBatchTargetBytes` and `StepBatchCeiling`
> reused, KW-D2's 64 KiB corrected as socket-side; **Q6** the shrink to
> 976 bytes is accepted; **Q7** the four-item residue confirmed with
> nothing added.
>
> One thing XG settled that this file only raised: a description chunk is
> **its own message kind with its own sequence**, not a `STEP_BATCH` with
> a flag — `StepBatchHeader::seq` is asserted contiguous per edge, so
> folding a differently-shaped payload into it would break the assertion
> or force chunks to be counted as batches.

## 1. What is refused today, and how wide the refusal actually is

`CommandDispatcher::ShipStatement` refuses a shipped **read** whenever the
session has a result sink installed
(`src/server/command_dispatcher.cpp:4257`; KW's reasoning is the comment
above it at `:4232-4256`). Under KWP/1 every session has one
(`src/server/kwp_session.cpp:215-292`, `WireResultSink`), so on a typed
client the refusal is unconditional for every shape that ships.

**The refusal is narrower than "a foreign read" and wider than "an
enrolled read".** Which route a foreign read takes is decided at
`src/server/command_dispatcher.cpp:7274-7277` (the range-stage read
surface's gate) and `:7362-7363` (the widened-shape fall-through):

| shape | route today | typed client |
|---|---|---|
| star read, autocommit, unsorted, no quota | remote-step edge (`remote_reads_->Open`, `:7380`) | **works** — `FinishRemoteReads` emits through the sink (`:5146`, `:5259-5290`) |
| star read spread over ≥ 2 stages | same, one stage per contiguous run (`:7333-7340`) | **works** |
| **projection or fold, one stage** | falls through `widened && stages.size() == 1` (`:7362`) and **ships as text** | **refused** |
| **any read inside an explicit transaction** | `!session.in_explicit_txn()` (`:7277`) excludes the edge; `MayEnrolShip` ships it (`:7479`) | **refused** |
| `ORDER BY`, `LIMIT`, `OFFSET`, hoisted sub-chain | edge gate excludes (`:7275-7276`); ships if `SoleForeignOwner` (`:7477`) | **refused** |
| shipped **write** | ships as text; its answer is a completion tag | unaffected, by design |

The third and fourth rows are the ones that bite. **Scenario 2's booking
opens with a projected read inside a transaction**, which is both of them,
and that is what
`bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md` §4.1 hit on
three binaries before concluding.

Two things follow that `known-gaps.md` does not currently say, and §6
corrects there:

- The engine **already delivers typed rows from a foreign core** for the
  star-read shape. What is missing is not the capability; it is the
  capability on the shipping route.
- The enrolled read cannot simply be handed to the existing edge, because
  the edge executes against the owner's latest-committed view and an
  enrolled read must see **its own transaction's uncommitted writes on
  that peer** — the reason `:7479`'s comment gives for reads shipping at
  all since RR1.

## 2. What the ship wire carries, in bytes

Both ship PODs fill exactly one ring slot, asserted rather than assumed
(`include/kds/server/statement_ship_service.hpp:307-309`):

| quantity | value | source |
|---|---:|---|
| ring slot payload | **1,024 B** | `include/kds/sched/ring_transport.hpp:224` |
| ship request header | 32 B | `statement_ship_service.hpp:146` |
| longest shippable statement | **992 B** | `kShippedStatementTextMax`, `:147-148` |
| ship reply header | 32 B | `:261` |
| **longest shipped reply** | **992 B** | `kShippedStatementReplyTextMax`, `:262-263` |
| what a STEP_BATCH can carry | **1,000 B** | `StepBatchCeiling(1024)`, `step_pipeline.hpp:238-244` |

**So the order's sketch — "the ship reply carries those bytes opaquely" —
cannot be built as written.** A result set does not fit in 992 bytes, and
this is not a corner case: it is already a live defect on the text arm,
registered in `known-gaps.md` as "a shipped read whose reply exceeds 992
bytes is answered `Unsupported`". Typing the reply without moving the rows
off the reply POD would convert one refusal into another.

The rows have to cross on **something that batches**. §4 proposes the
thing that already does.

**And batching does not delete the cap — it moves it from the result to
the row.** The last line of the table is the one to read twice: a batch is
one ring message, so a *row* wider than `StepBatchCeiling` cannot be
carried by any batching policy and is refused `ResourceExhausted` naming
its own width (`known-gaps.md`'s 2026-08-27 entry; the retraction sits
beside the constant at `step_pipeline.hpp:210-219`). Reachable at
defaults — "roughly fifteen full-width varchar columns at
`inline_cell_width = 64`, and a single value at its 4,096 ceiling". So the
honest claim for XF1 is **992 bytes of whole reply → 1,000 bytes of one
row**, which for a result of thirty narrow rows is the difference between
refused and served, and for one wide row is no change at all. Closing
*that* needs a fragmented batch or `crosscore.md` §9's ring sizing, and
neither is this row's to take.

## 3. What already exists, and must not be duplicated

Three facts decide this design more than any preference does.

**(a) There is exactly one row encoding in the engine.** STEP_BATCH
carries "rows in the KWP D5 encoding (`wire/row_codec.hpp`) — the same
bytes `RowBatchWriter` produces, so there is exactly one row format in the
engine" (`include/kds/server/step_pipeline.hpp:222-226`).
`WireResultSink` encodes through `wire::EncodeValue`
(`src/server/kwp_session.cpp:223-254`). A typed shipped read therefore
needs **no new codec**, and adding one would retract that sentence.

**(b) The sink seam has four emission points, and this is a fifth
consumer rather than a fifth formatter.**
`include/kds/server/result_sink.hpp`'s header names them; they are the
local walk (`command_dispatcher.cpp:7649` describe, `:7693` emit), the
sorted drain (`:7716`), the aggregate fold (`:6915`, `:6951`), and the
cross-core fan-in (`:5146`, `:5259-5290`). The owner side of a shipped
read gets a sink installed on its session where today it gets none — the
shipped session is built at
`src/server/shipped_statement_executor.cpp:130-147` and dispatched at
`:155-159`, so `TextResultSink` renders by default
(`command_dispatcher.cpp:7504-7505`).

**(c) The batch bound is the ring slot, and the engine already has two
targets that are not it.** Named plainly, because the order's own sketch
cites the wrong one:

| constant | value | what it bounds |
|---|---:|---|
| `wire::kRowBatchTargetBytes` | 64 KiB | an `S_ROW_BATCH` **frame on a socket** (`include/kds/wire/kwp.hpp:81`) |
| `kStepBatchTargetBytes` | 32 KiB | a STEP_BATCH **target**, not a bound (`step_pipeline.hpp:228`) |
| `StepBatchCeiling(max_message_bytes)` | **1,024 − `sizeof(StepBatchHeader)`** | what a ring slot can actually carry (`step_pipeline.hpp:238-244`) |

The 64 KiB target KW-D2 ratified is a socket-side quantity and **cannot
bound a ring-side batch**, which is three orders of magnitude smaller. The
engine has been bitten by exactly this once: a 32 KiB target against a
1 KiB slot meant "a cross-core read of 42 rows answered zero rows,
silently" (`step_pipeline.hpp:221-226`; it cites
`docs/inflight/bugs/step-batch-wider-than-ring-slot-vanishes.md`, which is
**a stale citation** — the bug file was written and then deleted when the
fix landed, per the bugs bucket's own rule, and the surviving record is
`known-gaps.md`'s 2026-08-27 entry). **This row introduces no third
target** — per the engine's no-second-name rule the answer edge reuses
`kStepBatchTargetBytes` and `StepBatchCeiling` unchanged, and Q5 of the
ask is whether that is what the operator wants.

## 4. The proposal: an answer edge, not a bigger reply

**Shape.** A shipped read keeps its request and its reply exactly as they
are — the statement crosses as text, the owner binds it against its own
catalog, the status and the watermark come back on the reply POD — and the
*rows* cross on a second channel that already exists: the step edge, in
the direction the owner already streams.

```
arrival core                                  owner core
  ShipStatement(read, form = typed)
    mints answer tag T, registers a receiver for it
    ──── SHIPPED_STMT_REQUEST {…, form, T} ─────►
                                          binds; installs ShipBatchSink
                                   ◄──── SHIPPED_ROW_DESC {T, fields} ────
                                   ◄──── STEP_BATCH {T, seq, rows} ───────
    ──── STEP_CREDIT {T} ───────────────────►
                                   ◄──── STEP_BATCH {T, seq+1, …} ────────
    ◄──── SHIPPED_STMT_REPLY {status, watermark, text_len = 0} ──────────
    forwards desc + rows into the session's sink; unregisters T
```

**Five properties, each because the code above forces it:**

1. **No new row codec, no new batch target, no new credit protocol.** The
   batches are `StepBatchBuilder`'s, bounded by `StepBatchCeiling` of the
   ring slot, granted by the credit `SessionStepClient` already returns
   per batch (`include/kds/server/session_step_client.hpp:22-25`).
2. **The requester registers before the owner sends.** `OnStepBatch`
   discards a tag matching no open read
   (`session_step_client.hpp:110-111`), so the tag is **minted by the
   arrival core and carried on the request** — the owner cannot name a tag
   the requester has not registered.
3. **The reply POD is unchanged in size and in meaning.** It is the
   terminator: on a typed read `text_len = 0`, and the rows are the
   edge's. The 992-byte cap stops binding a typed read's *result* — a
   1,000-byte cap on its widest *row* replaces it (§2) — and it still
   binds the text arm, which is untouched (Q4 asks whether that is the
   intended end state).
4. **A text client's arm does not move a byte.** The request's `form` byte
   is 0 for a session with no sink, the owner installs no sink, and
   `TextResultSink` renders exactly what it renders today. That is what
   makes the newline arm's byte-identical suite the regression test for
   this whole row.
5. **The description comes from the owner**, because the owner is the only
   core that compiled the statement. That is also the one genuinely new
   message (Q3): a projected read's field list is not derivable from any
   relation's schema on the arrival core.

**What it costs the request POD.** A 16-byte `PipelineTag`
(`step_pipeline.hpp:39-48`) plus a `form` byte must ride the request.
`ShippedStatementRequestPayload` has exactly one spare byte
(`reserved0[1]`, `statement_ship_service.hpp:254`), so the tag costs
**16 bytes off `kShippedStatementTextMax`** — the longest shippable
statement falls 992 → 976 B. Stated rather than absorbed, because it is a
client-visible bound moving, and it is Q6.

## 5. The five things the spec must say, and where each is forced

### 5a. Capability and version — fail closed, by refusal

A peer that did not understand `form = 1` would ignore the byte, render
text, and answer on the reply POD — and the arrival core would then
forward *nothing* to a client promised a result set, or forward a rendered
line as one opaque value. Both are misparse, which `docs/spec/protocol.md`
§11's posture forbids.

Cores in one instance are one binary, so a skew cannot arise **within** a
mount — but the same is true of nothing else on this wire, and every other
enum here keeps a zero-collision rule
(`statement_ship_service.hpp:224-233`, `:280-291`). The draft therefore
says: `form = 0` is rendered text (today's behaviour, the only value any
existing sender writes), `form = 1` is typed, and **any other value is
refused by name**, never defaulted. An owner that cannot serve `form = 1`
answers `Status::Unsupported` on the reply POD and opens no edge; the
arrival core then refuses the statement rather than delivering a shape the
client cannot branch on. Same fail-closed reading the `role` byte already
takes (`statement_ship_service.hpp:174-183`).

### 5b. Cancellation mid-batch

Three ways a typed shipped read ends other than by its own EOF, and the
edge answers all three because it already answers them for the fan-in:

- **The arrival core's deadline fires** (`kShippedStatementDeadlineNs`,
  10 s). `FinishShippedStatement` returns `UnknownOutcome` for a read with
  "the read returned nothing and changed nothing"
  (`command_dispatcher.cpp:4443-4459`). The registered tag must be closed
  on that path — `SessionStepClient::Close` sends STEP_CANCEL when the
  read is still open remotely (`session_step_client.hpp:114-117`) — or the
  owner streams into a receiver nobody will read, which is the leak the
  fan-in's `CloseAll` guard exists to prevent (`:5062-5069`).
- **The owner fails mid-result.** Rows already emitted are on the edge and
  the reply then carries a non-OK status. **A partial result set must not
  reach the client as a whole one**, so the arrival core holds the batches
  until the reply arrives and forwards only on a status of OK — which is
  what the fan-in does, for the same reason.
- **The client disconnects.** The session's teardown closes the tag on the
  path that already closes a pending shipped statement (`:757-767`).

The one case with no precedent is a **duplicate** (D4): a resend answered
from the dedup record has no rows to answer with. That is 5c.

### 5c. Memory — and the half the order's sketch does not cover

Two quantities, not one.

**(i) The batch in flight** is bounded by the credit ceiling times
`StepBatchCeiling`, exactly as the fan-in's is. The order's "bounded by
the existing target, not a new constant" holds here, with §3c's correction
that the existing constant is `kStepBatchTargetBytes` and the existing
bound is the ring slot.

**(ii) The dedup record is not bounded by anything of the kind.**
`Remember(key, sequence, status, text)` runs on **every** shipped
statement including a read
(`src/server/shipped_statement_executor.cpp:263`) and holds the answer for
`kShippedDedupRetentionNs` = 2 × 10 s = **20 s**
(`shipped_statement_executor.hpp:126-128`) under a 4,096-record cap
(`:133`). Today a read's remembered answer is at most 992 bytes. Typed,
the "answer" is the whole result set, and remembering it makes the record
a result-set cache of unbounded width — 4,096 records × an arbitrary
result. **This is Q2**, and the two defensible answers are opposite in
kind:

- **Exempt a read from the record.** A read has no side effect, so a
  duplicate may re-execute; D4's rule — "guessing is the one thing
  forbidden" — is written about a write against an engine-issued pk. The
  cost is that a re-executed read under READ COMMITTED may answer
  different rows than the original, which is already true of two
  successive RC statements and so is not a new promise broken.
- **Remember the status and not the rows.** A duplicate is then answered
  `UnknownOutcome` rather than re-run — safe, non-retryable, and it costs
  a client a `SELECT` it may safely re-issue itself.

CLA has no basis to prefer one; both are in the ask with the above.

### 5d. What stays refused, and it is named rather than implied

Closing this refusal does not close every shape. After XF1 the following
still do not deliver typed rows from a foreign core, and the spec says so
by name so a client sees one rule rather than a surprise:

- **A row wider than `StepBatchCeiling`** — 1,000 bytes at the shipped
  1,024-byte slot. §2's second half: the cap moves from the reply to the
  row, it does not vanish, and a wide-row relation is refused
  `ResourceExhausted` on this route exactly as it is on the fan-in's.
  Owned by `crosscore.md` §9's ring sizing.
- **A result the edge cannot finish inside the deadline.** The edge
  streams, so *total size* stops being a hard cap; a slow enough result
  still meets the 10 s deadline and is `UnknownOutcome`. Unchanged, stated.
- **`ANALYZE` of a foreign relation**, refused at every route today
  (`:7444`, `:7275`) because the owner would describe a run this core did
  not perform. Untouched.
- **A join over a spread relation**, which is R5's and not this row's.
- **Whatever the operator's answer to Q7 excludes.**

### 5e. The `form` byte is asked, not inferred

The arrival core knows whether its session has a sink
(`session.result_sink() != nullptr`, `:4257`) and that is the whole input:
`form = 1` iff a sink is installed. No config key, no capability
negotiation, no second name for "this client is typed" — the sink's
presence is already the engine's one answer to that question
(`result_sink.hpp:60-64`: "**that is how a caller tells a result set from
a completion**").

## 6. Docs this changes when it lands (XF6's list, not XF0's edits)

- `docs/spec/crosscore.md` §4 — the answer edge, the `form` byte, the
  fail-closed rule, and the ship reply's new role as a terminator.
- `docs/spec/protocol.md` — one sentence: a shipped read reaches a KWP
  client as an ordinary `S_ROW_DESC` + `S_ROW_BATCH` sequence, and a
  client cannot tell from the wire which core answered it. That is the
  property KW's refusal was protecting and this row delivers.
- `docs/inflight/known-gaps.md` — the shipped-read entry closed or
  narrowed to what §5d leaves, **and** corrected on one point of fact: it
  says "the fix is typed rows on the ship wire", which §2 shows the ship
  wire's 992-byte reply cannot carry. The fix is typed rows on the *edge*.
- `CLAUDE.md`'s cross-core row — one line.

## 7. The questions, and which section raises each

Full statements, options and prices in
`instructions/v2.7.1/ratification-xf0.md`. Listed here so a reader of this
file knows what is not settled.

| # | question | raised in |
|---|---|---|
| Q1 | the answer edge, or a different carriage | §2, §4 |
| Q2 | the dedup record's fate for a typed read | §5c |
| Q3 | how the description crosses, and its own bound | §4, §5a |
| Q4 | does the 992-byte reply cap stay on the text arm | §2, §4 |
| Q5 | reuse `kStepBatchTargetBytes`, or scope it | §3c |
| Q6 | 16 bytes off the longest shippable statement | §4 |
| Q7 | which shapes stay refused after this lands | §5d |

## 8a. XG1's build status (2026-09-01)

**The request side is built; the owner's half is not, and the refusal is
still in place until it is.** `kShippedTypedAnswerBuilt`
(`command_dispatcher.hpp`) is the one line that opens the gate, and it is
`false`: shipping `form = 1` to an owner that renders text would answer a
typed client with a rendered line on the reply POD, which is the exact
failure the original refusal exists to prevent. Half-wiring it would be
worse than not wiring it.

| piece | state |
|---|---|
| the `form` byte, its two values, and the fail-closed decode | **built** — `ShippedAnswerTypedOf` refuses any other byte by name, which is also how an owner too old to serve a form answers |
| `PipelineTag answer_tag` on the request POD | **built** — `kShippedStatementFixedBytes` 32 → 48, so `kShippedStatementTextMax` is **976**, asserted where it is derived |
| `SessionStepClient::RegisterInbound` | **built** — a receiver for a tag nothing here opened; no `STEP_OPEN` sent, `output_layout` left empty because the layout is the owner's to state |
| `ShipStatement`: mint, register, ship, close-on-refusal | **built**, behind the gate |
| `PendingShippedStatement` carries the tag across the park | **built** |
| the owner's batch sink on the shipped session | **built** — `WireResultSink` reused with a settable byte target, sealed at the transport's `StepBatchCeiling` instead of the socket's 64 KiB. One sink class, one row encoding, two sealing bounds |
| the answer edge on the owner | **built** — `RemoteStepServer::OpenAnswerEdge` / `PushAnswerBatch` / `CloseAnswerEdge`: a fifth producer on the existing pipeline, so credit, `Drain`, `OnStepCredit`, `OnStepCancel` and the EOF are reused unchanged |
| the description message kind, its chunking and its send | **built** — `kShippedRowDesc` with its own header and sequence, chunked to `ShippedRowDescCeiling`, carrying `wire::EncodeRowDescription`'s bytes |
| the description's reassembly on the arrival core | **built** — `SessionStepClient::OnShippedRowDesc`, in-order or refused, wired through `WireStepEndpoints` |
| shipping the batches under credit | **built** — queued in `Finish`, drained on each `STEP_CREDIT` |
| the dedup exemption (XG-R2) | **built**, narrowed to the typed arm — see the finding below |
| **the arrival core's forward into the session sink** | **not built** — see §8b |

**A finding the build turned up, and it narrows XG-R2 as specified.**
XG-R2 exempts *a read* from the dedup record, but **the owner cannot tell
a read from a write**: nothing on the request says so, and the last spare
byte on the POD went to `form`. What the owner can identify is a *typed*
answer, which the arrival core sets only at the read fork — so the
exemption is buildable exactly where XG-R2's own second paragraph argues
for it (a typed read's "answer" is the whole result set, and a record
keeping it would be a result-set cache needing a cap). A **text-arm** read
keeps its record, which is today's behaviour and is bounded at 992 bytes,
so nothing is unbounded either way. Stated here rather than silently
narrowed; it wants the operator's eye only if the wider exemption was
wanted for its own sake.

## 8b. The one piece left, and the decision inside it

**What remains is the arrival core's forward**: on an OK terminator, take
the reassembled description and the stored batches and put them into the
session's `ResultSink`. Everything either side of it is built and green.

**It is not a gap, it is a question the `ResultSink` seam does not
answer.** XG-R1 says the session-side sink "forwards frames without
re-encoding", and the rows on the edge are already in the D5 encoding a
`WireResultSink` emits — so a byte-for-byte forward is exactly right *for
that sink* and exactly wrong for a `TextResultSink`, which takes rendered
text through the same `Emit`. The interface cannot currently tell the two
apart, and `Emit`'s contract is "a row an `Encode*` call produced" —
meaning *this* sink's encoder.

Three ways out, and CLA recommends the second:

1. **Decode and re-encode through the seam**, as `FinishRemoteReads` does
   for the fan-in. Works for any sink and needs no interface change, but
   it contradicts XG-R1's "without re-encoding", and it needs a
   `FieldDescription` → `SysColumnRow` synthesis to reach
   `wire::FieldToValueChecked` — which is where a `varchar`'s `len` and a
   `type_mod` are not obviously the same field, and getting it wrong is a
   mis-widthed value rather than an error.
2. **Let the sink say whether it takes encoded rows.** A predicate on
   `ResultSink`, false by default and true on `WireResultSink`, and the
   forward is then a copy. Honours XG-R1 literally, costs three lines, and
   the discriminated interface is honest rather than hidden — a text
   session never reaches this path, because `typed_answer` requires a sink
   and the text arm ships `form = 0`.
3. **Hand the sealed batch to the sink whole**, since `WireResultSink`
   already frames batches for delivery. Fewest copies of all, and the
   largest interface change.

Stopped here rather than guessed at, because option 1's synthesis is the
kind of thing that produces plausible values instead of an error.

## 8. XF1's tasks, written against answers not yet given

Listed so the ask is read against the work it authorizes. **None starts
until §7 is answered**; a task whose shape an answer changes says so.

- **XF1-a — the wire.** `form` and the answer tag on the request; the
  reply's terminator role; the description message. *Shape depends on Q1,
  Q3, Q6.*
- **XF1-b — the owner's sink.** A `ShipBatchSink` over `StepBatchBuilder`,
  installed on the shipped session when `form = 1`, sealing at
  `StepBatchCeiling` of the transport's slot. *Depends on Q5.*
- **XF1-c — the arrival core's forward.** Register on ship, hold batches,
  forward description then rows into the session's sink on an OK reply,
  close on every exit. Reuses `FinishRemoteReads`'s `CloseAll` discipline
  and must **not** re-encode: the bytes are already the codec the sink
  emits.
- **XF1-d — the refusal, narrowed rather than deleted.** `:4257` keeps
  refusing what Q7 leaves out, and names it.
- **XF1-e — the dedup record.** *Depends entirely on Q2.*

**Tests XF1 owes**, per the order:

1. `tests/testdata/kwp_golden.txt` gains a shipped-read byte session.
2. The newline arm's byte-identical suite must not move — the whole
   regression test for §4's property 4.
3. A `cores = 1` guard cell and a `peer_listeners = off` guard cell, each
   unchanged within its floor.
4. A partial-failure cell: the owner errors after batch 1, the client sees
   a failed statement and **no rows**.
5. A deadline cell: the tag is closed and the owner's later batches are
   discarded rather than delivered.
