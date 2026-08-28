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
| R6-7 | PL-A revisit | **Built 2026-08-28** (analysis and doc; no engine code, which was the expected outcome). Verdict is a proposal — the operator rules |
| R6-8 | Dispatch | **Built 2026-08-28**, this worktree |
| R6-9 | Docs | — |
| **RP7** | **The correctness gate** (parent §5 in full) | **Run 2026-08-28**, this worktree. 12 cells x 3 passes, 36/36; suite 2,872 and `sim.sh` 171/0 against the pre-R6 arm's 2,789 and 171/0, one sitting. Re-run after the `origin/main` merge at `6cc8236`: 2,917 green, matrix 12/12. CP2 concluded. Overhead not measured |
| **RR0** | **The watermark (D3), and the join rule** | **Built 2026-08-28**, `acbd6b5` |
| **RR1** | **The read half of enrolment** | **Built 2026-08-28**, `acbd6b5`, same commit as RR0 |
| RR2 | The ceiling sweep, and CR3 | — |
| RR3 | B5 re-run, and CR4 | — |
| RR4 | R6-9 — the spec | **Written 2026-08-28**, `acbd6b5`: `docs/spec/cross-owner-txn.md` is new; `crosscore.md`, `wal.md` §3/§11-3/§15 and `client-manual.md` amended. The ceiling paragraph is owed RR2's number |
| RR5 | The two RP8 debts | **Paid 2026-08-28**, `acbd6b5`: `bench/docs/README.md`'s cross-owner entry, and the per-leg timer gap recorded at `observability.md` §8a beside M3's |
| RP8 | R6-B cells B1-B5 | **Run 2026-08-28**, `53f6aae`. `bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`. D7's ~2x holds, HP1 holds, HP2's falsifier does not fire; B5 structurally blocked |

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

The stop sequence ends, on every core, in `CoreRuntime::ShutdownCheckpoint`
— which **flushes the core's pages first and then checkpoints**, PW3b's
order, and `core_runtime.hpp` gives the reason: *"with the table empty the
redo start is the BEGIN LSN itself and the next mount reads this
checkpoint's own two records rather than everything since the oldest dirty
page."*

**Where `RollbackAllEnrolled` sits relative to it differs by core, and the
review of this row is what established that** — the row's first prose said
"per peer, the rollback then the checkpoint", and that is core 0's order,
not a peer's:

- **core 0**: `Serve` calls its own executor's `RollbackAllEnrolled`
  (`expeditor.cpp:1827`) as soon as the reactor stops, and its final
  `Sync()` + `Checkpoint()` are the last two statements of the function —
  rollback, *then* checkpoint;
- **a peer**: `Serve` syncs and calls `core->ShutdownCheckpoint()`
  (`expeditor.cpp:1861`) inside the per-core loop, and the peer's
  `RollbackAllEnrolled` runs later still, in `~CoreRuntime`
  (`core_runtime.cpp:46`) when `cores_.clear()` destroys it — checkpoint,
  *then* rollback.

