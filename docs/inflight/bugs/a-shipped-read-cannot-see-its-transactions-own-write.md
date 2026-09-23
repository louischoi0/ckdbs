# A transaction cannot read its own uncommitted write to a peer-owned relation

**Found** 2026-09-23 by AT-S6's survey, on `at-s6-2pc-retired` at
`c63e49f`. **Not fixed** — the fix is an operator decision, AT-0 item 4 /
D18: whether a read still ships at all. Verified by a cell, not by reading:
`tests/shipped_read_own_write_test.cpp`, landed **disabled** because it is
the cell the fix owes.

**Cost: a quiet wrong answer, inside one transaction.** A client writes a
row, reads the relation back in the same transaction, and is told the row
is not there — no error, no refusal, the empty answer a relation with no
matching row gives. It then commits, reads again, and the row is there.

## The wrong behaviour

The two halves of one transaction run in two places, under two transaction
ids, and visibility is by id.

- **The write runs where the session is** since AT-S5: `HandleInsert`'s
  ship fork is gone, so an `INSERT` into a relation core 1 owns is written
  by core 0's transaction, into pages every core reads through the one
  frame table (AM-S2 step 3). The row's header names **core 0's**
  transaction.
- **The read still ships**, which AT-S5 left untouched by name (D18, AT-0
  item 4): `HandleSelect`'s fork tests `MayShip(session) ||
  MayEnrolShip(session)` and carries the statement to the owner.
- **The participant runs it under its own transaction.** `EnrolFor` opens
  a local transaction on core 1 at the coordinator's isolation level; its
  id is core 1's. `CheckVisibility`'s `own_trx_id` arm matches the
  *reader's* id, which is not the writer's, so the uncommitted row is
  invisible and the read answers zero rows.

Nothing refuses, because from each half's own side nothing is wrong.

**The premise that expired.** `HandleSelect`'s fork says why a read must
ship inside a transaction: *"a transaction that wrote a row on a peer and
then reads it back must see its own uncommitted write, and only the peer's
own transaction can show it."* That was true while the write shipped to
the same participant — both halves were that participant's transaction.
AT-S5 moved the write and left the sentence, and the sentence is now the
argument for the defect.

## Reproduction

`tests/shipped_read_own_write_test.cpp`, `DISABLED_` on the two-core rig:
a relation placed on core 1, then on core 0 `BEGIN`, `INSERT INTO r1
VALUES (41)`, `SELECT v FROM r1`, `COMMIT`, `SELECT v FROM r1`. Measured,
not predicted: the insert answers `INSERTED`, the in-transaction read
answers a header and no rows, the commit succeeds, and the read after it
answers `41`.

## The fix is a decision, and there are three shapes

1. **A read stops shipping** (AT-0 item 4 / D18). The read runs where the
   session is, as the write does, and sees its own row because it is the
   same transaction. It also takes 2PC's last traffic with it — a
   participant is enrolled by a shipped read and by nothing else since
   AT-S5 — which is what AT-R6 assumes when it says a transaction has no
   participant on another core.
2. **A read of a relation this transaction has written runs locally**, and
   every other read still ships. Narrower, and it needs the transaction to
   remember which relations it has written.
3. **The participant reads under the coordinator's id.** The wire already
   carries `coordinator_txn` (AO-S4b) and the snapshot (AN-S3); the
   participant's check view would take the coordinator as its own writer,
   the way the retired foreign-key probe's check view did (AO-S5(b) C1).
   Keeps shipping and keeps 2PC.

(1) closes it and makes AT-S6 buildable as written; (2) and (3) keep the
crossing alive and leave AT-R6's premise false.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-0 item 4 (the
decision) and AT-S6 (the stage that cannot proceed as written until it is
taken). `docs/spec/cross-owner-txn.md` and `docs/spec/crosscore.md` §4a
are the sections that state the shipped read's contract.
