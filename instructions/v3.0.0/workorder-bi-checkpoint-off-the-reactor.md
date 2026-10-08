# Work order BI — the checkpoint stops holding its reactor: one step per task, its syncs parked

Written 2026-10-08 on `worktree-checkpoint-reactor-workorder` from
`65a28553` (`v2.7.0-703-g65a28553`). It follows the operator's words of
2026-10-08, verbatim (`raft-marks-2026-10-08.md` §2-§5):

- **W0:** *"작업 지시서 하나를 새로 작성해 push하라. 주제: checkpoint가
  코어의 reactor를 동기적으로 막는 문제를 없앤다."*
- **W1:** *"(1) Step 하나를 reactor task 하나로 실행해 step 사이에
  yield한다."*
- **W2:** *"(2) data file sync와 EnsureDurable은 writer/I/O thread로 넘기고,
  호출한 task는 park한다(RequestSyncNow와 같은 모양)."*
- **W3:** *"S1은 측정만 한다. checkpoint 구간의 reactor 정지 시간과 같은
  코어 다른 세션의 p99를 build-release로 잰다. 코드 변경은 계측뿐이다."*
- **W4:** *"세션 배치(BA-R13, CN-10 §3.4)와 core 0 commit drain의 inline
  fdatasync는 범위에서 뺀다."*
- **W5:** *"기존 작업 지시서와 같은 구조를 따르고, 모든 주장에
  [source-read]/[design]/[measured] 태그를 붙인다."* and *"Q 항목은 전부
  미마킹 상태로 둔다."*

**Written, not opened.** W1 and W2 are the operator's decisions and stand
here as BI-R1 and BI-R2. Every BI-Q item is unmarked (W5), BI-Q0 included,
so no stage is startable.

**Claim tags as always:** `[source-read]` with `path:line` at `65a28553`,
`[design]` for CLA's reasoning about code not yet written, `[measured]` with
the invocation. `[quiet-wrong]` marks an item where a wrong choice turns a
refusal or a slow answer into a wrong one.

**The letter.** `[source-read]` BH is taken: `worktree-bh-purge-key` holds
the PURGE order (`b3b5ad5c` BH-S0 .. `04649133` BH-S5), not merged to
`main` at `65a28553`. BI is the next free letter.

**Why this order exists.** `[measured]` On 2026-10-08 a client's 4-session
insert load stalled a core for up to 5.7 s at a time, and 21 of the 22
stalls of 2 s or more ended at that core's own checkpoint anchor
(`docs/inflight/bugs/a-checkpoint-run-holds-its-cores-reactor-for-its-whole-length.md`,
build-release of `19843fee`; scripts and raw data at
`/home/cdkbs/bench-runs/checkpoint-reactor-stall-19843fee/`:
`pingprobe.py`, `threadsample.py`, `stallstats.py`). `[source-read]` The
bug entry reads the cause, and §1 below re-reads it at `65a28553`: the
periodic checkpoint is one synchronous call on its core's reactor thread,
with no yield in it.

**BI against BA.** `[source-read]` `workorder-ba-parallelism.md` §1.12
names this stall as half of P12 (`:546-575`), and BA-R10's third paragraph
already proposes W1 in other words: *"The checkpoint runs through its own
`Step()`, `pages_per_step` at a time, one per reactor iteration, instead of
`RunToCompletion`"* (`:932-934`). It is owned by BA-S13 (`:1019`), behind
BA-Q1's census gate (`:1030`). BA is paused and BA-Q0..Q13 are unmarked.
`[design]` BI takes the checkpoint half of P12 and leaves BA-R10 the
trx-id carve; BI-Q1 asks for that.

## 0. What BI is

`[source-read]` A core's checkpoint run fires inside a timer callback and
runs `Start`, every `Step` and `Complete` before the callback returns. Each
step's log-gate wait, page writes and data-file `fsync` happen on the
reactor thread, and so does the anchor publish's whole-store writeback.
`[design]` BI turns the run into a `system`-group task. The task takes one
`Step` and yields before the next (W1). Each wait for the log or the data
file becomes a request to another thread, with the task parked until the
request completes (W2).

**BI's rules, one line each:**

