# Work order CH — a tuple key change in one transaction: an exploratory concept check

Written 2026-10-21 on `worktree-ch-tuple-key-update` from `9fdf3351`
(`v3.4.0`), on the operator's words, recorded in English
(`raft-marks-2026-10-21.md` §6):

- **W0**: *"So in the end, is the milestone about whether a pk update can
  be written as one transaction?"* - asked of CG
  (`workorder-cg-on-conflict-do-update.md`).
- **CLA's answer to W0**: no. CG is an upsert: the key stays, and K1 and
  K2 stand (CG-R2). Changing a tuple key's value inside one transaction is
  a separate design question, because it makes a new exception to K1 and
  K2. CLA offered (a) to widen CG, or (b) to write a separate exploratory
  milestone, and proposed (b).
- **W1**: *"$!b - write it as a separate milestone and push it, documents
  only."*

**Written 2026-10-21; not opened.** CH-Q0..Q7 each carry CLA's proposal
beside them, and none is marked. **Exploratory since W1** (§3): CH-S0
checks the concept for each class of change (§0) before anything is
built, and a class that fails the check is recorded, not built.

**Claim tags**: `[operator]` for W0-W1; `[source-read]` with `path:line`
at `9fdf3351`; `[design]` for reasoning about code not yet written;
`[quiet-wrong]` where a wrong choice turns a correct answer into a wrong
one with no error.

**The letter.** `[source-read]` CG is the last work order under
`instructions/v3.2.0/` on `origin/main` at `9fdf3351`, no worktree in
`git worktree list` names a `CH`, and no commit subject on `origin/main`
begins with the token. CLA's choice.

**CH against other orders.**
- CH would amend K2 (`docs/rules/keystoneid-invariant.md`), and in
  classes C2 and C3 K1 as well.
- It touches `PURGE` (BH, `heap-and-tuple.md` §4.1c) only as the
  existing precedent for a K1 exception, and leaves it as it is.
- CG-Q3 (b), reviving one's own delete-mark, is C2 restricted to an upsert
  of the same row. If CH-S2 lands, CG-Q3 reads its census from CH-S0.
- K5's offline re-key stays reserved and is not this order.
- Not part of AR0 §8's chain.

---

## 0. What CH is

`UPDATE t SET <tuple key> = <expr> WHERE ...` (or a multi-row form)
changes a tuple key's value as one statement inside one transaction.
CH-S0 checks the concept separately for three classes of target key:

- **C1 - a key never bound.** No committed tuple ever held the target.
  `[design]` This needs no K1 exception: it is the move a client can
  already write as `INSERT` the new key, `DELETE` the old, in one
  transaction, inferred from K1 and not run. The engine would compose
  that move from one statement. It amends K2 only.
- **C2 - a key whose committed tuple is delete-marked.** This covers a
  key this transaction deleted, and a key deleted earlier whose delete
  is resolved for every reader. Rebinding it inside the transaction is a
  new K1 exception beside `PURGE`'s.
- **C3 - a permutation of live keys in one statement.** A swap (A <-> B),
  a renumbering (`SET id = id + 1`), or any move whose target is live
  until another row of the same statement moves off it. This needs C2,
  plus a uniqueness check at the end of the statement instead of per row.

CH-S0 ends each class in build, narrow or drop. The classes can land
separately: C1 does not wait on C2.

---

## 1. The tree as read `[source-read]` at `9fdf3351`

**Why the key is fixed today.**
- **K2 is a compile-time refusal.** `exec::CompileAssignments` refuses a
  `SET` naming the tuple key with `Unsupported` at its byte, before any
  storage is touched (`src/exec/step_compiler.cpp:1920-1925`;
  `keystoneid-invariant.md` K2, K-M3). K-M3 gives the reason: *"the id
  names the tuple in the clustered tree, every index and Cabin entry, and
  every trail"*.
- **K1 binds a key to at most one committed tuple for the relation's
  life**, except across a `PURGE` that freed it (`heap-and-tuple.md` §4,
  §4.1c). `PURGE` runs only in autocommit, only once every reader sees
  the delete (`ResolvedForEveryReader`), and only on a relation with no
  Transition.

**Every consumer that stores or keys on a tuple key** (the census CH-S0
asks again):
- **The clustered tree.** A row lives at the slot its key sorts to, in
  the leaf whose range covers the key (invariants 3 and 4), and its undo
  chain is re-found by that key (`txn.md` §3.3) and holds one logical
  tuple (`txn.md` §2). `min_key` is immutable (invariant 2), but that
  alone does not forbid an in-place rewrite to a key that sorts to the
  same slot; what does is that an older reader must still find the old
  key's version at the old key. So a key change is a placement at
  another slot, never a rewrite of the Keystone word in place.
- **Secondary indexes.** Every entry carries the tuple key, and the whole
  entry is its sort key (`index.md` IX4b). An entry is never removed, and
  every probe re-checks the row now at the key (`heap-and-tuple.md`
  §4.1c, "What it leaves").
