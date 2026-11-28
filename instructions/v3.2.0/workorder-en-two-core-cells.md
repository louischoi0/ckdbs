# Work order EN - the two-core cells: a kick that cannot be lost, and a row's loser that cannot lose forever

Status: **WRITTEN 2026-11-28, not opened** (`raft-marks-2026-11-28.md`
§17), on the operator's word *"investigate the cause of the two-core
flakes and write the milestone that resolves them, then push"*. Written by
CLA in a claude.ai session directly on `main` at `e571ae57`
(`v3.4.17-312-ge571ae57`), documents only. **Every `[source-read]` claim
with a `path:line` was read by CLA at `e571ae57`**; nothing in this order
was built or run in this session, and no figure below is CLA's
measurement: every number is quoted from the record it names.

Tags: `[operator]`, `[source-read]`, `[measured]` (quoted, with its
record), `[design]`, `[quiet-wrong]`, as in DT's order.

Related: `docs/spec/sched.md` §4, §5; `docs/spec/txn.md` §5;
`docs/spec/wal.md` §6; `include/kds/sched/waker_table.hpp`;
`docs/inflight/known-gaps.md` (Testing: "Two rig cells fail
intermittently on a 24-CPU host", the `ContentionCountersTest` entry, and
both `DurableKickRigTest` entries);
`docs/inflight/bugs/two-core-increment-starves-one-core.md`;
`instructions/v3.2.0/workorder-ee-decide-slot.md` (EE, abandoned at S0,
C3 re-proposed); `instructions/v3.2.0/workorder-ec-multi-core-crash-harness.md`
(EC, written, which EN does not enter).

## 0. What EN is

