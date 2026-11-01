# Work order CJ — wait events and ASH (Active Session History)

Written 2026-10-31 in the main checkout on `main` at `3f225096`
(`v3.4.6-5-g3f225096`), no worktree (documents only), from two documents the
operator pasted into the session, translated into English here (nothing
Korean enters the tree): the spec "Wait Events & ASH" and its workplan. The
operator chose "work order" as their form (the session's question of
2026-10-31). The operator's words are recorded in `raft-marks-2026-10-31.md`
§5.

**Opened 2026-10-31** on the operator's standing go-ahead
(go-ahead-achieving-milestone, `raft-marks-2026-10-31.md` §6) on
`worktree-cj-wait-events-ash` from `ed32c10d`. CJ-Q0..Q7 are adopted as CLA
proposed, and CJ-Q8 (below) was added when WE03 found a premise false; none
is the operator's own per-item mark. **Stages WE01-WE13 and gates G1, G2 landed
2026-11-01 on `worktree-cj-wait-events-ash` (closing commit `f17bdf81`,
`v3.4.6-16-gf17bdf81`); WE14 stays reserved. Not merged, not pushed. Close
measurement and its reading: stage log §8, "The close".** CJ-Q13, CJ-Q14 and
CJ-Q15 were added and adopted on the standing go-ahead
(`raft-marks-2026-11-01.md` §1).

**(Written as: not opened. Nothing was built at writing.)** No stage has started, no
measurement has been taken, and no claim below about the tree is
`[source-read]` unless it says so: the pasted documents were written
against a tree this order has not yet read (§3 lists what it must read
first).

**Claim tags**: `[operator]` for the pasted spec's decisions WD1-WD13;
`[design]` for the spec's design; `[PROPOSED]` where the pasted spec says so
itself (WD7's 4 MiB, the 32 buckets, ring 1-64 MiB, the `arg_hash` placement,
the ±5 % and ±3 points tolerances); `[CLA]` for what this order adds beyond
the pasted documents.

## 0. What CJ is

An Oracle-style wait-event model and a per-core sampled history for KDS,
answering two questions from data:

- why was this statement slow (a per-statement DB-time breakdown);
- what did the system wait for in a given window (time-axis samples of
  active sessions).

**In v1**: an event registry with classes; per-task current-state recording
and cumulative wait statistics; a per-core ASH sampler into an in-memory
ring; `sys.*` views and an `ANALYZE` line.

