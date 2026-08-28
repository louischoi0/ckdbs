# Workplan — cross-owner transactions (R6)

The task rows, findings and retractions of `instructions/v2.4.0/2pc.md`.
The work order is the operator's input and is not edited here; this file is
what building it produces. **D1–D7 are ratified** — by the operator's
"follow CLA proposal" on 2026-08-28, together with the two `[OPEN]`s inside
D3 and D5 (2026-08-27) and Finding 1's option
(`instructions/v2.4.0/2pc.md` §2, 2026-08-27). "§2 — the ratification
record" below holds all of it with the operator's wording, and is what a
claim of settledness cites; a row not appearing there is not settled by this
file.

The deliverable §7 names a spec under `docs/spec/` as the end state. It
still does not exist, and now for the other of its two reasons: a spec is
what is confirmed **and implemented** (`CLAUDE.md`'s rule for that
directory), and R6-3 onward is not built yet. R6-9 writes it once RP7's gate
passes.

## §2 — the ratification record

Where the work order's RP0 row (`instructions/v2.5.0/cross-owner-protocol.md`
§5) points; this file has never numbered its sections, so the title is that
pointer rather than an ordinal. RP0 reported an unratified gate on
2026-08-27 and the operator opened it on 2026-08-28. Both halves are kept
below, in that order, because a record that erases its own gate cannot be
audited.

### Ratified 2026-08-28 — D1–D7, by the operator's "follow CLA proposal"

The operator supplied the ratification packet — *"Ratification packet — R6
design decisions D1–D7"*, prepared against `main` at `ec5f993` — and ratified
it with the words **"follow CLA proposal"**. That settles every row at the
packet's own summary table, CLA's reading being the ratified wording in each
 case. The source is
`instructions/v2.5.0/cross-owner-protocol-operator-input.md`.

| # | ratified as |
|---|---|
| **D1** | As written. The **arrival core coordinates**; participants are relation owners **discovered as the transaction runs**; a one-owner transaction takes the single-core path unchanged. Forecloses data-chosen coordinators |
| **D2** | As written **with its rejection reason amended**. Per-participant local trx ids from each core's own lease, the coordinator's `(session_id, transaction_id)` recorded beside them. The shared-id alternative is rejected **on mount validation** — `CoreRuntime::Open` refuses a mount whose peer stream names an id above the superblock's ceiling (`include/kds/txn/trx_id.hpp:110-113`), so a shared id puts foreign ids in every participant's stream — and **not** on the parent order's "global atomic counter or cross-stream ordering" grounds, which are **false at the source**: the trx-id domain is already global (`trx_id.hpp:92-93`, `trx_id_lease.hpp:11-18`). The outcome is unchanged; the reason is corrected so a later reader does not find it false and reopen a settled decision |
| **D3** | As written. A per-participant **watermark**, giving a cross-owner RR transaction a **consistent-per-core** snapshot and not a globally consistent one — a weakening that is a product property for `client-manual.md`, not only a spec line |
| **D3's `[OPEN]`** | **Yes — READ COMMITTED skips the watermark entirely**; watermarks are carried for REPEATABLE READ only |
| **D4** | As written. Two phases over the existing ring; a participant that replied prepared may not unilaterally abort; **the `COMMIT` in the coordinator's own stream is the decision, and it lives in exactly one stream**. Forecloses one-phase commit and presumed-commit/presumed-abort |
| **D5** | As written. An in-doubt participant may neither abort nor commit, holds its locks and waits; resolution is a retry in Finding 1's sense, so a coordinator that no longer holds the record answers `UnknownOutcome`; after `UNKNOWN_OUTCOME` the remedy is to **read the data**, never to retry, in the words shipped statements already use |
| **D5's `[OPEN]`** | **Block**, with a **bounded ceiling ending in a named refusal** |
| **D5's ceiling value** | **CLA's to propose and measure** — the packet offered that branch and "follow CLA proposal" takes it |
| **D6** | **Confirmed discharged** by R6-1 at `c97f5ca`: 24 bytes per request leg and 256 for the participant reply against a 1,024-byte slot, asserted against `kCoreRingPayloadBytes` rather than the literal. **R6-5's in-doubt ask is still owed its own sizing** |
| **D7** | Ratified **as a pre-registered prediction, not a decision**: two syncs deep, up to four participants wide |

**CLA note on D2's amended reason — not a re-opening.** The mount check is
real and the outcome stands, but it does not reach every shape of the
rejected alternative. `CoreRuntime::Open` refuses a stream whose high-water
id exceeds the **superblock's** ceiling
(`src/server/core_runtime.cpp:177-186`), and a shared id *carved from the
coordinator's lease* is below that ceiling by construction — `Carve` persists
the raise before returning the range (`trx_id.hpp:104-113`). So the check
fires on the unpersisted-global-counter variant and **not** on the carved
one; what rejects the carved variant is D2's own positive reason, each
stream's ids coming from its own core's lease. Written here because the
amendment exists precisely so a later reader does not find the reason false
and reopen a settled decision, and this is the one case where reading it
literally would.

### What the ratifications oblige — inherited by the row that owns each

Written here so RP1, RP3 and RP8 collect these rather than rediscovering
them. Three are the packet's own text, lifted out of its D3-`[OPEN]`,
D5-`[OPEN]` and D7 rows so the table above stays a table; the
isolation-level crossing and the `crosscore.md` §5 correction are CLA's,
derived from rows the packet ratified. None is optional and none is a
default CLA may pick:

- **R6-3 (RP1) — the coordinator's isolation level has to cross.** R6-2's
  note below, "What D2 does and does not give D3", records the gap:
  `EnrolFor` opens the participant's transaction at the *participant's* own
  `default_isolation()`. Ratification changes that gap's weight — the level
  now **selects the branch**, watermark or none, so a participant that
  mistakes it carries no watermark for a transaction that was promised one.
