# Ratification marks — 2026-09-08

**The operator's marks of 2026-09-08**, verbal, on four rows of
`workorder-ao-m2-lock-family.md`'s AO-0 table. Recorded by CLA against
`main` at `bc1040e` (`v2.7.0-*`; the v3.0.0 tag is still not cut, so
every commit named here carries the v2.7.0 describe).

Each row names the mark, what it obliges, and what it does **not** settle.
Per AR0's standing rule a CLA proposal is accepted by default except where
it fixes a constant or converts a refusal into a possible silent wrong
result; three of the four rows are in the first class or are user-visible,
which is why they are marked here rather than adopted silently.

The 2026-09-08 mark that benchmarks run **BTREE only for now** is a
separate act on `workorder-as-sus1-heap-suspended.md` AS-Q6 and is not
recorded in this document (§5).

---

## 1. AO-0 item 1 / AR2 E2 — the per-transaction borrow cap

| | |
|---|---|
| **Mark** | `max_locks_per_txn` = **65,536**; the refusal is **`ResourceExhausted`**, non-retryable, never escalated (AO-R10, AR2-R4) |
| **Class** | `[constant]`, user-visible |
| **Provenance of the value** | **`[provisional]`** — CLA's proposal from AR2 §7 and AO-R10, not a measured value. Guideline 2 applies: the number is recorded as provisional and stays so until a measurement names it or AO-S7 re-measures it |

**What the mark obliges.**

1. `kds.max_locks_per_txn` is the one name for the quantity (config key
   and constant alike). No second spelling.
2. The refusal is `ResourceExhausted` with **one new append-only detail**
   naming the cap and the transaction's count, per AO-R10. `status.hpp:69`'s
   comment widens to say the code now covers a borrow-count refusal beside
   its existing users (the lock table's own, the row codec's, the sort's,
   the aggregate's, the lease services' — `known-gaps.md`, Eviction).
3. **Non-retryable.** A cap cannot be waited out, which is why R4 declares
   it the one refusal the borrow model keeps. The reply's retryable bit is
   0; a client that retries the same statement meets the same cap.
4. **No escalation.** A transaction at the cap is refused; the engine does
   not silently widen its tuple borrows to a relation `X`. Escalation would
   turn a refusal into a wait that other transactions pay for without any
   record of why, and `[quiet-wrong]` on their side.
5. AO-S7 states whether the cap was reached in any C3 cell. If it was not,
   the value stays provisional with that fact recorded; if it was, S7's
   file names the cell and the count.

**What the mark does not settle.** Whether the cap is per transaction
across cores (the coordinator's borrows plus every participant's) or per
local transaction. AO-R10 counts per `Transaction`, which is per core; a
cross-owner transaction therefore has one cap per participant. That is the
reading built at AO-S2 (`Transaction::borrows_`) and stands until AT's
uniformity work asks the question again.

---

## 2. AO-0 item 5 — the subsystem's name

| | |
|---|---|
| **Mark** | The subsystem is named **`lock`**. `borrow` stays the AR2 concept name |
| **Class** | spec |

**Why `lock` and not `borrow`, stated so the choice is not re-litigated.**
AR2 §2 defines a *borrow* as the scope-bounded tenancy over one of five
units in **two families**: the page latch (critical-section scope, outside
the mode hierarchy) and the intention-moded locks (transaction scope, over
relation, range, slice and tuple). What M2 builds is the second family's
table. Naming that table's subsystem `borrow` would leave the latch family
outside a thing called `borrow`, against the definition it is meant to
implement. `lock` names the family the subsystem actually holds; `borrow`
names the umbrella and is used where both families are meant.

**What the mark obliges.**

1. AO-R1 stands as written; no rename. `LockTable`, `LockUnit`,
   `LockMode`, `LockKey`, `lock_table.hpp`, the `lock` log tag and
   `rules.md` §3's row keep their names.
2. Prose uses the two words by their definitions: *borrow* for the tenancy
   (either family), *lock* for the transaction-scoped family and its table,
   *latch* for the critical-section family. AO-S8's prose pass checks the
   documents it already lists for the three words being used
   interchangeably and corrects them; AR2's own body is exempt, since it is
   the definition.
3. `CLAUDE.md`'s Transactions row, when AO-S8 rewrites it, says "the lock
   table" for the mechanism and "borrow" for the model, in that order.

---

## 3. AO-0 item 6 / AO-R9 — waits at `cores = 1`

| | |
|---|---|
| **Mark** | Confirmed: at one core a write meeting an undecided holder **waits**, where it refused before M2 |
| **Class** | user-visible |

**This is a confirmation of shipped behaviour, not a change.** AO-S3
(`2026-09-04`) built the same-core wait for any in-flight holder, and
AO-S3b the mid-statement park; AO-S4a's detector is wired at
`core_count == 1`; AO-S5(a) found that a *server's* core 0 had no lock
table until that stage and gave it one, and AO-S4b handed the dispatcher
the instance table on every core. So a single-core server has waited since
`d8f873c`'s predecessor merge and the mark records the operator's word on
what those stages did.

**What the mark obliges.**

1. The zero-overhead-at-one-core rule holds and is stated per mechanism:
   a same-core wait needs no wake (the holder's decide runs on the same
   reactor, `WaitUntil` is a poll on the next tick), the partition latch is
   null at one core (`base/latch.hpp`), and the detector walks a graph
   that at one core is the instance's. Nothing in the wait path takes a
   cross-core primitive at `cores = 1`. AO-S7 measures this rather than
   asserting it: one cell at `cores = 1`, the refusal engine (a commit
   before AO-S3) against `HEAD`, interleaved.
