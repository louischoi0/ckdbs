# KDS Scheduling Specification

The reactor: how work is scheduled on a core. Consistent with `docs/rules/rules.md` (thread-per-core, no exceptions, deterministic testability) and `docs/spec/heap-and-tuple.md`.

---

## 1. Model

KDS does **not** schedule OS threads. At startup the engine spawns exactly one worker thread per core, pins each with CPU affinity, and never creates threads afterward. Each worker runs a **reactor**: a cooperative, run-to-completion event loop that owns the engine state assigned to that core. "The scheduler" is the policy inside each reactor that decides which ready task runs next. All **scheduler** data structures are core-local and lock-free by construction, with one documented exception: the reactor's sleep flag and its `Waker`, which other cores' threads read and write to end an idle block (§7).

**Core-local is the default, not a law of the engine** (AR0-2). Shared-nothing was retired as the memory model: state may be shared where a subsystem's spec says it is and says what serializes it. `rules.md` §3 indexes what is declared today — the WAL stream, this section's wake flag, the data file's capacity, the visibility window (AN-S1), the page frame's latch word (AM-S1) and, declared before a line of it exists, M2's lock table; §9-2 below states the boundary for the reactor thread.

Reference architectures: Seastar (ScyllaDB), glommio.

## 2. Reactor Loop

Each iteration executes fixed phases:

```
loop:
  1. drain I/O completions        // poll completion queue; wake waiting tasks
  2. expire timers                // timing wheel against injected clock
  3. (retired at AT-S10d)         // drained the cross-core rings; a kick
                                  //   arrives in phase 1 as the waker's fd
  4. run ready tasks              // pick by group policy (§4), up to loop budget
  5. submit pending I/O           // batch submission
  6. idle policy if nothing ran   // §7 - the block is phase 1's, and
                                  //   what ends it is a timer, an fd,
                                  //   or a peer's wake
```

Rules:

- Phases always run in this order; a phase may be empty but never skipped. Fixed ordering is required for deterministic replay.
- Phase 4 has a **loop budget** (max tasks or time slice per iteration) so completion draining and I/O submission latency stay bounded under load.
- The loop body performs no allocation in steady state; all queues are preallocated at startup.
- Phase 3 is kept as a number and not reused: the numbering is cited across the tree, and renumbering would make every earlier citation name the wrong phase.

## 3. Tasks