- **R6-5 (RP3) — three obligations ride on D5's `[OPEN]`.** The ceiling is a
  **named constant reached through one function**, and config-swept. The
  refusal at the ceiling is **retryable and named, and is not
  `UnknownOutcome`** — that code stays D5's answer to a coordinator record
  that is gone, and conflating the two would tell a *blocked writer* to read
  the data when its own statement plainly did nothing and may be retried.
  **Retryable has one spelling in this engine**: `IsRetryable` admits
  `kTxnConflict` alone (`include/kds/base/status.hpp:129`, and R6-2's note
  below calls it the code the wire's retryable bit follows), so RP3 either
  names the refusal by *message* under that code or widens `IsRetryable`
  engine-wide — a wire-contract change, and the choice is RP3's to make
  explicitly rather than by accident. And **R6-5 declares and sizes the
  in-doubt ask's wire form**, the leg the D6 row above says is still owed
  one.
- **R6-5's ceiling value is CLA's to propose and measure**, from the sync
  cost M3 measured and `bench/v2.1.0` §3a's four-stream overlap curve, then
  swept. It is ratified as an obligation, **not as a number**, and must not
  be invented as one.
- **R6-9 (docs) — two sentences the ratifications create.** One in
  `client-manual.md` distinguishing RR's consistent-per-core snapshot from
  RC's absent cross-core promise, since RC is the default and a reader of
  D3's RR wording would otherwise assume it covers RC. One correcting
  `docs/spec/crosscore.md` §5, whose *"the trx-id domain is global, so ids
  compare cleanly"* (lines 205-206) is a claim about **one core's own
  visibility test** — `ReadView::Visible`'s `trx_id >= up_to_trx_id` — and
  must not be read as ordering a cross-owner transaction's several ids
  against one another: that is the global instant D3's consistent-per-core
  weakening refuses, and guideline 3 (`workplan-crosscore.md` §3, which the
  parent order cites as `wal.md`'s) is the rule under it.
- **B1 (RP8) — report p50 and p99, never a single ratio.** M3 found
  shipping's cost in the tail (+11% p50 against +76% p99), so a 2× median
  alone would record D7 as confirmed while an unpredicted tail passed
  unnoticed.

### Ratified 2026-08-27 — the two `[OPEN]`s, in the operator's own words

These arrived in session a day before the packet, each restating its
question and naming its proposal.

| item | the operator's words | recorded as |
|---|---|---|
| **D3's `[OPEN]`** — the watermark under READ COMMITTED | *"READ COMMITTED cross-owner 트랜잭션이 watermark를 건너뛰는가. 제안은 「그렇다」."* | **Ratified as proposed.** A READ COMMITTED cross-owner transaction **skips the watermark entirely**; watermarks are carried for REPEATABLE READ only |
| **D5's `[OPEN]`** — an in-doubt participant against a writer of the same rows | *"in-doubt participant가 같은 행 writer를 막는가, retryable하게 거절하는가. 제안은 「막되 상한을 두고 named refusal로 끝낸다」."* | **Ratified as proposed.** The participant **blocks**, with a bounded ceiling ending in a **named refusal** — not a retryable refusal up front |

Reading a message that states its question and its proposal without a
verdict word as a ratification was CLA's inference, and RP0 recorded it as
the one thing here that could be wrong. **The 2026-08-28 packet settles it**
by restating both rows unchanged, so the inference is confirmed by a later
independent statement rather than merely left uncontradicted.

### Finding — the parent citation, inverted in both directions

`instructions/v2.5.0/cross-owner-protocol.md` opens — line 5, in its
preamble above §0 — *"The second half of `instructions/v2.5.0/2pc.md`."* No
such file exists on any branch; the parent order, which owns R6 whole and
carries D1–D7 in its §3, is `instructions/v2.4.0/2pc.md`. The ratification
packet inverts the same confusion, citing the order itself as
`instructions/v2.4.0/cross-owner-protocol.md` when it is at
`instructions/v2.5.0/`. Both are recorded rather than corrected — operator
input is not edited here — and tracked as cws issue
`cross-owner-protocol-parent-cite` (id 10). Every citation of the parent in
this workplan means the v2.4.0 path.

## Status

| # | Task | State |
|---|---|---|
| R6-0 | The retry bit (§2) | **Built 2026-08-27**, `40e9220` |
| R6-1 | Wire and sizing (D6) | **Built 2026-08-27**, this worktree |
| R6-2 | Participant transaction context (D2) | **Built 2026-08-27**, this worktree |
| R6-3 | Prepare and decide (D4) | **Built 2026-08-28**, `63a0f43` |
| R6-4 | Recovery (D4) | **Built 2026-08-28**, this worktree |
| R6-5 | In-doubt handling (D5) | **Built 2026-08-28**, `056cf9b`; review at `ad0aa2f` |
| R6-6 | PW3b extension | **Built 2026-08-28** (tests and prose; no engine change) |
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
  that fourth arm**: the *resolution ask* meeting a coordinator record that is
  gone, never D5's ceiling refusal, which §2's obligations keep off
  `UnknownOutcome` entirely. Not done at R6-0: the messages reach clients verbatim
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
(That `[OPEN]` was ratified as proposed on 2026-08-27, and D3 itself as
written on 2026-08-28 — "§2 — the ratification record" above.)

**And a gap the same reading exposes**: the coordinator's isolation level
never crosses. `EnrolFor` opens with the *participant's* `default_isolation()`
— its own server config — so a client that writes `BEGIN ISOLATION LEVEL
REPEATABLE READ` gets an RC participant transaction and does not learn it.
With D3 ratified, the level has to travel or the answer above is about a
level nobody selected — §2's R6-3 obligation. `reserved0[3]` still has room.

### An unrelated soundness note R6 invalidates

`TransactionManager::IsInFlight`'s header says it is per-core and that a
transaction on another core answers false — *"Sound for its one user only
while CC3 refuses cross-core writes."* **R6 is what makes CC3 stop
refusing.** Nothing in R6-2 depends on it (a participant's transaction is
local and this core's own manager knows it), but the note's stated premise
expires somewhere in this series, and R6-8 is where the refusal actually
lifts. Recorded so it is found then rather than assumed still true.

## R6-3 — prepare and decide

Built on `v2.5.0-cross-core-owner-protocol-1` at `63a0f43`, the first row
behind RP0's gate. D4's two phases, and the row where "prepared" stops being
a word and becomes a durability claim.

### The record: TXN_PREPARE, type 27

`wal::RecordType::kTxnPrepare`, appended to the enum with
`kMaxAssignedRecordType` moved to match — the pinning rule `record.hpp`
states, since a hand-typed maximum is what once left type 23 unwritable. The
payload (`TxnPreparePayload`, 20 bytes) carries the coordinator's **core,
session id and transaction id**; the envelope carries the **participant's
own** trx id and `kInvalidPageId`.

Three facts about that split, each load-bearing:

- **The envelope's id is local, and that is D2 enforced from the writing
  side.** `CoreRuntime::Open` refuses a mount whose stream names a trx id
  above the superblock's ceiling, so a foreign id in this stream is a stream
  that will not mount. The coordinator's id is *recorded*, never *used as*
  this stream's id.
- **The coordinator's core is in the payload**, unlike the ring message's,
  where it is `MessageHeader::src_core`. A record has no header to read it
  from, and a session id is minted per core — two cores mint the same one, so
  a record keyed on the id alone would resolve one coordinator's transaction
  against another's stream.
- **A payload naming coordinator transaction 0 is `Corruption`.** 0 is the id
  no transaction has, so recovery could never find a decision for it and the
  transaction would stay in doubt for ever. Fail-closed, the reading
  `DecodePageInit` gives an unknown page type.

Redo classifies it with the transaction boundaries (`TouchesNoPage`): it
names no page and changes none. **Analysis is its consumer and that consumer
is R6-4's**, which is the series' one genuinely open hole — see "What R6-3
leaves for R6-4" below.

### `WalManager::RequestDurable`, and why a new primitive

A prepare is not a commit, and the drain only syncs for a staged group commit
or for D3's loss-window interval. Without a way to say *someone is parked on
this record*, a prepare would wait out `relaxed_flush_interval_ns` (10 ms by
default) with a coordinator parked on it — a bounded wait, and still the
wrong answer for a record whose whole content is "this is on the platter".

`RequestDurable(lsn)` is that primitive: it does no I/O, never blocks, and
makes the next `DrainOnce()` sync. It is **not** folded into
`pending_group_commits_`, deliberately: that counter is a count of D2 commits
and `stats_.group_batches` reads it as one, so bumping it for a record that
commits nothing would make the batch statistics say something untrue about
the workload. The flag beside the LSN is not redundant either — `IsDurable`
is strict (`durability.hpp`), so "not yet durable" includes
`lsn == durable_lsn()`, and with no flag the initial `(0, 0)` state would
read as a standing request and sync every idle tick.

`EnsureDurable`'s blocking sync stays what a caller with no reactor uses.

### The participant: what "prepared" now forbids

`ShippedStatementExecutor` gained the two seams (`PrepareSeam`, `DecideSeam`)
because it owns the enrolment `(coordinator core, session id)` keys on;
`Txn2pcServer` is the transport and looks nothing up — `StatementShipServer`'s
split, one level over.

**The promise is made after the sync, never at the append.** `Prepare` logs,
calls `RequestDurable`, and parks in `AwaitPrepared`; only when
`IsDurable(lsn)` answers true does `prepared` go true and the reply go out. A
reply sent at the append would promise a durability the device has not given,
which is the one thing prepare means.

From that flag, three paths change and each would otherwise be a durable
disagreement:

- **`ExpireEnrolled` passes over a prepared context.** The idle ceiling
  (`kShippedTxnIdleCeilingNs`) stops applying — D4 forbids the unilateral
  abort, and the ceiling that does apply is D5's, which is R6-5's.
- **`RollbackAllEnrolled` leaves it standing** and counts
  `left_in_doubt_at_stop()`. A rollback at shutdown appends TXN_ABORT for a
  transaction the coordinator may already have committed in its own stream —
  the exact durable contradiction two-phase commit exists to prevent. The
  horizon it pins dies with the process, so leaving it costs nothing.
- **`EnrolFor` refuses a statement on a prepared context**, retryably. Prepare
  is a promise about what is *already* durable, so a statement admitted after
  it would write rows the PREPARE record does not cover and a commit decided
  on that promise would make the transaction durable in part.

**R6-2's named gap is answered here.** A poisoned session (`kFailedTxn`) was
still "in a transaction", so every later statement met the failed-txn gate and
answered "current transaction is aborted", which told the coordinator nothing.
Prepare is where it becomes legible: the participant refuses, rolls its own
half back, and the coordinator's answer is ABORT.

Every prepare refusal is **retryable** (`TxnConflict`), and that is a
statement about the protocol rather than a choice of code: a refused prepare
aborts the whole transaction, so nothing committed anywhere and a retry is
safe. The decide leg's refusals are `InvalidArgument` instead — a decide for a
transaction this core never prepared is a protocol anomaly, not a load
condition, and it is logged at Error because the commit arm of it is this
protocol's worst reachable outcome (the decision is durable and one
participant's half is missing).

The decided end runs through the **ordinary** `COMMIT`/`ROLLBACK`, on
`DispatchAsync`, so it joins this core's group commit rather than taking an
`fdatasync` on its reactor — the trap `EndEnrolled`'s header already named.

### The coordinator: one field, then the path it always took

`HandleCommit`'s first act is `session.has_participants()`. Empty — every
single-core instance, every fixture, and every transaction that touched one
owner — and it calls `CommitLocal`, which is the body `HandleCommit` has
always had, lifted out unchanged. **That is D1's fast path as a wiring
property**: no participant is enrolled unless a statement shipped inside a
transaction, so a dispatcher never told about a 2PC client cannot have one.
`Txn2pcClient::prepare_messages()` staying 0 is how the work order's §5 asks
for it to be asserted, and the test asserts exactly that.

With participants, the sequence is in `DispatchAsync` because every step of it
is a park:

1. `PrepareAcrossOwners` opens the phase and sends. **Every refusal it can
   produce happens before the first prepare leaves** — no reactor, no shipping
   identity, a bad participant list — so a client that sees one knows nothing
   was asked and its transaction is still open.
2. Park on the prepare phase. It settles when every participant has answered
   or the deadline passes, and **a prepare timeout is an ABORT**: no decision
   was written, so nothing committed anywhere.
3. All prepared → `CommitLocal`, whose COMMIT record **is** the decision (D4).
   Its own failure flips the branch to abort, since `CommitLocal` has already
   rolled back by the time it answers ERR.
4. **The decision is made durable before anyone is told**, whatever the
   durability class — `relaxed`'s window is a promise about this stream's own
   recent commits, not about a record another core is about to act on. This is
   the ordinary commit wait taken early, not an extra one.
5. Send the decision, park on the acknowledgements, answer the client. An
   unacknowledged participant is a **log line, not an outcome**: the decision
   is durable and the client's transaction is settled either way.

A refused prepare reaches the client in the **participant's own code and
words**. Wrapping every one in `TxnConflict` would invite a retry loop on a
refusal that will recur; the participant is the only side that knows which
kind its refusal is.

### Named costs and debts this row creates

- **A silent participant costs the client two deadlines, not one**
  (`2 × kTxnPhaseDeadlineNs` = 20 s). The prepare phase times out and decides
  ABORT; the decide phase then tells the same silent core and waits out its
  own ceiling for an acknowledgement that changes no outcome. Bounded and
  never silent (HP3), and left as it is because **R6-5 owns the ceiling this
  would be shortened by** — the alternative, waiting only on participants that
  answered the prepare, splits "who is told" from "who is waited on" and is a
  decision that belongs with D5's constant. `phase_timeouts()` reads 2 for one
  such transaction, which is how it is visible from outside. **The review
  found a second consequence the first draft did not price**: a statement's
  connection cannot be torn down while it is in flight
  (`TcpServer::CloseClient` defers behind `in_flight`), so a dropped client
  whose participant is silent holds its connection for those 20 s too.
  Exposure is zero until R6-8 opens the path, and RP3 lands before RP6, so
  the owner of the ceiling reaches it first.
- **`kTxnPhaseDeadlineNs` is not D5's ceiling.** It is the coordinator's
  per-phase presumed-lost point, sized by `kShippedStatementDeadlineNs`'s
  argument. D5's in-doubt ceiling — a named constant reached through one
  function, config-swept, ending in a named retryable refusal that is not
  `UnknownOutcome` — is R6-5's to propose and measure, per §2's obligations.
- **The ack wait is on the client's commit path**, which matches D7's cost
  model ("two syncs deep, plus two ring round trips"). B1 measures it.
- **Nothing enrols a participant on a live path yet.** `MayShip` still refuses
  inside an explicit transaction and `CheckWriteAffinity` still refuses the
  cross-owner write, so `Session::EnrolParticipant` has no caller outside the
  tests — R6-8's row, which is also where `in_txn` starts being set on a
  shipped statement. R6-0's retry bit landed the same way and for the same
  reason.
- **The three `UnknownOutcome` arms in `Execute` are still three.** R6-0
  deferred the collapse to "where the fourth caller appears", and R6-3 adds no
  fourth arm — the decide leg's refusals are deliberately not `UnknownOutcome`.
  R6-5's resolution ask is the fourth, and the collapse belongs there.
- **A participant whose decided COMMIT fails locally has no repair path.**
  It is counted (`decide_refusals()`), logged at Error, and reported to the
  coordinator; `HandleCommit`'s failure arm has already rolled its half back,
  so nothing at runtime can put it right. R6-4 is what makes such a
  transaction resolvable at all.

### What R6-3 leaves for R6-4, stated rather than implied

**A prepared-but-undecided transaction is still rolled back at the next
mount.** Analysis reads a TXN_PREPARE the way it reads every record whose
envelope names a transaction — as evidence the transaction existed, hence a
loser — and undo unwinds it. So a participant that crashes while prepared
loses its half even if the coordinator committed. That is R6-4's subject and
the reason the work order gates the whole series behind RP7 rather than
shipping rows one at a time; it is recorded here because a durable prepare
that recovery does not honour is exactly the kind of half-built state that
reads as finished from the outside.

### The `critics-developer` pass, and what it moved

Run against `63a0f43`; the fixes landed on top of it. One real bug, one
reachable protocol hole, three cuts taken and two declined — listed because
a review whose findings are all silently accepted was not read.

**Fixed in the review itself — the coordinator broadcast ABORT while
leaving its own half open.** The first draft's comment claimed
*"`CommitLocal` has already rolled it back"*, which is true of its second
failure arm (a failed `Commit`, which aborts and finishes) and **false of
its first**: `enforcer_.CommitTxn` returns with the transaction deliberately
still open, so a *local* caller may retry `COMMIT`. A cross-owner one may
not — the ABORT that follows reaches every participant, so the transaction
would be aborted everywhere except on the core that decided it, and the
retry that open state invites could no longer succeed. The coordinator's
half is now rolled back to match the decision it is about to broadcast, with
the commit's own error text kept verbatim as the client's answer (no wire
change; HP4 intact).

**Applied after the review — a decide that meets a prepare still reaching
the device.** The first cut refused it, and the review found the reachable
cause: not a race at the ceiling, but **another participant refusing
instantly**, which lets the coordinator decide ABORT while this core is
still syncing. A refused decide there strands a participant that goes on to
become prepared with no decision ever coming — the sweep skips it, shutdown
skips it, and this row has no resolution ask. The decision is now **held on
the context and applied when the prepare wakes** (`decision_pending`,
`ApplyHeldDecision`, `StartDecision`), which is exact rather than
timing-dependent: the alternative the review offered — a participant
durability ceiling strictly under the coordinator's — closes only the
deadline race and not this one.

**Applied — the decide leg's retry bit is read.** R6-1 put it there to
separate a benign resend from a decide for a transaction this core never
prepared, and leaving it unread made the first of those arrive as the
second: a resent commit whose ack was lost was answered `InvalidArgument`
and counted in `decide_refusals()`, the counter whose own header calls it
"the one anomaly on this leg that is not a lost message". A marked resend
meeting no context is now acknowledged. **The prepare leg's bit stays
unread, and that is not an omission**: a prepared context is never swept, so
a resent prepare meeting no context means the same thing a first one does,
and the refusal is already right.

**Recorded, not fixed:**

- **The decision-durability park has no ceiling** (`DispatchAsync`, between
  the durable COMMIT and telling the participants). HP3's literal falsifier
  — except that the group-commit park two stages below it is equally
  unbounded and has been since D2, so this is an **inherited class rather
  than a new one**, and picking a ceiling for one commit wait and not the
  other would be arbitrary. **RP7 must report it as inherited-or-fixed**
  rather than let HP3 read as confirmed on a wait that was never bounded.
- **`FinishDecision`'s failure path leaves a prepared context standing**,
  and the sweep passes over it — so a decided end that refuses costs one
  live transaction, one enrolment slot and the read horizon it pins, for the
  life of the process. On the commit arm that is genuine doubt and R6-5's;
  on the abort arm it is residue, and retrying it is **not** the unilateral
  abort D4 forbids, so R6-5 can. The comment that claimed "the sweep will
  try again" is corrected — it was true only of an unprepared context.

**Cuts taken:** the no-seam refusal was a verbatim duplicated string
literal and is now one function; `OnPrepare`/`OnDecide`'s size-check-and-copy
preamble is one template; `Enrolled::prepare_lsn` and
`TxnPhaseOutcome::sent_ns` were write-only and are gone (R6-5 can re-add the
LSN with a reader); the fast-path test's stray `BEGIN` on the dispatcher's
autocommit session — in the one test whose subject is that the fast path
does nothing extra — is gone.

**Cuts declined, with reasons:**

- **A shared ring-waiter across the four services** (`StatementShipClient`,
  `IndexBuildClient`, `AssertionBuildClient`, `Txn2pcClient` are one
  `map<request_id, Outcome>` + `Settled`/`Find`/`Close` + a deadline + an
  identity check, ~180 lines of near-copy). The review is right, and it is
  **not this row's**: R6-3 should not be where three settled wire contracts
  move. It should be its own row, because the fifth copy is already
  predictable — R6-5's in-doubt ask.
- **Collapsing `Prepare`/`Decide`'s send loops into one template.** Five
  lines each, differing in payload type and counter; the template would be
  about as long as what it removes.
- **`Txn2pcServer::prepares()/decides()/replies()` as dead weight.** Kept
  and now asserted in the coordinator fixture, which is what
  `StatementShipServer::requests()` does — a counter with a reader is
  observability, and the sibling service sets that precedent.

### Tests

`tests/txn_2pc_protocol_test.cpp`, three fixtures for three failure shapes:
the participant driven through its seams over a real WAL (the record's
contents, the sweep and shutdown exclusions, the statement refusal, both
decide arms, every refusal path), the coordinator over a real three-core ring
(N participants, a refusal, a deadline, the leg check, the shapes refused
before a send), and the dispatcher's `COMMIT` end to end against a stub
participant (the fast path's zero prepare messages, commit, a refused prepare,
a silent participant, and the no-reactor refusal). `tests/wal_payload_test.cpp`
gained TXN_PREPARE's round trip and its zero-id corruption.

## R6-4 — recovery, and CP1

Built on `r6-4-prepare-recovery`. The row that makes R6-3's durable state
mean something: a transaction this core prepared is now resolved at mount
instead of rolled back as a loser.

### The fourth outcome

`TxnOutcome::kPrepared`. Analysis had three verdicts and a `TXN_PREPARE`
fits none of them - a prepared transaction is not a winner, not an aborted
transaction whose compensations are already in the log, and emphatically not
a loser. Adding it is what stops undo from rolling back a transaction the
coordinator may have committed and acknowledged to a client, which is the
one durable disagreement two-phase commit exists to prevent.

Three details the scan needs, each a way to get it wrong:

- **A terminal record outranks a prepare.** The scan is forward, so a
  participant that heard its decide leg writes `TXN_COMMIT` or `TXN_ABORT`
  after its prepare and needs no resolution at all - `note_txn` is amended
  so `kPrepared` is the one outcome that may never overwrite an existing
  one.
- **`prepared_txns` is pruned to the `kPrepared` set at the end of the
  scan**, rather than erased at each terminal record. That keeps the scan a
  single forward pass with no look-behind, and it makes the map exactly the
  set `RecoverCore` must resolve.
- **A prepare naming no transaction is `Corruption`.** The envelope's id is
  the participant's own (D2) and is what everything downstream keys on.

### The resolution, and the two reads it is made of

`wal::PreparedResolver` is a phase injected the way `UndoPhase` is, and for
one more reason: the ask reaches **another core's log**, and the layout of a
log directory is `server/`'s to know. `server::CoordinatorStreamResolver`
opens `wal-<coordinator core>-*.log` and scans it for the coordinator's
transaction id.

**It is not a cross-stream comparison, and that is the row's whole
soundness argument.** Guideline 3 forbids ordering two streams' records
against each other; nothing here orders anything. The participant's own
records say what to redo; the coordinator's say whether it committed, found
by matching an id. Two independent reads. The prepare's LSN is carried only
so a refusal can name it, and is never compared with anything in the other
stream. **HP5 holds, with the site**: `prepared_resolver.cpp`'s scan reads
`record.header.txn_id` and `record.type()` and touches no LSN at all.

**Where it runs**: immediately after analysis, before the loser refusal and
before redo. It has to be before the refusal because it decides which set
each prepared transaction joins, and it is read-only, so a mount that
refuses here has written nothing.

**Why an absent decision is an abort rather than a guess.** The scan starts
at LSN 0 and reads the coordinator's whole stream, so "no COMMIT and no
ABORT for this id" is a fact about every byte that core made durable -
nothing recycles a segment in this engine. And the two sides reach that
verdict independently: a coordinator with no COMMIT record for its own
transaction rolls it back at its own mount, because analysis calls a
transaction with no terminal record a loser. Participant and coordinator
abort the same transaction for the same reason, from their own streams, with
no message between them.

**What refuses the mount**, because a wrong answer here is worse than no
answer:

- a prepared transaction with **no resolver installed** - this core may
  neither roll it back nor publish it, so it does not open (the refusal
  `RecoverCore` already makes for losers with no undo phase, one protocol
  up);
- an **absent coordinator stream**. Every core publishes a completion
  checkpoint at every mount (RC08), so a core of this database with no
  segment files is a log directory that lost a file, not a core that never
  wrote - and answering "abort" could discard a transaction its coordinator
  committed and told a client about;
- a prepare naming **this core as its own coordinator**: a coordinator never
  writes `TXN_PREPARE`.

### CP1 — recovery across a core-count change

The work order names R6-4 as the place `wal.md` §3's second `[OPEN]` is
answered, and asks for the three shapes enumerated with what mount does for
each. **The answer is that the question is already closed by a refusal this
engine has had since M6, and R6 does not reopen it.**

`SuperBlockFields::core_count` pins the count at bootstrap, and
`bootstrap.cpp` refuses any mount whose configured `cores` differs, naming
both numbers and telling the operator to restart at the pinned count. So:

| shape | what mount does |
|---|---|
| **Fewer cores than the database was created with** | Refused at the door, before any stream is opened. Recovery never runs, so a prepare naming a core outside the new range is never read |
| **More cores** | The same refusal, in the same place, for the same reason |
| **Same count, re-indexed** | **Not detectable, and not made worse by R6.** Streams are named by index and carry no other identity, so a database whose streams were swapped mounts and misreads every one of them - its own heap records included, long before any prepare is reached. What would make it detectable is a per-stream identity (a uuid in the segment header, or a superblock slot), which is a format change and belongs to whoever lifts the pin |

Two things worth stating beyond the table, because they are what the
`[OPEN]` was actually worried about:

- **The resolution reads a file, not a running core.** The `[OPEN]`'s
  scenario - "a PREPARE record sits in a stream whose core no longer exists"
  - is not a problem for this mechanism even if the pin is one day lifted:
  `wal-<core>-*.log` is resolvable whether or not a reactor is currently
  serving it, and the mount refuses by name when the file is absent rather
  than guessing.
- **A hang is not reachable here** (HP3). The resolution is a bounded scan
  of a finite file; every failure is a refusal with a message, and there is
  no wait of any kind in the phase.

The `[OPEN]` itself is R6-9's to close in `wal.md`, with this as its answer.

#### CP1 amended 2026-08-28 — the operator narrows the `[OPEN]` rather than accepting the refusal as the end state

The direction arrived after R6-4 landed and changes what CP1 may claim.
**The table above stays true of the engine as built** — a differing `cores`
is refused at the door and recovery never meets a prepare from a stream it
cannot place — but the sentence *"the question is already closed by a
refusal this engine has had since M6"* was CLA reading a scope gap as a
settled decision. It is not: the refusal is the current behaviour, not the
end state.

The operator's direction, recorded in the terms it arrived in:

- **The count may change in both directions**, increase and decrease.
- **The reorganisation is a mount-time operation**, inside the window RV1
  already establishes — after the superblock is read, before the listener
  binds. **Online change is not supported and is not a goal.**
- That is **a scope decision, not an architectural exclusion**, and it
  forecloses nothing an online path would need: revocation and quiesce would
  layer onto the same reassignment logic rather than replace it. At mount
  there is no fault grant to revoke, because the store is built fresh —
  which is what `device_page_store.hpp`'s "nothing revokes a fault grant"
  would otherwise make expensive.

Three constraints ride with it and are **not negotiable**:

1. **Prepared transactions resolve before anything is reorganised.**
   Reassigning or discarding a coordinator's stream destroys the evidence
   R6-4 resolves a prepare against, so the resolver runs first and **an
   unresolved prepare refuses the mount**. R6-4 already refuses a mount it
   cannot resolve (no resolver, absent coordinator stream, a self-named
   coordinator); what this adds is an ordering requirement on a phase that
   does not exist yet, and it is the reason the constraint is written down
   *now* rather than when that phase is built.
2. **`core_count` is written last**, so a crash mid-reorganisation reads as
   the old count and the work reruns. **Reassignment must be idempotent** —
   a requirement on the mechanism, not a property to hope for.
3. **Modulo is not required.** Placement policy is open and belongs with
   R5's mover; correctness requires only that relations whose owner core no
   longer exists are moved.

**What this leaves R6-9.** `wal.md` §3's `[OPEN]` is *narrowed*, not closed:
**when** is settled, **how** is not. The three sites that carried it —
`wal.md` §3, `superblock.hpp`'s pin, `blueprint-range-ownership.md` §12 —
each carry the narrowing as of `056cf9b`, so R6-9's remaining work there is
to cite this section rather than to decide anything.

**One consequence for this milestone, stated because it is easy to miss.**
Constraint 1 makes R6-5's `txn_in_doubt_unresolved` an operational number
rather than a diagnostic one: a transaction whose coordinator answered
`UnknownOutcome` stays prepared until a mount resolves it, and under the
direction that same prepare would **refuse** a mount that was asked to
change the core count. The two features meet at exactly one field.

### What the row deliberately does not do

- **It writes no terminal record for a resolved commit.** A resolved *abort*
  gets one for free - undo writes `TXN_ABORT` for every loser it rolls back,
  so the next mount reads that transaction as `kAborted` and asks nobody. A
  resolved commit has no such writer, so it is re-resolved at each mount
  until a completion checkpoint moves the scan start past its prepare -
  which RC08 publishes at the end of every mount, so in practice once. The
  repeat costs one scan of the coordinator's stream and is idempotent; a
  writer for it would be a new recovery-time append path, and this row does
  not need one.
  **Read this only about a transaction that is already resolved.** For one
  that is still *in doubt*, the same movement of the scan start would have
  discarded the only record saying "do not decide this" - the review's bug 1
  below, and why the checkpoint is now floored at the oldest live prepare.
- **It caches nothing.** Two prepared transactions on one coordinator open
  that stream twice. The population is bounded by `kShippedMaxEnrolled` per
  core and a mount is not a hot path; a cache here would be invalidated by
  nothing at all.

### The `critics-developer` pass, and what it moved

Run against `e06c117`; the fixes landed on top of it. **Two correctness
bugs, both of which let a prepared transaction be decided without its
coordinator** - the row's one prohibition - plus three cuts taken and three
declined.

**Bug 1, reproduced by the review: a checkpoint re-seeded a live prepared
transaction as a loser.** A prepared participant is still `active_`, so
`TransactionManager::Snapshot()` lists it in every `CHECKPOINT_BEGIN`'s
active table - as an ordinary `{id, undo head}` pair, which cannot say it is
prepared. Once its pages had been written back, its recLSNs left the dirty
table and `RedoStartFrom` published a redo start **above** the
`TXN_PREPARE`. The next mount then scanned from the checkpoint, never saw
the prepare, read the active-list entry as a loser, and rolled the
transaction back - durably, with `TXN_ABORT`, and unrecoverably, since the
mount after that reads `kAborted` and asks nobody.

Fixed by the floor: `ActiveTransactions::OldestPreparedLsn()` (defaulted to
0, so every existing implementation is unchanged), `txn::Transaction`
carries the prepare LSN, and `Checkpointer::Start` lowers its redo start to
it. **This is an amendment to `wal.md` §11-3** - the redo start is now the
checkpoint's LSN lowered by every nonzero recLSN *and by the oldest live
prepare* - which R6-9 owes the spec and which the operator should see named:
the price is that an in-doubt transaction pins the log's redo start until it
is decided, which is the standard ARIES price and what D5's bounded wait
bounds. Zero on every stream with nothing prepared, so no existing
checkpoint's number moves by a byte.

**The field this restores was dropped one row ago, correctly.** R6-3's
review called `Enrolled::prepare_lsn` write-only and I deleted it. This is
its reader, and it lives on `txn::Transaction` now rather than on the
enrolment - which is the right home, because the checkpointer asks the
transaction manager, not the shipping executor.

**Bug 2: the resolver dropped the anchor-honesty check `Analyze` applies to
its own stream.** A coordinator stream whose last segment is gone reads as a
complete stream with no decision in it - `FileLogDevice::Open` catches a
numbering *gap* but not a missing tail - and a torn tail reads the same way.
Both produced "abort", which durably contradicts a coordinator that
committed and told a client so. The resolver now takes every core's anchor
and refuses when the scan ends before the durable point that core's anchor
was published with, which is exactly `analysis.hpp`'s argument applied to
somebody else's stream. The plumbing is `SuperBlock::wal_anchors()` →
`CoreRuntime::Config::anchors` → `RecoverCoreAtMount`.

That check also gives the "absent decision is an abort" rule the
completeness it claimed: the header said *"a fact about every byte that core
ever made durable"*, and without the anchor it was a fact about every byte
still present. The two are the same only once something says how many there
should be.

**Bug 3, taken though the review left it optional:** two terminal records
disagreeing about one id now refuse instead of resolving last-one-wins. It
is impossible by construction (no id reuse), and this file's whole posture
is that a wrong answer is worse than no answer.

**Cuts taken.** The resolution is now **one scan per coordinator, not per
transaction** (`ResolveAll` groups first) - up to `kShippedMaxEnrolled` full
scans of a possibly-huge stream collapsed to one, at a mount that is already
the worst this engine has, and `streams_read()` now means what its name
says. `MountRecovery`'s three counters were write-only and are now printed
by `SHOW META` when a mount actually resolved something, absent rather than
zeroed. The `default:` arm over `TxnOutcome` became explicit cases so a
fifth outcome is a compile warning rather than a runtime refusal; the
`kNoTxnId` check moved ahead of the decode it used to follow.

**Declined, with reasons.**

- **A `Warn` on every torn-tail-and-no-decision resolution** was the
  review's minimum; it is there, but the case it describes is no longer the
  weakest evidence this function acts on - the anchor check is what makes it
  sound, so the line records the shape rather than apologising for it.
- **`records_scanned()` deleted as unread.** Kept and asserted instead: it
  is the number that makes the one-scan-per-coordinator claim checkable
  from outside, which is the whole point of the batching cut above it.
- **Carrying preparedness in `CHECKPOINT_BEGIN`'s active table** (the
  review's alternative fix for bug 1). It is self-describing and the entry
  has grown before, but it is a payload-format change with a compatibility
  rule attached, against a floor that is fifteen lines and changes no
  existing number. If the log-pinning cost ever bites, that is the trade to
  revisit and the reason to revisit it will be measurable.

### Two notes the review left for later rows

- **The sim harness cannot resolve a cross-owner prepare.** `sim/instance.cpp`
  passes no `wal_dir`, and the sim's devices are in-memory with no log
  *directory* at all - so a simulated mount that met a prepared transaction
  would refuse. Harmless today (single core, nothing prepares), and RP7 runs
  `scripts/sim.sh`, so it is stated here rather than discovered there.
- **The resolver opens another core's log device**, which `file_log_device.hpp`
  calls core-local. Sound because every mount runs on the startup thread
  before any reactor worker exists, and that dependency is now a stated
  precondition in `prepared_resolver.hpp` rather than a coincidence.

### Tests

`tests/prepared_recovery_test.cpp`, three fixtures for the three ways to get
this wrong: analysis calling a prepare a loser (the fourth outcome, its
identity, and both ways a stream decides its own prepare), the resolver
guessing (committed, aborted, no decision, absent stream, self-named
coordinator), and the mount publishing or rolling back on its own authority
(no resolver refuses; a committed verdict leaves undo untouched; an aborted
verdict hands the transaction to undo; a refused resolution refuses the
mount). The coordinator's stream is written independently of the
participant's in every case, so neither knows the other's LSNs - which is
the arrangement that makes "no cross-stream comparison" visible rather than
merely claimed.

## R6-5 — in-doubt, and D5's two bounded waits

Built on `v2.5.0-cross-core-owner-protocol-2` at `056cf9b`. The row that
turns "in doubt" from a state the participant sits in into one it acts on,
and the row that answers D5's `[OPEN]` in the operator's ratified direction.

### The third exchange, and the first live sender of R6-0's bit

`kTxnResolveRequest`/`kTxnResolveReply` (37, 38), 24 bytes each — the
smallest messages in the protocol, and the sizing the D6 row of §2 above
said this leg was still owed. A participant that replied prepared and has
heard no decide for `kTxnInDoubtCeilingNs` asks its coordinator what was
decided; the answer is a code and a decision byte, no words, because a
participant is not a client and nothing it is told here is rendered for
anyone.

**The ask carries R6-0's retry bit set, always, and that is not a
formality.** It is what makes answering from a record instead of by
re-deciding safe: a coordinator that no longer holds the record must answer
`UnknownOutcome` (D5), and the bit is what says the sender knows that. An
ask that arrives with the bit **clear** is refused `InvalidArgument` rather
than served — the contract `known-gaps.md` states ("every retry path built
from R6-3 on has to set it") is a promise about senders, and this is the one
leg where a violation could otherwise be silent. R6-0 has had no live sender
since it landed; it has one now, which is the correction that entry was
carrying.

### The coordinator's memory, and the two ways it can be honest

A `DecisionRecord` per `(session_id, transaction_id)`, opened **at the first
prepare** and written **before the decide messages go out**. Both moments
are load-bearing and neither is where a first draft would have put it:

- **Opened at prepare**, because an ask that lands while the prepare phase
  is still open must be answered *"not yet, ask again"* and not
  *"unknown"* — the second is terminal by D5, so a participant told it stops
  asking about a transaction whose decision is milliseconds away and waits
  for the next mount instead.
- **Written before the sends**, because the decision is already durable in
  this core's stream by then (`DispatchAsync`'s order, D4's reason), so a
  decide message the ring drops is still answerable with the real decision.
  Recording after the loop would leave exactly the window that produces the
  ask.

`Close` drops the record when every participant acknowledged — nobody is
left to ask, and keeping it would hold a map node for the retention on the
**healthy** path, which is every cross-owner transaction. A phase that timed
out keeps it, because the participant that did not acknowledge is precisely
the one that will ask. Past `kTxnDecisionRetentionNs` (10 × the phase
deadline) it is dropped and the answer becomes `UnknownOutcome`, which is
not a correctness bound but a bound on how long a rare failure may keep
asking: the durable record the retention does **not** touch is this core's
stream, which R6-4 reads at the next mount.

### D5's `[OPEN]`, built as ratified: the writer blocks

**The wait is on the statement, not on the row, and that is forced.** A
first-updater-wins conflict is found inside a row callback under a page
span, which is no place to park — so the statement is refused having written
nothing, `DispatchAsync` parks until the doubt clears, and the statement
runs again from the top. What the client sees is one statement that took a
while, which is what "blocks" means to it.

**"Having written nothing" is what makes the re-run a repeat rather than a
second application**, and it is checked rather than assumed: `BeginWrite`
marks the transaction's trail length and `EndWrite` compares — a statement
that wrote rows before it hit the conflict (`SET v = v + 1` over four rows,
conflicting on the fifth) is **not** re-runnable at any price and is
answered with the conflict it produced, unwaited. Only the clean case
reaches the park.

**The poison is withheld while the wait is still possible.** Inside an
explicit transaction a failed statement poisons the session (`txn.md`
§10-8), and a poisoned session cannot run the statement again — a wait whose
end is a forced `ROLLBACK` is not a wait. So `EndWrite` skips the poison for
exactly this refusal, and `DispatchAsync` applies it at the ceiling, where
the statement has genuinely failed.

**One deadline covers every blocker.** The bound is taken before the first
wait and shared by every later one, so a statement blocked, freed, and
blocked again by a *second* in-doubt transaction still ends within one
ceiling — which is what keeps HP3 true of a shape that is otherwise a loop
with a bounded body.

**The refusal at the ceiling**, per §2's three obligations: `TxnConflict`,
because `IsRetryable` admits that code alone and a client's retry loop reads
the bit it sets — so RP3 takes the "name it by message under that code"
branch and does **not** widen `IsRetryable`, which would have been a
wire-contract change; **not** `UnknownOutcome`, which would tell a client to
go and read data its statement never touched; and named, so the message says
a coordinator has not decided rather than only that a row is busy.

### The ceiling, proposed and derived — `kTxnInDoubtCeilingNs` = 200 ms

CLA's to propose and measure (§2). **The derivation is the healthy decision
window on this host**, not a round number: a coordinator's decision sync
(~0.94 ms single-stream, `bench/v2.1.0/results-multicore-writers-v2.1.0.md`
§3's 1,066/s and `results-shipping-pretasks-v2.1.0-10-g82a2749.md` §3a's
1,118/s), plus a sibling participant's prepare sync still in flight (the
same ~0.94 ms; four streams overlap at 3.371× on this volume, §3a, so a
four-wide prepare costs each participant ~1.1 ms rather than 4 × 0.94), plus
two ring hops at ~21 µs (§3a). About **2 ms at the median**. 200 ms is ~100×
that and ~18× the largest unattributed latency this host has (the ~11 ms
periodic stall, `results-knob-sweep-cell2` §5): a writer never meets the
refusal on a healthy path, and one that meets a genuinely lost decision is
refused inside a fifth of a second rather than after the coordinator's ten.

**It sits deliberately *under* `kTxnPhaseDeadlineNs`**, and that is a choice.
A coordinator may legitimately take up to that deadline to decide when
another participant is silent, so an ask inside that window is answered "not
yet" at the cost of one round trip, and a writer refused inside it is
refused for a transaction that is slow rather than lost. Sizing the ceiling
to the coordinator's worst case instead would make every blocked writer pay
a failure's price on a healthy day; the ratified rule bounds the *stall*, and
the stall is the writer's.

**Reached through one function** — `CommandDispatcher::InDoubtCeilingNs()` —
and swept as `in_doubt_ceiling_ms`. **What the knob moves is the writer's
block, and only that**: the participant's *ask cadence* reads
`kTxnInDoubtCeilingNs` at the constant. The two are the same number by
derivation and not the same quantity, and sweeping them together would make
`0` mean "refuse a writer at once" on one and "ask the coordinator every
reactor tick" on the other. §2's obligation is about D5's `[OPEN]`, which is
the writer's block; the cadence is a second quantity and nothing needs a
knob for it yet. Stated because the first cut's comment claimed both went
through the function, and the review found that false. **The measured sweep is not this row's**:
it needs a live cross-owner path, which `MayShip` and `CheckWriteAffinity`
still refuse until R6-8, so the number above is the proposal and its
derivation, and RP8 is where a measurement can replace it. Stated rather
than left implied, because §2 ratified the obligation and not the number.

**`in_doubt_ceiling_ms = 0` is not an off-switch**: it is D5's *other*
branch — refuse retryably up front — reachable by configuration so the two
can be measured against each other. It is also what an unconfigured
dispatcher holds, which is every fixture, so a test that has no cross-owner
transaction to be in doubt about behaves exactly as it did before this row.

### What this row closes that R6-3 left open

**`FinishDecision`'s prepared residue retries itself now**, and by the path
R6-3 predicted rather than by a new one. That review recorded a decided end
that fails leaving a prepared context standing with nothing to retry it —
residue on the abort arm, genuine doubt on the commit arm. Both are now
reached by the in-doubt sweep: the context is still prepared, so it asks on
the next ceiling, the coordinator answers from a record it kept (the
participant's refusal means the phase did not settle `AllPrepared`, so
`Close` kept it), and `Resolve` runs `StartDecision` again. Retrying an
**abort** this way is not the unilateral abort D4 forbids — the decision is
the coordinator's and it is being re-applied, not re-made.

### Named costs and debts this row creates

- **An in-doubt participant asks for ever, by decision.** There is no cap on
  asks: a capped participant would hold locks with nothing left that could
  free it before the next mount, and an ask is two ring messages against a
  transaction that is already blocking writers. What ends the asking is an
  answer — including `UnknownOutcome`, which is terminal and stops it.
- **An `UnknownOutcome` answer leaves a transaction nothing at runtime can
  finish.** It holds its rows, blocks their writers for a ceiling each, and
  pins the log's redo start (R6-4's floor) until the next mount resolves it
  against the coordinator's stream. `txn_in_doubt_unresolved` in `SHOW META`
  is the number that says this has happened, and it is the one field in the
  new block worth alerting on.
- **The block is row-granular in its trigger and statement-granular in its
  wait.** A statement that already wrote rows is refused rather than waited
  on, which is a stricter answer than "blocks writers of the same rows"
  literally promises. Recorded rather than hidden: the alternative is
  re-applying a partially applied statement, and that is a wrong answer
  rather than a slow one.
- **Nothing reaches any of this on a live path yet.** `MayShip` still refuses
  inside an explicit transaction and `CheckWriteAffinity` still refuses the
  cross-owner write, so R6-8 is the row that makes an in-doubt participant
  reachable outside a test — which is also when B-cell measurement of the
  ceiling becomes possible.

### The `critics-developer` pass, and what it moved

Run against `056cf9b`; the fixes landed at `ad0aa2f`. **Two correctness
bugs, both of which the row's own prose already claimed were not there** —
which is the reason to record them at length rather than as a line.

**Bug 1 — a refused write left an explicit transaction committable, and it
was silent.** `EndWrite` withholds the poison whenever a blocker is
recorded, but **only `DispatchAsync` consumes one**. The synchronous
`Dispatch()` handles every other pending field and never looks at
`in_doubt_block`; nor does a `DispatchAsync` on a dispatcher with no clock.
On those paths a client was handed `ERR TXN_CONFLICT` for a statement that
failed inside `BEGIN`, and the session stayed `kInTxn` — so `BEGIN` →
blocked `UPDATE` → `ERR` → **`COMMIT` returned `COMMIT`**. The transaction
committed without the statement. `txn.md` §6's failure atomicity is per
transaction and `HandleCommit`'s `session.failed()` gate is the only thing
enforcing it; this disarmed that gate. Fixed at the *source*: the blocker
is recorded only under `may_park_ && clock_ != nullptr`, the same pair the
wait tests, so one condition keeps `EndWrite`, the export and the wait
consistent — and it makes true the two claims that were false, the
`InDoubtBlock` declaration's *"a fixture sees the pre-R6-5 behaviour"* and
this file's *"the poison is withheld while the wait is still possible"*.

**Bug 2 — `UnknownOutcome` was documented terminal and was not.** The
unknown arm of `Resolve` moved the stamp and nothing else, and the sweep's
only guard is the ceiling — so a participant re-asked every 200 ms for the
life of the process, two ring messages and two Warn lines each, and
`++in_doubt_resolved_unknown_` every time. That last is the concrete wrong
answer: `txn_in_doubt_unresolved` is documented as a count of
*transactions* and became a count of asks, climbing at ~5/s per stuck
transaction. **Four independent statements said terminal** — D5's
ratification, the reply payload's header, the accessor's own doc, and this
file's *"what ends the asking is an answer — including `UnknownOutcome`,
which is terminal and stops it"* — and none of them was code.
`Enrolled::resolve_terminal` is the flag they all assumed.

**Cuts taken.** `Txn2pcServer::asks_`/`resolve_replies_` had **no reader**
anywhere and counted a number `SHOW META` already prints from the executor
(`in_doubt_asks`) — two names for one quantity, which is what R6-3's review
kept `prepares()/decides()/replies()` on the opposite test for. Gone. The
decision map had **two removal policies** — retention by age, cap by map
key, an ordering the comment itself conceded was arbitrary across sessions
— now one ordering (time) and one `OpenDecisionRecord` that `Prepare` and
`Decide` share, which also closes the asymmetry where only `Prepare`
expired. `SHOW META`'s coordinator gate now includes the three counters
that can fire on their own, so a refused ask is not invisible on exactly
the run that produced one.

**Recorded, not fixed — a slow decide that loses the race to its own ask.**
R6-5 creates an arrival order R6-3 could not: the ask resolves the
transaction and releases the context, and the original decide — merely
slow, not lost — arrives behind it. `Decide` finds no context, the
coordinator's decide leg never sets the retry bit, so the benign-resend arm
is unreachable and it lands on the anomaly arm: `decide_refusals_`, an
Error line, and a refusal that keeps the coordinator's record for the full
retention. **The outcome is correct and every report about it was wrong.**
The Error no longer asserts the loss it cannot know — it names both
reachable causes — and `decide_refusals()` says it now covers this benign
case. The exact fix needs either a decided-window on the participant (a
second dedup mechanism on the row that is already the fifth wire leg) or a
bit the coordinator has no way to set at send time, and it is only
reachable when a decide takes longer than 200 ms to cross a ring whose hop
is ~21 µs. **R6-8 owns it**, and RP7 should not read a non-zero
`txn_decide_refusals` as proof of a lost half without checking this.

**HP3, restated honestly by the review.** Every *statement* wait is
bounded: the in-doubt park takes its deadline once and shares it across
re-runs, and the reactor's 10 ms idle cap means the deadline is observed
promptly. Two waits are not bounded and neither is new — the two
commit-durability parks, the class R6-3's review already recorded as
inherited — and the **ask loop is unbounded by decision**, which bug 2's
fix narrows to the unanswered case. **RP7 must report HP3 as "holds for
every statement wait; the ask loop and the two commit parks are unbounded,
one by decision and two as inherited"**, not as confirmed.

**And one prediction of R6-3's that did not come true**, worth correcting
because a later reader would otherwise act on it: that review declined the
shared-ring-waiter cut on the ground that *"the fifth copy is already
predictable — R6-5's in-doubt ask."* It is not. `Ask` opens no waiter and
nothing parks on it; the participant's own sweep is the retry. The
consolidation row is still worth doing for the existing four, but its
stated trigger did not fire.

### Tests

`tests/txn_2pc_protocol_test.cpp` gains three fixtures for D5's three sides:
the coordinator's memory (a decision answered, a still-open phase told to
ask again, an absent record answered `UnknownOutcome` and never guessed, the
retention forgetting one, the record's lifetime across an acknowledged and
an unacknowledged decide phase, and a clear retry bit refused), the
participant's wait (one ask per ceiling and not before, a commit and an
abort applied from the answer, and an `UnknownOutcome` leaving the
transaction in doubt rather than guessing), and the blocked writer (a wait
that ends in the statement running, a ceiling that ends in the named
retryable non-`UnknownOutcome` refusal, `0` as D5's other branch, a
transaction not poisoned while it waits, and an **ordinary** in-flight
writer not waited on at all — the narrowness that keeps a client's think
time out of this). `tests/txn_2pc_wire_test.cpp` gains the ask's sizing and
the ceiling's two ordering assertions.

