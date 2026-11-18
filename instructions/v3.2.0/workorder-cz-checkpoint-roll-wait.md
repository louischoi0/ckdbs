# Work order CZ - a checkpoint that does not hold its reactor on a segment roll: the premise measured before anything is built

Status: **OPENED 2026-11-18; CZ-S0, CZ-S1 and CZ-S2 done; S1 read as C1
alone; CZ-S2's items CZ-Q14 to CZ-Q22 marked (a) (§43 there), CZ-S3 not
opened and not started, the session handed off** on
worktree `cz-checkpoint-roll-wait` from `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`), on the operator's word of 2026-11-18
(`raft-marks-2026-11-18.md` §31): *"Proceed with the CZ milestone."* S0
rested on no item of §4. **On the operator's word of the same day (§41
there) V0 is passed, every item of §4 is marked (a) and CZ-S1 is opened.**
CZ-S0 was pushed to `main` with the hook skipped on the operator's word
(§32 there). **In CZ-S1's spike C1 alone met CZ-Q1 (a)'s bar as written,
on a host builds reached, and the operator read S1 as C1 alone and opened
CZ-S2** (§42 there). **CZ-S2's model is §8.** **C2 was priced further
(CZ-S1b, §7) and is now DG's subject**, outside CZ. CZ-S1, CZ-S1b, CZ-S2
and the marks were pushed to `main` with the hook skipped on the
operator's word (§44 there). Written the same day on
worktree `bl-rung3-probe` from `3b3f4851` (`v3.4.12-25-g3b3f4851`) on the
operator's word (§13 there): *"For now, leave it as a work order."* Every
`[source-read]` claim of §1 and §2 was read at `3b3f4851`; §7's stage log
names the commit of each later reading.

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
  - **CZ-S0's reading (2026-11-18, at `a4d2fb3b`): no live bug, and the
    "unless" above does not apply.** The dirty table's gap is covered by
    the fold and by the publish. The active table's is covered only by
    `Start` being one poll, which a park between the snapshot and the
    append removes. So under C1b the snapshot is taken again after a park.
    Trying the latch before the snapshot and appending under that hold is
    the alternative, unread: `Append` takes the latch itself. The
    assertion bases are a third snapshot in `Start`, held under the
    assertion directory's latch across their appends, and CZ-S2 owes that
    site its own design (the results file's §6.2).
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

**Every item is marked (a)**: CZ-Q9 by `raft-marks-2026-11-18.md` §18,
the others by §41 there, which also passes V0 on CZ-Q1 (a)'s bar. (a) was
CLA's proposal in each. CZ-Q11 to CZ-Q13 were added at CZ-S0. **CZ-Q14 to
CZ-Q22 were added at CZ-S2 and are marked (a), each CLA's proposal, by
§43 there**; each is argued in §8, and CZ-S3 rests on them. That word
marks the items and asks for a handoff: it does not open CZ-S3.

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
| CZ-Q11 | Added at CZ-S0. What the bar is read on beside p99: a run has 4,006 reader samples, its p99 is the 41st slowest, and S0's `levels` took 0-38 stall samples a run, so the stalls show at p99.9 (71.1 ms against 5.2 ms) and barely at p99 (3.2 ms against 2.3 ms). | the bar stays p99 (CZ-Q1); p99.9 and the count of samples at or above 10 ms are reported beside it at V0 and V1, with no bar | (b) a second clause on p99.9; (c) a clause on the stall count |
| CZ-Q12 | Added at CZ-S0. The checkpoint's page writes on a read core: a read core's run writes the instance's dirty pages on its reactor, and one page write stalled readers for 164 ms in S0's `half` cell, with no roll beside it. C1 does not touch it. | out of CZ: it is `known-gaps.md`'s entry on the checkpoint's `pwrite`s on the reactor; S1 and the close report a run's longest page write beside the stalls | (b) into CZ as a candidate |
| CZ-Q13 | Added at CZ-S0. M4 did not vary the roll rate: the inserter is closed-loop, so half the batch left 11 rolls a window against 10-12. | dropped: S1's spike answers what M4 stood in for | (b) M4 again with an inserter held to a rate, which is a driver change with its own review |
| CZ-Q14 | Added at CZ-S2, `[quiet-wrong]`. The log park's flush when the latch is busy or another thread's flush is in flight (§8.3). | the flush is skipped and the writer asked for `flushed_lsn()` and nothing above it; the task takes the durability park with the wait of CZ-Q15 beside it and tries the flush again when that ends, to the bound of CZ-Q4 | (b) no second try: the park ends only by a roll's publication or core 0's drain, whose period is the setting `relaxed_flush_interval_us`; (c) the spike's form, a retry with no time between tries |
| CZ-Q15 | Added at CZ-S2. The wait between two tries when no roll holds the latch (§8.3). | a park of 1 ms, by a deadline in the park's predicate, read at the reactor's next turn; every busy answer at a site counts against one bound of 16 | (b) a yield, which on an idle reactor is a spin of microseconds; (c) the blocking call at once |
| CZ-Q16 | Added at CZ-S2, `[quiet-wrong]`. The assertion bases' appends, core 0's alone (§8.4). | they stay blocking, inside the directory latch's one hold, and the visit's whole time is counted | (b) the begin record and the whole run under one tried hold of the stream latch, a new held-append entry |
| CZ-Q17 | Added at CZ-S2. The park on a roll (§8.3). | a flag the roll sets and a scope guard clears, read at the reactor's next turn, with a ceiling of 1 s on one park and no kick | (b) a kick to every core at a roll's end, the registry plumbed to the stream |
| CZ-Q18 | Added at CZ-S2. The wait event the park is booked under (§8.7). | a new event, `sysio.wal_roll`, in `wait-events.md`'s registry | (b) `sysio.wal_write`, the log park's event, which names another wait |
| CZ-Q19 | Added at CZ-S2. A tried append that takes the latch and must itself roll (§8.2). | it rolls on the task's reactor, as today, and the roll line's `core` counts it | (b) the tried append answers busy when it would roll and leaves the roll to the next appender, the blocking call past the bound |
| CZ-Q20 | Added at CZ-S2. The owed test "a crash inside the park, in `scripts/sim.sh`'s corpus" (§8.6). | replaced by rig tests; the simulator is untouched | (b) the simulator is given a shared stream and the cadence task, a change larger than C1 |
| CZ-Q21 | Added at CZ-S2; (b) is `[quiet-wrong]` as CZ-Q14 is. The park's flush when the latch is free: this thread writes the staged ring of every core (§8.3). | as today; CZ changes how the task waits for the latch, not what it writes | (b) a peer's checkpoint never flushes and always leaves it to core 0's drain |
| CZ-Q22 | Added at CZ-S2. A recycle skipped on a busy latch, run after run (§8.4). CZ-Q10 (a) marked "left for the next run"; nothing bounds how many runs. | after 16 skips in a row the detach blocks, counted; a skip is counted on the run line | (b) no bound: V1 reports the longest row of skips beside the live segments |

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

**Tests CZ-S3 owes**: §8.8, which replaced the list that stood here.

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

### CZ-S0 - the premise and the census, 2026-11-18

On worktree `cz-checkpoint-roll-wait` at `a4d2fb3b`
(`v3.4.12-42-ga4d2fb3b`). No engine code; the driver unmodified.
**Results, with the census as its §6:
`bench/v3.4.12/results-cz-s0-premise-v3.4.12-42-ga4d2fb3b.md`.** Suite
not executed: no build input changed. Overhead not measured; measured at
the milestone's close.

**What ran.** The rung-3 probe's cell, 5 interleaved runs of five arms,
25 cells, every one rc=0 with 0 errors: `isolated`, `levels`, `nockpt`
(`checkpoint_interval_ms = 0`), `half` (`--batch 512`) and `iso0`
(`isolated` with the cadence off, a control this order did not name).
Building and running the cells took 14.2 minutes. The quiet gate held the
matrix for another 25.8 minutes while two other sessions built and ran
suites, so the measurement spanned 40.5 minutes of wall time, **over the
20-minute budget on the clock**; whether such a hold counts is the
operator's to say. Nothing was cut.

**V0, on CLA's proposed bar (CZ-Q1 (a), unmarked): passed.** The verdict
is the operator's.

| reading | value |
|---|---|
| reader schedule p99, `nockpt` against `isolated` | 2.3 ms against 1.4 ms (medians of 5), 1.60x; 5 of 5 pairs within 2x |
| reader schedule p99, `levels` | 3.2 ms, 2.27x, 2 of 5 pairs above 2x; without the one cell a build ran into, 2.8 ms, 1.99x, 1 of 4 |
| reader schedule p99.9, `levels` / `nockpt` / `isolated` | 71.1 ms / 5.2 ms / 4.5 ms (medians) |
| reader samples at or above 10 ms, 5 runs, `levels` / `nockpt` | 75 samples / 1 sample |
| read cores' stream-latch wait a window, `levels` / `nockpt` | 19-702 ms / 0 µs in every run |

**What S0 found.**
- **The premise's direction holds.** The 10 ms stalls go with the cadence
  checkpoint, and with it the read cores' whole wait on the stream latch,
  while the log still rolls 12 times a window.
- **Its size at p99 was small this day**: about 2x, not the probe's 19x,
  on a host with a 2.2x higher floor and a third of the probe's stall
  samples. At p99 the gate barely separates the arms; the p99.9 and the
  latch counters do (CZ-Q11).
- **Where the wait is** (M2): by timing, 5 of 6 stall episodes in `levels`
  are the begin record's append. By latch time, at least 810 ms of the
  read cores' 1,280 ms was taken at a site the run line does not time.
- **What C1 would leave**: a page write stalled readers for 164 ms once
  in `half`, no roll beside it (CZ-Q12). The cadence-off arm also loses
  the long rolls and recycles nothing (M3).
- **M4 answered nothing** (CZ-Q13).

**The census.**
- **Takers on a core with no write**: the cadence checkpoint at §1.2's
  five kinds of site, and nothing else that this cell reaches. Not every
  caller of `Append` was read; the measured zero stands in for that.
- **Facts §1.2 did not have**: `WalStream::Flush` takes the latch with an
  empty ring, spins on `yield` while another thread's write is in flight
  (`src/wal/stream.cpp:493`, `:512-514`) and writes the staged ring on the
  calling reactor; a peer's recycle runs through core 0's manager on the
  peer's thread; a read core's run snapshots and writes the instance's
  dirty pages.
- **CZ-R3**: no live defect; the retake is required; `Start` has a third
  snapshot, the assertion bases (CZ-R3's new bullet). The comment in
  `Start` on pages dirtied "after this point" is wrong for the interval
  between the snapshot and the append at `cores > 1`. CZ-S3 corrects it;
  if the order closes before S3, the close does, in a comment-only change.
- **BQ's rung-2 ceiling** bounds the drain, not the checkpoint, and does
  not conflict with §1.1's.

**What CZ-S1's spike must count**: the results file's §7.

**Reviewed by `critics-developer`** against the raw data and the source,
before the commit. It recomputed every figure from the 25 cells, opened
every citation and tried four schedules against CZ-R3's gap; the stage's
conclusions and "no live defect" stood.
- **Corrected by the review:**
  - live segments were one too high in every row (a roll line's `seg` is
    the segment entered);
  - "at least 833 ms" subtracted the begin append alone, and is 810 ms
    with the inline gate;
  - one of two "page write" episodes was a publish-phase call;
  - the pool figures, three citations, two medians taken as upper medians
    and five wordings the data did not bear.
- **Applied from its findings:**
  - `levels` r5 is flagged as contaminated beyond its end, and `levels`
    is given with and without it;
  - V0's pairs are stated as not all adjacent, with the pairs against
    `iso0`;
  - the reactors' two physical cores are stated;
  - the budget is stated as over on the clock;
  - CZ-R3's cover is two things, not three, with the anchor's clamp and
    the no-yield premise named; the assertion bases and the unread
    alternative were added;
  - the script loads each cell once and names its thresholds.
- **Rejected in part:** the cut of this entry to V0's rows and a pointer.
  The findings that amend §1.2 and CZ-R3 stay here, in short, because this
  order is their home.

**For the operator.**
1. V0's verdict (CZ-Q1). CLA's reading: passed on the proposed bar; the
   premise's direction is confirmed and its size at p99 was small this
   day. A second M1 on a quiet host would firm the size and costs about
   10 minutes.
2. The marks on CZ-Q0 to CZ-Q8 and CZ-Q10, and on the three added items,
   CZ-Q11 to CZ-Q13. CZ-S1 needs CZ-Q3 and CZ-Q10 at the least.
3. The word for CZ-S1.

### CZ-S1 - the ceiling, 2026-11-18

On worktree `cz-checkpoint-roll-wait`. The spike is on branch
`worktree-cz-s1-spike` at `20d89234` (`v3.4.12-66-g20d89234`) and is not
merged; its diff is in the results file's archive. **Results:
`bench/v3.4.12/results-cz-s1-ceiling-v3.4.12-66-g20d89234.md`.** Suite not
executed: the spike is not landed and the work branch changes no build
input. Overhead not measured; measured at the milestone's close.

**What ran.** CZ-S0's cell, 5 interleaved runs of five arms on one binary,
25 cells, every one rc=0 with 0 errors: `isolated`, `levels`, `c1` (the
cadence task's latch sites try and park), `c2` (no zero prewrite where
`posix_fallocate` reserved) and `c1c2`. Building, a smoke cell and the
cells took 10.0 minutes; the quiet gate and the driver's wait for load
held the matrix another 22.8 minutes, outside the budget
(`raft-marks-2026-11-18.md` §41). Nothing was cut. The driver ran with the
spike's `--stall-cores`, which CZ-S0's §7 asked for and `bench/README.md`
counts as a driver change. Another session's build reached 6 of the 25
cells, two of them `isolated`; none of `levels` or `c1`.

**The ceiling, against CZ-Q1 (a)'s bar.**

| reading | `isolated` | `levels` | `c1` | `c2` | `c1c2` |
|---|---|---|---|---|---|
| reader schedule p99, median of 5 | 1.5 ms | 5.1 ms | 1.4 ms | 3.2 ms | 3.4 ms |
| against `isolated`'s median | 1.00x | 3.42x | 0.95x | 2.11x | 2.29x |
| pairs within 2x | 5 of 5 | 2 of 5 | 5 of 5 | 2 of 5 | 2 of 5 |
| p99.9, median of 5 | 4.5 ms | 138.8 ms | 4.4 ms | 5.9 ms | 6.2 ms |
| samples at or above 10 ms, 5 runs | 0 samples | 174 samples | 0 samples | 46 samples | 9 samples |
| holds over 10 ms inside a read core's checkpoint run, 5 runs | 0 holds | 14 holds | 0 holds | 7 holds | 1 hold |
| roll hold, median | no roll | 250.9 ms | 251.4 ms | 5.7 ms | 5.7 ms |
| inserter, median, against `levels` | - | 158,412 rows/s | +2.0 % | +53.4 % | +38.6 % |

**What S1 found.**
- **C1 alone met the bar as it is written, in the spike, on a host builds
  reached.** Two of its five pairs rest on an isolated cell that ran under
  a build. The three pairs with no build on either side are 0.95x, 1.26x
  and 1.07x; against the three isolated cells no build reached `c1` is
  1.21x with 3 of 5 runs within 2x.
- **The wait's sites** (`levels`, the read cores, by the spike's
  counters): the log park's flush 1,240.7 ms, the begin record 556.1 ms,
  the end record 7.8 ms, the recycle and the inline gate none. The flush
  is the site CZ-S0 could not time.
- **C2 alone does not meet the bar.** It shortens the common roll and
  keeps a tail, and because the inserter is closed-loop the C2 arms carry
  39-54 % more log: a heavier cell, not `levels` with one change. C1 with
  C2 was not better than C1 at p99; builds reached 3 of its 5 cells, so
  the size of that is not established.
- **What C1 leaves on a read core**: the inline gate (41 calls in 13
  logged runs, the longest 3.8 ms), page writes (the longest 6.2 ms), and
  holds of about 100 ms that are not the latch, each seen once in the 25
  cells and none inside a `c1` window - a flush call of 208.4 ms with
  2.1 ms of latch wait, a hold of 116.8 ms in a run that logged no line,
  and a publish-phase call of 96.6 ms.
- **Facts §1.2 did not have**:
  - a flush call can hold a read core with almost no latch wait: the spin
    on another thread's flush in flight, or this thread's own write of the
    staged ring;
  - a run in flight at the server's stop is abandoned and writes no run
    line, so run-line totals are lower bounds;
  - the anchor advances the durable redo start before it calls the
    recycler (`src/server/superblock_checkpoint_anchor.cpp:197-205`), so a
    skipped recycle is retried only when a later commit advances it again;
  - a foreground statement can spin on a page latch for a roll's hold
    (one poll of 117.5 ms on a read core in `c1c2`, inferred). No
    checkpoint site is in that, and C1 does not touch it.

**What CZ-S2 owes that the spike skipped.**
- A busy latch that no roll holds. The spike yields and retries with no
  bound - 7,391 times in `c1`'s 5 windows, and 49,805 times in one run of
  `c1c2`, about 61 ms of spin. A yield on an idle reactor returns at once.
  Another thread's flush in flight is such a holder too.
- What a skipped flush rests on when the holder is not a roll.
- The recycle skipped on a busy latch, against the retry condition above
  and the live-segment bound.
- The park's wake: the spike asks for none and is read at the reactor's
  next turn.
- The task's holds across the new parks (CZ-R9).
- The assertion bases' site, which the cell does not exercise, and the
  blocking call past 16 parks, which no run reached.
- The fail-stop and a crash inside a park.

**Reviewed by `critics-developer`** against the raw data and the spike's
source, before the commit. It recomputed every quoted figure from the 25
cells, read the spike site by site against the results file's §7, and
looked for a spike defect that would explain `c1`'s result; it found none.
- **Corrected by the review:**
  - `c2`'s stall samples overlap a run, a roll and neither in 21, 20 and
    25, not 22, 21 and 24; 11 of `levels`' 13 episodes name their run's
    core, not 12;
  - the C2 arms carry 39-54 % more log (39-53 % was the row rate); the
    holds were 22.8 minutes;
  - only the driver's wait gave up before the two isolated cells a build
    reached, not both gates; `start` is the whole phase, not the append.
- **Applied from its findings:**
  - the read cores' zero latch wait under C1 is by construction (a tried
    latch charges nothing); the evidence is now the scheduler's holds and
    polls, added to the tables;
  - "the stalls go, all of them" is "the stream-latch stalls go", with
    the non-latch holds listed;
  - `c2` r5's 24 samples are read as an unlogged checkpoint run's hold,
    not as the build's, and `c1c2` r5's 8 as a foreground statement's;
  - the pairs with no build on either side are given, and the bar is
    stated as met as written, on a host builds reached;
  - `--stall-cores` can only add stall samples where it fires, so
    `levels`' count is not set beside CZ-S0's;
  - the recycle's retry condition, the unlogged runs, the spike's
    `tl_busy` defect (no effect on these runs) and the unbalanced arm
    order are stated; the thesis was cut and the lists given one home.