- **BI-R1** (W1, the operator's). The cadence callback only submits a run
  task. The task takes one `Step` and yields before the next.
- **BI-R2** (W2, the operator's). The log gate's `EnsureDurable` and the
  data-file sync become requests that return at once. The task parks on a
  predicate until each completes, as a commit parks today.
- **BI-R3.** A run parks only between writebacks. Across a `co_await` it
  holds no writeback claim, pin or page latch, and no superblock or
  registry latch.
- **BI-R4** `[quiet-wrong]`. A run's gate hold ends with its task, and a
  shutdown checkpoint never skips.
- **BI-R5.** What W1 and W2 do not move is named, counted by S1 and decided
  by an item, never left implicit: the page writes themselves, the per-run
  gate inside `WriteBack`, the claim wait's sleeps, and the publish's
  whole-store writeback.

**BI does not do:**

- **Session placement** (W4). That covers BA-R13 and BA-S16, CN-10 §3.4
  (decision U6), and `docs/pending/session-load-balancing.md`.
- **Core 0's commit drain's inline `fdatasync`** (W4).
  `[source-read]` That is `WalManager::DrainOnce`'s `Sync()` on an owning
  manager with a parked group commit (`src/wal/manager.cpp:478`), which is
  BA-R4 part 1 and BA-S7. `[design]` BI-R2 moves core 0's *checkpoint* log
  waits, which today reach the same inline `Sync()` (`:347` → `:255`). It
  does so by calling `RequestSyncNow` from the checkpoint task. The drain's
  call is not touched.
- **The trx-id carve's whole-pool sync** (BA-R10's first half).
- **The client `SYNC`'s store sync** (`src/server/command_dispatcher.cpp:1261`).
- **The page writes' own latency.** W2 names the syncs. `pwrite` stays on
  the reactor, bounded per poll by one step (§1.6).
- **The `CHECKPOINT_BEGIN` size ceiling** (about 87,000 dirty pages,
  refused at the first append), which the bug entry records.
- **One checkpoint cadence for the instance.** That is option (e) of
  `docs/pending/v20-insert-slowdown.md` §4.
- **An SLO-feedback controller** (`docs/spec/wal.md` §11-2 "SLO-throttled",
  `docs/spec/sched.md` §4 "Shares are static").
- **Anything on disk.**

## 1. Survey at `65a28553`

This section was read, not run, except where it quotes the bug entry's
measurement.

### 1.1 Where a run runs: a timer callback, charged to no group

- `[source-read]` **The cadence.** Each peer arms
  `SubmitEvery(period, [this] { (void)Checkpoint(); })` after a phase of
  `core_id/cores` of a period (`src/server/core_runtime.cpp:526-541`). Core
  0 arms the same at phase 0 (`src/server/expeditor.cpp:1911`).
- `[source-read]` **The callback runs inline in phase 2.**
  `Scheduler::ExpireTimers` calls `due.fn()` directly
  (`src/sched/scheduler.cpp:157`). `RunOnce` calls `ExpireTimers` at
  `:410`, before `RunReadyTasks` at `:416` and the post-task hook at
  `:429`. The header asks for the opposite: *"a timer callback must be as
  short as a phase-1 io handler: the thing it should do is Submit() the
  work"* (`include/kds/sched/scheduler.hpp:146-149`). So does the spec:
  *"Timer expiry enqueues the waiting task into its scheduling group"*
  (`docs/spec/sched.md:109`).
- `[source-read]` **So the checkpoint is not a `system`-group task.** The
  spec says otherwise in two places: *"run as a `system`-group task per
  core"* (`docs/spec/wal.md` §11) and `include/kds/wal/checkpointer.hpp:17-18`.
  Its time therefore lands in what `sched.md` §4 calls *"charged to no
  group"*. `sched_system_polled_us` does not see it, and only
  `sched_wall_us − Σ polled − sched_idle_block_us` bounds it.
  `[source-read]` The one `kSystem` producer in `src/` at this commit is
  `src/server/tcp_server.cpp:386`.
- `[source-read]` **The chain.** `CoreRuntime::Checkpoint`
  (`core_runtime.cpp:571`) and `Expeditor::Checkpoint` (`expeditor.cpp:1234`)
  each call `Checkpointer::RunGated` (`src/wal/checkpointer.cpp:331`).
  `RunGated` takes the gate's RAII `Hold` (`:333`) and calls
  `RunToCompletion` (`:345`). `RunToCompletion` resumes an in-progress run
  or calls `Start` (`:314-318`), loops `Step` until done (`:319-327`), and
  then calls `Complete` (`:328`).
- `[source-read]` **The header promises what the loop breaks.**
  `checkpointer.hpp:32-36` says *"Step() flushes a bounded batch, so a
  checkpoint spreads across reactor iterations"*. `pages_per_step` = 64 is
  *"small enough that a checkpoint does not monopolize a reactor
  iteration"* (`:270-277`). `RunToCompletion` is *"for callers that do not
  need the checkpoint spread across reactor iterations (tests, and
  shutdown)"* (`:332-335`). Yet the cadence calls it.
- `[source-read]` **The spec principle it breaks.** `sched.md` §3:
  *"Blocking syscalls inside tasks are forbidden; all waiting is expressed
  as suspension"* (`:42`). `docs/spec/page.md` §15 item 2 rejects mmap
  because a blocked thread *"freezes every task on the core"*, which is
  what a run does.

### 1.2 What one step does

- `[source-read]` **The path.** `Step` (`checkpointer.cpp:202`) calls
  `target_.FlushPages(batch)` (`:214`). That is
  `PageStoreCheckpointTarget::FlushPages`
  (`include/kds/storage/page_store_checkpoint_target.hpp`), which calls
  `DevicePageStore::FlushPages` (`src/storage/device_page_store.cpp:2234`).
- `[source-read]` **`WriteBack` (`:1839`), in order:**
  - it picks the calling core's gate (`:1851`) and refuses after a log
    fail-stop (`:1857`);
  - `AwaitWalGate` (`:1861` → `:1770`) scans the batch's highest
    `page_lsn` under the structure latch, then calls
    `gate->EnsureDurable(highest)` outside it (`:1808`);
  - then, per run of up to `kWritebackRunPages` = 8
    (`include/kds/storage/device_page_store.hpp:425`), it:
    - claims the frames;
    - waits out a frame another writeback has claimed with
      `AwaitWritebackClaim` (`:1977`). That is 64 yields and then 50 µs
      `sleep_for`s on the reactor thread (`:1820-1836`);
    - copies each page under its shared page latch;
    - re-gates with `gate->EnsureDurable(run_lsn)` if a page's LSN passed
      the batch gate (`:2059-2068`);
    - writes with `device_.WritePageRun` / `WritePage` (`:2073-2076`);
    - cleans.
- `[source-read]` **Then the maps and the sync.** `FlushPages` writes the
  maps through `FlushMaps` (`:2252`; page writes at `:550`, `:561`, no
  sync). It then calls `device_.Sync()` (`:2275`) if anything was written.
  That is `FilePageDevice::Sync`, an `fsync` loop that retries only
  `EINTR` (`src/storage/file_page_device.cpp:241-249`).
- `[source-read]` **`EnsureDurable` has two shapes.**
  `WalManager::EnsureDurable` (`src/wal/manager.cpp:334`) calls `Sync`
  (`:184`):
  - **A peer (attached manager).** It flushes through the stream latch,
    then calls `writer_->EnsureDurable(target)` (`:224`). That waits on the
    writer's condition variable *on the reactor thread*
    (`src/wal/writer.cpp:33`, `done_.wait` at `:38`).
  - **Core 0 (owning manager).** It runs `stream_->Sync()` (`:255`), an
    inline `fdatasync` on core 0's reactor.

  The operator's citation, `manager.cpp:211`, lands in the comment that
  explains the wait (`:212-215`). The call is `:224`, and the wait is in
  `writer.cpp:38`.
- `[source-read]` **Core 0 has a writer.** `expeditor.cpp:820` starts it,
  so `RequestSyncNow` on core 0 hands the sync to it
  (`manager.cpp:285-306`). Only a writerless manager (tests, tools) syncs
  inline there (`:286-291`).

### 1.3 `Complete` and the publish

- `[source-read]` **`Complete`** (`checkpointer.cpp:238`) appends
  `CHECKPOINT_END` (`:258`). It makes the record durable through the same
  two `EnsureDurable` shapes (`:269`), then calls `anchor_.Publish` (`:278`).
- `[source-read]` **`SuperBlockCheckpointAnchor::Publish`**
  (`src/server/superblock_checkpoint_anchor.cpp:66`):
  - It folds and encodes into page 0 under the superblock latch (`:89`),
    fetching page 0 through `store_.Get`. `Get` is an exclusive, write-mode
    fetch (`include/kds/storage/page_store.hpp:204-209`).
  - Outside the latch it calls `store_.Sync()` (`:132`).
    `DevicePageStore::Sync` (`:2195`) is `Flush` (`:2169`): every frame
    dirty in the pool at that moment is collected and written back with
    its gates, then the maps. A `device_.Sync()` follows (`:2197`).
  - Only after that sync returns OK does `D` move and the recycler run
    (`:160`).
- `[source-read]` **This writeback is not paced.** It is one call over
  every page any core dirtied during the run, which is the bug entry's
  *"one unpaced writeback of whatever its own length let build up"*.
- `[source-read]` **The publish's sync is load-bearing for the steps.**
  `FlushPages` skips its own sync when another core's writeback carried
  its pages, on the ground that *"`SuperBlockCheckpointAnchor::Publish`
  syncs the store itself, after every step"* (`device_page_store.cpp:2256-2274`).

### 1.4 What BI builds on

- `[source-read]` **Coroutines.** `MakeCoroTask` is at
  `include/kds/sched/coro.hpp:405`, `Yield` at `:416`, and `WaitUntil`
  (a predicate re-tested once per poll) at `:447-458`. The `system` group
  is *"checkpoint/WAL housekeeping"* (`include/kds/sched/task.hpp:21`).
- `[source-read]` **One poll per task per iteration.** `sched.md` §4's two
  floors poll a ready task at most once per iteration and every ready group
  at least once. A run that yields after each `Step` therefore takes one
  step per iteration under any load.
- `[source-read]` **The park W2 names.** A commit stages its LSN, then
  `co_await sched::WaitUntil{&durable}` on `IsDurable(lsn) || stopped()`.
  It answers the error through `EnsureDurable` afterwards
  (`src/server/command_dispatcher.cpp:755-773`). `RequestSyncNow` flushes,
  calls `writer_->RequestSync`, and returns without waiting
  (`manager.cpp:285-306`).
- `[source-read]` **The writer syncs the log and nothing else.** It takes
  a device and a sync function and *"the only thing it knows how to do is
  make bytes that are already written durable"* (`include/kds/wal/writer.hpp:51-54`).
  Its sync runs at `writer.cpp:115`. Nothing in it touches the data file.
- `[source-read]` **The data-file sync has no gate.** `FilePageDevice::Sync`
  returns an `fsync` failure and is not sticky: a later call can report OK.
  Every core's reactor calls it, through checkpoint steps, the publish, the
  carve, `SYNC` and shutdown. The log's sync is the contrast: since
  `fsync-fail-stop` it is one gate, one sync at a time, and a failure stops
  it for good, because *"on Linux a failed `fsync` can drop the dirty pages
  and clear the error"* (`docs/spec/wal.md` §6 item 5).
- `[source-read]` **No completion kick.** A parked predicate is re-tested
  per poll, and an idle reactor blocks up to `max_idle_block_ms` = 10
  (`scheduler.hpp:83`). The writer wakes only threads blocked in its own
  `EnsureDurable` (`writer.cpp:38`, `done_`). It never kicks a reactor.
  BA-R4 part 1 proposes that kick for commits, and it is not built.

### 1.5 What the bug entry measured, and what it did not

- `[measured]` The load and its result:
  - **Config:** `cores = 4`, `durability = relaxed`,
    `checkpoint_interval_ms = 5000`, `buffer_pool_frames = 786432`.
  - **Load:** 4 sessions of 1024-row named-key `INSERT`s into one btree.
  - **Probe:** 8 KWP connections pinging every 200 ms.
  - **Result:** 22 stalls of 2 s or more, 21 ending within [-1.5 s, +1 s]
    of the stalled core's own anchor (12 when the ends were shifted 2.5 s).
    The longest was 5.7 s.
  - **Thread state** of the stalled core: 93 % off-CPU. `jbd2_log_wait_commit`
    26 %, `folio_wait_bit_common` 20 %, `rq_qos_wait` 16 %,
    `futex_do_wait` 30 %. It was never in `ep_poll`.

  Invocation: the archived `pingprobe.py`, `threadsample.py` and
  `stallstats.py` against xrock's server, 08:41:05-08:49:05 UTC.
- `[measured]` **What it does not give**, and S1 owes:
  - pages per run (`pages_flushed` is a Debug line, and the server logged
    at `info`);
  - the split of a run's time between the log gate, the page writes, the
    data-file `fsync`, the `CHECKPOINT_END` wait and the publish's
    writeback;
  - a *statement's* latency on the stalled core, since it measured pings;
  - a cell at a commit named by `git describe` under `bench/v3.0.0/`.

  The `wchan` split does not separate a buffered write's waits from an
  `fsync`'s.

### 1.6 What W1 and W2 leave, read against the tree

- `[design]` **The page writes.** They stay on the reactor. Under BI-R1 a
  poll writes at most one step's 64 pages in runs of up to 8, plus that
  step's map pages. Whether one step's writes stay under BA's 10 ms bar on
  a loaded device is S1's to measure, not this survey's to assume.
- `[design]` **The per-run gate** (`device_page_store.cpp:2059-2068`). It
  is a synchronous `EnsureDurable` inside `WriteBack`, reached when a page
  in the run was logged after the step's batch gate. A task cannot park in
  it: the frames are claimed and the run's bytes are copied at that point,
  and BI-R3 forbids a park while holding a claim. BI-Q4 decides it.
- `[design]` **The claim wait.** `AwaitWritebackClaim`'s sleeps last as
  long as another writeback's single run. A run is claimed, written and
  released before the next is looked at (the comment at `:1898-1902`), so no
  BI park lengthens it.
- `[design]` **The publish.** W1 alone makes it worse: a spread run lasts
  longer, so more is dirty when the publish's unpaced `Flush` runs. BI-Q5
  decides it.
- `[design]` **Run length and the gate.** A spread run holds the
  instance's gate longer, and every other core's tick skips while it does
  (`checkpointer.hpp:225-234`). Per-core anchors and `D` move later, so
  BC's recycling and BF's reclaim, which read `D`, lag. A longer replay
  range lengthens recovery. W1 accepts spreading, and §5 prices it.
- `[design]` **The stagger stops helping once runs approach the period.**
  `core_runtime.cpp:527-533` says why the ticks are staggered: *"whenever
  a run outlasts the start-up skew, the same core wins each time and the
  rest skip for good, holding the fold, and so the anchor, at their
  completion checkpoints"*. Take `cores = 4`, a 5 s period and 4.9 s runs.
  Core 0's run ends just before its own next tick, which then enters while
  cores 1-3 skip every time. `FoldedAnchor`'s minimum then stays at their
  last records (`superblock_checkpoint_anchor.cpp:38-64`), `D` stops, and
  the log is never recycled. That is not a wrong answer but a log that
  grows until restart. BI-Q8 decides it.
- `[design]` **Shutdown** `[quiet-wrong]`. Today a run cannot be in flight
  when a reactor stops, because it is synchronous. Under BI-R1, a stopped
  reactor's queue can still hold a parked run whose frame holds the gate's
  `Hold`. The shutdown checkpoints then run on the expeditor's thread after
  the join: peers' at `expeditor.cpp:2097` (`ShutdownCheckpoint`,
  `core_runtime.cpp:581-590`), core 0's at `:2163`. Each calls `RunGated`,
  which would find the gate held and skip.
  `docs/inflight/known-gaps.md:152-185` states what rests on *every*
  shutdown checkpoint publishing: the assertion scan's floor. A missing
  one can bring a peer up refusing an asserted relation's writes for the
  life of the mount.