The review adds two more, one per bug, each failing without its fix:
`ThePathThatCannotWaitPoisonsExactlyAsItAlwaysDid` and
`AnUnknownAnswerEndsTheAskingRatherThanRepeatingItForEver`.

Full suite green at `ad0aa2f`: **2,855 tests, 18 new**. Overhead not
measured — the v2-stage A/B suspension of 2026-08-24 stands.

## R6-6 — the graceful stop, and PW3b's open item re-read

Built on `v2.5.0-cross-core-owner-protocol-2`. **No engine change**: the
mechanism this row is about already exists — R6-3 leaves a prepared context
standing at shutdown, R6-4 floors the checkpoint's redo start at the oldest
live prepare, and analysis carries the fourth outcome. What did not exist is
proof that the three meet correctly, and the meeting point is the one place
the transaction could be lost without anything refusing.

### The joint nobody had tested

`Expeditor::Serve`'s tail runs, per peer: the executor's
`RollbackAllEnrolled` (which passes over a prepared context), then
`CoreRuntime::ShutdownCheckpoint`, which **flushes the core's pages first
and then checkpoints** — PW3b's order, and its own comment gives the
reason: *"an empty dirty table makes the redo start the `CHECKPOINT_BEGIN`
LSN itself."*

That sentence is exactly the hazard. A redo start at the `CHECKPOINT_BEGIN`
LSN is **past the `TXN_PREPARE`**, so the next mount would scan from after
the record that says "this transaction is not this core's to decide", find
the active-list entry the checkpoint carried, read it as an ordinary loser,
and roll back a transaction the coordinator may have committed and
acknowledged to a client. R6-4's floor is what prevents it, and until this
row the floor was tested only against a **scripted** `ActiveTransactions`
answering a number the test chose.

