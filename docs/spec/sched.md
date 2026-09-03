# KDS Scheduling Specification

The reactor: how work is scheduled on a core. Consistent with `docs/rules/rules.md` (thread-per-core, no exceptions, deterministic testability) and `docs/spec/heap-and-tuple.md`.

---

## 1. Model

KDS does **not** schedule OS threads. At startup the engine spawns exactly one worker thread per core, pins each with CPU affinity, and never creates threads afterward. Each worker runs a **reactor**: a cooperative, run-to-completion event loop that owns the engine state assigned to that core. "The scheduler" is the policy inside each reactor that decides which ready task runs next. All **scheduler** data structures are core-local and lock-free by construction, with one documented exception: the reactor's sleep flag and its `Waker`, which other cores' threads read and write to end an idle block (§7).

**Core-local is the default, not a law of the engine** (AR0-2). Shared-nothing was retired as the memory model: state may be shared where a subsystem's spec says it is and says what serializes it. `rules.md` §3 indexes what is declared today — the WAL stream, this section's wake flag, and the data file's capacity; §9-2 below states the boundary for the reactor thread.

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
  6. idle policy if nothing ran   // §7 - the block is phase 1's, and
                                  //   what ends it is a timer, an fd,
                                  //   or a peer's wake
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
- Task representation: **C++20 stackless coroutines** (`include/kds/sched/coro.hpp`). Every cross-core operation is a request whose answer arrives later, and a coroutine is how "wait" is spelled without blocking the reactor or hand-rolling a call chain into a state machine. `Task::Poll()` returning `kSuspended`/`kDone` is a coroutine's resume protocol, so the scheduler needed no change for it. The cost is a heap-allocated frame per coroutine, so this is for *suspendable* work — a statement, a pipeline step, a lease request — and never the per-tuple path.

## 4. Scheduling Groups

Purpose: foreground OLTP and background engine work (physical relayout, statistics maintenance, hint-index upkeep) share each core without background work damaging tail latency.

- Every task belongs to exactly one **scheduling group**. Groups: `foreground` (OLTP execution), `maintenance` (relayout, stats, hint upkeep), `system` (checkpoint/WAL housekeeping). Groups are engine-defined; adding one is a design change, not a tuning knob.
- Each group has a **share weight**. The scheduler tracks consumed runtime per group (measured via the injected clock) and, past the two floors below, picks the next task from the runnable group with the lowest share-normalized consumed runtime (Seastar-style proportional scheduling). Consumption counters decay periodically so history does not dominate.
- **Two floors under the share law.** Within one reactor iteration a task is polled **at most once** — a task that suspends is not re-polled until the next iteration, and a task submitted by a poll waits for the next iteration — and every group with a task ready when the iteration began is polled **at least once**, whatever its ratio says, in fixed group order (`foreground`, `maintenance`, `system`: a saturated reactor hands the first poll of every iteration to `foreground`, a systematic bias stated rather than hidden). The loop budget is clamped at construction to at least the group count, or the second floor could not hold. The floors exist because a parked coroutine answers `kSuspended` in nanoseconds: without them the loop budget re-polls one parked task up to the budget every iteration, charging every poll to its group, and the group's debt under the share law then starves its own next task. The share law governs everything past one poll per group. **Consequence**: a coroutine chain advances one step per iteration, each iteration paying one `PollReady`.
- **Reactor time spent outside task polls — the WAL drain, the idle block — is charged to no group**, and the gap is readable from outside. `SHOW META` prints `sched_wall_us` (reactor wall clock since its first iteration), `sched_iterations`, and per group `sched_<group>_polled_us`, `sched_<group>_polls` and `sched_<group>_consumed_us`. **Two counters per group, and the distinction is load-bearing**: `consumed_us` is the share law's own input and is *halved periodically* (`MaybeDecayConsumedRuntime`) so history does not dominate the pick, which makes it a scheduling weight and never a total; `polled_us` and `polls` are cumulative and never decay. `SHOW META` also prints `sched_idle_block_us` — wall time inside a `PollReady` this reactor was *allowed* to block in — so `sched_wall_us − Σ sched_*_polled_us − sched_idle_block_us` is the time charged to nobody that was **not** sleep, and a spin is visible as `polls` climbing while `polled_us` does not. An idle block belongs to no group. The counters cost two integer adds per poll, on the poll path.
- **A parked coroutine is not runnable, and the idle policy knows it.** The two floors above are about how a *ready* task is picked; this is about what "ready" means. A task that suspends goes back on its queue, so a queue is not a test of whether there is work; §7 carries the rule and its one hazard.
- **Shares are static.** There is no SLO-feedback controller: nothing observes foreground latency and nothing adjusts a group's share at runtime. Relayout code never self-throttles with sleeps (invariant 6).
- Group accounting is core-local. There is no global coordinator.

