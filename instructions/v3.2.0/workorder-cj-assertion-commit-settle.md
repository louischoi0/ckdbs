# Work order CJ — an assertion reservation's commit settle, removed: an exploratory and verifying milestone

Written 2026-10-31 on `main` from `18264195` (`v3.4.6-4-g18264195`), on
the operator's words, recorded in English (`raft-marks-2026-10-31.md` §1):

- **W0** (2026-10-31): the operator proposed Oracle's delayed block cleanout
  for KDS: *"if KDS updates each tuple's lock byte or state at commit,
  consider a per-core transaction status table and a cleanout deferred to
  the reader"*.
- **CLA's answer to W0**: no tuple is visited at commit
  (`TransactionManager::FinishCommitBody`, `src/txn/manager.cpp:287-337`,
  writes no page), and no reader writes a page to learn a commit (the floor
  and the window answer, `txn.md` §4.1). The one commit-time step whose page
  work scales with what the transaction wrote is the assertion reservation's
  settle (`AssertionEnforcer::CommitTxn`). CLA read it and found that the
  bit it clears has no reader (§1.3), and proposed three options: (a) keep
  it, (b) remove the settle, (c) give the entry a writer id so the bit can
  be resolved lazily.
- **W1**: *"Tell me about the assertion reservation settle."*
- **W2**: *"Write an exploratory, verifying milestone document on (b)."*

**Written 2026-10-31; not opened.** CJ-Q0..Q7 carry CLA's proposals and
none is marked. **Exploratory and verifying since W2** (§3): CJ-S0
verifies the premise and measures the settle before anything is built,
and its verdict is build, narrow or drop.

**Claim tags**: `[operator]` for W0-W2; `[source-read]` with `path:line`
at `18264195`; `[measured]` with its results file; `[design]` for
reasoning about code not yet written; `[quiet-wrong]` where a wrong choice
turns a correct answer into a wrong one with no error.

**The letter.** `[source-read]` CI is the last work order under
`instructions/v3.2.0/` on `origin/main` at `18264195`, `git worktree list`
names no `CJ`, and no commit subject on `origin/main` begins with the
token. CLA's choice.

**CJ against other orders.**
- CJ amends `docs/spec/assertion.md` §5.1 (the `flags` row), §6.2 step 4,
  §7's record list and AS6b's "Cost asymmetry, stated", and `wal.md`'s
  assertion row (`docs/spec/wal.md:168`).
- It leaves the admission, the hold, the reserve, `AbortTxn`,
  `ASSERT_ROLLBACK`, AS6a's snapshot and fold, and AS6b's `kEntryOrphaned`
  as they are.
- It does not take up a deferred cleanout of tuples. §0 says why it is not
  needed.
- Not part of AR0 §8's chain.

---

## 0. What CJ is

**The hypothesis.** A reservation's entry is written with `kEntryReserved`
set. At commit, `CommitTxn` fetches each page the transaction's
reservations landed on, appends an `ASSERT_COMMIT`, clears the bit and
stamps the page, all **before the transaction's commit record**. CJ holds
three hypotheses:

- **H1 - the bit has no reader.** No admission, no recovery step, no
  verification and no tool reads the on-page `kEntryReserved` of a live
  entry, so its value after commit decides nothing (§1.3, to be confirmed
  by CJ-S0).
- **H2 - removing the settle removes a failure point.** With the page work
  gone, `CommitTxn` takes the pending list out of the registry and cannot
  fail. The stranded-reservation path (§1.6) then has no failure to start
  from. A stranded reservation does not always fail closed (§1.6): a
  stranded departure leaves its group's total too low. So H2 is a
  correctness argument whichever way a reservation moves its group.
- **H3 - the settle costs something measurable.** It costs one page fetch,
  one record, one page dirtied and one stamp per `(assertion, page)` on
  every commit that reserved. This has not been measured (§1.7).

**What CJ would do (option (b)).** `CommitTxn` stops touching pages and
stops appending `ASSERT_COMMIT`. `kEntryReserved` is redefined from "this
reservation is in flight" to **"this entry was written by the reservation
path"** (an `ASSERT_RESERVE`, as against an `ASSERT_BUILD`). That is the
one meaning anything reads today (§1.3).

