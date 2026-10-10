# Work order BI — the checkpoint stops holding its reactor: one step per task, its syncs parked

Written 2026-10-08 on `worktree-checkpoint-reactor-workorder` from
`65a28553` (`v2.7.0-703-g65a28553`). It follows the operator's words of
2026-10-08, translated into English (`raft-marks-2026-10-08.md` §2-§5):

- **W0:** *"Write one new work order and push it. Subject: remove the
  problem of the checkpoint synchronously blocking a core's reactor."*
- **W1:** *"(1) Run one Step as one reactor task, yielding between
  steps."*
- **W2:** *"(2) Hand the data file sync and EnsureDurable to the writer/I/O
  thread, and park the calling task (the same shape as RequestSyncNow)."*
- **W3:** *"S1 only measures. It measures, in build-release, the reactor's
  stall time during a checkpoint and the p99 of other sessions on the same
  core. The only code change is instrumentation."*
- **W4:** *"Session placement (BA-R13, CN-10 §3.4) and the inline
  fdatasync of core 0's commit drain are out of scope."*
- **W5:** *"Follow the same structure as the existing work orders, and tag
  every claim [source-read]/[design]/[measured]."* and *"Leave every Q item
  unmarked."*

**Written, not opened.** W1 and W2 are the operator's decisions, and they
stand here as BI-R1 and BI-R2. Every BI-Q item is unmarked (W5), BI-Q0
included, so no stage is startable.

**Claim tags as always:**
- `[source-read]` with `path:line` at `65a28553`;
- `[design]` for CLA's reasoning about code not yet written;
- `[measured]` with the invocation;
- `[quiet-wrong]` marks an item where a wrong choice turns a refusal or a
  slow answer into a wrong one.

The operator's words, and a rule that restates them, are decisions rather
than claims, so they carry no tag.

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

- **BI-R1** (W1). The cadence callback only submits a run task. The task
  takes one `Step` and yields before the next.
- **BI-R2** (W2). The log gate's `EnsureDurable` and the data-file sync
  become requests that return at once. The task parks on a predicate until
  each completes, as a commit parks today.
- **BI-R3** `[design]`. A run parks only between writebacks. Across a
  `co_await` it holds no writeback claim, pin, page latch, superblock or
  registry latch, and nothing bound to its core's thread.
- **BI-R4** `[design]` `[quiet-wrong]`. A run's gate hold lives in its
  checkpointer, never in a task's frame. Shutdown releases every hold,
  abandons every in-progress run, and checkpoints from a fresh snapshot.
- **BI-R5** `[design]`. What W1 and W2 do not move is named, counted by S1
  and decided by an item, never left implicit:
  - the page writes themselves;
  - the batch gate after a park;
  - the per-run gate inside `WriteBack`;
  - the claim wait's sleeps;
  - the publish's whole-store writeback.

**BI does not do:**

- **Session placement** (W4): BA-R13 and BA-S16, CN-10 §3.4 (decision U6),
  and `docs/pending/session-load-balancing.md`.
- **Core 0's commit drain's inline `fdatasync`** (W4).
  - `[source-read]` That is `WalManager::DrainOnce`'s `Sync()` on an owning
    manager with a parked group commit (`src/wal/manager.cpp:478`), which is
    BA-R4 part 1 and BA-S7.
  - `[design]` BI-R2 moves core 0's *checkpoint* log waits, which today
    reach the same inline `Sync()` (`:347` → `:255`), by calling
    `RequestSyncNow` from the checkpoint task. The drain's call is not
    touched.
- **The trx-id carve's whole-pool sync**, which is BA-R10's first half
  (`[source-read]` `workorder-ba-parallelism.md:915-926`).
- **The client `SYNC`'s store sync** (`[source-read]`
  `src/server/command_dispatcher.cpp:1276`, `HandleSync`).
- **The page writes' own latency.** `[design]` W2 names the syncs, so
  `pwrite` stays on the reactor, bounded per poll by one step (§1.6).
- **The `CHECKPOINT_BEGIN` size ceiling.** `[source-read]` The bug entry
  records it at about 87,000 dirty pages, refused at the first append.
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
  `core_id/cores` of a period (`src/server/core_runtime.cpp:534-541`). Core
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
- `[source-read]` **So the checkpoint is not a `system`-group task**,
  although two places say it is: *"run as a `system`-group task per core"*
  (`docs/spec/wal.md` §11) and `include/kds/wal/checkpointer.hpp:17-18`.
  - Its time lands in what `sched.md` §4 calls *"charged to no group"*.
    `sched_system_polled_us` does not see it; only
    `sched_wall_us − Σ polled − sched_idle_block_us` bounds it.
  - The one `kSystem` producer in `src/` at this commit is
    `src/server/tcp_server.cpp:386`.
- `[source-read]` **The chain.** `CoreRuntime::Checkpoint`
  (`core_runtime.cpp:571`) and `Expeditor::Checkpoint` (`expeditor.cpp:1234`)
  each call `Checkpointer::RunGated` (`src/wal/checkpointer.cpp:331`).
  - `RunGated` takes the gate's RAII `Hold` (`:333`) and calls
    `RunToCompletion` (`:345`).
  - `RunToCompletion` resumes an in-progress run or calls `Start`
    (`:314-318`), loops `Step` until done (`:319-327`), and then calls
    `Complete` (`:328`).
