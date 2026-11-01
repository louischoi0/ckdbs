# Work order CK — an assertion reservation's commit settle, removed: an exploratory and verifying milestone

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

**Written 2026-10-31; opened 2026-10-31 on the operator's standing go-ahead
(go-ahead-achieving-milestone, `raft-marks-2026-10-31.md` §4)**, CK-Q0..Q7
adopted as CLA proposed. The adoption is CLA's under that word, not the
operator's per-item mark. CK-Q0 (a) reserved S0's verdict to the operator,
who marked it **build** on 2026-11-01 ("build, go ahead with S1"); **closed on
the work branch 2026-11-01** (§7, "CK close"), unmerged. **Exploratory and verifying since W2** (§3): CK-S0
verifies the premise and measures the settle before anything is built,
and its verdict is build, narrow or drop.

**Claim tags**: `[operator]` for W0-W2; `[source-read]` with `path:line`
at `18264195`; `[measured]` with its results file; `[design]` for
reasoning about code not yet written; `[quiet-wrong]` where a wrong choice
turns a correct answer into a wrong one with no error.

**The letter.** This order was written as **CJ** (`e4f55324`,
`c0814a6b`). **It was relettered CK on 2026-10-31 on the operator's word**
that CJ is already in use (W3, `raft-marks-2026-10-31.md` §2): another
session holds the letter for work not yet on `origin/main`. `[source-read]`
At `c0814a6b`, no branch on `origin`, no commit subject and no document
names a `CK`. The bug entry and its citations moved with the order.

**CK against other orders.**
- CK amends `docs/spec/assertion.md` §5.1 (the `flags` row), §6.2 step 4,
  §7's record list and AS6b's "Cost asymmetry, stated", and `wal.md`'s
  assertion row (`docs/spec/wal.md:168`).
- It leaves the admission, the hold, the reserve, `AbortTxn`,
  `ASSERT_ROLLBACK`, AS6a's snapshot and fold, and AS6b's `kEntryOrphaned`
  as they are.
- It does not take up a deferred cleanout of tuples. §0 says why it is not
  needed.
- Not part of AR0 §8's chain.

---

## 0. What CK is

**The hypothesis.** A reservation's entry is written with `kEntryReserved`
set. At commit, `CommitTxn` fetches each page the transaction's
reservations landed on, appends an `ASSERT_COMMIT`, clears the bit and
stamps the page, all **before the transaction's commit record**. CK holds
three hypotheses:

- **H1 - the bit has no reader.** No admission, no recovery step, no
  verification and no tool reads the on-page `kEntryReserved` of a live
  entry, so its value after commit decides nothing (§1.3, to be confirmed
  by CK-S0).
- **H2 - removing the settle removes a failure point.** With the page work
  gone, `CommitTxn` takes the pending list out of the registry and cannot
  fail. The stranded-reservation path (§1.6) then has no failure to start
  from. A stranded reservation does not always fail closed (§1.6): a
  stranded departure leaves its group's total too low. So H2 is a
  correctness argument whichever way a reservation moves its group.
- **H3 - the settle costs something measurable.** It costs one page fetch,
  one record, one page dirtied and one stamp per `(assertion, page)` on
  every commit that reserved. This has not been measured (§1.7).

**What CK would do (option (b)).** `CommitTxn` stops touching pages and
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
"Fail-closed" without the second and third rows. It is wrong, and CK-S3
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

**CK does not fix the crash path** (CK-Q5). It matters to CK for two
reasons:
- **The fix tells winners from losers by `TXN_COMMIT`, not by the bit.**
  After a full replay today, a set bit nearly identifies a loser's entry,
  and the entry carries enough to undo it (tuple key, value, group,
  departure). But the bit is not exact: a crash between a transaction's
  `ASSERT_COMMIT` and its `TXN_COMMIT` leaves a cleared bit on a loser. So
  the analysis phase's winners and losers are the only sound source.
  CK-R1 takes the bit-based fix off the table for good, and CK-R4 binds the
  fix to that.
- **CK removes one of the two paths that strand a reservation** (the failed
  settle, §1.6). That leaves the crash path as the only one.

---

## 2. Rulings proposed `[design]`

**CK-R1 - the bit's meaning.** `kEntryReserved` means *"written by the
reservation path"*. It is set at reserve, as today, and never cleared on
the live path. A build entry never carries it. `assertion_replay.cpp:44`'s
check keeps its meaning unchanged.

