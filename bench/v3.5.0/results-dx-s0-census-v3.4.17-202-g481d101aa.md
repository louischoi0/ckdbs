# DX-S0 census: an abort is entered with no pin, but its row locator fetches a sibling while holding the leaf; and a mostly dirty pool refuses 2.3-2.5 % of point updates, most of them at the window's open with the leaf held

**Thesis.** Measured on worktree `dx-pool-under-pressure` at `481d101aa`
(`v3.4.17-202-g481d101aa`), with no behaviour changed. Every `path:line`
here was read at `481d101aa`.

- **Every abort is entered with no pin.** Over the whole suite, 20,522
  entries into `TransactionManager::Abort`, 20,711 into
  `AssertionEnforcer::AbortTxn` and 143 into recovery's
  `RecoveryUndo::RollBackOne` all read `PinsHeldOnThisThread() == 0`
  (§1.1).
- **But a compensation can reserve with a pin held** `[source-read]`
  (§1.2).
  - The rollback's row locator descends for write. The descent keeps the
    leaf while `LeafStillCoversKey` fetches the right sibling
    (`src/storage/btree/btree.cpp:68-70`, `:109`).
  - A miss there in a full pool reserves with one pin held. Drain mode is
    skipped (`device_page_store.cpp:941`) and the reservation is refused.
  - The locator then turns the refusal into a scan fallback and answers
    NotFound, so the compensation fails and its write is left un-undone.
    That is `known-gaps.md:112`'s hazard, on a real path.
  - No measured run reached it. The probe counted only a drain-mode
    reservation whose walk was exhausted with a pin held: 0 such.
  - **So DX-S1 has a pinned reservation to serve**, by DX-Q2 (a)'s drain
    that needs no page of the caller's.