**Why not a deferred cleanout of the bit (option (c)).** An entry carries no
writer id (§5.1: tuple key, flags, hint, value, `group_id`), so a reader
meeting the bit cannot ask the window whether its writer committed. And
there is no reader to defer it to. Giving the entry a writer id is a format
change with no consumer, which the standing discipline forbids ("no
structure built without a confirmed consumer").

**Why tuples need no deferred cleanout (W0).** `[source-read]` A tuple
header is `trx_id:48 | undo_ptr | data_len | flags` with no commit bit
(`heap-and-tuple.md` invariant 12). AO-R3 left the lock family no persisted
bit (`heap-and-tuple.md` §2, `txn.md` §5). Commit publishes a window entry,
retires the id, moves the schema word and releases the borrows, and writes
no page (`manager.cpp:287-337`). A reader below the floor needs no lookup,
because an aborted writer's changes are physically undone before anyone can
read them (`txn.md` §4.1). Readers never write a page to record what they
learned. That is what Oracle's cleanout and PostgreSQL's hint bits pay
for, and KDS does not pay it.

---

## 1. The tree as read `[source-read]` at `18264195`

### 1.1 The protocol

`assertion.md` §6.2:

1. **Group key.** Compute it and hash it.
2. **Admission.** Checked against the group header's
   `count`/`sum`, which is **committed plus reserved**, with every held
   contribution counted. A pass takes a **hold**.
3. **Reserve.** Append the entry with `kEntryReserved`
   (`src/exec/assertion_check.cpp:563`). Under the directory latch, append
   `ASSERT_RESERVE`, apply the delta to the header and release the hold.
   The reservation joins `pending_[txn]` in memory.
4. **Commit.** *"clear `RESERVED` on the transaction's entries
   (piggybacked on commit processing; aggregate is already correct)"*.
5. **Abort.** Subtract each delta, mark each entry `kEntryOrphaned`, and
   append one `ASSERT_ROLLBACK` per reservation.

### 1.2 The settle

`AssertionEnforcer::CommitTxn` (`assertion_check.cpp:811-879`):

1. **Under the directory latch.** It moves `pending_[txn]` out of the map
   before anything can fail (the comment at `:826-836` says why), and
   groups the reservations by `(assertion, page)`. It skips an assertion
   dropped mid-transaction (`:840`).
2. **Outside the latch, per group.** In order:
   - `store.Get` and `BoundCabinPage::Open`;
   - `ASSERT_COMMIT` appended, with the group's index list;
   - `ClearReserved` per index (`src/storage/cabin_bound_page.cpp:192-194`);
   - `StampPageLsn`.

   Log, then clear, then stamp: the order `AbortTxn` documents.

**Its callers run it before the commit record.**

| Call site | What it serves | What follows it |
|---|---|---|
| `src/server/command_dispatcher.cpp:10694` | explicit `COMMIT` | `txn_->Commit` / `CommitDeferred` at `:10698` |
| `command_dispatcher.cpp:11663` | the autocommit write scope | `Commit` at `:11667-11668` |
| `command_dispatcher.cpp:11576` | the dispatcher with no transaction manager, under `catalog::kBootstrapXid` | none |

### 1.3 Every reader of `kEntryReserved`

The search covered `src`, `include`, `tests`, `tools`, `sim`, `scripts`
and `fuzz` for `kEntryReserved`, `reserved()`, `ClearReserved` and a raw
decode of an entry's flag byte. `SetFlags` (`cabin_bound_page.cpp:188`)
only masks. `device_page_store.cpp:1666` and `:2891` and `redo.cpp:90`
check the page type and never an entry. No Python tool and no `sys` view
decodes entry flags. Comment-only mentions (`wal/record.hpp:100`, `:118`,
`wal/payload.hpp:553`) and further test lines (`assertion_wal_test.cpp:174`,
`:320`, `:346`, `:445`, `:486`, `:516`) are not listed below.

| Site | What it does with the bit |
|---|---|
| `include/kds/storage/cabin_bound_page.hpp:73`, `:151` | definition, `reserved()` accessor |
| `assertion_check.cpp:563` | sets it at reserve |
| `assertion_build.cpp:251` | a build entry is written without it (*"committed: no kEntryReserved"*) |
| `cabin_bound_page.cpp:192-194` | `ClearReserved`, called by `CommitTxn` and `ReplayCommit` |
| `assertion_replay.cpp:44` | **the one read**: an `ASSERT_RESERVE` record's entry bytes must carry it, and an `ASSERT_BUILD`'s must not. These are **the record's** bytes, not the page's |
| `tests/assertion_wal_test.cpp:204`, `tests/bound_cabin_test.cpp:83`, `:197`, `:203` | the bit set, round-tripped and cleared |
| `tools/assertion_abort_benchmark.py:11` | prose only |

**What reads the other flags instead.**
- The AS6a linkage scan reads `orphaned()` (`src/exec/assertion_recover.cpp:69`).
- `VerifyAgainstEntries` reads `departure()` (`src/exec/bound_cabin.cpp:446-482`).
- Admission reads the group header and the in-memory holds and
  `pending_` (§6.1, §6.2).

None of them reads the page's `kEntryReserved`.

### 1.4 `ASSERT_COMMIT` at recovery

`ReplayCommit` (`assertion_replay.cpp:105-127`) clears the bit on the page
and nothing else: *"The directory is untouched on purpose: a reservation
counted in the aggregate from admission (§6.2), so commit moves flags and
never sums."* `:194` classifies it as an assertion record. The assertion
pass folds it at `:211-212` (*"a page half only"*).

### 1.5 The record

`AssertCommitPayload` (`include/kds/wal/payload.hpp:633-661`) is
`assertion_id u64, count u16, reserved u16` plus `count` u16 indexes.
`wal.md:168` lists it among the assertion records.

### 1.6 The failure the settle can reach

A `CommitTxn` that fails after taking `pending_[txn]` out leaves an
`AbortTxn` that finds nothing (`AbortTxn` returns OK on a missing id). So
the contributions it had not settled stay in the group totals, while the
rows behind them are compensated by the transaction's undo. **Which way
that fails depends on the contribution:**

| Stranded contribution | The group's total | The outcome |
|---|---|---|
| An arrival (`INSERT`, an `UPDATE` moving in) of a non-negative value | too high | **fails closed**: writes it could admit are refused |
| A **departure** (`DELETE`, an `UPDATE` moving out). Its delta lowers the total (`assertion_check.cpp:498`, `:600-601`; reserved at `:802`, `pending_` at `:612`) | **too low** | **fails open** `[quiet-wrong]`: the row comes back through undo, the total does not, and the bound (upper only, `assertion.md:63`) admits writes it should refuse |
| A `SUM` arrival of a negative value | too low | fails open, as above |

The dispatcher's comment (`command_dispatcher.cpp:11653-11660`) states
"Fail-closed" without the second and third rows. It is wrong, and CJ-S3
corrects it.

The bug entry `assertion-reservations-stranded-by-a-failed-settle.md` was
filed 2026-09-09 and closed unreproduced in the batch at `d83bd502`
(`git show d83bd502^:docs/inflight/bugs/assertion-reservations-stranded-by-a-failed-settle.md`).
The dispatcher still cites it at `command_dispatcher.cpp:10685-10693`. What
reaches it is a failed `store.Get`, `Open`, append or stamp, under drain
mode (`DrainOnPressure`, `:815`).

### 1.7 What the settle costs

**Not measured.** `tools/assertion_benchmark.py` prices the commit half
(every statement autocommitted) and `tools/assertion_abort_benchmark.py`
the abort half. No results file under `bench/` isolates the settle's
share.

The structure (AS6b, "Cost asymmetry, stated") is as follows. Entries are
appended at the chain's tail, so a transaction's K reservations against
one assertion usually share one page whatever their `GROUP BY` values. The
settle is then about one fetch, one record and one stamp **per assertion**,
plus K in-memory bit clears.

### 1.8 An adjacent finding: a crash loser's reservations

`assertion.md` §7 (`:477`) states that *"in-flight (uncommitted)
reservations at crash are rolled back by normal transaction recovery via
`ASSERT_ROLLBACK` compensation"*. The search found otherwise:
- `ASSERT_ROLLBACK` is appended by the live `AbortTxn` only
  (`assertion_check.cpp`).
- It is folded by `ReplayRollback` (`assertion_replay.cpp:129`).
- No mount path that emits one for a loser was found:
  - `src/wal/recovery.cpp` and `src/txn/recovery_undo.cpp` name no
    assertion record;
  - `redo.cpp:495-499` only defers assertion records to the assertion pass;
  - the assertion fold (`assertion_recover.cpp:268-312`) filters by
    assertion id and base, never by a transaction's outcome, and
    `ReplayEntry` folds every `ASSERT_RESERVE` delta;
  - `mount_recovery.cpp:171-305` and `sim/instance.cpp:159` hand the
    assertion pass no list of losers;
  - the linkage scan re-attaches every entry that is not orphaned
    (`assertion_recover.cpp:69`).
- No test pins it. `tests/assertion_crash_test.cpp` has one cell, and
  `tests/assertion_recover_test.cpp`'s abort cells abort live. The BS sim
  crashes at a seed-chosen op with an assertion declared
  (`sim/loop.cpp:416-425`), but no cell asserts a loser's group total.

The reading was confirmed by an independent read at `18264195`. So
`assertion.md:477-478` is wrong: a loser's contribution stays in its group
after a mount. **Its direction is §1.6's table**: a loser's arrival fails
closed, and **a loser's departure or negative `SUM` arrival fails open**
`[quiet-wrong]`. Filed as
`docs/inflight/bugs/an-assertion-keeps-the-reservations-of-a-crash-loser-and-a-failed-settle.md`
(found by reading, not reproduced).

**CJ does not fix the crash path** (CJ-Q5). It matters to CJ for two
reasons:
- **The fix tells winners from losers by `TXN_COMMIT`, not by the bit.**
  After a full replay today, a set bit nearly identifies a loser's entry,
  and the entry carries enough to undo it (tuple key, value, group,
  departure). But the bit is not exact: a crash between a transaction's
  `ASSERT_COMMIT` and its `TXN_COMMIT` leaves a cleared bit on a loser. So
  the analysis phase's winners and losers are the only sound source.
  CJ-R1 takes the bit-based fix off the table for good, and CJ-R4 binds the
  fix to that.
- **CJ removes one of the two paths that strand a reservation** (the failed
  settle, §1.6). That leaves the crash path as the only one.

---

## 2. Rulings proposed `[design]`

**CJ-R1 - the bit's meaning.** `kEntryReserved` means *"written by the
reservation path"*. It is set at reserve, as today, and never cleared on
the live path. A build entry never carries it. `assertion_replay.cpp:44`'s
check keeps its meaning unchanged.

**CJ-R2 - the settle.** `CommitTxn` moves `pending_[txn]` out under the
directory latch and does nothing else: no page, no record, no stamp. With
nothing left that can fail, CJ-Q7 decides its signature and the
dispatcher's three failure arms (§1.2).

**CJ-R3 - `ASSERT_COMMIT`.** The record is no longer written. Its record
type keeps its number and is never reused. Its replay arm stays for a log
written before CJ (CJ-Q4).

**CJ-R4 - no reader may trust the bit as "in flight"** `[quiet-wrong]`. A
future consumer that reads `kEntryReserved` as "this reservation has not
committed" would treat every committed reservation as undecided. Examples:
a verifier that skips reserved entries, a linkage scan that drops them, a
diagnostic view that subtracts them. Each would be wrong with no error.
Three guards:
- the definition's comment (`cabin_bound_page.hpp:64-73`) and §5.1 state
  CJ-R1;
- a cell pins that a committed reservation's entry carries the bit;
- a mutant reader that trusts the bit fails a cell (CJ-S2).

The ruling binds §1.8's fix as well. It tells losers apart by the
analysis phase's outcome and never by the bit.

**CJ-R5 - the stranding paths.** After CJ no commit-time step can strand a
reservation (H2), so the failed-settle row of the bug entry closes with
CJ. §1.8's crash-loser path, which fails open for departures, is confirmed
by a cell in CJ-S0 and left to its own order (CJ-Q5).

**CJ-R6 - what does not change.**
- Admission and its O(1) header read.
- The hold.
- `ASSERT_RESERVE`'s format.
- `AbortTxn` and `ASSERT_ROLLBACK`.
- AS6a's snapshot, fold and linkage scan.
- AS6b's `kEntryOrphaned`.
- `VerifyAgainstEntries`.
- Every statement on a relation with no assertion: byte for byte, and no
  cost.

---

## 3. The exploratory method (W2) `[design]`

- **CJ-S0 verifies H1 and measures H3 before anything is built.** It runs
  once CJ is opened and CJ-Q1..Q2 are marked. Its results file says, per
  hypothesis, held or refuted, with the evidence.
- **A spike may be written on the work branch to answer S0.** It is never
  merged: it is deleted, or rewritten as CJ-S1.
- **The verdict is build, narrow or drop.**
  - **Build**: H1 holds.
  - **Narrow**: a reader is found that can be moved onto another fact, and
    CJ-S1 moves it.
  - **Drop**: a reader is found that needs the bit's present meaning. It is
    recorded in §7 with what sank it, and the settle stays.
- **Nothing merges to `main` without the operator's word.** The redefinition
  of an on-page bit's meaning is the operator's to mark (CJ-Q0).

---

## 4. Questions - not marked

**CJ-Q0 - who decides after S0.**
- (a) The operator marks S0's verdict.
- (b) S0's verdict decides, and the stages run on the standing go-ahead.

**CLA proposes (a)**: CJ redefines what an authoritative page's bit
means.

**CJ-Q1 - the bit's fate.**
- (a) Keep setting it at reserve, never clear it (CJ-R1). No format
  change, and the replay check keeps its meaning.
- (b) Stop setting it. A reservation's entry then equals a build entry,
  and `assertion_replay.cpp:44` changes its rule.
- (c) Keep clearing it, lazily: not possible without a writer id (§0).

**CLA proposes (a)**.

**CJ-Q2 - the bar for "build".**
- (a) H1 alone: the settle's removal is justified by the failure point it
  deletes (H2), whatever S0 measures.
- (b) H1 and a measured gain outside the spread on at least one cell.

**CLA proposes (a)**: H2 is a correctness reason, and the measurement is
recorded either way.

**CJ-Q3 - the superblock.**
- (a) No bump. No binary reads the bit as "in flight" (H1), so a volume
  written after CJ means the same thing to a binary before it, and the
  reverse.
- (b) Bump, as every on-disk meaning change has since BB.

**CLA proposes (a) if S0 confirms H1 for every shipped tag** (v3.1.0
onward). Otherwise (b).

**CJ-Q4 - `ASSERT_COMMIT`'s replay arm.**
- (a) Kept as it is: an old log's record clears a bit nothing reads.
- (b) Kept as a no-op.
- (c) Refused `Corruption` on a volume written after CJ.

**CLA proposes (a)**: (c) needs the volume to say when it was written,
which is CJ-Q3 (b).

**CJ-Q5 - §1.8's crash-loser reservations.**
- (a) The bug entry is filed with this order (§1.8). S0 reproduces it
  with a rig cell for each direction (an arrival and a departure), and the
  entry gains the reproduction and names the order that would carry the
  fix. CJ goes on.
- (b) Fix it in CJ.

**CLA proposes (a)**: the fix is a recovery change (analysis-phase
winners and losers into the assertion pass) and CJ is a commit-path
change. **The departure direction fails open, and that ranks the fix
above CJ's own gain**. CLA recommends that the operator open its order
once S0 reproduces it, and not wait for CJ's close.

**CJ-Q6 - the abort's per-reservation cost.**
- (a) Out of scope.
- (b) Batch `ASSERT_ROLLBACK` per page in CJ.

**CLA proposes (a)**: (b) changes a record format, and nothing has
measured it. `tools/assertion_abort_benchmark.py` exists and no results
file names it.

**CJ-Q7 - `CommitTxn`'s signature.**
- (a) `void`, and the dispatcher's three failure arms (§1.2) and their
  stranded-settle comment are deleted.
- (b) Kept as `Status`, always OK.

**CLA proposes (a)**: an arm no failure reaches is a structure with no
consumer.

---

## 5. Stages

Every stage is reviewed by `critics-developer` and gated by the full
Debug suite. The measurement is held to the standing hour budget,
building included.

**CJ-S0 - the premise, verified and measured.** No engine code reaches
`main`. Four parts:

1. **The census (H1).** Every reader of the page's `kEntryReserved`, by
   `path:line`:
   - `src`, `include`, `tests`, `sim`, `tools`, `scripts`, `manual` and
     `docs`;
   - the backup manifest and restore checks (`backup.md`), any page dumper
     or verifier, and every `sys` view;
   - **every shipped tag from `v3.1.0`**, for CJ-Q3.

   A reader that decides anything refutes H1.
2. **The spike (H1, H2).** On the work branch, `CommitTxn` as CJ-R2 (no
   page, no record). Run:
   - the full Debug suite, the sim's assertion workload and the
     sanitizer builds (BS-S1);
   - `tests/assertion_*_test.cpp` and `tests/bound_cabin_test.cpp`.

   An expectation that fails is read: either it pins the bit's old meaning
   (rewritten under CJ-R1) or it found a reader (H1 refuted).
3. **The crash reading (CJ-R5, §1.8).** Cells on the spike, through the
   recovery rig:
   - a committed reservation whose page reached the device with the bit
     set, then a mount: the aggregate, the linkage scan and
     `VerifyAgainstEntries` agree with the base binary;
   - the same across a checkpoint;
   - AS6b's two orders (abort before and after a checkpoint) unchanged;
   - **a crash with a reservation undecided, once as an arrival and once
     as a departure**, which records what the base binary does (§1.8): the
     group total after the mount against a re-summation of the surviving
     rows. This is CJ-Q5's evidence;
   - a log carrying `ASSERT_COMMIT` records (from the base binary),
     mounted by the spike (CJ-Q4).
4. **The measurement (H3).** Build-release, the spike against the base,
   interleaved A/B in one window, server CPU per commit with the driver
   unpinned from the server's CPU:
   - assertions on the relation 0, 1 and 4;
   - reservations per transaction 1, 10 and 100;
   - `durability` `relaxed` (the CPU share alone) and `group` (with the
     sync);
   - `cores` 1 and 2;
   - the relation with no assertion as the control, which must not move.

   Driver: `tools/assertion_benchmark.py`, extended if it lacks the
   transaction-size knob.

The verdict goes in §7.

**CJ-S1 - the change**, on the operator's mark. In scope:
- CJ-R2 and CJ-Q7's signature;
- CJ-R1's comment at the definition;
- CJ-Q4's replay arm;
- the tests the spike found pinning the old meaning, rewritten.

**CJ-S2 - the verification.**
- **(i)** A committed reservation's entry carries the bit, and every
  consumer answers as the base did. The consumers are admission, the
  linkage scan, `VerifyAgainstEntries`, the fold and `sys.assertions`.
- **(ii)** A mutant that makes the linkage scan or `VerifyAgainstEntries`
  skip a reserved entry fails a cell (CJ-R4).
- **(iii)** S0 part 3's crash cells kept as tests.
- **(iv)** A `CommitTxn` under a full pool (drain mode) and an injected
  append failure: the transaction commits, and the totals equal a
  re-summation (H2).
- **(v)** The overhead A/B against the base: every cell inside the spread
  or better, the control unchanged.

**CJ-S3 - the close.** Documents to update:
- `assertion.md` §5.1's `flags` row, §6.2 step 4, §7's record list and
  AS6b's cost paragraph;
- `wal.md:168`;
- `cabin_bound_page.hpp`'s definition comment (`:64-73`) and its
  `Write`/flag-move comment (`:210-217`);
- the dispatcher's comments at `:10685-10693` and `:11653-11660`, the
  second's "Fail-closed" corrected (§1.6);
- `assertion.md:477-478`'s crash-recovery sentence, corrected to what the
  tree does (§1.8);
- the bug entry: its failed-settle half closed by CJ, its crash half left
  open;
- `known-gaps.md` (an assertion section naming §1.8's outcome);
- CLAUDE.md, if a row states the settle.

The stage log goes in §7.

---

## 6. Out of scope

- A deferred cleanout of tuples (§0): there is no commit-time tuple write
  to defer.
- A writer id on the entry, option (c) (§0).
- The abort's batching (CJ-Q6 (a)).
- The crash-loser fix (CJ-Q5 (a)).
- A change to admission, holds, `ASSERT_RESERVE`, AS6a or AS6b.

---

## 7. Stage log

Not opened.