**CK-R2 - the settle.** `CommitTxn` moves `pending_[txn]` out under the
directory latch and does nothing else: no page, no record, no stamp. With
nothing left that can fail, CK-Q7 decides its signature and the
dispatcher's three failure arms (§1.2).

**CK-R3 - `ASSERT_COMMIT`.** The record is no longer written. Its record
type keeps its number and is never reused. Its replay arm stays for a log
written before CK (CK-Q4).

**CK-R4 - no reader may trust the bit as "in flight"** `[quiet-wrong]`. A
future consumer that reads `kEntryReserved` as "this reservation has not
committed" would treat every committed reservation as undecided. Examples:
a verifier that skips reserved entries, a linkage scan that drops them, a
diagnostic view that subtracts them. Each would be wrong with no error.
Three guards:
- the definition's comment (`cabin_bound_page.hpp:64-73`) and §5.1 state
  CK-R1;
- a cell pins that a committed reservation's entry carries the bit;
- a mutant reader that trusts the bit fails a cell (CK-S2).

The ruling binds §1.8's fix as well. It tells losers apart by the
analysis phase's outcome and never by the bit.

**CK-R5 - the stranding paths.** After CK no commit-time step can strand a
reservation (H2), so the failed-settle row of the bug entry closes with
CK. §1.8's crash-loser path, which fails open for departures, is confirmed
by a cell in CK-S0 and left to its own order (CK-Q5).

**CK-R6 - what does not change.**
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

- **CK-S0 verifies H1 and measures H3 before anything is built.** It runs
  once CK is opened and CK-Q1..Q2 are marked. Its results file says, per
  hypothesis, held or refuted, with the evidence.
- **A spike may be written on the work branch to answer S0.** It is never
  merged: it is deleted, or rewritten as CK-S1.
- **The verdict is build, narrow or drop.**
  - **Build**: H1 holds.
  - **Narrow**: a reader is found that can be moved onto another fact, and
    CK-S1 moves it.
  - **Drop**: a reader is found that needs the bit's present meaning. It is
    recorded in §7 with what sank it, and the settle stays.
- **Nothing merges to `main` without the operator's word.** The redefinition
  of an on-page bit's meaning is the operator's to mark (CK-Q0).

---

## 4. Questions

**Adopted 2026-10-31 on the operator's standing go-ahead
(go-ahead-achieving-milestone), each as CLA proposed**: CK-Q0 (a), CK-Q1 (a),
CK-Q2 (a), CK-Q3 (a) if S0 confirms H1 for every shipped tag and otherwise
(b), CK-Q4 (a), CK-Q5 (a), CK-Q6 (a), CK-Q7 (a). Decided by CLA, on the
operator's word to follow its proposal; none is the operator's own mark.
CK-R4 and CK-Q1 are `[quiet-wrong]`: their premise (the bit has no reader,
§1.3) was re-read at `8d183e9b`, and `src` has not changed since `18264195`
(only `instructions/` and `docs/` commits), so the proposals stand and S0 part 1
re-verifies the census.

**CK-Q0 - who decides after S0.**
- (a) The operator marks S0's verdict.
- (b) S0's verdict decides, and the stages run on the standing go-ahead.

**CLA proposes (a)**: CK redefines what an authoritative page's bit
means.

**CK-Q1 - the bit's fate.**
- (a) Keep setting it at reserve, never clear it (CK-R1). No format
  change, and the replay check keeps its meaning.
- (b) Stop setting it. A reservation's entry then equals a build entry,
  and `assertion_replay.cpp:44` changes its rule.
- (c) Keep clearing it, lazily: not possible without a writer id (§0).

**CLA proposes (a)**.

**CK-Q2 - the bar for "build".**
- (a) H1 alone: the settle's removal is justified by the failure point it
  deletes (H2), whatever S0 measures.
- (b) H1 and a measured gain outside the spread on at least one cell.

**CLA proposes (a)**: H2 is a correctness reason, and the measurement is
recorded either way.

**CK-Q3 - the superblock.**
- (a) No bump. No binary reads the bit as "in flight" (H1), so a volume
  written after CK means the same thing to a binary before it, and the
  reverse.
- (b) Bump, as every on-disk meaning change has since BB.

**CLA proposes (a) if S0 confirms H1 for every shipped tag** (v3.1.0
onward). Otherwise (b).