- `[design]` **The mount's completion checkpoint** stays synchronous. It
  runs on the startup thread before any reactor exists, and says so
  (`src/server/mount_recovery.cpp:367-371`).
- `[design]` **Core 0's `relaxed` loss window.** The bug entry reads that
  window as unbounded while core 0 runs a checkpoint: the drain runs on
  core 0's reactor (`expeditor.cpp:2008`, `:2011`), and a peer's tick
  returns before the loss-window branch (`manager.cpp:495-497`). Under
  BI-R1 core 0's reactor turns between steps, so the post-task drain runs
  every iteration of a run. §5 checks that rather than assuming it.

## 2. Rulings

BI-R1 and BI-R2 are the operator's decisions (W1, W2). BI-R3..R5 are CLA's,
and BI-Q0 opens them.

### BI-R1 — One `Step` per task poll (W1)

- `[design]` **The cadence callback submits, and does nothing else.**
  `SubmitEvery`'s callback submits `MakeCoroTask(kSystem, run)` when no
  run task is in flight on this core, and returns. A per-runtime flag says
  whether one is; a tick during a run submits nothing. `SubmitEvery`
  coalesces missed periods already (`scheduler.cpp:135-154`), so the flag
  is the only new state.
- `[design]` **The run task:**
  1. Enter the gate, and skip as today on a miss.
  2. `Start`, in one poll. `CHECKPOINT_BEGIN` and the assertion snapshots
     stay one act under the registry's latch (`checkpointer.hpp:152-157`).
     They are not split.
  3. For each step: `Step`, then `co_await sched::Yield{}`.
  4. `Complete`, parked as BI-R2 says.
  5. `RunGated`'s log lines, with S1's per-run Info line.