## 5. Cross-Core Communication

- Topology: per-core-pair **SPSC lock-free rings** (N² rings for N cores), preallocated at startup. SPSC keeps each ring single-writer/single-reader — one writer and one reader by construction, so no atomics beyond the ring indices. This holds regardless of what else the engine shares: the rings are how *work* moves between cores, and nothing has been added to them.
- A message names a target-core operation and carries POD payload; on receipt (phase 3) the peer wraps it as a task in the sender-designated scheduling group. Replies are messages back to the origin core.
- **Backpressure:** a full ring fails the send with the KDS status type (no blocking, no `throw`). Callers must handle `ring_full` — typically by suspending the sending task until the reactor retries. Silent drop is forbidden.
- The ring interface is injectable: simulation replaces it with an in-memory model that can delay and reorder deliveries (§8).

`sched/spsc_ring.hpp` is the ring, `sched/ring_transport.hpp` the injectable seam and its real N² implementation, `sched/sim_ring_transport.hpp` the simulated one, `sched/ring_message.hpp` the header and the central kind enum, `sched/send_retry.hpp` the `ring_full` answer. Four properties callers depend on:

- **Delivery order is per edge only.** Messages on one `(src, dst)` pair arrive in send order; two messages from *different* peers to the same core have no defined relative order, and the real and simulated transports deliberately disagree about it — the real one rotates its peer sweep to avoid starvation, the simulation delivers by injected deadline. Nothing above this layer may depend on cross-peer order.
- **The receiving handler runs in phase 4, not phase 3.** The drain moves messages off the ring and queues a task per message, under its own loop budget. A handler is a task and must yield like one.
- **A successful send wakes a sleeping target** (§7). `TrySend` stays non-blocking and fallible; it costs a syscall only when the target is actually asleep, and a *refused* send wakes nobody — waking a core to find nothing is the spin the wake exists to remove. The wake follows the push and never precedes it.
- **A service armed inside `AttachTransport` takes the transport *parameter*, never the `transport_` member.** The member is assigned at the end of that function, so a service constructed earlier that reads it gets a null; the parameter is in scope the whole time. `src/server/core_runtime.cpp` carries the rule as a comment at the sites it binds, and no read of `transport_` occurs inside `AttachTransport`.

At `cores = 1` nothing constructs a transport and phase 3 costs one null
test. Above one core `Expeditor::Serve` builds the real N² transport and
attaches every reactor to it (`src/server/expeditor.cpp`), which is what
makes the rule above a production rule and not a test one.

## 6. Timers

- Per-core **hierarchical timing wheel** keyed on the injected monotonic clock. No `std::chrono` reads in engine logic (`docs/rules/rules.md` §4); the clock is a scheduler-provided interface.
- Timer expiry enqueues the waiting task into its scheduling group; expiry order among same-tick timers is FIFO by registration for determinism.

## 7. Idle Policy

When phase 4 finds no runnable task the reactor **blocks**: it arms
eventfd/ring wakeups and blocks until an event (`include/kds/sched/waker.hpp`).
There is no busy-poll mode.