**CK-Q4 - `ASSERT_COMMIT`'s replay arm.**
- (a) Kept as it is: an old log's record clears a bit nothing reads.
- (b) Kept as a no-op.
- (c) Refused `Corruption` on a volume written after CK.

**CLA proposes (a)**: (c) needs the volume to say when it was written,
which is CK-Q3 (b).

**CK-Q5 - §1.8's crash-loser reservations.**
- (a) The bug entry is filed with this order (§1.8). S0 reproduces it
  with a rig cell for each direction (an arrival and a departure), and the
  entry gains the reproduction and names the order that would carry the
  fix. CK goes on.
- (b) Fix it in CK.

**CLA proposes (a)**: the fix is a recovery change (analysis-phase
winners and losers into the assertion pass) and CK is a commit-path
change. **The departure direction fails open, and that ranks the fix
above CK's own gain**. CLA recommends that the operator open its order
once S0 reproduces it, and not wait for CK's close.

**CK-Q6 - the abort's per-reservation cost.**
- (a) Out of scope.
- (b) Batch `ASSERT_ROLLBACK` per page in CK.

**CLA proposes (a)**: (b) changes a record format, and nothing has
measured it. `tools/assertion_abort_benchmark.py` exists and no results
file names it.

**CK-Q7 - `CommitTxn`'s signature.**
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

**CK-S0 - the premise, verified and measured.** No engine code reaches
`main`. Four parts:

1. **The census (H1).** Every reader of the page's `kEntryReserved`, by
   `path:line`:
   - `src`, `include`, `tests`, `sim`, `tools`, `scripts`, `manual` and
     `docs`;
   - the backup manifest and restore checks (`backup.md`), any page dumper
     or verifier, and every `sys` view;
   - **every shipped tag from `v3.1.0`**, for CK-Q3.

   A reader that decides anything refutes H1.
2. **The spike (H1, H2).** On the work branch, `CommitTxn` as CK-R2 (no
   page, no record). Run:
   - the full Debug suite, the sim's assertion workload and the
     sanitizer builds (BS-S1);
   - `tests/assertion_*_test.cpp` and `tests/bound_cabin_test.cpp`.

   An expectation that fails is read: either it pins the bit's old meaning
   (rewritten under CK-R1) or it found a reader (H1 refuted).
3. **The crash reading (CK-R5, §1.8).** Cells on the spike, through the
   recovery rig:
   - a committed reservation whose page reached the device with the bit
     set, then a mount: the aggregate, the linkage scan and
     `VerifyAgainstEntries` agree with the base binary;
   - the same across a checkpoint;
   - AS6b's two orders (abort before and after a checkpoint) unchanged;
   - **a crash with a reservation undecided, once as an arrival and once
     as a departure**, which records what the base binary does (§1.8): the
     group total after the mount against a re-summation of the surviving
     rows. This is CK-Q5's evidence;
   - a log carrying `ASSERT_COMMIT` records (from the base binary),
     mounted by the spike (CK-Q4).
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

**CK-S1 - the change**, on the operator's mark. In scope:
- CK-R2 and CK-Q7's signature;
- CK-R1's comment at the definition;
- CK-Q4's replay arm;
- the tests the spike found pinning the old meaning, rewritten.

**CK-S2 - the verification.**
- **(i)** A committed reservation's entry carries the bit, and every
  consumer answers as the base did. The consumers are admission, the
  linkage scan, `VerifyAgainstEntries`, the fold and `sys.assertions`.
- **(ii)** A mutant that makes the linkage scan or `VerifyAgainstEntries`
  skip a reserved entry fails a cell (CK-R4).
- **(iii)** S0 part 3's crash cells kept as tests.
- **(iv)** A `CommitTxn` under a full pool (drain mode) and an injected
  append failure: the transaction commits, and the totals equal a
  re-summation (H2).
- **(v)** The overhead A/B against the base: every cell inside the spread
  or better, the control unchanged.

**CK-S3 - the close.** Documents to update:
- `assertion.md` §5.1's `flags` row, §6.2 step 4, §7's record list and
  AS6b's cost paragraph;
- `wal.md:168`;
- `cabin_bound_page.hpp`'s definition comment (`:64-73`) and its
  `Write`/flag-move comment (`:210-217`);
- the dispatcher's comments at `:10685-10693` and `:11653-11660`, the
  second's "Fail-closed" corrected (§1.6);
- `assertion.md:477-478`'s crash-recovery sentence, corrected to what the
  tree does (§1.8);
