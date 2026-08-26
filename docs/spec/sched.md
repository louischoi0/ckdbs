# KDS Scheduling Specification

The reactor: how work is scheduled on a core. `[OPEN]` items must not be assumed. Consistent with `docs/rules/rules.md` (thread-per-core, no exceptions, deterministic testability) and `docs/spec/heap-and-tuple.md`.

---

## 1. Model

KDS does **not** schedule OS threads. At startup the engine spawns exactly one worker thread per core, pins each with CPU affinity, and never creates threads afterward. Each worker runs a **reactor**: a cooperative, run-to-completion event loop that owns all engine state assigned to that core (shared-nothing). "The scheduler" is the policy inside each reactor that decides which ready task runs next. All scheduler data structures are core-local and lock-free by construction.

Reference architectures: Seastar (ScyllaDB), glommio.

## 2. Reactor Loop

Each iteration executes fixed phases:

```
loop:
  1. drain I/O completions        // poll completion queue; wake waiting tasks
  2. expire timers                // timing wheel against injected clock
  3. drain cross-core inboxes     // per-peer SPSC rings; enqueue as tasks
  4. run ready tasks              // pick by group policy (§4), up to loop budget
  5. submit pending I/O           // batch submission
  6. idle policy if nothing ran   // §7
```

Rules:

- Phases always run in this order; a phase may be empty but never skipped. Fixed ordering is required for deterministic replay.
- Phase 4 has a **loop budget** (max tasks or time slice per iteration) so completion draining and I/O submission latency stay bounded under load.
- The loop body performs no allocation in steady state; all queues/rings are preallocated at startup.

## 3. Tasks