- **Rejected in part:** importing CZ-S0's loaders into `cz_s1.py`. The
  copy stays, and the script says so, so that the archive stands alone;
  its three unused names were removed.

**For the operator.**
1. S1's reading: C0, C1 or C1 with C2. CLA's: C1 alone, as CZ-Q3 (a) has
   it.
2. The word for CZ-S2 (the model on paper, no code).
3. C2 outside CZ: the roll hold's median fell 44-fold and the bulk writer
   gained 53 % at `relaxed` on ext4, with the cost it moves to a commit's
   `fsync` not measured. Whether that is taken up, and under which order,
   is the operator's; CZ does not build it (CZ-Q3 (a)).

### CZ-S2 - the model, on paper, 2026-11-18

On worktree `cz-checkpoint-roll-wait`, from the operator's word
(`raft-marks-2026-11-18.md` §42), which also reads S1 as C1 alone. No
code. **The model is §8; CZ-Q14 to CZ-Q22 are its items for the operator
and CZ-S3 is not opened.** The source was read at `fd96b5d3`'s tree, which
is `origin/main`'s at `57d7c67f` for every file cited. Suite not executed:
no build input changed.

**Reviewed by `critics-developer`** against the source, before the commit.
It opened every citation and tried the model for a page ahead of its
record, a durable point over unwritten bytes, a begin record off its
instant, a hang, a deadlock and a wrong assertion base. **The four safety
claims held; liveness and completeness did not as first written.**
- **Corrected by the review:** six citations (the fold, the drain's two
  branches, the gate's admission, the once-a-round rule, the crash's
  cover, the simulator's entries).
- **Applied from its findings:**
  - the roll flag carried the park's liveness while the text called it a
    hint: it is now cleared by a scope guard and one park has a ceiling
    (CZ-Q17);
  - the first form of the flush site waited on core 0's drain alone, whose
    period is a setting with no ceiling: the flush is now tried again
    behind a timed park and the drain is the backstop (CZ-Q14);
  - the skipped flush must leave core 0's loss-window clock alone, or
    `relaxed`'s window doubles with no error (§8.3);
  - a tried append on a full ring was an uncovered blocking flush (§8.2);
  - a timed park is existing machinery, so CZ-Q15 (c) was stated
    unfairly; it is now the proposal, and the two bounds are one;
  - the recycle had no bound on skips in a row (CZ-Q22), its plumbing and
    `BackupControl`'s latch are named, the bases' count times the whole
    visit, a manager with no writer keeps its blocking arm, and the tests
    gained six;
  - §5's test list and this entry were cut to pointers.
- **Rejected:** none.
- **Left open by it, and stated in §8:** how often a park's first try at
  the flush is busy, which only V1 can tell.

**For the operator.** CZ-Q14 to CZ-Q22, and then the word for CZ-S3.
**Since marked**: CZ-Q14 to CZ-Q22 are each (a) on the operator's word of
the same day (`raft-marks-2026-11-18.md` §43). The word for CZ-S3 is
still owed.

### CZ-S1b - C2 priced further, 2026-11-18

On worktree `cz-checkpoint-roll-wait`, from the operator's word
(`raft-marks-2026-11-18.md` §42), under CZ-Q3 (a)'s "C2 is priced in S1".
CZ-S1's spike binary, unchanged; the driver gained `--insert-rate` on the
spike branch (`e9ee523f`), which that word admits against CZ-Q13 (a).
**Results:
`bench/v3.4.12/results-cz-s1b-c2-priced-v3.4.12-66-g20d89234.md`.** Suite
not executed: no build input changed.

**What ran.** 55 cells in two passes, every one rc=0 with 0 errors, 17.7
minutes of cells inside the budget and 40.9 minutes held outside it. A
build was on the host at 12 of them. The second pass was added when the
first showed the rolls short at its rate.

| measurement | reading | with the prewrite | without it (C2) |
|---|---|---|---|
| A, C1 in both arms, the inserter held to 120 statements a second | reader p99 / walker statements a window | 2.7 ms / 10,269 statements | 1.8 ms / 11,735 statements |
| A, the same at 150 a second | reader p99 / walker statements a window | 1.7 ms / 5,532 statements | 1.3 ms / 9,029 statements |
| B, a one-row commit at `group` | writer p50 from its schedule / service p95 | 799 µs / 985 µs | 809 µs (+1.3 %) / 1.6 ms (+65 %) |
| B, the same at `strict` | writer p50 from its schedule / service p95 | 792 µs / 945 µs | 813 µs (+2.6 %) / 1.6 ms (+71 %) |
| B, the bulk writer at `group` | rows a second / statement p50 / p99 | 121,469 rows/s / 5.6 ms / 66.8 ms | 146,073 rows/s (+20.3 %) / 6.7 ms / 11.1 ms |

**What the table cannot carry.**
- **The prewrite's cost is a disturbance of about half a second a
  segment, from the half-segment mark**; a roll's hold is what is left of
  it when the roll arrives (251 ms at 304 ms from the mark, 4 ms at
  577 ms). The roll waits at the sync gate, not for the body.
- **Neither C1 nor C1 with C2 meets CZ-Q1 (a)'s bar as written in these
  cells**, but C1 with C2 at 120 a second. They are not V1's cell.
- **For §8.3 and CZ-Q21**: the stall samples C1 alone kept here (27, in
  two runs) are the checkpoint's own flush call with no latch wait, 51 ms
  and 104 ms, inside that disturbance. That is the task writing the
  staged ring on a read core's reactor.

**Reviewed by `critics-developer`** against the raw data, before the
commit. It recomputed every table from the 55 cells and tested each
inference against the logs.
- **Corrected by the review:** two roundings and one column of the
  script (a per-core tally printed as the instance's).
- **Applied from its findings, each of which changed a reading:**
  - "a commit pays about 2 %" was the median's. The p95 separates the
    arms in every run, +65 % and +71 %, and is now the file's first
    finding;
  - "a cliff in the log's rate, the log outrunning the fill" was the
    wrong mechanism: the body was ready at every roll at the held rates,
    and the hold is the sync gate's. The file now gives the disturbance's
    place in a segment, computed in the script;
  - "every session gains" contradicted the walker's -16 % at `group`;
  - "C1's margin is thin" was two misses of the bar as written;
  - the held inserter falls behind in the C1 arms, `group` did not group,
    the commit cells never ran the preparer, the third long stall, and
    `c1h`'s stalls read from their run lines are stated.
- **Rejected:** rewriting the two run scripts as one. They are the record
  of what ran. The script's dead helpers stay for the same reason the
  copy does.

**CLA's judgement, which §42 left to it**: the numbers support an order
on the prewrite, and not C2 as it stands. It is
`workorder-dg-wal-segment-body.md`, written and not opened; the letter DG
is CLA's proposal.

## 8. The model for C1 (CZ-S2) `[design]`

Every `[source-read]` claim below was read at `fd96b5d3`. Nothing here is
built; CZ-S3 builds what the operator marks, and CZ-Q14 to CZ-Q22 are
each marked (a) (`raft-marks-2026-11-18.md` §43), so the model as written
below is the one to build. CZ-S1's numbers are of the
spike, which this model differs from at the flush and at the wait (§8.3):
V1 measures the model.

### 8.1 The change in one paragraph

Only the cadence task (`Checkpointer::RunTask`) changes, and the entries
it calls. At its begin record, its end record, its log park's flush and
its recycle it asks for the stream latch with a try. If the latch is held
it does not wait on its reactor thread: it parks and tries again, and at
the recycle it leaves the work to a later run. The synchronous checkpoint
(mount, shutdown, `RunToCompletion`), every commit, the drain,
`EnsureDurable` and the roll itself are untouched (CZ-R1, CZ-R5), and so
is a one-core instance, whose stream has no latch (CZ-R6).

### 8.2 The entries

- **Three tried entries on `WalStream`**, each beside the entry it
  mirrors: a tried append, a tried flush and a tried detach. Each takes
  the latch with `Latch::try_lock` (`include/kds/base/latch.hpp:77`) and
  answers "busy" through an out-parameter when it is held. No status code
  is added: busy is not an error and reaches no client.
  - The tried flush and the tried detach also answer busy when another
    thread's flush is in flight, where the blocking ones spin
    (`src/wal/stream.cpp:493`, `:342`).
  - A stopped stream answers its stop, as today, not busy.
- **`WalManager` forwards them** as explicit entries. The spike's
  thread-local switch is not kept.
  - **A tried append on a full ring.** Today's append answers a full ring
    with a blocking flush on the appender's thread, up to a bound
    (`src/wal/manager.cpp:389-397`). The tried append answers it with the
    tried flush: done, it appends again; busy, it answers busy with
    nothing appended. A begin record of 120 KB in a ring of 1 MiB
    (`include/kds/wal/stream.hpp:146`) meets a full ring far more often
    than it meets a roll.
  - **A manager with no writer** (a test's, a tool's) keeps today's arm at
    the flush site, the blocking `Sync` (`manager.cpp:252-257`): there is
    no writer to ask.
- **A roll flag.** The stream sets an atomic flag under the latch for the
  length of a roll (`src/wal/stream.cpp:420-437`), **cleared by a scope
  guard**, so that an exit that does not stop the stream - the seal's
  encode (`:371-374`), the header's (`:174-176`) - clears it too. It says
  why the latch is busy. With the park's ceiling (§8.3) nothing's
  correctness or liveness rests on it.
- **A tried append can still roll** (CZ-Q19). When it takes the latch and
  its record does not fit, the roll runs on the task's reactor, as today.
  A begin record of 10,000 dirty pages is 120 KB (12 bytes an entry,
  `include/kds/wal/payload.hpp:491`) of a 64 MiB segment, one such roll in
  about 550 runs; the largest record `LogBegin` cuts to is 279 KB, one in
  240. An end record is a few bytes.

### 8.3 The wait

One rule at the three sites that wait (the begin record, the end record,
the flush): **a busy answer parks the task, the task tries again when the
park ends, and after 16 busy answers at a site in one run it calls the
blocking entry** (CZ-Q4 (a), CZ-R4). What the park waits for depends on
the holder.

- **A roll**, a quarter of a second at the median. The park ends when the
  roll flag clears, when the log stops, or after 1 s (CZ-Q17).
  - No wake is asked for. A parked task's predicate is asked at most once
    a reactor iteration (`src/sched/scheduler.cpp:246-249`,
    `include/kds/sched/coro.hpp:175-182`), and a reactor with nothing
    ready blocks at most `max_idle_block_ms`, 10 ms
    (`include/kds/sched/scheduler.hpp:78`). A roll's thread has no path to
    the wake registry today: it is the writer's alone
    (`include/kds/wal/manager.hpp:339-342`).
  - Rolls that follow one another without the flag being seen clear are
    one park. So the bound is reached by a roll only through the ceiling:
    16 s of a latch that never came free.
- **Any other holder**: another core's append, a flush's swap, a flush in
  flight, a detach. In CZ-S1's `levels` over 99.9 % of the read cores'
  latch wait was a roll's. The park ends after 1 ms or when the log stops
  (CZ-Q15).
  - The deadline is in the park's predicate, as a row wait's is
    (`src/server/command_dispatcher.cpp:470-472`); no timer is armed, so
    its resolution is the reactor's turn, at most one idle block.
  - A yield in its place is a spin: CZ-S1 saw sixteen of them pass in tens
    of microseconds on an idle reactor, and 49,805 in one run.
- **At the flush the park is also the durability park** (CZ-Q14,
  `[quiet-wrong]`). When the tried flush answers busy the task asks the
  writer for `flushed_lsn()`, parks until its LSN is durable or the wait
  above ends, and, not yet durable, tries the flush again.
  - **Never above `flushed_lsn()`.** The writer publishes as durable
    exactly what it was asked for (`src/wal/writer.cpp:105-109`, "the
    snapshot rule"), on the promise that those bytes were written before
    the ask. `flushed_lsn_` is stored only behind a write that returned
    (`src/wal/stream.cpp:506`, `:538`), and today's ask reads it unlatched
    too (`src/wal/manager.cpp:267`). A task that asked for its own LSN
    with its flush skipped would have the writer publish a durable point
    over bytes still in the ring, and a page could then reach the data
    file ahead of its record (CZ-R2).
  - **The skip leaves core 0's loss-window clock alone.** `RequestSyncNow`
    stamps `last_sync_ns_` behind its flush (`manager.cpp:268`), and core
    0's drain flushes when 10 ms have passed since that stamp. A skip on
    core 0's own task that stamped it would put off the drain's flush
    behind a flush that did not happen, and `relaxed`'s loss window would
    reach twice its setting with no error.
  - **Under a roll** the task's record is below the append point the roll
    found. The roll's seal flushes the ring and `StartSegment` publishes
    the stream's durable point after its sync
    (`src/wal/stream.cpp:163-165`), and `durable_lsn()` is the larger of
    the stream's and the writer's (`include/kds/wal/manager.hpp:262-266`).
    The park ends durable, with no second try.
  - **The backstop** is core 0's drain. It runs every iteration and on a
    1 ms timer, cannot take its free-tick exit while any core has bytes
    unflushed (`src/wal/manager.cpp:495-509`), and flushes the whole ring
    once `relaxed_flush_interval` has passed since its last ask
    (`:539-552`), at once with a group commit pending (`:510-526`), in
    every durability class. The model does not rest on it: that interval
    is a file key with no ceiling, and core 0 can be inside a long poll.
  - **Not measured**: how often a park's first try is busy. CZ-S1 counted
    busy answers, not parks that began with one. Each costs the task one
    reactor turn more than today; V1 reports a run's wall time (CZ-Q2 (a)).
- **When the latch is free the flush is today's** (CZ-Q21): this thread
  writes the staged ring, every core's bytes. CZ-S1's 208 ms flush call
  with 2 ms of latch wait may have been that write or the spin on a flush
  in flight; the model removes the second and leaves the first. CZ-S1b saw
  the first twice under C1, 51 ms and 104 ms with no latch wait, and they
  were C1's only stall samples there (§7, "CZ-S1b").

### 8.4 The sites

- **The begin record (C1b)**, `[quiet-wrong]` by CZ-R3. The task tests the
  roll flag first and parks on it, so that no snapshot is taken to be
  thrown away. It then calls `Start` in its tried form: both snapshots,
  then the tried append of the record, in one poll. Busy, nothing was
  appended and no checkpoint is in progress; the task parks (§8.3) and
  calls it again.
  - **CZ-R3 holds by construction.** No suspension lies between the
    snapshots and the append in any attempt, so what covers the active
    table today - that `Start` is one poll - covers it in every attempt,
    and the window for another core's append is shorter than today's,
    there being no latch wait in it. The retake is the second call.
  - A busy answer is not a refusal: it does not count in the census's
    `start_refused`.
- **The assertion bases** (CZ-Q16, `[quiet-wrong]`). Only core 0's
  checkpoint writes them (`src/server/core_runtime.cpp:356-368`). They
  are appended behind the begin record in the same poll, under one hold of
  the directory latch (`src/exec/assertion_check.cpp:284-304`), and every
  write that reserves against an assertion takes that latch and then the
  stream's.
  - **They stay blocking.** A run is a base only when it is whole, and an
    assertion's pending list is accepted only directly behind its base
    (`src/exec/assertion_recover.cpp:262-283`, `:313`). A task that gave
    up between a base and its pending list would leave a base whose
    undecided reservations are never compensated: counts too high at the
    next mount, with no error. A task that released the directory latch
    part-way and went on would log stale bases.
  - **The exposure that leaves**: a roll another core starts between core
    0's begin record and the last of its bases, or a reserving writer that
    holds the directory latch while it waits on a roll. Core 0's reactor
    then waits, as today. The count is the whole visit's time, not the
    stream latch's wait.
  - Giving up before the first base is safe - analysis merges every
    `CHECKPOINT_BEGIN` in range (`src/wal/analysis.cpp:156-176`), and a
    begin record with no end is already written today when `Start` fails
    behind `LogBegin` (`src/wal/checkpointer.cpp:210-212`) - but it buys
    only the first append of the run.
- **The log park's flush (C1a).** §8.3.
- **The end record (C1b).** The tried append and the park of §8.3. No
  snapshot is behind it. A crash while the task is parked leaves a begin
  record with no end, which a crash leaves today.
- **The recycle (C1c).** The anchor's commit calls the recycler only when
  the durable redo start advanced, having advanced it first
  (`src/server/superblock_checkpoint_anchor.cpp:195-205`). The task's
  commit asks for the tried detach; busy - the latch held, or a flush in
  flight - it is skipped and counted, and is no recycle failure.
  - **The plumbing.** "Tried" passes from the anchor's `Commit` through
    `Landed` and the recycler's function to `BackupControl::Recycle` and
    `RecycleBelow`. The synchronous `Publish` shares `Landed` and stays
    blocking.
  - **The next attempt** is the next publish, by any core, that advances
    the durable redo start, and a detach is cumulative, so it covers what
    was skipped. That start is the fold, the lowest of the cores' redo
    starts (`superblock_checkpoint_anchor.cpp:38-64`), so it advances when
    the core that holds the lowest publishes again: within one rotation of
    the cores' runs, an idle instance's included, since a run with no
    logged page in its snapshot takes its own begin record as its redo
    start (`src/wal/checkpointer.cpp:214-229`).
  - **Skips in a row are bounded at 16** (CZ-Q22), then the detach blocks.
    Without that nothing bounds them: no count of live segments refuses
    anything, a roll's hold covers 25-34 % of a window in this cell, and
    CZ-R4's reason - a log that is not recycled - would hold only in
    probability. What a skip costs until the next run is disk space and
    the sync's walk of the live segments (about 50 µs a segment, BQ-S1).
    CZ-S1 skipped 1 of 13 commits in `c1`.
  - **Left**: the recycler takes `BackupControl`'s latch with a blocking
    lock (`src/server/backup.cpp:476-479`), and `BACKUP START` holds that
    latch across a blocking append (`:96-114`). A roll at that moment
    stops the task's reactor whatever the detach does.
- **The inline WAL gate** is untouched (CZ-Q10 (a), CZ-R5).

### 8.5 What the task holds across a new suspension (CZ-R9)

| suspension | where | what the task holds |
|---|---|---|
| the park before a begin record | before `Start` returns OK | the checkpoint gate's flag (`include/kds/wal/checkpointer.hpp:306-345`) and its run report; no page, pin, latch or claim - none is taken before `Start` succeeds |
| the park before an end record | behind the last step's yield (`src/wal/checkpointer.cpp:517`) | what that yield holds today: the gate's flag and the report |
| the park at the flush | where the durability park is today | what that park holds today |
| the recycle | none | it is skipped, not waited for |

A run that parks holds the gate longer, and the gate has no timeout: other
cores' ticks skip, and the gate then admits the asking core whose last
entry is oldest (`checkpointer.hpp:350-365`). CZ-S1's longest run was
1,676 ms under C1 against 1,678 ms.

### 8.6 The stop, the abandon and a crash

- **The log's stop** ends every park (each predicate reads it), and the
  tried entry called next answers the stop; the task ends with it, as an
  append's refusal ends it today (CZ-R8).
- **An abandoned task** (shutdown) is tested behind every new suspension,
  as behind every existing one. The log park gains a second try, so it
  takes the task's generation to test it there.
- **A crash** inside a park at the begin record leaves nothing; at the end
  record, a begin record with no end. Analysis over several begin records
  with no end is covered (`tests/wal_analysis_test.cpp:337`), and the
  simulator crashes behind an abandoned run (`sim/loop.cpp:1522-1528`).
- **The simulator reaches no site of C1** (CZ-Q20): one core, an unshared
  stream, and `RunToCompletion` or `Start`, one `Step` and `Abandon` by
  hand, never the cadence task (`sim/instance.cpp:41`, `:197`,
  `:223-234`).

### 8.7 What is counted

The run line keeps, a site: busy answers, parks on a roll, the bound
reached, the time parked, and the time inside a blocking call past the
bound. It keeps the assertion visit's whole time and the recycles
skipped. The spike's other call times and its thread-local latch counter
go. The park is a suspension and so needs a wait event (CZ-Q18).

### 8.8 The tests CZ-S3 owes

The rig is `tests/checkpoint_log_park_test.cpp`'s: a shared stream, core 1
attached, reactors driven by hand on a manual clock, a log device whose
sync can be held - and a held sync under a roll is a held roll
(`tests/wal_roll_instruments_test.cpp:179`).

1. A roll held while core 1's task reaches its begin record: the task
   parks and no begin record is in the log. A transaction begun and a page
   dirtied during the hold are in the record appended after the release
   (CZ-R3).
2. A roll held while the park's flush is asked: the flush is skipped,
   `durable_lsn()` never passes `flushed_lsn()` at any step (CZ-R2), and
   the park ends at the roll's publication.
3. Another thread's flush in flight (a device that gates its write, as
   `tests/wal_append_off_latch_test.cpp`'s does) while the park's flush is
   asked: skipped, tried again behind the timed park, and done once the
   write is released.
4. The same with the write never released until core 0's drain is called:
   the park ends by the drain, and core 0's own skipped flush leaves
   `last_sync_ns_` where it was.
5. A roll held at the end record: no end record until the release, and the
   anchor is published after it.
6. The bound: a roll held while the clock is moved a ceiling at a time;
   the sixteenth busy answer is the last and the blocking entry is called,
   released by a helper thread.
7. The roll flag left set by a seam: the park ends at its ceiling.
8. The log stopped while the task is parked at its begin record, at its
   end record and at the flush: the task ends with the stop and publishes
   no anchor.
9. The task abandoned behind each new suspension: it does nothing more.
10. A tried append on a full ring, with the tried flush busy and then
    free.
11. The recycle skipped on a held latch and taken by the next run that
    advances the fold; sixteen skips, then the blocking detach
    (`tests/two_core_rig.hpp`'s `fold_anchor` and `recycle`, with a
    cadence task).
12. Core 0's run with an assertion source and a roll held behind its begin
    record: the run is whole and its pending lists follow their bases.
13. One core: the tried entries never answer busy, and a run's records are
    those of the blocking entries.

Dropped from the list that stood in §5: two cores' checkpoints parked on
one roll (the gate admits one run for the instance), and the simulator's
crash (CZ-Q20).

### 8.9 The texts CZ-S3 owes

- `docs/spec/wal.md` §6, step 1: "every core's append, flush and drain
  waits the roll out" gains the cadence checkpoint's exception and its
  rule for the skipped flush; §11 the tried sites.
- `docs/spec/sched.md` §4: the checkpoint task's new park.
- `docs/spec/wait-events.md`: the event `sysio.wal_roll` (CZ-Q18 (a),
  marked).
- `include/kds/wal/stream.hpp`'s protocol header: the tried entries and
  the roll flag.
- The drain's comments (`src/wal/manager.cpp:495-509`,
  `include/kds/wal/manager.hpp:70`): the free-tick rule gains a second
  dependant, the checkpoint's skipped flush.
- `Start`'s comment on pages dirtied "after this point" (CZ-S0).
- At the close: `CLAUDE.md`'s WAL row, `known-gaps.md`'s roll entry and
  BL-R7's remainder (CZ-Q6 (a)).
