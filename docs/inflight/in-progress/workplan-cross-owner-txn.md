# Workplan — cross-owner transactions (R6)

The task rows, findings and retractions of `instructions/v2.4.0/2pc.md`.
The work order is the operator's input and is not edited here; this file is
what building it produces. **D1–D7 are not ratified** — confirmed are
Finding 1's option (`instructions/v2.4.0/2pc.md` §2, 2026-08-27) and the two
`[OPEN]`s *inside* D3 and D5, which are items in those rows and not the rows
themselves ("§2 — the ratification record" below) — so nothing below may be
quoted as a settled design decision except where it says the operator
confirmed it.

The deliverable §7 names a spec under `docs/spec/` as the end state. It does
not exist yet and must not be written until the decisions it would state are
ratified; a spec is what is confirmed and implemented (`CLAUDE.md`'s rule for
that directory), and D1–D7 are today proposals with reasoning.

## §2 — the ratification record

The work order's RP0 row (`instructions/v2.5.0/cross-owner-protocol.md` §5)
makes this record the milestone's first row and its own gate: *"If any is
unratified, this order stops here and reports."* This section is that
report. It is titled §2 because that is where RP0 points; this file has
never numbered its sections, and the order's own §2 — "The gate this
milestone opens on" — names exactly D1–D7 and the two `[OPEN]`s.

### Ratified — the two `[OPEN]`s, in the operator's own words

`instructions/v2.5.0/cross-owner-protocol-operator-input.md` holds the two
messages as they came, on 2026-08-27, and is the source of the quotations;
it and the work order are operator input and are not edited here.

| item | the operator's words | recorded as |
|---|---|---|
| **D3's `[OPEN]`** — the watermark under READ COMMITTED | *"READ COMMITTED cross-owner 트랜잭션이 watermark를 건너뛰는가. 제안은 「그렇다」."* | **Ratified as proposed.** A READ COMMITTED cross-owner transaction **skips the watermark entirely**; watermarks are carried for REPEATABLE READ only |
| **D5's `[OPEN]`** — an in-doubt participant against a writer of the same rows | *"in-doubt participant가 같은 행 writer를 막는가, retryable하게 거절하는가. 제안은 「막되 상한을 두고 named refusal로 끝낸다」."* | **Ratified as proposed.** The participant **blocks**, with a bounded ceiling ending in a **named refusal** — not a retryable refusal up front |

**The reading is CLA's, and it is the one thing in this section that could
be wrong.** Each message restates its question and names its proposal
without a verdict word; they are read as ratifications because the operator
sent them unprompted against an order that gates on exactly these two. If
either reading is wrong, this section is where it is corrected, and the
correction costs nothing until R6-3 builds on it.

Two consequences these answers now have, stated here because they were
conditional before and are not any more:

- **D3's answer makes the isolation level's travel load-bearing.** R6-2's
  note below, "What D2 does and does not give D3", records the gap:
  `EnrolFor` opens the participant's transaction at the *participant's* own
  `default_isolation()`. What ratification changes is that gap's weight —
  the level now selects between carrying a watermark and not carrying one,
  so a participant that mistakes it takes the **wrong branch** rather than
  merely a weaker view. R6-3 owes the crossing; `reserved0[3]` has room.