- `[source-read]` **The header promises what the loop breaks.**
  - `checkpointer.hpp:32-36`: *"Step() flushes a bounded batch, so a
    checkpoint spreads across reactor iterations"*.
  - `pages_per_step` = 64 is *"small enough that a checkpoint does not
    monopolize a reactor iteration"* (`:270-277`).
  - `RunToCompletion` is *"for callers that do not need the checkpoint
    spread across reactor iterations (tests, and shutdown)"* (`:332-335`).

  Yet the cadence calls `RunToCompletion`.
- `[source-read]` **The spec principle it breaks.**
  - `sched.md` §3: *"Blocking syscalls inside tasks are forbidden; all
    waiting is expressed as suspension"* (`:42`).
  - `docs/spec/page.md` §15 item 2 rejects mmap because a blocked thread
    *"freezes every task on the core"*, which is what a run does.

### 1.2 What one step does

- `[source-read]` **The path.** `Step` (`checkpointer.cpp:202`) calls
  `target_.FlushPages(batch)` (`:214`). That is
  `PageStoreCheckpointTarget::FlushPages`
  (`include/kds/storage/page_store_checkpoint_target.hpp`), which calls
  `DevicePageStore::FlushPages` (`src/storage/device_page_store.cpp:2234`).
  Both seams are synchronous (`checkpointer.hpp:170-181`).
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
      `AwaitWritebackClaim` (`:1977`): 64 yields, then 50 µs `sleep_for`s
      on the reactor thread (`:1820-1836`);
    - copies each page under its shared page latch;
    - re-gates with `gate->EnsureDurable(run_lsn)` if a page's LSN passed
      the batch gate (`:2059-2068`);
    - writes with `device_.WritePageRun` / `WritePage` (`:2073-2076`);
    - cleans the frames (`:2095-2106`), before any sync.
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
- `[source-read]` **Core 0's inline gate sync is a recorded decision.**
  - `manager.cpp:185-195` keeps every waited-on sync of an owning manager
    on the calling thread, *"a checkpoint's gate"* among them, because a
    hand-off *"doubled `group`'s p99"*.
  - `expeditor.cpp:808-816` lists *"the checkpoint gate's"* sync as staying
    on the reactor for the same reason.

  `[design]` The reason does not reach a checkpoint. Nobody waits on a
  checkpoint's latency, while every session on the core waits out its
  inline sync. BI-R2 overrides that line for the checkpoint alone, and
  BI-S5 restates both comments.
- `[source-read]` **Core 0 has a writer.** `expeditor.cpp:820` starts it,
  so `RequestSyncNow` on core 0 hands the sync to it
  (`manager.cpp:285-306`). Only a writerless manager (tests, tools) syncs
  inline there (`:286-291`).

### 1.3 `Complete` and the publish

- `[source-read]` **`Complete`** (`checkpointer.cpp:238`) appends
  `CHECKPOINT_END` (`:258`). It makes the record durable through the same
  two `EnsureDurable` shapes (`:269`), then calls `anchor_.Publish` (`:278`).
  The seam is synchronous (`checkpointer.hpp:199-204`).
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
  syncs the store itself, after every step"*
  (`device_page_store.cpp:2256-2274`).

### 1.4 What BI builds on

- `[source-read]` **Coroutines.** `MakeCoroTask` is at
  `include/kds/sched/coro.hpp:405`, `Yield` at `:416`, and `WaitUntil`
  (a predicate re-tested once per poll) at `:447-458`. The `system` group
  is *"checkpoint/WAL housekeeping"* (`include/kds/sched/task.hpp:21`).
  `wal/` already depends on `sched/` (`include/kds/wal/manager.hpp:12`).
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
- `[source-read]` **The writer syncs and recycles the log, and touches
  nothing else.** It takes a device and a sync function and *"the only
  thing it knows how to do is make bytes that are already written durable"*
  (`include/kds/wal/writer.hpp:51-54`). Its sync runs at `writer.cpp:115`.
  It also unlinks recycled segments and syncs the log directory after a
  pass (`writer.cpp:53-59`, `:99`, `:140`; `wal.md` §11-4). Nothing in it
  touches the data file.
- `[source-read]` **The data-file sync has no gate.**
  - `FilePageDevice::Sync` returns an `fsync` failure and records nothing,
    so a later call can report OK.
  - Every core's reactor calls it: through checkpoint steps, the publish,
    the carve, `SYNC` and shutdown.
  - The log's sync is the contrast. Since `fsync-fail-stop` it is one gate,
    one sync at a time, and a failure stops it for good, because *"on
    Linux a failed `fsync` can drop the dirty pages and clear the error"*
    (`docs/spec/wal.md` §6 item 5).

  The review of BI-S0 found what that costs, now
  `docs/inflight/bugs/a-failed-data-file-fsync-leaves-its-pages-clean.md`
  (BI-Q3).
- `[source-read]` **A park's wake is a timer.**
  - A parked predicate is re-tested once per poll.
  - An idle reactor blocks until the next timer, capped at
    `max_idle_block_ms` = 10 (`scheduler.cpp:170-209`, `scheduler.hpp:79`).
  - Every core arms the drain at `wal_drain_interval_ns`. That is 1 ms by
    default (`include/kds/server/expeditor.hpp:442`), passed to peers at
    `expeditor.cpp:1698` and armed at `core_runtime.cpp:507-508` and
    `expeditor.cpp:2010-2011`.

  So a parked step on an idle core waits at most about 1 ms past its
  sync. The writer wakes only threads blocked in its own `EnsureDurable`
  (`writer.cpp:38`), never a reactor.