**The block is accountable.** `SHOW META` prints `sched_idle_blocks`,
`sched_parked_idle_blocks`, `sched_wake_race_skips`, `sched_idle_block_us`,
`sched_wakes_sent`, `sched_wakes_received` and `sched_spurious_wakes`
(§4 for what the duration buys, `docs/spec/client-manual.md` §3 field by
field). Two of them are checks rather than measurements: the instance's
`sched_wakes_sent` must equal the sum of the cores' `sched_wakes_received`,
and `sched_spurious_wakes` climbing far past `sched_wake_race_skips` would
mean senders waking cores they have nothing for.

**The wake, and its one atomic.** One `Waker` (an eventfd) per reactor,
armed at `AttachTransport` and registered with that reactor's backend like
any other readable handle — so a single-core build arms nothing and pays
nothing. A sender wakes **only a destination that is actually asleep**,
reading that core's `sleeping` flag first, because an eventfd write is a
syscall on the sender's critical path and the cells shipping is already fast
in are exactly the ones where the owner is never asleep. The flag cannot be
missed: sender and receiver touch the two variables in opposite orders with
a `seq_cst` fence on **both** sides, so sequential consistency forbids both
reads returning stale — either the sender sees the flag and writes the wake,
or the receiver's pre-block re-check sees the message and does not sleep
(`ring_transport.hpp` carries the argument; `Scheduler::wake_race_skips()`
counts the second case). **The sender's fence is not optional**: its store
is the ring's release, and StoreLoad is the one reordering x86 TSO permits.
The simulated transport does not wake — its reactors are multiplexed by a
seeded harness, and a second "who runs now" input is the nondeterminism §8
forbids.

**A block always has a ceiling.** `max_idle_block_ms` (10 ms) bounds every
idle block, so a wake that is somehow missed costs latency and never
liveness. That is not belt-and-braces; the census below has two entries that
depend on it. **`PollReady` is never given a negative timeout** — invariant
7a.

**Parked is not ready.** `IdleTimeoutMs` does not read a non-empty queue as
work to do. A block is permitted only after a full iteration in which
**nothing advanced**: no I/O event, no timer, no message drained, no task
that completed or executed a line, no task newly submitted, and no work
from the post-task hook. "Executed a line" is `Task::advanced_in_last_poll`
— `CoroTask` answers it from whether the poll resumed the coroutine at all,
and every other task type inherits `true`, so an untracked task keeps the
reactor awake rather than being slept through. The block therefore arrives
one iteration after the last advancing one: the reactor sleeps on evidence
it has collected, never on a prediction.

**The hazard, because it is the one that would make a worse engine.** The
group commit parks on `durable_lsn`, and the only thing that moves it is the
post-task hook running after phase 4 (`expeditor.cpp`). A rule that let the
reactor block between the staging and the hook's sync would put the WAL
drain interval on *every commit* — trading a spin for a durability
regression. That is why `SetPostTaskHook` takes a `std::function<bool()>`
and why both drain sites answer it with `HasPendingGroupCommits()`, read
before the drain clears it.

**The cost of sleeping** falls where the core was idle anyway: a reactor
that spins notices its reply in nanoseconds, and one that sleeps has to be
woken, so a single parked waiter on a host with spare cores pays the wake's
latency; a core with other work does not.

**Every park and what ends it.** This is the table a change to the idle
policy must be checked against: a park with no wake source is
indistinguishable from one that has a wake it never needed.

| Park site | Predicate | What satisfies it | Ends the block? |
|---|---|---|---|
| `extent_lease_service.cpp:110`, `row_id_lease_service.cpp:142`, `trx_id_lease_service.cpp:106` | `WaitFor{&refill.granted}` | core 0's grant reply | ring message → **wake** |
| `command_dispatcher.cpp:238` | the remote read is done or torn down | the pipeline's reply | ring message → **wake** |
| `remote_step_service.cpp:600` (`actionable`), `:734` (`output_ok`), `exec/step_vm.cpp:1858` (`resume_gate_`) | pipeline credit, cancel, data | a peer's message | ring message → **wake** |
| `command_dispatcher.cpp:225` (shipped statement), `:252` (index build), `:266` (assertion build) | `Settled(id)`, **with the deadline read inside the predicate** | the owner's reply, or the deadline | the reply is a ring message → **wake**; the *deadline* has no timer of its own and is noticed only when the task is next polled, so it is honored to within one idle block. **This is what the ceiling above is for** — under an unbounded block a timed-out shipped statement would never answer |
| `command_dispatcher.cpp:279` (**group commit**) | `wal_->IsDurable(lsn)` | the post-task hook on **this** core, once per iteration (`expeditor.cpp:1723`), with the drain timer as backstop | on-core: nothing to wake. Any "parked is not ready" rule must count the hook's own work as progress, or every commit gains a drain interval |