- **D5's answer fixes R6-5's shape**: the participant blocks a writer of the
  same rows and the wait ends, at a ceiling, in a named refusal — a wait and
  a refusal to build, not a retryable decline. Whether that refusal is the
  fourth `UnknownOutcome` arm R6-0 deferred its collapse for ("R6-5 adds
  that fourth arm") is *not* settled by it: those three arms answer a
  duplicate *shipped statement* in `ShippedStatementExecutor::Execute`,
  where this one answers a *different* transaction meeting an in-doubt
  one's rows. The ceiling's value and the refusal's spelling are **not**
  ratified with the shape and are R6-5's to propose.

### Not ratified — D1–D7, and this is where the order stops

The two items above are `[OPEN]`s *inside* D3 and D5, not the D-rows
themselves. The D-rows remain what this file's header has said since it was
opened: proposals with reasoning, in `instructions/v2.4.0/2pc.md` §3.

| row | subject | status | what waits on it |
|---|---|---|---|
| **D1** | the arrival core coordinates; participants discovered as the transaction runs; a one-owner transaction takes the single-core path unchanged, paying nothing | **awaiting ratification** | R6-3's short-circuit and R6-8's participant discovery, and B3/HP1, which are D1's gate expressed as a measurement |
| **D2** | a participant runs a local transaction with its own trx id, keyed by the coordinator's `(session_id, transaction_id)` | **awaiting ratification** | R6-3's `PREPARE` record, which is where the coordinator's pair is actually written (R6-2 carries none — it finds the transaction by `(src_core, session_id)`). R6-2 was buildable regardless: an enrolment keyed on a pair of ids survives any D-choice (the v2.5.0 order's §2) |
| **D3** | the per-participant watermark; consistent-per-core, never a global instant | **awaiting ratification** (its `[OPEN]` is ratified above) | R6-3's read path |
| **D4** | two phases; the decision lives in exactly one stream, the coordinator's | **awaiting ratification** | R6-3 and R6-4 in full |
| **D5** | in-doubt handling and the client-facing `UNKNOWN_OUTCOME` contract | **awaiting ratification** (its `[OPEN]` is ratified above) | R6-5, and through it R6-8 (the parent gates that row on R6-3 and R6-5); R6-9 owes the read-the-data contract to `client-manual.md` |
| **D6** | prepare and decide fit the ring slot with no resize | **awaiting ratification** | nothing further for the two legs — R6-1 *confirmed* their fit by `static_assert` (24/24/256 bytes), which is evidence and not a decision. D5's third exchange is outside that answer and R6-5 sizes it (R6-1's note below) |
| **D7** | two durable syncs deep, participants wide up to the device's four | **awaiting ratification** | R6-B's B1 and B2, which exist to confirm or refute it |

**No default is taken for any of the seven, and none may be.** R6-3
implements D4 directly and reads D3's watermark rule and D5's in-doubt rule,
so building it against unratified proposals means writing the protocol
twice — the v2.5.0 order's §2 says so and this row does not soften it.
R6-0, R6-1 and R6-2 were buildable under the same gate only because a wire
bit, a sizing `static_assert` and an enrolment keyed on a pair of ids
survive any D-choice; nothing from R6-3 on does. **So every RP row after
RP0 stays pending behind this gate**, and the order stops here and reports —
which is RP0's stated function rather than a failure of it.

### Finding — the work order's citation of its parent does not resolve

`instructions/v2.5.0/cross-owner-protocol.md` opens — line 5, in its
preamble above §0 — *"The second half of `instructions/v2.5.0/2pc.md`."*
No such file exists: `instructions/v2.5.0/` holds `cross-owner-protocol.md`
and `cross-owner-protocol-operator-input.md` only, and the parent order —
the one that owns R6 whole, carries D1–D7 in its §3 and the §5 correctness
gate RP7 runs — is `instructions/v2.4.0/2pc.md`, which this file's own first
line has cited since it was opened. Recorded rather than corrected: the work
order is operator input and is not edited here. Every citation of the parent
in this workplan means the v2.4.0 path.

## Status

| # | Task | State |
|---|---|---|
| R6-0 | The retry bit (§2) | **Built 2026-08-27**, `40e9220` |
| R6-1 | Wire and sizing (D6) | **Built 2026-08-27**, this worktree |
| R6-2 | Participant transaction context (D2) | **Built 2026-08-27**, this worktree |
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

## R6-2 — participant transaction context