`Txn2pcShutdownTest` closes that: a real statement shipped, a real prepare
through the executor's seam, and then the stop sequence in `Serve`'s own
order over a target whose dirty table is empty *because the flush already
happened*. Three properties, and **the first two fail if the floor is
removed** — verified by removing it and watching them fail, then restoring:

- the published redo start is at or below the LSN
  `TransactionManager::OldestPreparedLsn()` reports, which is the wiring
  claim the unit test could not make;
- the mount that scans from exactly that anchor still finds the prepare —
  `analysis.prepared == 1`, `losers == 0`, and the `PreparedTxn` naming the
  coordinator the resolution will look the decision up in;
- a **second** checkpoint while still in doubt holds the floor too, since it
  is computed per checkpoint from the live transaction rather than
  remembered.

The third fixture pins the other half: with nothing prepared, a shutdown
checkpoint publishes the `CHECKPOINT_BEGIN` LSN exactly as it did before
R6-4 — which is what keeps PW3b's measured restart bound (*"2 records,
redo 0"*) intact on every stream that is not a participant.

### PW3b's open review item, re-read in R6's light

`known-gaps.md`'s C4: one failed checkpoint disarms every later checkpoint
on that core — `in_progress_` clears only on `Complete()`'s success path
(`src/wal/checkpointer.cpp:265`) while `Start()` refuses whenever it is set
(`:100`), and nothing resumes a half-finished checkpoint.

