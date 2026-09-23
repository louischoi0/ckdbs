# Cross-Owner Transactions — retired at AT-S6 (2026-09-23)

**There is no cross-owner transaction in this engine.** A transaction is
one core's, whole: every statement of it — read and write — runs on the
core its session is on, so no other core holds a half of it to prepare, to
decide, or to be in doubt about.

This file described the two-phase commit protocol that existed while that
was not true. It is kept as the record of what the protocol was and why it
went, in the shape
`docs/spec/create-pattern-user-defined-patterns-v1.md` set for a withdrawn
subject: **nothing below is a live contract**, and a citation to one of
its old sections resolves against `git show c63e49f:docs/spec/cross-owner-txn.md`.

## What it was

A statement whose relation another core owned was **shipped** there. The
owner opened a context on a session of its own — a *participant* — and the
sending core became the *coordinator*. At `COMMIT` the coordinator ran D4's
two phases over its participants: a `TXN_PREPARE` each participant made
durable in its own stream and answered, the coordinator's own decision
record as the durability point, then a decide each participant applied. A
participant that had prepared and heard nothing was **in doubt** and asked
(R6-5); a participant found at the next mount with a prepare and no
decision was resolved from the coordinator's stream.

Six ring kinds carried it (33–38), two more carried the statement (28, 29),
and three files implemented it: `statement_ship_service`,
`shipped_statement_executor` and `txn_2pc_service`.

## Why it went, in the order the reasons arrived

1. **AT-S5 (2026-09-09) moved every write.** A write runs where the session
   is, through the one frame table AM-S2 step 3 made the instance's, so the
   three write ship forks retired. What was left shipping was a read.
2. **A read that ships cannot see its own transaction's write.** The two
   halves ran under two transaction ids and visibility is by id, so a
   transaction that wrote a peer-owned relation and read it back in the
   same statement sequence was answered zero rows — a quiet wrong answer,
   found by AT-S6's survey and reproduced on the two-core rig. The fork's
   own comment argued for shipping on exactly that ground (*"only the
   peer's own transaction can show it"*), which had become the argument for
   the defect.
3. **The operator took AT-0 item 4 / D18's first shape on 2026-09-23**: a
   read stops shipping. It closes the defect — both halves are one
   transaction on one core — and it leaves the protocol with no traffic at
   all, which is what AT-R6 assumed when it said *"2PC no longer exists
   inside a single node"*.

**What was already true before the stage ran**: no participant could write
a `TXN_PREPARE` record. SA-T0 made a participant that wrote nothing skip
the record, and after AT-S5 no participant could write, so the durable half
of the protocol had been unreachable since that stage. The stage's
done-conditions — an undecided prepare cannot be constructed, recovery's
undecided-prepare arm is unreachable — held before a line was written.

## Where its parts live now

| what it did | what does it |
|---|---|
| carry a statement to the relation's owner | nothing — the statement runs here (`crosscore.md` §6) |
| let a transaction read a peer-owned relation | a local walk; every core faults every page |
| one instant on every core under RR (AN-S3) | one core, one view (`txn.md` §5) |
| bound a wait on a row held by a prepared transaction | the lock family's fault net, `lock_wait_fault_net_ms` |
| end a participant's context at the coordinator's decide | nothing to end |
| resolve an in-doubt participant at mount | nothing to resolve; `wal.md`'s mount scan carries what replaced it |

## What is not retired with it

**The remote-step protocol is a different mechanism and stays.** A fan-in
over a split relation and a two-step join open stages on other cores
(`kStep*`, `crosscore.md` §4a); a stage is a read, not a transaction half.
Whether it survives AT is AT-0 item 4's other half, undecided.