Built on `v2.4.0-2pc-0`. The largest row, and it came out small, because the
transaction it introduces is an **ordinary local transaction**: opened with
the same `Dispatch("BEGIN")` a client's connection runs, held on a session
that outlives the statement, ended with the same `ROLLBACK`. Zero new
transaction code — the `KwpLoadServer` argument, which already holds one
session's transaction across many messages, applied one layer over.

**The wire cost was one byte**, as the R6-1 note predicted:
`ShippedStatementRequestPayload::in_txn`, leaving `reserved0[3]`. It carries
no transaction id, because the owner finds the transaction by
`(src_core, session_id)` and records the coordinator's id when prepare brings
it (R6-3). The enrolment epoch the note floated is **not** built — the
staleness it guards against is handled by refusing instead, below.

**The fork is one `if`.** `Execute` routes an enrolled statement onto the
context's session and an autocommit one onto a session of its own; `Finish`
leaves an enrolled transaction open and keeps SS3's refusal *verbatim* for
the autocommit path. `Running` gained a `std::optional<Session> own_session`
plus a `Session*`, so which session is in play is a pointer rather than a
second code path.

**What is refused rather than guessed:**

- A context whose transaction is no longer open is **not joinable** —
  `UnknownOutcome`, not a silent new transaction. This is what the epoch
  would otherwise be for: a stale enrolment cannot be joined because a closed
  one is refused and an open one is the transaction the coordinator means.
- An enrolled statement that *ends* its own transaction (a shipped `COMMIT`)
  is `Unsupported` and the context is dropped. The decision is the
  coordinator's (D4), and a context left standing after its transaction died
  would run the next statement outside any transaction.
- A failed `BEGIN` records **nothing** — no half-open context for the next
  statement to join. A spent transaction-id lease surfaces here as the
  `TxnConflict` it is, retryable, and crosses back intact.

Refused enrolments are counted apart from `unanswerable()`, which is a
*duplicate* count whose every member is a client told `UNKNOWN_OUTCOME`; an
enrolment refusal is neither a duplicate nor in doubt, and folding the two
would make the one number SS-B4 reads mean two unrelated things.

**Not yet handled, and named so R6-3 finds it**: a context whose session is
*poisoned* (`kFailedTxn`) is still joinable, because `in_explicit_txn()` is
`state_ != kIdle`. Every later statement then meets the failed-txn gate and
returns "current transaction is aborted", which carries nothing telling the
coordinator "this participant is doomed, decide ABORT". That is R6-3's to
answer; what R6-2 owes it is the note.

**The lifetime bound, and the reason it is generous.** `kShippedTxnIdleCeilingNs`
is five minutes of *idleness* — the stamp moves when a statement **finishes**,
so a statement that runs four minutes does not eat its coordinator's grace.
It is a backstop for a lost abort, not a normal-path bound: a cross-owner
transaction is a client's `BEGIN … COMMIT` and the gap between two of its
statements is client think time, which has no engine-side bound. Being wrong
tightly rolls back a transaction a client is still using — a wrong answer.

**The sweep is registered twice, and the first cut registered it once.** A
peer gets it from `CoreRuntime::Run`; core 0's executor belongs to
`Expeditor`, which is a different object, so the original registration
covered cores 1..N−1 while three comments and this file claimed "every core".
That was a false report of compliance on the case the comment itself names —
core 0 is a participant whenever a peer's client writes a core-0-owned
relation, which under a rotating `AssignOwnerCore` is ordinary. Both the tick
and the shutdown rollback now exist in `Expeditor::Serve` as well, the latter
immediately after `scheduler.Run()` returns — the same "the reactor has
stopped, nothing will decide one now" moment `~CoreRuntime` uses.

