# Work order R6 — cross-owner transactions

Drafted 2026-08-27 against `main` at `8a4c795`.

## 0. Why this version exists

A relation's `owner_core` is an engine decision. The user did not make it,
cannot see it, and `AssignOwnerCore` takes it at `CREATE TABLE` from a
rotation counter. Today that internal decision surfaces as a **refused
transaction**: a `BEGIN … COMMIT` that writes two relations the engine
happened to place on different cores is refused, and nothing the user wrote
says why.

That is an abstraction leak, not a performance question. Statement shipping
closed the same leak on the other axis — after SS1–SS5 a client no longer
needs to know which core accepted its connection. This version closes the
remaining half.

| leak | hidden by | state |
|---|---|---|
| which core the session sits on | statement shipping | built |
| which core the relation lives on | **this version** | — |

**The comparison that governs every trade below is against a refusal, not
against a local transaction.** A cross-owner commit will cost more than a
single-core one; the honest "before" is that it did not happen at all. This
is the same reading `bench/v2.3.0/results-knob-sweep-cell2-*.md` §4a
reached for shipping, and it is why no benchmark ratio gates this work.

**What measurement is for here.** `known-gaps.md:1010` records the R6
residue as a measured distribution — `in_explicit_txn` and `subquery_write`
refuse 100%, `two_owner_read` 87.5% — and that distribution sizes the
*optimisation* effort, never the decision to build. A rare case gets a
correct simple implementation; a common one earns tuning. R6-B measures it
after the fact, not before.

## 1. Scope

**In.** A transaction — explicit (`BEGIN … COMMIT`) or a single statement —
whose writes touch relations owned by two or more cores, executed and
committed atomically.

**Out, and stated so nothing assumes otherwise.**

- **Distributed transactions across instances.** One instance, N cores, one
  device, one recovery. Nothing here is a network protocol.
- **Cross-core DDL.** DDL stays system-core-only. A transaction that mixes
  DDL and cross-owner DML is refused with the existing spelling.
- **Raising the ring payload.** `crosscore.md` §9's open sizing decision.
  R6's messages must fit the slot that exists (D6 below) or say plainly
  that they cannot, which would make the sizing decision R6's gate rather
  than its neighbour.
- **The 992-byte reply cap** and its `UNKNOWN_OUTCOME` spelling for reads.
  A separate defect on the same wire; R6 must not make it worse and does
  not fix it.

## 2. The prerequisite that is not negotiable

**Finding 1 (dedup eviction) must be decided before D5 is written.**

Today `kShippedDedupMaxRecords = 4096` can evict a record inside its
retention window, and a duplicate meeting the empty record is **re-executed**
— measured, 4,097 → 4,098 executed. It is latent only because no live path
produces a duplicate: `SendRetryTask` retries sends that `TrySend` refused,
which by definition never arrived.

**R6 ends that.** A prepare whose reply is lost must be re-asked — that is
what an atomic commit protocol does — so R6 introduces the first real retry
path and turns a latent defect into an active one on its first day.

The option this order assumes, and which the operator must confirm or
replace before D5: **a retry bit in the request** (`reserved0[5]` has the
room). First attempt sets 0 — an absent record means "not seen", execute. A
retry sets 1 — an absent record means `UnknownOutcome`, never execution.
Availability cost zero, and it separates the memory bound from correctness
entirely. R6's own prepare/commit messages then inherit the same discipline
rather than inventing a second one.

If a different option is taken, D5 changes and this order is revised. It is
not built around.

## 3. Design decisions

Every one of these is a decision the operator owns. They are written as
proposals with their reasoning so that a disagreement lands on the reason
rather than on the outcome.

### D1 — Who coordinates

The **arrival core** — the one holding the client's session — is the
coordinator. It already holds the session, the transaction id, the hop
limit and the client socket, and it is where `UNKNOWN_OUTCOME` has to be
spelled if anything is lost.

Participants are relation owners, discovered as the transaction runs rather
than declared up front: a transaction becomes cross-owner at the moment its
second owner is touched, and not before. **A transaction that turns out to
touch one owner must take the single-core path unchanged**, paying nothing —
this is the `cores = 1` no-regression argument (guideline 2) applied one
level in, and it is what keeps the common case at today's cost.

### D2 — What a participant holds

A participant runs the transaction's statements under a **local transaction
of its own**, with its own trx id from its own lease, and records the
coordinator's `(session_id, transaction_id)` beside it.

Rejected alternative: one global trx id shared across cores. It would need
either a global atomic counter — guideline 1 — or cross-stream id ordering,
which `wal.md` §3 excludes. Per-participant local ids with a coordinator
mapping keeps every stream's ids stream-local, which is the invariant the
whole recovery design rests on.

**Consequence to state in the client manual**: a cross-owner transaction has
several trx ids internally, and anything that prints one prints the
coordinator's.

### D3 — The visibility rule, and the honest weakening

`crosscore.md` CC4 today: a remote step reads the **owning core's latest
committed snapshot**; there is no cross-core ReadView. §5 records the RR
weakening and marks escalation-or-forwarding `[OPEN] alongside the 2PC
milestone`. This version is that milestone, so the `[OPEN]` closes here.