- A task is a unit of work executed run-to-completion until it finishes or **yields**.
- **Cooperative yielding is mandatory:** every task must yield within its budget (work-item count or injected-clock time). Any loop that cannot statically prove boundedness must contain an explicit yield check. Blocking syscalls inside tasks are forbidden; all waiting is expressed as suspension on I/O, timer, or message events.
- **No preemption.** Signal- or timer-driven preemption is forbidden — it destroys deterministic simulation.
- **Suspension safety.** A coroutine must not be parked while holding a resource that only makes sense within a call — above all a page span (`docs/spec/parser-v2.md` I15's R1). `sched::SetSuspendAudit` is the hook a higher layer installs to answer that, checked in debug builds at every suspension; `exec::InstallSuspendAudit()` is the executor's answer, installed per core on the thread that runs statements.
- **No work-stealing.** A task created on a core runs and completes on that core. Moving *work* between cores happens only by sending a message that causes the peer to create its own task.
- Task representation: **C++20 stackless coroutines** (decided 2026-08-05, `include/kds/sched/coro.hpp`). What forced it is that every cross-core operation is a request whose answer arrives later, and the engine had no way to spell "wait" that was not blocking the reactor or hand-rolling a call chain into a state machine — `docs/inflight/in-progress/workplan-crosscore.md` P4's step pipeline could not be written at all. Callbacks would have inverted the executor and all 22 allocation sites; fibers cost a stack per in-flight statement, which an engine sizing itself in pages cannot price. **The scheduler needed no change**: `Task::Poll()` returning `kSuspended`/`kDone` was already a coroutine's resume protocol. The cost is a heap-allocated frame per coroutine, so this is for *suspendable* work — a statement, a pipeline step, a lease request — and never the per-tuple path.

## 4. Scheduling Groups

Purpose: foreground OLTP and background engine work (physical relayout, statistics maintenance, hint-index upkeep) share each core without background work damaging tail latency.

- Every task belongs to exactly one **scheduling group**. Initial groups: `foreground` (OLTP execution), `maintenance` (relayout, stats, hint upkeep), `system` (checkpoint/WAL housekeeping). Groups are engine-defined; adding one is a design change, not a tuning knob.
- Each group has a **share weight**. The scheduler tracks consumed runtime per group (measured via the injected clock) and, past the two floors below, picks the next task from the runnable group with the lowest share-normalized consumed runtime (Seastar-style proportional scheduling). Consumption counters decay periodically so history does not dominate.
- **Two floors under the share law (2026-08-25, `docs/inflight/in-progress/workplan-peer-writer.md` PW7).** Within one reactor iteration a task is polled **at most once** — a task that suspends is not re-polled until the next iteration, and a task submitted by a poll waits for the next iteration — and every group with a task ready when the iteration began is polled **at least once**, whatever its ratio says, in fixed group order (`foreground`, `maintenance`, `system`: a saturated reactor hands the first poll of every iteration to `foreground`, a systematic 1-in-64 bias stated rather than hidden). The loop budget is clamped at construction to at least the group count, or the second floor could not hold. Both were forced by a trace, not a preference: a parked coroutine answers `kSuspended` in nanoseconds, and the loop budget re-polled a peer's parked lease-refill task up to 64 times an iteration, charging each poll to the `system` group (share 50); the group then owed the `foreground` (share 1000) twenty times that, and because a statement's time is the WAL drain's fdatasync — outside every group's account — the debt took hundreds of iterations to clear, during which the *next* refill sat unpolled (546 ms, 395 iterations, measured on the tree committed as `v2.0.0-52-g2c6ae23`). The share law still governs everything past one poll per group. **The consequence, stated**: a coroutine chain that used to advance up to 64 steps inside one iteration now advances one per iteration, each iteration paying one `PollReady` — paid for on the four-writer cell (0.99–1.03×), **unmeasured on the shape that pays most**, P4d's cross-core step pipeline under load. The accounting gap it exposed stands: reactor time spent outside task polls (the drain, the idle block) is charged to no group.
- **The accounting gap is measurable from outside since 2026-08-26** (T4 of the statement-shipping pretasks). The gap itself is unchanged - reactor time outside task polls is still charged to no group - but it can now be *read* rather than argued about. `SHOW META` prints `sched_wall_us` (reactor wall clock since its first iteration), `sched_iterations`, and per group `sched_<group>_polled_us`, `sched_<group>_polls` and `sched_<group>_consumed_us`. **Two counters per group, and the distinction is load-bearing**: `consumed_us` is the share law's own input and is *halved periodically* (`MaybeDecayConsumedRuntime`) so history does not dominate the pick, which makes it a scheduling weight and never a total; `polled_us` and `polls` are cumulative and never decay. So `sched_wall_us - sum(sched_*_polled_us)` is the time charged to nobody - the `PollReady` idle block on a quiet reactor, the WAL drain's `fdatasync` on a committing one - and a spin is visible as `polls` climbing while `polled_us` does not. `bench/v2.1.0` §11-5 recorded this as *not measurable from outside the process on this tree* and owed it to whoever next touched this section; this is that debt paid. The counters cost two integer adds per poll, on the poll path.
- **SLO feedback:** a per-core controller observes foreground latency (e.g., p99 over a sliding window). When latency exceeds the configured SLO, it reduces `maintenance` shares; when there is headroom, it restores them gradually. This is the single mechanism by which "SLO-aware relayout throttling" is implemented — relayout code itself never self-throttles with sleeps.
- Group accounting is core-local. There is no global coordinator; per-core controllers act independently.

## 5. Cross-Core Communication

- Topology: per-core-pair **SPSC lock-free rings** (N² rings for N cores), preallocated at startup. SPSC keeps each ring single-writer/single-reader, matching the shared-nothing ownership rule with no atomics beyond the ring indices.
- A message names a target-core operation and carries POD payload; on receipt (phase 3) the peer wraps it as a task in the sender-designated scheduling group. Replies are messages back to the origin core.
- **Backpressure:** a full ring fails the send with the KDS status type (no blocking, no `throw`). Callers must handle `ring_full` — typically by suspending the sending task until the reactor retries. Silent drop is forbidden.
- The ring interface is injectable: simulation replaces it with an in-memory model that can delay and reorder deliveries (§8).

**Status: built** (`docs/inflight/in-progress/workplan-crosscore.md` P1, 2026-08-04). `sched/spsc_ring.hpp` is the ring, `sched/ring_transport.hpp` the injectable seam and its real N² implementation, `sched/sim_ring_transport.hpp` the simulated one, `sched/ring_message.hpp` the header and the central kind enum, `sched/send_retry.hpp` the `ring_full` answer. Two properties are worth stating here because callers depend on them and neither is obvious from the paragraph above:

- **Delivery order is per edge only.** Messages on one `(src, dst)` pair arrive in send order; two messages from *different* peers to the same core have no defined relative order, and the real and simulated transports deliberately disagree about it — the real one rotates its peer sweep to avoid starvation, the simulation delivers by injected deadline. Nothing above this layer may depend on cross-peer order.
- **The receiving handler runs in phase 4, not phase 3.** The drain moves messages off the ring and queues a task per message, under its own loop budget. A handler is a task and must yield like one.
- **A successful send wakes a sleeping target** (§7, 2026-08-26). `TrySend` is still non-blocking and still fallible, and it costs a syscall only when the target is actually asleep; a *refused* send wakes nobody, since waking a core to find nothing is the spin this exists to remove. The wake is issued after the payload is visible to the reader, never before.

Nothing constructs a transport in production yet: with one reactor, phase 3 costs one null test.

## 6. Timers

- Per-core **hierarchical timing wheel** keyed on the injected monotonic clock. No `std::chrono` reads in engine logic (`docs/rules/rules.md` §4); the clock is a scheduler-provided interface.
- Timer expiry enqueues the waiting task into its scheduling group; expiry order among same-tick timers is FIFO by registration for determinism.

## 7. Idle Policy

When phase 4 finds no runnable task:

- **busy-poll mode** — spin on completion queues and rings. Lowest latency; 100% core occupancy. Default for dedicated appliance deployment. **Not built**, and this line has always described an intention rather than a mode the engine has.
- **blocking mode** — arm eventfd/ring wakeups and block until an event. For development and shared machines. **The block was built from the start; the eventfd wakeup landed 2026-08-26** (the v2.3.0 order's RW1–RW2), and for the two years between them this bullet described a mechanism the code did not have: a reactor blocked in `epoll_wait` and **nothing ended that block when a ring message arrived for it**, because the ring is memory and epoll cannot watch memory. Measured cost of the gap before it closed: a shipped statement's p50 tracked the owner's idle block over a fivefold range, a flat ~1.07 ms with the device in the path and without it (`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` §4a).
- Mode is a runtime option per reactor. Code must not assume either mode.

**The wake path, and its one atomic.** `IoBackend::Wake()` is the seam
(`include/kds/sched/io_backend.hpp`) — injected, never a syscall in engine
logic, so invariant 8 holds and §8's determinism survives; `EpollIoBackend`
implements it with an eventfd it owns and keeps in its own epoll set, and
the sticky counter is what makes a wake that lands just before the block
harmless. A sender wakes **only a core that is actually asleep**, gated on
that core's `sleeping_` flag, because an unconditional wake is a syscall per
message on the sender's critical path and the loaded cells are exactly the
ones where the target is never asleep. The store-load argument that makes
the flag race-free — Dekker's, `seq_cst` on both sides, plus the sleeper's
peek at its own inbox after publishing the flag — is written out at the top
of `include/kds/sched/core_waker.hpp` and is not restated here. The
simulated transport deliberately does **not** wake: its reactors are
multiplexed by a seeded harness, and a second "who runs now" input is the
nondeterminism §8 forbids.

**A block always has a ceiling; `PollReady(-1)` is forbidden.** A lost wake
must degrade to a bounded latency, never to a hang — `max_idle_block_ms`
(10 ms) is that bound, and it is what lets the wake path land before every
park in the engine is proven wakeable. The census below is why that matters.

**Every park and what ends it** (the v2.3.0 order's G2, taken at `bce12d0`;
this is the table a change to the idle policy must be checked against,
because the spin that used to hide a missing wake is what makes an
unclassified park dangerous):

| Park site | Predicate | What satisfies it | Ends the block? |
|---|---|---|---|
| `extent_lease_service.cpp:110`, `row_id_lease_service.cpp:142`, `trx_id_lease_service.cpp:106` | `WaitFor{&refill.granted}` | core 0's grant reply | ring message → **wake** |
| `command_dispatcher.cpp:238` | the remote read is done or torn down | the pipeline's reply | ring message → **wake** |
| `remote_step_service.cpp:600` (`actionable`), `:734` (`output_ok`), `exec/step_vm.cpp:1858` (`resume_gate_`) | pipeline credit, cancel, data | a peer's message | ring message → **wake** |
| `command_dispatcher.cpp:225` (shipped statement), `:252` (index build), `:266` (assertion build) | `Settled(id)`, **and the deadline is read inside the predicate** | the owner's reply, or the deadline | the reply is a ring message → **wake**; the *deadline* has no timer of its own and is noticed only when the task is next polled, so it is honored to within one idle block. **This is the case D4's ceiling exists for** — with an uncapped block a timed-out shipped statement would never answer |
| `command_dispatcher.cpp:279` (**group commit**) | `wal_->IsDurable(lsn)` | the post-task hook on **this** core, once per iteration (`expeditor.cpp:1723`), with the drain timer as backstop | on-core: nothing to wake, and no rule may let the reactor block between the staging and the hook. This is why "nothing completed this round" is not a safe idle predicate, and any parked-is-not-ready work must treat the hook's own work as progress |

Nine of the eleven sites are satisfied by a peer's message and are covered
by the wake. The two that are not are named above with what covers them
instead — a bounded block, and the post-task hook.

## 8. Deterministic Simulation

The reactor depends only on injectable interfaces: **I/O backend, clock, RNG, cross-core rings, idle policy**. In simulation:

- All N reactors run **single-threaded**, multiplexed by a simulated scheduler that picks which reactor advances next using a seeded RNG.
- The simulated environment can inject I/O errors and torn writes, delay/reorder cross-core messages, and skew per-core clocks.
- A failure reproduces from `(seed, build)` alone. CI runs the simulator across many seeds; any nondeterminism (iteration-order dependence, address-dependent hashing, real-time reads) is a build-rejecting defect.
- Practical consequences: containers used by the scheduler must have deterministic iteration order; hashing must be seed-stable; task IDs are sequential per core, never derived from pointers.

## 9. Invariants

1. One pinned worker thread per core; no thread creation after startup.
2. All scheduler state is core-local; no locks in the reactor or scheduler.
3. Reactor phases execute in the fixed order of §2.
4. Every task yields within its budget; no preemption; no work-stealing.
5. Every task carries a scheduling-group membership; group pick is share-proportional.
6. Background work is throttled only via group shares (SLO controller), never via ad-hoc sleeps.
7. Cross-core interaction goes through the SPSC ring interface only; sends are non-blocking and fallible. **A message delivered to a sleeping core wakes it** (§7): a send that succeeds either finds the target awake or ends its block, and no reactor waits out an idle block on work that has already arrived. The wake follows the push and never precedes it.
7a. A reactor's idle block is always bounded (`PollReady(-1)` is forbidden), so a wake that is somehow missed costs latency and never liveness.
8. Engine logic never reads real time, real randomness, or performs direct syscalls; only injected interfaces.

## 10. Open Decisions `[OPEN — do not assume]`

- Loop budget values and yield-budget units (task count vs simulated-time slice).
- SLO controller specifics: latency estimator, window size, share adjustment law.
- Ring capacity sizing and the suspension/retry protocol for `ring_full`.
- I/O backend abstraction (O_DIRECT thread-pool vs io_uring vs pluggable) and its completion-queue polling contract.
- Whether `system` group work (WAL flush) can bypass group accounting under commit-latency pressure.
