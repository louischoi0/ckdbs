# Work order CR - a `READ COMMITTED` write that lost to a decided newer version is run again, not refused

Status: **WRITTEN 2026-11-12, not opened.** Written on
`worktree-cr-read-committed-rerun` from `ce5347e6` (`v3.4.8-2-gce5347e6`)
on the operator's word of 2026-11-12 (`raft-marks-2026-11-12.md` §1):
option (c) of the pending question A1 is chosen, and *"write the work
order only"*. No stage is started; CR-S0 waits for the operator's word
and for the marks on §4's items. Every `[source-read]` claim below was
read at `ce5347e6`.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured.

Related: `docs/spec/txn.md` §5 (the rule CR changes);
`manual/client/client.md` §4, §5;
`instructions/v3.2.0/workorder-cq-column-reservation.md` §3 (A1), §8;
`bench/v3.4.7/results-cq-s0-premise-v3.4.7-13-g92449e65.md`;
`bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md` (M4);
`instructions/v3.0.0/workorder-ba-parallelism.md` (BA-S14, the structural
re-run CR's is modelled on).
The pending entry this order replaces,
`docs/pending/read-committed-rerun-on-decided-newer.md`, is deleted by the
commit that adds this file, as `docs/pending/README.md` rules;
`git show ce5347e6:docs/pending/read-committed-rerun-on-decided-newer.md`
retrieves it.

## 0. What CR is

`[operator]` Under `READ COMMITTED`, a write statement that is refused
because the row's newest version was written by a transaction that is
**decided** and that the statement's view cannot see is, where it has
written nothing, **run again under a fresh view** instead of answered
`TXN_CONFLICT` - in autocommit, and inside an explicit `READ COMMITTED`
transaction (A1's option (c)).

`[design]` In autocommit it changes no result a client could not already
get by retrying: it moves the retry into the engine and saves the round
trip. Inside an explicit transaction it does what a client cannot - a
client cannot retry one statement of a failed transaction - and keeps the
statements before it. It is a contract change in `txn.md` §5, which today says "KDS
aborts retryably rather than re-reading ... a deliberate simplification -
there is no re-read loop" (`docs/spec/txn.md:772-776`).

`REPEATABLE READ` is outside CR: there the refusal is first-updater-wins
and is the level's contract.

## 1. The tree as read

### 1.1 What is measured `[measured]`

| measurement | shape | refused |
|---|---|---|
| CP-S0 M4, `v3.4.7-1-gcc54a296`, `cores = 4`, `relaxed` | 8 sessions, autocommit, an absolute `SET` on one row | `2,519` of `32,000` statements (`7.9 %`), all `TXN_CONFLICT` |
| CQ-S0 M1, `v3.4.7-13-g92449e65`, `cores = 4`, `group` | 8 sessions, an explicit transaction debiting one row | `1.4-12.8 %` of attempts across the hot arms, every one decided-newer, each failing its transaction; `1.4 %` with the debit first, `2.5 %` with it last, `3.3 %` with it sent with `COMMIT`, at eight following statements |
| CQ-S0 M2, the same commit, `strict` | the same | `0-0.4 %` |

No deadlock and no fault-net refusal stands behind any of them. **Not
measured**: which part of these refusals is §1.2's window and which its
marker case; what a re-run costs in latency under sustained contention on
one row.

### 1.2 What the tree does `[source-read]` at `ce5347e6`

- **The verdict.** A writer whose view cannot see the row's current
  writer is refused `TxnConflict`, whether that writer is in flight or
  committed after the view (`src/txn/manager.cpp:188-207`). Under `READ
  COMMITTED` every statement mints a fresh view
  (`src/txn/manager.cpp:168-186`), so the refusal's window is from that
  mint to the statement's write.
- **An in-flight writer is waited for; a decided one is refused.** The
  dispatcher's `CheckWriteConflictBlocking` records the holder a refused
  statement waits for (`src/server/command_dispatcher.cpp:11660-11709`),
  and `NoteBlockingWriter` records only a holder in flight, or one
  decided and not yet released
  (`include/kds/server/command_dispatcher.hpp:819-864`). A statement
  refused with no holder recorded is answered; inside an explicit
  transaction `EndWrite` fails the transaction (`session.Poison()`,
  `src/server/command_dispatcher.cpp:11761-11782`), **inside
  `DispatchAndStage`, before either entry point's re-run loop sees the
  outcome**, and in autocommit the attempt's transaction is aborted.
  The verdict has two callers, `UPDATE`'s row path (`:10194`) and
  `DELETE`'s (`:12829`); an `INSERT`'s and a unique index's checks read
  latest state and are not this verdict.
- **Not recording a holder does not mean the writer is decided.**
  `NoteBlockingWriter` also records nothing where the path cannot park
  (every synchronous `Dispatch`), where a lock wait is already recorded,
  where there is no lock table and the waiter has a trail, and for a
  `REPEATABLE READ` waiter whose re-run is futile
  (`src/server/command_dispatcher.cpp:11166-11197`).
- **The row's `U` is asked before the conflict is decided**, on the
  per-row path (`src/server/command_dispatcher.cpp:10176-10194`). `[design]`
  So a statement refused decided-newer inside an explicit transaction
  already holds that row's `U` - `DELETE` its `X` (`:12825-12828`), a
  declared key window its window before it walks (`:10056-10071`) - and
  `EndWrite` releases nothing of an explicit transaction's on a failed
  statement. **On the same row, with a lock table, a re-run there can
  lose again only to the marker case below**, not to a third writer. Two
  limits: a dispatcher with no lock table borrows nothing, and a
  predicate that is not a key can qualify, under the fresh view, a row
  whose unit the first attempt never asked. In autocommit the refused
  attempt's transaction ends (`AbortOwnedScope`) and the unit is
  released.
- **The marker case.** A view's ceiling is the published maximum capped
  by every open commit's marker, and a marker can cap commits that are
  already acknowledged, the committing session's own next statement
  included (`include/kds/txn/instance_visibility.hpp:62-105`). `[design]`
  So a fresh view minted at once can still be below the commit the
  statement lost to, while a third commit is open; a re-run that does not
  wait for the ceiling to pass that commit is refused again on the same
  ground.
- **Two re-runs exist.** A statement that waited on a holder having
  written nothing is run again whole at its decide - one that wrote rows
  parks mid-walk and resumes under its first view and trail mark
  (`:10700-10716`), and if its holder committed that resume is refused
  decided-newer by construction, a source of this refusal CR does not
  rescue (CR-R1) - and a statement refused by a stale descent that
  wrote nothing is run again after a yield, inside a deadline of
  `lock_wait_fault_net_ms` taken at the first refusal
  (`StructuralRerunDue`, `src/server/command_dispatcher.cpp:934-954`, the
  loops at `:822` and `:1229`). It refuses to re-run once a row has left
  the result sink, and rewinds the sink otherwise. Whether a statement
  wrote anything is `statement_trail_mark_`
  (`include/kds/server/command_dispatcher.hpp:2729-2741`); a statement
  that wrote rows is not re-runnable, there being no savepoints.

## 2. The mechanism `[design]`

One note, one test in `EndWrite`, the existing loops. Steps 3 and 4 are
written as CR-Q3 (a) and CR-Q4 (b) would have them; the marks decide.

1. **The note.** Where the conflict verdict is raised, the statement asks
   the instance directly whether the row's writer is in flight; where it
   is not and has a commit LSN, the statement notes "decided newer" with
   that LSN, as a stale descent notes a structural refusal. A writer
   with no commit LSN is not noted, and under `REPEATABLE READ` nothing
   is. The note is never inferred from a holder not having been recorded
   (§1.2).
2. **The test is `EndWrite`'s**, as BA-S14's is, because that is where an
   explicit transaction is failed: a refusal whose note is set, at
   `READ COMMITTED`, with the trail at the statement's mark, on a path
   that re-runs, **withholds the poison**; any other refusal clears the
   note. The test reads the note and never the status code, which the
   write paths hand `EndWrite` as an `InvalidArgument` built from the
   reply. A trail at its mark implies no assertion reservation, both
   write sites asking the registry after the conflict check - for CR-S0
   to confirm.
3. **The admission**, beside `StructuralRerunDue` in both entry points:
   the withheld refusal, no row left the sink (`Streamed()`), the
   deadline not passed. The sink is rewound. **Whatever ends the re-runs
   owes the poison back**, as `out->poison_withheld` does today.
4. **The fresh view.** In autocommit the re-run is a fresh statement
   under a fresh transaction, as the wait's re-run is. Inside an explicit
   transaction the statement is run again from `StartStatement`, and
   what the refused attempt borrowed stays the transaction's. Before the
   mint the re-run waits until the snapshot ceiling covers the noted
   commit LSN, or gives up at the deadline. **This wait is taken holding
   units** - the row's `U`, the relation's `IX`, a foreign key's `S` -
   where the wait it is modelled on, a session's own acknowledged commit
   (`src/server/command_dispatcher.cpp:761-775`), is premised on holding
   nothing and has no deadline; it is outside the wait-for graph, and it
   is sound only because a marker lifts without asking any lock. On the
   synchronous path the same wait is a yield loop (`:1204-1216`).
5. **Giving up.** Past the deadline the refusal is answered as today:
   `TXN_CONFLICT`, `retryable = 1`, and inside an explicit transaction
   the transaction failed. A client's retry loop stays owed.

## 3. Rules

- **CR-R1. A statement that wrote anything is never run again**
  `[quiet-wrong]`: a row, an index entry, a spill, an assertion
  reservation, a row sent. A multi-row statement refused at its second
  row is answered as today.
- **CR-R2. `REPEATABLE READ` is untouched**, and a test pins that its
  refusal is still answered.
- **CR-R3. The re-run is bounded** by the deadline (CR-Q3) and answers
  the same refusal past it. No statement spins.
- **CR-R4. A re-run inside an explicit transaction leaves the
  transaction as it would be had the statement been sent once, after the
  writer it lost to was visible** - except that the units the refused
  attempt borrowed stay held, a `U` on a row the re-run no longer
  qualifies among them, each counting against `max_locks_per_txn`.
- **CR-R4a. The poison is withheld in `EndWrite` and owed back** by
  whatever ends the re-runs `[quiet-wrong]`: a withheld poison nobody
  returns leaves a failed statement's transaction committable.
- **CR-R4b. The ceiling wait is bounded by the deadline and asks no
  lock**; it is the one wait CR adds that the deadlock detector does not
  see, and a test holds a marker open across it.
- **CR-R5. Off the refusal's path the cost is nil**: a statement never
  refused executes no new instruction but the admission's test at an
  outcome that is already a refusal. Measured at the close.
- **CR-R6. No new keyword or format.** Whether a setting is added is
  CR-Q3's and CR-Q6's.
- **CR-R7. The spec moves with the code**: `txn.md` §5's paragraph and
  verdict table (`:772-776`) and its "none of them is a re-read loop"
  (`:836`), `manual/client/client.md` §4's "retry the whole transaction"
  for this refusal and §5's retry figures' wording, in the stage that
  lands the behaviour, not before.
- The standing rules: each stage reviewed, the full suite per stage, the
  overhead measured once at the close in `build-release`, a stage's
  measurement inside 20 minutes under a 60 s watchdog, nothing pushed or
  merged without the operator's word.

## 4. Items for the operator (CR-Q), CLA's proposals

None is assumed; each waits for a mark.

| | item | options | CLA proposes |
|---|---|---|---|
| CR-Q1 | Does CR-S0 gate the build? | (a) S0 is a premise check with a bar: if the re-run cannot get past the marker case inside the deadline for most refusals, or a client-side emulation shows the statement's p99 under sustained contention worse than refusing, CR stops and returns to the operator. (b) (c) is built whatever S0 shows; S0 only sizes it | (a) |
| CR-Q2 | One stage or two | (a) autocommit first (CR-S1), the explicit transaction second (CR-S2), each landing whole. (b) one stage | (a) |
| CR-Q3 | The bound | (a) the deadline alone, `lock_wait_fault_net_ms` from the first refusal, shared with the structural re-run and the waits. (b) a round count as well | (a): no second name for a quantity a setting expresses |
| CR-Q4 | The marker case | (a) decided after CR-S0 measures its share: wait for the ceiling if it is material, a yield and re-run if it is not. (b) wait for the ceiling whatever its share. (c) yield and re-run only | (a) |
| CR-Q5 | Which statements | (a) every write whose refusal is this verdict and that wrote nothing - `UPDATE`, `DELETE`, `ON CONFLICT DO UPDATE`'s update arm, as CR-S0's census lists them, each with a test. (b) single-row `UPDATE` by tuple key only | (a) |
| CR-Q6 | An off-switch | (a) none. (b) a boot key | (a) |
| CR-Q7 | What is reported | (a) one tally beside `structural_reruns` in the contention counters, and the wait under an existing wait event. (b) nothing new | (a) |
| CR-Q8 | S0's instrument | (a) S0 may carry a diagnostic counter on the work branch, never landed, that classes each decided-newer refusal as window or marker. (b) no engine code at S0; the share is inferred from the durability rungs | (a) |

## 5. Stages

As CR-Q2 (a) proposes; under (b) CR-S1 and CR-S2 are one stage.

| stage | what | ends in |
|---|---|---|
| **CR-S0** | The contract read and the census: every site that raises the verdict, every write path's order of borrow and check, what a refused attempt holds and has written at each, the mid-walk park's resume, the synchronous path, a dispatcher with no lock table, what `NOW()` and a scalar subquery do on a second run. The premise measured in M4's and CQ-S0 M1's shapes: the refusals classed window or marker (CR-Q8), and the rounds and latency of an immediate client retry as the re-run's upper bound. No behaviour change | a results file under `bench/v3.4.8/`, the census in §7, CR-Q1's bar read |
| **CR-S1** | The note, the admission, the marker wait as CR-Q4 is marked, in **autocommit**. Tests: the re-run commits; `REPEATABLE READ` still refused; a statement that wrote is still refused; the deadline; a row sent; a sim cell | `txn.md` §5 and the manual for autocommit; M4 re-measured |
| **CR-S2** | The same inside an explicit `READ COMMITTED` transaction: the transaction not failed, the statement run again from `StartStatement`. Tests as S1's, and the transaction's earlier writes intact, `COMMIT` after a re-run durable, a deadlock through a re-run refused by the detector | `txn.md` §5 and the manual whole; CQ-S0 M1 re-measured, debit first and last |
| **CR-S3** | The close: the overhead A/B over the whole change, the known gaps, the report | the operator's word on landing |

## 6. What CR does not do

- No change to what a view sees, nor to the verdict function, which
  stays a pure function of the row's writer and the view.
- No column reservation: CQ closed narrow and stays closed.
- No change to the wait for an in-flight writer, the deadlock detector or
  the fault net.
- No client-visible guarantee that a conflict is never returned.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-12

On `worktree-cr-read-committed-rerun` from `ce5347e6`, on the operator's
word. No stage is opened and no code is written.

**The review** (`critics-developer`, 2026-11-12). It confirmed §1.1's
figures and §1.2's line ranges and readings. Applied:

- **§2's test moved into `EndWrite`.** The draft put the admission beside
  `StructuralRerunDue`, which runs after `EndWrite` has already failed
  an explicit transaction; the poison is now withheld there and owed
  back (CR-R4a).
- The note asks in-flight directly and carries the commit LSN: "no
  holder recorded" was read as "decided", which four other cases also
  produce (§1.2).
- "A re-run can lose again only to the marker" narrowed to the same row
  with a lock table; the mid-walk park's resume named as a source CR
  does not rescue.
- The ceiling wait stated as a wait taken holding units, outside the
  wait-for graph (CR-R4b).
- Proposals written as decided - the deadline, the two stages, no
  setting, the ceiling wait - marked as the item's proposal, here and in
  the index row; §0's and CR-R4's wording for an explicit transaction;
  CR-R7's list; the cuts in §6; a dated pointer in CQ's order, which
  cited the deleted entry.

**Rejected**: nothing.