- A task is a unit of work executed run-to-completion until it finishes or **yields**.
- **Cooperative yielding is mandatory:** every task must yield within its budget (work-item count or injected-clock time). Any loop that cannot statically prove boundedness must contain an explicit yield check. Blocking syscalls inside tasks are forbidden; all waiting is expressed as suspension on I/O, timer, or message events.
- **No preemption.** Signal- or timer-driven preemption is forbidden — it destroys deterministic simulation.
- **Suspension safety.** A coroutine must not be parked while holding a resource that only makes sense within a call — above all a page span (`docs/spec/parser-v2.md` I15's R1). `sched::SetSuspendAudit` is the hook a higher layer installs to answer that, checked in debug builds at every suspension; `exec::InstallSuspendAudit()` is the executor's answer, installed per core on the thread that runs statements.
- **No work-stealing.** A task created on a core runs and completes on that core. Nothing moves *work* between cores since AT-S10d: a core that needs another to act writes shared state and kicks it (§5), and the kicked core's own parked task does the work.
- Task representation: **C++20 stackless coroutines** (`include/kds/sched/coro.hpp`). Every cross-core operation is a request whose answer arrives later, and a coroutine is how "wait" is spelled without blocking the reactor or hand-rolling a call chain into a state machine. `Task::Poll()` returning `kSuspended`/`kDone` is a coroutine's resume protocol, so the scheduler needed no change for it. The cost is a heap-allocated frame per coroutine, so this is for *suspendable* work — a statement, a lock or durability wait, a lease request — and never the per-tuple path.

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

**One mechanism: write-then-kick** (AR0-6-R1). A core that has something
for another writes it into shared state under that state's own latch, then
kicks the destination through the instance's wake registry
(`sched/waker_table.hpp`, §7). The kick carries no payload and no meaning:
a woken reactor learns why from the structure it parks on. **There is no
message, no queue between cores and no ring since AT-S10d**, and nothing
crosses that a kick does not cover.

Three consumers:

- **The lock table's slot flip** (AU-S2, `txn.md` §5): a decide flips the
  waiter's slot under the partition latch and kicks the waiter's core.
- **D19's connection handoff** (AT-S10c, `include/kds/server/connection_handoff.hpp`),
  built only where `TcpServer::Listen` answers `Unsupported` because the
  platform refuses `SO_REUSEPORT`. There core 0 binds the port alone,
  places each accepted socket on a core round-robin over every core,
  itself included (`NextCore`), pushes it into that core's inbox under the
  inbox's latch, and kicks it; the destination's reactor runs a parked task
  (`TcpServer::Host`, attached by `CoreRuntime::HostHandedConnections`)
  that takes the inbox and adopts each socket as though it had accepted it.
  The socket travels in the inbox because a kick carries no payload. A
  session crosses once, at placement, and runs to completion where it lands.
- **A stop** (AU-S3): the scheduler's stop flag is atomic, and core 0 sets
  a peer's and kicks it; a peer's STOP sets core 0's through the
  instance's hook and kicks it.

**A kick is best-effort, and the cost of losing one is bounded.** A kicker
reads the destination's `sleeping` flag and writes the eventfd only when it
is set; one landing between the destination's last look and its raising of
the flag reads clear and is skipped, and the destination waits out one idle
block (`max_idle_block_ms`, §7). Every consumer above is level-triggered -
its parked task re-asks its predicate after every block - so a lost kick
costs latency and never the wake.

**What went at AT-S10d**, with the last kind that used it (the two id
leases, AT-S10b): the per-core-pair SPSC rings and their N² preallocation
(`sched/spsc_ring.hpp`), the injectable transport and its real and
simulated implementations (`sched/ring_transport.hpp`,
`sched/sim_ring_transport.hpp`), the message header and kind enum
(`sched/ring_message.hpp`, D25's census at 2), the `ring_full` retry task
(`sched/send_retry.hpp`), phase 3's drain and handler table, and
`AttachTransport` on the scheduler and on `CoreRuntime`. The full text of
this section as it stood is `git show ba8c824:docs/spec/sched.md`.

## 6. Timers

- Per-core **hierarchical timing wheel** keyed on the injected monotonic clock. No `std::chrono` reads in engine logic (`docs/rules/rules.md` §4); the clock is a scheduler-provided interface.
- Timer expiry enqueues the waiting task into its scheduling group; expiry order among same-tick timers is FIFO by registration for determinism.

## 7. Idle Policy

When phase 4 finds no runnable task the reactor **blocks**: it raises its
`sleeping` flag and blocks in its I/O backend until an fd, a timer or a
kick ends it (`include/kds/sched/waker.hpp`). There is no busy-poll mode.

**The block is accountable.** `SHOW META` prints `sched_idle_blocks`,
`sched_parked_idle_blocks`, `sched_idle_block_us`, `sched_wakes_sent` and
`sched_wakes_received` (§4 for what the duration buys; `command_dispatcher.cpp`
carries each field's reading, the client manual lists none). One of them is a check
rather than a measurement: the instance's `sched_wakes_sent` must equal
the sum of the cores' `sched_wakes_received`. **`sched_wake_race_skips`
and `sched_spurious_wakes` went with the ring at AT-S10d**: the first
counted the pre-block re-check finding a queued message and the second a
wake that found an empty inbox, and with no queue the first cannot move
and the second would count every wake.

**The wake, and its one atomic.** One `Waker` (an eventfd) per reactor,
armed by `AttachWakerTable` and registered with that reactor's backend like
any other readable handle — so a single-core build, which attaches no
table, arms nothing and pays nothing. The same call registers the reactor
in the instance's one wake registry (`sched/waker_table.hpp`), which is
where every write-then-kick consumer (§5) goes. **The attach point takes an
interface, `WakeRegistry`, and `WakerTable` is its one production
implementation** (AU-S1c, on AV-R1's mark): the seam is the *table*, never
`Waker`, whose `Wake()` stays non-virtual — so the flag read and the skip
counter have one implementation, and the cost is on the kick path: one
indirect call per cross-core `Kick`. The other implementation is
`SimWakerTable` (`sched/sim_waker_table.hpp`), the two-core rig's: it
wraps a real table, logs `(tick, dst)` per kick and forwards at
`tick + delay(seed, dst, tick)`, where the tick is a counter the rig
advances and never wall time. A kicker wakes **only a destination that is
actually asleep**, reading that core's `sleeping` flag first, because an
eventfd write is a syscall on the kicker's critical path and a busy
destination is never asleep.

**The flag can be missed, and that is the stated cost** (AR0-6-R1). A
kicker that published just before the destination raised its flag reads it
clear and skips the kick, and the destination waits out one idle block.
Closing that window takes a store-buffer pair - a `seq_cst` fence on both
sides - *and* the destination re-reading the predicate it is about to park
on after raising the flag. The ring supplied that third leg in
`HasPending`; no write-then-kick consumer has a predicate the reactor can
re-read before it blocks, so since AT-S10d the re-check, both fences and
`Scheduler::wake_race_skips()` are gone rather than kept for a leg nothing
supplies. Every consumer is level-triggered, re-polled after the block, so
a skipped kick is slow and never wrong.

**A block always has a ceiling.** `max_idle_block_ms` (10 ms) bounds every
idle block, so a wake that is somehow missed costs latency and never
liveness. That is not belt-and-braces; the census below has entries that
depend on it. **`PollReady` is never given a negative timeout** — invariant
7a.

**Parked is not ready.** `IdleTimeoutMs` does not read a non-empty queue as
work to do. A block is permitted only after a full iteration in which
**nothing advanced**: no I/O event, no timer, no task
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
| `command_dispatcher.cpp:737` (**group commit**) | `wal_->IsDurable(lsn)` | the post-task hook on **this** core, once per iteration (`expeditor.cpp:1953`), with the drain timer as backstop | on-core: nothing to wake. Any "parked is not ready" rule must count the hook's own work as progress, or every commit gains a drain interval |
| `tcp_server.cpp` `RunHost` (**D19's handoff**, a peer's hosting task; only where the port cannot be shared) | `handoff->Pending(core)`, or the server gone | core 0's `Offer`, which sets `pending` under the inbox latch and then kicks | the kick → **wake**. There is no pre-block re-check (§5, and above), so an offer racing the reactor into its block is noticed at the block's ceiling, one idle block late, never lost |

Each row is named with what covers it. The two id-lease refill parks,
satisfied by core 0's grant over the ring and covered by the wake, went
with the leases at AT-S10b; the remote read's park and the remote step
server's two, with the executor's `resume_gate_` they drove, went with
the remote-step protocol at AT-S10; the shipped statement's `Settled(id)`
park, whose reply was a ring message and whose deadline was the reason
the ceiling above existed, went with statement shipping at AT-S6 and its
row with the ring at AT-S10d. **Not in the table and owed to it**: the
lock family's two parks (`command_dispatcher.cpp`'s `decided` and
`freed` predicates, AO), each bounded by the fault net read inside its
predicate.

## 8. Deterministic Simulation

The reactor depends only on injectable interfaces: **I/O backend, clock, RNG, wake registry, idle policy**. In simulation:

- All N reactors run **single-threaded**, multiplexed by a simulated scheduler that picks which reactor advances next using a seeded RNG.
- The simulated environment can inject I/O errors and torn writes, delay kicks, and skew per-core clocks.
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
coroutine parked on state only another thread changes and then kicks, on a
reactor that "parked is not ready" has allowed to sleep, with the test
carrying **its own deadline** so a lost wake fails a named assertion rather
than timing out a suite.

**The two-core rig is the other deterministic shape, and it is deterministic
over less** (`instructions/v3.0.0/workorder-av-two-core-rig.md` AV-R3).
Two real reactors on two threads over one store, one stream and one
`SimWakerTable` (`sched/sim_waker_table.hpp`): what the seed fixes is the
delay each kick draws, as a pure function of `(seed, dst, tick)` with no
stream between draws — so which kicks happen and at which tick is the two
threads' interleaving, which nothing reproduces, and what each of them
costs in ticks is the seed's. A single-threaded driver reproduces the whole
log. A cell that needs a state reached deterministically reaches it by a
barrier, and the rig's tick is advanced by the cell rather than by any
clock, so a held kick ends a real idle block only when the cell says so.

## 9. Invariants

1. One pinned worker thread per core; no thread creation after startup.
2. All scheduler state is core-local but the sleep flag and `Waker` of §7, and the scheduler takes no lock. **The reactor is not lock-free, and the exception is named rather than general** (AR0-2): a task appending to the WAL takes the log's latch (`wal.md` §3), a task syncing takes the log device's segment-table lock under it, and a peer's wait on the writer takes the writer's mutex with the latch released. Those three were the whole list until six joined them, and two more at AT-S5d. **AO-S8 counted the list rather than appending to it**, and found it three short of the tree — the enumeration is the invariant, so a lock missing from it is the defect this clause names, not a documentation debt. Each carries its order in `rules.md` §3's table and in its own header:

    - the **visibility window latch** (`txn/instance_visibility.hpp`, AN-S1), a leaf;
    - the **page frame's latch word** (`device_page_store.hpp`, `page.md` §6, AM-S1), outer to the WAL stream latch;
    - the **frame-table structure latch** and the **free-map latch** (`device_page_store.hpp`, AM-S2, AM-S3), the second inner to the first and to nothing else;
    - the **data file's growth lock** (`file_page_device.hpp`'s `grow_mu_`, `memory_page_device.hpp`'s `mu_`), inside the device around the allocation;
    - since M2, the **lock table's partition latch** (`txn/lock_table.hpp`, AO-R2) — one `std::mutex` per partition, null at `cores = 1`, taken with no page latch, no WAL latch and no window latch held, one partition at a time, and released before any park;
    - the lock table's **wait-for latch** (`lock_table.hpp`'s `wait_latch_`), which is deliberately *not* a partition's: the graph spans relations, so an edge is recorded under its own lock and never under the partition the waiter was refused by;
    - and since AT-S5d the **assertion registry's two** (`exec/assertion_check.hpp`, `assertion.md` §6.1): its **directory latch**, which never spans page work and does span the WAL append that describes a header change, and one **chain latch** per assertion, which spans its Bound Cabin's appends. Both null at `cores = 1`; the order is the chain latch, a cabin page's latch, the directory latch, the WAL stream latch.

    `wal/stream.hpp` and `wal/writer.hpp` state the WAL's order. **Three of these orders are stated and not checked** — the page latch's, the lock table's and the assertion registry's — which `rules.md` §3 calls a comment rather than a rule; the free map's is checked in debug by `HoldsLatch`. A subsystem may add another only by stating it in its own spec, with what it serializes and why a core-local alternative was rejected; an unstated lock in the reactor is a defect.
3. Reactor phases execute in the fixed order of §2.
4. Every task yields within its budget; no preemption; no work-stealing.
4a. A queue is not a claim of work: a task parked on a condition is not runnable, and the reactor may sleep while holding one (§7). A task type that cannot tell a park from a yield inherits `advanced_in_last_poll() == true` and keeps the reactor awake — the safe answer, never the accurate-looking one.
5. Every task carries a scheduling-group membership; group pick is share-proportional.
6. Background work is throttled only via group shares, never via ad-hoc sleeps.
7. Cross-core interaction is write-then-kick and nothing else since AT-S10d (§5: the lock table's slot flip, D19's connection handoff and a stop): shared state written under its own latch, then the destination kicked. A kick never blocks and cannot fail. **A kick that finds the destination asleep ends its block** (§7); one that races the destination into its block is skipped, and the work it announced waits at most one idle block (7a) - the ring's stronger guarantee, that no reactor waits out a block on work already queued, retired with its queue.
7a. A reactor's idle block is always bounded — `PollReady` is never given a negative timeout — so a missed wake costs latency and never liveness.
8. Engine logic never reads real time, real randomness, or performs direct syscalls; only injected interfaces.

## 10. Open Decisions

The open decisions of this subsystem are unrecorded here.