- `[design]` **W1 is read as one coroutine per run that yields after each
  `Step`, not as a new `Task` object per `Step`** (BI-Q9). To the
  scheduler the two are the same, since a task is polled at most once per
  iteration (`sched.md` §4). They differ in who holds the gate and the
  checkpointer's cursor between steps: under this reading, the task does.
- `[design]` **`RunToCompletion` stays for the synchronous callers:** the
  mount's completion checkpoint, the shutdown checkpoints and tests. Its
  header comment becomes true again.
- `[design]` **`pages_per_step` stays 64** (BI-Q6). S1 reports a step's
  reactor hold before anyone re-sizes it.

### BI-R2 — The waits leave the reactor; the task parks (W2)

- `[design]` **The log gate.** `AwaitWalGate`'s scan is split from its
  wait. Before a step's writeback, the task:
  1. computes the batch's highest `page_lsn` (the scan, under the structure
     latch, as today);
  2. if that LSN is not durable, calls `RequestSyncNow` on its core's
     manager;
  3. `co_await sched::WaitUntil` on `IsDurable(lsn) || stopped()`.

  `WriteBack`'s own `AwaitWalGate` is then a no-op for the ordinary case.
  `Complete` does the same for `CHECKPOINT_END`'s LSN in place of
  `EnsureDurable` (`checkpointer.cpp:269`). A fail-stopped log answers the
  park through `stopped()`, and the step fails, as `WriteBack`'s refusal
  does today (`:1857`).