`[measured]` Three two-core cells fail intermittently on the 24-CPU host,
and **every milestone since DT has closed past them** as "known two-core
flakes that fail at the base commit too": DT, DU, DS, DV (three suites,
none green), DW, DX, EB, EH, and the push of EF, EG and EH (§16 of the
day's raft marks: 5,437 of 5,438). The reading "fails at the base too"
shows a cell is not a regression; it does not show the cell is not a
defect, and the list has grown by one since it was first written (the
`ContentionCountersTest` cell joined at the DT/DU merge).

`[source-read]` CLA read the three cells and the paths under them. **They
are two causes, not three**:

1. **A cross-core kick can be lost by construction, and a lost kick costs
   the destination one whole idle block** (§1.1). Two of the three cells -
   `DurableKickRigTest` and `ContentionCountersTest` - are a `group`
   commit's waiter on a peer core paying that block. In the rig the block is
   5 s; **in a server it is 10 ms** (`max_idle_block_ms`), so the same race
   is a commit tail-latency cost on every multi-core instance, unmeasured.
2. **Under `READ COMMITTED` a single-row writer that loses
   first-updater-wins to a committed writer is refused, and nothing bounds
   how many times in a row it can lose** (§1.2). The third cell -
   `SetExprTwoCoreRigTest` - is that starvation on a hot row, which is the
   counter, balance and stock-decrement shape this engine is for.

`[design]` EN is: **close the kick** (S1), **bound the loser** (S2, behind
EN-Q2, which changes a `txn.md` §5 sentence and so is the operator's),
and **end the habit** - the suite's reading at a close admits no failure
outside a named list, and the list is empty when EN closes (S3).

## 1. The tree as read

### 1.1 The lost kick `[source-read]`

The kick (`include/kds/sched/waker_table.hpp:120-129`): the kicker reads
the destination's `sleeping` flag **relaxed** and writes the eventfd only
if it is set; otherwise it counts `skipped_` (`:125`) and returns. The
header states the race and its price (`:111-119`): "a publisher landing
between the destination's last look and its raising of the flag reads
clear, skips the kick, and the destination waits out one block
(`max_idle_block_ms`) ... Closing the window would take a `seq_cst` fence
here, one after the flag's store, and the reactor re-polling its parked
tasks after raising the flag. No fence is built".

The reactor (`src/sched/scheduler.cpp:365-389`): it computes its timeout,
stores `sleeping_ = true` relaxed (`:382`), and blocks in `PollReady`
(`:389`). The comment at `:365-368`: "Nothing is re-read after it, so a
kick racing the store is skipped and costs one idle block".

The WAL writer's durable kick (`src/wal/writer.cpp:157-166`): after the
watermark moves it takes a `seq_cst` fence (`:163`) and kicks every core
whose `durable_waiters_` count is above 0 (`:165`). Its other half,
`EnterDurableWait` (`include/kds/wal/writer.hpp:153-156`), increments the
count and fences. **That pair closes the first race** (waiter versus
watermark: one of them sees the other) **and not the second** (the kick
versus the destination's flag), which `writer.hpp:133-139` says in as many
words.

So a `group` commit on a peer core is lost when: the reactor's last poll
of the parked commit finds the watermark short; the writer moves the
watermark, fences, reads the count, and reads `sleeping` while it is still
clear; the reactor then raises the flag and blocks. It sleeps the whole
block though the event it waits for has happened.

The block: `max_idle_block_ms = 10` in a server
(`include/kds/sched/scheduler.hpp:78`), **5,000 in the two-core rig**
(`tests/two_core_rig.hpp:158`). The rig's reactor threads are not pinned
(no affinity call in `tests/two_core_rig.{hpp,cpp}`; the server's
reactors are pinned by `src/server/expeditor.cpp:1639-1640`), so on the
hybrid host one rig reactor can run on an E-core or be descheduled, which
widens the window between the last poll and the flag's store.

**The same `Kick` ends every cross-core wait** (`waker_table.hpp:20-22`):
the call sites at `e571ae57` are the WAL writer (`writer.cpp:165`), a lock
release (`src/txn/lock_table.cpp:444`), a visibility ceiling
(`src/txn/instance_visibility.cpp:121`), a connection handoff
(`src/server/connection_handoff.cpp:47`) and the expeditor
(`src/server/expeditor.cpp:1689`, `:2096`). Only the page reader already
writes unconditionally (`src/storage/page_reader.cpp:72`, `Wake`, DE-Q14).
A lost kick at a lock release costs the woken waiter the same block.

How the two cells meet it:

- `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`
  (`tests/durable_kick_rig_test.cpp:100`): five `group` inserts on the
  peer core, each to be answered within 2 s of a 5 s block. `[measured]`
  every failure whose output the records quote prints `kicks skipped 1`
  (`known-gaps.md`, both entries): 5 of 30 alone at `60dc593af`, 4 of 15
  alone at DV's tree, 22 of 200 at a load of 15 to 19, 3 of 200 on
  `07f4552a`, more than half of DT's `-j12` suites.
- `ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`
  (`tests/contention_counters_test.cpp:250`, bound at `:304`): core 0
  inserts `relaxed`, core 1 `group`, 200 rows each, 60 s. `[measured]` the
  records: every expiry has core 0 at 200 of 200 and core 1 short ("core 1
  at 152 of 200"), on a quiet host as on a loaded one; 4 of 20 alone at
  `60dc593af`; twenty runs taking 532 s, where the cell passes in 0.06 s
  in another run. **CLA's reading, not measured**: `relaxed` never parks on
  durability and `group` does, so only core 1 can pay a lost kick; 48 rows
  short after 60 s is twelve 5 s blocks, and a 26.6 s mean run is about
  five. The record's own open question - "a slow host cannot be told from
  a stopped writer" - is answered by this reading if S0 shows
  `kicks_skipped` rising with the stall.

### 1.2 The row's loser `[source-read]`

`txn.md` §5 (lines 897-901): "Under `READ COMMITTED` the last arm can
still fire in the narrow window between a statement's snapshot and its
write; KDS aborts retryably rather than re-reading. That is stricter than
PostgreSQL's `READ COMMITTED` and was a deliberate simplification - there
is no re-read loop."

The code: an autocommit statement's view is minted at its transaction's
`Begin` (`src/txn/manager.cpp:150`); the claim checks the row's current
writer against that view and, when the writer committed after it, returns
`TxnConflict` "row id=... was written by transaction ..."
(`src/txn/manager.cpp:197-215`). An undecided writer is waited for (§5,
"Detection is followed by a wait, not a refusal"); **a writer that
decided and released between the loser's `Begin` and its claim is not
waited for and is refused**.

`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`
(`tests/set_expr_two_core_rig_test.cpp`; `kIncrementsPerCore = 600`,
`kRetriesPerIncrement = 200` at `:28-29`): two cores each bump row 1 600
times and retry a retryable refusal at once. `[measured]` the bug file:
12 of 60 alone at `60dc593af` and at DU-S3's tree; 9 of 60 at DV's;
359 of 800 eight at a time; one failure in 86 ms with core 0 out of
retries on its eleventh increment while core 1 applied 600. No increment
is lost or doubled in any failing run: **it is a liveness defect, not a
quiet-wrong one**.

`[design]` Why one side keeps losing: the winner's cycle (claim, write,
commit, release) can fit inside the loser's window (`Begin`, then the walk
to the claim) every time, and the loser's retry mints a fresh view that is
already stale by the time it claims. Nothing in the loser's retry takes
anything the winner must respect, so the loser has no way to stop losing.
A slower reactor - an unpinned rig thread on an E-core - makes the
asymmetry persistent. **This is not a wake**: the cell's driver kicks core
1 continuously (`KickUntil` with `Kick(1)` in its predicate), so S1 is not
expected to move it.

### 1.3 What no record has measured

- How often a server's peer commit pays the 10 ms block, and what that does
  to a `group` commit's p99 at eight cores (`known-gaps.md` says so).
- Whether the starvation reaches a client of the server, where reactors
  are pinned and a statement crosses a socket: the cell is the rig's.
- Any of the three cells under TSan (`known-gaps.md` notes two-core cells
  never run under `tsan`).

## 2. The mechanism `[design]`

### 2.1 The kick, closed (S1)

The standard pair (a Dekker handshake), on the registry, so every call
site in §1.1 is covered at once:

- **Kicker**: set the destination's `pending` word (release), then a
  `seq_cst` fence, then read `sleeping`; write the eventfd if it is set.
- **Reactor**: store `sleeping = true`, then a `seq_cst` fence, then
  exchange `pending` to 0; if it was set, do not block (timeout 0) and
  clear `sleeping`. After a block, `pending` is exchanged to 0 as well.

Either the reactor sees `pending` and does not sleep, or the kicker sees
`sleeping` and writes the eventfd: no kick is lost. **A busy reactor
pays nothing new**: the fence is on the path that is about to sleep, which
`scheduler.cpp:370-374` already confines to `timeout_ms != 0`. **A single
core pays nothing**: the path is guarded by `waker_.has_value()`
(`:374`), absent on every single-core build (Guideline 2). The kicker
pays one fence a kick; the WAL writer already pays one a round
(`writer.cpp:163`).

`skipped_` stays: it now counts kicks a busy destination made
unnecessary, which is what its comment (`waker_table.hpp:142-144`) says it
is for.

### 2.2 The loser, bounded (S2, behind EN-Q2)

The proposal (EN-Q2 (a)): **a `READ COMMITTED` statement that has written
nothing and is refused because the row's writer committed after its view
is re-run once by the engine, with its row claim taken before its view
is minted.** Holding the tuple's `U` (or `X`) first, the re-run's view
sees the newest committed version and no writer can commit a newer one
before its claim; the competing writer now waits on it, as §5 already
makes writers wait on an undecided holder. One re-run, not a loop: the
re-run cannot meet the committed-after-the-view arm on the row it holds.

`[quiet-wrong]` What must not change: `REPEATABLE READ` is untouched (its
view is the transaction's and a refusal is its contract); a statement that
has written a row is not re-run (the refusal stands, as today); a
statement that names several rows or a window is out of S2's first scope
unless the S0 census shows its claim order admits the same argument; the
re-run evaluates the WHERE against the version its view sees, which is
the newest committed one, the answer PostgreSQL's re-check gives and the
one §5's F4 table already gives for a miss.

The other readings, for EN-Q2: (b) keep the refusal and give the retry
something the winner respects - a back-off or a registration on the row
that the next committer must yield to - which is EE's C3 in another form;
(c) keep the engine as it is, document the unbounded loss as a product
property, and change the cell to a fairness-independent stop.

## 3. Rules

- **EN-R1** No cross-core kick is lost: after S1, a reactor that blocks
  with a kick published for it is woken by that kick. Shown by the
  argument in §2.1 and by a cell that forces the window (a `SimWakerTable`
  seed, or a test seam between the flag's store and the block).
- **EN-R2** Zero overhead at one core, and nothing new on a busy
  reactor's iteration (Guideline 2; `scheduler.cpp:370-374`).
- **EN-R3** `[quiet-wrong]` S2 changes no answer: no increment lost or
  applied twice, `REPEATABLE READ` unchanged, a statement that has written
  a row never re-run, and the WHERE judged on the version the re-run's
  view sees.
- **EN-R4** No cell's bound is widened to make it pass. A cell is changed
  only where it asserts more than the contract it tests, and only after
  S1 or S2 has decided what the contract is.
- **EN-R5** Every price is measured (Guideline 2): the kicker's fence, the
  sleep path's fence, the re-run's cost on a contended and an uncontended
  point update, each A/B against EN's opening binary.
- **EN-R6** The suite at EN's close is green, every run; a failure
  outside the named list of S3 fails the close.

## 4. Items for the operator (EN-Q), CLA's proposals

| item | question | proposal |
|---|---|---|
| EN-Q0 | Open EN now, before EI to EM | **(a)** yes: EI (a two-phase participant) and EK (write skew) add cross-core waits and hot-row contention, and both meet these two causes |
| EN-Q1 | How the kick is closed | **(a)** the pending word and the two fences on the registry (§2.1); (b) `Wake` (unconditional) for the WAL writer's durable kick only, one syscall a busy core a sync; (c) leave the race, set the rig's block to the server's 10 ms and price the server's loss only |
| EN-Q2 | The row's loser | **(a)** one engine re-run with the claim before the view (§2.2), revising `txn.md` §5's "no re-read loop" sentence to "one re-run, claim first"; (b) a back-off or a row registration; (c) unchanged, documented, the cell's stop changed |
| EN-Q3 | Scope of S2 | **(a)** autocommit and explicit `READ COMMITTED` single-row writes (a tuple key or a unique key named whole) that have written nothing; windows and lists only if S0's census admits them |
| EN-Q4 | The suite's close reading | **(a)** a close's suite runs are green; a known failure is named in the order with its record and makes the close narrow, never "a pass" |
| EN-Q5 | The rig's reactors | **(a)** stay unpinned, as the host gives them: the cells must hold without help; pinning is an S0 arm only |

### Marks

None yet. EN-Q2 is a change of a `txn.md` §5 contract sentence and is
not adopted on the standing go-ahead: it waits for the operator's mark.

## 5. Stages

- **EN-S0 census and premise** (measurement and reading only):
  (1) the three cells alone, 60 runs each, Debug and Release, in three
  arms - both rig reactors on P-cores, one on an E-core, unpinned - with
  `kicks_skipped`, idle blocks and per-writer progress recorded per run;
  (2) the same under `-j8` with eight copies; (3) the three cells under
  TSan; (4) a server at eight cores, `group`, eight and 32 bookers
  (scenario 2 on EF-S1's drivers): skipped kicks to a core with a durable
  waiter, and the commit p50/p99, so the 10 ms price is a number;
  (5) the census of every write shape's order of view mint and row claim,
  for EN-Q3. **Premise to confirm**: every `DurableKickRigTest` and
  `ContentionCountersTest` failure has a skipped kick to the stalled core;
  no `SetExprTwoCoreRigTest` failure needs one.
- **EN-S1 the kick closed** (EN-Q1): §2.1, a cell that forces the window,
  `sched.md` §4 and `waker_table.hpp`'s comments revised to the new
  contract, the A/B of EN-R5.
- **EN-S2 the loser bounded** (EN-Q2, EN-Q3; only on the operator's
  mark): §2.2, `txn.md` §5 revised, a cell that forces the
  committed-after-the-view arm on a re-run, the cell of §1.2 at 0 of 60
  alone and 0 of 400 eight at a time, the A/B of EN-R5 including a hot-row
  booking.
- **EN-S3 the suite's reading** (EN-Q4): three full suites green at the
  closing commit; `known-gaps.md`'s four entries and the bug file closed
  with the commit that closed each; `CLAUDE.md`'s working rules carry the
  close reading.
- **EN-S4 the close**: the standard cells and scenario 2 on EF-S1's
  drivers, OLD against NEW interleaved; nothing slower outside the spread.

## 6. What EN does not do

- It does not touch the WAL writer's first fence pair, which is sound
  (§1.1).
- It does not pin the rig's reactors to make the cells pass (EN-Q5).
- It does not change `REPEATABLE READ`, a write that has written rows, or
  the lock modes of CP-S2b.
- It does not build EE's decide slot; S2 (b) would be its successor if the
  operator chooses it.
- It does not open EC (the multi-core crash harness), which would be the
  next home of a forced-window cell if S1's seam is not enough.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `main` at `e571ae57`, documents only: this order, its rows in
`instructions/v3.2.0/index.md` and `instructions/milestones.md`, and
`raft-marks-2026-11-28.md` §17. Not opened; no build, no run.