Nine of the eleven sites are satisfied by a peer's message and are covered
by the wake. The two that are not are named above with what covers them
instead.

## 8. Deterministic Simulation

The reactor depends only on injectable interfaces: **I/O backend, clock, RNG, cross-core rings, idle policy**. In simulation:

- All N reactors run **single-threaded**, multiplexed by a simulated scheduler that picks which reactor advances next using a seeded RNG.
- The simulated environment can inject I/O errors and torn writes, delay/reorder cross-core messages, and skew per-core clocks.
- A failure reproduces from `(seed, build)` alone. CI runs the simulator across many seeds; any nondeterminism (iteration-order dependence, address-dependent hashing, real-time reads) is a build-rejecting defect.
- Practical consequences: containers used by the scheduler must have deterministic iteration order; hashing must be seed-stable; task IDs are sequential per core, never derived from pointers.

**Where the wake path is covered.** The seed-driven harness under `sim/`
(`scripts/sim.sh`) builds a whole *instance* on crashable in-memory devices
and drives it through `CommandDispatcher`; it constructs **no reactor at
all**, so it has no idle block to interrupt and the wake path has no
representation in it — its role here is a regression gate, not coverage.
The deterministic coverage the wake needs lives in the scheduler suite
(`tests/scheduler_test.cpp`) against the **real** epoll backend, which is
the only place a block exists to be ended — including the composed shape, a
coroutine parked on a flag only a peer's message sets, on a reactor that
"parked is not ready" has allowed to sleep, with the test carrying **its
own deadline** so a lost wake fails a named assertion rather than timing out
a suite. The `SimRingTransport` answers the wake's two halves honestly and
wakes nobody: its reactors are multiplexed by a seeded harness, and a second
"who runs now" input is the nondeterminism this section forbids.

## 9. Invariants

1. One pinned worker thread per core; no thread creation after startup.
2. All scheduler state is core-local but the sleep flag and `Waker` of §7, and the scheduler takes no lock. **The reactor is not lock-free, and the exception is named rather than general** (AR0-2): a task appending to the WAL takes the log's latch (`wal.md` §3), a task syncing takes the log device's segment-table lock under it, and a peer's wait on the writer takes the writer's mutex with the latch released. Those three are the whole list (`wal/stream.hpp` and `wal/writer.hpp` state the order). A subsystem may add another only by stating it in its own spec, with what it serializes and why a core-local alternative was rejected; an unstated lock in the reactor is a defect.
3. Reactor phases execute in the fixed order of §2.
4. Every task yields within its budget; no preemption; no work-stealing.
4a. A queue is not a claim of work: a task parked on a condition is not runnable, and the reactor may sleep while holding one (§7). A task type that cannot tell a park from a yield inherits `advanced_in_last_poll() == true` and keeps the reactor awake — the safe answer, never the accurate-looking one.
5. Every task carries a scheduling-group membership; group pick is share-proportional.
6. Background work is throttled only via group shares, never via ad-hoc sleeps.
7. Cross-core interaction goes through the SPSC ring interface only; sends are non-blocking and fallible. **A message delivered to a sleeping core wakes it** (§7): a successful send either finds the target awake or ends its block, so no reactor waits out an idle block on work that has already arrived.
7a. A reactor's idle block is always bounded — `PollReady` is never given a negative timeout — so a missed wake costs latency and never liveness.
8. Engine logic never reads real time, real randomness, or performs direct syscalls; only injected interfaces.

## 10. Open Decisions

The open decisions of this subsystem are unrecorded here.