- `[design]` **On core 0 this moves the checkpoint's log syncs to the
  writer** (`expeditor.cpp:820`). Core 0's checkpoint stops reaching its
  inline `Sync()`. The drain's `Sync()` (`manager.cpp:478`) is W4's and
  unchanged.
- `[design]` **The data file.** A step's sync and the publish's sync become
  a request to the thread BI-Q2 names. The request returns a ticket, and
  the task parks on `WaitUntil` until the thread's completed ticket passes
  it or a failure is recorded. A failure fails the step or the publish,
  with the run left in progress, as a failed step is today
  (`checkpointer.cpp:214-226`). BI-Q3 decides whether that failure is
  sticky.
- `[design]` **"RequestSyncNow와 같은 모양"** is read as two things: a
  request that does no I/O on the caller's thread and returns, and a park
  on a predicate that the thread's progress satisfies. The commit's park
  (`command_dispatcher.cpp:755-773`) is the template, including the
  `stopped()` disjunct and the error answered after the wake.
- `[design]` **The wake.** Without a kick, an idle reactor re-tests a
  parked step only when its block ends, up to 10 ms (§1.4), and a run has
  two parks per step. BI-Q7 decides whether the threads kick.

### BI-R3 — Parks only between writebacks

`[design]` A run task holds none of the following across a `co_await`: a
writeback claim (`Frame::writing`), a pin, a page latch, the structure
latch, the superblock latch (`superblock_checkpoint_anchor.cpp:89`) or the
assertion registry's latch. That is `sched.md` §3's suspension safety,
applied. The suspend audit (`exec::InstallSuspendAudit`, `core_runtime.cpp:556`)
checks the pin half in debug. The rest is by construction: every park in
BI-R2 sits before a `WriteBack` call or after it returns.

### BI-R4 — A run's gate hold ends with its task; shutdown never skips `[quiet-wrong]`

`[design]` Before a core's shutdown checkpoint (`expeditor.cpp:2097`,
`:2163`):

1. Its in-flight run task, if any, is destroyed. That releases the `Hold`.
2. The shutdown checkpoint runs `RunToCompletion`, synchronous on the
   expeditor's thread. Its resume arm (`checkpointer.cpp:306-318`, BC-S4's)
   finishes the abandoned run, including the publish.

A shutdown checkpoint that finds the gate held by its own core's parked
task is the defect this rule exists for. BI-S2 pins it with a cell, and a
mutation that restores the skip must fail it.