- `[source-read]` **The kick has no unregistration.** `WakerTable::Kick`
  dereferences the destination's registered pointers
  (`include/kds/sched/waker_table.hpp:103-112`), and the table has no call
  that removes an entry. A peer's scheduler dies at `cores_.clear()`
  (`expeditor.cpp:2105`).

### 1.5 What the bug entry measured, and what it did not

- `[measured]` **The load and its result.**
  - **Config:** `cores = 4`, `durability = relaxed`,
    `checkpoint_interval_ms = 5000`, `buffer_pool_frames = 786432`.
  - **Load:** 4 sessions of 1024-row named-key `INSERT`s into one btree.
  - **Probe:** 8 KWP connections pinging every 200 ms.
  - **Result:** 22 stalls of 2 s or more, 21 ending within [-1.5 s, +1 s]
    of the stalled core's own anchor (12 when the ends were shifted 2.5 s).
    The longest was 5.7 s.
  - **Thread state** of the stalled core: 93 % off-CPU.
    `jbd2_log_wait_commit` 26 %, `folio_wait_bit_common` 20 %,
    `rq_qos_wait` 16 %, `futex_do_wait` 30 %. It was never in `ep_poll`.
  - **Per-core anchor gaps** reached 83-110 s in the 07:31 run, while runs
    were 6 s or shorter.

  Invocation: the archived `pingprobe.py`, `threadsample.py` and
  `stallstats.py` against xrock's server, 08:41:05-08:49:05 UTC.
- `[source-read]` **What it does not give** (bug entry, *Measurement*),
  which S1 owes:
  - pages per run (`pages_flushed` is a Debug line, and the server logged
    at `info`);
  - the split of a run's time between the log gate, the page writes, the
    data-file `fsync`, the `CHECKPOINT_END` wait and the publish's
    writeback;
  - a *statement's* latency on the stalled core, since it measured pings;
  - a cell at a commit named by `git describe` under `bench/v3.0.0/`.

  Nor does its `wchan` split separate a buffered write's waits from an
  `fsync`'s.

### 1.6 What W1 and W2 leave, read against the tree

- `[design]` **The page writes.** They stay on the reactor. Under BI-R1 a
  poll writes at most one step's 64 pages, in runs of up to 8, plus that
  step's map pages. Whether one step's writes stay under BA's 10 ms bar on
  a loaded device is S1's to measure, not this survey's to assume.
- `[design]` **The batch gate after a park.** While a step is parked on the
  log, this core's sessions keep re-dirtying hot pages in its batch, which
  raises their `page_lsn`. Under `relaxed`, nothing else moves the durable
  point, so `WriteBack`'s own `AwaitWalGate` (`:1861`) then syncs inline on
  the reactor. BI-R2 re-scans after the wake.
- `[design]` **The per-run gate** (`device_page_store.cpp:2059-2068`). It
  is a synchronous `EnsureDurable` inside `WriteBack`, reached when a page
  in the run was logged after the batch gate. A task cannot park in it:
  the frames are claimed and the run's bytes copied at that point, and
  BI-R3 forbids a park while holding a claim. BI-Q4 decides it.
- **The claim wait.** `[source-read]` A writeback claims, writes and
  releases one run before it looks at the next (the comment at
  `:1898-1902`). `[design]` Under BI-R3 no park lengthens another
  writeback's wait.
- `[design]` **The publish.** W1 alone makes it worse: a spread run lasts
  longer, so more is dirty when the publish's unpaced `Flush` runs. BI-Q5
  decides it.
- `[design]` **Run length and the gate.** A spread run holds the
  instance's gate longer, and every other core's tick skips while it does
  (`checkpointer.hpp:225-234`). Per-core anchors and `D` then move later,
  so BC's recycling and BF's reclaim, which read `D`, lag, and recovery's
  replay range grows. W1 accepts spreading, and §5 prices it.
- **The stagger fails once a run outlasts a stagger slot.**
  - `[source-read]` `core_runtime.cpp:527-533` says why the ticks are
    staggered: *"whenever a run outlasts the start-up skew, the same core
    wins each time and the rest skip for good, holding the fold, and so
    the anchor, at their completion checkpoints"*.
  - `[design]` The slot is `period / cores`. At `cores = 4` and a 5 s
    period, the ticks fall at 0, 1.25, 2.5 and 3.75 s. Take 2 s runs:
    - core 0 runs 0-2 s, and core 1's tick at 1.25 s skips;
    - core 2 runs 2.5-4.5 s, and core 3's tick at 3.75 s skips;
    - core 0 runs again at 5 s.

    Cores 1 and 3 never run. `FoldedAnchor`'s minimum then stays at their
    last records (`superblock_checkpoint_anchor.cpp:38-64`), `D` stops,
    and the log is never recycled. That is not a wrong answer but a log
    that grows until restart.
  - `[measured]` The bug entry's 83-110 s per-core gaps with runs of 6 s
    or less (§1.5) say this is live today. W1 makes runs longer. BI-Q7
    decides it.
