# Work order CZ - a checkpoint that does not hold its reactor on a segment roll: the premise measured before anything is built

Status: **WRITTEN 2026-11-18, not opened.** Written on worktree
`bl-rung3-probe` from `3b3f4851` (`v3.4.12-25-g3b3f4851`) on the operator's
word of 2026-11-18 (`raft-marks-2026-11-18.md` §13): *"For now, leave it as
a work order."* No stage is started. CZ-S0 waits for the operator's word
and for the marks on §4's items. Every `[source-read]` claim below was read
at `3b3f4851`.

**Exploratory, and the measurement comes first.** CZ-S0 and CZ-S1 can each
end the order with nothing built, and that is a result, not a failure.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

Related:
- `docs/spec/wal.md` §4.1, §6, §8-1 (the roll's order, the latch, the
  WAL-before-data gate);
- `docs/spec/sched.md` §4 (the checkpoint task and its rotation);
- `docs/inflight/known-gaps.md` (the WAL roll entry; BL-R7's remainder);
- `instructions/v3.0.0/workorder-bq-wal-segment-roll.md` (the roll priced;
  its rungs; H6);
- `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
  §5.2 bar (v) and §6 ("Rung 3's probe measured 2026-11-18");
- `bench/v3.4.12/results-bl-rung3-probe-v3.4.12-24-g67973ebc.md`;
- `bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md`,
  `bench/v3.0.0/results-bq-s2-rung1-v2.7.0-909-g4cf30270.md`.

## 0. What CZ is

`[operator]` The operator asked what the checkpoint's wait on a segment
roll is and how it could be improved, and then asked for the answer to be
left as a work order.

`[design]` BL closed with bar (v) not met: a `transactional` reader on a
core with no batch session still sees a p99 many times its isolated one.
In the rung-3 probe every stall of 10 ms or more overlaps a checkpoint
run. The checkpoint task, which runs on every core in turn, takes the
WAL's stream latch with a blocking lock. When a segment roll holds that
latch, the reader core's reactor thread stops, and every session on the
core waits with it.

The roll's own length is BQ's subject, and BQ closed with the latch kept.
CZ does not reopen that. CZ asks a narrower question, in this order:

1. whether the reader's excess in the partitioned cell goes away when
   no checkpoint runs - a necessary condition, not a proof that the
   blocking wait is the whole of it (CZ-S0);
2. what the most a non-blocking checkpoint could return is (CZ-S1);
3. only then, what the change is (CZ-S2) and its build (CZ-S3).

The checkpoint task is already a coroutine that parks. What CZ would
change is how it waits for the latch, and nothing about what it waits for.

## 1. The tree as read

### 1.1 What is measured `[measured]`

**The rung-3 probe** (`results-bl-rung3-probe-v3.4.12-24-g67973ebc.md`):
`cores = 4`, `relaxed`, the balancer placing, 6 `transactional` point
readers, an `analytic` walker and a `background` 1,024-row inserter on
core 0, 5 runs. The host is an 8 vCPU Intel Xeon Platinum 8581C.

| reading | value |
|---|---|
| reader schedule p99, `levels` arm | 12.2 ms (median of 5; 3.5-72.2 ms) |
| reader schedule p99, isolated | 649 µs (median of 5; 476 µs-1.3 ms) |
| reader samples at or above 10 ms | 227 over 5 runs, in 11 stall episodes (2-3 a run) |
| of those, overlapping a `[checkpoint] run:` interval | 227 |
| of those, overlapping a `[wal] roll` hold | 216 |
| longest roll hold inside the window | 349-761 ms a run |
| longest `system` poll on a read core | 120-322 ms (the bold cells of §3.3 there; a maximum since the server's start) |
| share of a window covered by checkpoint runs / by roll holds | 21-41 % / 25-34 % |
| foreground polls over 10 ms on a read core | 0 in every arm |
| WAL rolls a 10 s window | 11-12, at 73-75 MB/s of log |

**An arithmetic ceiling from the same runs** (`ceiling.py` and `ceiling.md`
in that file's archive). Each run has 4,006 reader samples, and the driver
stamps every one at or above 1 ms. Taking out the stamped samples that
overlap a checkpoint run leaves a p99 under 1 ms in 4 of 5 runs and 1.1 ms
in the fifth. Taking out those that overlap a roll hold leaves it under
1 ms in 3 of 5, and 1.2 and 1.7 ms in the others. Twice the isolated
median is 1.3 ms.

This is arithmetic on samples, not a build without the stall, and it is
generous:
- a sample that stays may have queued behind one that was taken out;
- checkpoint runs cover 21-41 % of a window, so some of the samples taken
  out would have been slow for another reason;
- the checkpoint's own page writes are still in the run.

BQ's rung-2 ceiling in the table below (2.6-145 ms) and this one (under
1 ms to 1.7 ms) are not reconciled. They differ in commit, host, placement
and what was taken out. CZ-S0 re-reads BQ's.

**What BQ measured at `4cf30270`**, 677 commits ago and on another
host:

| reading | value |
|---|---|
| roll hold at `relaxed` under one bulk writer | p50 about 330 ms, p99 about 790 ms, max 1.2 s |
| share of the hold in step c (the sync of the segment left), rolls at or above p99 | 89.8-99.5 %, almost all of it the wait for the writer's sync in flight |
| H4 option 3 (no zero prewrite where `posix_fallocate` reserved) | hold median 325 ms to 16 ms; pooled p99 809 ms to 1.14 s, inside the spread; 13 % of rolls carry 80 % of the hold; the bulk cell gains 43 % |
| H2 (a larger segment) | the time held is flat across sizes |
| rung 2's ceiling (a drain that does not block on a roll) | C1 reader p99 still 2.6-145 ms with every roll-aligned stall removed, across three driver-forced placements |
| peers' drains at `relaxed` | carry none of the stream-latch wait |

**Not measured, and it is the premise:**

- the reader's p99 in the partitioned cell on a build whose checkpoint
  does not block on the latch;
- which of the checkpoint's latch sites carries the wait beyond the begin
  record's (read in §1.2): the log park's flush, the end record, the
  recycle;
- whether anything else on a reader-only core takes the stream latch on
  the reactor thread;
- what a checkpoint that waits longer costs: its wall time, the log it
  keeps from recycling, the bulk writer's rate.

### 1.2 What the tree does `[source-read]` at `3b3f4851`

- **A roll runs under the stream latch.** `WalStream::Append` takes the
  latch (`src/wal/stream.cpp:392`). When the record does not fit,
  `AppendLocked` seals (`:426`) and rolls (`:430`) before the latch is
  released. `Roll` (`:271`) reaches `StartSegment` (`:154`), which syncs
  the segment left through `SyncDevice` (`:113`) and creates the next.
- **`SyncDevice` waits for the writer's sync in flight.** It takes
  `sync_mutex_` with a blocking lock (`:117`) and then runs the device
  sync itself (`:132`).
- **The stream latch is one blocking mutex for the instance.**
  `StreamLatchHold` (`include/kds/wal/stream.hpp:404-414`) is a
  `LatchGuard`, and `Latch::lock` is `std::mutex::lock` after one
  `try_lock` (`include/kds/base/latch.hpp:66-77`). `Latch::try_lock`
  exists (`:77`); no stream path uses it.
- **A one-core instance has no stream latch.** `latch_` is null unless the
  stream is shared (`src/wal/stream.cpp:44`).
- **The cadence task takes the latch at five kinds of site**, each on its
  reactor thread:
  - `LogBegin`'s `Append` of `CHECKPOINT_BEGIN`
    (`src/wal/checkpointer.cpp:46`) and the assertion snapshots' appends
    (`:142`, `:163`);
  - `AppendEnd`'s `Append` of `CHECKPOINT_END` (`:338`);
  - `ParkOnLog` (`:637-654`): it calls `WalManager::RequestSyncNow`
    (`:640`), whose `stream_->Flush()` (`src/wal/manager.cpp:259`) takes
    the latch (`src/wal/stream.cpp:476`);
  - the inline WAL gate a step or a publish batch takes past the park
    bound (below): `AwaitWalGate`'s and `WriteBack`'s `EnsureDurable`
    (`src/storage/device_page_store.cpp:1898`, `:2161`) reach
    `WalManager::Sync` (`src/wal/manager.cpp:314`). On a peer that flushes
    through the latch (`:183`) and then blocks on the writer's condition
    variable until the sync lands (`:189`, `src/wal/writer.cpp:35-43`); on
    core 0 it is `WalStream::Sync` (`manager.cpp:220`), the latch and then
    `sync_mutex_` (`stream.cpp:551-553`);
  - the recycle at the anchor's commit: `anchor_.Commit()`
    (`src/wal/checkpointer.cpp:581`) runs the recycler when the redo start
    advances (`src/server/superblock_checkpoint_anchor.cpp:205`, wired at
    `src/server/expeditor.cpp:1237-1244`), which reaches
    `WalStream::DetachBelow` (`src/wal/manager.cpp:146`) and its latch
    (`src/wal/stream.cpp:338`).
- **After its flush, `ParkOnLog` already parks.** It `co_await`s a
  `WaitUntil` on the durable LSN (`src/wal/checkpointer.cpp:644-652`).
  Inside `ParkOnLog` only the flush blocks the thread.
- **A step parks on the log a bounded number of times.** After
  `kGateParks` = 16 parks the step's own inline gate takes what is left
  (`include/kds/wal/checkpointer.hpp:521-527`,
  `src/wal/checkpointer.cpp:497`, `:622`).
- **The run line times two of the sites.** `start_*` is `Start`, the
  begin append inside it (`src/wal/checkpointer.cpp:191`), and
  `log_gate_batch_*`/`log_gate_run_*` are the inline gate
  (`src/storage/device_page_store.cpp:1897`, `:2160`). `log_park_us`
  starts after the flush (`src/wal/checkpointer.cpp:646`), and the end
  append and the recycle are not timed.
- **The cadence is a setting.** The file key `checkpoint_interval_ms`
  (`src/server/expeditor.cpp:325-334`) sets `checkpoint_interval_ns`, 5 s
  by default (`include/kds/server/expeditor.hpp:201`). At 0 neither core
  0's cadence (`src/server/expeditor.cpp:1993`) nor a peer's
  (`src/server/core_runtime.cpp:451`) is armed.
- **The next segment's body is zero-filled.** `Prewrite`
  (`src/wal/file_log_device.cpp:135`, called at `:383`) writes the whole
  segment and syncs it, so that a later commit's `fsync` does not pay an
  extent conversion (`:128-134`). The preparer's fill competes for the
  device with the writer's sync that a roll waits out (BQ-S2, inferred
  there).

**A reading from the probe's server log, not yet explained.** A
reader-only core's checkpoint can find thousands of dirty pages: core 2's
run in `levels` r4 wrote 2,601 pages over 1.37 s, with 672 ms parked on
the log (line 42 of that run's `.s.log` in the probe's archive). That
core's next run (line 64) is the shutdown checkpoint, after the shutdown
sync, so its zero pages say nothing. CZ-S0 reads where they come from.

**The begin site, read from the same logs.** On read core 3, `Start` held
its poll 125, 120, 89 and 94 ms in r1, r3, r4 and r5 (`start_max_us`; r2's
longest is 18 ms, on core 2). That is core 3's longest `system` poll in r1
and r3 (125.2 and 120.0 ms, §3.3 there), and core 2's 18.3 ms in r2.
`Start`'s other work is the two snapshots, so the time is most likely the
begin append's latch wait (inferred). The other bold cells of §3.3
(163-322 ms) match no `Start`: they are another site. The inline gate ran
0-2 times a run, 1-2 ms each.

## 2. The candidates `[design]`

Priced by CZ-S0 and CZ-S1; none is chosen here.

- **C0 - nothing.** The verdict if the reader's excess does not follow
  the checkpoint's blocking wait.
- **C1 - the checkpoint waits for the latch by parking.** At the
  checkpoint's latch sites only, the task tries the latch. If it is busy,
  the task parks and tries again at a later poll. The roll, its order and
  every other caller stay as they are.
  - **C1a - the log park's flush.** `ParkOnLog` asks the writer and parks
    without this thread's flush when the latch is busy.
    - Under a roll this is covered `[source-read]`: the roll's seal drains
      the ring, `StartSegment` publishes the durable LSN after its sync
      (`src/wal/stream.cpp:164-165`), and `durable_lsn()` is the larger of
      the stream's and the writer's (`include/kds/wal/manager.hpp:263-267`).
    - The hazard is a latch busy for a holder that is **not** a roll: the
      skipped flush then waits for core 0's next drain tick. CZ-S2 says
      what bounds that wait.
  - **C1b - the begin and end records.** A try-append that reports "busy"
    to the task, which parks and appends later (CZ-R3).
  - **C1c - the recycle at the anchor's commit.** `DetachBelow` tried and,
    when busy, left for the next run. Recycling later is safe by
    construction; CZ-S2 checks it against the live-segment bound.
  - **The inline WAL gate stays blocking.** It is `EnsureDurable`, the
    fallback past the park bound, and it ran 0-2 times a run at 1-2 ms in
    the probe. A park inside it would hold writeback claims, so it is not
    made parkable (CZ-Q10).
- **C2 - no zero prewrite where `posix_fallocate` reserved the extents**
  (BQ's H4 option 3). It shortens the common roll and leaves the tail, so
  by BQ-S2's numbers it cannot meet a p99 bar alone. It is listed because
  it is independent of C1 and its own guard was never evaluated.
- **Not a candidate: releasing the latch for the roll's I/O** (BQ's rung
  4, H6). It is `[quiet-wrong]` and needs the operator's own word
  (BQ-Q5 (b)). CZ does not take it up (CZ-Q7).

## 3. Rules

Binding from CZ-S2 on, if the order reaches it.

- **CZ-R1 - the roll is untouched.** BQ-R1's three clauses hold as
  written: the segment left is synced before the next exists, the next
  header is durable before its name, and a failed step stops the log
  before any later flush publishes past it.
- **CZ-R2 `[quiet-wrong]` - WAL before data.** No page reaches the data
  file ahead of its log record being durable (`wal.md` §8-1). A park
  replaces a block. It never replaces the wait.
- **CZ-R3 `[quiet-wrong]` - a begin record describes the instant it is
  appended at.** If the task parks between capturing the snapshot (active
  transactions, dirty pages) and appending it, the snapshot is taken
  again, unless CZ-S0 shows the gap is already covered.
  - The gap exists today and is not the roll's alone: `Start` snapshots
    both tables and then appends (`src/wal/checkpointer.cpp:192-197`), and
    at `cores > 1` another core can append between the two.
  - CZ-S0 reads what covers it. If nothing does, that is a live
    `[quiet-wrong]` bug at `cores > 1` and not a rule for later: it goes
    to the operator at once as a `docs/inflight/bugs/` entry, outside CZ's
    stages.
- **CZ-R4 - a checkpoint always finishes.** The tries are bounded, and
  past the bound the task takes the blocking path, as `kGateParks` does
  today. A checkpoint that never completes keeps the log from recycling.
- **CZ-R5 - only the checkpoint's sites change.** A commit, a drain,
  `EnsureDurable` (the checkpoint's inline gate included) and `SyncAll`
  keep the blocking latch.
- **CZ-R9 - a parked task holds nothing a roll or a writer needs.** No
  page latch, pin or writeback claim is held across a park for the latch.
  CZ-S2 lists what the task holds at each site; the checkpoint gate's own
  hold is the one the existing parks already keep.
- **CZ-R6 - `cores = 1` is untouched.** A one-core stream has no latch.
- **CZ-R7 - nothing on disk moves, and no setting is added.**
- **CZ-R8 - the fail-stop answers a parked task.** A log stopped while
  the task is parked ends the park with the stop's status, as `ParkOnLog`
  does today.

## 4. Items for the operator (CZ-Q), CLA's proposals

None is marked. (a) is CLA's proposal in each.

| | item | (a) | other |
|---|---|---|---|
| CZ-Q0 | Is CZ exploratory - may S0 or S1 close it with nothing built? | yes | (b) build C1 whatever S0 shows |
| CZ-Q1 | The bar. | **V0** (after S0): go on only if, with the cadence checkpoint off, the `levels` reader schedule p99 is within 2x the isolated p99 - the median of 5 against the isolated median of 5, and each run against the isolated run interleaved with it in at least 4 of 5 pairs. V0 is a necessary gate: failing it ends C1; passing it does not prove C1. **V1** (at the close): the same bar on the built change, with the checkpoint on | (b) the operator's numbers |
| CZ-Q2 | The guards at the close. | the bulk cell loses at most 3 % (BQ's C3 clause); a checkpoint's wall time and the live segment count at the window's end are reported, with no bar | (b) a bar on either |
| CZ-Q3 | Which candidate is built when V0 passes? | C1 alone; C2 is priced in S1 and built only if C1 misses V1 and the operator marks it | (b) C1 and C2 together; (c) C2 first |
| CZ-Q4 | The bound on tries before the blocking path. | 16, `kGateParks`' number, and S3 reports how often it is reached | (b) another number |
| CZ-Q5 | `group` durability. | one `levels` cell at `group` at the close, reported with no bar | (b) the bar at both; (c) not measured |
| CZ-Q6 | BL's record. | at the close CZ re-measures bar (v), amends `known-gaps.md`'s BL-R7 remainder and appends to the BL order's §6; BL is not reopened | (b) CZ does not touch BL's texts |
| CZ-Q7 | H6 (the latch released for the roll's I/O). | out of CZ | (b) in, on the operator's own word |
| CZ-Q8 | The measurement budget. | the standing one: 20 minutes a stage, about an hour at the close, cuts named in the results file | (b) lifted for S0 |
| CZ-Q9 | The letter. | CZ | (b) another |
| CZ-Q10 | The two sites C1's first form did not reach. | the recycle joins C1 (C1c); the inline WAL gate stays blocking, and S1 counts how often and how long it is taken | (b) both stay blocking; (c) the inline gate is made parkable too |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **CZ-S0 - premise and census** | No engine code. **Measurement** (M1-M4 below), `build-release`, inside the budget. **Census**: every reactor-thread taker of the stream latch on a core with no write; what covers the time between a begin record's snapshot and its append (CZ-R3); where a reader-only core's dirty pages come from; BQ's rung-2 ceiling re-read against §1.1's. | **V0**: go to S1, or close with the numbers |
| **CZ-S1 - the ceiling, a spike that stays on the branch** | A measurement-only patch, never landed: the checkpoint's latch sites try and park. It carries no proof and is run only on the `levels` cell. C2 is spiked the same way, alone and with C1. | the ceiling against CZ-Q1, and C0, C1 or C1 with C2 |
| **CZ-S2 - the model, on paper** | Each site against the code: the try, the park and its wake, the bound, the begin snapshot (CZ-R3), the durable publication C1a rests on, the fail-stop. The tests owed. The amendments owed (`wal.md` §6, `sched.md` §4). | items back to the operator before any code |
| **CZ-S3 - the build** | S3a the log park's flush (C1a); S3b the begin and end records (C1b); S3c the recycle (C1c). Each with its review and the full suite. | a branch at the push gate |
| **CZ-S4 - the close** | The interleaved A/B over the whole change at `cores = 1` (nil is the claim) and `cores = 4`; V1; the guards (CZ-Q2); the `group` cell (CZ-Q5); the texts (CZ-Q6); the row flipped. | closed, or closed narrow |

**CZ-S0's cells.** Each is the probe's `levels` cell
(`tools/bl_mixed.py --arm mixed --levels ... --meta-cores`, the probe's
`r3-mixed.sh`), 5 runs, interleaved with its isolated arm.

- **M1 - the cadence off.** `--extra-conf "checkpoint_interval_ms = 0"`
  against the default. The reader p99, the read cores' `system` polls
  over 10 ms, the roll-blocked wait, and **the roll-hold distribution in
  both arms**. This is V0's number.
  - The arm is confounded, and the results file says so. With no
    checkpoint there is no writeback on the device, which BQ-S1 names as
    part of what makes the writer's sync slow, so the rolls themselves may
    shorten. The checkpoint's on-reactor page writes go too, and the log
    stops recycling.
  - If the roll holds shorten in the cadence-off arm, the reader's gain is
    read against that, not credited to the latch wait.
- **M2 - the sites.** From M1's default arm: each checkpoint run's
  `log_park_us`, `start_max_us`, `log_gate_*` and wall time against the
  roll holds it overlaps, from the server log. The begin site and the
  inline gate can be read there (§1.2); the park's flush, the end append
  and the recycle cannot, so M2 says what S1's spike must count.
- **M3 - what the cadence-off arm keeps.** The live segment count and the
  bulk writer's rate with no checkpoint running, so that M1 is not read
  as a fix. A server with no cadence checkpoint recycles no log.
- **M4 - the rate.** One arm at half the inserter's batch, to see whether
  the excess follows the roll rate.

**Tests CZ-S3 owes.** Each forced with a barrier and repeated:
- a roll held while a checkpoint asks for the log park's flush;
- a roll held across a begin record's append, with a page dirtied in the
  gap;
- the bound reached, and the blocking path taken;
- the log stopped while the task is parked;
- a crash inside the park, in `scripts/sim.sh`'s corpus;
- two cores' checkpoints parked on one roll.
- the latch held by a holder that is not a roll while the park's flush
  is skipped;
- the recycle skipped on a busy latch, and taken by the next run.

Whether the simulator's instance has a shared stream was not read. CZ-S2
reads it, and where it has none the armed multi-core rigs carry the latch
cells.

## 6. What CZ does not do

- No change to the roll, its steps or its durability order.
- No release of the stream latch for the roll's I/O (H6).
- No change to the segment size or to the sync's breadth (BQ's H2 and H3,
  measured and rejected).
- No change to a commit's, a drain's or `EnsureDurable`'s wait.
- No change to the checkpoint's cadence or rotation as the fix. The
  cadence-off arm is a probe.
- No rung 3 or rung 4 of BL-R7.
- Nothing at `cores = 1`.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

Written on worktree `bl-rung3-probe` from `3b3f4851`
(`v3.4.12-25-g3b3f4851`), documents and one analysis script
(`ceiling.py`, with its output, in the rung-3 probe's archive). The
worktree is the probe's, not one named for CZ: the session could not open
a second worktree. The letter CZ is CLA's proposal. No suite was executed:
the commit changes no build input.

**Reviewed by `critics-developer`** against the source at `3b3f4851` and
the probe's raw data, before the commit.
- **Corrected by the review:**
  - §1.2's latch sites were three and are five (the inline WAL gate and
    the recycle at the anchor's commit);
  - the cadence's citations;
  - what the run line times (the begin site can be read from the log).
- **Applied from its findings:**
  - V0 is stated as a necessary gate with its pairing, and M1's confound
    is named;
  - C1c, CZ-R9 and CZ-Q10 were added;
  - C1a's publication is read from the tree, and its hazard restated as
    the non-roll holder;
  - CZ-R3's gap is stated as present today;
  - the ceiling's caveats, and the 227 samples as 11 episodes.
- **Rejected:** none.