### BI-R5 — The remainder is named

`[design]` Four things stay on the reactor or stay unpaced unless an item
moves them. S1 counts each, and the close reports each:

| remainder | where | decided by |
|---|---|---|
| a step's page writes | `device_page_store.cpp:2073-2076` | not moved (W2 names the syncs); S1's gate |
| the per-run gate | `:2059-2068` | BI-Q4 |
| the claim wait's sleeps | `:1820-1836` | not moved; S1 counts its time |
| the publish's whole-store writeback | `superblock_checkpoint_anchor.cpp:132` → `:2169` | BI-Q5 |

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BI-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The index row</li><li>`raft-marks-2026-10-08.md` §2-§5</li><li>The bug entry's *The fix* pointing here</li></ul> | S |
| BI-S1 | **The stall, measured** (W3) | <ul><li>**Code: instrumentation only, no behaviour change:**<ul><li>per run: wall time from the gate's entry to the publish's return, steps, pages written, the dirty-table size at `Start`, and a refused `Start` (the BEGIN ceiling) counted;</li><li>per phase, summed and maxed per run: `Start`, the log gate's wait (batch and per-run arms apart), the page writes, the map writes, the claim wait, the data-file `fsync`, the `CHECKPOINT_END` wait, the publish's writeback and the publish's `fsync`;</li><li>per reactor: the longest `RunOnce`, and a count of iterations over 10 ms, 100 ms and 1 s;</li><li>one Info line per run: core, wall start and end, and the totals, so a probe sample can be classed against its own core's runs;</li><li>`SHOW META` prints the per-reactor counters and the last run's per-phase split, for the session's core (`core=` already says which, `command_dispatcher.cpp:1321`).</li></ul>Clock reads per phase per step, never per page, on the scheduler's injected clock (`sched.md` invariant 8).</li><li>**Cells** for the counters: a run of N steps reports N, a scripted device's delay lands in its own phase, and the iteration counters move under a deliberately slow timer callback.</li><li>**The measurement** (§5.1), into `bench/v3.0.0/results-bi-s1-checkpoint-stall-<describe>.md`.</li><li>**The gate** (BI-Q10).</li><li>**The suite green.**</li></ul> | M |
| BI-S2 | **The run as a task** (BI-R1, BI-R3, BI-R4) | <ul><li>**Code:** the submit-only callback, the in-flight flag, the run coroutine and the shutdown abandon-and-resume. The waits are still inline, so S2 alone bounds a poll at one step, plus `Start`, plus `Complete`.</li><li>**Cells:**<ul><li>a run of N steps spans at least N reactor iterations;</li><li>a foreground task submitted during a run is polled between two of its steps;</li><li>the callback returns without running a step;</li><li>a tick during a run submits nothing;</li><li>a reactor stopped with a run parked mid-steps still publishes its shutdown checkpoint, peer and core 0 alike;</li><li>a crash mid-steps replays from the previous anchor (sim);</li><li>the assertion recovery cells green unchanged.</li></ul></li><li>**Mutations, each repeated:**<ul><li>`RunToCompletion` restored in the callback (killed by the interleave cell);</li><li>the in-flight flag removed (killed by the second-tick cell);</li><li>the shutdown's skip on a held gate restored (killed by the shutdown cell).</li></ul></li><li>**The suite green** and the sim corpus.</li></ul> | M |
| BI-S3 | **The log gate parked** (BI-R2, first half; BI-Q4) | <ul><li>**Code:** `AwaitWalGate`'s scan split from its wait; `RequestSyncNow` and the park for each step and for `CHECKPOINT_END`; core 0's checkpoint through its writer; the per-run gate as BI-Q4 marks; the wake as BI-Q7 marks.</li><li>**Cells:**<ul><li>on the two-core rig, a peer's step whose batch is past the durable point parks while another task on its reactor runs, and core 0's does the same through its writer;</li><li>WAL-before-data: a page whose record is not durable is never written (`EvictionWritebackTest.FlushBeforeEvictHoldsWithTwoCoresDirtyingOnePage` and a step-shaped cell);</li><li>a fail-stopped log answers a parked step, which writes nothing and leaves the run in progress.</li></ul></li><li>**Mutations:**<ul><li>the park removed and the gate skipped (killed by the WAL-before-data cell);</li><li>the `stopped()` disjunct removed (killed by the fail-stop cell's bound);</li><li>the batch scan taken outside the structure latch (a race cell, repeated with a barrier).</li></ul></li><li>**The suite green.**</li></ul> | M |
| BI-S4 | **The data-file sync off the reactor** (BI-R2, second half; BI-Q2, BI-Q3, BI-Q5) | <ul><li>**Code:** the thread BI-Q2 names, its request, ticket and completion; `FlushPages`' sync and the publish's sync through it; the publish as BI-Q5 marks; the failure rule as BI-Q3 marks. The synchronous callers (`SYNC`, the carve, shutdown, the mount) call the same gate synchronously.</li><li>**Cells:**<ul><li>a step parks while a scripted data-file sync is held, and the reactor polls another task meanwhile;</li><li>a failed data-file sync fails its step and publishes no anchor;</li><li>under BI-Q3 (a), no later sync reports OK;</li><li>under BI-Q5 (b), a crash after the publish finds every snapshot page on disk when another core's writeback carried some of them (the `[quiet-wrong]` cell);</li><li>the thread's lock is in `sched.md` §9's list.</li></ul></li><li>**Mutations:**<ul><li>the ticket compared before the sync returns (killed by the held-sync cell);</li><li>the failure cleared (killed by the sticky cell under (a)).</li></ul></li><li>**The suite green** and the sim corpus.</li></ul> | M |
| BI-S5 | **The close** | <ul><li>**The measurement** (§5.2).</li><li>**Text:**<ul><li>`wal.md` §11 steps 2-3 and §8-1 (core 0's checkpoint gate through the writer);</li><li>`sched.md` §4 (the checkpoint charged to `system`) and §9 invariant 2 (the data-file thread's lock);</li><li>`checkpointer.hpp:17-36`, `:269-277` and `:332-345`;</li><li>`device_page_store.cpp:2256-2274`'s cover argument, restated per BI-Q5;</li><li>`page.md` §13, which `wal.md` §11-2 cites for "checkpoint spreading" and which no longer carries it;</li><li>`CLAUDE.md`'s WAL row.</li></ul></li><li>**Docs:**<ul><li>the bug entry deleted if §5.2 meets BI-Q11's bar, or restated with the number;</li><li>`workorder-ba-parallelism.md`'s BA-R10 and BA-S13 amended per BI-Q1.</li></ul></li></ul> | S |

## 4. Items for the operator

All unmarked (W5).

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BI-Q0 | **Open BI**, with BI-S0..S5 and BI-R3..R5 as written; BI-R1 and BI-R2 are the operator's (W1, W2) | process | Yes | |
| BI-Q1 | **BI against BA.**<br>(a) BI owns P12's checkpoint half: BA-R10's third paragraph and BA-S13's mid-steps crash clause move here, and BA keeps the carve. BI-S1's gate replaces BA-Q1's census for this half.<br>(b) BI waits for BA's census | sequencing | (a). BA is paused, its census is unbuilt, and BI-S1 measures this stall directly against BA's own 10 ms bar (`workorder-ba-parallelism.md:681`) | |
| BI-Q2 | **The thread that takes the data-file sync.**<br>(a) A data-file I/O thread, one for the instance, created at startup.<br>(b) The WAL writer (`writer.hpp`) takes a second job.<br>(c) One thread per core | design | (a). A data-file `fsync` can run for seconds (the bug entry's `jbd2_log_wait_commit` share). On the writer, (b) would put every core's commit sync and the `relaxed` loss window behind it. (c) puts several concurrent `fsync`s on one descriptor, the case `wal.md` §6 item 5 names as one caller getting the error and another OK. (a) adds one thread at startup (`sched.md` invariant 1 allows it) and one mutex to §9's list | |
| BI-Q3 | **Data-file sync failure** `[quiet-wrong]`.<br>(a) One gate in the device: one sync at a time, and a failure sticky until restart. No anchor publishes after it, while the log keeps running.<br>(b) As today: the step fails, and the next tick retries the `fsync` | design | (a). §1.4: on Linux a retried `fsync` can report OK over pages a failed one dropped. Under (b), a run after a failure can publish an anchor past data that is not on disk, and recovery would skip the redo those pages need. The cost of (a): after a failure `D` stops, so recycling and BF's reclaim stop until a restart. The gap predates BI; BI's thread is where the gate goes | |
| BI-Q4 | **The per-run gate inside `WriteBack`** (`device_page_store.cpp:2059-2068`).<br>(a) `WriteBack` returns the run unwritten with the LSN it needs: claims released, frames still dirty. The task parks and re-runs the step.<br>(b) The inline wait stays as a counted remainder (BI-R5) | design, **[quiet-wrong] under (a)** | (b), with S1's per-run-arm counter as the gate: (a) is written only if S1 or BI-S3 counts it non-zero under load. (a) touches the order that keeps WAL-before-data: a run written before its LSN is durable is the violation `wal.md` §8-1 names | |
| BI-Q5 | **The anchor publish's whole-store writeback.**<br>(a) Paced. The publish collects once, the task writes the collection in `pages_per_step` batches with a yield between, then page 0, then the data-file sync, parked.<br>(b) Narrowed. Page 0 (through `WriteBack`'s claim) and the maps, then an unconditional data-file sync. The rest is left to the next run's steps.<br>(c) Unchanged | design, **[quiet-wrong] under (b)** | (a). It keeps every downstream reading of the publish as it is: the next run's snapshot size against the BEGIN ceiling, its redo start, and `D` for BC and BF. It paces the one writeback W1 leaves unpaced. (b) writes less per run, but grows the next BEGIN record, ages the next redo start by about one run, and rests on every snapshot page having been written before the last step returned, with one data file; a cell must pin that. (c) keeps the bug entry's unpaced writeback, made longer by W1. `CheckpointAnchor::Publish` is synchronous (`checkpointer.hpp:199-204`), so (a) and (b) both split the seam | |
| BI-Q6 | **`pages_per_step` stays 64** | measurement | Yes. S1 reports a step's reactor hold, and any change is put as an item at S1's report | |
| BI-Q7 | **The wake for a parked step.**<br>(a) The thread kicks the requesting core through the wake registry when the sync it asked for completes (write-then-kick, `sched.md` §9 invariant 7). The data-file thread does this from BI-S4; the log writer does it for a checkpoint's request.<br>(b) No kick: an idle reactor's block (≤ 10 ms) bounds each park's extra wait | design | (a). Under (b), a run of N steps on an idle core can wait up to 2 × 10 ms per step, about 27 s more at the BEGIN ceiling's 1,370 steps, and that time holds the instance's gate. The writer's kick is what BA-R4 part 1 builds for commits; BI builds it for the checkpoint's request only, and W4 keeps the drain out | |
| BI-Q8 | **The gate's fairness under longer runs** (§1.6).<br>(a) The gate admits the core whose last completed run is oldest. A tick from a core that is not next skips.<br>(b) Unchanged; §5.2 reports per-core anchor gaps | design | (a). A run near the period lets one core win every tick. The fold then holds every other core's last anchor and `D` stops: no wrong answer, but a log that is never recycled. (b) leaves that to a measurement that might not reach the run length that triggers it | |
| BI-Q9 | **W1's reading:** one coroutine per run, yielding after each `Step`, rather than a new task object per `Step` (BI-R1) | process | Yes. The scheduler cannot tell them apart. The coroutine keeps the gate hold and the cursor in one owner | |
| BI-Q10 | **BI-S1's premise gate.** BI stops for a ruling when either:<br>(i) no cell shows a run holding its reactor over 10 ms in one iteration; or<br>(ii) the per-phase split puts a step's page writes, which W1 and W2 do not move, over 10 ms in the cells | process | As written. (i) is BA's P12 bar; (ii) says the decisions would not reach the stall | |
| BI-Q11 | **The bar for BI-S5**, A = BI-S1's instrumented commit and B = the close:<br>(i) the longest reactor iteration inside a run under 10 ms in every cell, the publish included;<br>(ii) the same-core probe's p99 inside runs within 2× its p99 outside;<br>(iii) the OLTP overhead A/B within its own run-to-run noise;<br>(iv) run length, per-core and instance-wide anchor gaps, and `D`'s lag reported for A and B, with no bar | measurement | As written. (iv) has no bar because W1 accepts longer runs; it is reported so the operator can see what that costs | |
| BI-Q12 | **The measurement** (§5.2), at BI's close, per `CLAUDE.md`'s Session Workflow step 3 | process | As written | |

## 5. Measurement

### 5.1 BI-S1 (W3)

`[design]` In `build-release`, rebuilt at BI-S1's instrumented commit and
named by `git describe --tags`. `bench/README.md`'s five rules apply. The
load driver is the bug entry's archived `mix.py`, stated as such (rule 5's
spirit), unless a `tools/` driver has taken it.

