# Work order EE - the decide slot: one wake behind every wait for a transaction's decide, taken up from CP's C4

Status: **CLOSED 2026-11-27 on the operator's standing go-ahead, abandoned
at EE-S0, with nothing built** (`raft-marks-2026-11-27.md` §18; §7's
"EE-S0" and "Closed" entries). EE-S0
(`bench/v3.4.17/results-ee-s0-premise-v3.4.17-220-g046b09053.md`)
priced the polling waits: at the default 1 ms drain tick a wait with no
slot behind it ends 0.6-0.7 ms later than one woken at its slot (p50,
cross-core), the tick bounding it; no wait ended at the fault net in
3.6 million on one hot row. EE-Q1 (a)'s bar, CLA's reading: not met, so
EE-S1..S3 do not run and no engine code lands. EE-Q5's eight-core cell
put a standing fence at +1.78 us of server CPU a point write, near
BP-H10's figure, and C3 stands re-proposed as its own item
(`known-gaps.md`, Locks). Before that: **OPENED 2026-11-27 on the
operator's standing go-ahead** (`/go-ahead-achieving-milestone EE`,
scheduled six hours ahead; `raft-marks-2026-11-27.md` §17) on worktree
`ee-decide-slot` (branch `worktree-ee-decide-slot`) from `046b09053`
(`v3.4.17-220-g046b09053`); **EE-Q0..Q6 adopted (a) on that go-ahead** -
CLA's proposals, not the operator's per-item mark (§4, "Marks"). Before
that: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; EE-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here. **CP's order was not re-read in full for this order**:
§1 rests on `known-gaps.md`'s account of C3 and C4, and EE-S0 re-derives
the design at the current code before anything is built.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, `[loud]` (a wrong choice turns a wait into a visible,
retryable refusal), as in CP's order.

Related: `docs/spec/txn.md` §5; `docs/spec/index.md` §10.2;
`docs/spec/foreign-keys.md` §3a; `docs/spec/heap-and-tuple.md` §4.1c;
`docs/inflight/known-gaps.md`, "Locks", "Keystone ids", "Unique indexes
(BX)"; `instructions/v3.2.0/workorder-cp-lock-model-exploration.md`
(CP §3 C3, C4; §8.4; CP-Q4 (a), CP-Q12 (a)).

## 0. What EE is

`[source-read]` A writer meeting an undecided holder waits for its
decide; several waits have a wake behind them and several still poll,
and a wake is not a grant (`known-gaps.md:905`). CP designed one slot
per in-flight transaction, flipped at its decide, that would put a wake
behind every polling wait but one - and built nothing of it
(`known-gaps.md:939`: "It is a design for its own order").

`[design]` EE is that order. **Its premise is weak by CP's own
measurement, and EE says so before it asks to be opened**: at CP-S0 the
lock family's whole share of a point `UPDATE` was about 0.4 us of 39 us,
every parked ask in three cells was granted, and no wait ended at the
fault net. EE's case is therefore not throughput. It is that the
polling waits and the fault-net endings under sustained contention are
behaviours no cell has yet priced, and that one mechanism would replace
several. EE-S0 has to find a workload where that matters, or EE stops.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **The decide slot** (`:939`, read at `f5d310ec`; nothing of it in the
  tree): one slot per in-flight transaction, flipped where `Release`
  runs today (`FinishCommitBody` after `RetireInFlight` and the schema
  word; `Abort`). Its second half - the row's table entry published only
  while a fence stands - carries a `[loud]` risk: a fenced bulk
  statement that today waits before its walk would meet an undecided row
  mid-walk and be refused retryably. CP-Q4 (a) marks the form that
  avoids it, (alpha): the fence-taker waits out the statements that
  began before its publish.
- **A wake is not a grant** (`:905`): `TryAcquire` does not queue, so a
  third writer can take the unit between the flip and the re-run; under
  sustained contention the waiter ends at the 1 s fault net naming the
  last holder.
- **What still polls** (same entry): no lock table; a holder only the
  header names; an assertion's group; a `PURGE` waiting on a deleter's
  decide or the read horizon (`:818`: "Nothing wakes its wait"); a
  holder still in flight when its row's probe is granted.
- **The per-relation lock header is not built, and its eight-core
  number is not measured** (`:956`, C3): a standing range fence costs
  every point write of the relation one walk of the 64 partitions,
  about 1 us at four cores; "re-proposed as its own item only if an
  eight-core run reproduces the figure".