**Not in v1**: a durable history (AWR's analogue, reserved as WE14); per
function timing (the development-mode profiler's job); automatic diagnosis
(ADDM's analogue).

**Boundary with the development-mode profiler.**

| | wait events / ASH | development profiler |
|---|---|---|
| build | in production, always compiled | development mode only |
| unit | a suspension point; a task's state | cumulative time per function or stage |
| question | what was waited for | which code used the CPU |
| overhead | fixed by §6 | unbounded |

## 1. Decisions `[operator]`

The pasted spec states them as settled on 2026-10-31. They are recorded as
the operator's, not re-opened here.

| ID | Decision |
|---|---|
| WD1 | Cross-core attribution: the originating session records only `xcore.*` waits. The remote core measures the step as a task of its own and tags it `origin_session`. Remote CPU and waits are never summed into the originating session's DB time |
| WD2 | Only the event name (`class.event`) is the public contract. A numeric `event_id` is internal and not stable across versions |
| WD3 | Time is `clock_gettime(CLOCK_MONOTONIC)` (vDSO) behind a clock seam; tests use a logical clock; a TSC replacement waits for a measurement |
| WD4 | Cumulative statistics are always on and cannot be turned off. ASH is on by default with a runtime kill switch |
| WD5 | Wait-time distributions are fixed log2-bucket histograms |
| WD6 | ASH interval defaults to 1 s, settable 100 ms-10 s. Only active tasks are sampled (IDLE excluded) |
| WD7 | A ring is fixed memory per core: default 4 MiB = 64 B x 65,536 records `[PROPOSED]` |
| WD8 | v1 is memory only. A crash or restart starts empty. A durable history is reserved as WE14 |
| WD9 | RUNNABLE (waiting in the run queue) is measured as `sched.run_queue` |
| WD10 | A fail-fast conflict that does not wait is recorded as a zero-time, counter-only event |
| WD11 | ASH records the fingerprint only by default. The argument hash (`InstanceKey`'s `arg_hash`) is opt-in. Bind values are never recorded under any setting |
| WD12 | Overhead budget: P1 throughput loss <= 0.8 %, ASH adds <= 0.5 % (PK single-row read bench). The sum of a statement's DB-time parts differs from its elapsed time by <= 1 % |
| WD13 | Background tasks (checkpointer, writeback, Cabin builder and the like) are in both the cumulative statistics and ASH, told apart by a session-kind bit |

## 2. The design `[design]`

### 2.1 What a wait is

A wait is not an OS thread blocking. It is a task suspended in the
cooperative event loop. Core threads are not preempted, so every suspension
point records the task's state itself.

| State | Meaning | DB time |
|---|---|---|
| `ON_CPU` | the core thread is running this task | included |
| `RUNNABLE` | can resume, waiting its turn in the core's run queue | included (`sched.run_queue`) |
| `WAITING` | waiting for an event to complete | included, except `idle.*` |
| `IDLE` | waiting for the client's next request | excluded |

### 2.2 Invariants

- **I1 sole owner.** A task's state word is written and read by its owning
  core only, with no atomic operation. Another core reads it only through a
  cross-core step.
- **I2 instrumentation is complete.** Every suspension of a task happens
  inside a `WaitScope`. A development build asserts on a violation.
- **I3 DB time is conserved.** Per statement, `DB time = ON_CPU + RUNNABLE
  + sum of WAITING(e)` over non-idle events, and its difference from the
  statement's elapsed time is <= 1 % (WD12).
- **I4 no double counting.** One interval of time belongs to one task in one
  state (WD1).
- **I5 non-authoritative.** Wait statistics and ASH are volatile
  observations: not in the WAL, not recovered. This is invariant 8's shape
  (advisory, droppable without changing a result).

### 2.3 Cross-core steps

The requesting core's session waits in `xcore.step_response`, or in
`xcore.credit` when out of credits. The remote core measures the step as a
separate task recording `origin_session` and `flags.cross_core`. A combined
view is joined on `origin_session` at query time.

### 2.4 The event registry

A compile-time table: `name` (`class.event`), `class`, the meanings of
`p1`-`p3`, and `kind` (`timed` or `counter`). `event_id` is the table index
(u16); 0 is the reserved `ON_CPU`. No runtime registration.

| Class | Event | p1 / p2 / p3 | kind |
|---|---|---|---|
| Commit | `commit.wal_flush` | target LSN / durability class / group size | timed |
| User I/O | `io.page_read` | page_id / page class / core | timed |
| User I/O | `io.var_heap_read` | page_id / rel_oid / - | timed |
| User I/O | `io.scan_ring_read` | page_id / rel_oid / - | timed |
| System I/O | `sysio.writeback` | page_id / bytes / - | timed |
| System I/O | `sysio.checkpoint` | LSN / bytes / - | timed |
| System I/O | `sysio.wal_write` | LSN / bytes / - | timed |
| CrossCore | `xcore.step_response` | target core / step_id / - | timed |
| CrossCore | `xcore.credit` | target core / credits left / - | timed |
| Scheduler | `sched.run_queue` | scheduling group / queue length / - | timed |
| Resource | `res.eviction_retry` | retry count / pool use / - | timed |
| Network | `net.send_backpressure` | session / unsent bytes / - | timed |
| Application | `app.lock_conflict` | rel_oid / pk / holding trx | counter |
| Application | `app.assertion_reservation` | assertion id / group key hash / - | counter |
| Idle | `idle.client` | - | timed (excluded from DB time) |
| Idle | `idle.background` | - | timed (excluded from DB time) |

The Concurrency class (latch and pin contention) is reserved only: with
core-owned structures v1 has no place that raises it. `[OPEN]` (the spec's):
the attribution of a background writeback and a foreground page read that
share one I/O queue waits on how the `IoBackend` tags a request; it is
settled in WE04.

### 2.5 `WaitScope`

An RAII guard (the real signature is fixed in WE02). Construction sets
`state = WAITING` and records the event, its parameters and the start time;
destruction adds the elapsed time to the per-core and per-session statistics
and the histogram and returns the state to `ON_CPU`. No nesting (a
development build asserts). A `counter` event goes through
`record_counter_event(...)`, which only raises a count.

### 2.6 Cumulative statistics

| Unit | Items |
|---|---|
| core x event | count, total (us), max (us), log2 histogram |
| session x event | count, total (us) |
| session | ON_CPU, RUNNABLE, wait total (us) |

Histogram bucket k is [2^k, 2^(k+1)) us, 32 buckets `[PROPOSED]`, no
allocation. Session statistics are dropped when the session ends; core
statistics accumulate for the process.

### 2.7 ASH

1. Each event-loop turn checks the next sample time; when reached, it walks
   the core's active-task table.
2. Each non-IDLE task writes one record into the ring, by per-field `memcpy`
   helpers (no `reinterpret_cast` overlay).
3. A long step that runs past the sample time yields one record at the real
   time with `flags.late`; nothing is interpolated.
4. The next sample time is `previous scheduled time + interval`, so it does
   not drift.

**Record (64 B, little-endian).**

| Offset | Field | Size | Content |
|---|---|---|---|
| 0 | `sample_ts` | 8 | sample time (us) |
| 8 | `session_id` | 4 | session or background task id |
| 12 | `core_id` | 2 | owning core |
| 14 | `state` | 1 | low 2 bits the state, bit 7 background |
| 15 | `wait_class` | 1 | class |
| 16 | `event_id` | 2 | event (`ON_CPU` = 0) |
| 18 | `sched_group` | 1 | scheduling group |
| 19 | `flags` | 1 | bit 0 `late`, bit 1 `cross_core`, bit 2 `arg_hash_valid` |
| 20 | `fingerprint` | 8 | statement fingerprint (the same value the pattern system uses) |
| 28 | `step_id` | 4 | current step in the step chain |
| 32 | `origin_session` | 4 | originating session of a cross-core step (0 if none) |
| 36 | `trx_id` | 8 | transaction id (low 48 bits) |
| 44 | `p1` | 8 | event parameter 1 |
| 52 | `p2` | 4 | event parameter 2 |
| 56 | `p3` | 4 | event parameter 3 |
| 60 | `wait_elapsed_us` | 4 | how long the current wait has run (saturating) |

WD11's opt-in has no field of its own, to keep 64 B. `[PROPOSED]` An `ON_CPU`
or `RUNNABLE` sample has empty event parameters, so it carries `arg_hash` in
`p1` with `flags.arg_hash_valid` set; a `WAITING` sample keeps the event
parameters and carries no `arg_hash`.

**Ring.** One fixed circular buffer per core, overwriting, 4 MiB by default.
Retention falls with the number of active tasks (20 active tasks per core at
1 s keeps 65,536 / 20, about 54 minutes). Only the owning core writes; a
query is a cross-core read step that copies a snapshot on the owning core.

### 2.8 Surface

| View | Content |
|---|---|
| `sys.wait_event_names` | event_id, name, class, kind, meanings of p1-p3 |
| `sys.wait_stats` | core x event statistics (plus a total row) |
| `sys.wait_histogram` | core x event x bucket |
| `sys.session_waits` | session x event, cumulative |
| `sys.session_state` | each active task's state, event, parameters, wait elapsed (Oracle's `v$session_wait`) |
| `sys.ash` | ASH samples, filterable by time range, core, fingerprint, event |

Read-only; snapshot-time consistency only, with no exact cross-core
simultaneity. Events are exposed by name (WD2).

| Key | Default | Range | Runtime change |
|---|---|---|---|
| `kds.ash.enabled` | on | on / off | yes |
| `kds.ash.interval_ms` | 1000 | 100-10000 | yes |
| `kds.ash.ring_bytes_per_core` | 4 MiB | `[PROPOSED]` 1-64 MiB | restart |
| `kds.ash.capture_arg_hash` | off | on / off | yes |

Cumulative statistics have no key (WD4). The kill switch stops the sampler
only and keeps the ring; turning it on again continues the ring. State is
process memory: a restart begins at zero (WD8, I5).

`ANALYZE` gains one per-statement line, for example:

```
DB time 4.21 ms = CPU 1.02 + run_queue 0.11 + commit.wal_flush 2.95 + io.page_read 0.13
```

A cross-core step's remote CPU and waits print on a separate line, joined on
`origin_session`, and are not in that sum (WD1).

## 3. The tree CJ must read first `[CLA]`

The pasted documents name structures this order has not checked. Each is a
reading task of CJ-S0 (§5), not a finding:

- **The scheduler and its suspension points** (`docs/spec/sched.md` §3, §4):
  the task representation (C++20 stackless coroutines), the run queue, the
  per-scheduling-group accounting already in `sys.meta`, and
  `sched::CoreLoad`. I2 needs a census of every `co_await` and every
  park-and-kick site (commit parks, `CommitDeferred`, lock waits on slots,
  streaming-reply parks, yielding builds, the cross-core wake registry).
- **`sys` views** (`docs/spec/sys-views.md`, SV1-SV5): the new views must
  follow that contract; `sys.statement_stats`, `sys.lock_waits` and
  `sys.checkpoints` are memory-resident, per-core parts that already exist
  and overlap in part (§4, CJ-Q3).
- **The I/O path**: whether `IoBackend` can tag a request with its task, and
  whether a page read is a suspension today or a synchronous call on the
  reactor (`known-gaps.md` notes some `pwrite`s stay on the reactor; a
  synchronous call is not a suspension and has no `WaitScope`).
- **The transaction-lifetime, deadline and cancel paths** that end a task
  from outside.

## 4. Questions - not marked

`[CLA]`: these are what the pasted documents do not settle against the
tree. Each carries CLA's proposal; none is the operator's mark.

- **CJ-Q0 - the letter and the opening.** Written as CJ, the next letter
  after CI (CH is already taken). Whether and when to open it is the
  operator's word. *CLA proposes: leave it unopened until the word.*
- **CJ-Q1 - a census before WE02.** The workplan has no premise stage, and
  the repository's practice (CA-S0, CI-S0) is to measure the premise first.
  Two premises carry the budget: that a `WaitScope` and a clock read per
  suspension fit in 0.8 % on the PK read path, and that I2 is satisfiable
  (the count and kind of suspension points). *(a) CJ-S0, a census within the
  20-minute stage budget: count suspension points, and time a `CLOCK_MONOTONIC`
  read and a state-word write in `build-release`; (b) none, WE02 starts
  directly.* **CLA proposes (a).**
- **CJ-Q2 - the P1 measurement against the time budget.** The workplan
  requires >= 5 alternating on/off runs, median, same machine and commit
  (`bench/README.md`'s rules also apply). The standing budget is 20 minutes
  per stage and about 1 hour per milestone, building included. The G1 and G2
  overhead cells must be planned to fit, and the results file says what was
  cut. *Only the operator's word lifts the budget.*
- **CJ-Q3 - overlap with existing views.** `sys.statement_stats` and
  `sys.lock_waits` already record per-statement and lock-wait data, and
  `app.lock_conflict` is a counter of what `sys.lock_waits` shows. *(a) the
  new views stand beside them, `app.*` counters count refusals only;
  (b) a lock wait is also a `WaitScope` and `sys.lock_waits` reads from it.*
  **CLA proposes (a)** for v1, and revisits (b) at WE04 with the census.
- **CJ-Q4 - WE13 names a mover that is gone.** The pasted workplan says
  "physical-optimizer signal link". The mover, relayout planner and the
  `physical_optimizer` key were deleted in BZ; the nearest live consumer is
  the Cabin controller (`docs/spec/cabin-controller.md`), whose inputs are
  its own collector and the decay score. *(a) WE13 is re-aimed at the Cabin
  controller as an auxiliary input, still `[EXPERIMENTAL]` and sharing its
  `SET CABIN_OPTIMIZER` switch; (b) WE13 is dropped.* **CLA proposes (a)**,
  and it stays G2-gated either way.
- **CJ-Q5 - a document location that does not exist.** The pasted spec cites
  `docs/wait-events.md` and the workplan `docs/wait-events-workplan.md`.
  Neither exists, and `docs/spec/` holds only what is built. *This order
  carries the design (§2) until a stage builds it; each stage moves what it
  builds into `docs/spec/wait-events.md` (new) and `sys-views.md`.* No
  separate workplan file is kept; the stages are §5.
- **CJ-Q6 - the sampler runs on the event-loop turn.** A per-turn time check
  and a walk of the active tasks adds work to the OLTP path (WD12's 0.5 %).
  *(a) as written (the turn checks); (b) a `kMaintenance`/`system`-class
  tick takes the sample.* **CLA proposes (a)** as the spec says, to be
  re-priced in CJ-S0; (b) is the fallback if G2's overhead cell fails.
- **CJ-Q7 - unlogged and the superblock.** I5 and WD8 say nothing is logged.
  *CLA reads that as: no WAL kind, no superblock bump, no catalog row.* The
  views are computed, like `sys.meta`. Confirmed by CJ-S0 reading
  `sys-views.md`; a stage that needs a persisted word stops and asks.

- **CJ-Q8 - a page read is not a suspension (found at WE03, `[source-read]`
  `src/storage/device_page_store.cpp` `FaultIn`).** The spec reads a wait as
  a coroutine suspended in the loop (§2). A page fault is a blocking `pread`
  on the reactor thread: the core stops, no task is parked, and the whole
  core waits. *(a) a `WaitScope` around a synchronous call is allowed: it
  never suspends, its span is the event's and is taken out of ON_CPU (I4);
  (b) leave synchronous reads unmeasured until they are made suspensions.*
  **CLA proposes (a)** - adopted. It is the more useful measurement, since a
  blocking read stalls every task on the core, not only the one that
  faulted; a reader of `sys.wait_stats` must remember that such an event's
  time is the core's stall (stated in WE12's dictionary).

- **CJ-Q9 - the spec's 16 events leave four waits unclassified, and its
  cross-core class has no site (found at WE04, `[source-read]`).** I3 needs
  every DB-time wait to be an event. The tree parks on a lock or a write
  block (`command_dispatcher.cpp` 447, 681), an online index build's drain
  (2662), a `PURGE`'s horizon wait (841) and a session's ceiling wait on
  another core's commit marker (759), and the 16 events name none: the lock
  waits are `sys.lock_waits`' rows (CJ-Q3), and `app.lock_conflict` is a
  counter. Separately, `xcore.step_response` and `xcore.credit` have no
  site: the remote step park retired with AT-S9 (the dispatcher's own
  comment at 743: "never a stage on another core"). *(a) three timed events
  are added - `app.lock_wait`, `app.horizon_wait`, `commit.ceiling_wait` -
  and the two `xcore.*` events stay registered, reserved like Concurrency;
  (b) fold the lock waits into `app.lock_conflict` and make it timed;
  (c) leave them unscoped.* **CLA proposes (a)** - adopted. (b) changes a
  spec event's kind, (c) breaks I2 and I3. `app.lock_conflict` still counts
  the refusals (CJ-Q3 (a) stands: `sys.lock_waits` keeps its rows, and the
  wait is now also a scope).
- **CJ-Q10 - what an unscoped park does once `unclassified` is gone (found at
  WE04).** The order says "make I2 an assert". Unit tests and rigs park
  synthetic coroutines on a bare `WaitFor`, so an unconditional assert fails
  them. *(a) the assert fires where a core opts in
  (`SetStrictWaitScopes`), which `CoreRuntime::Run` and `Expeditor::RunUntilStopped` (core 0) do on their reactor threads,
  so each server and rig test that runs through a core is the census; a
  synthetic coroutine on a bare test scheduler is counted and booked to no
  event; (b) unconditional, and every synthetic test takes a scope or a
  guard; (c) keep the counter.* **CLA proposes (a)** - adopted.

## 5. Stages (the pasted workplan, translated)

Order:

```
P1  WE01 -> WE02 -> WE03 -> WE04
                       \-> WE05
    -- G1 ------------------------------
P2  WE06 -> WE07 -> WE08
                 \-> WE09
    -- G2 ------------------------------
P3  WE10 (after G1), WE12 (after WE05)
    WE11, WE13 (after G2)
P4  WE14 [RESERVED]
```

P1 can ship alone once G1 passes.

**Common rules** (the workplan's). Every commit message names the spec
section and the decision ID. Every later PR that adds a suspension point adds
its `WaitScope` (I2), and the review checklist carries that line. Time is
read only through the clock seam, never by a direct system-clock call in
instrumentation. A record is encoded only by per-field `memcpy` helpers. An
overhead measurement compares instrumented and compiled-out builds, same
machine, same commit, alternating, 5 or more runs, median. Every stage is
reviewed by `critics-developer` and gated by the full suite.

### P1 - the wait-event base

**WE01 - the event registry.** *(WD2)*
- Work: the compile-time table (§2.4) with `event_id` the index and 0 the
  reserved `ON_CPU`; all v1 events registered; `sys.wait_event_names`.
- Done when: a duplicate name or a missing class fails to compile
  (`static_assert`); the view returns every event by name.

**WE02 - the clock seam, task state and `WaitScope`.** *(WD3, WD9)*
- Work: a clock interface (`CLOCK_MONOTONIC` in production, a logical clock
  in tests); a state word in the task context (state, event_id, p1-p3,
  wait_start); a `RUNNABLE` transition recorded at run-queue entry and exit;
  the `WaitScope` guard and `record_counter_event()`; development-build
  asserts for a suspension outside a `WaitScope` and for nesting; a
  bench-only flag that compiles instrumentation out (never in a production
  build).
- Done when: a unit test on the logical clock records state transitions and
  elapsed time exactly; the compiled-out build matches the existing bench.

**WE03 - first instrumentation: Commit, User I/O, Scheduler.** *(WD9)*
- Work: `commit.wal_flush` (group size in `p3`); `io.page_read`,
  `io.var_heap_read`, `io.scan_ring_read`; `sched.run_queue` joined to
  WE02's transitions.
- Done when: with an injected `IoBackend` fsync delay the mean of
  `commit.wal_flush` is within +-5 % of the injected value `[PROPOSED
  tolerance]`; an injected page-read delay shows in `io.page_read` only;
  the existing suite has 0 I2 assert violations, any suspension point not
  yet classified wrapped in a temporary `unclassified` event and listed.

**WE04 - second instrumentation: CrossCore, Resource, Network, System I/O,
Application.** *(WD1, WD10, WD13)*
- Work: `xcore.step_response`, `xcore.credit`, and the remote step task's
  `origin_session` and `cross_core` tags; `res.eviction_retry`;
  `net.send_backpressure`, `idle.client`; background-task registration with
  `sysio.*`, `idle.background` and the session-kind bit; the `app.*`
  counters; every `unclassified` point from WE03 removed.
- Done when: no `unclassified` event and 0 I2 violations; in a cross-core
  join simulation remote time is not added to the originating session's DB
  time (I4). `[OPEN]` the shared-queue attribution of §2.4 is settled here
  after reading the `IoBackend` tagging.

**WE05 - cumulative statistics and views.** *(WD4, WD5)*
- Work: core x event count, total, max and a 32-bucket log2 histogram;
  session x event and session CPU, RUNNABLE and wait totals; the views
  `sys.wait_stats`, `sys.wait_histogram`, `sys.session_waits`,
  `sys.session_state`; per-core statistics merged by a cross-core read step.
- Done when: over all simulated statements the I3 error is <= 1 %; a
  session's statistics are dropped at its end with no memory growth.

**G1 gate.**

| Check | Bar |
|---|---|
| I2 completeness | 0 assert violations over the whole suite and simulation |
| I3 DB time | per-statement error <= 1 % |
| Overhead | PK single-row read throughput loss <= 0.8 % against the compiled-out build |
| Determinism | the same seed gives identical cumulative statistics |

### P2 - ASH

**WE06 - the ring and the record codec.** *(WD7)*
- Work: a fixed per-core ring (`kds.ash.ring_bytes_per_core`); a 64 B
  encoder and decoder at §2.7's offsets.
- Done when: an encode-decode round trip and a wrap-around boundary test
  pass; `static_assert` the record size is 64.

**WE07 - the sampler.** *(WD6, WD11, WD13)*
- Work: the per-turn sample-time check and active-task walk; `late` and the
  drift-free next time; background tasks in, IDLE out; the `capture_arg_hash`
  opt-in path (§2.7's `[PROPOSED]` rule).
- Done when: on the logical clock the sample count equals elapsed time over
  the interval (exactly, `late` aside); an injected long step shows `late`
  and one record.

**WE08 - `sys.ash`.** *(WD1 at query time, WD2)*
- Work: a read-only cross-core step collecting each core's ring snapshot,
  merged in time order; filters on time range, core, fingerprint, event name
  and session kind.
- Done when: merged output is monotone in time; a query during sampling is
  not corrupted.

**WE09 - settings and the kill switch.** *(WD4, WD6)*
- Work: runtime change of `kds.ash.enabled`, `interval_ms`,
  `capture_arg_hash`; range checks and error codes.
- Done when: an off-to-on change keeps the ring and continues; an out-of-range
  value is refused.

**G2 gate.**

| Check | Bar |
|---|---|
| ASH distribution | the injected-delay share matches the per-class sample share within +-3 points on >= 10,000 samples `[PROPOSED]` |
| Determinism | the same seed and logical clock give a byte-identical `sys.ash` |
| Overhead | ASH on vs off throughput difference <= 0.5 % |
| Boundaries | wrap-around, `late`, merge order and kill-switch tests pass |
| Volatility | after a crash and restart the instance starts empty and healthy |

### P3 - integration

- **WE10 - `ANALYZE` (after G1).** The DB-time line and the separate
  cross-core line (§2.8). Done when the `ANALYZE` total equals the
  `sys.session_waits` delta.
- **WE11 - KDS Studio panel (after G2).** Active sessions over time (stacked
  by class), top events, top fingerprints, a fingerprint-to-event drill-down.
  Data sources are `sys.ash` and `sys.wait_stats` only; no new protocol
  frame.
- **WE12 - event dictionary (after WE05).** Per event: meaning, `p1`-`p3`,
  usual causes, remedies; a test that catches a mismatch between the
  registry and the documented list.
- **WE13 - Cabin controller signal `[EXPERIMENTAL]` (after G2; CJ-Q4).** ASH
  fingerprint activity as an auxiliary input. It must not break the existing
  deterministic replay tests and shares the controller's switch.

### P4 - reserved

- **WE14 - a durable history `[RESERVED]`.** AWR's analogue. Storage format,
  retention, size control and whether it is an unlogged class are separate
  decisions taken before it starts. Needs G2.

## 6. Overhead budget and verification

| Item | Budget | Measured against |
|---|---|---|
| P1 (cumulative statistics + state recording) | throughput loss <= 0.8 % | PK single-row read bench, against the compiled-out build |
| ASH addition | throughput loss <= 0.5 % | the same bench, ASH on vs off |
| DB-time accuracy | per-statement error <= 1 % | every statement of the simulated workload |

Measured in `build-release` (never Debug), with the interleaved A/B the
repository requires, inside the time budget of CJ-Q2. The bench-only flag
that compiles instrumentation out is not used in production builds.

## 7. Out of scope

A durable history (WE14); function-level timing; automatic diagnosis; a
runtime event registry; any WAL kind, superblock bump or catalog row for
wait data (CJ-Q7); the Concurrency class's events (reserved, no v1 site).

## 8. Stage log

**Adopted on the operator's standing go-ahead of 2026-10-31
(go-ahead-achieving-milestone), CLA's proposal in each case:** CJ-Q0 (left
unopened until the word - the word is the go-ahead, so opened), CJ-Q1 (a)
(a census, folded into this log's CJ-S0 entry), CJ-Q2 (the budget stands),
CJ-Q3 (a) (the new views stand beside the old), CJ-Q4 (a) (WE13 re-aimed at
the Cabin controller), CJ-Q5 (this order carries the design), CJ-Q6 (a) (the
sampler on the loop turn), CJ-Q7 (nothing logged or persisted), CJ-Q8 (a).

**CJ-S0 - the census** (`[source-read]`, `ed32c10d`).
- Every suspension in the tree is one of three awaitables in
  `include/kds/sched/coro.hpp` - `Yield`, `WaitFor`, `WaitUntil` - resumed
  from one place, `CoroTask::Poll`. That is what makes I2 checkable: the
  bracket goes around the one resume site and the scope around each
  `co_await`.
- `co_await` sites outside the coroutine plumbing: `src/exec/step_vm.cpp`
  (the sliced walk's `Yield` / `WaitUntil` at 2110-2112, the rest are
  child-step awaits), `src/server/command_dispatcher.cpp` (lock-wait,
  relation-lock and write-block parks, the group-commit park, DDL and drain
  parks, `Yield`s), `src/wal/checkpointer.cpp` (log and data-sync parks),
  `src/server/tcp_server.cpp:470`.
- Page reads, WAL appends and many `pwrite`s are synchronous (CJ-Q8).
- The clock: `sched::Clock` already exists (`SystemClock` is
  `steady_clock`, `CLOCK_MONOTONIC`); WD3 is met by reusing it. The
  scheduler reads its own `clock_` around every poll. WE02 keeps a
  per-thread `SetWaitClock` beside it rather than threading the
  scheduler's clock into `sched/coro.hpp`; **unifying the two is open** and
  belongs to G1's overhead cell (four clock reads per resuming poll instead
  of two).
- The overhead cost of the clock read was **not measured** (no time-boxed
  `build-release` census was run); G1 carries it.

**WE01 - the event registry** (`include/kds/stats/wait_event.hpp`,
`sys.wait_event_names`, `docs/spec/sys-views.md` §6.5). Built. The table has
the 16 events of §2.4, the reserved `cpu.on_cpu` (id 0) and the temporary
`sched.unclassified`, 18 rows. `static_assert`s refuse an id that is not its
index, a name that is not `<class prefix>.<event>`, a duplicate name, and a
DB-time flag that is not "all but idle". `cpu` is a class prefix this order
adds for id 0.

**WE02 - the seam, the state, the scope** (`include/kds/sched/wait_state.hpp`,
`src/sched/wait_state.cpp`, hooks in `CoroTask::Poll`). Built.
- **I2 is a counter, not an assert, until WE04.** An unscoped park is booked
  as `sched.unclassified` and counted in `CoreWaitContext::unscoped_parks`;
  every existing wait is unscoped, so an assert now would fail the suite.
  WE04 turns the counter into an assert. Nesting is an `assert`.
- ON_CPU is a segment timer: a `WaitScope` ends the segment on opening and a
  new one starts on closing, so a blocking scope's time is the event's alone.
- `KDS_WAIT_INSTRUMENTATION_OFF` compiles the hooks out; no CMake option sets
  it yet (the P1 measurement adds one).

**WE03 - first sites, partial.** `commit.wal_flush` around the group-commit
park (`command_dispatcher.cpp`), and `io.page_read` / `io.scan_ring_read` /
`io.var_heap_read` around `FaultIn`'s `pread` (a var-heap page is named from
the bytes read). `sched.run_queue` is WE02's transition. **Not done:**
- `commit.wal_flush`'s group size (`p3`) is 0: the writer keeps no count of
  the commits one sync covers.
- The other commit parks were not read at WE03. **Read at WE04: they are not
  parks.** Each (`command_dispatcher.cpp` 5410, 10766, 11688, 12141 and
  `DeferCommit`) only sets `pending_lsn`, and the one park that waits on it
  is the scoped one at the statement's tail, so WE03's scope covers them all.
- `io.page_read`'s `p2` (page class) and `p3` (core) are 0.
- The acceptance tests with injected IoBackend latency are not written; the
  unit tests are on a logical clock.
- The census of `sched.unclassified` over the suite: taken at WE04.

**Reviews.** `critics-developer` on WE01-WE03 (`ed32c10d` plus the working
tree): two I4 defects applied - a scope destroyed outside its task's poll
charged the running task's ON_CPU time, and a yield inside a scope was booked
as run-queue time too - each with a test; the segment timer replaced the
poll-span arithmetic (removing three fields); the scope's start is the
task's `since_ns`; the OFF build no longer reads the clock in the task
constructor. **Rejected:** unifying the scheduler's clock now (S3 - see the
census; it needs a read of `scheduler.cpp` and belongs with G1's
measurement); dropping `ParkedOnAWait` for a hoisted bool (the helper is
three lines and keeps `Poll`'s loop unchanged).
The review also notes that the shared pool makes a core waiting on another
core's in-flight fault look ON_CPU, which contradicts §2.4's "no v1 site for
the Concurrency class"; recorded as a known gap for the operator.

**Suite.** 4348/4348 passed (4 disabled or skipped) before the review's fixes
(`-LE bs-nightly`, `-j8`, Debug, `ed32c10d` plus the WE01-WE03 tree); re-run
after them: 4350/4350 passed (4 disabled or skipped), same command, at the tree committed as WE01-WE03.

**WE04 - second instrumentation** (on `worktree-cj-wait-events-ash` from
`c5c2edc1`). Adopts CJ-Q9 and CJ-Q10 above.
- **Scopes added.** `app.lock_wait` at the write-block wait, the relation-lock
  wait and the online-build drain; `app.horizon_wait` at the `PURGE` horizon
  wait; `commit.ceiling_wait` at the acknowledged-commit ceiling wait;
  `sysio.checkpoint` at the checkpoint's data-sync park and `sysio.wal_write`
  at its log park; `net.send_backpressure` at the sliced walk's
  `resume_when` park; `idle.background` at the connection-handoff host task.
- **Counters.** `app.lock_conflict` on each refused ask in `LockTable::Acquire`
  (the in-latch exit and the fence exit); `app.assertion_reservation` where a
  refused admission names a reserving transaction.
- **`sched.unclassified` is deleted.** The registry is 20 rows. An unscoped
  park is counted in `unscoped_parks`, booked to no event, and an `assert`
  on a core that set `SetStrictWaitScopes` (CJ-Q10).
- **No site, stated rather than invented.** `xcore.step_response` and
  `xcore.credit` (CJ-Q9); `res.eviction_retry` (the pool's retry is a
  bounded synchronous loop on the reactor, `device_page_store.cpp` ~860
  `kRefuseRetries`, not a suspension, and it can run inside a `FaultIn`
  whose `io.*` scope is open - a second scope there would nest, which §2.5
  forbids; left as ON_CPU time, open for WE05's review); `idle.client`
  (a session is not a task while it waits for its client: the read is an
  event callback, so no task is suspended). Their rows stay in the registry.
- **`[OPEN]` the shared-queue attribution of §2.4** is settled by finding no
  shared I/O queue: page reads are a blocking `pread` on the reactor
  (CJ-Q8), a writeback is the checkpoint's `pwrite` on the same thread, and
  the log and data syncs run on their own threads and are waited through
  `ParkOnLog` / `ParkOnDataSyncTicket`, each its own scoped park. No request
  is tagged, so no attribution is needed.
- **Review (`critics-developer`).** Applied: core 0's reactor was not strict
  (it is an `Expeditor`, not a `CoreRuntime`) - `RunUntilStopped` now sets it;
  `sysio.checkpoint`'s p1 was the sync ticket where the registry says lsn -
  now 0. **Rejected:** collapsing the ~25 rig-idle blocks into a `RigIdle`
  helper (a 16-file test rebuild on a full disk for a cosmetic gain; open for
  a later tidy). **Not done, recorded:** `net.send_backpressure` carries no
  session or unsent-bytes; `app.lock_wait` p1 can be a borrow id or 0 at the
  relation-lock site; `app.horizon_wait` p1 is a trx id even when it waits on
  an epoch; an admission refused by another transaction's reservation is
  booked as `app.lock_wait` (it goes through the write block) beside the
  `app.assertion_reservation` count; an unscoped park leaves WAITING on
  `cpu.on_cpu`, to decide before WE06's sampler; `sim/` drives no core
  runtime, so G1's "suite and simulation" is the suite only so far.
- **Suite.** The run before the review's two edits: 4349 passed, 1 failed -
  `DropTableReclaimCrashTest.AReclaimCutAnywhereIsFinishedByTheNextMount`,
  `No space left on device` copying a volume under `/tmp` (a tmpfs with 3 GB
  free), an environment limit and not a wait fault. Re-run below.
  Re-run after the review edits (ctest -j8 -LE bs-nightly, Debug): 4350 of
  4351 passed, the same one failing for the same ENOSPC reason. That cell is
  not a pass; it passed at c5c2edc1 with room on /tmp.

**WE05 - cumulative statistics and views** (from `7fdf3741`).
- **Built.** `EventTotals` gains `max_ns` and a 32-bucket log2 histogram
  (`WaitBucketOf`: bucket k is [2^k, 2^(k+1)) us); every counter is a
  `RelaxedU64` (one writer, relaxed load/store, readable by any core). A core's
  counters live in a static slot per core id (`CoreWaitTotals`, bound by
  `BindWaitContext` in `CoreRuntime::Run` and `Expeditor::RunUntilStopped`);
  everything else a thread mutates is in its own constant-initialised
  thread-local `CoreWaitContext`. `SessionWaitStats` (count and total per
  event, ON_CPU, the published task state) is owned by `Session`, registered
  in a mutex-guarded table and dropped with its last owner; `SubmitStatement`
  points the statement's task at it before the task is first polled. Views
  `sys.wait_stats` (per core and event, plus NULL-core total rows),
  `sys.wait_histogram`, `sys.session_waits`, `sys.session_state`; the
  "cross-core read step" is each view reading every bound core's words, not a
  message to the owning core (snapshot-time consistency, which §2.8 allows).
- **Conservation (I3)** is tested at the task level on the logical clock
  (ON_CPU + events == elapsed, session figures equal the core's). A figure over
  "all simulated statements" is not taken: `sim/` drives no core runtime.
- **Review (`critics-developer`).** Applied: the session lost its first queue
  wait (the pointer was set inside `DispatchAsync`, after the wait was booked;
  now set at submission); per-hook TLS init guard removed and `Ctx()` read once
  per hook (G1); `bound` made atomic; a thread-private/shared split so two
  threads bound to one slot lose counts rather than corrupt each other's
  tasks (finding A); a finished session publishes IDLE `idle.client` instead of
  a growing ON_CPU (B); unused `current_core` deleted (D); `WaitBucketOf` by
  `bit_width`; the event-index clamp deleted. **Rejected / open:** `session_id`
  is a private counter, not the KWP handshake's id (C) - joining them needs the
  protocol identity, which `Session` does not hold; `MakeSessionWaitStats`
  keeps its custom deleter (the proposed derived-class form needs the same
  registration and is not shorter); `TaskStateName` stays in `sys_views.cpp`
  until WE08 needs it. Rig tests that drive `DispatchAsync` directly book to no
  session.

**WE06 - the ring and the record codec** (`include/kds/stats/ash_record.hpp`,
`ash_ring.hpp`, `tests/ash_ring_test.cpp`). Built: the 64-byte record at
§2.7's offsets by per-field little-endian puts (no overlay), `static_assert`s
on the size and the offsets, a trx id stored as its low 48 bits; `AshRing`, a
byte-sized overwriting ring behind a `kStatsRing` latch, allocated on the
first record. Round-trip, offset and wrap-boundary tests pass. The ring is not
yet owned per core or reachable from a view (WE07, WE08).

**Suite** at the tree committed as WE05-WE06 (`ctest -j8 -LE bs-nightly`,
Debug): 4360 of 4361 passed; the one failure is the same `ENOSPC` cell as
above, not a pass.

**WE07 - the sampler** (from `db2696fe`). Adopts CJ-Q11 and CJ-Q12 below, on
the operator's standing go-ahead of 2026-10-31.
- **CJ-Q11 - what `late` means.** §2.7 says a long step yields one record
  "with `flags.late`" and fixes no threshold. *(a) late when the turn came more
  than a tenth of the interval after its due time; (b) late on any lateness;
  (c) late after a whole interval.* **CLA proposes (a):** (b) flags nearly every
  record on a real clock, (c) misses the stalls the flag is for. Adopted.
- **CJ-Q12 - ON_CPU is never sampled (found in review).** The sampler runs
  between polls, where no task is ON_CPU: `WaitOnPollExit` leaves each one
  RUNNABLE, WAITING or IDLE. So CPU time shows in `sys.ash` as RUNNABLE with a
  near-zero elapsed time. *(a) the late batch also records the task polled last
  as ON_CPU; (b) state that a between-polls sampler cannot observe ON_CPU and
  read RUNNABLE as "on CPU or queued".* **CLA proposes (b):** (a) invents a
  sample for a task that was not running at the sample time, and §2.7 says
  nothing is interpolated. Adopted. **Open for the operator:** this makes
  `sys.ash` weaker than Oracle's ASH for CPU attribution; `sys.wait_stats`'
  `cpu.on_cpu` rows are the exact figure.
- **Built.** `AshSampler` per core, owned by `Scheduler` and asked once per
  turn (`Due`, one relaxed load and a compare when not due); the walk of the
  ready queues through a new `Task::wait_state_for_sampling()`; the drift-free
  grid (a turn after several intervals writes one batch and moves to the first
  grid point after it); IDLE out, background tasks in with the background bit
  and a per-core number; `arg_hash` in `p1` of a non-WAITING sample when opted
  in; the dispatcher stores the statement's `fetch_id` as the session's
  fingerprint.
- **Not done.** `trx_id`, `step_id`, `origin_session` and `flags.cross_core`
  are always 0. `arg_hash` has no writer (`DispatchOutcome` carries only
  `fetch_id`; the value exists at the fingerprinting site), so
  `capture_arg_hash` does nothing yet outside the unit test. The interval is
  not range-checked (WE09), and a reactor's idle block is not capped at the
  next sample time (it overshoots by at most `max_idle_block_ms`, 10 ms
  by default, which at a 100 ms interval is exactly the late threshold).
  `turn_ns` is read before the turn's handlers, so a long handler makes a
  sample's time slightly early.
- **Review (`critics-developer`).** Applied: the fingerprint was left over from
  the previous statement for a task's first samples - cleared at the statement's
  entry (a sample between submission and first poll can still see the old one);
  a stale comment; `ash()` moved into the scheduler's public section.
  **Rejected:** merging `Due` and `Begin` (the test drives a batch by hand).

**Suite** at the tree committed as WE07 (`ctest -j8 -LE bs-nightly`, Debug):
4365 of 4366 passed; the one failure is the `ENOSPC` cell noted above, not a
pass.

**WE08 - `sys.ash`** (from `37a8b1c9`).
- **Built.** A process-wide registry of the per-core rings (`RegisterAshRing`,
  `AshRingOfCore`), filled by `AshSampler::set_core_id` and cleared by the
  sampler's destructor only if the slot still holds its own ring. `sys.ash`
  copies each ring under its own latch and merges by `sample_ts_us`, ties by
  core, then ring order. Filters are a `WHERE` on `sample_ts_us`, `core`,
  `fingerprint`, `event` and `session_kind` (`session` / `background`).
  Columns also carry `state`, `class`, `sched_group`, `late`, `arg_hash_valid`
  (1 = `p1` is the argument hash), `p1`-`p3`, `wait_elapsed_us`.
- **Not shown.** `step_id`, `origin_session` and `trx_id` (no writer yet).
  `sample_ts_us` is the instance's monotonic clock, not wall time.
- **Review (`critics-developer`).** Found and fixed: at `cores = 1` core 0 never
  called `AttachWakerTable`, so its ring was never registered and `sys.ash` was
  empty - `Expeditor::Start` now sets core 0's id. Applied: the `arg_hash_valid`
  column. **Not applied:** a `CoreRuntime`/expeditor-level test of the one-core
  registration (no rig starts an expeditor at one core cheaply); the
  `ring()` comment trim; `std::ranges::copy` in place of the two-line insert.
- **Suite** at this tree (`ctest -j8 -LE bs-nightly`, Debug): 4369 of 4370
  passed; the one failure is `DropTableReclaimCrashTest` failing with ENOSPC on
  `/tmp`, not a pass. Overhead not measured; measured at the milestone's close.

**WE09 - settings and the kill switch** (from `5363f42a`). Adopts CJ-Q13 on the
operator's standing go-ahead of 2026-11-01 (go-ahead-achieving-milestone).
- **CJ-Q13 - names, ranges, spelling.** The order's table spells the keys
  `kds.ash.*`; the config file's keys are bare words with underscores
  (`inline_cell_width`), and `ring_bytes_per_core` has a `[PROPOSED]` range.
  *(a) file keys `ash_enabled`, `ash_interval_ms`, `ash_ring_bytes_per_core`,
  `ash_capture_arg_hash`, runtime change by `SET ASH {ENABLED | INTERVAL_MS |
  CAPTURE_ARG_HASH} [=] v`, ring 1-64 MiB; (b) a dotted key family.* **CLA
  proposes (a):** it matches every existing key and `SET CABIN_OPTIMIZER`.
  Adopted. The ring size is restart-only and has no statement.
- **Built.** One `AshControl` of relaxed atomics per sampler, registered with
  its ring; the setters range-check (`CheckAshIntervalMs`, `CheckAshRingBytes`,
  the one home of the ranges, used by the file too), update the process
  defaults and every registered sampler under one mutex; a sampler adopts the
  defaults when it registers, so a `SET` before a core registered still reaches
  it. Boot values are applied in `Expeditor::Start` before any scheduler is
  built. `SET ASH` is admin (the existing `SET` classification) and its
  refusals carry the token's byte. Off keeps the ring; on re-arms a fresh grid
  and continues it. A shortened interval pulls the next sample in at the next
  turn.
- **Review (`critics-developer`).** No blocking bug. Applied: the shortened
  interval waited out the old grid point (`Due` now caps the next due time);
  byte positions on `SET ASH` refusals; one shared range checker instead of
  two copies; one `Update` helper for the three setters; one decision for
  ENABLED vs CAPTURE_ARG_HASH. **Not applied:** `Expeditor::Config` holding a
  `sched::AshSettings` (the four fields stay, defaults restated - a drift risk
  noted); the documentation sync (`kds.conf.sample`, `docker/kds.conf`,
  `manual/server/server.md`, `client-manual.md`, the spec) is left to the
  milestone's close, where the durable content moves.
- **Limits.** The settings are process-wide: two instances in one process
  share one kill switch. A sampler that never registers keeps its own defaults.
- **Suite** at this tree (`ctest -j8 -LE bs-nightly`, Debug): all but
  `DropTableReclaimCrashTest` (ENOSPC on `/tmp`, not a pass). Overhead not
  measured; measured at the milestone's close.

**G2 gate** (from `82f59864`; `tests/ash_gate_test.cpp`, logical clock).

| Check | Result |
|---|---|
| ASH distribution | Met on 12,000 samples: a task alternating `commit.wal_flush` / `io.page_read` on a seeded schedule; the sampled share is within 3 points of the injected share, and each sample names the true state turn for turn (stricter than the bar). |
| Determinism | Met: one seed and clock run twice give byte-identical encoded records; a different seed gives different ones. |
| Overhead | **Not measured.** ASH on vs off needs `build-release` and a second build tree the disk cannot hold; it is the milestone-close measurement. |
| Boundaries | Met by tests already in the tree: wrap-around (`ash_ring_test`), `late` and the grid (`ash_sampler_test`), merge order (`show_sugar_contract_test`), the kill switch and a shortened interval (`ash_sampler_test`, `SET ASH` in `show_sugar_contract_test`). |
| Volatility | Met at the unit level: a new scheduler for a core id starts with an empty ring and the dead one leaves the registry. A real crash and restart of an instance is not run: `sim/` drives no core runtime. |

- **Review (`critics-developer`).** Applied: the truth count was one turn out of
  step with the sample point; the share test now asserts exactly 12,000 records;
  the determinism test asserts it produced records and that a second seed
  differs in the encoded samples; the restart test no longer depends on the
  process defaults or on test order. The per-sample check was added on the
  review's suggestion. **Not applied:** none rejected.
- **Suite** at this tree (`ctest -j8 -LE bs-nightly`, Debug): all but
  `DropTableReclaimCrashTest` (ENOSPC on `/tmp`, not a pass).

**G1 gate** (run after P2, before WE10; `tests/ash_gate_test.cpp`).

| Check | Result |
|---|---|
| I2 completeness | Met for the suite: 0 assert violations over `ctest -LE bs-nightly` (Debug), every core a strict one. `sim/` drives no core runtime, so "and simulation" is not covered. |
| I3 DB time | Met: over 2,000 seeded statements ON_CPU + run queue + waits equal the elapsed time within 1 % (exactly, on the logical clock). |
| Overhead | **Not measured** (milestone-close measurement; the disk cannot hold `build-release`). |
| Determinism | Met: the same seed gives identical cumulative statistics, histograms included. |

**WE10 - `ANALYZE`** (from `aeec4e0d`).
- **Built.** `ANALYZE` appends `DB time 4.21 ms = CPU 1.02 + commit.wal_flush
  2.95 + ...` (`FormatDbTimeLine`): the session's on-CPU and timed in-DB-time
  event deltas across the run, in registry order, `sched.run_queue` printed
  `run_queue`, idle and counters left out, each part rounded to 10 us and the
  total their sum. `FlushOnCpu` books the running task's open ON_CPU segment
  before each reading, so the statement's own CPU is in the second one; it
  changes no total, only when a segment is booked.
- **Scope of "equals the delta".** The total equals the `sys.session_waits`
  delta taken between the two readings, i.e. the run itself; the parse prefix,
  the queue wait before the dispatch and the tail after the second reading are
  outside it.
- **Printed only when measured.** The line appears only when the task being
  polled books to the ANALYZE's own session (`RunningTaskBooksTo`). A
  synchronous `Dispatch` with no such task would print a false 0, so it
  prints no line (found in review).
- **No cross-core line.** `xcore.*` has no call site (WE04 finding), so no
  remote step exists to print; the events stay registered and would count in
  the originating session if booked (WD1).
- **Review (`critics-developer`).** Applied: the measured gate; the total as
  the sum of rounded parts; a compiled-out-build guard and a 1 ms ticking
  clock for the in-task test (it was flaky below the 10 us resolution); a
  `static_assert` on ON_CPU being index 0. **Not applied:** an
  `ANALYZE`-over-`DispatchAsync` test (the branch is shared; the in-task test
  polls the synchronous call); the manual and spec lines for the new output,
  left to the milestone's close.
- **Suite** at this tree (`ctest -j8 -LE bs-nightly`, Debug): all but
  `DropTableReclaimCrashTest` (ENOSPC on `/tmp`, not a pass). Overhead not
  measured; measured at the milestone's close.

**WE12 - event dictionary** (from `2a21fb58`).
- **Built.** `docs/spec/wait-events.md`: per event the registry's class, kind and
  DB-time flag, what `p1`-`p3` hold (or "not filled yet"), meaning, usual causes,
  remedies and the sites that book it. `tests/wait_event_doc_test.cpp` fails when
  the page and `kWaitEvents` disagree on a name, the order, class, kind, DB-time
  flag or a parameter name.
- **Review (`critics-developer`).** Eight claims in the first draft were wrong
  against the code and are corrected: `sysio.checkpoint`'s `p1` is not filled;
  `sysio.wal_write` issues no write (it parks until the log is durable up to a
  page's LSN); `commit.ceiling_wait` is at the head of the next statement, not
  before an acknowledgement; `io.scan_ring_read` is the Cabin optimizer's build,
  not a `SELECT` walk; `app.horizon_wait`'s `p1` and three wait kinds;
  `commit.wal_flush`'s `p2` numbers; `idle.background`'s only site; `idle.client`
  also on session creation. Test hardened: a missing file, a repeated field.
  **Limit:** the test cannot check the prose (which parameter is filled, whether a
  site is true); the review is the only check on it.

**WE11 - the wait-event panel** (from `2a21fb58`). Adopts CJ-Q14 on the operator's
standing go-ahead of 2026-11-01 (go-ahead-achieving-milestone).
- **CJ-Q14 - KDS Studio is not in this tree.** The order asks for a Studio panel;
  no Studio source exists here, and the order fixes only its data sources.
  *(a) deliver the panel's data contract - the queries - and a reference text
  rendering, and leave Studio's own view to its owner; (b) stop for the operator.*
  **CLA proposes (a):** the order's done-when is about the data (`sys.ash` and
  `sys.wait_stats` only, no new frame), which this checks. Adopted. **Open for the
  operator:** the Studio view itself.
- **Found.** A `sys` view answers plain filtered selects only: no `GROUP BY`, no
  aggregate, no alias on the catalog-view path (`a catalog view in this query form
  is not supported`), against SV5's "ordinary typed table". So the panel's counting
  is client-side, over one `samples` read. To go into `known-gaps.md` at the close.
- **Built.** `tools/ash_panel.sql` (three reads: the newest sample, the samples
  since a time, the cumulative waits), `tools/ash_panel.py` (KWP client: active
  sessions over time stacked by class, bucketed by the sampling interval across
  cores; top events; top fingerprints; a fingerprint-to-event drill-down; the
  cumulative table) and `tests/ash_panel_queries_test.cpp`, which runs every query
  against the engine and checks rows, order, the time filter and the NULL-core
  filter.
- **Review (`critics-developer`).** Applied: the default port was wrong (15432);
  class letters collided in the stacked bars; the time axis counted one core per
  point above one core (now bucketed); the test could pass with a broken panel (now
  parses rows); `--fingerprint 0` was ignored. **Not applied:** a test of the Python
  tallies (nothing in the suite runs Python); a mention of the panel in
  `wait-events.md` (left to the close, where the views are written up).

**WE13 - an auxiliary wait signal for the Cabin controller `[EXPERIMENTAL]`**
(from `5af336a3`). Adopts CJ-Q15 on the operator's standing go-ahead of
2026-11-01 (go-ahead-achieving-milestone), under CJ-Q4 (a).
- **CJ-Q15 - what "an auxiliary input" may be.** The order says ASH fingerprint
  activity feeds the controller, must not break the deterministic replay tests
  and shares the controller's switch. No cost model prices a wait. *(a) collect
  it as a fourth signal S4 (a decayed count of WAITING samples per shape) in the
  snapshot, read by no decision, so no decision or digest can move; (b) feed it
  into `Decide` with a weight.* **CLA proposes (a):** (b) needs a cost model the
  tree does not have, and any weight is a guess the replay tests would then pin.
  Adopted. Nothing consumes S4 yet; it is the input a later cost model reads.
- **Built.** `SessionWaitStats::pattern_id`, published by the dispatcher beside
  the fingerprint (cleared at the statement's head, set after its dispatch);
  `AshSampler::set_wait_sink(stats::OptimizerSignals*)`, called once per
  WAITING sample of a statement that has a shape; `OptimizerSignals::
  NoteWaitSample`, counting only a shape already tracked (a wait never admits or
  evicts one) and only while `SET CABIN_OPTIMIZER` is on (its gate is the
  switch); `SnapshotFingerprint::wait_samples_q8`. Wired for core 0 in
  `Expeditor::Start` and for every peer in `CoreRuntime::Open`.
- **Unchanged.** `Decide` reads `frequency_q8`, `pages_q8` and the log fields
  only; the digest is `{version, decay_epoch}` and `NoteWaitSample` does not move
  the version; eviction ranks by executions. The cabin traces and optimizer
  tests pass unchanged, and a new test shows two collectors fed alike, one also
  fed 100 waits, with equal decision fields.
- **Review (`critics-developer`).** No correctness bug. Applied: the
  one-implementation `AshWaitSink` interface is gone (a forward-declared
  `OptimizerSignals*`; `stats` no longer includes the sampler header); the
  dispatcher no longer publishes `arg_hash`. **Found, open for the operator:**
  `fingerprint->arg_hash` is taken after a KWP bind is substituted into the SQL
  text, so it covers bind values; WD11 says bind values are never recorded and a
  hash of a low-entropy value can be reversed. Publishing it would have given
  `ash_capture_arg_hash` its first writer; it is left unpublished, so that
  setting still changes nothing outside its unit test (recorded in
  `known-gaps.md`). **Not applied:** none rejected.
- **Suite** at this tree (`ctest -j8 -LE bs-nightly`, Debug): all but
  `DropTableReclaimCrashTest`, which fails with ENOSPC because `/tmp` is a
  nearly full tmpfs; the same three cells run with `TMPDIR` on the root
  filesystem pass. Overhead not measured here; measured at the close.

**The close** (closing commit `f17bdf81`, `v3.4.6-16-gf17bdf81`; opened from
`ed32c10d`).
- **Measurement** (`ck-tester`, release builds, four arms interleaved with the
  order rotated and the servers pinned; `bench/v3.4.6/results-cj-close-v3.4.6-16-gf17bdf81.md`).
  PK single-row read, one session, `cores = 1`, default durability.
  - **G1** (loss <= 0.8 % against the compiled-out build): the quiet batch
    (36 runs) lost 0.03 % [-0.42, +0.49] - met; the noisy batch (36 runs, host
    load 2.4) 1.02 % [-1.75, +3.72] and all 72 runs pooled 0.56 % [-0.93, +2.02]
    - **not resolvable at the bar's size**.
  - **G2** (ASH on vs off <= 0.5 %): the quiet batch was 0.48 % faster
    [-0.97, -0.01] - met; the noisy batch 1.84 % [-0.96, +4.50] and pooled
    0.77 % [-0.71, +2.25] - **not resolvable**; the pooled point estimate is
    above the bar, its interval spans zero.
  - **Read this plainly:** the quiet batch is the headline only because it was
    chosen after both were seen. The measured noise floor (the arm without CJ
    against the arm with the hooks compiled out) is 0.09 % quiet and 0.49 %
    pooled, so a delta under about 0.5 % is not a finding. The bars were met on
    the quiet batch and neither met nor missed on the whole; they were not shown
    to be missed.
  - **Milestone delta** (opening commit against closing commit, point read):
    about 0.1 %, inside the noise floor on both batches. No regression seen.
  - **Cut**: `cores > 1`, scans, joins, lock contention, an ASH interval below
    1000 ms, the cross-core and lock-wait scope paths. The autocommit INSERT cell
    cannot resolve the commit path's added scope below about 5 %.
  - **Not part of this run**: I3 accuracy, determinism and I2 completeness (the
    G1/G2 test cells above and the suite carry those).
  - A point read is about 8.2 us of engine DB time (7.9 CPU, 0.29 run queue) of
    a 46 us client latency; an INSERT is 432 us of `commit.wal_flush` in 490 us.
- **Suite** at the closing commit (`ctest -j8 -LE bs-nightly`, Debug): green.
  `DropTableReclaimCrashTest` fails with ENOSPC when its directory is on `/tmp`
  (a nearly full tmpfs); with `TMPDIR` on the root filesystem its three cells
  pass.
- **Documents moved**: `docs/spec/sys-views.md` §6.6-6.7 (the views, DB time,
  the sampler, the settings), `docs/spec/wait-events.md` (new), `kds.conf.sample`,
  `docs/spec/client-manual.md`, `manual/server/server.md`, `manual/sql/sql.md`
  (`ANALYZE`), `docs/inflight/known-gaps.md` ("Wait events and ASH (CJ)"),
  `CLAUDE.md`'s Observability row, the index row.
- **Open for the operator**: KDS Studio's own view (CJ-Q14); whether an argument
  hash may be published (WD11 against KWP's substituted binds); `sys.ash`'s
  CPU attribution (CJ-Q12, ON_CPU is not observable to a between-polls sampler);
  `sim/` driving no core runtime, so I2 is not checked under simulation.