- **Load.** 4 sessions of 1024-row named-key `INSERT`s into one btree.
- **Config.** `checkpoint_interval_ms = 5000`. `buffer_pool_frames` is the
  bug entry's 786,432, or the largest the host holds, stated.
- **Cells.** `cores` {1, 4} × `durability` {`relaxed`, `group`}, each at
  least 10 minutes, three runs per cell, the cells interleaved.
- **The same-core probe.** One probe session per core, found by opening
  connections until `SHOW META`'s `core=` covers every core
  (`multicore_benchmark.py`'s method). Each probe runs a pk point `SELECT`
  on a separate warm relation every 20 ms.
  - **Open-loop:** latency counts from each request's *scheduled* send
    time, so a 5 s stall counts as about 250 samples, not one. A
    closed-loop probe records one slow sample per stall, and a p99 taken
    from it would hide the stall.
  - **Classing:** a sample is *inside* when its scheduled time falls in
    one of its own core's runs, by the per-run Info line, and *outside*
    otherwise.
  - A `C_PING` probe per core, the bug entry's, runs beside it.
- **Report, per core and cell:**
  - runs, refused runs, and steps and pages per run;
  - run wall time (p50 and max) and the per-phase split (sum and max);
  - the longest iteration, and the iterations over 10 ms, 100 ms and 1 s,
    inside runs and outside them;
  - probe p50, p99, p99.9 and max, inside and outside, for the statement
    and for `C_PING`;
  - anchor gaps, per core and instance-wide;
  - `sched_wall_us − Σ polled − idle`, to confirm the run is charged to no
    group (§1.1).
- **Host.** `/proc/loadavg`, `/proc/pressure/io` and `df -T`, per cell.

### 5.2 BI-S5

`[design]` A against B, as BI-Q11 defines them, with §5.1's cells plus:

- **The OLTP overhead A/B**, interleaved, with a pool larger than the
  working set and the checkpoint cadence on. BI adds a task submit per run,
  a flag test per tick and two parks per step, and nothing per statement.
  The expectation is flat.
- **An idle-core run.** A run on a core with no foreground load, A against
  B: its wall time, which is BI-Q7's price if (b) was marked.
- **Core 0's `relaxed` loss window during its own run.** The longest gap
  between interval syncs while core 0 checkpoints (§1.6).

## 6. Row status

### BI-S0 — written 2026-10-08

Written on `worktree-checkpoint-reactor-workorder` from `65a28553`
(`v2.7.0-703-g65a28553`). No file under `src/`, `include/` or `tests/`
moved, and no suite ran.