- the bug entry: its failed-settle half closed by CK, its crash half left
  open;
- `known-gaps.md` (an assertion section naming §1.8's outcome);
- CLAUDE.md, if a row states the settle.

The stage log goes in §7.

---

## 6. Out of scope

- A deferred cleanout of tuples (§0): there is no commit-time tuple write
  to defer.
- A writer id on the entry, option (c) (§0).
- The abort's batching (CK-Q6 (a)).
- The crash-loser fix (CK-Q5 (a)).
- A change to admission, holds, `ASSERT_RESERVE`, AS6a or AS6b.

---

## 7. Stage log

Opened 2026-10-31 in worktree `ck-assertion-commit-settle` at `8d183e9b`
(`origin/main` `d517cebc` and `main` `3f225096` merged).

### CK-S0 part 1 - the census (H1), 2026-10-31

`[source-read]` In worktree `ck-assertion-commit-settle` at `706beedb`, the
search was `kEntryReserved`, `reserved()`, `ClearReserved` and `SetReserved`
over `src`, `include`, `tests`, `sim`, `tools`, `scripts`, `fuzz`, `manual` and
`docs` (the bug entries aside).

- **One reader of the page-entry bit decides anything**:
  `src/exec/assertion_replay.cpp:44`, and it reads the **record's** entry
  bytes, never a page's (§1.3). Every other hit sets the bit
  (`assertion_check.cpp:563`), writes a build entry without it
  (`assertion_build.cpp:251`), clears it (`cabin_bound_page.cpp:192`,
  `assertion_check.cpp:869`, `assertion_replay.cpp:120`), defines it, or is a
  test or a comment. No `docs`, `manual`, `scripts`, `sim`, `fuzz`, backup
  manifest or restore check names it.
- **Shipped tags, all ten read** (`v3.1.0`, `v3.2.0`, `v3.3.0`, `v3.4.0`,
  `v3.4.1`..`v3.4.6`): each has exactly one `.reserved()` call outside the
  definition, `src/exec/assertion_replay.cpp:44`. CK-Q3 (a)'s condition
  ("S0 confirms H1 for every shipped tag") is met.
- **H1 holds at part 1.**

### CK-S0 parts 2-4 - 2026-11-01

`[measured]` In worktree `ck-assertion-commit-settle` at `b66a0dbe`
(`v3.4.6-12-gb66a0dbe`). The first build stopped at configure for want of
GTest, and then at a compile for want of `/tmp` space; both were the host's,
not the tree's (GTest installed by the operator, compiles run with `TMPDIR`
on the root disk).

**Part 2, the spike** (`CommitTxn` erases `pending_[txn]` and nothing else).
- **The base suite**, `-j6`, 4337 cells: 4332 pass, 4 not run (2 disabled, 2
  skipped), 1 failed (`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`);
  it passed alone, 5 of 5.
- **The spike suite**, `-j6`, 4337 cells: 4332 pass, 4 not run, 1 failed
  (`SortedLeafRigTest.NamedKeysFromTwoCoresInterleaveIntoOneLeafInKeyOrder`,
  `file_rig_crash.hpp:81`, a statement answered `ERR`). That cell declares no
  assertion, and **the base binary fails it too: 9 of 50 serial runs**, so it is
  a pre-existing flake and not the spike's. It is not filed as a bug here.
- **Every `assertion_*` and `bound_cabin` cell** (174) passes on the spike, so
  no expectation pinned the settle. No cell had to be rewritten, and none
  found a reader.
- **The sim corpus**, Debug `ckdbs-sim`, `SIM_BIN=build/ckdbs-sim
  scripts/sim.sh 4`, on the spike: 280 runs, 0 failures.
- **Not run: the sanitizer builds** (BS-S1). They cost a rebuild each and are
  left for CK-S2.

**Part 3, the crash cells** (`tests/assertion_crash_test.cpp`, `b66a0dbe`).
- A committed departure across a crash (`ACommittedDepartureFreesItsSlotAcrossACrash`)
  gives the same answer on the base and the spike.
- **CK-Q4**: an image written by the base binary, holding 2 `ASSERT_COMMIT`
  records, mounts on the spike and enforces; an image written by the spike
  (no `ASSERT_COMMIT`) mounts on the base and enforces. This is the evidence
  for CK-Q3 (a) in both directions, and it was run once by a throwaway cell
  that is not in the tree.
