# Cross-Owner Transactions — the two-phase commit protocol

What happens when one client transaction touches relations owned by more
than one core. This is the spec `docs/spec/crosscore.md`'s scope boundary
deferred (*"cross-core **commit** … remains `[OPEN]` per `docs/spec/wal.md`
§3 — reserved for a later 2PC design, not designed here"*) and that
`docs/spec/wal.md` §3 and §15 point at. It is **confirmed and implemented**,
which is what puts it in `docs/spec/` rather than in `docs/inflight/`: every
rule below is enforced by code and covered by a test named where it matters.

Built across three work orders — `instructions/v2.4.0/2pc.md` (R6-0…R6-2,
design decisions D1–D7), `instructions/v2.5.0/cross-owner-protocol.md`
(RP0…RP8), `instructions/v2.5.0/cross-owner-protocol-closing.md` (RR0…RR5).
The task rows, the findings, the retractions and the measurements live in
`docs/inflight/in-progress/workplan-cross-owner-txn.md`; **this file carries
what is true, not how it was arrived at**.

**Scope, stated once.** Cross-owner transactions are complete at **relation
granularity**. Multi-*range* transactions inherit this protocol unchanged —
a participant is a core, never a relation, so only owner discovery changes —
and are `blueprint-range-ownership.md` §11's R6, gated on RD3's resolver.
A **core-count change** is a mount-time reorganisation and its own milestone
(`docs/spec/wal.md` §3); the resolver here already survives one, because it
reads files rather than live cores.

---

## 1. The shape

A transaction is **cross-owner** the moment it touches a relation another
core owns. There is no declaration, no join, no registration up front:

- The **arrival core coordinates** — the core the client's connection is
  on. Not the data's owner, not core 0, and not a coordinator chosen by
  what the transaction touches (D1).
- **Participants are discovered as the transaction runs.** The first
  statement this session ships to core *P* enrols *P*; the list is
  `Session::participants()`, in discovery order, and it is empty until the
  first ship.
- A transaction whose every relation is owned by one core has **no
  participants** and takes the single-core path unchanged. That is a test
  on `participants().empty()`, and it is what keeps `cores = 1` and every
  local transaction byte-identical to what they were before this protocol
  existed.
- A participant runs the transaction as an **ordinary local transaction**
  under **its own** trx id, carved from its own core's lease (D2). The
  coordinator's `(session_id, transaction_id)` is recorded beside it when
  prepare brings it. There is no shared transaction id and no global
  counter — a shared id would put foreign ids in every participant's
  stream, which `CoreRuntime::Open`'s mount check refuses.

### 1a. How a statement reaches a participant

By **statement shipping** (`docs/spec/crosscore.md` §6): the statement
crosses as *text*, is parsed and bound on the owner against the owner's own
catalog, and executes there. Inside a transaction it differs from the
autocommit case in exactly three wire bits:

| bit | on | meaning |
|---|---|---|
| `in_txn` | request | run this under the transaction held for `(coordinator core, session_id)`, and do not end that transaction when the statement finishes |
| `isolation` | request | the **coordinator's** level, because the level selects a branch (§3) and a participant that fell back to its own config default would give a transaction the weaker promise while its client was told the stronger one |
| `join` | request | this statement may **join** a context and may not open one (§2a) |

and one on the way back:

| field | on | meaning |
|---|---|---|
| `read_watermark` | reply | the participant's watermark for this transaction (§3), or 0 for "none stated" |

**Both reads and writes ship** (2026-08-28, RR1). Until that row the read
site tested only the autocommit gate, so a foreign read *inside* a
transaction was refused while the identical read outside one shipped — and
that asymmetry made the whole protocol unreachable from any realistic
workload, because a booking or ordering transaction reads before it writes.

**A read enrols, and it must.** A transaction that wrote a row on a peer
and then reads that relation has to see its own uncommitted write; only
that peer's own transaction can show it, and nothing the coordinator holds
can supply it.

**What that costs, measured** (2026-08-28,
`bench/v2.5.0/results-rr-read-half-*.md` §5): the read itself costs
2.25x-9.08x an autocommit foreign read, because an enrolling session falls
through both remote-read fast paths and takes the general shipping route
rather than the purpose-built single-hop reader — a change of route, not a
branch added to a path. And the larger half is not the read: a core that
only *read* becomes a full participant and pays a **full cross-owner
decide** at commit, 7-30x what the read cost. The read-only-participant
reply that would remove it is a new answer on an existing leg, not built,
and that ratio is what says what it would be worth.

**What still does not ship**, each a scope statement:

- a statement **spanning two owners** (`SoleForeignOwner` refuses two
  foreign owners) — a multi-owner *statement* is not a multi-owner
  *transaction*, and only the second is this protocol's business;
- a statement on a path that **cannot park**, because a protocol opened
  from a path that cannot await its answers would leave participants
  prepared with nobody to decide for them;
- `ANALYZE`, which would answer a request for a plan with a result set.

---

## 2. The protocol

Two phases, both over the existing core ring, both in `6b`'s
request/reply shape — POD payload, parked waiter on the coordinator, a
deadline, `Status::FromWire` (D4).

1. **Prepare.** The coordinator sends prepare to every participant. A
   participant makes its work durable — its own stream, its own
   `fdatasync` — writes a `TXN_PREPARE` record naming the coordinator's
   `(session_id, transaction_id)`, and replies **prepared** or **refused**.
   The promise is made *after* the record is durable, never at the append.
2. **Decide.** Every participant prepared ⇒ the coordinator commits **in
   its own stream**, and that `COMMIT` **is the decision**. Any refusal or
   timeout ⇒ abort. Either way it then tells the participants, and waits
   for their acknowledgements. A participant applies the decision as its
   own ordinary local COMMIT or ROLLBACK, and **under D2 `group` it
   acknowledges at the append**, its own record's durability riding the
   next drain — see the contract below.

**The decision lives in exactly one stream**, and that is the whole design.
LSNs are stream-local and are never compared across cores
(`workplan-crosscore.md` §3's guideline 3), so a decision recorded in two
places could be recovered two ways. One-phase commit and
presumed-commit/presumed-abort are foreclosed by the same rule.

**A participant that replied prepared may not unilaterally abort.** The
idle ceiling that ends an abandoned context stops applying to it; the
shutdown path leaves it in doubt rather than rolling it back, because an
`TXN_ABORT` for a transaction the coordinator may already have committed
is durable disagreement — the one outcome two-phase commit exists to
prevent. It ends only by the decided COMMIT or ROLLBACK, or at the next
mount.

**A prepared transaction takes no further statement.** Prepare is a promise
that everything the transaction wrote is durable; a statement admitted
after it would write rows the record does not cover.

**The transaction's durability point is the coordinator's decision
record, and a participant's own terminal record is a redo shortcut.**
Ratified 2026-08-31 (`instructions/v2.7.1/ratification-xd1.md`, enacted by
`instructions/v2.7.1/workorder-xd.md`), and it states what the protocol
already practiced rather than relaxing anything. Once the decision is
durable in the coordinator's stream, the outcome is fixed and reachable
everywhere: a participant whose own COMMIT survived is a winner by redo,
and one whose COMMIT was lost holds a durable `TXN_PREPARE` and resolves
to the same answer against that stream (§2c). Both routes end at the same
outcome, so the participant's record is what makes recovery *cheap*, never
what makes it *correct*.

Two things follow, and both are contracts rather than observations:

- **Under D2 `group` a participant acknowledges a decide at its COMMIT
  append**, not after its own `fdatasync`. The append registers the commit
  with the group, so the next drain syncs it whether or not anybody is
  parked; what the old wait removed was a serialization, not a durability.
  This takes the chain from three device syncs to two.
- **D1 `strict` is unchanged and keeps three.** Its sync happens *inside*
  the commit call, before it returns, so there is no post-append wait to
  move; buying the same saving there would mean committing a participant's
  half at a class the session did not ask for. **D3 `relaxed` never took
  this wait** and is likewise unchanged.

What is *not* licensed by this: a participant may still not acknowledge a
**prepare** before its record is durable (step 1's rule stands — the
promise is the durability), and the coordinator may still not answer a
client before its own decision record is durable. Those two waits are the
protocol; only the third was bookkeeping.

### 2a. A context is joined, never re-opened

A participant's context is keyed on `(coordinator core, session_id)` and
nothing else — the statement leg carries no transaction id. Two things end
one while its coordinator's transaction is still open: the **idle
ceiling** (`kShippedTxnIdleCeilingNs`, 300 s of idleness, for a coordinator
that never decides) and the participant core stopping. Both erase it.

So the coordinator states, on every statement after the first it sent that
owner, that the statement may only **join**. A participant told to join and
finding nothing refuses `TxnConflict`, retryably: nothing of the
transaction survives on that core and running it again from the top has
nothing to undo.

**Without this the engine committed transactions in part.** The next
statement opened a fresh transaction, prepare and commit made *that* half
durable, the rolled-back half was gone, and the client was told everything
committed. Closed 2026-08-28 (RR0); `shipped_join_refusals` counts it, and
it is `shipped_enrolment_expiries` seen from the other side.

One byte suffices because `Session::Finish()` mints a fresh `ship_id` for
every cross-owner transaction, so a key can only ever name one
transaction — *"have you got it"* is the whole question.

### 2b. In doubt, and the bounded wait

A participant that has replied prepared and heard no decision is **in
doubt**. It may neither abort nor commit; it holds its locks and waits
(D5). Two things bound the wait:

- The participant **asks** its coordinator what was decided, once per
  `in_doubt_ceiling_ms`, over a third exchange that is a *resolution ask*
  and not a third phase. A coordinator that still holds the decision
  answers it. One that no longer holds the record answers
  `UnknownOutcome`, which is terminal: nothing at runtime can resolve that
  transaction and the next mount is what does.
- A **writer of the same rows** blocks rather than being refused
  immediately, and is refused by name at `in_doubt_ceiling_ms` — a
  **retryable** refusal, not `UnknownOutcome`. The distinction is load
  bearing: `UnknownOutcome` tells a client to read the data back, and a
  *blocked writer*'s own statement plainly did nothing.

`in_doubt_ceiling_ms` is a config key reached through one function, and it
bounds two things rather than one — see §5.

### 2c. Recovery

At mount, a `TXN_PREPARE` with no decision in its own stream is neither a
winner nor a loser: it is a **fourth outcome**. Its coordinator's stream is
the authority, and resolving it is a *file* read, not a message — which is
why it survives a core-count change and a coordinator that is not running.

A prepared transaction **floors the checkpoint's redo start** at its
`TXN_PREPARE` (`ActiveTransactions::OldestPreparedLsn`,
`Checkpointer::Start`). Without that floor the record leaves the replay
range once the transaction's pages are written back, the next mount reads
the active-list entry as a loser, and rolls back a transaction the
coordinator may have committed — D4's exact prohibition, reached with no
message and no refusal anywhere. `docs/spec/wal.md` §11-3 carries it.

The price is the standard one: an in-doubt transaction pins the log, and
§2b's ceiling is what bounds how much.

**The retention obligation, in force from 2026-08-31.** Resolution takes
*no decision found ⇒ ABORT* and scans the coordinator's stream whole from
LSN 0, because no sound lower bound exists — a bound from this stream's
LSNs would be the cross-stream comparison guideline 3 forbids, and one
from the coordinator's checkpoint would assume the decision sits above it.
That answer is sound only while the decision is still *in* that stream, so:

> **A coordinator's stream may not recycle a segment holding a decision
> until every participant of that transaction has made its own terminal
> record durable.** A pre-durable acknowledgement does not discharge this:
> either the ack carries the participant's durable point, or retention is
> floored by something other than the acks.

Stated **before** the policy it constrains, deliberately. Nothing recycles
a WAL segment today (`wal.md` §11 is `[PROPOSED]` and retention is an open
item in §15), so there is no hole to fix — and the code that would open one
is not the code that would look wrong: a retention policy written against a
core's own checkpoint alone is locally correct and silently recovers
another core's committed transaction as aborted. Until D2's ack moved to
the append (§2), the acknowledgement itself proved a participant's half was
durable and a policy could have keyed on it; it no longer does, which is
why the rule is written down rather than inherited.
`instructions/v2.7.1/ratification-xd1.md` carries the argument.

---

## 3. Isolation — what a cross-owner transaction promises

**A cross-owner transaction under REPEATABLE READ sees a
consistent-per-core snapshot, not a globally consistent one** (D3). It is a
weakening and it is a product property, not only a spec line —
`docs/spec/client-manual.md` states it in the client's words.

Concretely:

- The coordinator carries a **per-participant watermark**: that
  participant's `ReadView::up_to_trx_id`, established by the participant's
  first reply and never moved for the transaction's life. It is compared
  with nothing on this core and nothing on any other participant — **and
  not because the id spaces differ**, since there is one instance-wide
  trx-id sequence leased per core. It is because the quantity is a
  high-water mark over the ids *that* core has issued plus its own
  in-flight set, so two of them are two cores' answers to a question about
  themselves and ordering them numerically orders nothing. The single
  global instant that would let them be compared needs a global commit
  sequence, which is the shared counter D2 rejects.
- The participant's own enrolled transaction is what *delivers* the
  promise: opened at the coordinator's level, it pins its view at its own
  BEGIN and cannot re-mint it. §2a's join rule is what guarantees it cannot
  be silently replaced by a newer one either.
- The coordinator's copy is the standing check on that: a reply naming a
  different watermark means the snapshot moved, and the transaction is
  refused rather than answered from two views. **On this tree that branch
  is unreachable** — the one thing that moves a pinned view is the context
  being replaced, which §2a's join rule refuses a leg earlier and on the
  participant — and it is written down rather than left to be inferred,
  because a check's existence otherwise reads as evidence its case occurs.
  It does not catch a level that failed to cross: a participant running
  READ COMMITTED reports no watermark at all, so nothing is held and
  nothing is compared. `txn_watermark_refusals` counts what it does catch.
- **READ COMMITTED carries no watermark at all** (D3's `[OPEN]`, ratified
  2026-08-27). RC already permits every statement to observe the latest
  committed state, so there is nothing for a watermark to pin, and the
  default level therefore pays nothing for any of this.

Two cross-owner RR transactions can disagree about the order of two commits
on two cores. That is the price of shared-nothing, and it is the price
`crosscore.md` §5 was already paying: this narrows it from *no ReadView at
all* to *a ReadView per core* and stops there.

**The remote-step pipeline does not run inside such a transaction**, and
that is a rule rather than an accident. `HandleSelect`'s two remote-read
fast paths — the single-step one that takes `SELECT * FROM <peer
relation>` and the two-step join — sit *above* the shipping fork and answer
from the owning core's latest-committed view, outside any transaction this
session holds (`crosscore.md` §5's CC4). Inside an autocommit statement
that is the documented weakening. Inside a cross-owner transaction it is a
**wrong answer**: this transaction's own writes on that owner live in the
transaction the owner is holding for it, and no view the pipeline can take
shows them. So a session that can enrol is diverted to the ship path, where
the read joins that transaction.

It is gated on `MayEnrolShip` rather than on "is in a transaction", so
exactly the sessions that can *reach* the ship path are diverted: a
dispatcher with no 2PC client, or a path that cannot park, keeps the
pipeline it had.

**The price is the reply cap, and it is real.** A shipped read's answer
must fit one ring slot — `kShippedStatementReplyTextMax`, 992 bytes of
reply text — so a cross-owner transaction's read of a participant is
bounded by that, and an answer past it is refused (*the read returned
nothing and changed nothing*) rather than truncated. A refusal and not a
wrong answer, which is the trade this row took deliberately: correctness
first, and the cap is the next thing to lift (`crosscore.md` §9's ring
sizing). The same read outside a transaction still takes the pipeline and
has no such bound.

---

## 4. What a client sees

- A cross-owner `COMMIT` answers `COMMIT trx_id=<n>` exactly as a local one
  does, and a client cannot tell from the reply that two phases ran.
- A prepare refusal or a participant that never answers aborts the whole
  transaction and says **who** refused, in that participant's own words and
  with its own retryable bit.
- A shipped statement whose answer is lost is `UNKNOWN_OUTCOME`,
  **non-retryable by construction**: this engine issues primary keys, so a
  blind retry of a statement that may have committed inserts a second row.
  The remedy is to **read the data back**, never to retry — the words
  shipped statements already use.
- A **write** that fails on its owner **poisons the transaction**, exactly
  as a local write's failure does: failure atomicity is per transaction, so
  the client must `ROLLBACK`. A **read** that fails does not, for the same
  reason and read the same way — a local `SELECT` that fails leaves its
  transaction open, and the two have to agree.
- A shipped **read** whose answer never arrives is told so in its own
  words: *the read returned nothing and changed nothing*. The code is still
  `UNKNOWN_OUTCOME`, because the answer genuinely did not arrive and
  nothing here may invite a retry loop, but the write's advice — read the
  data back — is not given to a statement whose whole purpose was to read
  it.
- A `ROLLBACK` tells the participants too, so their contexts end at the
  client's word rather than at the idle ceiling.
- **The client's answer has never depended on participant
  acknowledgements**, and since 2026-08-31 that is stated rather than left
  implied. The coordinator answers from its own decision record: where a
  participant does not acknowledge, the coordinator logs a line and the
  client is still told `COMMIT`, because the decision is durable and the
  transaction is settled — the unacknowledged participant is holding locks
  in doubt (§2b), which is a liveness fact about that core and not an
  outcome the client is owed. So an acknowledgement is a **release
  signal**, never a durability proof, and moving D2's ack to the COMMIT
  append (§2) does not weaken anything the client is promised. What the
  durability class still governs, exactly as for a one-owner transaction,
  is when the **decision** reaches the platter before the client hears it.

---

## 5. Sizing and configuration

| name | where | what bounds it |
|---|---|---|
| `in_doubt_ceiling_ms` | `CommandDispatcher::InDoubtCeilingNs()`, default `kTxnInDoubtCeilingNs` = 200 ms | **The writer's stall, and only that.** Swept 2026-08-28 against both axes it was proposed to answer to (`bench/v2.5.0/results-rr-read-half-*.md` §7-§9). Stall tracks the knob — p0 within a few percent of the configured value — and at 200 ms the refusal fired once in ~60,000 attempts. **Log retention does not track it at all**: peak WAL held per checkpoint tick was indistinguishable from a control where nothing ever prepared. The floor in §2c is real and does hold the log back; what bounds *how long* is `kTxnPhaseDeadlineNs` where the coordinator is alive but slow, and the next mount where it is not — neither of which this knob moves |
| `kShippedTxnIdleCeilingNs` | `shipped_statement_executor.hpp`, 300 s | How long an abandoned participant context is held before it is rolled back. Deliberately far above the statement deadline: nothing on a healthy path reaches it |
| `kShippedMaxEnrolled` | `shipped_statement_executor.hpp`, 16 | How many cross-owner transactions one core holds as a participant. A bound on a **shared** resource — each enrolment is one of `txn::kMaxTrackedLiveTxns`, which local clients share — so without it a coordinator storm would refuse an unrelated connection's `BEGIN` with nothing naming the cause |
| wire sizing | `txn_2pc_service.hpp` | 24 bytes per request leg, 256 for the participant reply, against a 1,024-byte ring slot — asserted against `kCoreRingPayloadBytes`, never the literal (D6) |

### 5a. Measured sizing — what the protocol costs, in parts

Measured 2026-08-31 at `951a91a` by work order XD
(`instructions/v2.7.1/measurement-xd.md`), on scenario 2 over a real
ring; the file is
`bench/v2.7.0/results-xd-commit-decomposition-v2.7.0-2-g951a91a.md`, and
R6-B (`bench/v2.5.0/results-r6b-cross-owner-cost-*.md`) is the prior
number every ratio here is stated against.

**The commit is three device syncs, and the class does not change that
except in one leg.** Counted from outside the process with `SHOW META`'s
`wal_syncs`, per core, over 20 transactions per arm:

| durability class | one-owner commit | cross-owner commit |
|---|---:|---:|
| `strict` | 1.00 sync | **3.00** — 2 on the participant, 1 on the coordinator |
| `group` | 1.00 sync | **3.00** |
| `relaxed` | 0.00 sync | **2.00** |

The chain is §2's, and **two of its three legs are unconditional on the
durability class by construction**: the participant's prepare
(`shipped_statement_executor.cpp`, `RequestDurable` then a park — the
promise is made after the record is durable, never at the append) and the
coordinator's decide (`command_dispatcher.cpp`, whose comment states it —
*"whatever the durability class"*, because `relaxed`'s window is a promise
about this stream's own recent commits, not about a record another core is
about to act on). Only the third leg, the participant's own local COMMIT,
rides the class — which is why `relaxed` shows 2 and not 3, and why **the
cross-owner increment is +2 syncs in all three classes**.

**At one transaction the legs are additive, with no batching discount.**
On ext4 the two extra legs cost ~1,002 µs each against a 925.9 µs local
sync on the same device, and the ratio to a one-owner commit is
**3.077× under `strict`** (median of three repeats) and **3.111× under
`group`** — close to the naive 3-syncs-over-1 reading. R6-B's 1.975× is not
in conflict: it prices a shape that ships one row per participant, where
scenario 2 ships every one of a booking's 6-8 statements individually, so
the two bracket what a transaction pays by how much of it is shipped.

**Concurrency shares the device but not the queue.** As bookers rise 1 → 8
under `group`, syncs per booking fall 3.00 → 2.45-2.50 — the three legs'
`RequestDurable`s do ride the same drain — while the commit's own p50
*rises* 2.93 ms → 4.83-5.01 ms (the two b = 8 cells), because the legs
are sequential within a
transaction and core 0's foreground occupancy climbs behind them (2.6% →
5.6% of reactor wall time). Batching is a throughput property here, never
a latency one.

**It is a device cost.** Moving only the WAL segments to tmpfs — durability
semantics and code paths untouched, the data file left on ext4 — collapses
the cross-owner increment **51-62×**, from 1,988.7 µs to 38.0 µs at one
booker and 3,743.9 µs to 60.4 µs at eight. The ~40 µs residue is the ring
hop and the parks. Those numbers are a probe of where the mass sits and are
never engine numbers.

**What this sizing is *not* evidence about**: the read-only-participant
reply of §8. Scenario 2's booking writes to its participant on four
statements, so no participant of it can ever answer read-only; that lever
is priced by `bench/v2.5.0/results-rr-read-half-*.md`'s read-half
workload and by nothing here.

---

## 6. Observability

`SHOW META`, core-local like every counter there, so a whole-instance
reading is one per core. **Absent rather than zeroed** where the shape is
structurally impossible.

| field | reads |
|---|---|
| `shipped_enrolled` / `shipped_enrolments` | this core as a participant: how many cross-owner transactions it holds now, and how many it has opened. Each live one pins this core's read horizon and one of its 64 live-transaction slots |
| `shipped_enrolment_refusals` | enrolments this core declined — its own limit, or a trx-id lease it could not draw. Retryable |
| `shipped_enrolment_expiries` | **should be 0**: contexts the idle ceiling rolled back because no decide arrived. Non-zero names an abandoning coordinator, not a rate |
| `shipped_join_refusals` | statements that could only join a context and found none — §2a, the other side of the line above. **A subset of `shipped_enrolment_refusals`**, not a count beside it: every refusal the enrolment path returns is counted there too, so the two are never summed |
| `txn_watermark_refusals` | transactions refused because a participant answered from a different snapshot than the one they had been reading it at (§3) |
| `txn_in_doubt`, `_asks`, `_committed`, `_aborted`, `_unresolved` | the in-doubt population and what became of it. `txn_in_doubt_unresolved` is the one number naming a transaction nothing at runtime can finish |
| `wal_syncs`, `wal_interval_syncs` | not this protocol's counters, but the ones its cost is read from (XD0, 2026-08-31, `docs/spec/client-manual.md`). Every device sync this core performed, and the D3 idle tick within it; `wal_syncs - wal_interval_syncs` is the part somebody was parked on. Cumulative from mount, so a reading is a before/after delta, and §5a's per-transaction sync counts are exactly this field divided by the transactions between two readings |

---

## 7. Testing

- **The kill −9 matrix** (`bench/txn_2pc_kill_matrix_probe.py`): six crash
  points across the protocol, twelve cells with the ordinal siblings, run
  three passes. Four of the six expect **0** committed on both relations
  (a 1 would be a transaction nobody decided) and two expect **1** (a 0
  would be a decision made durable and not carried out). In every one,
  **unequal counts are a torn transaction**, which is the failure the
  protocol exists to make impossible.
- Unit coverage: `tests/txn_2pc_protocol_test.cpp` (both halves of the
  protocol, in doubt, resolution, the blocked writer),
  `tests/shipped_statement_executor_test.cpp` (the participant's context,
  the ceiling, the join rule, the watermark),
  `tests/core_runtime_test.cpp` (end to end over two real cores),
  `tests/prepared_recovery_test.cpp` (the fourth outcome at mount).
- **What the simulation corpus does not cover, stated because 171/0 is
  otherwise read as more than it is**: `sim/instance.cpp` mounts core 0
  alone, so it cannot mount a prepared transaction at all. A green corpus
  means this protocol broke nothing the corpus covers.

---

## 8. What this protocol does not do

- **Multi-range transactions** — inherits this protocol unchanged;
  `blueprint-range-ownership.md` §11's R6, gated on RD3's resolver.
- **A read-only participant optimisation** — a core that only read is
  prepared with a durable record like any other (§1a).
- **Reads whose answer exceeds one ring slot** from a participant (§3's
  price) — the cap itself is `crosscore.md` §9's ring sizing.
- **Cross-core foreign keys.**
- **Snapshot forwarding to the remote-step pipeline** (§3's last
  paragraph).
- **Online core-count change** — mount-time, and its own milestone.
