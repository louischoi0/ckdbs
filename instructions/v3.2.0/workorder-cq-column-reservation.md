# Work order CQ — column reservations: a numeric delta that does not hold its row, an exploratory proof of concept

Written 2026-11-11 on `worktree-cq-lock-free-reservation` from `adbe2638`
(`v3.4.7-1-gadbe2638`), brought to `origin/main` `5d858feb` by the merge
`018bece3` (`v3.4.7-11-g018bece3`) the same day, on the operator's words,
recorded in English (`raft-marks-2026-11-11.md` §6):

- **W0**: *"Among the features KDBS could borrow, is there an
  OLTP-specialized feature of Oracle's?"*
- **CLA's answer to W0**: four candidates, ranked - Lock-Free Reservation
  (23ai), Transaction Guard, immutable tables, priority transactions - with
  Lock-Free Reservation first. The answer was given from `CLAUDE.md` alone
  and **missed `docs/conceptnotes/cn7-delta-verbs.md`**, whose rulings bear
  on it directly (§1.2, CQ-Q1).
- **W1**: *"Write a milestone document that borrows feature 1. It is an
  exploratory (experimental, empirical) milestone and the proof of concept
  comes first. Adopting it must weigh cost against benefit. Write the
  milestone document first."*
- **W2**: *"No, not a concept document - write a work order under
  instructions."*

**Opened 2026-11-11** on the operator's W3 (`raft-marks-2026-11-11.md`
§7: *"Start this work in 40 minutes."*): **CQ-Q1 (a) is the operator's own
mark**, and CQ-Q0 and CQ-Q2..CQ-Q9 are adopted as CLA proposed by the same
choice (§5). **CQ-S0 ran the same day; by CQ-Q6 (a) its verdict is build**, and it found that under `group` a client writing the row last already has the whole gain, which CLA carries to the operator as CQ-Q10 (§8). **CQ-Q10 (a) was marked the same day and CQ-S1 written (§9)**: C1 holds on paper after three corrections, its bill is itemised, and CQ-Q11..CQ-Q16 were put to the operator before S2. **CQ-Q11..CQ-Q16 were marked (a) on 2026-11-12 and CQ-S2, the reduced spike, ran (§8)**: a debit written first no longer holds its row - `4,584 tps` against `1,760 tps`, level with the client's own remedy - and the bar's third clause, the off-path cost, is inside the spread but not shown to be zero. **CQ-S3: the operator's verdict is narrow, 2026-11-12 (§8)** - C1 is not adopted, the spike is reverted, and what survives is the manual's advice to write the shared row last, A1's pending question and this order's model and numbers. **CQ is closed and landed on `main` by a squash merge on the operator's word of 2026-11-12** (`raft-marks-2026-11-11.md` §11): one commit, so the spike's commits are in the work branch's history only. **Exploratory on W1** (§4): CQ ends in **adopt, narrow or drop**
with §3's ledger filled in, and nothing it builds reaches `main` as a
feature (CQ-Q2). Reviewed on 2026-11-11 by the `critics-developer` pass
(§8).

**Claim tags**: `[operator]` for W0-W2 and for CN-7's rulings;
`[source-read]` with `path:line` at `018bece3`; `[measured]` with its
results file; `[recalled]` for what CLA states of Oracle from memory,
unverified until CQ-S0 reads Oracle's documentation; `[design]` for
reasoning about code not written; `[quiet-wrong]` where a wrong choice
turns a correct answer into a wrong one with no error; `[loud]` where a
choice adds a wait or a refusal a client sees.

**The names.** *Reservation* and *pending* already have an owner in this
tree: an assertion's (`ASSERT_RESERVE`, `docs/spec/assertion.md` §6). This
order says **column reservation** for a transaction's journaled delta,
**the column's reserve** for the instance's per-row tally of them, and
**assertion reservation** for the old thing, every time (CQ-Q7).

**CQ against other orders and notes.**

- **CN-7** (`docs/conceptnotes/cn7-delta-verbs.md`) is the nearest prior
  text, and the operator's rulings in it exclude this feature as written
  (§1.2). The operator set them aside for the spike only (CQ-Q1 (a));
  at the close they stand as ruled (§8, CQ-S3).
- **CP** (`workorder-cp-lock-model-exploration.md`, closed and on `main`
  in `v3.4.8`; through S2 when CQ was written) measured the lock family's price and built
  the `U` mode. CQ's premise starts from CP-S0's numbers (§1.4) and the
  mechanism CLA proposes rests on `U` (§2, C1).
- **BJ** (`instructions/v3.0.0/workorder-bj-expression-update.md`) gave
  `SET` its expressions; `SET c = c - 100` is the statement a column
  reservation reads. CQ adds no verb.
- **CK and CL** removed an assertion reservation's commit-time page work
  and fixed its crash-loser and failed-commit leaks. CQ puts page work at
  commit for one class of transaction (§3, K1) and must not reopen CL's
  class of defect (CQ-R3).
- Not part of AR0 §8's chain.

---

## 0. What CQ is

Oracle 23ai's Lock-Free Reservation `[recalled]`: a numeric column is
declared `RESERVABLE`; an `UPDATE` of the form `SET c = c ± <expr>` on a
row named by its primary key **does not lock the row**. It records the
delta in a per-table journal, checks the column's `CHECK` constraint
against the committed value and every pending delta that could worsen it,
and the row is locked and written **at commit**, for the length of the
apply. Two transactions that each debit one account, and then go on to do
other work, no longer wait on each other from the statement to the commit.

**The question CQ answers** is whether that buys anything *on this engine*
and what it costs here. Oracle's case does not answer it: the window the
feature closes is narrower in KDS, where under `group` a row's hold is
released before the sync and the lock family's waits measure `0.03 %` of
DB time on the contended scenario (§1.4). Three questions in order, each
able to end CQ:

1. **Is there a benefit to reach?** A shape a client plausibly sends on
   which writers of one row lose throughput to the row's hold, by an
   amount a cheaper remedy does not already recover (CQ-S0).
2. **Can the concept be sound here?** A model with its bound check, its
   commit-time apply and its wait rules argued before code (CQ-S1).
3. **Does a spike realise the benefit at a stated cost?** (CQ-S2), with
   the verdict and §3's ledger at CQ-S3.

---

## 1. The tree as read

### 1.1 What a numeric delta is today `[source-read]` at `018bece3`

- `UPDATE t SET c = c - 100 WHERE <tuple key> = k` is one statement since
  BJ: the expression is typed at compile and evaluated per row against the
  before-image (`docs/spec/types.md` §3.2b). Nothing marks it commutative:
  it is an overwrite, logged as an image.
- **The row is held from the statement to the decide.** A point write that
  declares no key window asks the tuple unit per row
  (`src/server/command_dispatcher.cpp:10212`, `BorrowChain` over
  `LockKey::Tuple`) - in `U` for an `UPDATE` since CP-S2b - and then
  `CheckWriteConflictBlocking`; the header's `trx_id` names the writer
  until it decides. A second writer of the row waits for that decide
  (`docs/spec/txn.md` §5).
- **`U` passes over `S`; `X` does not** (`include/kds/txn/lock_table.hpp:357-360`):
  an `UPDATE` proceeds beside a key share, a `DELETE` waits for it.
- **A decided-newer version is refused, not re-read.** Under `READ
  COMMITTED` a statement whose snapshot predates the row's last commit is
  refused `TxnConflict`, retryably: *"KDS aborts retryably rather than
  re-reading ... there is no re-read loop"* (`docs/spec/txn.md:772-776`).
- **An explicit transaction lives at most 60 s**
  (`txn_lifetime_ceiling_ms`, `docs/spec/txn.md` §1), so a row hold - and
  a column reservation, if one existed - is bounded by it.
- **There is no row-level `CHECK` constraint.** The only `CHECK` the
  parser reads is `CREATE ASSERTION`'s (`src/parser/parser.cpp:2152`). A
  bound such as `c >= 0` is written today as a `WHERE` guard.
- **No word of this feature is in the tree**: `grep -i
  'reservable\|escrow'` over `src`, `include`, `docs/spec`, `docs/rules`
  and `manual` returns nothing.
- **Savepoints do not exist** (`docs/spec/txn.md:1162-1167`), so no
  partial rollback gives back part of a transaction's reservations.

### 1.2 CN-7, the operator's rulings `[operator]`

`docs/conceptnotes/cn7-delta-verbs.md` (2026-09-26, revised 2026-10-10)
records the operator's proposal of `INC`/`DEC` and three rulings:

| # | ruling | what it meets in a column reservation |
|---|---|---|
| R1 | No implicit conversion: a `SET` is never turned into an `INC`/`DEC` by the engine | A reservation read off `SET c = c - 100` is the engine treating a `SET` as a delta |
| R2 | Universal: **no column declaration** - *"no `ESCROW`, no `RESERVABLE`"* | Oracle's shape is a column declaration |
| R3 | `RETURNING` at admission returns the value the write produced | Under a reservation that value is not known until commit |