**The floor's property is the same under either order, and that is why the
fixture models one of them.** `RollbackAllEnrolled` never touches a
prepared context (D4, and R6-3's own test pins it), so the set
`OldestPreparedLsn()` reports is byte-identical before and after it: the
checkpoint reads the same number whichever side of the rollback it runs on.
What the peer's inverted order does cost is stated in "What this row does
not prove" below.

**PW3b's own sentence, quoted above, is exactly the hazard.** A redo start
at the `CHECKPOINT_BEGIN` LSN is **past the `TXN_PREPARE`**, so the next
mount would scan from after the record that says "this transaction is not this core's to decide", find
the active-list entry the checkpoint carried, read it as an ordinary loser,
and roll back a transaction the coordinator may have committed and
acknowledged to a client. R6-4's floor is what prevents it, and until this
row the floor was tested only against a **scripted** `ActiveTransactions`
answering a number the test chose.

`Txn2pcShutdownTest` closes that: a real statement shipped, a real prepare
through the executor's seam, and then the stop sequence in core 0's order
(rollback, then checkpoint) over a target whose dirty table is empty
*because the flush already happened*. Three properties, and **all three
fail if the floor is removed** — verified by removing the two lines in
`checkpointer.cpp:149-151`, watching every one of them fail alongside the
R6-4 unit test, and restoring:

- the published redo start is at or below the LSN
  `TransactionManager::OldestPreparedLsn()` reports, which is the wiring
  claim the unit test could not make;
- the mount that scans from exactly that anchor still finds the prepare —
  `analysis.prepared == 1`, `losers == 0`, and the `PreparedTxn` naming the
  coordinator the resolution will look the decision up in;
- a **second** checkpoint while still in doubt holds the floor too, because
  the floor is applied to every checkpoint from the live transaction rather
  than only to the one that first saw the prepare. (It does not
  discriminate a floor computed once and cached — that would answer the
  same LSN. And its first assertion is the same property the fixture above
  asserts, so it is an `ASSERT`: with the floor gone this fixture must stop
  at the shared precondition rather than report the second checkpoint as
  the thing that broke.)

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
  with nothing failing and nothing to see in a log. `SHOW META`'s in-doubt
  block is what separates this case from C4's: C4 shows as a checkpoint
  Error line, and this shows as a counter — `txn_in_doubt` while anything
  is pinning the redo start, `txn_in_doubt_unresolved` for the subset no
  runtime path can finish. The repair for one is a `wal.md` §11 behaviour
  decision, and for the other it is R6-8's live path plus a mount.

C4 stays where it is — `known-gaps.md`, unfixed, owned by `wal.md` §11 —
with the re-read recorded there.

### What this row does not prove

**Nothing here drives a real `Expeditor` with `cores > 1`.** PW3b's own S6
finding says so of its own call site — *"`Serve`'s per-peer call site has no
test at all, since nothing builds an `Expeditor` with `cores > 1`"* — and
R6-6 inherits that gap rather than closing it: the sequence is pinned at the
level below, exactly as PW3b pinned its own. What is untested in both cases
is the same wiring, and RP7's kill −9 matrix against a real two-core server
is where it is exercised. **The peer's inverted order above is what that
gap costs**: nothing exercised it, so the row's first prose asserted core
0's order of a peer and no test could contradict it.

**And one thing the peer's order costs that is not R6-6's to fix**, named
here because this row is the one that read the sequence: a peer's
`RollbackAllEnrolled` runs *after* its final `Sync()` and after its
`ShutdownCheckpoint`, so the compensations it writes and the pages it
dirties are both dropped — nothing drains that core's WAL or flushes its
store again, and neither destructor does. The two are lost *together*, so
the next mount finds the transaction in the checkpoint's active list, calls
it a loser and rolls it back from a durable image that never saw the
partial undo: **correct, and correct only because both halves are lost at
once.** The cost is that a graceful stop leaves a peer's abandoned
cross-owner transactions to be rolled back at the next mount rather than
at the stop — and a prepared one is untouched either way (D4). It belongs
to PW3b's call site, beside S6.

## R6-7 — PL-A's revisit, and CP4

Built on `v2.5.0-cross-core-owner-protocol-2`. **No engine code**, which the
work order named as the expected outcome and as the finding if it were
otherwise: nothing in R6-3 … R6-6 touches page identity across streams, and
§ below says how that was checked rather than assumed.

### The clause, and that it has fired

`docs/spec/page-lsn-cross-stream.md` §9 reserved one: *"if cross-core commit
(2PC) is ever ratified, PL-A is re-opened **by that decision** — one global
LSN may then pay for both. Until then it is declined, not deferred."*
D1–D7 were ratified on 2026-08-28, so **the clause has fired and PL-A is
open.** `CLAUDE.md`'s Open Decisions index carries the same trigger in the
Transactions & WAL line and now reads one event behind.

### CP4 — what 2PC changes about cross-stream page handoff: nothing, and here is why

The order asks for the argument rather than the assertion, so it is made
from the built protocol and not from the design intent.

**What PL-A would have bought 2PC.** One global LSN makes comparisons
meaningful everywhere, so a decision could be *assembled* from several
streams and recovery could order two streams' records against each other.
That is the second decision the clause hoped PL-A would pay for.

**The ratified protocol asks for neither, by construction.**

- **D4 puts the decision in exactly one stream.** The coordinator's `COMMIT`
  record *is* the decision; nothing is assembled, so nothing is ordered.
- **The wire carries no LSN at all.** All five payloads in
  `server/txn_2pc_service.hpp` — prepare, decide, the participant reply, and
  R6-5's ask and answer — hold ids, a status, a decision byte, a retry bit
  and, on the reply, the refusal's own message bytes. The type `wal::Lsn`
  does not appear in that header once.
- **D2 keeps every stream's ids stream-local, by construction on the write
  path.** The participant runs its own local transaction and its envelope
  carries *that* id; the coordinator's `(session, transaction)` ride in the
  `TXN_PREPARE` payload, where nothing keys a stream's own recovery on
  them. **This one is a construction and not a refusal, and the correction
  is the review's** (RP4/RP5 review): the row first cited
  `CoreRuntime::Open`'s mount check — "a stream naming a trx id above the
  superblock's ceiling does not open" — as the enforcement, and it is not.
  Every core draws its ids from **one** counter, the superblock's
  `next_trx_id`, in blocks core 0 grants (`txn/trx_id.hpp`, PW1), so a
  coordinator's id is an ordinary id *below* the ceiling and that check
  passes over it. What the check enforces is a stream naming ids that were
  never granted; foreign-versus-own is not a distinction it can make. The
  claim below does not rest on it: what matters for PL-A is that the ids
  are not LSNs and are never ordered against each other, which the
  resolver's `std::map::find` is.
- **R6-4's resolution is two independent reads**, which HP5 predicted and
  the row confirmed with its site: `prepared_resolver.cpp`'s scan reads
  `record.header.txn_id` and `record.type()` and touches no LSN.

**The one comparison that looks cross-stream and is not.** R6-4's review
added an anchor-honesty check to the resolver: core A, resolving its own
prepare, reads core B's stream and refuses if the scan ends before the
durable point **B's own anchor** was published with
(`prepared_resolver.cpp:124-129`, `scan.end_lsn < anchor_durable_lsn`). Both
numbers are B's, so this is a within-stream completeness check that A
happens to perform — the same argument `analysis.hpp` makes about a stream's
own scan, applied to somebody else's. It is worth naming precisely because
it is the one place a reader would go looking for a violation of guideline 3
and find something that superficially resembles one.

**And the direction R6 *does* move a page-LSN fact — safely, by PL-B's own
rule.** R6-4 floors a checkpoint's redo start at the oldest live prepare, so
a participant holding one makes every later scan on that core **start
earlier** than it otherwise would. PL-B rule 3 removes a handed-off page
from the dirty table when the forward scan meets its `PAGE_HANDOFF`, and
rule 6 restamps a returning page at re-acquisition — so a scan that starts
earlier meets *more* handoff records, in order, and reaches the same state.
Lowering a redo start is safe for the handoff contract; raising one past a
handoff would not be, and nothing in R6 raises one. This is the only
interaction between the two mechanisms and it runs in the harmless
direction.

**Verdict, and it is a proposal rather than a closure.** PL-A is re-opened
by the clause and CLA's reading is to **decline it again**: the second
decision it would have paid for does not exist, because the ratified 2PC was
designed to need no cross-stream comparison and the built one demonstrably
makes none. Its cost is unchanged — an atomic on the engine's hottest path,
retracting `wal.md` §3's "no shared tail pointer, no lock, no atomic
contention on the append path" and `workplan-crosscore.md` guideline 1. The
comparison §7 said would change if 2PC arrived did change: it got *worse*
for PL-A, because 2PC turned out to be free of the thing PL-A was going to
subsidise.

**This is not CLA's to close.** Storage and WAL decisions are the operator's
(`CLAUDE.md` Open Decisions), and the clause re-opened a ratified decision
rather than delegating it. R6-9 carries the proposal into
`page-lsn-cross-stream.md` §9 once it is ruled on; until then that file
records the revisit as executed with this verdict pending, and `CLAUDE.md`'s
Transactions & WAL line needs the same correction.

## R6-8 — dispatch, and CP3

Built on `v2.5.0-cross-core-owner-protocol-2`. **The row that makes every
row before it reachable.** Until this one, `MayShip` refused inside an
explicit transaction, so nothing enrolled a participant on a live path and
R6-3 … R6-6 were exercised only by tests calling the seams by hand.

### The gate, and why it is a second one rather than a widened first

`MayEnrolShip` asks everything `MayShip` asks, with the
explicit-transaction test inverted and `txn_2pc_ != nullptr` in its place —
a core that cannot run D4's two phases must not make a transaction
cross-owner, or the transaction reaches `COMMIT` with a participant and no
protocol. The three write paths test `MayShip(...) || MayEnrolShip(...)`.

Two gates rather than one widened gate, because they admit different shapes
for different reasons and one of the differences is a **scope boundary**:
`MayEnrolShip` is asked only by the write paths. A cross-core **read**
inside a transaction keeps exactly the behaviour it had, since shipping one
would enrol a participant to give a snapshot D3's watermark is what makes
meaningful — and the watermark is not built. Reads are R6-9's
`crosscore.md` question.

### Three things the row had to get right, each of which would have been silent

- **The local scope must not be ended.** `AbandonWriteForShipping` ends an
  autocommit scope through `EndWrite`'s *failure* arm, which inside an
  explicit transaction **poisons the session** — so a cross-owner
  transaction would have been aborted by its own first shipped statement,
  and the client told to `ROLLBACK` a transaction that is intact. An
  enrolled ship now ends nothing: the scope is the client's open
  transaction, this half wrote nothing, and nothing failed.
- **The enrolment is recorded after the send, never before.** Every refusal
  `Ship` makes happens before the request leaves, so enrolling first would
  put a core into the prepare phase that was never asked anything — turning
  a refusal the client could retry into an aborted transaction.
- **The home core is untouched.** `BindHomeCore` runs at the end of
  `CheckWriteAffinity`, which a shipped statement never reaches, so a
  coordinator's `home_core_` still binds only on its *own* local writes.
  CC3's one-stream rule therefore keeps governing this core's half exactly
  as it did, which is what makes "the coordinator's half is the local one"
  true rather than aspirational.

### §2's obligation on D3, found unmet and paid here

The ratification lists it against R6-3: *"the coordinator's isolation level
has to cross"*. **R6-3 landed without it**, and this row is where that
stopped being harmless — a participant opened its transaction with
`Dispatch("BEGIN")` on its own dispatcher, whose `default_isolation()` is
that server's config key, so `BEGIN ISOLATION LEVEL REPEATABLE READ` on the
coordinator produced a READ COMMITTED participant and told nobody. Under D3
the level *selects the branch*, so the client was promised one thing and a
participant gave another.

One byte out of `ShippedStatementRequestPayload::reserved0`, carried on the
enrolled path only. **0 means "not stated" and is not a level** — the
zero-collision rule, so a zeroed buffer decodes as an absence — and a byte
outside the enum is **refused before anything runs**, `ShippedStatementRoleOf`'s
rule for exactly its reason. Autocommit requests state none and are
byte-identical to what SS2 sent: a single-statement transaction mints its
view at the same instant under either level, so carrying it there would
change a measured path to no observable end.

### CP3 — the refusal counter's third era, and it is short

`cross_core_write_refusals` is recorded at two sites, both inside
`CheckWriteAffinity`, and R6-8 changes **which statements reach them**
rather than what they say. What still counts:

**Site 1, the foreign-owner arm** — reached only where the ship fork
declined, which after this row is:

| what still refuses | why it is not shippable |
|---|---|
| no shipping client (`statement_ship_ == nullptr`) | every single-core instance and every fixture — which is what keeps `cores = 1` byte-identical |
| a path that cannot park (`may_park_` false) | the synchronous `Dispatch()`: a send from there has nowhere to deliver its answer, so `UnknownOutcome` would be the only honest refusal left after it (SS2's rule 1) |
| no coordinator armed, inside a transaction (`txn_2pc_ == nullptr`) | single-core and fixtures again; a transaction cannot be made cross-owner by a core that cannot decide it |
| a session that already arrived shipped | the hop limit — what arrived shipped does not ship on |
| an `UPDATE`/`DELETE` whose `WHERE` carries a subquery | the fork resolves nothing about a second relation, so the affinity answer is the honest one |
| a KWP **load chunk** (`ExecuteInsert`, `line.empty()`) | there is no statement text to ship — a load chunk arrives already parsed, so the fork has nothing to send and the relation's owner cannot be reached (`kwp_load_server.cpp:443`) |
| a poisoned session inside a transaction | defence: the failed-txn gate refuses the statement earlier |

**Site 2, the CC3 home-core arm — unreachable on any path this engine
has**, and CP3 is where that was established rather than assumed.
`BindHomeCore` has exactly one call site, at the end of
`CheckWriteAffinity`, after the foreign-owner arm has returned for every
relation this core does not own — so a bound `home_core_` is always
`core_id_`, and this arm needs it to be something else. It was equally
unreachable before R6-8. The guard stays (one comparison against a
transaction's writes splitting across two streams) with the fact recorded
at the site.

**So on a production multi-core instance with shipping and 2PC armed, the
third era is four classes**: the synchronous path, the hop limit, the
subquery shape, and the KWP load chunk. That is the roadmap's remaining
cross-core write debt stated exactly, and it is what B4 measures. (The
review of this row corrected the count: the first writing of it said three
and left the load chunk out, which is a live path — `KwpLoadServer` calls
`ExecuteInsert` with no text, and `command_dispatcher.hpp`'s note on `line`
already said what that costs.)

**Refusals that are not this counter's and are untouched**: `PeerDdlRefused`
(DDL on a peer, which also carries §5d's purge-gate argument),
`CrossCoreReadUnsupported`, `RelationWriteRightsPending` and
`IndexBuildPending` (owner-side, about rights rather than ownership), and
the shape gate's tail — FK-linked, cabined, and a relation under an
assertion this core cannot enforce.

### HP4, reported honestly

**Exactly one existing refusal test needed editing, and it is the class this
row converts.** `AStatementInsideATransactionIsNotShippedAndKeepsItsRefusal`
asserted `TXN_CONFLICT` for the cross-owner explicit-transaction shape, on
SS2's ground that *"nothing crosses transaction state"* — which D4
supersedes. It is renamed rather than deleted, and now asserts the
conversion at the site that used to assert its opposite, so the change is
legible in the tree and not only in this file.

HP4's falsifier says an edited refusal test *stops the row*. Read literally
that would stop R6-8 on the one refusal R6-8 exists to convert, which is
not what the hypothesis means: its subject is *"every **other** refusal"*.
Every other refusal test in the suite passes unedited — 2,862 green — and
that is HP4's real claim, confirmed.

### The `critics-developer` pass, and what it moved

Run against `7eaa14a`. **Two client-visible transaction-contract bugs, both
on the path this row makes live, and one of them a durable wrong answer** —
which is the argument for reviewing a row that opens a gate rather than one
that adds a mechanism.

**Bug 1 — a rolled-back cross-owner write committed with the *next*
transaction.** A participant's context is keyed on `(coordinator core,
session_id)` and nothing else, because the statement leg carries no
transaction id; `Session::ship_id()` was minted once and kept for the
connection's life. So two consecutive transactions of one session were
indistinguishable to a participant — and since nothing tells a participant
about a transaction that never reached prepare, a `ROLLBACK` left its
context standing for the *next* transaction's first shipped statement to
join. The review reproduced it on the two-core rig: `BEGIN; INSERT 91;
ROLLBACK; BEGIN; INSERT 92; COMMIT;` leaves **both** 91 and 92 durable on
the peer. Fixed at `Session::Finish()`, which drops the shipping id where
the transaction enrolled anyone — conditioned on `participants_`, so an
autocommit session's id is still minted once and the measured path is
untouched.

**Bug 2 — an owner's refusal left the transaction committable.** A failed
statement inside `BEGIN` poisons the session (`txn.md` §6, §10-8); the
local path takes that in `EndWrite`, which an enrolled ship deliberately
skips, and nothing took it where the owner's verdict arrives. A client told
`ERR` could `COMMIT` — and because failure atomicity is per transaction on
the participant too, a ten-row `INSERT` failing on the seventh leaves six
rows in the participant's open transaction for that `COMMIT` to make
durable. The `UnknownOutcome` at the shipped deadline was the same hole and
worse. Fixed in `DispatchAsync`, immediately after `FinishShippedStatement`.

**Bug 3, low** — `AbandonWriteForShipping`'s new early return also swallowed
the *no-manager* scope, whose end settles assertion reservations under
`kBootstrapXid`. Its comment claimed every scope reaching it before R6-8 was
owned; a dispatcher with no transaction manager also yields `owned ==
false`. Guarded on `scope.txn != nullptr` now — unreachable in production
wiring, and exactly the byte-identical claim HP4 makes.

**And a gap the review found and left, closed here** — the row's real
omission. `HandleCommit` forked on `has_participants()` and
`HandleRollback` did not, so a client's `ROLLBACK`, a poisoned
transaction's forced one, and the one `TcpServer::CloseClient` sends on a
dropped connection all ended this core's half and **told nobody**: each
participant went on holding its rows, pinning that core's read horizon and
one of its sixteen enrolment slots, until the five-minute idle ceiling — on
a loop any client can run. D4 already requires the telling (*"either way it
then tells the participants"*), so this is D4 built rather than a decision
taken. `Txn2pcClient::AbortAndForget` sends the abort with **no waiter
opened at all**, which is the shape a rollback wants rather than a
compromise: the outcome is abort whatever a participant answers, and the
caller that most needs it — the connection-close rollback — runs through
the synchronous `Dispatch()` and could not park for an acknowledgement.
Counted apart as `aborts_forgotten()`, since those acknowledgements are
*expected* to find no waiter and would otherwise read as a deadline problem.

**One false claim in this row's own prose, corrected**: the rollback test
said the owner's half was unwound "by the idle sweep at worst, and by the
decide leg's abort on the ordinary path". Neither happened — the test
passed because the participant's transaction was still *open*. It now
asserts the release.

**Cuts proposed and not taken**, with reasons: `(MayShip || MayEnrolShip)`
appears at three sites and could be one `MayShipWrite` — worth doing, and
worth doing where a fourth write path is added rather than as a rename in
the row that introduced the second predicate; `ShipStatement`'s
`session.transaction() != nullptr` is unreachable (`Adopt` never stores
null) and reads as a possibility that does not exist; and
`statement_ship_service.hpp` includes `txn/manager.hpp` for one enum a
forward declaration would cover. Taken: none of the three changes
behaviour, and the first is the only one that would prevent a defect.

### Tests

`tests/core_runtime_test.cpp`'s two-core rig gains core 0's coordinator
half, so the whole protocol runs over a real ring with the peer's
participant half wired by `AttachTransport` as production wires it: a write
inside a transaction ships, enrols its owner and is invisible until the
`COMMIT` runs both phases; a `ROLLBACK` sends no prepare and leaves the
owner's rows unseen (**not unwound** — the review below corrected that
claim, and the test now says which); the coordinator's level reaches the
participant's enrolled transaction while the peer's own default stays READ
COMMITTED, so the two are distinguishable; and a core with no coordinator
armed keeps the old refusal, counted where `crosscore.md` §6 says it is.

The review adds two regression tests, one per correctness bug, and the
abort leg adds two more: the participant *releases* on a `ROLLBACK` rather
than merely staying uncommitted, and a dropped connection's synchronous
rollback reaches its participants too.

Full suite green: **2,865 tests**. Overhead not measured — the v2-stage A/B
suspension of 2026-08-24 stands.

### The `critics-developer` pass, and what it moved

Run against `7eaa14a`. **Two correctness bugs on the path this row makes
reachable, one of them a wrong answer a client can produce in three
statements**, plus one guard narrowed and three prose claims corrected.

**Bug 1 — a rolled-back cross-owner write committed with the next
transaction.** A participant's context is keyed on
`(coordinator core, session_id)` and nothing else: the statement leg
carries no transaction id, so two consecutive transactions of one session
are indistinguishable there — while `Session::ship_id()` was minted once
and kept for the connection's life. Nothing tells a participant about a
transaction that never reached prepare (`RollbackLocal` sends no message),
so the context outlived its transaction and the *next* transaction's first
shipped statement joined it. Reproduced end to end on the two-core rig:
`BEGIN; INSERT peer(91); ROLLBACK; BEGIN; INSERT peer(92); COMMIT` left
**both** rows on the owner. Fixed in `Session::Finish()` — a transaction
that enrolled anyone drops the shipping id, so the next one addresses a
fresh context. Conditioned on `participants_`, so the autocommit path's id
is still minted once and kept, and only a cross-owner transaction pays the
extra dedup record. Regression test:
`ARolledBackCrossOwnerTransactionsWritesDoNotCommitWithTheNext`.

**Bug 2 — an owner's refusal left the transaction committable.** Failure
atomicity is per transaction (`txn.md` §6, §10-8): a statement that fails
inside `BEGIN` poisons the session and the client must `ROLLBACK`. The
local path takes that poison in `EndWrite`, which an enrolled ship
deliberately skips — and nothing took it where the *owner's* verdict
arrives, which is the only place it can be taken, since at ship time the
statement had not run. So a client told `ERR` could `COMMIT`, and an
owner-side statement that failed part-way (its rows stay in the
participant's open transaction, per-transaction atomicity again) made those
rows durable. The deadline's `UnknownOutcome` was the same hole and the
sharper case. Fixed in `DispatchAsync`, one test either side:
`AShippedStatementTheOwnerRefusesPoisonsTheTransactionThatSentIt`.

**A guard narrowed.** `AbandonWriteForShipping`'s new early return tested
`!scope.owned`, whose comment claimed every scope reaching it before R6-8
was owned. A dispatcher with **no transaction manager** also produces an
unowned scope, and its end is `EndWrite`'s no-manager arm settling the
statement's assertion reservations under `kBootstrapXid` — the autocommit
ship's path on such a configuration, which HP4 calls byte-identical. Now
`scope.txn != nullptr && !scope.owned`, which is the shape the row means.

**Left open, and named because R6-8 is what makes it reachable: a
`ROLLBACK` never reaches its participants.** `HandleCommit` forks on
`has_participants()`; `HandleRollback` does not, so a client's `ROLLBACK` —
and a poisoned transaction's, and the one `TcpServer::CloseClient` sends
for a dropped connection — ends the coordinator's half and tells nobody.
Each abandoned participant holds its rows uncommitted, pins that core's
`ReadHorizon()`, and holds one of `kShippedMaxEnrolled` (16) and one of
`kMaxTrackedLiveTxns` (64) until `kShippedTxnIdleCeilingNs` — **five
minutes**, on a loop any client can run. Not fixed here because the fix is
a decision, not an edit: the abort decide is a park, so it either makes
`ROLLBACK` wait a ring round trip (a client-visible latency change on a
path that never had one) or needs a fire-and-forget lane whose waiter
nothing closes; and `TcpServer`'s close path calls the **synchronous**
`Dispatch`, which cannot park at all. `StartDecision` already accepts an
abort for an unprepared context, so the participant side needs nothing.
Recorded in `known-gaps.md`.

**Prose corrected at the source**, three claims: the rollback test's *"the
owner's half is unwound before this returns"* (nothing unwinds it — the row
is unseen because the participant's transaction is still open, and the test
now asserts the open context rather than implying it away); CP3's class
list, which omitted the KWP load chunk and so counted three classes where
there are four; and `AbandonWriteForShipping`'s *"every scope that reached
here was owned"*.

Suite after the fixes: **2,864 green** — the 2,862 above plus one
regression test per bug. Overhead not measured, the v2-stage suspension
standing.

## RP7 — the correctness gate

The parent order's §5 in full (`instructions/v2.4.0/2pc.md` 232-249), run on
`worktree-v2.5.0-crosscore-protocol-2` at `5f08c0d` for the instrument and
re-run whole after the review's sixth point landed. One sitting,
2026-08-28. What the row adds to the tree is one facility and one probe,
and the facility is the reason §5's first bullet is answerable at all.

### The instrument, and why it is in the engine rather than in the harness

§5 does not ask for a crash somewhere in the protocol. It names four
positions — *"before prepare, between prepare and its durability, after
prepare before decide, after decide before participants learn"* — and asks
what a restart makes of each. Those windows are microseconds wide. The
existing precedent, `bench/shipped_kill_recovery_probe.py`, kills *"wherever
it lands"* and says so in its header, which answers a different question:
that the shipping path survives an arbitrary crash, not that a named
position does.

There are two ways to stop a process at a named source line. A debugger is
one, and this host has no `gdb`. The other is the process killing itself,
which is what `include/kds/base/crash_point.hpp` is: `KDS_CRASH_POINT` read
from the environment **once**, compared against the name at each call site,
and on a match `kill(getpid(), SIGKILL)`.

Three properties, each load-bearing and each stated because a weaker one
would have been easier to write:

- **It is `SIGKILL`, not `exit` or `abort`.** No destructor runs, no buffer
  drains, no `atexit` fires; the WAL and the data file hold exactly the bytes
  the device had already taken. `std::exit` unwinds statics and all three of
  the alternatives let a libc-buffered stream flush, which would make the
  durable state a property of the exit rather than of the protocol position.
- **It is armed by the environment and by nothing else.** No config key, no
  `SHOW META` field, no wire surface, so nothing a client can send arms one.
  `ParseCrashPointArm` is exposed and tested because it is the only branch in
  the file, and the failure it forbids is a spec that silently loses its
  tail: `a:b` read as name `a` would arm a point that does not exist, and
  every cell would then report a survival as a property of the protocol.
  `CrashPointHit`'s empty-name test is the same guard one level down —
  `KDS_CRASH_POINT=:5` parses to an empty name, and that line is what keeps
  it inert.
- **No call site is on the one-owner path.** All six sit inside blocks a
  transaction with no participants never enters. Asserted behaviourally
  below rather than by inspection.

The six points, with the position each occupies:

| point | site | durable state it leaves |
|---|---|---|
| `coordinator.before_prepare` | `command_dispatcher.cpp:3907`, before `txn_2pc_->Prepare` | participants hold uncommitted writes and were never asked |
| `participant.prepare_logged_predurable` | `shipped_statement_executor.cpp:515`, after `LogTxnPrepare`, before `RequestDurable` | a record nothing has asked the device for |
| `participant.prepare_durable_prereply` | same file, `:605`, after `MarkPrepared`, before `reply(OK)` | a durable promise the coordinator has not heard |
| `coordinator.prepared_predecide` | `command_dispatcher.cpp:380`, after the phase settles, before `CommitLocal` | every promise made, no decision written anywhere |
| `coordinator.decided_presend` | `command_dispatcher.cpp:445`, after the `IsDurable` park, before `txn_2pc_->Decide` | the decision durable in one stream, no participant holding it |
| `participant.decide_applied_preack` | `shipped_statement_executor.cpp:822`, after `enrolled_.erase`, before `reply(OK)` | **this half committed, the coordinator unacknowledged** |

The sixth is the review's, and §5 is not met without it — see the review
section below.

### The matrix

**Both sides of the wire are threads of one process**, so a kill takes the
coordinator and its participants down together — the observation
`shipped_kill_recovery_probe.py` already made about shipping. §5's "on each
side" is therefore a statement about *which core's code was executing*, not
about two processes, and the six names cover both sides.

The shape: `cores = 3`, `placement = rotate`, a client session on core 0.
Rotation never places on core 0 (`catalog/core_placement.hpp`'s
`AssignOwnerCore`), so the two relations land on cores 1 and 2 and the
client's core is a coordinator with two participants and no local data
write of its own. One row per relation is written before the transaction, so
a relation reading 0 because it was lost *whole* is distinguishable from one
that rolled back — and the seed is checked on **both** relations, which the
review had to correct.

Twelve cells, run three times over (`--repeat 3`): **36/36 PASS**.

| cell | rows in each relation | why that value |
|---|---|---|
| `coordinator.before_prepare` | 0 = 0 | nobody was asked |
| `participant.prepare_logged_predurable` (and `:2`) | 0 = 0 | a record nothing asked the device for promises nothing |
| `participant.prepare_durable_prereply` (and `:2`) | 0 = 0 | a promise the coordinator never heard cannot be decided |
| `coordinator.prepared_predecide` | 0 = 0 | every promise made, no decision anywhere |
| `coordinator.decided_presend` | **1 = 1** | the decision was durable; the mount must carry it out |
| `participant.decide_applied_preack` (and `:2`) | **1 = 1** | one half already applied; the mount must reach the same answer for the other |
| `resolution.coordinator_stream_gone` | mount refused | the decision cannot be read, so it is not guessed |
| `fastpath.local_only`, `fastpath.cores1` | 1 = 1, process alive | D1: no prepare was ever sent |

Every killed cell confirms `rc = -9` and finds its own point's line on the
dead process's stderr, so no cell counts rows after a death it did not
cause. **Unequal counts would be a `TORN` verdict on any of them**, which is
the failure two-phase commit exists to make impossible and the one this
matrix is shaped to catch.

### The mechanism is read, not inferred

Counting rows says a transaction is whole; it does not say *what made it
whole*. The probe therefore runs at `log_level = info` and reads the
resolver's own verdict line out of the restarted instance's log:

    coordinator.prepared_predecide
      core 1: transaction 4100, prepared for core 0's session 1 transaction 8,
        resolves to ABORT (its coordinator's stream holds no decision, so
        nothing committed anywhere)
      core 2: transaction 8196, ... resolves to ABORT (...)

    coordinator.decided_presend
      core 1: transaction 8196, prepared for core 0's session 1 transaction 8,
        resolves to COMMIT (its coordinator's decision)
      core 2: transaction 4100, ... resolves to COMMIT (its coordinator's decision)

`coordinator.before_prepare` logs no resolution at all, which is the third
reading: nothing was prepared, so nothing needed resolving. Those three are
the protocol end to end on a real instance — a decision that lived in
exactly one stream reaching two participants that never heard it, and the
same machinery declining to publish when the decision is absent.

### The asymmetric state, and the honest limit on pinning it

The state a wrong recovery would **tear** rather than roll back is the one
where the two participants die in *different* durable states — one half
already committed in its own stream, the other still merely prepared. The
restart then has to reach the same answer by two different routes: redo for
the first, resolution against the coordinator's stream for the second.

**No crash point can pin it.** The participants run on separate cores and
reach these lines concurrently, so "A past it and B not" is a race, not a
position. The probe therefore *records* it — `mixed` is true when exactly
one resolution line appears — rather than requiring it, and `--repeat` is
how it is sought. Over the three passes it was reached at four points,
`participant.decide_applied_preack` among them; a hand sweep of six runs of
that cell alone reached it once, so the rate is real but low. In every case
the two counts agreed and took the expected value.

That is the honest position: the asymmetric case **is** exercised and the
engine is right in it, but it is exercised by sampling rather than pinned by
construction, and a run in which `mixed` is false has tested the symmetric
case only.

### §5 bullet by bullet

**Atomicity, adversarially** — the table above; all-or-nothing by count at
every cell, three passes.

**In-doubt resolution, including the coordinator's record gone** — split
across two levels, and the split is worth stating rather than blurring. At
the *mount*, `resolution.coordinator_stream_gone` kills at
`coordinator.decided_presend`, deletes core 0's WAL stream, and the mount
**refuses**. What it refuses with is the finding: analysis's own anchor
check fires first — *"core 0's stream ends at lsn 4096, before the durable
point 4360 its checkpoint anchor was published with"* — so
`CoordinatorStreamResolver`'s absent-stream arm is **unreachable by this
route** and stays proved where it is provable, in
`prepared_recovery_test.cpp`'s `AnAbsentCoordinatorStreamRefusesRatherThanAborting`.
Either way §5's property holds: the decision cannot be read and the instance
says so instead of picking one. At *runtime*, the `UnknownOutcome` answer to
an ask whose coordinator no longer holds the record is
`Txn2pcResolveTest.ATransactionThisCoreHasNoRecordOfIsUnknownAndNeverGuessed`
and `Txn2pcInDoubtTest.AnUnknownAnswerLeavesTheTransactionInDoubtRatherThanGuessing`;
reaching it end to end would need the decide message dropped on a working
ring, which nothing in the tree can inject. **Stated as a limit of the gate,
not as coverage.**

**The one-owner fast path sends no prepare** — two readings, and the second
is new. At unit level
`Txn2pcCommitTest.AOneOwnerCommitSendsNoPrepareAndTakesThePathItAlwaysTook`
asserts `Txn2pcClient::prepare_messages() == 0`. At process level the
`fastpath.*` cells assert it by **a kill that does not happen**: the instance
runs with `coordinator.before_prepare` armed for its whole life and a
transaction every one of whose relations its own core owns, and it survives
and answers `COMMIT` with both rows present. A single prepare would have
killed it.

**`cores = 1` unchanged** — `fastpath.cores1` is that shape and passes, and
the suite is green at one core as at three. The *measured* half of this
bullet is not run: the interleaved A/B overhead measurement is suspended for
v2-stage development (operator amendment 2026-08-24), so **overhead is not
measured** and is reported as such rather than implied.

**A hang is a blocking finding** — none was produced, and **HP3 is reported
as the R6-5 review restated it, not as confirmed**: it *holds for every
statement wait*; the ask loop and the two commit-durability parks are
unbounded, one by decision and two as inherited. What RP7 adds is the
measurement on the side the reviews could not reach: every restart in the
matrix came back in **0.10-0.15 s** against a 90 s ceiling the probe would
have called `HANG`, no cell needed a timeout to finish, and no cell was
retried into a pass. So the *mount* half of HP3 is now measured; the two
unbounded parks are unchanged and are re-stated below rather than allowed to
read as bounded because a probe did not hit them.

### The four obligations earlier rows handed to RP7 by name

Each row above named RP7 as the place its item would be answered. Answered
here, and none of the four is closed by this row — three are reported as
they were asked to be, and the fourth bounds a result RP7 itself produced.

**1. The decision-durability park: reported as inherited, not fixed.** R6-3's
review recorded that `DispatchAsync`'s wait between the durable COMMIT and
telling the participants has no ceiling, and asked RP7 to *"report it as
inherited-or-fixed rather than let HP3 read as confirmed on a wait that was
never bounded."* It is **inherited**: the group-commit park two stages below
it has been equally unbounded since D2, and bounding one commit wait and not
the other would be arbitrary. RP7 did not fix it and does not claim it away
— the matrix cannot reach it, because a crash point kills the process rather
than stalling a device.

**2. HP3 is restated, not confirmed** — above, in the R6-5 review's own
words.

**3. The sim corpus does not cover cross-owner recovery, so its green does
not extend to R6.** R6-4's review left this for RP7 because RP7 runs
`scripts/sim.sh`: `sim/instance.cpp:67` mounts **core 0 alone** through
`RecoverCoreAtMount` with no resolver and no log *directory*, so a simulated
mount can never meet a prepared transaction. Confirmed by reading at this
commit. The 171/0 on both arms therefore says R6 broke nothing the corpus
covers — which is what it was run for — and says **nothing** about
cross-owner prepare resolution. That property is covered by
`prepared_recovery_test.cpp` and by this row's matrix, and by nothing in
`sim/`.

**4. `txn_decide_refusals` is not read here, and the R6-8 note says why it
must not be.** R6-8 recorded a benign refusal that can raise the counter
when a decide takes longer than 200 ms to cross a ring whose hop is ~21 µs,
and asked RP7 not to read a non-zero value as proof of a lost half. RP7
makes no claim from that field: the probe's evidence is the row counts and
the resolver's own log lines, and it never reads `txn_decide_refusals` at
all. Stated so a later reader does not mistake the silence for a zero.

### CP2 — the fast path is free at the instruction level, and here is every site

The work order asks for this conclusion in a specific form: *"name what a
single-owner commit executes that it did not before, and if the answer is
'one predictable branch on a field already in cache', say so with the
site."* M3's standard — sha256-identical binaries — cannot be met, because
R6 does change the binary. The enumeration can be, and it is short. Four
sites, read at `5f08c0d`:

1. **The commit path: one branch.** `src/server/command_dispatcher.cpp:6903`
   — `if (session.has_participants()) return PrepareAcrossOwners(session);`.
   `has_participants()` is `!participants_.empty()` on a `std::vector` member
   of the `Session` the caller already holds
   (`include/kds/server/session.hpp:220`). One test, always false on a
   one-owner transaction, on a field already in cache. **That is the whole of
   D1's cost on the commit path.**
2. **The rollback path: the same branch again**, at
   `src/server/command_dispatcher.cpp:6995`, the leg the R6-8 review added.
3. **The write path: nothing.** `CheckWriteConflictBlocking`
   (`command_dispatcher.cpp:7130`) returns on `verdict.ok()` before reaching
   anything R6 added; `IsInDoubt`'s scan over `live_` is reached only by a
   write that has **already lost a conflict**, a pre-existing error path. A
   one-owner instance with no conflicts executes zero added instructions
   here.
4. **The checkpoint path: one extra pass over an already-hot vector.**
   `src/wal/checkpointer.cpp:149` calls
   `ActiveTransactions::OldestPreparedLsn()`, which walks `live_` — the same
   vector the active table above it was just built from — and returns 0 on
   any core that is not a participant right now, leaving
   `pending_redo_start_` byte-identical. This is the one item that is not a
   single branch, and it is **per checkpoint, not per commit and not per
   statement**.

RP7's own instrument adds nothing to any of the four: all six crash points
sit inside blocks a one-owner commit never enters, which the `fastpath.*`
cells assert from outside by surviving a full armed run.

**HP1 is not falsified.** Its falsifier was *"CP2 finds work on the
single-owner path that is not one cached-field branch"*; item 4 is the only
candidate, it is off the commit path entirely, and its measured half belongs
to B3, which RP8 owns and which the A/B suspension governs.

### The suite and the corpus, both arms, one sitting

| arm | commit | `ctest` | `scripts/sim.sh` |
|---|---|---|---|
| pre-R6 | `ec5f993` (the `v2.4.0` tag) | **2,789 passed, 0 failed** | **171 runs, 0 failures** |
| R6 + RP7 | this worktree | **2,872 passed, 0 failed** | **171 runs, 0 failures** |

Both Debug, both on 2026-08-28, the pre-R6 arm a `git archive` export of
`ec5f993` built in a scratch tree — never a subtraction from a stored file.
The two sweeps are the same 171 cells because the committed seed file is
byte-identical across the trees and `sim.sh`'s four fresh seeds are derived
from the date: nine seeds × (3 modes × 2 fault settings × 3 value profiles +
1 advisory pair) = 171 either side. **R6 adds 83 tests and no regression**,
in the suite or in the corpus.

**Re-run after the merge**, because a green suite before a merge says
nothing about after one. `origin/main` had moved four commits under this
work — the varchar/char milestone (VC-A, VC-B) — and the merge at `6cc8236`
was clean. On the merged tree: **2,917 tests pass, 0 fail**, and the kill
matrix is **12/12** again, the asymmetric state reached at three points.
The gate is therefore reported against the tree that would be pushed, not
against the one it was built on.

One correction to an earlier row while the number was in hand: the R6-8
review section records **2,864** green, and `dae5ce2` measures **2,865**. A
one-test discrepancy in the prose, not in the tree.

And one thing 171/0 does *not* say: `known-gaps.md`'s open heap-chain
finding (seed 20260826003, `--faults io`) is a **2026-08-26** date-derived
seed that was deliberately not added to the committed corpus, so neither
arm ran it. A green sweep means no regression against what the corpus
covers, not that the corpus has nothing to find.

### §6's three additions

**No diff outside §1's in-list.** The changed-file set is eight files:
`include/kds/base/crash_point.hpp`, `src/base/crash_point.cpp`,
`tests/crash_point_test.cpp` and `bench/txn_2pc_kill_matrix_probe.py` (new);
`CMakeLists.txt` and `tests/CMakeLists.txt` (one source line each); and the
two protocol files that host the points,
`src/server/command_dispatcher.cpp` and
`src/server/shipped_statement_executor.cpp`. None of §1's
explicitly-not-touched four is among them: `RangeEligible` and its three
files, `sys.ranges`, the free map and the reactor wake path are all
untouched, and no file outside the two protocol ones was edited even where a
review finding pointed at one (see L2 below).

**HP4 holds by construction here.** Its falsifier is *"any existing refusal
test needs its expected text or status edited"*, and RP7 edited no existing
test at all — the only test change is a new file.

**The sitting.** Everything above ran on **2026-08-28**, one sitting, both
arms.

### The `critics-developer` pass, and what it moved

Run against `5f08c0d`. The reviewer verified (a) that the facility cannot
fire unarmed — by exercising it against `libkds.a` with four specs including
the `:5` empty-name case — (b) that all six points sit where their names
claim, and (c) that the one-owner path pays literally nothing. It then found
**three correctness bugs in the probe and one gap in the matrix**, all
applied.

**The gap, and it is the one that mattered.** *"The decide leg has no crash
point on either side, so the matrix never produces a partially-applied
decision."* Correct, and §5's "on each side" was not met without it: every
cell restarted from a state where zero halves were published, so the state
in-doubt resolution actually exists to repair was untested. The sixth point
is that finding, at the site the review named
(the `status.ok()` arm of `FinishDecision`, after `enrolled_.erase`;
`shipped_statement_executor.cpp:822` once the comment above it landed), with two cells expecting 1. **What building it then
showed is that the review's framing was one step optimistic**: the point
guarantees *this* participant is done, not that the other is behind, and the
two apply concurrently — so the asymmetric state is a race the point samples
rather than a position it pins. That is the `mixed` field and the `--repeat`
flag, and the section above states it as a limit.

**Three probe bugs, fixed.**

- **A leaked *armed* server on four error paths.** `arm_and_kill`'s early
  returns closed client sockets and abandoned the `Popen` still listening.
  Every cell binds the same port, so the next cell's `wait_up` could connect
  to the **previous cell's armed instance, holding the previous cell's data
  file** — a verdict read off the wrong process. `stop_proc()` now runs on
  every exit, and the three verbatim copies of that teardown elsewhere in
  the file collapse into it.
- **The seed row was asserted on `t0` only.** The seed exists to separate
  "the transaction rolled back" from "the relation came back empty", and for
  the five expected-0 points a `t1` lost whole reads as `{0, 0}` — equal, and
  equal to the expectation. A vacuous PASS on the exact failure the seed
  guards. Now checked on both.
- **`--only` matching nothing exited 0.** An empty gate is not a passed gate.

**Two more, applied.** `verdict`'s resolution arm could not report `HANG`
(the three shared guards are now hoisted above the `kind` fork, which also
deletes the duplicated copies); and `run_cell` had no
`except (ConnectionError, OSError)` around its post-restart block, so one
dying instance would have aborted the whole run instead of failing one cell.

**Two prose corrections at the source**, both found by reading the arms
rather than the comments: `coordinator.prepared_predecide` fires on the
refusal arm as well as the commit arm, and `coordinator.decided_presend`'s
decision is durable *by absence* on the ABORT arm rather than by the park.
Also the header's cost claim, which said "one load of a cached pointer" where
it is an out-of-line call, a once-guard acquire load and an `empty()` test.

**One finding rejected, by name.** L2 proposed lifting `tagged_rows` and
`session_on_core` into `tools/multicore_benchmark.py`, since
`shipped_kill_recovery_probe.py` carries near-identical copies. The
duplication is real and the destination is right — but the edit would put a
diff in two files outside §1's in-list, and §6 asks RP7 to assert the
changed-file set rather than to intend it. **Handed to whoever next touches
either probe**, with the note that `wait_up` is *not* part of it: it returns
`(ok, why)` and polls `proc.poll()` so a hang becomes a verdict rather than
an exception, which `wait_for_port` cannot do.

### What this row does not prove

- **The runtime `UnknownOutcome` leg is unit-tested only.** Nothing in the
  tree can drop a decide message on a working ring, so the end-to-end shape
  where a participant asks and its coordinator has forgotten is unreachable
  from a probe.
- **`CoordinatorStreamResolver`'s own absent-stream refusal is unreachable
  from a deleted file**, because analysis's anchor check refuses first. The
  refusal is real and tested; what is not established is a route to it from
  outside the process.
- **The asymmetric participant state is sampled, not pinned** (above).
- **Overhead is not measured.** The v2-stage A/B suspension stands. The fast
  path is asserted structurally (CP2) and behaviourally (`fastpath.*`), never
  as a number.
- **The kill is deterministic, so it is not a fuzz.** Six named positions is
  what §5 asked for. A crash at an unnamed instruction between them would be
  the sim corpus's job — except that the corpus cannot mount a prepared
  transaction at all (obligation 3 above), so **nothing in this tree
  fuzz-tests cross-owner recovery**. That is the largest single gap RP7
  leaves, and closing it means giving `sim/` more than one core and a log
  directory, which is a workplan of its own.

## RP8 — the B cells, and what they settled

Run on `worktree-v2.5.0-crosscore-protocol-2` at `53f6aae`
(`v2.4.0-28-g53f6aae`), same sitting as RP7. The numbers, the method and
every caveat live in
`bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md`; this is
the row's summary and the three things the milestone must carry forward.

**B1/B2/B3 — the predictions hold.** Seven interleaved rounds, 300
committed transactions per arm per round, 2,100 per arm, rows in = rows out
on every cell and no refusals. A two-owner transaction costs **1.479x** the
same work done as two separate one-owner transactions (the cell's own
comparison, measured inside one instance) and **1.975x** a one-owner commit
in D7's framing, p99 1.085x and 1.867x respectively. **HP2's falsifier does
not fire on either reading.** Width is flat from two participants to four
and first clears the noise floor at five, which is D7's "two deep, up to
four wide" shape and `bench/v2.1.0` §3a's overlap curve again. **HP1 holds**:
one-owner transactions on an R6 build against a same-sitting build of the
pre-R6 tag sit in [0.929, 1.031], inside a ~19% noise band the run
established from its own repeated cell — **D1's gate passes**.

**A hypothesis raised and refuted in the same sitting, recorded because the
first half of it is true.** A 20-transaction smoke sample read 3.55x and
suggested D7 undercounted the durable syncs by one. The *mechanism* is
real and confirmed by source at three sites — the coordinator's prepare
wait (`command_dispatcher.cpp:368`), its own decide-durability wait
(`:419-434`), and its wait for participants' post-decide commit-acks
(`:470`, each gated on the participant's own `IsDurable` park in
`shipped_statement_executor.cpp:751-786`) — and all three use the identical
durability primitive, so none is structurally free. The *cost prediction*
is false: three confirmed sequential waits do not cost 3x, and the measured
aggregate sits under a naive additive model in all seven rounds. Two
candidate explanations are named in the results file and neither is
confirmed. The lesson the file draws is about the host: a one-rep number on
this device cannot distinguish a 2x design from a 3x one.

**B4 — the refusal counter's third era, confirmed at scale.** R6-8's
conversion holds across every `xowner` cell: up to 10,500 cross-owner
explicit-transaction writes, **zero** refusals. What still refuses is
`CheckReadAffinity`'s classes and the residue CP3 already named; only
subquery-in-`WHERE` was reachable from an ordinary client session and it is
confirmed live. Found on the way: **`txn_decide_refusals` has no `SHOW META`
projection at all** — `Txn2pcClient::decide_refusals()` is a test-only
accessor — so R6-8's caution about misreading it governs a unit test, not
anything an operator can see.

**B5 — not merely unrun, structurally blocked, and it is a finding about
D3.** The scenario workload cannot reach the two-phase path in any
deployment shape: its booking transaction **reads** a foreign relation
before it writes, and `CommandDispatcher::CheckReadAffinity`
(`command_dispatcher.cpp:4422-4443`) refuses every cross-core read
unconditionally, in a transaction or out of one, independent of R6-8. So
the number B5 exists to produce — what fraction of a realistic workload
takes the two-phase path — is **zero, for a reason that has nothing to do
with the commit protocol**. Cross-owner *writes* became reachable at R6-8;
cross-owner *reads* inside a transaction did not, and until they do, R6
accelerates a shape no scenario in this tree can express. That is the
sizing input the order wanted, arriving as a blocker rather than a
percentage.

## RR0 — the watermark, and the bug found under it

Built on `v2.5.0-crosscore-protocol-3` at `acbd6b5`. Two things
landed here and only one of them was on the order's list.

### CR2, answered first, because it changed the row

The order asks whether a cross-owner RR transaction can fail to see its own
earlier write on another participant, and says the distinction is the
conclusion: *"if it can, that is a correctness bug and not a staleness
property"*. **It can, and it is a bug** — and the bug is not about
REPEATABLE READ at all, which is why the fix is level-independent and why
the watermark is not what closes it.

A participant's context is keyed on `(coordinator core, session_id)` and
nothing else (`ShippedStatementExecutor::DedupKey`); the statement leg
carries no transaction id, by `statement_ship_service.hpp`'s sizing
argument. Two things end a context while its coordinator's transaction is
still open: the idle ceiling (`kShippedTxnIdleCeilingNs`, 300 s,
`ExpireEnrolled`) and this core stopping. Both **erase** it. Before this
row, the next statement of that same transaction found no context and
`EnrolFor` opened a **fresh** one — and then:

- prepare found a context and promised it durable,
- the decided COMMIT committed it,
- the coordinator was told every participant committed,
- and the *first* half, the one the ceiling rolled back, was gone.

A transaction committed in part, with nothing anywhere saying so. It needs a
300-second idle gap inside one transaction and a second statement to the
same participant after it, which is rare and entirely reachable — a client
that holds a transaction open across a slow external call is the shape.

**The fix is one wire byte**, `join` on `ShippedStatementRequestPayload`
(the request had two reserved bytes; one is left). The coordinator sets it
on every enrolled statement after the first it sent to that owner, which it
reads from `Session::participants()` — the list it already keeps — so the
enrolling statement carries 0 and every later one carries 1. A participant
that is told "join, do not open" and finds nothing to join refuses
**`TxnConflict`**, retryably, because that is exactly true: nothing of the
transaction survives on that core and running it again from the top has
nothing to undo. `SHOW META` projects `shipped_join_refusals`, which is
`shipped_enrolment_expiries` seen from the other side.

It is one byte rather than the coordinator's 8-byte transaction id — the
thing that would otherwise be needed to tell a re-enrolment from a new
transaction on the same session — because `Session::Finish()` already mints
a fresh `ship_id` for every cross-owner transaction (the R6-8 review's
clearing), so a key can only ever mean one transaction and *"have you got
it"* is the whole question.

**Demonstrated, not argued**: with the bit reverted,
`AStatementThatCanOnlyJoinIsRefusedWhenTheParticipantsContextIsGone`
(`tests/core_runtime_test.cpp`) answers `INSERTED oid=4000 id=5 page=130
slot=4` where it now answers a refusal — the second transaction opened, and
the first one's row was already rolled back.

### HR2, confirmed: the watermark rides the reply and needs no new leg

D3's ratified form has the coordinator carry a per-participant watermark and
the participant read at or before it. Read against this engine, the
coordinator **has observed nothing on a participant before that
participant's first answer** — there is no earlier message and no shared
clock — so the first reply *is* the first observation. The watermark
therefore rides the reply leg: `ShippedStatementReplyPayload` gains a
`read_watermark`, placed so the reply's fixed header stays 32 bytes and the
reply text cap does not move.

What the participant reports is its enrolled transaction's
`ReadView::up_to_trx_id`, **in that core's own id space**, compared with
nothing on any other core (`wal.md` guideline 3). **REPEATABLE READ only**,
per D3's `[OPEN]` ratified *yes, READ COMMITTED skips the watermark
entirely*: an RC transaction re-mints its view at every statement boundary
by design (`TransactionManager::StartStatement`), so a watermark for it
would name a view already gone.

The coordinator holds one value per participant on the `Session`, cleared
with the transaction by `Finish()`. The first reply establishes it; a later
reply that names a different one is refused, because a participant's
enrolled RR transaction pins its view once and never re-mints it. **What
that check is actually for**, now that the join bit closes the re-enrolment
route: it is the standing test on R6-8's level crossing. If the isolation
byte ever failed to cross, the participant would run READ COMMITTED while
its client was promised REPEATABLE READ, its view would move at every
statement, and the transaction would be refused rather than answered from
two snapshots. `SHOW META` projects `txn_watermark_refusals` when non-zero.

**And "reads at or before it" is delivered by the pinned view, not by a
value sent back down.** The participant's RR transaction pins at its own
BEGIN and cannot move; there is no earlier position for the coordinator to
push it to, because the coordinator had none to offer. The order's HR2 holds
— no fourth leg, no new exchange — and D3's promise is delivered by R6-8's
level crossing plus this row's guarantee that the pinned view cannot be
silently replaced.

### CR1 — what the RC path pays, named at the site

An RC cross-owner **read** executes, over an RC autocommit foreign read:

1. `MayEnrolShip(session)` at `command_dispatcher.cpp`'s read site — one
   call, five pointer/bool tests, reached only because `MayShip` short-circuits
   false inside a transaction. On the autocommit path `MayShip` is true and
   `MayEnrolShip` is never called.
2. In `ShipStatement`: the isolation optional (`session.transaction()->isolation()`),
   `session.HasParticipant(owner_core)` — a linear scan of a vector whose
   length is the transaction's participant count — and `EnrolParticipant`,
   the same scan again with a possible `push_back`.
3. On the participant: `EnrolFor`'s `enrolled_.find(key)` and the reuse of a
   standing `Session` **instead of** constructing a fresh one per statement,
   which is what the autocommit arm does. After the first statement this is
   cheaper, not dearer.
4. In `Finish`: the enrolled arm's `enrolled_.find(key)` — which was already
   there before this row — and, inside it, one isolation compare. **The
   watermark adds no lookup**: a first draft had a helper of its own doing a
   second `find`, and it was folded into the existing one precisely because
   the RC path must not pay for a mechanism it is ratified to skip.
5. In `FinishShippedStatement`: `reply->read_watermark != 0`, which is false
   at RC, so the comparison and nothing else.

6. At the pipeline gate: one more `MayEnrolShip`, evaluated **last** in
   each condition, after the free shape tests — so a local read never asks
   it, which is what keeps CP2's "free at the instruction level" a claim
   about the local path rather than one this row spent.

**HR1 holds at the branch level**: there is no per-read work beyond the
enrolment branch the write half already pays, plus one more test of the same
predicate at the gate. But CR1's honest answer has **two** further halves
the hypothesis did not ask for, and both are larger than anything above.

**The first is a route change, not a branch.** Before this row a foreign
read inside a transaction that reached `SELECT * FROM <peer relation>` was
answered by the single-step pipeline; now it is shipped. Those are two
different mechanisms with two different cost profiles — batched step
messages against one statement carried as text and one reply — so R1 and R2
are not measuring a branch's price but a substitution's. The pipeline is
untouched for every session that cannot enrol, so no measurement ever taken
on that path moves.

**The second is per transaction: a cross-owner read makes its owner a full
two-phase participant.** The
read enrols, so at COMMIT the coordinator prepares a core that only read: a
`TXN_PREPARE` record and its own `fdatasync` on that core's stream, plus the
decide leg and its ack. That is a **per-transaction** cost, not a per-read
one, and it is the price of the read seeing this transaction's own
uncommitted writes on that core — which only that core's own transaction can
show, and which nothing this core holds can supply. The standard remedy is
the read-only-participant reply: a participant that wrote nothing answers
prepare with "nothing to prepare, released" and drops out of the decide
phase. That is a new answer on an existing leg rather than a fourth phase,
but it changes `AllPrepared()` and what recovery expects, so it is **out of
this order's scope by §1** and handed on named. The R1/R2 cells price what
it would be worth.

## RR1 — the read half

Built on `v2.5.0-crosscore-protocol-3` at `acbd6b5`, in the
same commit as RR0 because the watermark has nothing to report until a read
can reach it.

`command_dispatcher.cpp`'s read site now tests
`(MayShip(session) || MayEnrolShip(session))`, which is the three write
sites' spelling. **`Txn2pcService` is untouched** — HR3 asserted by diff:
the change is a gate, two payload fields and a `Session` vector, and no
protocol leg, crash point or recovery path moved.

`SoleForeignOwner`'s two-foreign-owner refusal stays, per the order: a
multi-owner *statement* is not this row's business, only a multi-owner
*transaction*. So a read joining a local relation to a foreign one, or two
foreign ones, is refused exactly as it was.

### The gate the row first missed, and what a review is for

A first draft of this row landed the gate and stopped there, recording the
remote-step pipeline as *"a pre-existing property of CC4 this row leaves
standing"*. The `critics-developer` pass showed that reading was wrong in
the way that matters, and it showed it by **reproducing** rather than by
arguing: it copied the row's own test, added the production
`SetRemoteReads` wiring that `ForeignIndexRig` omits, and got

```
PROBE reply: id,v\n1,10\n2,20\n3,30
PROBE shipped delta: 0
```

— the transaction never saw its own row 77, and the read never shipped at
all. Two things had been missed. First, it is not only the *two-step* path:
the **single-step** pipeline takes `SELECT * FROM <peer relation>` whole,
which is the commonest read there is and the exact shape the row's own
tests used. Second, the tests could not see it, because the rig installs no
pipeline and production always does — so the row's headline claim was being
proved against a wiring nobody runs.

And it is not a weakening. Inside an autocommit statement, reading a peer
at latest-committed is CC4's documented rule. Inside a cross-owner
transaction it is a **wrong answer**: that transaction's own writes on the
owner live in the transaction the owner holds for it, and no view the
pipeline can take shows them. The order says so itself — CR2, *"if it can,
that is a correctness bug and not a staleness property"*.

**Both fast paths are now skipped for a session that can enrol**, gated on
`MayEnrolShip` rather than on `in_explicit_txn()` so that exactly the
sessions which can *reach* the ship path are diverted and every other
configuration keeps the pipeline it had. Autocommit is untouched, and with
it every measurement ever taken on that path.

**The price is real and is stated rather than buried.** A shipped read's
answer must fit one ring slot — 992 bytes of reply text — so a cross-owner
transaction's read of a participant is bounded by that and is **refused**
past it rather than truncated. Two consequences the row had to fix with it,
both of which only became reachable because reads now ship:

- an over-long answer arrived as `UnknownOutcome`, whose words are a
  write's (*read the data back rather than retrying*) and are advice to do
  the thing the statement just failed at. A read has no outcome to be
  unknown about; the code stays, because the answer genuinely did not
  arrive and nothing may invite a retry loop under a `retryable` bit, and
  the false sentence goes;
- **a failed shipped read poisoned its transaction**, where a failed
  *local* `SELECT` does not — `Poison()`'s call sites are the write, DDL
  and in-doubt paths. The two agreed by construction while only writes
  shipped; since reads ship, agreeing takes saying so.
  `PendingShippedStatement::read`, set at the read fork, is what says it.

Both are pinned by `AnOverLongShippedReadIsRefusedAndLeavesItsTransactionOpen`,
and its sibling `TheSameReadOutsideATransactionTakesThePipelineAndHasNoCap`
holds the other half — the same statement, same rows, answered whole by the
pipeline outside a transaction, with `shipped_executed` still 0.

### The `critics-developer` pass, and what it moved

Run against the working tree before `acbd6b5`.

**Applied.** **C1**, a use-after-free the reviewer fixed itself: the
watermark refusal's log line read `reply->read_watermark` *after* `Close()`
had erased the node it points into — the same rule `DescribePrepareFailure`
states one function down, and the ordinary arm ten lines below already
followed. **C2**, the pipeline gate above. **C3** and **C4**, the two
consequences of the gate. **C5**: `join_refusals_` was documented as
*"counted apart from"* `enrolment_refusals_` and is in fact a **subset** —
`Execute` counts every refusal `EnrolFor` returns — so the doc changed
rather than the number, because an existing series reads that field.
**C6**, two false claims in the row's own comments: the watermark check
cannot see a level that failed to cross (an RC participant reports 0 and
nothing is compared), and two watermarks are incomparable **not** because
the id spaces differ — there is one instance-wide sequence, leased per core
— but because each is a high-water mark over the ids *that* core issued.
**S2**, `EnrolParticipant` duplicating `HasParticipant`'s loop verbatim.
**S4**, line numbers copied from the work order that were stale in the tree
the comment lives in.

**Rejected, by name.** **S1** proposed the coordinator's watermark
machinery as a candidate cut, on the ground that RR0's join bit makes its
false branch unreachable — which is true, and which is why the reviewer
offered the alternative rather than the deletion. Taken as the
alternative: D3 is ratified, the check is one comparison on a field already
in the reply, and the fix is that the comment now says plainly that the
join rule is what makes it unreachable, so its existence is not read as
evidence the case occurs. **S3** proposed replacing `watermarks_` with a
vector parallel to `participants_` to drop a duplicated key column.
Rejected: parallel vectors are the smell this codebase avoids, and the
saving is a linear scan over a list whose length is the participant count.

## Open, carried from the work order

- **D1–D7 are ratified** (2026-08-28) and so are both `[OPEN]`s
  (2026-08-27); §2 above is the record. What they leave open is not a
  decision but a set of obligations, listed there against the row that owns
  each — R6-3's isolation-level crossing, R6-5's named ceiling constant and
  its non-`UnknownOutcome` refusal, R6-5's sizing of the in-doubt ask,
  R6-9's two doc sentences, and B1's p50-and-p99 reporting.
- ~~**The two-phase path is unreachable from any scenario in this tree, and
  the blocker is a *read***~~ (RP8's B5) — **closed by RR1**, `acbd6b5`.
  The refusal was narrower than "unconditional": the read site tested
  `MayShip` alone, which requires `!in_explicit_txn()`, so a foreign read
  *inside* a transaction fell through to `CheckReadAffinity` while the
  identical read outside one shipped. It now tests both gates. What
  `CheckReadAffinity` still refuses is CP3's third era — a statement
  spanning two owners, and one from a path that cannot park — and RR3 is
  what turns B5 from a blocker back into a number.
- **A cross-owner transaction's read of a participant is bounded by the
  reply cap**, and that is the live limit RR1 leaves. A shipped read's
  answer must fit one ring slot (992 bytes of reply text) and is refused
  past it; the same read outside a transaction still takes the pipeline and
  has no such bound. `crosscore.md` §9's ring sizing is what lifts it, and
  until it is lifted the honest reading of "reads cross" is *reads of
  bounded answers cross*.
- **A read-only participant is prepared like any other** — a `TXN_PREPARE`
  and an `fdatasync` on a core that only read. The standard remedy is a
  read-only reply on the existing prepare leg, which is a new *answer*
  rather than a fourth phase, but it changes `AllPrepared()` and what
  recovery expects. Out of this order's scope by §1, handed on named.
- **`wal.md` §3's second `[OPEN]` is answered** (CP1, R6-4's section above):
  a core-count change is already refused at the door by the superblock's
  pinned `core_count`, so recovery never meets a prepare from a stream it
  cannot place, and the resolution reads a **file** rather than a running
  core in any case. The re-indexed-at-the-same-count shape is undetectable
  with the current format and is not made worse by R6. Closing the `[OPEN]`
  in `wal.md` itself is R6-9's.