- **Shutdown** `[quiet-wrong]`. `[source-read]` Today a run cannot be in
  flight when a reactor stops, because the run is synchronous. The
  shutdown checkpoints run on the expeditor's thread after the join:
  - peers' at `expeditor.cpp:2097`, through `ShutdownCheckpoint`
    (`core_runtime.cpp:581-590`), which syncs the store first;
  - core 0's at `:2163`, after its own `Sync()`.

  A peer's scheduler lives until `cores_.clear()` (`:2105`), and core 0's
  until `running_.reset()` (`:2173`). `Scheduler` has no call that drops
  one queued task (`scheduler.hpp:101-164`). `[design]` Under a naive
  BI-R1 that put the gate's `Hold` in the task's frame, two things break:
  - **Every shutdown checkpoint skips.** A parked run on *any* core keeps
    the instance's gate held, so each `RunGated` skips.
    `docs/inflight/known-gaps.md:152-185` says what rests on every
    shutdown checkpoint publishing: the assertion scan's floor. A missing
    one can bring a peer up refusing an asserted relation's writes for the
    life of the mount.
  - **Resuming the parked run is no better.** `RunToCompletion`'s resume
    arm (`checkpointer.cpp:314-318`) would publish the parked run's old
    snapshot: an old recLSN as the redo start under a recent
    `checkpoint_lsn`. That is exactly the record shape
    `known-gaps.md:176-182` names as the defect's precondition. It also
    defeats the shutdown checkpoint's own purpose, a redo start equal to
    its `CHECKPOINT_BEGIN` (`expeditor.cpp:2151-2159`).

  BI-R4 is the rule for both.
- **The mount's completion checkpoint stays synchronous.** `[source-read]`
  It runs on the startup thread before any reactor exists, and says so
  (`src/server/mount_recovery.cpp:367-371`).
- **Core 0's `relaxed` loss window.** `[source-read]` The bug entry reads
  the window as unbounded while core 0 runs a checkpoint: the drain runs on
  core 0's reactor (`expeditor.cpp:2008`, `:2011`), and a peer's tick
  returns before the loss-window branch (`manager.cpp:495-497`).
  `[design]` Under BI-R1 core 0's reactor turns between steps, so the
  post-task drain runs every iteration of a run. §5.2 checks that rather
  than assuming it.
- `[source-read]` **The seams S1 to S4 touch.**
  - `Step` and `CheckpointTarget::FlushPages` are synchronous
    (`checkpointer.hpp:170-181`, `:320-324`). S3 and S4 split them into a
    scan, a write and a sync request, and BI-Q5's options split `Publish`.
  - `Checkpointer` holds no clock (`checkpointer.hpp:355-372`), and the
    page store's checkpoint path reads none, so S1 hands them the
    scheduler's.
  - The suspend audit's pin half runs on core 0 only: a peer borrows the
    pool and installs the audit with no store (`core_runtime.cpp:547-556`).
  - `CoreRuntime::Checkpoint` sets a `CurrentCoreGuard` (`:573`). A task
    frame destroyed on the expeditor's thread at shutdown must not hold
    one.

## 2. Rulings

BI-R1 and BI-R2 restate the operator's decisions (W1, W2). BI-R3..R5 are
CLA's, and BI-Q0 opens them.

### BI-R1 — One `Step` per task poll (W1)

- `[design]` **One body, in the checkpointer.** `Checkpointer` gains the
  run coroutine and the per-core in-flight state, so `Expeditor` and
  `CoreRuntime` keep sharing `RunGated`'s single body
  (`checkpointer.hpp:337-344`) rather than growing two copies.
- `[design]` **The cadence callback submits, and does nothing else.**
  - `SubmitEvery`'s callback submits `MakeCoroTask(kSystem, run)` when no
    run is in flight on this core, and returns.
  - A tick during a run submits nothing. `SubmitEvery` already coalesces
    missed periods (`scheduler.cpp:135-154`).
- `[design]` **The run task:**
  1. Take the gate into the checkpointer (BI-R4), or skip as today on a
     miss.
  2. `Start`, as one synchronous call in one poll. `CHECKPOINT_BEGIN` is
     appended at `checkpointer.cpp:157`, outside the registry latch. The
     latch spans only the snapshot appends (`:164`;
     `checkpointer.hpp:152-157`).
  3. For each step: `Step`, then `co_await sched::Yield{}`.
  4. `Complete`, parked as BI-R2 says.
  5. `RunGated`'s log lines, with S1's per-run Info line.
- `[design]` **W1's "one task" is read as one coroutine per run that
  yields after each `Step`** (BI-Q8). To the scheduler, that is the same as
  a new task per `Step`, because a task is polled at most once per
  iteration either way (`sched.md` §4).
- `[design]` **`RunToCompletion` stays for the synchronous callers:** the
  mount's completion checkpoint, the shutdown checkpoints and tests. Its
  header comment becomes true again.
- `[design]` **`pages_per_step` stays 64** until S1 reports a step's
  reactor hold (BI-Q9).

### BI-R2 — The waits leave the reactor; the task parks (W2)

- `[design]` **The log gate.** `AwaitWalGate`'s scan is split from its
  wait. Before a step's writeback, the task:
  1. computes the batch's highest `page_lsn` (the scan, under the structure
     latch, as today);
  2. if that LSN is not durable, calls `RequestSyncNow` on its core's
     manager;
  3. `co_await sched::WaitUntil` on `IsDurable(lsn) || stopped()`;
  4. after the wake, re-scans in the same poll that calls `WriteBack`, and
     parks again if the batch has moved past the durable point (§1.6).
     The rounds are bounded, and what is left after the bound is the batch
     gate's inline remainder (BI-R5), counted.

  `Complete` does the same for `CHECKPOINT_END`'s LSN in place of
  `EnsureDurable` (`checkpointer.cpp:269`). A fail-stopped log answers the
  park through `stopped()`, and the step fails, as `WriteBack`'s refusal
  does today (`device_page_store.cpp:1857`).