2. `client-manual.md` says a conflicting write waits, and names the fault
   net (`kLockWaitFaultNetNs`, 11 s while 2PC is in the tree) as the only
   clock-ended exit, and the deadlock refusal as the only other. AO-S8
   verifies the manual says this; if AO-S3 already wrote it, S8 records
   that.
3. A refusal that AO-S3's row lists as *kept* (the cap, R4; RepeatableRead's
   exclusion from the wait, AO-S3b) is not converted by this mark. The
   RepeatableRead exclusion is AO-S6's to lift or keep, with its own
   cells.

---

## 4. AO-0 item 11 — the partition latch

| | |
|---|---|
| **Mark** | **Mutex first** (`base/latch.hpp`'s `std::mutex`, AO-R2's S1 default). A spin primitive only if AO-S7's numbers ask for it |
| **Class** | design; measurement-gated |

**This mark amends AR0 D2(a).** `ar0-architecture-revision.md:87` wrote
"spinlocks (atomics)" for the table's partitions and AR0-M2 left the
primitive undecided. The mark takes the mutex; AR0-M's D2 row gains an
*amended 2026-09-08* note pointing here, so the two documents do not
disagree.

**What the mark obliges.**

1. `LockTable`'s partitions stay on `base/latch.hpp`, which is a
   `std::mutex` at `cores > 1` and null at one core. No atomic is added to
   the partition for this mark, so `rules.md` §3's atomics census does not
   move.
2. **The switch condition is written before S7 runs**, not read off its
   result. CLA proposes the following for AO-S7's order, to be marked
   there:
   - the cell: C3's contended arm — N sessions across `cores = 8` updating
     one row, `group` durability, `build-release`, interleaved with the
     same cell on the uncontended arm;
   - the quantity: the partition latch's share of the update's wall time,
     read from the `SHOW META` accounting AO-S1 added (or, if none
     attributes to the latch, added there first and the cell re-run);
   - the threshold: a spin primitive is tried **only if** that share
     exceeds the same cell's cross-core wake cost, which is the number the
     hop already costs (C1: ~46% of a statement's wall time with the hop,
     AL-S8's figure). Below it, the latch is not the bottleneck and a spin
     buys nothing measurable;
   - the comparison, if tried: mutex against spin on the same host,
     interleaved, both arms in the file, and the spin adopted only on a
     delta both repeats agree on.
3. AO-S5(a)'s finding stands separately: `ClearWaitFor`'s `wait_latch_`
   was a real mutex on every commit path and is now gated on an atomic
   edge count. That gate is not a partition latch and this mark does not
   reach it; it is listed so nobody reads "mutex first" as licence to put
   one back on the commit path.

---

## 5. What these marks do not reach

Stated so the AO-0 table's remaining rows are read as still open.

| row | state |
|---|---|
| AO-0 item 2, the partition count (64 × cores) | `[constant]`, still CLA's proposal; AO-S7 re-measures per AO-R2 |
| AO-0 item 3, the fault net and the cadence | the net's value stands per AO-R8; **the 100 ms cadence is not built and will not be** — AO-S4a and AO-S4b detect at registration, and a cadence walk finds nothing a registration did not. AO-R7 needs an amendment recording that, which this document does not make |
| AO-0 item 7, `in_doubt_ceiling_ms` | inert since AO-S3; refuse at startup or keep until M3 — open |
| AO-0 item 8, READ UNCOMMITTED's order | open |
| AO-0 item 9, the FK split (F3's wait in M2, D9(a)'s fence in M3) | open; AO-S5(b) built F3's cross-core wait, so the M2 half is done and the row awaits confirmation |
| AO-0 item 10, the `rules.md` §3 row's interim "declared in" | open |
| The 2026-09-08 mark **benchmarks run BTREE only** | AS-Q6's second class (tools that emit explicit `HEAP`); recorded against `workorder-as-sus1-heap-suspended.md`, not here. Its own open item — whether `f6ed10c` is re-measured under BTREE drivers as AM-S6's baseline, or `HEAD` alone becomes the first BTREE baseline — is the operator's next word |
| The instance-wide in-flight bit (`IsInFlight` over `InstanceVisibility`'s per-core slots) | discussed 2026-09-07, not proposed in any order yet; its bearing on AO-S6's cross-core half is stated in that discussion and nowhere in the tree |

---

## 6. Where the marks land

- `workorder-ao-m2-lock-family.md` AO-0 rows 1, 5, 6 and 11: the
  "CLA proposal / state" cell gains **marked 2026-09-08** with a pointer to
  this file.
- `ar0-architecture-revision.md` AR0-M, D2's row: *amended 2026-09-08
  (partition latch: mutex first; `raft-marks-2026-09-08.md` §4)*.
- `instructions/v3.0.0/index.md`: this file's row, one line, beside
  `raft-marks-2026-09-05.md`.
- No engine code changes on this mark. The cap's config key and refusal
  detail are AO-S6's to build with the units, not this document's.

---

*CLA's note at recording, 2026-09-08, on `ao-marks-2026-09-08` from
`bc1040e`: the text above is the operator's, verbatim. Two of §5's rows
were answered by the operator later the same day and are recorded where
those answers land — the AS-Q6 question (`f6ed10c` **is** re-measured
under the BTREE drivers as the baseline; `raft-marks-2026-09-08-as-q6.md`)
and the in-flight bit (proposal accepted as a per-core, single-writer,
active-bit-only publication; `workorder-ax-inflight-publication.md`).
Neither row is edited here: this document records what stood when it was
written.*