- **Foreign keys.** A child's fk column holds the parent's tuple key, and
  *"ON UPDATE CASCADE does not exist (the referenced key is immutable)"*
  (`foreign-keys.md` F1). The only actions are RESTRICT and NO ACTION (F2).
- **Cabin and assertion directories** store tuple keys, never locations
  (`cabin.md` C2; `assertion.md` §5.1's entry, *"never changed - `DELETE`
  writes the departure"*).
- **Transitions** keep a row's state history by induction on the key.
  `PURGE` is refused on a governed relation because a freed key could
  restart that history (`transition.md` §5).
- **Waystone trails.** Only a `SELECT` records one
  (`workorder-cf-waystone-premise.md` §1.1). A replay re-checks every key
  it reads (invariant 9).
- **A heap's tuple key is hidden**, and no SQL statement names it
  (`heap-and-tuple.md` §3.1b).

---

## 2. Rules every class keeps `[design]`

- **CH-R1 - a key change is a delete-mark and a placement.** The old slot
  is delete-marked by the transaction. The new key is placed by the named
  key path (`heap-and-tuple.md` §4.1), with its duplicate check, BD-Q9's
  wait and `next_id`'s admission. Nothing rewrites a Keystone word in
  place, and no `min_key` moves. In C2 that duplicate check finds the
  delete-marked slot and refuses `AlreadyExists` (invariant 11), so C2's
  new version is written onto that slot instead (CH-Q5), a path the
  named key placement does not have today.
- **CH-R2 - each consumer runs its own existing path for both halves.**
  The old key's half is `DELETE`'s, and the new key's half is `INSERT`'s:
  index entries, Cabin witness, assertion departure and arrival, and the
  forward foreign-key check of the row's own fk columns. Nothing is a
  second write path. What a half means for identity (a move, or a death
  and a birth) is CH-Q4.
- **CH-R3 - no reader's answer changes.** A reader whose view predates the
  change sees the row at the old key only. A reader whose view follows it
  sees the row at the new key only. No reader sees it at both or at
  neither. `[quiet-wrong]` if broken.
- **CH-R4 - existing paths unchanged.** Every statement that does not
  `SET` a tuple key answers byte for byte as at `9fdf3351`. The one
  exception is K2's refusal text, which CH changes on purpose.

---

## 3. The exploratory method (W1) `[design]`

- **CH-S0 checks the concept for C1, C2 and C3 and writes no engine code
  to `main`.** It runs once CH is opened and CH-Q1 is marked. Its verdict
  goes in §7, per class: build, narrow, or drop.
- **A spike may be written on the work branch to answer S0.** It is never
  merged: it is deleted, or rewritten as the stage that builds the class.
- **A class that fails is recorded in §7 with the reason that sank it,**
  and is never built.
- **Nothing merges to `main` without the operator's word.** Every K1 or
  K2 amendment is the operator's to mark.

---

## 4. Questions - not marked

**CH-Q0 - who decides after S0.**
- (a) The operator marks each class after S0's report.
- (b) S0's verdict decides, and the stages run on the standing go-ahead.

**CLA proposes (a)**: C2 and C3 amend K1, which an invariant's owner marks.

**CH-Q1 - which classes S0 checks.**
- (a) C1 only.
- (b) C1 and C2.
- (c) C1, C2 and C3.

**CLA proposes (c)**: S0 is a reading and costs no build, so checking C3
costs only the reading. Whether a client needs C3 is S0 part 4's question,
not a premise.

**CH-Q2 - a parent with children.** The row being moved is a foreign-key
parent, and a child names its key.
- (a) Refuse with RESTRICT, naming the child, as a parent `DELETE` is
  refused (F2).
- (b) Move the children too: `ON UPDATE CASCADE`. F1 says this does not
  exist, and F2 admits no action besides RESTRICT and NO ACTION.
- (c) NO ACTION deferred to the statement's end: a statement that also
  repoints every child passes. An `UPDATE` writes one relation, so this
  reaches only a self-referencing foreign key.

**CLA proposes (a)**: (b) reopens F1/F2, and (c) needs C3's end-of-statement
check first.

**CH-Q3 - the duplicate check for C3.**
- (a) Per row, as today. `SET id = id + 1` then fails on the first row
  whose target is still live, depending on walk order: PostgreSQL's
  behaviour without a deferrable constraint.
- (b) At the end of the statement. Every target is checked after every
  move off, under the key units of both sets.
- (c) No C3: a statement whose target is live is refused, whatever the
  order.

**CLA proposes (c) unless S0 finds (b) sound**: (a)'s outcome depends on the
order of the walk, which no client controls.

**CH-Q4 - what a move is to each consumer.** Is the row at the new key
the same row?
- (a) A death and a birth. The assertion takes a departure and an
  arrival. The Transition at the old key passes `ON DELETE FROM`, and the
  new key meets `ON INSERT`'s initial set. Indexes and Cabin take a
  delete's and an insert's effects.
- (b) A move. One identity, and the Transition's history follows the
  row. This needs new state, or a rule that a moved key restarts nothing.
- (c) Refuse a key change on a relation carrying a Transition, as
  `PURGE` is refused (`transition.md` §5).

**CLA proposes (a), with (c) for Transitions in C2 and C3**: (a) is what
CH-R2 already runs. In C1 the new key was never bound, so (a) is the
client's own `DELETE` and `INSERT`, which the induction already admits:
the old key must pass `ON DELETE FROM` and the new key `ON INSERT`. In C2
and C3 the new key was bound, so (a) restarts a governed key's history
under an old identity, the one way out `transition.md` §2 names and §5
closes for `PURGE`.

**CH-Q5 - C2's horizon.** When can a delete-marked key be taken again
inside a transaction?
- (a) Only when this transaction deleted it. The new version clears the
  delete-mark and writes a `kOverwrite` undo record whose image is the
  deleted row, so an older reader steps back through it and then through
  the `kDeleteMark` record to the committed old row (`txn.md` §4.3), as
  through an `UPDATE`. No path today overwrites a delete-marked slot, and
  the slot's chain then holds two logical tuples, against `txn.md` §2.
- (b) (a), or a committed delete that `ResolvedForEveryReader` passes, as
  `PURGE` gates.
- (c) No C2.

**CLA proposes (a) if S0 finds its MVCC reading sound**: (b) is `PURGE`'s
gate inside an explicit transaction, which `PURGE` itself refuses
(BH-R2, BH-Q4 (a), PU10).

**CH-Q6 - the value expression.**
- (a) Any BJ expression typed as the key's integer type (`types.md`
  §3.2b), the result checked against `[1, 2^48 - 1]`, `OutOfRange` per row.