- **Row 2 onward of a multi-row `INSERT` meeting an undecided writer of
  its key is refused `TxnConflict` rather than waited for** (`:1871`,
  BX-Q3 (a)).
- **A named key meeting an undecided insert past 2^16 versions is
  refused rather than waited on** (`:892`), wrongly if the first
  transaction rolls back; the operator's G21 ruling kept the cap.
- **A hot row's client-visible refusal is not a lock's** (`:987`): it is
  the decided-newer verdict, CR's subject.

`[source-read]` in the code at `8274c951`: `LockTable::TryAcquire`
(`src/txn/lock_table.cpp:142`, `include/kds/txn/lock_table.hpp:634`);
the in-flight table holds 1,024 ids a core (`CLAUDE.md`, "Transactions &
MVCC").

## 2. The mechanism `[design]`

As `known-gaps.md` reports CP's C4; EE-S0 re-derives it.

1. **The slot.** Every in-flight transaction has one wake slot, found
   from its id. It is flipped once, where the transaction's units are
   released at its decide.
2. **The waits move onto it.** Each entry of "what still polls" whose
   wait is for a transaction's decide parks on that transaction's slot:
   a holder only the header names, an assertion group's holder, a
   `PURGE`'s deleter. A wait for the read horizon is not a decide and
   stays as it is (EE-Q4).
3. **A wake that is a grant** is a separate question (EE-Q2): the unit
   handed to its oldest waiter at the release, which is a queue the
   lock table does not have today.
4. **The second half** - rows no longer published to the table except
   under a fence - is not in EE unless marked (EE-Q3); it is where the
   `[loud]` risk lives.

## 3. Rules

- **EE-R1. No wait becomes a refusal** `[loud]`: every statement that
  waits today waits under EE, and CP-Q4 (a)'s form (alpha) is the only
  one built if EE-Q3 is marked.
- **EE-R2. No wake is lost.** A waiter that misses its flip sleeps to
  the fault net and is refused retryably - `[loud]`, never a wrong
  answer: the flip and the park are ordered as the existing unit slots
  order theirs, and a rig cell forces the decide between a waiter's
  check and its park.
- **EE-R3. The deadlock detector sees every wait it saw**, and each
  wait moved onto the slot draws the same wait-for edge.
- **EE-R4. Nothing of the family is persisted**, as today.
- **EE-R5. A statement that meets no undecided holder pays nothing new**
  beyond what the slot's allocation costs at `BEGIN`, measured against
  CP-S0's 0.4 us.
- **EE-R6. The fault net stays** (`lock_wait_fault_net_ms`, 1 s) as the
  bound on every wait.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EE-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EE-Q0 | Is EE opened? | (a) yes, as an exploratory order gated on S0; (b) no: CP-S0's numbers stand and nothing asks for it | (a), with (b) a sound answer |
| EE-Q1 | EE-S0's bar | (a) S0 must show, on the current tree, either a polling wait whose latency is the poll's interval and not the decide's, in a workload an operator would run, or waits ending at the fault net under sustained contention on one row; if neither, EE stops with nothing built. (b) built regardless | (a) |
| EE-Q2 | A wake that is a grant | (a) outside EE: a queueing lock table is its own design; S0 measures how often a woken waiter loses the unit. (b) inside, FIFO per unit | (a) |
| EE-Q3 | The second half (row entries only under a fence) | (a) outside EE; (b) inside, as form (alpha) | (a) |
| EE-Q4 | A `PURGE`'s wait on the read horizon | (a) stays polling; (b) a wake at the horizon's advance, a second slot kind | (a) |
| EE-Q5 | C3, the per-relation lock header | (a) S0 runs the eight-core cell CP did not; C3 is re-proposed only if BP-H10's figure reproduces. (b) not measured | (a) |
| EE-Q6 | The 2^16 chain (`:892`) and row 2 onward (`:1871`) | (a) outside EE: G21 and BX-Q3 are marked; (b) reopened | (a) |

**Marks.** EE-Q0..Q6 are adopted (a), each as CLA proposed it, **on the
operator's standing go-ahead of 2026-11-27 (go-ahead-achieving-milestone)**
- one word for the milestone, not a mark on each item, and none the
operator's own (`raft-marks-2026-11-27.md` §17). No EE-Q item is
`[quiet-wrong]`. **EE-Q1 (a)'s bar is read by CLA**, at EE-S0 (§7): not
met. The operator's EE-Q1 (b), "built regardless", is the word that would
run EE-S1..S3 over that reading.

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EE-S0** | C4 re-derived at the current code: every wait for a decide, what each parks or polls on, where `Release` runs. EE-Q1's two measurements and EE-Q5's eight-core cell, in `build-release` | a results file; the census in §7; EE-Q1's bar read |
| **EE-S1** | The slot and its flip; the header-named holder's wait moved onto it | EE-R2's rig cell; the deadlock cells unchanged |
| **EE-S2** | The assertion group's and `PURGE`'s deleter waits | a cell each, red before it at the poll's interval |
| **EE-S3** | The close: EE-R5 measured, `txn.md` §5's "what still polls", `known-gaps.md` | the operator's word on landing |

## 6. What EE does not do

- No new lock mode or unit, no change to what any statement borrows.
- No change to the decided-newer verdict (CR's, written and not
  opened).
- No persisted lock, no change to the fault net's value.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.

### Opened 2026-11-27

On worktree `ee-decide-slot` from `046b09053` (`v3.4.17-220-g046b09053`),
on the operator's standing go-ahead, scheduled six hours ahead
(`raft-marks-2026-11-27.md` §17). EE-Q0..Q6 adopted (a) (§4, "Marks").
**The line numbers §0 and §1 cite in `known-gaps.md` were read at
`8274c951` and have moved**: at `046b09053` the decide slot's entry is at
`:1014` (cited `:939`), "a wake is not a grant" at `:985` (`:905`) and
`PURGE`'s "Nothing wakes its wait" at `:898` (`:818`); the entries say
what §1 says they do.

### EE-S0, 2026-11-27: the census and the premise

`bench/v3.4.17/results-ee-s0-premise-v3.4.17-220-g046b09053.md`, at
`046b09053`, `build-release`, a hashed copy of the binary; no engine code.

**C4 re-derived at the current code** (the results file's §2 carries the
table with every site):

- **Woken at a slot**: a refused unit's wait (`AwaitWriteBlock`), a decided
  holder not yet released (AY-S3), a granted probe above the ceiling (the
  ceiling's lift), a relation's lock (`AwaitRelationLock`) and an online
  build's drain. `Release` runs where §1 says: `FinishCommitBody` after
  `RetireInFlight` and the schema word, and `Abort`.
- **Polled, and reachable by a statement an operator writes**: an
  assertion group's reserver (`AdmitInsert`, `AdmitAndReserveUpdate` -
  `NoteBlockingWriter` with no wake) and a `PURGE`'s undecided deleter.
  `PURGE`'s horizon wait polls too, and is not a decide (EE-Q4 (a)).
- **Polled, and reached by no writer known with a lock table**: a holder
  only the header names (every row an `UPDATE` or `DELETE` writes is claimed
  before the check, under its tuple unit or a declared range no tuple
  holder can share, every `INSERT` takes its tuple
  `X`, and the KWP load inserts through `ExecuteInsert`), and a granted
  probe whose holder is still in flight. Without a table: a foreign key's
  busy parent and the header-only holder. By reading; no cell drives them.
- **Neither polled wait opens a `sys.lock_waits` row or counts in
  `relation_stats`' `lock_waits`**: a row is opened on a slot only, so
  CP-S0's "every parked ask granted" did not see them.

**EE-Q1's two measurements**:

1. **What a poll costs a wait.** Every core arms the WAL drain timer at
   `wal_drain_interval_us` (1,000 by default), and a timer caps the idle
   block, so a polled wait ends at the waiter core's next drain tick, not
   at the idle block's 10 ms ceiling (§0 named no interval). Cross-core, the assertion arm answered
   +630 us and the `PURGE` arm +573 us later than a slot-woken row wait at
   p50 (+832 and +807 us at p95; 60 asks an arm). With the drain off
   (`wal_drain_interval_us = 0`, which the server warns about at boot),
   15 of 30 assertion waits and 11 of 30 `PURGE` waits ran past 2 ms,
   toward the 10 ms ceiling (4 and 3 past 9 ms).
2. **Waits at the fault net on one hot row.** Eight sessions, four cores,
   `BEGIN; UPDATE; COMMIT` for 15 s, with 0 and 2 ms held: 3,637,789 waits,
   **0 at the fault net**, 0 deadlocks. At 2 ms held a commit cost 7.0
   parks, since a wake is not a grant, and the slowest transaction took
   344 ms.

**EE-Q5's cell**: at `cores = 8`, eight writers, a standing range fence
costs the uncovered point writes **+1.78 us of server CPU a statement**,
paired over 22 blocks of three runs (14 positive; t-interval +0.63 to
+2.93 us), about +15 % of an unfenced 12.1 us, with 2.4x the
partition-latch waits; the client's p50 does not move outside its spread.
BP-H10's +12 % reproduces within the interval, so under EE-Q5 (a) **C3 is
re-proposed as its own item**, recorded in `known-gaps.md` (Locks). It is
not EE's to build (§6: no change to what any statement borrows).

**EE-Q1 (a)'s bar, CLA's reading: not met.** Its first arm asks for a
polling wait "whose latency is the poll's interval and not the decide's,
in a workload an operator would run", and §0 sets the test as "a workload
where that matters". **The letter of the first arm is within reach**: the
cell held each decide 20 ms, and with a holder that decides in under a
millisecond - a short booking transaction - the one extra drain tick would
be most of the wait (not run). CLA reads the arm as not met on what the
wait is and what it costs, not on the decide being the larger term: the
poll adds at most one drain tick after the decide, under a millisecond at
the default; an operator reaches it only at an assertion group's last unit
of room, where the wait ends in a refusal whenever the reserver commits
(`:9089-9091`: "an abort admits the re-run, a commit makes the refusal
true"; how often reservers commit was not measured, the cell ran the
rollback case), and through `PURGE`, an administrator's statement bounded
at its own 1 s, whose wait ends in a success. The second arm is not met:
no wait ended at the net. The reading is CLA's and recorded as such; the
operator's EE-Q1 (b) is the word that builds EE-S1..S3 over it.

Tools: `tools/ee_s0_premise.py` (new), `tools/cp_s0_fence.py`
`--cpu-schedstat` (on-CPU ns, where the 100 Hz ticks could not resolve
a 2 us delta). Neither is a build input. Reviewed by `critics-developer`
(below).

### Closed 2026-11-27, abandoned at EE-S0

EE-Q1 (a): the bar is not met, so EE stops with nothing built. EE-S1..S3
do not run, no engine code lands, and no close measurement is taken - there
is no code change to measure (the overhead measurement CLAUDE.md's step 3
owes a milestone's code change has nothing to compare). `known-gaps.md`'s
Locks entries carry the prices EE-S0 measured and C3's re-proposal.

### EE-S0's review (`critics-developer`, 2026-11-27)

On `ee-decide-slot` at `ef190004a`. No finding changed a conclusion; every
figure reproduced from the archive. **Applied**: a drain-off figure
restated wrongly in two records ("reached the ceiling"; 15 and 11 of 30
ran past 2 ms, 4 and 3 past 9 ms); the header's "+1.9 us" (+1.78); "§0
presumed the 10 ms ceiling", which §0 does not say; a note that
`wait_aborts` counts autocommit writes only, so its 0 in the hot cell is
no evidence; a note that the archived fence text labels schedstat values
"CLK_TCK"; the driver's `sys.lock_waits` filter keyed per core (`seq` is a
core's; the runs were unaffected); the driver's docstring; **the bar
reading restated** - it had leaned on the decide being the larger term,
which the 20 ms hold chose, and on "mostly delay a refusal", which only
the assertion arm supports and only when its reserver commits; the
census's missing `res.txn_slot` wait (DJ-S3, polled, a wait for some
decide on the core); "claimed under its tuple unit" made "under its tuple
unit or a declared range"; the fault-net refusal classified by its text
in the driver; p50 deltas rounded (+630, +573 us, not truncated); the
pooled blocks' mixed resolution and the wake cell's placement probe and
arm order stated as limits; `cp_s0_fence.py`'s stale docstrings.
**Rejected**: cutting the figures from the index row, the raft marks and
this log to a citation - the house records carry their figures (DX's,
CW's), and the restated errors are corrected; a shared `bench_common`
`Server` for the nine drivers that copy one - `dk_close.py`'s speaks
`kwp.Connection` where this one needs `ServerConnection`'s transaction
frames and text `SHOW META`, and the refactor is outside EE; the driver's
percentile rule replaced by `bench_common.nearest_rank` - the published
figures are the driver's rule (`pct`, round-half index), and changing it
would move them by up to a rank with no re-run.