**Proposal: forward the coordinator's snapshot to participants, and accept
that it is a snapshot of *each participant's own* history.** Concretely, the
coordinator carries a per-participant watermark — the last commit it has
observed on that core — and a participant reads at or before it. This gives
a transaction a stable view of each core it touches, repeatable across
statements, which is what RR promises.

**What it does not give**, and this must be written down rather than
discovered: a single global instant. Two participants' watermarks are points
in two independent streams, and `wal.md` guideline 3 forbids comparing them.
A cross-owner RR transaction therefore sees a **consistent-per-core**
snapshot, not a globally consistent one, and two such transactions can
disagree about the order of two commits on two cores.

The alternative — a globally ordered snapshot — requires a global commit
sequence, which is the same shared-counter that D2 rejects. **The weakening
is the price of shared-nothing and it is already the price CC4 pays; this
version narrows it from "no ReadView at all" to "a ReadView per core" and
stops there.**

`[OPEN]` for the operator: whether READ COMMITTED cross-owner transactions
skip the watermark entirely (cheaper, and RC already permits latest-committed
per statement). Proposal: yes — carry watermarks only for RR.

### D4 — The protocol

Two phases, both over the existing ring, both using 6b's request/reply
shape (POD payload, parked waiter on the coordinator, deadline,
`Status::FromWire`):

1. **Prepare.** The coordinator sends prepare to every participant.
   A participant makes its work durable — its own stream, its own
   `fdatasync` — writes a `PREPARE` record naming the coordinator's
   `(session_id, transaction_id)`, and replies **prepared** or **refused**.
   After replying prepared it may not unilaterally abort.
2. **Decide.** All prepared → the coordinator writes `COMMIT` to **its own**
   stream and this record is **the decision**; any refusal or timeout →
   `ABORT`. Either way it then tells participants, which write their own
   `COMMIT`/`ABORT` and release.

**The decision lives in exactly one stream** — the coordinator's — because
a decision that had to be assembled from several streams would be a
cross-stream ordering question, which guideline 3 forbids. Recovery reads
the coordinator's stream to learn the outcome and each participant's to
learn what to redo.

### D5 — In-doubt, and what the client is told

A participant that has replied prepared and then loses contact is
**in doubt**: it may not abort (the coordinator may have committed) and may
not commit (it may have aborted). It holds its locks and waits.

Resolution is by asking the coordinator, and the ask is a **retry** in
Finding 1's sense — §2's bit set, so a coordinator that no longer holds the
record answers `UnknownOutcome` rather than re-deciding.

**The client contract**, which must be in `client-manual.md` and not only
here: after `COMMIT` returns `UNKNOWN_OUTCOME` the transaction may or may
not have applied, and the remedy is to **read the data**, never to retry the
transaction. This is the same contract shipped statements already carry and
the same words should be used.

`[OPEN]` for the operator: whether an in-doubt participant blocks writers of
the same rows (correct, and can stall) or refuses them retryably (available,
and surfaces an engine-internal state to the user). Proposal: block, with a
bounded wait that ends in a named refusal, so the stall has a ceiling.

### D6 — Message sizing

Prepare and decide carry ids and a status, not statement text — tens of
bytes against the 1,024-byte slot. **No ring resize is required**, and R6
must confirm that in R6-1 rather than assume it: if any R6 message needs
more, `crosscore.md` §9's sizing decision becomes this order's gate and the
order stops until it is taken.

### D7 — Where cost is paid, stated before it is measured

A cross-owner commit costs **at least two durable syncs in sequence**
(prepare, then decide), where a single-core commit costs one. Against the
~0.9 ms sync this host measures, a two-owner transaction should cost roughly
twice a one-owner one, plus two ring round trips at ~20 µs each — the wire
is noise here, as it was for shipping.

Participants' prepare syncs can overlap: they are separate streams and
`bench/v2.1.0` §3a measured this device overlapping four streams at 3.37×
before declining. So the expected shape is **two sync latencies deep,
however many participants wide, up to four**. Above four participants the
device's own curve, not the protocol, is the limit.

This is written now so that R6-B either confirms it or finds something the
design did not predict — the latter being the more useful outcome.

## 4. Implementation — R6 series