- (b) A literal or `column ± literal` only.

**CLA proposes (a)**: BJ already types and evaluates the expression.

**CH-Q7 - the amended invariant texts.** K2 becomes "changes only by a
delete-mark and a placement in one transaction". K1's exception list gains
C2's rule if C2 lands. Invariant 11 (CLAUDE.md, `heap-and-tuple.md` §8)
loses "never updatable". The question is who words them.
- (a) CLA drafts them at the stage that builds the class, and the
  operator marks them.
- (b) The operator words them before the stage starts.

**CLA proposes (a)**.

---

## 5. Stages

Every stage is reviewed by `critics-developer` and gated by the full
suite. The overhead is measured once, at the close, inside the budget
of 2026-10-13: about an hour, building included.

**CH-S0 - the concept check.** No engine code reaches `main`. It has
four parts:

1. **The census.** Every consumer in §1 is asked, per class, what a key
   change does to it under CH-Q4's readings, with `path:line`. A consumer
   with no sound answer sinks the class.
2. **The MVCC reading for C2 (CH-Q5).** A new key's version stacked on a
   slot this transaction delete-marked: does every older reader still
   reach the old version through undo, and does CH-R3 hold, including
   after a rollback and after a crash at every point the move logs?
3. **The C3 reading (CH-Q3).** What an end-of-statement uniqueness check
   needs from the lock family (two key-unit sets, deadlock between two
   permuting statements), and whether BA-S14's re-run of a refused
   statement still holds when the statement has moved rows before it is
   refused.
4. **The demand reading.** Does a client in reach change a key? Read the
   xrock client and the `anti-kdbs` repros for it, as source-read, never
   assumed.

Optionally, a spike for C1 on the work branch, showing CH-R1, CH-R2 and
CH-R4 hold on the existing tests. The verdict goes in §7, per class.

**CH-S1 - C1**, on the operator's mark (CH-Q0). It covers:
- K2's amendment (CH-Q7) and the `SET` path;
- CH-Q2's refusal, CH-Q4's consumer reading and CH-Q6's expression;
- the tests: each refusal with its byte; each consumer for both halves;
  CH-R3 under an older and a newer view on another core; a concurrent
  writer of the target key; rollback; and a crash at every record the
  move logs (the simulation harness).

**CH-S2 - C2**, only on its mark, with K1's amendment.

**CH-S3 - C3**, only on its mark, and only after S2.

**CH-S4 - the close.**
- The close measurement: a key change against the client's own
  `INSERT` + `DELETE`, and an ordinary `UPDATE`, interleaved A/B in
  `build-release` against `9fdf3351`.
- The stage log, and every text that states the key never changes:
  `keystoneid-invariant.md`, `heap-and-tuple.md` §4 (both sentences) and
  §8, `txn.md` §2's first bullet, `cabin.md` C2, `assertion.md` §5.1,
  `foreign-keys.md` F1, `manual/sql/sql.md` and `known-gaps.md`.
- CLAUDE.md's invariant 11 and its Keystone rows.

---

## 6. Out of scope

What CH does not reach:
- `ON UPDATE CASCADE` and `SET NULL` (F1, F2), unless CH-Q2 (b) is marked;
- K5's offline re-key, and any change to `next_id`'s meaning (K3, K4);
- a heap's hidden key: `Schema::FindColumn` never resolves it
  (`heap-and-tuple.md` §3.1b), so a `SET` naming it is the unknown-column
  refusal and K2 is never reached;
- the upsert (CG);
- a key change across relations.

---

## 7. Stage log

Not opened.
