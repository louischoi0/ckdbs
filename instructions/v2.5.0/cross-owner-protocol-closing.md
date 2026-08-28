# Work order R6-R — the read half, and the close of 2PC

Drafted 2026-08-28 against `main` at `a02a666` (`v2.2.1-108-ga02a666`).

The third and intended last order in the cross-owner line, after
`instructions/v2.4.0/2pc.md` (R6-0…R6-2, D1–D7) and
`instructions/v2.5.0/cross-owner-protocol.md` (RP0…RP8). It takes what
R6-P deferred, plus the one blocker RP8 found by measuring.

## 0. Why a third order, and whether it is the last

R6-P closed every row it opened. The protocol is built, the kill −9 matrix
is 36/36, and D7's prediction survived measurement at 1.479× within one
instance. Then B5 went to measure what fraction of a realistic workload
takes the two-phase path and found the fraction is **zero**, for a reason
outside the protocol:

> `CheckReadAffinity` refuses every cross-core read unconditionally, so a
> transaction that reads a peer-owned relation before writing never reaches
> a cross-owner commit.

Read at the source, the refusal is narrower than "unconditional" and that
narrowness is the whole opening. The write paths at
`command_dispatcher.cpp:4532`, `:6516` and `:7409` each test
`MayShip(session) || MayEnrolShip(session)`. **The read path at `:6058`
tests `MayShip(session)` alone**, and `MayShip` requires
`!session.in_explicit_txn()` (`:3856`). So a foreign read inside an
explicit transaction cannot ship, falls through to `CheckReadAffinity`, and
is refused — while the identical read outside a transaction ships fine.

R6-8 built the write half of enrolment and stopped there deliberately: RP6
recorded that a cross-core read inside a transaction keeps its behaviour
*"because the D3 watermark that would make it meaningful is not built."*
That was the correct call then, because nothing measured the cost of the
gap. RP8 measured it. The gap is the whole product.

**Can this be the last 2PC order?** Yes, with one boundary stated plainly.
After this milestone, cross-owner transactions are complete **at relation
granularity**: both halves cross, isolation is honoured, the spec exists.
What remains after is not 2PC:

- **Blueprint §11's R6 — multi-*range* transactions** — is gated on this
  protocol and reuses it unchanged. Under ranges a participant is still a
  core (R6-2 keys enrolment on `(src_core, session_id)`, never a relation);
  only R6-8's discovery changes, from a field read to RD3's resolver. One
  row, and it belongs to R3's line, not this one.
- **Core-count change** — direction settled 2026-08-28 (`78c86e9`), its own
  milestone, and R6-4's resolver already survives it because it reads files
  rather than live cores.

So this order closes 2PC. It does not close everything 2PC will be asked to
carry.

## 1. Scope

**In.** D3's per-participant watermark, built. The read half of enrolment.
D5's 200 ms ceiling swept and settled. R6-9's spec pass. The two
measurement debts RP8 flagged.

**Out.** Range granularity of any kind. Core-count change. `CheckReadAffinity`'s
non-transactional refusals, which RP8 §8 already classified as CP3's third
era and which stay refused. Any new protocol leg — this order builds no
fourth phase.

## 2. What the previous orders left, itemised

Each of these is a sentence some earlier row wrote about a later one. They
are gathered here so none is discovered late.

| Source | Owed |
|---|---|
| RP6 (`7eaa14a`) | Reads inside a transaction keep the old behaviour; the watermark that would make shipping them meaningful **is not built**. Named as R6-9's, but it is code, not doc |
| RP3 (`056cf9b`) | The 200 ms in-doubt ceiling is a **proposal**, derived from this host's ~0.94 ms sync and 21 µs ring hop. The sweep needed R6-8's live path, which is RP8 — and RP8's cells were B1–B5, so **the sweep fell through the gap between the two rows** and is neither run nor recorded as not-run |
| RP7 (`56b2fe7`) | HP3 reported, not confirmed. The sim corpus cannot mount a prepared transaction at all, which bounds what its green means |
| RP2 (`814c568`) | `wal.md` §11-3's revision — checkpoint's redo start floors at the oldest live prepare — is in the code and owed to the spec |
| R6-3 (`63a0f43`) | A participant that fails a decided COMMIT locally is counted and logged with no runtime repair. R6-4 made such transactions resolvable at mount; whether that fully discharges the item needs stating, not assuming |
| RP8 (`a02a666`) | `bench/docs/README.md` entries for the two new drivers (`ck-tester` rule 7). No per-leg server-side timer exists for prepare/decide/ack, which is why §5c's two candidate explanations could not be separated |

## 3. The conclusions this milestone must produce