**The re-read's answer: R6 does not make it a correctness bug, and the
direction of the failure is why.** A disarmed checkpointer publishes *no*
anchor, so the redo start stays where the last successful checkpoint left it
— which is **earlier**, so a `TXN_PREPARE` written after it is inside the
replay range by construction. C4 can only make a mount scan more, never
less, and there is no arrangement of it that puts a prepare outside the
range. The failure mode R6-4 guards against needs a checkpoint that
*succeeds* and advances too far; C4 is the opposite.

Two consequences worth stating rather than leaving implied:

- **C4's cost under R6 is smaller than it was, not larger.** While anything
  on the core is in doubt the floor already pins the redo start at the
  prepare, so a checkpoint that would have been disarmed was going to buy
  little. The graceful-restart bound PW3b measured is lost either way, and
  that was already C4's charge.
- **But R6 adds a *second*, by-design way to lose that bound**, and an
  operator should not confuse the two. A transaction whose coordinator
  answered `UnknownOutcome` (R6-5) stays in doubt until the next mount, and
  the floor therefore pins the redo start at its prepare for the whole life
  of the process — so checkpoints stop shortening recovery on that core,
  with nothing failing and nothing to see in a log. `SHOW META`'s
  `txn_in_doubt_unresolved` is what separates this case from C4's; C4 shows
  as a checkpoint Error line and this shows as a counter. The repair for one
  is a `wal.md` §11 behaviour decision, and for the other it is R6-8's live
  path plus a mount.