CN-7 §6 places the concurrent-hold ledger outside itself: *"if the ledger
comes it comes without a declaration, opened where contention is observed
... decided by the physical optimizer, under a note of CN-1's family,
after AX has landed and AT-S13's driver has been re-run without the wait
gap."* Since that sentence was written, AX landed, the driver was re-run
(CP-S0's M4, §1.4), and the physical optimizer was deleted (BZ; CF-Q6 (b)
restated the project's purpose). None of that amends a ruling: R1-R3
stand until the operator says otherwise, which is why CQ-Q1 is the
operator's alone and CQ is not opened on a standing go-ahead.

### 1.3 The engine already has one escrow `[source-read]` at `018bece3`

`docs/spec/assertion.md`: `CREATE ASSERTION ... GROUP BY (...) CHECK
SUM(col) <= N` keeps a per-group running aggregate and admits a write by
**reserving** its delta into the group, under the registry's latch, so two
cores cannot both fit a group that has room for one (§6.1); a refusal
caused by an undecided reservation **waits** for that decide and is in the
wait-for graph (AO-S6e-c, §6.2). That is escrow, over a group of rows.
What it does not give a balance column: the bound is **one integer literal
for every group** (§3); **upper bounds only** (AS11), a floor being
`NotImplemented`; and the quantity is a `SUM` over rows to compute, not a
cell to read. It is priced as the alternative A3 (§3) and the candidate C2
(§2). Its reservations are **logged**, which is what made a crash loser's
delta a `[quiet-wrong]` defect until CL.

### 1.4 What is measured so far `[measured]`

`bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md`, `cores = 4`:

| cell | number | what it says to CQ |
|---|---|---|
| M1, point `UPDATE`, one session | the lock family's whole share is about `0.4 us`, about `1 %` of a round trip | Removing a row ask saves nothing measurable on an uncontended write |
| M3, scenario 2, 8 bookers, `group` | `2,374 TPS`; 84 lock waits, all granted, p50 `76 us`; `app.lock_wait` **`0.03 %` of DB time**; `commit.wal_flush` `44 %` | On this scenario the hot row's hold is not what bounds throughput |
| M4, 8 sessions on one row, **autocommit**, an absolute `SET`, `relaxed` | **`2,519` of `32,000` hot statements refused `TXN_CONFLICT`** - §1.1's decided-newer refusal, not a lock refusal; 17 parked waits, mean `40 us` | Under autocommit a client sees the **refusal**, about `8 %`. A column reservation does not reach this shape (CQ-R7); A1 does |
| M6, two sessions, `group` against `strict` | under `group` the second writer answers before the first's `COMMIT` ack in 300 of 300; under `strict` above one core it waits the sync (305 waits, p50 `440 us`) | Under `group` the hold ends before the sync. Under `strict` it spans the sync by design (BA-S7) |

**Not measured anywhere**: a row held by an **explicit multi-statement
transaction** across its later statements and its client's round trips.
That is the window a column reservation closes, and it is CQ-S0's first
cell.

---

## 2. Candidates `[design]`

Settled in CQ-S1, never before CQ-S0's verdict. Written here so §3's
costs have something to be the cost of. The first draft of C1 had two
`[quiet-wrong]` holes the review found (§8); what follows is C1 with them
closed on paper, and S1 owes the argument in full.

### C1 — a delta journal, applied at commit, with the row shared (CLA's proposal)

- **At the statement.** A reserving `UPDATE` (§2.1) takes the relation
  `IX` as every write does and the row in **`S`**, the mode a child writer
  takes on its parent row (D9(a)). It writes **no version**. After the
  bound check it adds its delta to **the column's reserve** - an
  instance-wide, memory-only entry for `(relation, tuple key, column)`
  holding the undecided debits and credits - and to its transaction's
  journal.
- **Why `S`.** Reservations of one row coexist. A `DELETE` asks `X` and
  waits, so no reserved row vanishes. An `UPDATE` asks `U` and passes over
  the share. Each reservation is one lock-table entry, so
  `max_locks_per_txn` already bounds them per transaction. The price: this
  is a *shared* row, not a lock-free one, and the name says so.
- **The bound check**, under the reserve's latch: read the reserve first,
  then the cell of the row's **latest committed version** - never the
  statement's snapshot. With a floor `MIN`, a debit `d` is admitted iff
  `committed - held_debits - d >= MIN`; held credits are not counted. With
  a ceiling `MAX`, the mirror, with credits in the debits' place.
- **A delta leaves the reserve only after its transaction's decide is
  published** - a commit after its window entry (`FinishCommitBody`,
  `src/txn/manager.cpp:287`; under a deferred `strict` commit that is
  after its sync), an abort after its undo. Between the apply and the
  publish the delta is in the reserve and not yet in "committed"; after
  the publish it can be in both for a moment. **Counted twice at worst,
  never zero times**: a false refusal is possible, a false admission is
  not (CQ-R2).
- **Every writer of a marked column goes through the reserve.** `U` passes
  over `S`, so an ordinary `UPDATE` of the column runs beside
  reservations: an autocommit delta (CQ-R7) runs the same check and holds
  its delta in the reserve until its own decide, and an absolute `SET c =
  5` on a marked column is refused by name. Without this an ordinary
  writer's undecided debit is invisible to a reserver's check.
- **At commit**, on the dispatcher's `COMMIT` path before the commit
  record: for each journal entry, in `(relation oid, tuple key)` order,
  take the row in `U`, mint a view **after** the grant (as `MintCheckView`
  does for the foreign-key check, `src/txn/manager.cpp:87`), read the
  latest version, **re-check the bound**, and write the new version
  through the ordinary per-row update path - undo, log record, hooks. The
  re-check makes any hole the argument missed a refused `COMMIT`, `[loud]`,
  rather than a committed violation.
- **At abort, and at a crash.** Nothing **of a reservation** is logged or
  persisted, so a transaction that never reached its apply leaves nothing.
  An apply that failed part-way (K2) has written ordinary versions with
  ordinary undo, rolled back like any loser's, live or at mount (CQ-R3).
- **Autocommit never reserves** (CQ-R7): it has no window to close.

### C2 — the assertion registry as the ledger

The reserve and the bound live in a Bound-Cabin-like group
(`assertion.md` §5), logged and authority-class, one group per row. It
reuses a built reservation protocol and its wait. It also inherits what
that costs: logged reservations (CL's class), the registry latch on every
admission, a snapshot at every checkpoint. The committed value must still
be written into the row at commit, so C2 saves no part of C1's apply.

### 2.1 What a reserving statement is, whichever candidate

`UPDATE t SET c = c ± <constant expression> [, ...] WHERE <tuple key> = k`
inside an explicit transaction, every assigned column marked and nothing
else assigned. What else may touch a marked column is the census's
(CQ-S0): a `SET` reading `c` into another column, `RETURNING c`, the
column in a `WHERE`, a set-oriented `UPDATE`, `UPDATE ... FROM`, `ON
CONFLICT DO UPDATE`'s arm.

---

## 3. The ledger: benefit against cost `[design]` until a stage fills it

**This table is CQ's deliverable.** Every row gets a number, or "not
measured" with the reason; S3's verdict is read off it. **Read at S3**:
§8's CQ-S3 entry carries each row's standing at the close.

### Benefits, each a hypothesis

| # | benefit | known so far | filled by |
|---|---|---|---|
| B1 | **The hold on a shared row shrinks from statement-to-decide to apply-to-decide, and the client did not have to arrange it.** C1's apply is mechanically what a client does by writing the hot row last (A2), so B1's size is at most A2's, and its distinct value is twofold: the client's transaction order is free, and **the bound's verdict arrives at the statement** - a debit that cannot fit is refused before the transaction does its other work, without holding the row | **S0**: debited first with 8 statements after it, `group`, hot `1,571 tps` against `4,437 tps` for the piped last-writer arm. **S2**: the reserved arm, debit still first, `4,584 tps` against hot's `1,760 tps` (`2.60x`) and the piped arm's `4,584 tps` - `100 %` of the gap; `1.59x` at 2 statements; under `strict` `1,184-1,198 tps` against `713-845 tps` and the piped arm's `1,230-1,240 tps` | **S2** |
| B2 | An explicit transaction's delta is never refused decided-newer: the apply reads at latest state. Autocommit is not covered (CQ-R7), and M4 measured autocommit | **S0**: every refusal on the hot row is decided-newer: under `group` `1.4-12.8 %` of attempts. **S2**: none in about `250,000` reserved transactions, against `1.5 %` (hot) and `4.6 %` (piped) beside them | **S2** |
| B3 | A waiter no longer ends at the 1 s fault net behind a long holder of the row | **S0** (M4'): `18` commits in 20 s and `134` fault-net refusals behind a 1.2 s pause. **S2**, the same cell reserved: `136` commits, no refusal | **S2**, one cell |
| B4 | The bound (`c >= MIN`) is a declared invariant enforced on every write of the column, not a `WHERE` guard a statement may omit | **S1** (§9.4): enforced on every statement that can move the cell - the reserve's check, `INSERT`'s, and a refusal of every other form (§9.8). No number | S1 |

### Cheaper alternatives the benefit is measured against

| # | alternative | cost | gives |
|---|---|---|---|
| A1 | **Re-run instead of refuse** for a decided-newer row under `READ COMMITTED` (`txn.md:772-776`'s deliberate choice, the operator's to change). The autocommit re-run under a fresh view already exists for key-window writes and for a stale descent (BA-S14), so this is one more arm of it, not a new loop | small; a contract change | M4's `8 %`, which C1 does not reach, and B2's `1.4-12.8 %` in explicit transactions if the re-run is admitted there (priced by count in S0, not built) |
| A2 | **The client orders its transaction**: the hot row's write last, pipelined with `COMMIT` in one write (BN-R7) | none in the engine; a manual page | **S0: the whole gap under `group`**, piped (`4,437 tps`) or with a round trip before `COMMIT` (`4,447 tps`, disjoint `4,402 tps`), where the client can reorder; a third of disjoint under `strict`; not the early verdict, not B2 |
| A3 | **An entries relation under an assertion** (§1.3): each debit an `INSERT`, the bound a `SUM` | built; every admission takes the registry latch and logs a reservation, and reading the balance is a `SUM` over a relation that only grows | concurrent debits under one uniform, fixed ceiling |
| A4 | Nothing: the measured waits are small (§1.4) | none | - |

### Costs

| # | cost | kind | filled by |
|---|---|---|---|
| K1 | **Commit does page work again**: one fetch, one version, one log record per reserved row, on a path CK just emptied of its last page work | CPU and latency, reserving transactions only | **S2**: `COMMIT` p50 `837 us` against the hot arm's `758 us` at `k = 8`; server CPU a transaction `420 us` against the piped arm's `415 us` |
| K2 | **`COMMIT` waits on a lock and can be refused for a conflict**, neither of which happens today: the apply meets an undecided writer of the row and waits for its decide; it can close a cycle (a reserver that also deletes the row, or a cycle through non-reserving holders) and be refused naming deadlock; it can reach the 1 s fault net; a full pool can refuse it a frame before its no-refuse window; the bound's re-check can fail; and a hook the apply runs - an assertion's `SUM`, a Transition, a unique check - can refuse | `[loud]` | **S1** (§9.6): 13 outcomes, 10 of them new, 5 before any write. **S2**: none occurred in the measured cells; the apply waited for the row's `U` `5,318-5,737` times a cell, all granted, p50 `18-19 us` |
| K3 | **Under `strict` the apply's `U` spans the sync** unless the deferred commit's order changes, which CQ does not touch. The shared window is then bounded by the sync rate as today | **S0**: the last-writer arms reach `1,233-1,300 tps`, a third of disjoint's `3,827 tps`; the piped arm's `COMMIT` p50 is `2,425 us` against `1,015 us` | **S1** (§9.7): stated as the contract - no throughput beyond the last-writer arm under a deferred `strict` commit |
| K4 | **A transaction does not read its own reservation** (Oracle's rule `[recalled]`): its `SELECT` of the column returns the committed value. An overlay breaks the scan kernel's reject-on-bytes (BM) for that column; a refusal breaks `SELECT *` | `[quiet-wrong]`, CQ-Q4 | **S1** (§9.7): the committed value, and a refusal of another column's `SET` reading it (§9.8 item 11) |
| K5 | **`RETURNING` cannot return the new value** (CN-7 R3) | a refusal by name | **S1** (§9.7): wider than the column - refused whatever the items |
| K6 | **A declared bound is new DDL surface** - the tree has no row `CHECK` - in a project whose scope is deliberately narrow | feature scope, CQ-Q1 | the operator |
| K7 | **A format event on adoption**: a column flag and a bound in the catalog, a superblock bump, and a bound-violation code in the wire's pinned registry. The spike has none (CQ-R5) | adoption only | S3 |
| K8 | **Refusals by interaction**: the column as a tuple key, an index key or covered column, a unique column, a foreign key, a Transition's column, an assertion's `GROUP BY` column, a Cabin key function's input, and an absolute `SET`; an assertion's `SUM` column needs its own answer | surface narrowed, each a truthful refusal | **S1** (§9.8): 17 refusals |
| K9 | **Every writer of a marked column pays the reserve**, autocommit included: a latch, the check, an entry held to its decide | CPU on marked relations | **not measured**: the spike refuses every other write of the column (CQ-Q15 (a)) |
| K10 | **The reserve is an instance-wide map**: its entries' lifecycle, a memory bound (`max_locks_per_txn` is per transaction, not per instance), and a hot row's entry latch moving between cores - the same line the row's lock entry moves on today | memory and a contended latch | **S1** (§9.2): bounded by the lock table's own bound, no setting; the latch a leaf lock. S2 (the contended latch) |
| K11 | **A debit the bound refuses only for undecided reservations waits on the reserve** (CQ-Q5 (a)) - a wait neither the header nor the lock table names, of the polled kind `txn.md:728` lists beside an assertion's group | a wait source to argue (CQ-R4), a poll | **S1** (§9.6): argued; **not built** - the spike refuses at once (CQ-Q5 (b)) |
| K12 | **The test estate**: the sim's oracle has no model of a write that lands at commit, and the PostgreSQL twin has no statement to diff against | hardening owed on adoption | S3 |
| K13 | **`REPEATABLE READ`**: the apply writes over versions the transaction's view never saw. Refused in the spike; a contract owed on adoption | CQ-Q8 | **S1** (§9.7): refused; an explicit `REPEATABLE READ` transaction cannot write the column |
| K14 | **A second visibility rule, and three functions on every transaction's path**: the apply writes over a committed version its view may not cover (§9.1, CQ-Q14); `CommitLocal`, `FinishCommitBody` and `Abort` each gain a test, among fifteen sites (§9.9) | `[quiet-wrong]` for the rule; CPU for the sites, CQ-R1 | S1 (found). **S2**: the spike's binary against the base's on unmarked relations - throughput ratios `0.976-1.004`, ranges overlapping in all six cells, the spike's median lower in five and its server CPU a transaction higher by `1-5 us` in all six: inside the spread, not shown to be zero |

---

## 4. Rules and method `[design]`

**Rules every candidate keeps**

- **CQ-R1 - off the path, nothing.** A statement on a relation with no
  marked column answers byte for byte as at the opening commit and costs
  nothing outside the spread. The four contract suites stay green.
- **CQ-R2 - the bound is never violated in a committed state.** A false
  refusal is admissible; a false admission is `[quiet-wrong]` and ends the
  candidate.
- **CQ-R3 - nothing of an undecided reservation is durable.** No record of
  a reservation, no page stamped, no snapshot. The apply's records are
  ordinary writes of a transaction that has not yet committed, undone like
  any loser's.
- **CQ-R4 - a wait is the header's or the lock table's** (`txn.md` §5), or
  it carries a soundness argument as explicit as `lock_table.hpp`'s
  publish-then-verify. K11's wait is the one that needs it.
- **CQ-R5 - the spike is not a format event.** No record kind, no
  superblock bump, no catalog column, no SQL surface, no wire code; the
  mark and the bound reach the spike through a hook the branch alone
  carries.
- **CQ-R6 - the invariants stand**: the 20-byte MVCC header (12),
  fixed-length tuples (13), K1 and K2.
- **CQ-R7 - an autocommit statement never reserves.**

**Method**

- **Measure first** (CF's method, CP's): S0 writes no engine code and
  measures the ceiling before anyone designs a way to reach it.
- **The spike is evidence, not a feature**: the narrowest thing that can
  realise B1 - one `int64` column, a floor, point updates by tuple key,
  `READ COMMITTED` - on the work branch only.
- **Every stage can end CQ.** Its entry in §8 says build, narrow or drop,
  with the number that said so.

---

## 5. Items for the operator (CQ-Q), CLA's proposals

**Marked 2026-11-11: (a) on every item** (`raft-marks-2026-11-11.md` §7).
CQ-Q1 (a) is the operator's own mark; the others are CLA's proposals,
adopted by the operator's choice of the option that named them. The table
keeps the options as they were put. **CQ-Q10** was added by S0 and marked
(a) on the operator's word the same day (`raft-marks-2026-11-11.md` §8).
**CQ-Q11..CQ-Q16 were added by S1 (§9) and marked (a), each as CLA proposed, on the operator's word of 2026-11-12** (`raft-marks-2026-11-11.md` §9). **CQ-Q1 (a) put its rulings again at S3: they stand** (§10 there).

| # | item | options | CLA proposes |
|---|---|---|---|
| CQ-Q0 | the letter | (a) CQ - CJ..CL are on `main`, CM, CO and CP are other orders', CN is the concept notes' prefix, and no ref names CQ; (b) another | (a) |
| CQ-Q1 | **CN-7's R1, R2 and R3 against CQ** (§1.2) | (a) **CQ explores a marked column as an exception**: the three rulings are set aside for the spike only, nothing ships, and they are put again at S3 with the ledger; (b) they stand - only an undeclared shape is explored (a reservation opened by a statement's own spelling, the bound a `WHERE` guard), which S1 must first show can be sound; (c) CQ is not opened | (a): the spike persists and ships nothing either way, and under CF-Q6 (b) a bound the engine enforces is a declared invariant, which is an argument for a declaration the operator may or may not accept |
| CQ-Q2 | what CQ may leave on `main` | (a) **documents, S0's driver and the results files only**; the spike stays on the branch, and productising it is a later order written after S3; (b) the spike lands behind a default-off switch | (a) |
| CQ-Q3 | the mechanism | (a) **C1 with the row in `S`**; (b) C1 with no row unit, the reserve a wait source for `DELETE` too; (c) C2 | (a), settled in S1 |
| CQ-Q4 | `[quiet-wrong]` a transaction's read of its own reserved column (K4) | (a) **the committed value, stated** (Oracle's rule); (b) an overlay of its own deltas; (c) refused by name | (a) for the spike; put again at S3 if CQ adopts |
| CQ-Q5 | a debit the bound refuses only because of undecided reservations (K11) | (a) **waits for a decide**, as an assertion's does (`assertion.md` §6.2), with CQ-R4's argument owed; (b) refused at once, retryably | (a) as the contract; the spike may build (b) and its entry says so |
| CQ-Q6 | the bar | (a) **S0 says build** iff, on a held-row cell and outside the spread across its repetitions, the pipelined hot-last arm commits `>= 1.25x` the hot arm at 8 sessions - that arm, not the disjoint one, is what C1 can reach; **S2 passes** iff the spike's arm recovers at least half of that gap, every run's final balances equal the serial oracle's with no committed state below `MIN`, and an unmarked relation is inside the spread. Whether a gain a client could have had by A2 is worth K1-K13 is S3's judgement, not the bar's; (b) a gain alone | (a) |
| CQ-Q7 | the names | (a) **column reservation** and **the column's reserve**, no SQL spelling chosen until adoption; (b) Oracle's "lock-free reservation" | (a): C1 shares a row, so "lock-free" would misdescribe it |
| CQ-Q8 | the spike's scope | (a) **`READ COMMITTED` only, `group` and `strict` both measured, `REPEATABLE READ` refused `NotImplemented`**; (b) wider | (a) |
| CQ-Q9 | A1, the re-run | (a) **priced in S0, not built in CQ**; if S0 shows what it recovers, it goes to `docs/pending/` as the operator's question on `txn.md` §5; (b) built in CQ | (a) |
| CQ-Q10 | **added by S0; marked (a) by the operator, 2026-11-11; an amendment of CQ-Q6 (a)**, which left to S3 whether a gain a client could have had by A2 is worth the costs. S0 found that under `group` A2 has the whole gain, so that judgement can be made before the spike is paid for | (a) **S1 only**: the model and K2's and K8's lists are written, and S2 is put to the operator again with them; (b) S1 and S2 as ordered; (c) **narrow now**: no spike - A2 goes into the manual, A1 to `docs/pending/`, and CQ closes on S0's numbers | (a): S1 is paper and is what a judgement of the costs against B4 and the early verdict needs; the spike is the expensive stage and S0 did not make its case on throughput |
| CQ-Q11 | `[loud]` **a `COMMIT` refused in its apply** (§9.6): what it does to the transaction | (a) **rolled back and ended**, as a failed commit is today (`command_dispatcher.cpp:10862-10896`): the client retries the transaction, which is what a `TxnConflict` inside it costs now; a wait that can be run again is, and never reaches the client; (b) the transaction stays open and failed until the client's `ROLLBACK`, a state no `COMMIT` leaves today | (a) |
| CQ-Q12 | `[quiet-wrong]` **the state a reserving statement's `UPDATED <n>` is read from** (§9.3) | (a) **the newest decided version**, stated: a row committed after the statement's snapshot counts 1 where an ordinary `UPDATE` counts 0, and a row whose delete committed after it counts 0 where an ordinary one is refused; (b) the row must also be visible to the snapshot, else 0 and nothing reserved | (a): the statement names the row by key alone and holds its `S`, and the check reads decided state by necessity (§9.1). It is also what makes correction 3's first test reachable: such a row can be invisible to the apply's view |
| CQ-Q13 | `[loud]` **the archive's fail-closed gate**, which refuses writing statements and not `COMMIT` (`command_dispatcher.cpp:1344-1347`) | (a) **the apply does not ask it**: the reserving statement passed it, no `COMMIT` is refused by it today, and what is logged past a closed gate is bounded by the journal; (b) the apply asks it, and a gate closed since the statement refuses the `COMMIT` | (a) |
| CQ-Q14 | `[quiet-wrong]` **the apply's conflict test** (§9.1, correction 3), an exception to `txn.md` §5, which §7 had excluded | (a) **both view tests of the row path replaced, for the apply alone, by "the writer is committed" under the held `U`**: it writes over a committed version its transaction's view may not cover - sound for a delta that reads the cell it writes over, and no reader's view moves; (b) the view-based tests stay, the pass-over made a refusal: an apply is refused whenever a peer's commit marker hides the row's last committer, and B2 is not delivered | (a); (b) keeps §7 whole and would be priced by S2 |
| CQ-Q15 | **S2, put again** as CQ-Q10 (a) requires, with §9 | (a) **a reduced spike**: a floor on one `int64` column with the type's range as its ceiling, every other write of the column refused, CQ-Q5 (b)'s refusal at once, measured on S0's hot-first cell and M4' under `group` and one cell under `strict`, with CQ-R2's and CQ-R3's cells and the off-path A/B; K9 and K11 not measured; (b) S2 as §6 orders it; (c) no spike: CQ goes to S3 on S0 and S1 - A2 into the manual, C1's model kept here | (a): it is the proof of concept W1 put first, at the two cells where S0 left a reservation something a client cannot do for itself (§9.10) |
| CQ-Q16 | `[loud]` **a `DELETE` of a row reservers keep arriving at** waits for every `S` and can end at the fault net (§9.6) | (a) **accepted**, the class a parent's `DELETE` is already in under a stream of child writers (`foreign-keys.md` §2c); (b) a reserver is refused while a `DELETE` waits, which needs a grant that looks at queued waiters, and the lock table's does not | (a) |

---

## 6. Stages

Every measurement runs under the standing rules: `build-release`,
interleaved A/B, 20 minutes a stage, a 60 s watchdog,
`git describe --tags` in the results file.

| stage | what | gate |
|---|---|---|
| **CQ-S0** | **The premise and the census; no engine code.** (1) **Oracle's contract read** from its documentation, every `[recalled]` claim confirmed or corrected. (2) **The census** `[source-read]`: every consumer of a column's value on `UPDATE`'s per-row path and at commit - assertion admission, a Transition's edge, the forward foreign-key check, the Cabin witness, index maintenance and the unique check, `RETURNING`, `ON CONFLICT DO UPDATE`'s arm, the `strict` deferred commit, the pool's no-refuse window - each with what a write landing at commit does to it (K8, K2). (3) **The measurement**, `cores = 4` pinned as CP-S0's, sessions' cores recorded (BL-S3 may move one between transactions), a new driver `tools/cq_s0_premise.py` over `bench_common` (a new file rather than a wider `lock_contention_benchmark.py`, by that driver's own header): **M1** the held-row shape - 8 sessions, `BEGIN`; `UPDATE` one row `SET c = c - 1`; `k` point writes on the session's own rows, `k` in `{0, 8}`; `COMMIT` - in four arms, **hot** (one shared row, written first), **hot-last** (the debit last, a round trip before `COMMIT`), **hot-last pipelined** with `COMMIT` (A2 as §3 states it, and C1's ceiling) and **disjoint** (the bound on any scheme), under `group`, three repetitions; committed TPS, `sys.lock_waits` by outcome, `TXN_CONFLICT` split decided-newer against lock (B2, A1), fault-net counts; **M2** M1's `k = 8` hot and pipelined arms under `strict` (K3); **M4'** M1's hot arm with a client pause taking the hold past 1 s (B3), cut first if the budget binds. (4) `bench/<version>/results-cq-s0-premise-<describe>.md` and §8: **build, narrow or drop** by CQ-Q6 (a) | the operator's opening, CQ-Q1 marked |
| **CQ-S1** | **The model**, a design memo in this order (§9): the mechanism (CQ-Q3) with CQ-R2's argument written as `lock_table.hpp` writes publish-then-verify - the check's read order, the release after the publish, every writer through the reserve, the apply's re-check; the apply's place on the dispatcher's `COMMIT` path and the release's in `FinishCommitBody`; what a failed apply leaves; K2's waits and refusals enumerated; K3, K4, K5, K10, K11 and K13 as contracts; the census turned into the refusal list; the amendments a build would owe to `txn.md` §5, `foreign-keys.md` and `rules.md` §3. Every `[quiet-wrong]` or `[loud]` choice goes to the operator as a CQ-Q amendment before S2 | S0 says build or narrow |
| **CQ-S2** | **The spike.** What S1 kept, at CQ-Q8's scope, on the work branch, with a `critics-developer` review and the full suite. Cells of its own: the bound under concurrent debits, with an ordinary writer of the column among them (CQ-R2); an abort, a failed apply and a crash mid-apply leaving nothing (CQ-R3); a `DELETE` waiting on a reservation; a commit's deadlock refused and rolled back. Then S0's driver with a `reserved` arm beside the four, and the off-path A/B on an unmarked relation (K1, K9, CQ-R1) | S1's model; the operator's marks on S1's amendments |
| **CQ-S3** | **The verdict.** §3's ledger filled; one of **adopt** (a later order productises - the declaration, the catalog, the manual, the sim's model - and CQ-Q1's rulings are put again), **narrow** (what survives, named) or **drop** (the spike deleted, the numbers kept here). The index row, `known-gaps.md` if anything is left standing, and CN-7 annotated with what CQ found | S2 landed; the close is the operator's |

---

## 7. What CQ does not do

- It does not build `INC`/`DEC` (CN-7), a row `CHECK` constraint, or a
  bound over more than one column or row.
- It does not touch the commit protocol or `txn.md` §5's decided-newer
  refusal (K3 and A1 are stated and priced, not tasks).
- It does not add savepoints, `SERIALIZABLE` or `SELECT ... FOR UPDATE`.

---

## 8. Stage log (CLA, appended as stages land)

### Written 2026-11-11 (W1, W2)

Written on `worktree-cq-lock-free-reservation` at `99b59c5b` from
`adbe2638`; `origin/main` `5d858feb` (CP through S2) merged at `018bece3`,
the one conflict `index.md`'s rows, both kept. Documents only, so **the
suite was not executed** and nothing was measured.

**The review** (`critics-developer`, 2026-11-11, on the draft at
`99b59c5b`). It verified every `[source-read]` citation and CP-S0's
numbers as quoted, and confirmed that `S` at the statement and `U` at
commit compose with the family as CP-S2b built it: two committers holding
`S` and asking `U` serialise, and are not `foreign-keys.md` §2c's
conversion deadlock. Applied:

- **Two `[quiet-wrong]` holes in C1 as drafted.** The draft released a
  delta from the reserve at the apply, before the commit was published,
  so a concurrent check could count it zero times (floor 0, committed 100:
  two debits of 100 both admitted). And an ordinary writer of a marked
  column - `U` passes over `S` - was invisible to a reserver's check. §2's
  C1 now releases after the publish, reads the reserve before the latest
  committed value, puts every writer of the column through the reserve,
  refuses an absolute `SET`, and re-checks the bound at the apply.
- The apply's view (minted after its `U` grant), its place (the
  dispatcher's `COMMIT` path, not `FinishCommitBody`), its order
  (`(relation oid, tuple key)`), and a failed apply's ordinary undo;
  CQ-R3 reworded to "no record of a reservation".
- **B1 restated**: C1's apply is A2 done by the engine, so the pipelined
  hot-last arm is its ceiling and the disjoint arm only a bound; the bar
  (CQ-Q6) rebuilt on that, with a spread rule and a correctness clause
  that has a referent. The draft's decided-newer benefit removed from
  C1's side: M4 is autocommit and an absolute `SET`, so it is A1's.
- K2 corrected (a `COMMIT` is refused today for a poisoned transaction, a
  failed `strict` sync, a stopped log and a cancel; what is new is a lock
  wait and a conflict refusal) and widened; K10 and K11 added, with the
  wire code under K7; A3's and A1's costs corrected.
- CQ-Q1 names R1 beside R2 and R3, and §1.2 no longer argues the
  operator's side of it. "Pending" renamed away from the assertion's.
- The stale claims the merge made false: CP on `main`, its results file,
  `known-gaps.md`'s corrected entry, `raft-marks-2026-11-11.md`.
- Cut as bloat: two cost rows restating a rule and a question, the
  budget question, four of §7's bullets, the `strict` benefit (now a cell).

**Rejected**: reusing `tools/lock_contention_benchmark.py` for S0 instead
of a new driver. That driver's header states the project's rule the other
way - a new file over widening a driver inside a measurement stage - and
its shape is autocommit statements, not transactions. The duplication the
review named is met by building on `bench_common`.

### CQ-S0, 2026-11-11: the contract read, the census, the premise measured - **build** by the bar, whose arm is the client's own remedy

Run on `worktree-cq-lock-free-reservation` at `92449e65`
(`v3.4.7-13-g92449e65`) on the operator's opening (W3). No engine code
was written.

**(1) Oracle's contract**, read in the Database Development Guide's
"Using Lock-Free Reservation" (23ai and 26ai) and Database Concepts,
through a summarising fetcher - so the phrases are that tool's rendering,
and the two Oracle staff blog posts were not reachable. Every
`[recalled]` claim of §0 is **confirmed**: the `RESERVABLE` declaration on
`NUMBER`, `INTEGER` or `FLOAT`; a primary key required and named whole in
the `WHERE`; `SET c = c ± <expr>` only, a direct assignment an error; no
row lock before commit; the journal table; the row locked and the journal
applied at commit; a rollback voiding the reservations. Corrections and
additions the order did not have:

- **A bound that cannot be met is an immediate error, never a wait**, and
  pending credits of others are not counted ("without relying on any
  active replenishment"). CQ-Q5 (a)'s wait is this order's own choice, not
  Oracle's.
- **Oracle has a commit-time refusal of its own**: a `CHECK` naming a
  reservable and an ordinary column is validated again at commit and
  cancels the transaction if the ordinary column moved.
- **A `DELETE` of a row with pending reservations is retried for 5 s and
  then fails "resource busy"**; DDL blocks until they finish, and a
  reservable update that meets either is refused.
- **Own reads (K4)**: Oracle's pages say a transaction reads its own
  reservations *from the journal table*; that the column itself shows the
  committed value is confirmed by a third party's test only.
- `RETURNING` is not supported; no index, primary key, identity, virtual
  column, unique or foreign key on the column; reservable and ordinary
  columns may not be assigned in one statement; at most ten reservable
  columns a table.
- Oracle states the use case as "reservable updates in long-running
  transactions", says a reservable update "is allowed" on a row another
  transaction has locked and does not block updates of the row's other
  columns - C1's `S` beside `U` - and **states no cost anywhere**.
- Not found: isolation levels, `NULL` in the column, a multi-row
  reservable update.

**(2) The census** `[source-read]` at `92449e65`, by a read-only pass over
`UpdateParsed` (`src/server/command_dispatcher.cpp:9856`, the row lambda
`:10118`) and the commit path; CLA re-read the lines cited here. What it
settles for S1:

- **Where the apply goes**: in `CommitLocal` (`:10848`), before
  `DefersCommit` and `txn_->Commit` / `CommitDeferred` (`:10858`). The
  transaction is still active there, no commit marker is set, and a
  failure falls into the existing abort (`:10884-10896`). Not inside
  `txn_->Commit`, whose marker caps every view mint on the instance; not
  in the after-logged hook, where the record is already appended. The
  reserve's release belongs after `PublishCommit`
  (`src/txn/manager.cpp:306`) and at every abort site.
- **A wait at `COMMIT` has to come before the first applied row.** A
  parked statement is re-run only if it wrote nothing since its trail
  mark (`:11757-11758`), and `COMMIT` sets no mark (`:1041`). So the apply
  takes every journal row's `U` first, in key order, and writes only once
  all are granted.
- **A new session state**: a `COMMIT` refused for a deadlock or at the
  fault net today would poison a session whose transaction is still open.
- **`COMMIT` is outside the archive's fail-closed gate** (`:1344-1347`
  lists the writing statements), so an apply that logs row writes either
  asks the gate or bypasses it.
- **`RETURNING` reads the transaction's trail** (`:9738-9741`); a
  reserving statement leaves no entry, so it would return no row beside
  `UPDATED 1`. Refused whatever its items (K5 is wider than the column).
- **`UPDATED <n>`**: a reserving statement must still answer 1 or 0, from
  the latest committed version rather than its snapshot - a visibility
  difference from an ordinary `UPDATE`, to be stated.
- **The type's own range**: `c - k` overflows against the value at
  commit, so an overflow can newly refuse a `COMMIT`.
- **Its own transaction's later acts**: a `DELETE` of a row it reserved
  on, or a DDL on the relation, between the reservation and the commit.
- **An assertion's `SUM` column** would have to reserve at the statement,
  and that reservation is logged - C2's cost. Refused in the spike.
- **A Cabin key function's input**: no correctness hole found (the
  witness runs at the apply, in order with the write); the refusal stands
  on usefulness. Whether the banking gate counts a reserver that has
  written nothing was **not verified**.
- **The lifetime sweep skips a statement in flight**
  (`src/server/tcp_server.cpp:932`), so a `COMMIT` parked in its apply is
  bounded by the fault net, not swept.
- **K8 and K2 are longer than §3 listed them**: the pass returns fourteen
  refusals by interaction - adding `ON CONFLICT DO UPDATE`'s assignments,
  a set-oriented `UPDATE`, `UPDATE ... FROM`, another column's `SET`
  reading the marked one, and the two own-transaction acts above - and
  fourteen ways a `COMMIT` can newly fail, adding the lock cap at the
  apply's asks, a stale descent, the overflow, a moved catalog, a log
  append failure and the archive gate. S1 owns both lists.

**(3) The measurement** `[measured]`,
`bench/v3.4.7/results-cq-s0-premise-v3.4.7-13-g92449e65.md`: `cores = 4`,
8 sessions, one process each, `tools/cq_s0_premise.py`. **Two passes**,
5 min 09 s and 7 min 10 s, inside the stage's 20 minutes; the second
exists because the review (below) found the first pass's driver short of
this stage's own row, and **every figure here is pass 2's**. Pass 1
agrees with it. The host was quiet at each start; the oracle - every row
of both relations against what each session counted as committed - is
exact in 61 cells of 61. The transaction is `BEGIN`, a debit, `k` point
writes on the session's own rows, `COMMIT`; each figure is the median of
three repetitions:

| cell | hot (debit first) | hot-last | hot-last piped with `COMMIT` | disjoint | disjoint piped |
|---|---|---|---|---|---|
| `group`, `k = 8` | `1,571 tps` | `4,447 tps` | `4,437 tps` | `4,402 tps` | `4,690 tps` |
| `group`, `k = 2` | `3,742 tps` | `6,301 tps` | `6,751 tps` | `6,356 tps` | `7,261 tps` |
| `group`, `k = 0` | `7,825 tps` | `7,668 tps` | `8,304 tps` | `9,078 tps` | `9,261 tps` |
| `strict`, `k = 8` | `823 tps` | `1,300 tps` | `1,233 tps` | `3,827 tps` | `3,852 tps` |

- **The window is real**: debited first, the row costs `64 %` of the
  throughput at `k = 8` and `41 %` at `k = 2` under `group`, and `78 %`
  under `strict`. The loss is in the debit statement (p50 `1,331 us`
  against `96 us`, p99 `28.7 ms`, at `k = 8`).
- **Under `group` the client can close all of it**: the debit written
  last is level with disjoint (`102 %` of the gap at `k = 8`, `98 %` at
  `k = 2`), and piping it with `COMMIT` cannot be told apart from that.
- **Under `strict` nothing short of the commit protocol closes it** (K3):
  the last-writer arms reach a third of disjoint.
- **Every refusal is decided-newer**, each failing its transaction: under
  `group` `1.4-12.8 %` of attempts, and more of them once the debit is
  last (`1.4 %`, `2.5 %`, `3.3 %` at `k = 8`); under `strict` `0-0.4 %`.
  No deadlock and no fault-net refusal in M1 or M2.
- **M4'**, one cell: a holder pausing 1.2 s uses the row serially at its
  bound - `18` commits in 20 s - and `134` waiters are refused at the
  fault net while `116` are granted. The pause costs refusals; only a
  holder that does not hold the row across it would let the eight commit
  about `133`.
- **What bounds a cell**: the server used at most `1.62` of its four
  cores, so the cells are bound by round trips and the sync, not server
  CPU. Client CPU was not recorded, so disjoint is a floor on what the
  server could do.

Not measured: `sys.ash`'s share, client CPU, PostgreSQL's floor. M4' is
one cell and shows a mechanism, not a rate.

**(4) The verdict, by CQ-Q6 (a): build.** The piped last-writer arm
commits `2.82x` the hot arm at `k = 8` (hot's best `1,593 tps`, the piped
arm's worst `4,317 tps`), `1.80x` at `k = 2` and `1.50x` under `strict`,
each outside the spread; at `k = 0` the arms overlap and the bar is not
met there.

**The bar's arm is A2 itself**, as CQ-Q6 (a) anticipated when it left
"whether a gain a client could have had by A2 is worth K1-K13" to S3. S0
answers the first half of that early: under `group` a client that writes
the row last has the whole gain, piped or not, so on throughput a
commit-time apply adds nothing to it. What is left for a column
reservation is:

- clients that cannot write the row last - the verdict is needed before
  the other work, or the transaction pauses after the debit (M4');
- the decided-newer refusals (B2), which A1 would also remove, with no
  new structure;
- B4, the bound as a declared invariant, which no number measures.

Against them stand K1-K13, of which the census lengthened K2 and K8.
**CQ-Q10** (§5) therefore asks the operator to amend CQ-Q6 (a)'s schedule:
the judgement it assigned to S3 is put now, before the spike is paid for.
CLA proposes (a) - S1, on paper, and S2 asked again with the model in
hand - and has started neither. A1 is priced at `1.4-12.8 %` of hot-row
transactions under `group`; by CQ-Q9 (a) it is owed a `docs/pending/`
entry, **not written**: that directory holds questions the operator has
raised, so the entry waits for the operator's word on CQ-Q10. (Written
at S1, on CQ-Q9 (a); the S1 entry below says on what licence.)

**The review** (`critics-developer`, 2026-11-11, on the stage as it stood
after pass 1). It found no counting bug - the piped arm's refused debit
is answered by a refused `COMMIT` and commits nothing
(`src/server/command_dispatcher.cpp:1356`) - recomputed every quoted
figure from the JSON, and confirmed the verdict as the bar's. Applied:

- **Pass 2**, for three shortfalls of the driver: the sessions' cores,
  which this stage's row requires, were not recorded; the oracle covered
  the hot row only, so the disjoint arms - every ratio's denominator - and
  the work rows were unchecked; the arm order put hot first in two
  repetitions of three. The driver now records each session's core at
  both ends, checks every row of both relations and one row affected per
  statement, starts each repetition from a different arm, records the
  server's CPU, keeps the competing-build list, and sends a refused piped
  transaction's SYNC and ABORT in one write so its failure path costs what
  the unpiped one's does. Pass 2 also ran `k = 2` and the plain hot-last
  arm under `strict`, which pass 1 had left out with budget to spare.
- **A2's label**: §3 defines A2 as the debit last *and piped*; the first
  draft of this entry, the results file and the driver called the unpiped
  arm A2. The arms are now named for what they send, and A2's row in §3
  gives both.
- **"What the bar does not say"** was wrong - CQ-Q6 (a) says it - and is
  reworded above; CQ-Q10 is stated as an amendment of CQ-Q6 (a).
- **Overstatements cut**: "a refusal for every waiter" past 1 s (`116` of
  `250` waits were granted); M4' read as lost throughput (the row is at
  its serial bound); a refusal range that silently included `strict`;
  "piping is slower" at `k = 0` and a derived `2.63x` ceiling, both inside
  the spread; two inferences now marked as such. Added: the client-bound
  caveat, the shared leaf pages, that debit latencies are over successful
  debits, and that a full ring's percentiles lean to a cell's end.
- Units in every cell of the results file, and the driver's uncommitted
  state at the run stated there.

**Rejected**: moving `host_state` into `bench_common` and adding an
`execute_then_commit` to `tools/kwp.py`. Both touch a helper that some
thirty drivers import, inside a measurement stage; CP's review deferred
the same consolidation for the same reason, and it stays deferred. The
exact wait count from `sys.meta` was not added: the ring's sample and the
client's latencies carry the stage's reading.

**The suite**: not executed. The stage's only new file outside `bench/`
and `instructions/` is `tools/cq_s0_premise.py`, which no CMake file
names, and the pre-push hook's `ctest` does not run it. The overhead
measurement is the milestone's close's, and CQ has changed no engine code.

### CQ-S1, 2026-11-11: the model - C1 holds on paper after three corrections; six items for the operator, S2 among them

Written on `worktree-cq-lock-free-reservation` from `ca7124f6` on the
operator's mark of CQ-Q10 (a) (`raft-marks-2026-11-11.md` §8). Paper
only: **no engine code, nothing measured, the suite not executed** - the
stage changed no build input.

**What it is**: §9. CLA read the commit path, the lock table's ask, the
snapshot ceiling and the abort for it (`src/txn/manager.cpp`,
`src/txn/lock_table.cpp`, `include/kds/txn/instance_visibility.hpp`,
`CommitLocal` and the point path in the dispatcher), and took the rest
from S0's census.

**What it found.**

- **A `[quiet-wrong]` hole in C1 as §2 states it**, the third the order
  has had: "the latest committed version", read through a view, can miss
  a commit that has published, left the reserve and been acknowledged,
  because a peer's open commit marker caps every view below it. The check
  and the apply ask whether the writer is committed, whatever the ceiling
  (§9.1, corrections 1 and 3),
  and the release moves to between the retire and the borrows' release
  (correction 2).
- **Correction 3 is an exception to `txn.md` §5**, which §7 said CQ would
  not touch. It is CQ-Q14.
- **The bill** is §9's first paragraph and §3's K2, K8 and K14 (new),
  with K3 restated as a contract - no throughput under a deferred
  `strict` commit beyond what S0 measured.
- **The benefit is unchanged from S0**: the early verdict and a free
  statement order, B2 if CQ-Q14 (a) is taken, B3 among reservers only
  (§9.6), and B4.

**The verdict of the stage**: not a drop on soundness - §9.4's argument
found no event that breaks the bound. Whether the bill is worth a spike
is CQ-Q15, the operator's; CLA proposes the reduced spike and has not
started it.

**A1**: `docs/pending/read-committed-rerun-on-decided-newer.md` is
written **on CQ-Q9 (a) alone**, which the operator adopted and which
sends the re-run there once S0 has shown what it would recover. The
operator's word on CQ-Q10 did not name it; S0's entry had said it would
wait for that word, and CLA reads CQ-Q9 (a) as the licence and says so
in the report. §9.1's fact adds a cost to it: a re-run can lose again to
the same marker unless it waits for the ceiling.

**The review** (`critics-developer`, 2026-11-11, on the draft). It
confirmed correction 1's premise, correction 2's ordering as a real
happens-before chain, that rollback's compensation keeps §9.4's (O1) and
(O2), that a grant ignores queued waiters, that K11's edges enter the
wait-for graph, and that a `COMMIT` can be run again. **It found the
draft unsound in three places**, all applied:

- `[quiet-wrong]` **The transaction's own versions.** Its own `U` or `X`
  covers its `S`, so on a row it wrote the head is its own and undecided:
  a debit after its own `INSERT` answered `UPDATED 0`, and the apply
  wrote over its own update. §9.8 item 13 now refuses both directions.
- `[quiet-wrong]` **A second view test on the row path.** The row lambda
  passes over a row its view cannot see; an apply could write nothing and
  commit. Correction 3 now replaces both tests and step 3 refuses a
  journal row that wrote no row.
- `[quiet-wrong]` **"Not in flight" is not "committed"**: an abort whose
  compensation failed retires with its version on the page. Correction 1
  now asks for a committed writer.
- Also applied: the check counts in-flight transactions only, which
  bounds the double count by the check's own length and ends the wait on
  a decided holder; `H` and `D` defined with `max(0, ...)` over other
  transactions and the violation separated from the wait; refusals added
  for a nullable column, a `WHERE` beyond the key, any writing statement
  that reads the column, the DDL side of items 2-6 and a dispatcher with
  no lock table; the reserve's bound multiplied by cores and columns;
  "nothing waits under the leaf" in place of a lock order the row lambda
  does not keep; the outcome table's count (10 of 13 new), its row 5, and
  the path that cannot park; `CheckWriteConflict` left unchanged, which
  takes one site off every transaction's path; the starving `DELETE` made
  an item (CQ-Q16); four citations; §9.10's repeated list cut.

**Rejected**: nothing whole. One finding is taken in part: the review
asked that the counts appear in one place; they stay in §3's cells as
well, because the ledger is what S3 reads.


### CQ-S2, 2026-11-12: the reduced spike - a debit written first no longer holds its row; two of the bar's three clauses met, the third inside the spread

Built and measured on `worktree-cq-lock-free-reservation` at `3dea53c2`
(`v3.4.8-11-g3dea53c2`; the spike `f72b8232`, one fix `3dea53c2`) on the
operator's marks of CQ-Q11..CQ-Q16 (a) (`raft-marks-2026-11-11.md` §9).
**On the work branch only** (CQ-Q2 (a)); no record kind, superblock,
catalog column, SQL surface or wire code (CQ-R5).

**What was built**, at CQ-Q15 (a)'s scope - §9 with these cuts, each
stated because the spike is narrower than the model:

- `include/kds/txn/column_reserve.hpp`: the reserve, a member of
  `InstanceVisibility`; one mark for the instance, a relation's bare name,
  a column and a floor, from `KDS_CQ_SPIKE_MARK` or a test.
- `ReserveColumnDelta` (the dispatcher, from `UpdateParsed`): §9.3. The
  relation `IX` and the row `S` before the descent, the check under the
  leaf's exclusive hold, the reserve read before the cell, both ends
  bounded. **No undo walk**: an undecided head is another reserver's
  apply, whose net is taken back out of the cell, or a writer of other
  columns, which carried the cell.
- `ApplyColumnReservations` (from `CommitLocal`): §9.5. Every row's `U`
  first, then each row through `UPDATE`'s own path as a generated
  statement, the head judged by "its writer committed" (CQ-Q14 (a)), the
  floor re-checked. A refusal that is no wait rolls the transaction back
  (CQ-Q11 (a)), on both dispatch paths.
- The release in `FinishCommitBody` and `Abort`, between the retire and
  the borrows.
- **Refused where §9.8 asks a narrower refusal**: any reserving statement
  on a relation with an index, an assertion, a Transition, a Cabin or a
  child foreign key; any reservation by a transaction that has written
  the relation, and any write of the relation by one that holds a
  reservation on it.
- **Not built**: `INSERT`'s bound check (a row can be inserted below the
  floor); the own-transaction DDL refusal and the DDL-side refusals; a
  write of *another* relation that reads the marked column; K11's wait
  (CQ-Q5 (b)); an ordinary writer of the column through the reserve. A
  stale descent at the reserving statement fails the transaction where
  the model would run it again, and the statement dirties a leaf it does
  not write.

**The cells** (`tests/column_reserve_rig_test.cpp`, eight): two debits
that do not both fit are not both admitted, on two cores; a
transaction's deltas net and a credit is not lent; a rollback leaves
nothing; a failed apply compensates the row it wrote and rolls the whole
transaction back; a `DELETE` is held off and every other write of the
column refused; an undecided writer of another column is read past and
then holds the apply off; the apply writes the relation the statement
named whatever `USE` ran since; two cores making 800 attempts at a
balance of 500 commit exactly 500 and end at the floor. **Not executed: a
crash mid-apply** - nothing of a reservation is logged and the apply's
versions are an ordinary loser's, so the claim rests on §9.5, not on a
cell.

**The suite**: 4435 of 4435 at `3dea53c2` (Debug, `ctest -LE bs-nightly`,
the hook's range, run with `-j4`); 4433 of 4433 before the review's fixes
and 4435 of 4435 at `f72b8232`.

**The measurement** `[measured]`,
`bench/v3.4.8/results-cq-s2-spike-v3.4.8-11-g3dea53c2.md`: `cores = 4`, 8 sessions,
two passes of 5 min 10 s and 6 min 31 s, **every figure pass 2's**; the
oracle exact in 52 cells of 52. Medians of three under `group`, with
eight statements after the debit:

| arm | committed | refused |
|---|---|---|
| hot, the debit first (today) | `1,760 tps` | decided-newer, `1.5 %` of attempts |
| **reserved, the debit first** | `4,584 tps` | none |
| hot-last piped with `COMMIT` (A2) | `4,584 tps` | decided-newer, `4.6 %` of attempts |
| disjoint | `4,474 tps` | none |

- **B1 is realised**: `2.60x` the hot arm, `100 %` of the gap to A2
  (`1.59x` and `101 %` with two statements after the debit), with the
  debit answered in `88 us` at the median where the hot arm's waits
  `1,307 us`.
- **And it is A2's size, as S0 said**: the reserved arm equals the piped
  arm. Under `strict` it reaches `1,184-1,198 tps` against the piped
  arm's `1,230-1,240 tps` and disjoint's `3,425-3,718 tps` (K3).
- **B2**: no decided-newer refusal in about `250,000` reserved
  transactions.
- **B3**, one cell: with a 1.2 s pause after the debit, `136` commits in
  20 s and no refusal, against `18` commits and `134` fault-net refusals.
- **K1**: `COMMIT` p50 `837 us` against `758 us`; the apply's waits for
  the row's `U` are `5,318-5,737` a cell, all granted, p50 `18-19 us`.
- **Off the path (CQ-R1)**: the spike's binary against the base's, no
  mark - ratios `0.976-1.004`, the ranges overlapping in all six cells,
  the spike's median the lower in five and its server CPU a transaction
  higher by `1-5 us` in all six. Three repetitions do not separate that
  from the spread.

**The verdict, by CQ-Q6 (a)**: the first clause is met; the second is met
by the oracle in the measured cells and by the test cells for the bound,
which the measured cells do not approach; the third is **inside the
spread and not shown to be zero**. CLA reads that as **S2 passed with the
third clause open**, and says so rather than calling it a pass. Not
measured: K9, K11, a crash, client CPU, the milestone's close overhead.

**What S2 adds to S0's and S1's reading.** The mechanism works and costs
what S1 listed. It buys what a client that writes the row last already
has, without asking the client to; beyond that, the transaction that
pauses after its debit, and no decided-newer refusal. A1, in
`docs/pending/`, would remove the refusals with no new structure.

**The review** (`critics-developer`, 2026-11-12, on the spike before its
first commit). It walked the admission against every interleaving of a
reserver's apply, publish, retire and release and found no false
admission, and confirmed the release on every exit. Applied:

- **A wrong-relation apply**: the generated statement resolved the
  relation in the session's current namespace, so a reservation on
  `ns1.c` from a `public` session failed its `COMMIT` or, with a
  `public.c`, debited that one. The journal now carries how the
  statement named the relation; the apply refuses a row whose `U` it
  does not hold and a write that landed on another oid.
- CQ-Q11 (a) on the synchronous dispatch path and on a failed
  `BeginWrite`; one column decoded, so a spilled cell elsewhere in the
  row is not met; `COMMIT` keeps its own shape in the statement
  statistics; the journal written before the reserve; the mark's parse
  strict; the per-borrow test reordered so an empty journal is one load.
- Three cells added or strengthened: the namespace, an undecided writer
  of another column, and the reserve's emptiness after a poisoned
  transaction's rollback; the failed-apply cell now fails on its second
  row.

**Rejected or deferred, with the reason**: building the apply's statement
as an AST instead of text (the namespace is carried instead; a build
would not generate SQL); one helper for the release and one lookup shared
with `ResolvedForEveryReader` (two copies of six lines in a spike that is
deleted or rewritten at S3); admission under a shared leaf hold and one
latch (it changes §9.4's O1, which S1 argued for the exclusive hold);
the stale-descent re-run and the `strict` head cell the review named as
the most valuable missing one - **not built**, stated above.

**Found by the measurement, not the review**: 4 debits of about 250,000
in pass 1 were refused because a head committed between the check's two
questions. Fixed in `3dea53c2` and absent from pass 2.

**The overhead**: not measured; measured at the milestone's close.


### CQ-S3, 2026-11-12: the verdict - **narrow**, the operator's; CQ closed on the work branch

On `worktree-cq-lock-free-reservation` at `fe53fdf8`
(`v3.4.8-13-gfe53fdf8`), on the operator's word and choice
(`raft-marks-2026-11-11.md` §10). The close is the operator's and so is
the verdict; CLA proposed it.

**The ledger, read at the close** (§3):

| | what the stages found |
|---|---|
| B1, the hold shrinks without the client arranging it | **realised and no larger than A2**: `4,584 tps` reserved against `1,760 tps`, and `4,584 tps` for a client that writes the row last (S2) |
| B2, no decided-newer refusal | **realised**: none in about `250,000` reserved transactions (S2). A1's option (c) would retry these in the engine instead; not built, not measured |
| B3, no fault-net refusal behind a long holder | **realised among reservers, one cell**: `136` commits against `18` (S2); a `COMMIT` still waits behind an ordinary holder of the row's `U` (§9.6) |
| B4, the bound as a declared invariant | **stated, not built whole**: the spike had no `INSERT` check (S2) |
| A1, the re-run | priced at `1.4-12.8 %` of contended transactions under `group` (S0); in `docs/pending/` |
| A2, the client writes the row last | **the whole throughput under `group`** (S0, S2); a third of disjoint under `strict`; now in the manual |
| A3, an entries relation under an assertion | not measured |
| A4, nothing | what stands for a client that does not reorder: `1,571-1,760 tps` in the measured shape |
| K1, page work at commit | `COMMIT` p50 `837 us` against `758 us` (S2) |
| K2, a `COMMIT` that waits and can be refused | 13 outcomes, 10 new (S1); none occurred in S2's cells |
| K3, `strict` | no gain beyond the last-writer arm, measured (S0, S2) |
| K4, K5, K13 | contracts (S1); in the spike K4 a stated read and a coarse refusal, K5 and K13 refusals |
| K6, K7, new DDL surface and a format event | **not incurred**: nothing is adopted |
| K8, refusals by interaction | 17 refusals (S1); the spike built a coarser set |
| K9, K11 | **not measured** (CQ-Q15 (a)) |
| K10, the reserve | bounded by the lock table's bound (S1); its latch not measured apart |
| K12, the test estate | **not paid**: eight rig cells on the branch, reverted with the spike; no sim model, no crash cell, no deadlock cell |
| K14, a second visibility rule and three functions on every transaction's path | inside the spread, a lean of `1-5 us` of server CPU a transaction not shown to be zero (S2) |

**The verdict: narrow.** A column reservation works on this engine;
§9.4 argues it sound on paper and the spike's cells found no false
admission. It is not adopted, because what it buys
on throughput a client already has by ordering its transaction, and what
it buys beyond that - a transaction that cannot reorder or pauses after
its debit, and no decided-newer refusal - does not carry K2, K8, K14,
the exception to `txn.md` §5, a declaration CN-7 ruled out, and a format
event. **What survives, named:**

- **A2 in the manual**: `manual/client/client.md` §5 tells a client to
  write the row many transactions share last, with S0's figures and
  what the advice does not do; sending it with `COMMIT` is given as a
  saved round trip, not a gain.
- **A1 stays pending**: `docs/pending/read-committed-rerun-on-decided-newer.md`.
- **This order**: §9's model, with the three corrections a build would
  need, and both results files, as the design of record should a client
  appear that cannot reorder.
- **S0's driver**, `tools/cq_s0_premise.py`, as it ran at S0.

**What does not survive.** The spike is reverted (`fe53fdf8`); its two
commits, `f72b8232` and `3dea53c2`, keep it in the branch's history, and
S2's results file names the commit it measured. CQ-Q1 (a)'s exception
ends with it: **CN-7's R1, R2 and R3 stand**, and CN-7 §6 is annotated
with what CQ found. `docs/inflight/known-gaps.md` gets no entry: nothing
of CQ is left standing in the engine.

**The suite**: not executed after the revert. Every build input is
`origin/main` `7acda8a8`'s again (`git diff origin/main` over `src`,
`include`, `tests`, `scripts` and the CMake files is empty), and the one
file CQ adds outside documents and `bench/`, `tools/cq_s0_premise.py`, is
named by no CMake file. These are the build inputs CP's close ran
4427 of 4427 at `7d21468a` (`raft-marks-2026-11-11.md` §4; `git diff
7d21468a 7acda8a8` over the same paths is empty). CQ's own last run was
4435 of 4435 at `3dea53c2`, with the spike.

**The overhead**: no measurement is owed. CQ leaves no build input:
against `origin/main` `7acda8a8` the tree differs in documents, `bench/`
and the driver, and against the opening `018bece3` the only source
difference is CP-S3's comment trims, merged from `main`.

**The review** (`critics-developer`, 2026-11-12, on this stage's
documents). It confirmed the revert, the build inputs, the driver's
identity with S0's and every S2 figure quoted. Applied:

- **The manual's bullet rewritten.** The draft quoted the reverted
  spike's binary, gave a refusal range that belonged to other arms, and
  advised sending the debit with `COMMIT` as though it gained
  throughput. It now quotes S0 alone, says that writing the row last
  trades waiting for retries (`2.5 %` against `1.4 %`), and gives the
  one-write form as a saved round trip that raises the refusals.
- The overhead sentence, which called the milestone's source change
  empty where CP-S3's comment trims are in it; the stage's commit; the
  4427 of 4427 that these build inputs ran at; B2's row, which said A1
  "would give the same"; "sound" restated as argued; the close table's
  missing A4 and its K4, K8, K12 and A1 rows; §3's sentence.
- CN-7's annotation cut to a pointer, with a header line naming it: a
  concept note is not amended with another order's measurements.
- The marks' §10 no longer records "no later order is opened", which the
  operator's choice did not say; the stale header lines about CP and
  about CQ-Q1 brought up to date; §5 records CQ-Q1's re-put.

**Rejected**: nothing.

**For the landing, the operator's**: a `--no-ff` merge of this branch
carries `f72b8232` and `3dea53c2` into `main`'s history, though not its
tree. CQ-Q2 (a) speaks of what CQ leaves on `main`; whether history
counts is the operator's to say, and a squash merge avoids the question.

**Landed, 2026-11-12**: the operator said to squash (`raft-marks-2026-11-11.md` §11). `main` takes the tree of `04bbbc26` plus this record as one commit; `f72b8232` and `3dea53c2` stay on
`worktree-cq-lock-free-reservation` and are not in `main`'s history. The push skipped the pre-push hook on the operator's word; the suite was not executed on the landed tree, whose build inputs are `origin/main` `7acda8a8`'s (4427 of 4427 at `7d21468a`).


## 9. The model (CQ-S1)

Written 2026-11-11 on `worktree-cq-lock-free-reservation` from `ca7124f6`
on the operator's mark of CQ-Q10 (a) (`raft-marks-2026-11-11.md` §8). A
design memo: **no engine code, nothing measured, the suite not executed**.
`[source-read]` here is at `ca7124f6`, whose source is `92449e65`'s; `CD`
is `src/server/command_dispatcher.cpp`. Everything not tagged
`[source-read]` is `[design]`. **The line numbers are `ca7124f6`'s**: the
merge of `origin/main` `7acda8a8` that followed brought CP-S3's comment
trims, after which the dispatcher's lines past `4555` sit 9 lower and
past `4804` 22 lower, and `lock_table.hpp`'s moved; no code changed.

**What S1 concludes.** C1 can be argued sound on this engine, after three
corrections to §2's text that reading the commit path forced (§9.1) and
the review's additions to them (§8). Its price is now itemised: **13
outcomes at `COMMIT`, 10 of them ones no `COMMIT` has today** (§9.6),
**17 refusals by interaction** (§9.8), **fifteen code sites**, three of
them functions on every transaction's path (§9.9), and one carve-out from
`txn.md` §5 that §7 of this order had ruled out (CQ-Q14). Nothing in it
recovers throughput that S0's last-writer arm does not already have.
**Six items go to the operator before S2** (§5, CQ-Q11..CQ-Q16), one of
them whether S2 is built at all, as CQ-Q10 (a) requires.

### 9.1 What S1 changes in §2's C1

| # | §2 said | S1 says | why |
|---|---|---|---|
| 1 | The check reads the cell of the row's "latest committed version"; the apply mints a view after its `U` grant | **Neither reads through a view.** Both read the newest version whose writer is **committed whatever the ceiling** - in the commit window, below its floor, or always-visible: the lookup `ResolvedForEveryReader` begins with (`src/txn/manager.cpp:841-846`), without the pending-commit bound it then applies | `[quiet-wrong]` otherwise. A view's ceiling is the published maximum **capped by every open commit's marker** (`include/kds/txn/instance_visibility.hpp:62-91`), and a marker "caps commits that are already acknowledged" (`:100-105`) - under `strict` for a whole sync. A transaction can publish, leave the reserve and release, and still be invisible to a view minted afterwards: its debit would be counted zero times. "Not in flight" is not the test either: an abort whose compensation failed retires with its version still on the page (`src/txn/manager.cpp:587-594`, `:611`) |
| 2 | The delta leaves the reserve "after the publish" | **After `RetireInFlight` and before the borrows' release** - between `src/txn/manager.cpp:325` and `:336` at a commit, between `:611` and `:626` in `Abort` - **and a check counts only the deltas of transactions still in flight** | A commit publishes (`:306`) before it retires, so a delta the check skips as retired, or finds gone, is already in the cell the check then reads, or was never committed. Before the release, so a committer the release wakes finds the reserve already without it |
| 3 | The apply writes "through the ordinary per-row update path" | **Through it, with both of its view tests replaced** by correction 1's: the row lambda passes over a row its view cannot see and writes nothing (CD:10135-10137), and `CheckWriteConflict` refuses a head its view cannot see (`src/txn/manager.cpp:188-197`). The apply carries its own check; `CheckWriteConflict` is not changed | `[quiet-wrong]` for the first: by correction 1's fact an apply could write nothing, commit, and drop an acknowledged delta - the failure `ON CONFLICT DO UPDATE` already guards against (CD:6805-6817). The second is the decided-newer refusal B2 exists to remove. Both are a carve-out from `txn.md` §5, which §7 said CQ does not touch: **CQ-Q14** |

Also settled, not corrections: **a transaction's deltas on one row net to
one signed number** (a transaction commits or aborts whole, so its net is
what the bound must fit); **both ends are always bounded** - a declared
`MIN` or `MAX`, and the column type's own range where none is declared,
checked on credits as a floor is on debits - so the apply's arithmetic cannot overflow where the statement's check
passed; and `S` then `U` on one unit by one transaction is **two entries**
against `max_locks_per_txn` (`S` does not cover `U`,
`include/kds/txn/lock_table.hpp:281-291`; the cap is tested before the
conflict, `src/txn/lock_table.cpp:210`).

### 9.2 State

- **The journal**: in the `Transaction`, one entry per `(relation oid,
  tuple key, column)` holding the net delta. One core's, because a
  transaction never crosses (`src/server/tcp_server.cpp:681-694`
  `[source-read]`). Not logged, not in the trail.
- **The column's reserve**: an instance-wide map from `(relation oid,
  tuple key, column)` to the list of `(transaction id, net delta)` not yet
  released, in latched partitions as the lock table's are. An entry exists
  while one transaction is in its list. **Every pair in it is matched by a
  lock-table entry of that transaction on that row**, so its size is
  bounded by what already bounds the lock table - `max_locks_per_txn`
  times 1,024 in-flight ids a core, times the marked columns of a
  relation - and K10 needs no setting.
  Memory only; a mount starts with it empty.
- **Lock order**: relation `IX`, the row unit, the leaf's exclusive hold,
  the reserve partition's latch. **Nothing waits under the leaf**: the
  reserving branch asks its row unit before the descent, where the row
  lambda asks under the leaf without waiting (CD:10212). The latch is a
  leaf lock: nothing is asked and no page is fetched under it. A release
  takes the latch alone.

### 9.3 The reserving statement

`UPDATE t SET c = c ± <constant> WHERE <tuple key> = k` in an explicit
`READ COMMITTED` transaction, `c` marked and alone in the `SET` (§2.1):

1. Relation `IX`, then the row in `S`, both waited for as any borrow is,
   **before the descent** - the order the forward foreign-key check keeps
   and for its reason (CD:4533-4546 `[source-read]`): after it, a whole
   `DELETE` fits between the read and the hold.
2. `LocateByTupleKey`, the leaf held exclusive as the point path holds it
   today (CD:10603-10641 `[source-read]`). Absent, or delete-marked at its
   newest decided version: `UPDATED 0`, nothing reserved (CQ-Q12).
3. **Under that leaf hold**: latch the entry; `H` = the sum of `max(0,
   net debit)` over every **other** transaction in it that is still in
   flight, plus this transaction's own net with the new delta if that is
   a debit; unlatch. Then `C` = the cell in the newest committed version
   (§9.1), reached down the undo chain as a reader reaches a version.
   Admitted iff `C - H >= MIN`; a ceiling is the mirror, with credits.
   Another transaction's credit is never counted: its abort would take it
   back. Admitted: latch, record the pair, unlatch; journal it; `UPDATED
   1`. No version, no undo, no log record, no trail entry.
4. Not admitted, and `C` less this transaction's own net is already
   below `MIN`: the bound's violation, the transaction failed as any
   refused statement fails it. Otherwise only others' deltas stand in
   the way: §9.6's wait.

### 9.4 The bound is never violated in a committed state (CQ-R2)

**The invariant.** For a row and a floor, at every instant:
`C_dec - D >= MIN`, where `C_dec` is the cell of the newest decided
version and `D` the sum of the net debits of every transaction whose delta
is admitted and not yet in `C_dec`. Any subset of those transactions may
then commit and the cell stays at or above `MIN`; credits only raise it.

**Every event keeps it**, given three orderings:

- **(O1) One row's admissions and applies are serial.** Each runs under
  the row's leaf held exclusive (§9.3 step 3, §9.5), and a version is
  written only under it. So between an admission's read of `H` and its
  recording, the row and the set of admitted deltas change only by
  releases.
- **(O2) `H` is read before `C`, and counts in-flight transactions
  only.** A delta the check skipped, or did not find, belongs to a
  transaction that had retired, hence published or compensated, before
  `C` was read: `C` has it, or it was never committed. A delta the check
  counted belongs to a transaction in flight at the latch; if it commits
  before `C` is read, that read finds its version committed and the
  delta is in both - **a false refusal, bounded by the check's own
  length**, never a false admission.
- **(O3) Nothing else moves the cell.** Every statement that would is in
  §9.8's list or goes through the same check: an absolute `SET` is
  refused; an `INSERT` checks its value against the bound and meets no
  reservation, since none can predate the row; a `DELETE` asks `X` and
  waits for every reserver's `S`; DDL that could move the column, its mark or
  its bound asks the relation `X` and waits for every reserver's `IX`
  (an online index build asks only `IS`, and is refused on a marked
  column, §9.8); an autocommit delta takes `U` and runs step 3
  with its own delta, releasing it at its own decide (CQ-Q15 offers the
  spike a refusal here instead).

Then: at an admission `C = C_dec` (O1) and `H` exceeds `D` plus the new
delta only by deltas already in `C`, so `C_dec - D >= C - H >= MIN`
after it; a
commit moves a net debit from `D` into `C_dec` and leaves the difference
where it was; an abort or a crash lowers `D`; a credit's commit raises
`C_dec`.

**The apply's re-check** (§9.5) is the same computation under `U`. By the
argument it cannot fail; it stays so that a hole the argument missed is a
refused `COMMIT`, `[loud]`, not a committed violation. By correction 2
the transaction that held `U` before it has left the reserve.

**B4** follows: the bound is checked on every statement that can move the
cell, by the engine, and a statement cannot omit it.

### 9.5 The apply, the release, and what a failure leaves

**At `COMMIT`**, in `CommitLocal` (CD:10848) before `DefersCommit` and the
commit call (CD:10858-10860 `[source-read]`), for a transaction with a
journal:

1. Mark the trail - `COMMIT` never reaches the one place a mark is set
   (CD:11137) - and re-resolve each relation's `TableAccess`.
2. **Every journal row's `U` first**, in `(relation oid, tuple key)`
   order. Nothing is written yet, so where the dispatcher may park, a refused
   ask parks and the `COMMIT` is run again whole - today's rule for a
   statement that wrote nothing; a `COMMIT` on a path that cannot park is
   refused.
   Others' `S` on the row does not refuse it; an undecided `U` or `X`, or a
   range `U` over the key, does (`U` refuses `U` and `X`,
   `lock_table.hpp:357-360`), and a grant is judged against holders, never
   against queued waiters (`src/txn/lock_table.cpp:219-225`
   `[source-read]`).
3. Per row, in the same order: locate the leaf, read the newest version -
   committed, since `U` is granted and §9.8 item 13 keeps the
   transaction's own versions off the row - re-check the bound, and write
   `C + net` through `UPDATE`'s own row path (CD:10118): the no-refuse window,
   the encode with every other cell carried from the version just read,
   undo, the trail, `HEAP_OVERWRITE`, the page stamp. **Each journal row
   writes exactly one row, or the `COMMIT` is refused.**
4. Then the commit as today. Under `group` the row's `U` goes at
   `FinishCommitBody`, before the sync; under a deferred `strict` commit
   it and every reserver's `S` stay until the sync returns (K3).

**The release** is one call, placed by correction 2 at both ends of a
transaction: in `FinishCommitBody` and in `Abort`, after the retire and
before `locks_->Release`. Every exit a transaction has passes through one
of the two - a failed commit, a deferred commit whose sync failed,
`ROLLBACK`, the lifetime sweep and a closed connection (both dispatch
`ROLLBACK`), an autocommit scope's abort.

**What a failure leaves (CQ-R3).** Before step 3 nothing exists but
memory. A failure inside step 3 leaves ordinary versions with ordinary
undo and falls into the failed-commit path that exists
(CD:10862-10896), whose `Abort` compensates them and then releases. A
crash at any point leaves no record of a reservation: the reserve is
gone with the process and the mount rolls the loser's versions back as
any loser's. Nothing is added to the checkpoint, the log or a page.

### 9.6 What a `COMMIT` can newly do (K2), and waits (CQ-R4)

Today a `COMMIT` appends one record and fails only for a poisoned or
expired session, a stopped log, a failed sync or a cancel. With a
journal - rows 10, 12 and 13 are today's outcomes reached from more
causes, the other ten are new:

| # | outcome | when | the client sees |
|---|---|---|---|
| 1 | waits for a row's `U` behind an undecided writer of the row or a range window | before any write | a slower `COMMIT`; recorded as a lock wait |
| 2 | that wait reaches the 1 s fault net | before any write | refused `TxnConflict` |
| 3 | that wait would close a cycle | before any write | refused, naming deadlock |
| 4 | `max_locks_per_txn` at a `U` ask (each is a second entry) | before any write | refused `ResourceExhausted` |
| 5 | the catalog word moved since the statement, by any catalog write on the instance | at step 1's re-resolve, before any write | re-run, unseen |
| 6 | a stale descent declines the row | first row: re-run; later rows: refused | `TxnConflict` |
| 7 | a full pool at the leaf fetch or the row's no-refuse window | per row | refused `ResourceExhausted` |
| 8 | the bound's re-check fails | per row | refused; a defect of §9.4 if it ever happens |
| 9 | the row is absent or delete-marked, or the write wrote no row | per row | refused; a tripwire, unreachable while §9.8 items 13-14 and correction 3 hold |
| 10 | a log append fails on an undo or row record | per row | refused; the log is stopped |
| 11 | rows applied, then any of 6-10 | after a write | refused, the applied rows compensated |
| 12 | the compensation of 11 fails | after a write | today's "and rolling it back failed" reply, from more causes |
| 13 | deferred `strict`: the log stops after the apply and the stage | after the park | refused; `FinishDeferredCommits`' abort (CD:1159) compensates |

Not the census's fourteen: its overflow is closed by §9.1's
bounds, its hook refusals by §9.8 (no assertion, Transition, unique or
index hook can fire on a marked-only write), and its archive gate is
CQ-Q13. **What every refused row of this table does to the transaction is
CQ-Q11**: the census found that 2 and 3, as the dispatcher handles a
statement's wait today, would leave a session poisoned with its
transaction still open, a state no `COMMIT` has now.

**B3's limit.** Reservers do not wait on each other for the row, but a
reserver's `COMMIT` waits behind any transaction holding the row's `U` to
its decide - an explicit transaction that updated another column of the
row and went idle - and ends at the net after 1 s (row 2). And a `DELETE`
of a row that reservers keep arriving at can starve to the net, as a
parent's `DELETE` does under a stream of child writers (CQ-Q16).

**Two wait sources, and only one is new.** The apply's waits are the lock
table's: slot, kick, graph and net as they stand (CQ-R4 met by
construction). **K11's is new**: a debit refused only because of other
transactions' deltas (CQ-Q5 (a)) names one of them through the
registration an assertion's group wait uses (`NoteBlockingWriter`,
CD:10388 `[source-read]`), parks, and is run again from the top. Its
argument:

- **No false admission**: the re-run is a whole new admission (§9.3); the
  wait decides nothing.
- **No lost wake**: it has no slot and is polled, one of `txn.md` §5's
  "what still polls", so a missed kick costs a poll interval.
- **No hidden deadlock**: the edge to the named holder is in the lock
  table's wait-for graph, so two reservers each waiting on the other's
  delta, or a cycle through a row lock, is refused naming deadlock.
- **It ends**: at the holder's decide, bounded by the 60 s lifetime
  ceiling, and before that at the statement's one 1 s net.
- **A holder that decided in between**: the registration declines a
  holder no longer in flight (CD:11203-11204), which today fails the
  statement. Here the statement is run again at once instead - the delta
  it lost to is settled either way.
- **What reaching the net means**: as for an assertion's group
  (`assertion.md` §6.2), ordinary contention and not a stuck holder - a
  balance near its floor with a long transaction's debit in front of it.
  Oracle refuses that debit at once (§8, S0). The net's message today
  names a stuck holder or an assertion's group (CD:531-540) and would
  need a third wording.

### 9.7 Contracts

- **K3, `strict`**: a column reservation gives no throughput under a
  deferred `strict` commit beyond S0's last-writer arms (`1,233-1,300
  tps` against `3,827 tps` disjoint): the apply's `U` spans the sync and
  committers of one row pass at the sync's rate. What it gives there is
  the early verdict and a free statement order.
- **K4, own reads** (CQ-Q4 (a)): a transaction's `SELECT` of the column
  returns the committed value and never its own delta. Its consequence is
  a refusal (§9.8 item 11): another column's `SET` reading the marked one
  in that transaction would store a value that omits the delta.
- **K5**: `RETURNING` on a reserving statement is refused **whatever its
  items** - it reads the statement's trail (CD:9738-9745), which is empty,
  and would answer no row beside `UPDATED 1`.
- **K13**: under `REPEATABLE READ` a reserving statement is refused
  `NotImplemented`, so an explicit `REPEATABLE READ` transaction cannot
  write a marked column at all; its reads are untouched.
- **`UPDATED <n>`** (CQ-Q12): answered from the newest decided version,
  not the statement's snapshot, and counted in `sys.relation_stats` once,
  at the statement.
- **The lifetime ceiling** bounds a reservation at 60 s; a `COMMIT`
  parked in its apply is a statement in flight, which the sweep skips
  (`src/server/tcp_server.cpp:932`), so the net bounds it instead.
- **Not verified, owed by S2 if it runs**: that the undo walk of §9.3 step
  3 is admissible under an exclusive leaf hold; whether the assertion hook the row path calls on any relation with an
  assertion (CD:10259, CD:10377) can refuse or log on a write that moves
  none of its columns; a cancel frame against a parked `COMMIT`.

### 9.8 The refusal list (K8), from the census

Each a refusal by name at the statement or at the mark's declaration; in
the spike `NotImplemented`, the code a build would owe decided there.

| # | refused | ground |
|---|---|---|
| 1 | the mark on the tuple key | already impossible: a tuple key is never assigned |
| 2 | the mark on an index key or covered column, unique included | every delta would append an entry (IX4b) and a covering probe would read a stale value until the apply |
| 3 | the mark on a foreign-key column | `SET fk = fk - 1` is refused today (BJ) |
| 4 | the mark on a Transition's governed column | the edge would be judged at commit |
| 5 | the mark on an assertion's `GROUP BY`, `SUM` or `WHERE` column | a delta would move a row between groups at commit; a `SUM` column would need a logged assertion reservation at the statement (C2's cost); the `WHERE` case was not read and is refused with them |
| 6 | the mark on a Cabin key function's input | usefulness - one set per distinct value - not a found hole |
| 7 | the mark on a column that is not `int64`, or is nullable | scope (spike); `NULL - k` would make the bound three-valued |
| 8 | an absolute `SET`, or any `SET` of the column that is not `c ± constant` | §9.4 (O3) |
| 9 | `RETURNING` on a reserving statement | K5 |
| 10 | the column assigned in `ON CONFLICT DO UPDATE`, a set-oriented `UPDATE`, `UPDATE ... FROM`, or under any `WHERE` beyond the tuple key's equality - `AND c >= 1`, the guard clients write today, included | each would need the check per row under a hold it does not have |
| 11 | any writing statement that reads the marked column, in a transaction holding a reservation on the relation - another column's `SET`, `INSERT ... SELECT`, a `WHERE` | K4: it would store or act on a value that omits the transaction's own delta |
| 12 | a reserving statement that also assigns an unmarked column | §2.1 |
| 13 | a reserving statement on a row its own transaction has inserted, updated or deleted, and any write of a row it holds a reservation on | its own `U` or `X` covers its `S`, so the head is its own undecided version: an own insert would answer `UPDATED 0`, and the apply would write over its own update or find the row gone |
| 14 | DDL on a relation its own transaction holds a reservation on | the mark, the bound or the column could move under the journal |
| 15 | a reserving statement under `REPEATABLE READ` | K13 |
| 16 | an index, an assertion, a Transition, a Cabin or a foreign key created on a marked column | items 2-6 from the other side; an online index build holds only `IS`, which a reserver's `IX` admits, so no lock stops it |
| 17 | a reservation where the dispatcher has no lock table | every ask is granted there (CD:11280), so neither the `S` nor the `U` exists |

A foreign key's **parent** relation needs no refusal: a reserver's `S` is
the mode a child writer holds, the apply's `U` passes over it, and a
parent `DELETE` waits for both.

### 9.9 What a spike touches, and what a build would owe

**Fifteen sites.** New: the reserve (a header and a source file); the
journal on `Transaction`. Changed, on a marked relation's path only:
`UpdateParsed`'s point path (the reserving branch); §9.8's statement
refusals at compile; the row lambda (an ordinary writer of the column, or
its refusal); `INSERT`'s bound check; the own-row and own-relation
refusals; the DDL-side refusals; the apply's row write with its two view
tests replaced; `COMMIT`'s trail mark and re-run, which it has never
needed; K11's registration and the net's message; the branch-only hook
that carries the mark and the bound (CQ-R5). **Changed on every
transaction's path**: `CommitLocal` (a test for a non-empty journal), and
`FinishCommitBody` and `Abort` (the release, the same test). Those three
are what CQ-R1's off-path A/B has to show inside the spread.

**Amendments on adoption**, none written now: `txn.md` §5 - the
reservation's `S` on the transaction's own row, the apply's `U` at
commit, the conflict test's new arm, the reserve as a wait source, and
that a `COMMIT` waits and can be refused; `txn.md` §1 - the ceiling
bounding a reservation; `foreign-keys.md` §2c - a reserver beside child
writers; `rules.md` §3 - the reserve's latch and its place in the lock
order; `protocol.md` §11 - a bound-violation code; `wait-events.md` - an
event for K11's wait; `client-manual.md` - the refused `COMMIT`.

### 9.10 CLA's reading for CQ-Q15

S0 took the throughput case away under
`group` and S1 adds no benefit, only the bill. What a spike can still
show is the proof of concept W1 put first, on the two cells where a
reservation does something a client cannot: the **hot-first** arm, where
the debit's verdict comes before the other work (`1,571 tps` today
against `4,437 tps` for the same work reordered), and **M4'**, a
transaction that pauses after its debit (`18` commits in 20 s today,
about `133` if the row were not held). CLA proposes the spike cut to
those: a floor on one `int64` column with the type's range as its ceiling,
every other write of the column
refused instead of taught the reserve, CQ-Q5 (b)'s refusal at once
instead of K11's wait, `group` measured on those two cells and `strict`
on one. K9 and K11 are then **not measured**, and the entry would say so.
If the operator judges a free statement order not worth §9.6 and §9.8 on
any number, (c) closes CQ on S0 and S1 without one.