- **CK-Q5 (a), reproduced on the base binary**: an undecided **departure**
  fails open (a full group admits another row after the mount) and an
  undecided **arrival** fails closed (both probes refused where one should
  be admitted). The spike answers the same, so the path does not run
  through the settle. The two cells are `DISABLED_` and red when enabled;
  the bug entry carries the reproduction.
- **Not written: the same across a checkpoint**, and AS6b's two orders as new
  cells; the existing `AnAbortBeforeTheCheckpoint...` and `AnAbortAfter...`
  cells (`assertion_recover_test.cpp:1242`, `:1296`) pass on the spike. A
  checkpoint variant needs a checkpoint driven through the rig, and is left
  for CK-S2.

**Part 4, H3** (`bench/v3.4.6/results-ck-s0-settle-v3.4.6-12-gb66a0dbe.md`,
driver `tools/assertion_settle_benchmark.py`, a sibling of
`tools/assertion_benchmark.py`, which is unchanged). Release builds of both
trees, an interleaved A/B in one window, the driver pinned off the server's
CPUs; the full grid ran (0, 1 and 4 assertions, K = 1, 10, 100, `relaxed`
and `group`, 1 and 2 cores), with 1,000 preload rows only and 5 measured rounds.
- **Server CPU per commit: no cell is outside the spread**, and the control
  moves as much as the asserted cells. The meter is 10 ms `/proc` ticks (1 to
  85 µs per commit), and about one window in five ran at twice the CPU for an
  unidentified cause. So CPU cannot resolve the settle.
- **The `COMMIT` statement's latency shows it**, `relaxed` only: spike minus
  base at p50 is -0.3 to -0.9 µs at K=1, -0.7 to -2.7 µs at K=10, and -4.2 to
  -4.8 µs (one assertion) and -14.8 to -16.0 µs (four) at K=100, against a
  control of 0.3 µs or less. That is about 40 to 70 ns per reservation, under
  0.5 % of a K=100 transaction's wall time. Under `group` the settle is
  unresolved against the sync's noise, not shown to be zero.
- **Cut**: preload 200 and 10,000 rows, per-round percentiles, and a count of
  `ASSERT_COMMIT` records or pages on the base (the settle's cost is inferred
  from latency, not counted).

### CK-S0 verdict - for the operator's mark (CK-Q0 (a))

| Hypothesis | Result |
|---|---|
| **H1**, the page's bit has no reader | **Held**: one reader in ten tags, reading the record's bytes; no cell, view or tool reads a page's bit; 174 assertion cells and the sim corpus pass without the settle. Not covered: the sanitizer builds and a checkpoint-variant cell |
| **H2**, removing the settle removes a failure point | **Held by reading**: the spike's `CommitTxn` cannot fail. Not exercised by a cell: the full-pool / injected-append failure of CK-S2 (iv) is not written, and the failure it removes was never reproduced (§1.6) |
| **H3**, the settle costs something measurable | **Held in sign, small**: visible only in the `COMMIT` statement's latency, tens of ns per reservation; no server-CPU cell is outside the spread |

**CLA's reading of §3's rule: build**, on H1 and CK-Q2 (a), whose reason is
H2 and not H3. The operator marks it: CK-Q0 (a) reserves this verdict, because
CK redefines what an authoritative page's bit means. Nothing in CK-S1 has been
built, and the spike is not in the tree (its patch was kept outside it).

**Separately, CK-Q5's recommendation stands and is firmer**: the crash-loser
path is reproduced on the base, a departure fails open `[quiet-wrong]`, and CLA
recommends the operator open an order for its fix without waiting for CK's
close.

### CK-S1 - the change (2026-11-01, operator's mark: "build, go ahead with S1")

The operator marked CK-S0's verdict **build** on 2026-11-01 ("build, go ahead
with S1"), so CK-Q0 (a) is the operator's own mark and not an adoption.

Landed on `ck-assertion-commit-settle`: `AssertionEnforcer::CommitTxn` is
`void CommitTxn(txn_id)` and erases the pending set only (CK-R2, CK-Q7 (a));
the dispatcher's three failure arms are gone; `kEntryReserved`'s definition
states CK-R1 and CK-R4; `ReplayCommit` stays (CK-Q4); the cell
`ACommitLogsNoAssertCommitAndTheMountKeepsItsTotals` pins that no
`ASSERT_COMMIT` is written and the mount keeps a commit's totals.

