# Work order EI - atomic commit across services: whether this engine is a two-phase participant, explored before anything is built

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`service-txn-orders` (branch `worktree-service-txn-orders`) from
`72580aa04` (`v3.5.1-6-g72580aa04`) on the operator's word of 2026-11-28
(`raft-marks-2026-11-28.md` §6, §7). **Exploratory**: EI-S0 is a census on
paper and decides whether anything of §2 is built; no stage is started and
EI-S0 waits for the operator's word and for the marks on §4's items. Every
`[source-read]` claim with a `path:line` was read by CLA at `72580aa04` in
the session that wrote this order.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/txn.md` §1, §5, §6, §8; `docs/spec/wal.md` §3, §5.2,
§12; `instructions/v3.0.0/workorder-bp-per-core-residue.md` (BP-R4);
`instructions/v3.0.0/ar0-architecture-revision.md`;
`docs/conceptnotes/cn5-commit-outcome-and-idempotency.md` (CN-5); EJ, EK
and EL, written the same day.

## 0. What EI is

`[operator]` The operator's premise of 2026-11-28: *"kdbs is a DB optimized
for modern software in a microservice environment"*, and from CLA's survey
of the transaction under that premise, *"write milestones for C1, C2, C3
and C8; depending on the case, write them as exploratory or
proof-of-concept milestones"*. C1 is the survey's first constraint: **no
distributed transaction - no two-phase commit, no XA**.

`[source-read]` The premise is the operator's of that day and is in no
document of the tree: `CLAUDE.md` ("What This Project Is") states the
purpose as OLTP for financial systems, settled as CF-Q6 (b), and no file
under `docs/`, `manual/` or `CLAUDE.md` carries the word "microservice"
(`grep -rni microservice`, 0 lines, at `72580aa04`). **It entered the
tree the same day by another session**: `origin/main` at `3aa8a141e`,
merged into this branch at `843484413`, carries a working rule in
`CLAUDE.md` - "a feature is built and improved for the concept KDS is: a
high-performance OLTP engine that guarantees consistency, optimized for
modern software environments such as a microservice architecture", with
"'What This Project Is' bounds the scope". EI rests on the word and that
rule, and moves no purpose statement (EI-Q5).

`[design]` A service that owns its data and must change it together with
another service's has three shapes in the field: a coordinator's two-phase
commit over participants that can **prepare**; a saga of local commits
with compensations; and a local commit that carries a message out with it
(an outbox). The second and third need from a database only what EL (a
commit whose outcome a client can learn, run at most once) and EJ (a feed
of committed changes) explore. **EI asks the one question those two do not
answer: should a transaction of this engine be able to stop between
"cannot fail any more" and "decided", and survive a restart there.** EI's
null hypothesis is **no** (§2, candidate N), and S0 is written to be able
to refute it.

## 1. The tree as read `[source-read]`

- **A prepare existed here and was retired.** `TXN_PREPARE` (27) was "a
  two-phase participant's prepare" of the per-core engine; no build that
  writes superblock 22 appends it, analysis refuses a log carrying one
  `Corruption`, and its number stays assigned so no later kind reuses it
  (`include/kds/wal/record.hpp:164-167`, `:273-278`;
  `src/wal/analysis.cpp:77-82`; `docs/spec/wal.md:51`, `:98`, `:210`;
  BP-R4, `instructions/v3.0.0/workorder-bp-per-core-residue.md:469-478`).
  It was a protocol among the cores of one instance, never a client's:
  AR0 calls its knob `in_doubt_ceiling_ms` (200 ms) "dead once 2PC goes"
  (`instructions/v3.0.0/ar0-architecture-revision.md:273`), and `txn.md`
  §1 records "Prepared contexts - gone at AT-S6 with the protocol that
  prepared one" (`docs/spec/txn.md:87`).
- **Recovery knows three outcomes and no fourth.** Analysis classifies a
  transaction `kWinner`, `kAborted` or `kLoser`
  (`include/kds/wal/analysis.hpp:62-68`), and every loser is rolled back at
  mount before the database is served (`docs/spec/txn.md:1426-1434`).
- **Visibility rests on that undo.** Below the read view's floor an id
  answers "committed" with no lookup, "*only* because an aborted
  transaction's page changes are physically undone ... and, across a
  crash, by recovery's undo phase ... which rolls back every loser before
  the database is served. That is the load-bearing assumption of the
  whole design"; the floor is an id below which every transaction is
  resolved and no id will be issued again (`docs/spec/txn.md:500`,
  `:514-523`).
- **No lock outlives a crash.** "The lock family holds no bit on the page
  (AO-R3: no persisted lock bit, no page bit, no superblock field; a crash
  releases every lock because the loser is rolled back at mount)"
  (`docs/spec/txn.md:825-827`).
- **A transaction is short by scope, and bounded by a sweep with no
  off-switch.** "A transaction completes within 10 seconds; one that
  exceeds 60 seconds may be aborted", the scope from which "every
  retention-sized quantity is derived" (`docs/spec/txn.md:11-18`); the
  sweep rolls back an explicit transaction at `txn_lifetime_ceiling_ms`
  and `0` is refused at startup (`docs/spec/txn.md:20-25`).
- **A transaction is one core's and one instance's.** "No transaction
  touches more than one core" (`docs/spec/txn.md:3`).
- **A commit that fails aborts**, and `TXN_ABORT` is appended with no
  durability wait (`docs/spec/txn.md:1377-1402`); the same passage states
  the one place the engine does not know an outcome it reports (EL's).

## 2. The candidates `[design]`

None is designed past what S0 needs to judge it.

- **N - no prepared state.** A transaction of this engine is decided where
  it commits, as now. Atomicity across services is the client's saga or
  outbox, and the engine's part is EL's token and EJ's feed. Costs
  nothing; leaves a coordinator-based client (XA, a transaction manager)
  unserved.
- **P - a prepared state a client names.** `PREPARE` makes the
  transaction's writes and its promise to commit durable, ends the
  session's hold on it, and leaves it **in doubt** until a `COMMIT
  PREPARED` or `ROLLBACK PREPARED` naming it arrives on any session, across
  a restart. What it would have to add, each an item of S0's census:
  1. a fourth recovery outcome, **in doubt**: neither undone nor a winner,
     its undo kept, its id still in flight after the mount. **It
     contradicts the assumption §1 quotes**: an id in doubt after a mount
     is one whose changes were not undone, and nothing a row's header
     carries says "undecided" - so either the floor is held below that id
     for as long as it stays in doubt, with the window and its
     reclamation behind it, or every reader sees its rows (EI-R1);
  2. **its claims rebuilt at mount**, which follows only from item 1.
     With its id in flight again, a row it wrote is claimed by its
     header, and a unique index's check finds that row busy - the key
     unit itself is given back at the append
     (`docs/spec/index.md:1049-1053`). An assertion's reservation is
     logged (`ASSERT_RESERVE`, `docs/spec/wal.md:182`) and today given
     back at the mount for a loser; for a transaction in doubt the mount
     must keep it. What the lock table alone held, and nothing rebuilds:
     a declared range, a foreign key's `S` on a parent row, the relation
     `IX` a DDL must meet;
  3. **the read horizon held with no session behind it**, so a prepared
     transaction nobody decides stops the purges for the instance;
  4. **a rule against the lifetime ceiling**: a prepared transaction may
     not be rolled back by the sweep (it promised), and may not be exempt
     without bound (item 3). The two sentences of `txn.md` §1 it meets are
     "the 60 seconds are the mechanism" and "a ceiling has no off-switch";
  5. a new record kind under a new number, a superblock version, and a
     name space for prepared transactions that outlives sessions.
- **B - a bounded prepare.** P with a **presumed abort at a bound**: a
  prepared transaction undecided past the bound is rolled back by the
  engine and the coordinator's later `COMMIT PREPARED` is refused by name.
  It keeps the ceiling's meaning and breaks two-phase commit's (a
  coordinator that decided "commit" and is late finds a participant that
  aborted), so it is a protocol of its own and no XA participant.
  `[quiet-wrong]` if spelled as P.

## 3. Rules

- **EI-R1. A transaction in doubt is never undone and never visible**
  `[quiet-wrong]`: a mount that rolls one back, or a reader that sees its
  rows, answers a state no coordinator decided.
- **EI-R2. A claim a prepared transaction held is held after the mount, or
  the prepare is refused** `[quiet-wrong]`: a unit the lock table alone
  carried and the mount did not rebuild lets a second writer insert into
  a window the prepared transaction declared, delete the parent its child
  row names, or drop its relation - and a reservation the mount gave back
  lets one take the assertion's room - so the later `COMMIT PREPARED`
  commits a state the engine's own invariants refuse.
- **EI-R3. The lifetime ceiling keeps its sentence or the spec changes it
  in words**: no path makes a transaction outlive the sweep without the
  operator's mark on EI-Q3.
- **EI-R4. The retired kind stays retired** (BP-R4): `TXN_PREPARE` (27) is
  refused as now; a new prepare is a new kind and a superblock version.
- **EI-R5. Nothing of §2 is spelled before it is built**: no stage
  accepts `PREPARE TRANSACTION` or a sibling until the state behind it
  exists, and no frame, bit, keyword or code is reserved by this order.
  What the engine answers such a statement today was not read.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EI-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EI-Q0 | Is EI opened, and how far? | (a) S0 only, on paper, its verdict put to the operator; (b) S0 and S1; (c) not opened: N recorded in `txn.md` §9 as a decision and the order abandoned | (a) |
| EI-Q1 | What S0 judges the candidates by | (a) the engine's stated scope - a 10 s transaction, a 60 s ceiling, invariants enforced on every write - so a candidate that needs an exception to any of them carries the exception's cost on its row; (b) compatibility with XA clients first | (a) |
| EI-Q2 | The null hypothesis | (a) N stands unless S0 names a service shape that neither a saga over EL's token nor an outbox over EJ's feed can make atomic; (b) P is the target and S0 only sizes it | (a) |
| EI-Q3 | A prepared transaction against the lifetime ceiling, if P or B is ever taken | (a) not decided by this order: S0 lays out exempt, bounded and refused with what each breaks; (b) decided now | (a) |
| EI-Q4 | The proof of concept S1, if S0's verdict admits one | (a) in the simulation harness only: one in-doubt transaction carried across a crash, EI-R1 and EI-R2 as its oracle, no client surface; (b) with a KWP surface | (a) |
| EI-Q5 | The operator's premise and the purpose statement | (a) as `CLAUDE.md` stands at `3aa8a141e`: the concept is a working rule, "What This Project Is" bounds the scope and is not touched; (b) the purpose statement itself gains the service environment, in the operator's wording | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EI-S0** | The census, on paper: for P and B, every item of §2's list read against the tree with a `path:line` - which claims the page already carries and which only the lock table does, what the mount would have to rebuild, what the horizon and the ceiling say; and for N, the service shapes named in EI-Q2 worked through over EL's and EJ's mechanisms as written | a verdict among N, P and B with the cost of each beside it; the operator's word |
| **EI-S1** | (gated on S0 and EI-Q4) A proof of concept in `sim/`: an in-doubt outcome in analysis, the claims of EI-R2 rebuilt, a crash at every op of a prepared transaction's life | the sim corpus and the suite; a list of what the proof did not carry |
| **EI-S2** | The close: `txn.md` §9 states the verdict as a decision; `known-gaps.md` if anything was built and left partial | the operator's word on landing |

## 6. What EI does not do

- No coordinator role: this engine never drives another's commit.
- No transaction over two instances of this engine, and no change to "a
  transaction is one core's".
- No XA wire surface, no JTA or driver work.
- Nothing of EL's token or EJ's feed; EI reads both as written.
- No reuse of record kind 27.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `service-txn-orders` from `72580aa04`, on the operator's word. No stage
is opened and no code is written. The survey the order answers was given
in conversation and is in no file; its item C1 is §0's first paragraph.

**The opening review** (`critics-developer`, the same day, at `5d8dd1ddf`)
found §2's P wrong in two places, each corrected above: the first writing
said a prepared transaction's rows keep their first-updater verdict "by
itself", where the read view's floor answers every old id as committed
(`txn.md:500`, `:514-523`), so item 1 now names the assumption P
contradicts; and it listed a unique index's key unit and an assertion's
reservation as the lock table's alone, where the first is released at the
append and the second is logged. EI-R5 claimed a refusal by name that was
not read; reworded.
