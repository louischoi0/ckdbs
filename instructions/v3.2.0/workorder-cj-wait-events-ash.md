# Work order CJ — wait events and ASH (Active Session History)

Written 2026-10-31 in the main checkout on `main` at `3f225096`
(`v3.4.6-5-g3f225096`), no worktree (documents only), from two documents the
operator pasted into the session, translated into English here (nothing
Korean enters the tree): the spec "Wait Events & ASH" and its workplan. The
operator chose "work order" as their form (the session's question of
2026-10-31). The operator's words are recorded in `raft-marks-2026-10-31.md`
§2.

**Written, not opened.** Nothing is built. No stage has started, no
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

None. Written 2026-10-31; not opened.