**Review finding that corrects CK-R5 (not fixed here, not in scope).**
CK-R5's "no commit-time step can strand a reservation" is false. `CommitTxn`
still runs *before* `TransactionManager::Commit`, as it did on the base, so a
failed `Commit` reaches `AbortTxn` with an empty pending set: the reservations
stay in their group totals while undo takes the rows back (a departure or a
negative SUM fails open, `[quiet-wrong]`). `CommitLocal`'s failure arm never
calls `AbortTxn`, and the deferred path's `wal_->stopped()` abort finds the set
gone too. What CK removed is the settle's own failure, nothing else. The fix
(drop the set only after the commit succeeds, abort in `CommitLocal`) changes
what `ReserverOnLocked` sees on another core and is its own order, with the
crash-loser fix (CK-Q5). CK-S3 must not repeat CK-R5's sentence, and the bug
entry's failed-settle half stays open for this path.

### CK-S2 and CK-S3 (2026-11-01, worktree `ck-assertion-commit-settle`, `59167d99`)

- **(i)** `AssertionRecoverTest.ACommittedReservationsEntryKeepsItsBitAndEveryConsumerAnswersAsBefore`:
  entries written with `kEntryReserved` and never cleared; the linkage scan
  attaches all, the sums and `VerifyAgainstEntries` agree. Passes.
- **(ii)** The same cell runs a verifier that skips a reserved entry and
  requires it to fail. Passes (the mutant is caught). The linkage scan's own
  mutant was not written.
- **(iii)** S0 part 3's crash cells are kept in `tests/assertion_crash_test.cpp`
  (the committed departure cell enabled, the two crash-loser cells `DISABLED_`
  for the order that fixes them), plus
  `ACommitLogsNoAssertCommitAndTheMountKeepsItsTotals`.
- **(iv)** **No cell, by construction**: `CommitTxn` is `void` and takes no
  store and no log, so there is no pool refusal or append failure to inject.
  H2 is carried by the signature, not exercised.
- **(v)** `bench/v3.4.6/results-ck-close-v3.4.6-15-g59167d99.md`, base
  `8d183e9b` against `59167d99`, release builds, interleaved, driver unpinned:
  `relaxed` with assertions, the `COMMIT` p50 is shorter in every cell and never
  longer (4.2 to 5.9 us at K=100 with one assertion, 14.7 to 17.8 us with
  four); the control is unchanged on one core and moved up to 1.5 us on two,
  which is the floor. `group` and server CPU resolve nothing, as in S0, and no
  `group` cell is worse. Cut: preload 200 and 10,000, `strict`, per-round
  percentiles.
- **Review** (`critics-developer`): the diff correct; six stale comments and a
  dead `#include <map>` fixed; **it found CK-R5's claim false** (the entry
  above). Rejected: renaming `CommitClearsTheReservedFlagInPlace` in
  `bound_cabin_test.cpp` (stale name, left so test ids hold; to rename with the
  next touch).
- **Suite**: Debug `ctest` at `9fd02f2e` (comment edits included) 4363 of 4363
  passed, and the assertion and Bound Cabin cells (148) pass under gcc
  ASan+UBSan on `59167d99`. The first run's one failure was
  `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, the known flake,
  passing alone. **Not run**: clang ASan/TSan (no clang on the host,
  `scripts/bs/install-llvm18.sh` not run), the sim corpus on the final tree, a
  checkpoint-variant crash cell and `scripts/bs/mutants.sh`. The full suite was
  not re-run after the two commits that touched only tests and docs
  (`59167d99`'s tests: the 148 above and the new cell ran).
- **CK-S3**: `assertion.md` (the `flags` row, §6.2 step 4, the record list,
  the recovery sentence, the cost paragraph), `wal.md`, the bound-page and
  enforcer comments, the dispatcher's comments (the stranded-settle and the
  "Fail-closed" paragraphs deleted with their arms), the bug entry and
  `known-gaps.md`. CLAUDE.md states no settle in any row, so it is unchanged.
- **`origin/main`** fetched at close: nothing beyond the earlier merge.

### CK close

CK is closed on the work branch: build verified (H1), H2 by signature, H3
small and sign-correct. The reason to have built it is one fewer failure point,
not speed. **Two fail-open paths remain and are not CK's**: the crash loser
(CK-Q5) and a failed `TransactionManager::Commit` (CK-R5's correction); CLA
recommends one order for both. Push, merge to `main` and any tag wait for the
operator.