- `[design]` **On core 0 this moves the checkpoint's log syncs to the
  writer** (`expeditor.cpp:820`). It overrides `manager.cpp:185-195` for
  the checkpoint alone, for §1.2's reason. The drain's `Sync()`
  (`manager.cpp:478`) is W4's and unchanged.
- `[design]` **The data file.** A step's sync and the publish's sync become
  a request to the thread BI-Q2 names:
  - the request returns a ticket;
  - the task parks on `WaitUntil` until the thread's completed ticket
    passes it, or a failure is recorded;
  - a failure fails the step or the publish, with the run left in
    progress, as a failed step is today (`checkpointer.cpp:214-226`);
  - BI-Q3 decides what a failure does beyond that.
- `[design]` **"the same shape as RequestSyncNow"** is read as two things: a
  request that does no I/O on the caller's thread and returns at once, and
  a park on a predicate that the thread's progress satisfies. The commit's
  park (`command_dispatcher.cpp:755-773`) is the template, including the
  `stopped()` disjunct and the error answered after the wake.
- `[design]` **The wake.** The drain timer ends an idle block within about
  1 ms (§1.4), so a parked step needs no kick to make progress. BI-Q6
  decides whether the threads kick anyway.

### BI-R3 — Parks only between writebacks

`[design]` Across a `co_await`, a run task holds none of the following:
- a writeback claim (`Frame::writing`), a pin or a page latch;
- the structure latch;
- the superblock latch (`superblock_checkpoint_anchor.cpp:89`);
- the assertion registry's latch;
- a `CurrentCoreGuard` or anything else bound to its core's thread.

That is `sched.md` §3's suspension safety, applied. By construction,
every park in BI-R2 sits before a `WriteBack` call or after it returns.
The suspend audit checks the pin half on core 0 only (§1.6), so S2's cells
check it on a peer.

### BI-R4 — A run's gate hold lives in its checkpointer; shutdown starts fresh `[quiet-wrong]`

- `[design]` **The hold.** The run's `CheckpointGate::Hold` is kept in the
  `Checkpointer`, beside the in-flight state, not in the coroutine's frame.
  The task releases it when the run completes or fails.
- `[design]` **Shutdown.** After the join and before the first shutdown
  checkpoint (`expeditor.cpp:2097`), the shutdown path does this for every
  core, core 0 included:
  1. Release its hold.
  2. Abandon its in-progress run (`Checkpointer::Abandon`): drop the
     snapshot and the cursor, log nothing. The abandoned run leaves a
     `CHECKPOINT_BEGIN` and a whole snapshot run with no
     `CHECKPOINT_END`, which is what a crash mid-run leaves today
     (`checkpointer.hpp:43-44`).
  3. Each shutdown checkpoint then runs a fresh `RunToCompletion` after
     its store sync, so its redo start is its own `CHECKPOINT_BEGIN`, as
     `expeditor.cpp:2151-2159` intends.
- `[design]` A shutdown checkpoint that skips on a held gate, or that
  publishes a resumed snapshot, is the defect this rule exists for. BI-S2
  pins both.

### BI-R5 — The remainder is named

`[design]` Five things stay on the reactor or stay unpaced unless an item
moves them. S1 counts each, and the close reports each:

| remainder | where | decided by |
|---|---|---|
| a step's page writes | `device_page_store.cpp:2073-2076` | not moved (W2 names the syncs); BI-Q9's gate |
| the batch gate after a park | `:1861`, past BI-R2's bounded re-park | counted; BI-Q9's gate |
| the per-run gate | `:2059-2068` | BI-Q4 |
| the claim wait's sleeps | `:1820-1836` | not moved; counted |
| the publish's whole-store writeback | `superblock_checkpoint_anchor.cpp:132` → `device_page_store.cpp:2169` | BI-Q5 |

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BI-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The index row</li><li>`raft-marks-2026-10-08.md` §2-§5</li><li>The bug entry's *The fix* pointing here</li></ul> | S |
| BI-S1 | **The stall, measured** (W3) | <ul><li>**Code: instrumentation only, no behaviour change:**<ul><li>per run: wall time from the gate's entry to the publish's return, steps, pages written, the dirty-table size at `Start`, and a refused `Start` (the BEGIN ceiling) counted;</li><li>per phase, summed and maxed per run: `Start`, the log gate's wait (batch and per-run arms apart), the page writes, the map writes, the claim wait, the data-file `fsync`, the `CHECKPOINT_END` wait, the publish's writeback and the publish's `fsync`;</li><li>per reactor: the longest `RunOnce`, and a count of iterations over 10 ms, 100 ms and 1 s;</li><li>one Info line per run: core, wall start and end, and the totals, so a probe sample can be classed against its own core's runs;</li><li>`SHOW META` prints the per-reactor counters and the last run's per-phase split for the session's core (`core=` already says which, `command_dispatcher.cpp:1321`).</li></ul>The scheduler's injected clock is handed to the checkpointer and the store's checkpoint path (§1.6; `sched.md` invariant 8). It is read per phase per step, never per page.</li><li>**Cells** for the counters:<ul><li>a run of N steps reports N;</li><li>a scripted device's delay lands in its own phase;</li><li>the iteration counters move under a deliberately slow timer callback.</li></ul></li><li>**The measurement** (§5.1), into `bench/v3.0.0/results-bi-s1-checkpoint-stall-<describe>.md`.</li><li>**The gate** (BI-Q9).</li><li>**The suite green.**</li></ul> | M |
| BI-S2 | **The run as a task** (BI-R1, BI-R3, BI-R4) | <ul><li>**Code:**<ul><li>the run coroutine and the in-flight state in `Checkpointer`;</li><li>the submit-only callback;</li><li>the hold kept in the checkpointer;</li><li>`Abandon`, and shutdown's release-abandon-fresh order.</li></ul>The waits are still inline, so S2 alone bounds a poll at one step, plus `Start`, plus `Complete`.</li><li>**Cells:**<ul><li>a run of N steps spans at least N reactor iterations;</li><li>a foreground task submitted during a run is polled between two of its steps;</li><li>the callback returns without running a step;</li><li>a tick during a run submits nothing;</li><li>with a run parked mid-steps on core 2, core 1's and core 0's shutdown checkpoints both publish;</li><li>each published shutdown redo start equals that checkpoint's own `CHECKPOINT_BEGIN` LSN;</li><li>on a peer, a run task holds no pin at any park (the audit's missing half);</li><li>a crash mid-steps replays from the previous anchor (sim).</li></ul></li><li>**Mutations, each repeated:**<ul><li>`RunToCompletion` restored in the callback (killed by the interleave cell);</li><li>the in-flight state removed (killed by the second-tick cell);</li><li>the hold moved back into the frame (killed by the cross-core shutdown cell);</li><li>shutdown resuming instead of abandoning (killed by the BEGIN-LSN cell).</li></ul></li><li>**The suite green** and the sim corpus.</li></ul> | M |
| BI-S3 | **The log gate parked** (BI-R2's first half; BI-Q4, BI-Q6) | <ul><li>**Code:**<ul><li>`AwaitWalGate`'s scan split from its wait;</li><li>`RequestSyncNow`, the park and the bounded re-scan for each step and for `CHECKPOINT_END`;</li><li>core 0's checkpoint through its writer;</li><li>the per-run gate as BI-Q4 marks, and the wake as BI-Q6 marks.</li></ul></li><li>**Cells:**<ul><li>on the two-core rig, a peer's step whose batch is past the durable point parks while another task on its reactor runs, and core 0's does the same through its writer;</li><li>a page re-dirtied during the park is re-gated by a second park, not an inline sync (the batch-gate counter stays 0 within the bound);</li><li>WAL-before-data: a page whose record is not durable is never written (`EvictionWritebackTest.FlushBeforeEvictHoldsWithTwoCoresDirtyingOnePage`, `tests/eviction_test.cpp:531`, and a step-shaped cell);</li><li>a fail-stopped log answers a parked step, which writes nothing and leaves the run in progress.</li></ul></li><li>**Mutations:**<ul><li>the park removed and the gate skipped (killed by the WAL-before-data cell);</li><li>the re-scan removed (killed by the re-dirty cell);</li><li>the `stopped()` disjunct removed (killed by the fail-stop cell's bound).</li></ul></li><li>**The suite green.**</li></ul> | M |
| BI-S4 | **The data-file sync off the reactor** (BI-R2's second half; BI-Q2, BI-Q3, BI-Q5) | <ul><li>**Code:**<ul><li>the thread BI-Q2 names, with its request, ticket and completion;</li><li>`FlushPages`' sync and the publish's sync through it;</li><li>the publish as BI-Q5 marks, and a failure as BI-Q3 marks;</li><li>the thread quiesced before `cores_.clear()` if BI-Q6 (a) is marked (§1.4).</li></ul>The synchronous callers (`SYNC`, the carve, shutdown, the mount) reach the same gate synchronously.</li><li>**Cells:**<ul><li>a step parks while a scripted data-file sync is held, and the reactor polls another task meanwhile;</li><li>a failed data-file sync fails its step and publishes no anchor, and then behaves as BI-Q3 marks;</li><li>under BI-Q5 (b), a crash after the publish finds every snapshot page on disk when another core's writeback carried some of them (the `[quiet-wrong]` cell).</li></ul></li><li>**Mutation:** the ticket compared before the sync returns (killed by the held-sync cell).</li><li>**The suite green** and the sim corpus.</li></ul> | M |
| BI-S5 | **The close** | <ul><li>**The measurement** (§5.2).</li><li>**Text:**<ul><li>`wal.md` §11 steps 2-3 and §8-1 (core 0's checkpoint gate through the writer);</li><li>`sched.md` §4 (the checkpoint charged to `system`) and §9 invariant 2 (the data-file thread's lock in the list);</li><li>`checkpointer.hpp:17-36`, `:269-277` and `:332-345`;</li><li>`manager.cpp:185-195` and `expeditor.cpp:808-816`, restated for the checkpoint's hand-off;</li><li>`device_page_store.cpp:2256-2274`'s cover argument, restated per BI-Q5;</li><li>`page.md` §13, which `wal.md` §11-2 cites for "checkpoint spreading" and which no longer carries it;</li><li>`CLAUDE.md`'s WAL row.</li></ul></li><li>**Docs:**<ul><li>the bug entry deleted if §5.2 meets BI-Q10's bar, or restated with the number;</li><li>`workorder-ba-parallelism.md`'s BA-R10 and BA-S13 amended per BI-Q1.</li></ul></li></ul> | S |

## 4. Items for the operator

All unmarked (W5). Every proposal is `[design]` unless a cell says
otherwise.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BI-Q0 | **Open BI**, with BI-S0..S5 and BI-R3..R5 as written. BI-R1 and BI-R2 are the operator's (W1, W2) | process | Yes | |
| BI-Q1 | **BI against BA.**<br>(a) BI owns P12's checkpoint half: BA-R10's third paragraph and BA-S13's mid-steps crash clause move here, and BA keeps the carve. BI-S1's gate replaces BA-Q1's census for this half.<br>(b) BI waits for BA's census | sequencing | (a). BA is paused, its census is unbuilt, and BI-S1 measures this stall directly, against BA's own 10 ms bar (`workorder-ba-parallelism.md:681`) | |
| BI-Q2 | **The thread that takes the data-file sync.**<br>(a) A data-file I/O thread, one for the instance, created at startup.<br>(b) The WAL writer (`writer.hpp`) takes the job beside syncing and recycling the log.<br>(c) One thread per core | design | (a).<ul><li>A data-file `fsync` can run for seconds (the bug entry's `jbd2_log_wait_commit` share). On the writer, (b) would put every core's commit sync, the `relaxed` loss window and recycling behind it.</li><li>(c) puts several concurrent `fsync`s on one descriptor, the case `wal.md` §6 item 5 names as one caller getting the error and another OK.</li><li>(a) adds one thread at startup, which `sched.md` invariant 1 allows, and one mutex to §9's list.</li></ul> | |
| BI-Q3 | **A failed data-file sync** `[quiet-wrong]` (`docs/inflight/bugs/a-failed-data-file-fsync-leaves-its-pages-clean.md`).<br>(a) The instance stops. Recovery replays from the last durable anchor, as PostgreSQL's `data_sync_retry = off` does.<br>(b) Sticky: no later data sync reports OK and no anchor publishes until a restart, while the instance keeps serving.<br>(c) As today: the step fails and the next tick retries | design | (a).<ul><li>The bug entry reads two wrong answers under (c). A frame cleaned before the failed sync can be evicted and read back stale at runtime. A retried run can also publish an anchor past records its pages still need.</li><li>(b) closes the second but not the first: those frames are already clean, so stopping later writebacks does not bring their bytes back.</li><li>Only a restart re-reads the log over them, and (a) is that restart. Its cost is an outage on a device error the log may have survived.</li><li>The defect predates BI. BI's thread is where the check goes.</li></ul> | |
| BI-Q4 | **The per-run gate inside `WriteBack`** (`device_page_store.cpp:2059-2068`).<br>(a) `WriteBack` returns the run unwritten with the LSN it needs (claims released, frames still dirty), and the task parks and re-runs the step.<br>(b) The inline wait stays as a counted remainder (BI-R5) | design, **[quiet-wrong] under (a)** | (b), with S1's per-run-arm counter as the gate. (a) is written only if S1 or BI-S3 counts it non-zero under load. (a) touches the order that keeps WAL-before-data: a run written before its LSN is durable is the violation `wal.md` §8-1 names | |
| BI-Q5 | **The anchor publish's whole-store writeback.**<br>(a) Paced. The publish collects once; the task writes the collection in `pages_per_step` batches with a yield between, then page 0, then the data-file sync, parked.<br>(b) Narrowed. Page 0 (through `WriteBack`'s claim) and the maps, then an unconditional data-file sync. The rest is left to the next run's steps.<br>(c) Unchanged | design, **[quiet-wrong] under (b)** | (a).<ul><li>It keeps every downstream reading of the publish as it is: the next run's snapshot size against the BEGIN ceiling, its redo start, and `D` for BC and BF. It also paces the one writeback W1 leaves unpaced.</li><li>(b) writes less per run, but grows the next BEGIN record and ages the next redo start by about one run. Its soundness is BA-R10's one-data-file argument (`workorder-ba-parallelism.md:923-926`) plus every snapshot page having been written before the last step returned; a cell must pin that.</li><li>(c) keeps the bug entry's unpaced writeback, made longer by W1.</li><li>(a) and (b) both split the synchronous `Publish` seam (`checkpointer.hpp:199-204`).</li></ul> | |
| BI-Q6 | **A kick for a parked step.**<br>(a) The thread kicks the requesting core through the wake registry when its sync completes (write-then-kick, `sched.md` §9 invariant 7).<br>(b) No kick: the 1 ms drain timer ends an idle block (§1.4) | design | (b).<ul><li>A park costs at most about 1 ms past its sync. On an idle core that is 2 × 1,370 × 1 ms ≈ 2.7 s added to a run at the BEGIN ceiling, and §5.2's idle-core run measures it.</li><li>(a) saves that time but needs every kicking thread quiesced before a peer's scheduler dies, since the wake registry has no unregistration (§1.4). The log writer's kick is also what BA-R4 part 1 builds for commits, which W4 keeps out.</li></ul> | |
| BI-Q7 | **The gate's fairness** (§1.6).<br>(a) The gate rotates. It admits the core whose last *entry* is oldest, and falls back to any asking core when that core has not asked within one period.<br>(b) Unchanged; §5.2 reports per-core anchor gaps | design | (a).<ul><li>Once a run outlasts `period / cores`, some cores skip every tick. The fold then holds their last anchors and `D` stops: no wrong answer, but a log that is never recycled. The bug entry's 83-110 s gaps say it happens today.</li><li>Keying on the last entry rather than the last completion keeps a core whose runs fail from blocking the rest.</li><li>The fallback keeps a core that stopped asking from blocking them.</li></ul> | |
| BI-Q8 | **W1's reading:** one coroutine per run, yielding after each `Step`, rather than a new task object per `Step` (BI-R1) | process | Yes. The scheduler cannot tell them apart, and the coroutine keeps the cursor and the run's state in one owner | |
| BI-Q9 | **BI-S1's premise gate, and `pages_per_step`.** BI stops for a ruling when either:<br>(i) no cell shows a run holding its reactor over 10 ms in one iteration; or<br>(ii) the per-phase split puts one step's page writes (64 pages, which W1 and W2 do not move) over 10 ms.<br>`pages_per_step` stays 64 unless (ii) holds, in which case its new value is part of the ruling | process | As written. (i) is BA's P12 bar. (ii) says the decisions would not reach the stall at 64 pages a step | |
| BI-Q10 | **The bar for BI-S5**, with A = BI-S1's instrumented commit and B = the close:<br>(i) the longest reactor iteration inside a run under 10 ms in every cell, the publish included;<br>(ii) the same-core probe's p99 inside runs within 2× its p99 outside;<br>(iii) the OLTP overhead A/B within its own run-to-run noise;<br>(iv) run length, per-core and instance-wide anchor gaps, and `D`'s lag reported for A and B, with no bar | measurement | As written. (iv) has no bar because W1 accepts longer runs; it is reported so that cost can be seen | |
| BI-Q11 | **The measurement** (§5.2), at BI's close, per `CLAUDE.md`'s Session Workflow step 3 | process | As written | |

## 5. Measurement

### 5.1 BI-S1 (W3)

`[design]` In `build-release`, rebuilt at BI-S1's instrumented commit and
named by `git describe --tags`. `bench/README.md`'s five rules apply. The
load driver is the bug entry's archived `mix.py`, stated as such, unless a
`tools/` driver has taken it.

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

`[design]` A against B, as BI-Q10 defines them, with §5.1's cells plus:

- **The OLTP overhead A/B**, interleaved, with a pool larger than the
  working set and the checkpoint cadence on. BI adds a task submit per run,
  a test per tick and up to two parks per step, and nothing per statement.
  The expectation is flat.
- **An idle-core run.** A run on a core with no foreground load, A against
  B: its wall time, which is BI-Q6's price under (b).
- **Core 0's `relaxed` loss window during its own run.** The longest gap
  between interval syncs while core 0 checkpoints (§1.6).

## 6. Row status

### BI-S0 — written 2026-10-08

Written on `worktree-checkpoint-reactor-workorder` from `65a28553`
(`v2.7.0-703-g65a28553`). No file under `src/`, `include/` or `tests/`
moved, and no suite ran.

**Its review** (`critics-developer`, read-only, at `14c281a0`) checked
about 100 citations and found about 95 right. Applied, in the text above:

- **BI-R4 was wrong twice**, and is rewritten:
  - The skip was cross-core: the gate is the instance's, so a parked run
    on any core would make every shutdown checkpoint skip.
  - The proposed resume would have published an old cadence snapshot,
    the assertion floor's precondition.

  The hold now lives in the checkpointer, and shutdown abandons and starts
  fresh.
- **The batch gate after a park** was called a no-op. It is not, under
  re-dirtying and `relaxed`. BI-R2 now re-scans and re-parks, bounded,
  and the remainder joins BI-R5.
- **The fairness threshold** is `period / cores`, not "near the period",
  and is live today (the bug entry's 83-110 s gaps).
- **The wake arithmetic was about 10× high.** The 1 ms drain timer ends an
  idle block, so BI-Q6 now proposes no kick. The kick's missing
  unregistration is recorded.
- **`CHECKPOINT_BEGIN` is appended outside the registry latch.**
- **The decision BI-R2 overrides** (`manager.cpp:185-195`,
  `expeditor.cpp:808-816`) is now cited.
- **The writer also recycles the log.**
- **The seams S1-S4 need** are named: synchronous `Step`/`FlushPages`, no
  clock in the checkpointer, one body in the checkpointer, the audit's
  core-0-only pin half, and `CurrentCoreGuard`.
- **Tags** were added where claims had none or had the wrong kind.
- **`SYNC`'s citation** moved to `HandleSync`.
- **Cuts:**
  - the first draft's BI-Q6 (`pages_per_step`) folded into the premise gate, and the items
    renumbered;
  - S4's lock-list cell moved to S5's text;
  - S2's "assertion recovery cells green" dropped as a duplicate of the
    suite;
  - BI-Q5 (b) cites BA-R10's argument rather than re-deriving it.
- **One finding went past the order**: BI-Q3 (a) as first written, sticky
  failure with the log running, does not close the runtime case. Frames
  cleaned before a failed `fsync` can be evicted and read back stale. That
  defect predates BI and is now
  `docs/inflight/bugs/a-failed-data-file-fsync-leaves-its-pages-clean.md`.
  BI-Q3 is restated around it.

**Declined:**
- **Deleting the measurement item** (now BI-Q11). Every order carries one
  (BG-Q8, BE-Q8), and the review of BG-S0 kept the same shape.
- **Merging BI-Q8 into BI-R1.** The rule states the reading, and the item
  asks the operator to confirm it. That is the house shape, in which §0,
  §2 and §4 each state a rule once.