**A second cost the first draft did not price.** An enrolment is not only a
read-horizon pin; it is one of `txn::kMaxTrackedLiveTxns` (64), the whole
core's supply, **shared with every local client**. Enough abandoned
enrolments would refuse an unrelated connection's `BEGIN` with `OutOfSpace`
and nothing naming the cause. `kShippedMaxEnrolled = 16` — a quarter of the
table — is the cap, and past it a participant refuses `TxnConflict`, the one
code the wire's retryable bit follows, because another cross-owner
transaction ending is a thing that happens on its own.

**A constraint R6-3 must honour, written here because that is where it would
silently become wrong**: `ExpireEnrolled` is only sound while nothing has
prepared. After a participant replies prepared it may not unilaterally abort
(D4), so R6-3 must exclude prepared contexts from the sweep, and the ceiling
for those belongs to D5's in-doubt resolution instead. The sweep already
skips a context with a statement running on it, for the reason
`TcpServer::CloseClient` defers teardown behind `in_flight`.

### What D2 does and does not give D3 — and it is isolation-dependent

A first draft of this note claimed D3's per-core ReadView "mostly falls out
of D2". **That is only true under REPEATABLE READ, and it is not the
default.** `CommandDispatcher::default_isolation_` is `kReadCommitted`, and
`TransactionManager::StartStatement` re-mints the read view on every
statement under RC — holding one transaction open across statements
therefore does *not* by itself give a participant a stable view. Under RR it
does: `StartStatement` is a no-op there, so the view taken at `BEGIN` is the
view every statement of that transaction sees.

So, precisely:

- **Under RR**, D2's held transaction delivers "a ReadView per core"
  structurally. A watermark would add only the stronger property of pinning
  that view to a point the *coordinator* has observed.
- **Under RC** (today's default and what an enrolled statement runs at), the
  view is deliberately re-minted per statement, so there is no stable view
  for a watermark to stabilise — and none is wanted, because seeing
  everything committed before the statement began is what RC *means*.

That is an argument **for** D3's own proposed answer to its `[OPEN]`: carry
watermarks only for RR. The two halves agree, and R6-2 is the evidence.
(That `[OPEN]` was ratified as proposed on 2026-08-27 — "§2 — the
ratification record" above. D3 itself is still unratified.)

**And a gap the same reading exposes**: the coordinator's isolation level
never crosses. `EnrolFor` opens with the *participant's* `default_isolation()`
— its own server config — so a client that writes `BEGIN ISOLATION LEVEL
REPEATABLE READ` gets an RC participant transaction and does not learn it.
Whichever way D3 is ratified, the level has to travel or the answer above is
about a level nobody selected. `reserved0[3]` still has room.

### An unrelated soundness note R6 invalidates

`TransactionManager::IsInFlight`'s header says it is per-core and that a
transaction on another core answers false — *"Sound for its one user only
while CC3 refuses cross-core writes."* **R6 is what makes CC3 stop
refusing.** Nothing in R6-2 depends on it (a participant's transaction is
local and this core's own manager knows it), but the note's stated premise
expires somewhere in this series, and R6-8 is where the refusal actually
lifts. Recorded so it is found then rather than assumed still true.

## Open, carried from the work order

- **D1–D7 await ratification**, and that is where R6-P stops: RP0 reports
  the gate and every later RP row stays pending behind it. R6-0's §2 option
  is confirmed; the rest are proposals. Per-row status is in "§2 — the
  ratification record" above.
- **D3's `[OPEN]` is closed** — ratified as proposed 2026-08-27: a READ
  COMMITTED cross-owner transaction skips the watermark entirely. What it
  leaves open is the coordinator's isolation level crossing to the
  participant, which R6-3 owes.
- **D5's `[OPEN]` is closed** — ratified as proposed 2026-08-27: block, with
  a bounded ceiling ending in a named refusal. The ceiling's value and the
  refusal's spelling are not ratified with it and are R6-5's to propose.
- **R6-4 must answer `wal.md` §3's second `[OPEN]`** — recovery across a
  core-count change. A refusal to mount is an answer, and is recorded as one.