C4 stays where it is — `known-gaps.md`, unfixed, owned by `wal.md` §11 —
with the re-read recorded there.

### What this row does not prove

**Nothing here drives a real `Expeditor` with `cores > 1`.** PW3b's own S6
finding says so of its own call site — *"`Serve`'s per-peer call site has no
test at all, since nothing builds an `Expeditor` with `cores > 1`"* — and
R6-6 inherits that gap rather than closing it: the sequence is pinned at the
level below, exactly as PW3b pinned its own. What is untested in both cases
is the same wiring, and RP7's kill −9 matrix against a real two-core server
is where it is exercised.

## Open, carried from the work order

- **D1–D7 are ratified** (2026-08-28) and so are both `[OPEN]`s
  (2026-08-27); §2 above is the record. What they leave open is not a
  decision but a set of obligations, listed there against the row that owns
  each — R6-3's isolation-level crossing, R6-5's named ceiling constant and
  its non-`UnknownOutcome` refusal, R6-5's sizing of the in-doubt ask,
  R6-9's two doc sentences, and B1's p50-and-p99 reporting.
- **`wal.md` §3's second `[OPEN]` is answered** (CP1, R6-4's section above):
  a core-count change is already refused at the door by the superblock's
  pinned `core_count`, so recovery never meets a prepare from a stream it
  cannot place, and the resolution reads a **file** rather than a running
  core in any case. The re-indexed-at-the-same-count shape is undetectable
  with the current format and is not made worse by R6. Closing the `[OPEN]`
  in `wal.md` itself is R6-9's.