**CR1 — what a cross-owner read costs, and whether the watermark is on the
hot path.** D3's ratified form has the coordinator carry a per-participant
watermark and the participant read at or before it. RC skips it entirely
(D3's `[OPEN]`, ratified yes), and RC is the default isolation level, so
**the common path must pay nothing**. Reach CR1 by naming what an RC
cross-owner read executes that an RC autocommit foreign read does not. If
the answer is "one branch on a field already read", say so with the site;
if it is more, that is a finding and it changes the shape.

**CR2 — the watermark's staleness contract, stated as a product property.**
The coordinator's watermark for a core is what it last *observed*, which is
not what that core has last *committed*. A participant reading at a stale
watermark sees less than it could. Reach CR2 by stating how stale the
watermark can be in the worst case, in terms of what advances it, and
whether a cross-owner RR transaction can therefore fail to see its own
earlier write on another participant. **If it can, that is a correctness
bug and not a staleness property** — the distinction is the conclusion.

**CR3 — the ceiling's second axis.** RP2's review made the in-doubt ceiling
bound two things, not one. A prepared transaction floors checkpoint's redo
start (`ActiveTransactions::OldestPreparedLsn`), so the ceiling limits
**how much log cannot be truncated**, not only how long a writer waits.
Reach CR3 by pricing both at the swept values, and pick the ceiling against
whichever binds first. A ceiling chosen on latency alone is chosen on half
the evidence.

**CR4 — what B5 reads once reads cross.** B5 is the only R6-B cell that
returned a blocker instead of a number. Re-run it. The fraction of a
realistic workload on the two-phase path is this line's actual product
metric, and it has never been measured.

## 4. Hypotheses — each with its falsifier

**HR1 — the RC read path costs nothing.** An RC cross-owner read costs what
an RC autocommit foreign read costs, because D3's `[OPEN]` removed the
watermark from it.
*Falsifier*: any per-read work on the RC path beyond the enrolment branch
the write half already pays. This is D3's `[OPEN]` earning its ratification
or failing to.

**HR2 — the watermark needs no new message.** The coordinator already
learns a participant's commit position from the prepare and decide legs it
exchanges; the watermark rides existing traffic.
*Falsifier*: RR correctness requires a watermark fresher than any existing
leg delivers, which means a new exchange and a fourth kind — and §1 puts a
fourth leg out of scope, so this stops the row and reports.

**HR3 — the read half needs no new protocol, only a wider gate.** Extending
`MayEnrolShip` to the read path at `:6058` plus the watermark is the whole
change; prepare, decide, resolve and recovery are untouched.
*Falsifier*: any edit inside `Txn2pcService`, `PreparedResolver`, or the
crash-point set. If the read half needs the protocol to change, the
protocol was incomplete and this is no longer the last order.

**HR4 — 200 ms survives the sweep.** The proposed ceiling holds against
both axes CR3 names.
*Falsifier*: either axis prefers a materially different value. The
prediction is recorded so that moving it is a finding rather than a tuning.

**HR5 — the fast path is still free.** Everything B3 measured at
[0.929, 1.031] stays there after the read half lands.
*Falsifier*: a re-run outside that band. HP1 has held through six rows; the
row that breaks it should be visible as the one that did.

## 5. Task series — RR rows

| # | Task | Gate |
|---|---|---|
| **RR0** | **The watermark** (D3, ratified `2a3969e`). Per-participant, carried on existing legs (HR2), applied on the participant's read. **RR-skips-it wired as the ratified `[OPEN]` answer**, so the default level pays nothing (HR1). CR2's staleness contract written before the code, not after | none |
| **RR1** | **The read half.** `:6058` tests `MayEnrolShip` alongside `MayShip`, matching the three write sites. `SoleForeignOwner`'s two-foreign-owner refusal (`:4130-4132`) **stays** — a multi-owner *statement* is not this order's business, only a multi-owner *transaction*. Nothing in `Txn2pcService` changes (HR3) | RR0 |
| **RR2** | **The ceiling sweep**, and **CR3**. `in_doubt_ceiling_ms` swept against both axes: writer stall, and log retained via the checkpoint floor. The value lands as measured or the proposal is re-affirmed with its number | RR1 |
| **RR3** | **B5 re-run**, and **CR4**. The cell RP8 could not reach. Plus B3 re-run for HR5, same sitting, same shape | RR1 |
| **RR4** | **R6-9 — the spec.** `docs/spec/` gains the cross-owner protocol; `crosscore.md` §6's three-refusal list narrows to CP3's third era; **`wal.md` §11-3 revised** (RP2's checkpoint floor); `client-manual.md` gains D3's per-level weakening — **RR gets a per-core consistent snapshot, RC gets no cross-core promise at all** — and D5's `UNKNOWN_OUTCOME` contract in shipped statements' existing words | RR1, RR2, **CR2** |
| **RR5** | **The two RP8 debts.** `bench/docs/README.md` entries for both drivers (rule 7). The absent per-leg timer recorded where the subsystem owns it, since it is the second time an instrument gap blocked an attribution (M3's `shipped_statement_us` was the first) | RR3 |

## 6. Correctness — ahead of any number

- **RP7's matrix re-run whole**, 12 cells × 3 passes, on the tree with the
  read half in it. A read that crosses inside a transaction is a new way to
  become a participant, so every crash point is newly reachable by a new
  path.
- **A cross-owner RR transaction sees a per-core consistent snapshot**, and
  a test says what that means concretely rather than asserting the phrase.
- **CR2's own answer tested**: whether a transaction sees its own earlier
  write on another participant. Whichever way it lands, the test pins it.
- **`cores = 1` unchanged.** Sixth consecutive row asserting this.
- **HP3 stays reported, not confirmed**, unless this order builds something
  that lets sim mount a prepared transaction — and it should not, because
  §1 puts new protocol out of scope. **The bound RP7 recorded carries
  forward unweakened.**
- Full suite and `scripts/sim.sh`, baseline and re-read **in one sitting**,
  which sitting named.

## 7. Measurement

`build-release`, interleaved arms in one sitting, per-arm processes, fresh
server and data file per invocation, per-rep spreads before any median,
rows in = rows out. Overhead A/B still suspended (2026-08-24). Anything not
run is reported as **not run** — RP8's ceiling sweep fell through a gap
between two rows precisely because no one wrote that sentence about it.

| cell | measures | reads against |
|---|---|---|
| **R1** | RC cross-owner read against RC autocommit foreign read | **HR1**, CR1 |
| **R2** | RR cross-owner read against the same at RC | D3's weakening, priced |
| **R3** | `in_doubt_ceiling_ms` swept: writer stall | HR4, CR3's first axis |
| **R4** | Same sweep: log retained, via checkpoint's floor | **CR3's second axis** — the one RP2's review added |
| **R5** | **B5, re-run** — the two-phase fraction of the scenario benches | **CR4**. This line's product metric |
| **R6** | B3 re-run, pre-R6 arm built the same sitting | **HR5** |

**Host is not fixed.** RP8 ran on 4 physical / 8 logical cores with SMT on;
earlier cells ran elsewhere. Every result file states its own host and no
number is compared across two of them. Where a value is *derived* from host
characteristics — D5's ceiling from the ~0.94 ms sync and 21 µs hop — the
derivation is restated on the host that swept it, or the sweep's number
replaces it.

Results to `bench/v2.5.0/`, named by `git describe --tags`, in the format
RP8 used: host with physical-versus-logical and SMT state, filesystem,
build flags, binary provenance, per-rep tables, findings tagged
**measured** or **source-read** with sites, a §"what this run does not
measure", and the PostgreSQL section stating why no honest twin exists —
RP8 §10's reasoning holds and should be cited, not re-derived.

## 8. Improvement — what this leaves better

- **The line's own product metric measured for the first time.** Every
  order so far measured the protocol's cost. None measured whether anything
  uses it. B5's re-run is that number.
- **The read/write asymmetry closed.** Three write sites test two gates;
  one read site tests one. That asymmetry was invisible until B5 tried to
  drive a realistic workload through it, and it is the kind that hides
  because each half looks complete alone.
- **A ceiling chosen on both its axes.** RP2's review found the second axis
  after RP3 had already proposed a value against the first. CR3 makes the
  final value answer to both.
- **The spec written once, at the end**, when what it describes is
  confirmed and implemented — the reason R6-9 was held back through two
  orders rather than drafted early and amended six times.

## 9. Deliverable

- D3's watermark built, RC skipping it, CR2's staleness contract written
  before the code.
- `:6058` gated like the three write sites; `Txn2pcService` untouched
  (HR3 asserted by diff).
- The ceiling settled against both axes, or the proposal re-affirmed with
  its number.
- CR1–CR4 written into `workplan-cross-owner-txn.md`.
- RP7's matrix re-run on the tree with the read half in it.
- R1–R6 under `bench/v2.5.0/`.
- **`docs/spec/` carries the cross-owner protocol**, and `crosscore.md`,
  `wal.md` §11-3 and `client-manual.md` are amended.
- Each row landing with its worktree name and short commit **inside the
  sentence that makes the claim**, and its `critics-developer` pass with
  findings applied or rejected by name.

**On closing.** When RR5 lands, the cross-owner line is closed at relation
granularity and this order is the last of it. What inherits the protocol is
blueprint §11's R6 — multi-range transactions — which is R3's line, needs
RD3's resolver, and changes exactly one row of this one. That inheritance
should be recorded in the blueprint when this closes, so the next reader
finds the gate already satisfied rather than re-deriving whether 2PC exists.

Open items handed on rather than fixed: R3's D1, D2, D4, D6; core-count
change; RD6's `desc_page_id` finding; the 992-byte reply cap against rule 1;
the ~11 ms periodic stall; RW-C1; and the two cws issues.