- **The dirty-pool refusal reproduces**, and at a higher rate than DM-S1
  recorded (§2).
  - 1,683-5,515 refusals in each 6 s lifetime, or 2.26-2.49 % of
    statements. DM-S1's base arm recorded 143-220, or 0.37-0.55 %.
  - **77-91 % of them come from `BeginNoRefuseWindow`**
    (`src/storage/device_page_store.cpp:1051`, "cannot hold another
    mutation's N frames"); the rest come from `ReserveFrame` (`:961`). DX
    §2.2 names only `ReserveFrame`.
  - **Both sites refuse with the row's leaf held exclusive.** The point
    `UPDATE` opens its window inside the row visit, with the leaf in hand
    (`command_dispatcher.cpp:14636`, called at `:14870`). A descent's
    sibling fetch also holds the leaf.
  - So DX-R3's wait, which holds no latch and no pin, cannot be taken
    where the refusal is raised. It is taken after the statement has let
    go of everything (§4).
- **DX-Q1's bar, CLA's reading: met.** The refusals reproduce, so DX-S2
  builds the wait.
- **The writeback queue is filled by the CLOCK walk alone and drained on
  core 0's 50 ms tick** (§3). A reservation outside drain mode never drains
  it, and at `cores = 1` the tick does not run while a statement holds the
  reactor. The refusals fall between ticks. That is the cause the order
  recorded as likely (`known-gaps.md:40`), now read in the code.

| field | value |
|---|---|
| date executed | 2026-11-27: the baseline suite about 11:58-12:00 UTC; the probe suite 12:06-12:09 UTC; the dirty-pool cell 12:10:50-12:11:15 UTC |
| branch / commit | `worktree-dx-pool-under-pressure` at `481d101aa`, `git describe --tags` = `v3.4.17-202-g481d101aa` |
| work order | `instructions/v3.2.0/workorder-dx-pool-under-pressure.md` §5, DX-S0 |
| probe build | Debug (`./build`), `481d101aa` plus `archive/.../probe.py.as-run` (stderr lines only), reverted after the run |
| cell build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, from `git archive 481d101aa` |
| binary | a copy at `build-dx-base/run/bin/kds_server`, sha256 `17cda4082ac1714e5ba146bf72b212f50593de24aead4a4a87b61fb736cce0c7` |
| device | `/dev/mapper/fedora-root`, xfs; the data file opened without `O_DIRECT` |
| cell | `tools/dm_s1_check.py`, unmodified: 1,000,000 rows (the cold base `archive/.../load_cold.py.as-run` loads, by `dm_s0_premise.make`, as DM-S1's `cold()` does), `buffer_pool_frames = 2048`, `cores = 1`, 4 writer sessions, random-key `UPDATE ... WHERE id = k`, 6 s a lifetime, 4 lifetimes from one loaded copy. `DM_MEMO_ENTRIES` is read by nothing at this commit (DM's memo never landed), so `--arms 0,0,0,0` is four lifetimes of the one engine |
| port | 15781 |
| host | load 2.00-2.19 (1 min) before each lifetime; no build or suite process (`pgrep` empty) |
| time | the Debug build and its probe rebuild about 6 min, the baseline suite 96 s, the probe suite 182 s, the Release build 5 min, the load 5 s, the cell 25 s; about 17 min in all, inside the 20 min stage budget |

## 1. Who enters an abort holding what

### 1.1 Measured: the probe over the whole suite

The probe wrote one stderr line at each entry into
`TransactionManager::Abort` (after its repeat-abort return),
`AssertionEnforcer::AbortTxn` and `RecoveryUndo::RollBackOne`, giving
`PinsHeldOnThisThread()`. It also wrote one line where `ReserveFrame`'s
walk had been exhausted in drain mode with pins held, the branch where the
drain is skipped. That line counts only full-pool reservations, so its 0
says no full-pool, drain-mode reservation was made with a pin held in the
suite. It does not say none can be (§1.2). The suite ran with every label,
`bs-nightly`'s allocation sweep included (`ctest -j4`, 5,369 tests run).

| site | entries | pins ≠ 0 |
|---|---|---|
| `TransactionManager::Abort` | 20,522 entries | 0 entries |
| `AssertionEnforcer::AbortTxn` | 20,711 entries | 0 entries |
| `RecoveryUndo::RollBackOne` (mount undo) | 143 entries | 0 entries |
| `ReserveFrame`, walk exhausted in drain mode with a pin held | 0 entries | 0 entries |

`archive/.../probe-tally.txt` holds the counts. A server a test starts as
a child process writes its stderr elsewhere, and those lines are not
counted. Every in-process rig and dispatcher test is counted.

The suite results with the probe: 5,366 of 5,369 passed. The three
failures each passed 3 of 3 times when run alone on the same build. They
ran beside a Release build and a data load, and their messages are a rig
that failed to open (`StreamingReplyTest.AClientThatStopsReadingParksTheWalkAndAHangupEndsIt`,
`ce_streaming_reply_test.cpp:75`, then a segfault in the test body) and
lock-wait refusals (`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`,
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`). The clean
baseline at the same commit (`ctest -j8 -LE bs-nightly`) passed 5,341 of
5,342 in 96 s. Its one failure was `DropTableReclaimCrashTest.AReclaimCutAnywhereIsFinishedByTheNextMount`
on `Disk quota exceeded` while copying a WAL into `/tmp` (tmpfs), and it
passed 3 of 3 times alone.

### 1.2 Read: the call sites and what each compensation holds

`[source-read]` at `481d101aa`:

- **Four callers of `TransactionManager::Abort`**, all in
  `src/server/command_dispatcher.cpp`:
  - `FinishDeferredCommits` (`:1565`), between statements;
  - `DdlBuildLock::End` (`:5602`), the DDL build's own transaction at its
    scope's end;
  - `RollbackLocal` (`:15528`), for `ROLLBACK`, the ceiling and a close;
  - `AbortOwnedScope` (`:16510`), an autocommit statement's failure or its
    failed commit, from `EndWrite` (`:16463`, `:16479`) and from
    `~IndexBuildRun` (`:3543`).

  Each runs at a statement's or a transaction's boundary. None runs inside
  a walk. `RollbackLocal` and `AbortOwnedScope` call
  `AssertionEnforcer::AbortTxn` first. So does `EndWrite`'s arm for a
  dispatcher with no transaction manager (`:16391`, `kBootstrapXid`),
  also at a statement's boundary. `AbortTxn` fetches one page an iteration
  (`src/exec/assertion_check.cpp:942`), holding one at a time.
- **A compensation's fetches, and the one that holds a pin.**
  - `Compensate` (`src/txn/manager.cpp:412`) gets the trail's page. When
    the row moved, it releases that page before the locator runs
    (`:487-489`).
  - **The locator's descent holds the leaf while it fetches the right
    sibling.** `RowLocatorForRollback` (`command_dispatcher.cpp:10353`)
    calls `LocateByTupleKey`, `kWrite` by default
    (`command_dispatcher.hpp:3007`). That runs `exec::LocateRow`, then
    `BtreeLookup(kWrite)`, then `DescendTo(leaf_for_write = true)`.
    - The descent has no latch coupling between internal nodes
      (`src/storage/btree/btree.cpp:162-168`). But it keeps the leaf while
      `LeafStillCoversKey` (`:248`) runs `store.GetForRead(right)` (`:109`):
      "Peak pins: 1 here, and 2 for the span of `LeafStillCoversKey`"
      (`:68-70`).
    - A miss there, in a full pool, reserves with one pin held. Drain mode
      is skipped (`device_page_store.cpp:941`), and the walk's retries end
      in `ResourceExhausted`.
    - `LocateByTupleKey` answers that refusal with a scan fallback
      (`:10398-10411`). The locator answers the fallback NotFound, "could
      not be relocated for rollback" (`:10370-10377`). `Compensate` fails
      and `Abort` keeps unwinding, so that write stays un-undone.
    - The path needs a rollback of a row a leaf division moved, a full
      pool and a right sibling that is not resident. Unverified whether any
      test drives it: by grep, only `tests/supplied_key_test.cpp` names the
      locator.
  - The row compensations (`src/txn/row_compensation.cpp`) write the held
    page and the log only.
  - `ReleaseVarHeapSlot` (`src/txn/varheap_release.cpp:15`) holds its one
    page.
- **So the drain's own condition is the gap.** Drain mode writes back only
  at `t_pins_here == 0` (`device_page_store.cpp:941`), and the locator's
  sibling fetch reaches it with one pin held. Nothing in the tree enforces
  "no pin at an abort's entry" either. The probe says only that the suite
  never broke it.

## 2. The dirty-pool cell

| lifetime | updates (rows affected = 1) | rows affected ≠ 1 | refused at the window's open (`:1051`) | refused at `ReserveFrame` (`:961`) | refused, client | `pool_refused` | pool misses | inline reclaim batches | frames reclaimed / inline batch | dirty frames queued | dirty frames drained | background batches |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| L1 | 226,158 updates | 0 updates | 5,012 refusals | 503 refusals | 5,515 refusals (2.38 %) | 6,521 refusals | 378,559 misses | 246,883 batches | 1.42 frames | 204,012 frames | 203,358 frames | 556 batches |
| L2 | 146,929 updates | 0 updates | 3,189 refusals | 351 refusals | 3,540 refusals (2.35 %) | 4,242 refusals | 245,139 misses | 159,714 batches | 1.40 frames | 132,250 frames | 131,494 frames | 391 batches |
| L3 | 72,754 updates | 0 updates | 1,294 refusals | 389 refusals | 1,683 refusals (2.26 %) | 2,461 refusals | 121,120 misses | 88,458 batches | 1.21 frames | 64,817 frames | 64,817 frames | 234 batches |
| L4 | 111,737 updates | 0 updates | 2,321 refusals | 531 refusals | 2,852 refusals (2.49 %) | 3,914 refusals | 187,096 misses | 142,333 batches | 1.19 frames | 100,284 frames | 100,284 frames | 309 batches |

- **The rate is steady and the volume is not.** The refused share is
  2.26-2.49 % in every lifetime. Throughput moved 3.1 times between
  lifetimes on a quiet host, from 72,754 to 226,158 updates in 6 s. That
  variance is not explained here. The refused share is what DX-S2's cell
  reads.
- **The window's open is 77-91 % of the client's refusals** (5,012 of
  5,515; 3,189 of 3,540; 1,294 of 1,683; 2,321 of 2,852), and
  `ReserveFrame` is the other 9-23 %.
- **Against DM-S1's base arm** (`results-dm-s1-ceiling-v3.5.0-89-gc003e7fe.md`
  §3: the `c003e7fe` binary, `v3.5.0-89-gc003e7fe`, with
  `DM_MEMO_ENTRIES = 0`, on another host). That arm refused 143-220 times
  against 39,020-40,177 updates in 6 s, 0.37-0.55 % of statements computed
  as this file computes its own.
  - This tree runs 1.81-5.80 times the updates.
  - Its share is 4.1-6.8 times DM-S1's.
  - The two runs differ in host, commit and throughput, so the difference
    is recorded and not attributed.
- **`pool_refused` exceeds the client's count by 702-1,062 per lifetime**
  (1,006, 702, 778, 1,062). Unverified which refusals do not reach the
  client. There are two candidates:
  - a statement re-run after a structural refusal;
  - §1.2's fallback, where a descent refused for the pool becomes a scan
    that is refused again, so one statement counts twice. The server log's
    "falling back to a scan" warnings would count it, but the cell runs at
    `log_level = warn` and the log was not kept.

## 3. Where writeback is queued from, and where it is drained

`[source-read]` at `481d101aa`, `src/storage/device_page_store.cpp`:

- **Queued** only by `SweepLocked` (`:3386`, the queue at `:3429-3435`). The CLOCK walk queues a
  frame that has no pin, is not of a resident class, is held by no other
  core, has usage 0 and is dirty. `SweepLocked` runs from:
  - `InlineBatchLocked`, the batch a full pool's `ReserveFrame` (`:932`)
    and `BeginNoRefuseWindow` (`:1045`) walk under the structure latch;
  - `MaintainFreeReserve` (`:2360`), core 0's background batches;
  - `EvictColdFrames` (`:3374`), which no source outside the store calls
    (a test's door).
- **Drained** (`DrainDirtyEvictionQueue`, `:2347`, `HeldFrames::kSkip`)
  only:
  - **on core 0's writeback tick**, every 50 ms
    (`src/server/expeditor.cpp:2326`, `:2396-2398`). The tick runs inside
    `MaintainFreeReserve`'s loop and once after it;
  - **in drain mode with no pin held** (`ReserveFrame`, `:941-943`). Drain
    mode is opened by `TransactionManager::Abort`,
    `AssertionEnforcer::AbortTxn`, the mount's recovery
    (`expeditor.cpp:1051-1057`), `PURGE INDEX` (`command_dispatcher.cpp:4026`),
    the heap slot release and index purge steps (`:9820`, `:9853`) and
    `PURGE`'s two phases (`:17966`, `:18112`).

  The checkpointer writes back through `FlushPages`
  (`src/wal/checkpointer.cpp:298`), and `PersistPage` (`device_page_store.cpp:2569`) writes
  one page. Neither drains the queue.
- **So a statement's reservation outside drain mode never writes back.**
  - Its walk queues the dirty frames it passes, then retries
    `kRefuseRetries` = 8 times (`device_page_store.hpp:737`) without
    yielding, and refuses.
  - The queue it filled is written back at the next 50 ms tick.
  - **At `cores = 1`, as in this cell, the tick does not run while a
    statement holds the reactor** (`expeditor.cpp:2324-2325`). The retries'
    premise, "a drain on another core may have cleaned frames meanwhile"
    (`device_page_store.cpp:945`), has no other core to rest on, so the 8
    retries likely cannot succeed by construction. Likely; unverified.
  - DX §2.2's "a refused reservation hands a batch of dirty frames to the
    writeback path before it parks" is aimed at this gap.
- The cell's 10,800-34,000 frames queued a second count the background
  sweep's queueing as well as the writers'.

## 4. What this decides

- **DX-Q1 (a)'s bar, CLA's reading: met.** The refusals reproduce, so
  DX-S2 builds the wait, for both refusal sites, `BeginNoRefuseWindow`
  included.
- **The wait is taken where nothing is held, not where the refusal is
  raised.** Both sites refuse with the row's leaf held exclusive, and DX-R3
  forbids a wait holding a latch or a pin. So the statement lets go of
  everything first, by returning its refusal, as BA-S14's structural
  re-run does.
  - Then, under the statement's one deadline and only if it wrote nothing
    (DX-R1), the wait drains the dirty queue holding nothing and runs the
    statement again.
  - A statement that wrote rows first keeps today's refusal (the order's §2.3).
  - Because the wait is not at the refusal site, what each site holds when
    it refuses does not constrain it, and S0 does not measure it.
- **DX-S1 serves a pinned reservation.** The rollback locator's sibling
  fetch (§1.2) reserves with the leaf held, so DX-Q2 (a)'s drain must
  write back with a pin held, skipping every frame pinned when it is
  claimed. DX-S1 also builds:
  - the always-on check at `TransactionManager::Abort`,
    `AssertionEnforcer::AbortTxn` and `RecoveryUndo::RollBackOne`;
  - DX-R2's cell, which faults a rollback into a pool of dirty frames;
  - a store-level cell for the drain with a pin held.

  The locator itself is not reordered: once the drain needs no page of the
  caller's, releasing the leaf before the coverage check would buy only a
  second descent.