| # | Task | Gate |
|---|---|---|
| R6-0 | **The retry bit** (§2). The wire field, its semantics on both sides of the dedup record, and a test that an absent record plus a set bit yields `UnknownOutcome` and never an execution. Small, and everything after it assumes it | operator's Finding 1 decision |
| R6-1 | **Wire and sizing.** Prepare/decide message kinds, POD payloads, `static_assert` against the ring slot per D6. If anything does not fit, stop and report — do not shrink a field to make it fit | R6-0 |
| R6-2 | **Participant transaction context** (D2). A participant runs shipped statements under a local transaction keyed by the coordinator's `(session_id, transaction_id)` instead of the autocommit-per-statement shape SS3 built. This is the largest row: SS3's D3 says a shipped statement "runs under the owner's ordinary local implicit transaction", and this replaces that for the explicit case while leaving autocommit exactly as it is | R6-1 |
| R6-3 | **Prepare and decide** (D4). The coordinator's parked waiter over N participants; the participant's durable prepare; the decision record in the coordinator's stream. The one-participant fast path (D1) short-circuits before any of it | R6-2 |
| R6-4 | **Recovery** (D4). Analysis learns each participant's prepared-but-undecided transactions and resolves each against the coordinator's stream. **This is where `wal.md` §3's second `[OPEN]` — recovery across a core-count change — has to be answered**: a prepare in a stream whose core no longer exists, or exists with a different index, must resolve rather than hang. If the answer is a refusal to mount, that is an answer and it is recorded as one | R6-3 |
| R6-5 | **In-doubt handling** (D5): the participant's wait, its bounded ceiling, the resolution ask with R6-0's bit, and the client-facing `UNKNOWN_OUTCOME` contract | R6-3 |
| R6-6 | **PW3b extension.** A prepared-but-undecided transaction must survive shutdown and restart. PW3b's third point landed 2026-08-25; this adds the state R6 introduces and re-reads that workplan's open review item (`known-gaps.md:250`) in this light | R6-3 |
| R6-7 | **PL-A revisit.** The page-LSN spec reserved a revisit clause for exactly this milestone (`crosscore.md` §9). Execute it: state what 2PC changes about cross-stream page handoff, or state that it changes nothing and why | R6-3 |
| R6-8 | **Dispatch.** `CheckWriteAffinity` stops refusing the cross-owner explicit-transaction shape and enrolls a participant instead. Every other refusal keeps its exact spelling and wire bit, including the DDL and hop-limit ones. `cross_core_write_refusals` keeps its semantics: post-R6 it reads what is *still* refused, which is a third era for the same counter | R6-3, R6-5 |
| R6-9 | **Docs.** `crosscore.md` CC3 and CC4 rewritten (the refusal becomes enrolment; the `[OPEN]` on snapshot forwarding closes per D3); `wal.md` §3's cross-core `[OPEN]` closes and its core-count `[OPEN]` closes or is restated with R6-4's answer; `client-manual.md` gains D3's per-core-snapshot weakening and D5's read-the-data contract; `known-gaps.md`'s R6 entry closes and the residue entry becomes third-era | R6-0…R6-8 |

## 5. Correctness — the gate, ahead of any number

- **Atomicity, adversarially.** Kill −9 at each protocol point, on each
  side: before prepare, between prepare and its durability, after prepare
  before decide, after decide before participants learn. After every one,
  every relation's rows are all-or-nothing for that transaction, verified
  by count.
- **In-doubt resolution** exercised deliberately, including the case where
  the coordinator's record is gone and `UnknownOutcome` is the honest
  answer.
- **The one-owner fast path** does not enter the protocol: assert it by
  counting prepare messages on a transaction that touches one owner. Zero.
- **`cores = 1`** unchanged, guideline 2, measured against the pre-R6 tag.
- **Full suite and `scripts/sim.sh`.** Baseline at the pre-R6 commit,
  re-read after; the sim corpus varies day to day (cell 6), so both runs go
  in one sitting or the comparison is void.
- **A hang is a blocking finding.** Two-phase commit's characteristic
  failure is not a slow transaction, it is one that never answers.

## 6. R6-B — measurement, after correctness

Same discipline as the RW-B cells: `build-release`, interleaved arms in one
sitting, per-arm processes, fresh server and data file per invocation,
per-rep spreads before any median, rows in = rows out.

| cell | measures |
|---|---|
| B1 | **The cost of two phases**: a two-owner transaction against the same work as two separate one-owner transactions. D7 predicts ~2×; report what it is |
| B2 | **Width**: participants 2, 3, 4, and past four where the device's overlap curve declines. D7 predicts depth 2 and width up to the device's limit |
| B3 | **The fast path is free**: one-owner transactions on an R6 build against the pre-R6 tag. Any loss here is D1's gate failing |
| B4 | **The residue, third era**: what `cross_core_write_refusals` still counts. The classes that remain are the ones nothing in the roadmap converts, and naming them is this cell's product |
| B5 | **Scenario benches** (`results-scenario1-vs-pg.md` and its two siblings): what fraction of a realistic workload takes the two-phase path. This is the number that sizes future optimisation, and it is the one this order deliberately did not gate on |

## 7. Deliverable

Design decisions D1–D7 ratified or amended by the operator, in a spec under
`docs/spec/`. Task rows landing with their worktree and review cited.
Results files in `bench/`, named by `git describe --tags`, in the format the
v2.3.0 cells use: header with host including physical-versus-logical cores
and SMT state, filesystem, build flags, binary provenance; per-rep tables;
findings tagged measured or source-read with sites; and what the run does
not measure.

Open items handed on rather than fixed: the 992-byte reply cap, the
~11 ms periodic stall (`results-knob-sweep-cell2` §5, unattributed), the
92–98% unaccounted reactor wall clock, and PW1c-6c's three residues.
