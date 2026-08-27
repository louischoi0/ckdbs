# Work order — the reactor wake path

Drafted 2026-08-26 against `main` at `fb10216`. The measurement that
produced it: `bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md`
(SS-B, tree byte-identical to `582d914`).

Discipline as before: every claim **measured** (with its cell) or
**source-read** (`path:line` + commit); `build-release` for numbers;
`git describe --tags` names results; task rows that land state their
worktree and cite their review.

---

## 1. What is wrong, stated exactly

A reactor with no ready task sleeps in `epoll_wait` for up to
`max_idle_block_ms`, and **nothing wakes it when a ring message arrives**.
The ring is polled, not signalled: `Scheduler::DrainInbox()`
(`src/sched/scheduler.cpp:62`) calls `transport_->TryReceive` once per
iteration, and an iteration only begins after the sleep ends. No `eventfd`,
no `Wake`, no self-pipe exists anywhere in `src/` or `include/`
(**source-read**).

Two source facts set the floor:

- `Scheduler::IdleTimeoutMs()` (`src/sched/scheduler.cpp:196-214`) returns
  a **whole-millisecond** count, and where a timer bounds it the remainder
  is rounded **up** — `(deadline - now + 999'999) / 1'000'000` — with the
  comment stating the intent: a late timer is cheaper than a spin. That
  reasoning is sound for timers and says nothing about messages.
- That count is handed to `::epoll_wait` unchanged
  (`src/sched/epoll_io_backend.cpp:88`), whose resolution is milliseconds.

So the minimum a message can wait behind an idle reactor is **1 ms**, and
the default `max_idle_block_ms = 10` (`include/kds/sched/scheduler.hpp:79`)
sets the maximum at ten.

**Measured consequence.** A shipped statement costs a flat **1,064–1,068 µs**
more than a seated one — the same constant with the device sync in the path
and with it removed. Move the knob and the latency follows it exactly:
1.08 / 2.10 / 3.11 / 5.12 ms at `wal_drain_interval_us` 1000 / 2000 / 3000 /
5000, with the seated control flat at 23 µs; below 1 ms nothing changes
(50, 200, 500, 1000 µs all give 1.08 ms) because of the rounding above.
SS-B §4a.

**This is not a shipping defect.** Any cross-core message to an idle core
pays it. Shipping is the first feature to put one on a client's critical
path, which is why it surfaced now. Two other measured symptoms are the same
fact from other angles:

- Shipping runs at **0.43–0.53×** wherever the owner is idle between
  statements (SS-B §3 B1, §7 B4 K=1, §9 B6 — three independent cells), and
  at **0.93–0.99×** from four sessions upward where the owner never sleeps.
  The whole R1 penalty is this sleep.
- A single parked waiter holds its arrival core at **89% busy** (93% at
  K = 16), 3.1M → 7.9M polls/s while the cost of a poll stays flat at
  0.059–0.068 µs (SS-B §7). `IdleTimeoutMs` returns **0** whenever a ready
  queue is non-empty (`scheduler.cpp:196-199`) and a parked coroutine sits
  in one, so the reactor never sleeps and never stops polling.

The two symptoms are the same missing mechanism seen from both ends:
**readiness is not represented, so the scheduler either sleeps through work
or spins on the absence of it.**

---

## 2. Why this is the next task, ahead of the alternatives

- It is the **largest measured effect in the tree**: a factor of two on
  shipping in the R1 regime, and a whole core per parked waiter.
- It is **narrow**: one signalling primitive and one readiness predicate,
  inside `sched`. It changes no page format, no WAL record, no wire message,
  no catalog row.
- It **retires a pending decision instead of feeding it.** `crosscore.md`
  §9's routing question — whether to ship conditionally — inherits B1's 2×
  as its motivating number. If that 2× is a sleep rather than a property of
  shipping, then building conditional routing now would add permanent
  complexity to work around a defect that is about to disappear. **Do this
  first, re-measure B1, and let §9 decide against the corrected number.**
- Every other open item is either measurement (SS-B's remaining cells),
  a decision (`statement_ship_service.hpp` rule 1, assertion ownership), or
  a separate track (eviction, free map). None is blocked by this, and this
  blocks the honest reading of the others.

---

## 3. Decisions this order takes (operator-delegated unless marked)

- **W1 — Signal, do not shorten.** The fix is a wake path, not a smaller
  `max_idle_block_ms`. Shortening the block trades latency for a spin on
  every idle core and does not reach below 1 ms anyway (SS-B §4a measured
  50 µs and 1000 µs as identical). The knob keeps its current default.
- **W2 — One wake fd per reactor, registered with that reactor's poller.**
  An `eventfd` (Linux, `EFD_NONBLOCK`) owned by the core, added to its
  epoll set like any other source. A sender writes to the target core's fd
  after enqueuing to the ring. This keeps the shared-nothing shape: the fd
  is written by others but read and drained only by its owner, exactly as
  the ring's indices are (guideline 1's existing exception, not a new one).
- **W3 — Signal after enqueue, unconditionally, and let the receiver
  deduplicate.** No "is it sleeping" test on the sender side: that test is
  a race and the counter it would need is shared state. An `eventfd`
  coalesces by construction — a sleeping reactor wakes once for N writes,
  and a busy reactor's read finds an already-drained counter. Measure the
  cost of the unconditional write (§5 W-B4); optimise only if it shows.
- **W4 — Parked coroutines must not count as ready.** `IdleTimeoutMs`
  returns 0 while any ready queue is non-empty; a coroutine parked on a
  reply is not runnable and must not sit where it forces that. Represent
  parked tasks separately from runnable ones so an all-parked reactor
  sleeps and is woken by W2's fd when the reply arrives. This is the half
  of the defect that costs a core; W2 without W4 fixes latency and leaves
  the spin.
- **W5 — The timer rounding stays.** `IdleTimeoutMs`'s round-up is correct
  for timers on its own stated reasoning. With W2 in place a message no
  longer waits on it, so the rounding stops being a message-latency
  property. Do not change it as part of this work.
- **W6 — Scope: `sched` and its backend only.** No change to the ring's
  format, to `RingTransport`'s contract, or to any caller. A wake is an
  addition beside the existing send, not a replacement for it — a build
  with the wake path disabled must still be correct, only slow. State that
  as an invariant and keep a config escape hatch for exactly one release,
  so a regression can be bisected against it.

---

## 4. Tasks

| # | Task | Gate |
|---|---|---|
| W1 | **The fd.** Per-reactor `eventfd`, created at reactor construction, registered with its `IoBackend`, drained on wake. Non-Linux backends (if any exist in tree) get a self-pipe or an explicit unsupported path — check before assuming epoll is the only backend | none |
| W2 | **The signal.** `RingTransport::TrySend` (or the layer directly above it — pick the one place every enqueue passes through, and say which and why) writes the target core's fd after a successful enqueue. Unconditional, per W3. The write must not fail the send: an `EAGAIN` on a saturated counter means a wake is already pending, and is success | W1 |
| W3 | **Parked ≠ ready.** Per W4: a parked coroutine leaves the ready queue and is returned to it by the wake that resumes it. `HasReadyTask()` and `IdleTimeoutMs()` see only runnable work. This is the invasive row — audit every enqueue and requeue site, and every place `ready_queues_` is inspected | W1 |
| W4 | **Drain on wake.** Waking on the fd must call `DrainInbox()` in the same iteration, before the sleep can be re-entered, or the wake is lost and the message waits for the next one. Assert this ordering with a test that sends to a reactor known to be idle and measures the delivery latency, not just its eventual arrival | W1, W2 |
| W5 | **Shutdown and teardown.** `kShutdown` is already a message rather than a flag, deliberately (workplan-crosscore P1: making it an atomic would put an atomic outside the ring indices). Verify the wake path does not create a second shutdown route or a teardown race — the fd is closed by its owner, after its last drain | W1–W4 |
| W6 | **Docs.** `docs/spec/sched.md` §4 gains the wake path and the parked-vs-ready distinction; the 1 ms floor is recorded as historical with its measurement cited; `docs/inflight/known-gaps.md`'s entry for this closes with its number. The 92–99% unaccounted reactor time is **not** closed by this work and stays open, still owed to the same section | W1–W5 |

---

## 5. W-B — the measurement

Same host and discipline as SS-B (Xeon 8488C, 4 physical / 8 logical, SMT
on, ext4 on a block device, `build-release`, 5 reps, median with full
spread, every ratio divided by a fresh null cell). Reuse SS-B's probes and
its `bench/run_ssb.py` contention gating so the two runs are comparable
cell for cell.

| # | Cell | What it judges |
|---|---|---|
| W-B1 | **B1 re-run**, one session per owner, three owner cores | The headline. SS-B measured **0.526**. If the wake path is the whole story this lands near 1.00; if it lands between, the remainder is the wire and the waiter, and *that* is the number §9 inherits |
| W-B2 | **The knob sweep re-run** (`wal_drain_interval_us` 1000/2000/3000/5000, plus 50/200/500) | SS-B's signature was shipped p50 tracking the block exactly while the seated control stayed flat at 23 µs. After the fix the tracking must be **gone** — shipped p50 flat across the sweep. This is the direct proof the mechanism was the one identified, and it is worth more than the ratio |
| W-B3 | **B4 re-run**, K = 1, 4, 16 | SS-B measured the arrival core at 89–95% busy from K = 1, polls 3.1M → 7.9M/s at a flat 0.059–0.068 µs per poll. After W3 an all-parked reactor should sleep: polls collapse, CPU collapses, statement p50 unchanged or better. Report all three |
| W-B4 | **The unconditional-signal cost** | W3's deliberate simplification, priced. A busy reactor now takes an extra write per enqueue and a drain per wake. Compare against SS-B's seated-arm baselines at S = 14, where the owner never slept and so had nothing to gain: any regression there is this write's cost, and it must be small enough that W3 stays unconditional |
| W-B5 | **B2 re-run**, S = 2, 4, 8, 14 | SS-B measured 0.93–0.99× and 521–547 × S. This must not move down. It is the no-regression cell for the regime the fix does not target |
| W-B6 | **`cores = 1` floor** | Guideline 2. A single-core instance sends no cross-core messages and must not pay for the fd. SS-B's `cores = 1` cells are the baseline; report the delta as a number and do not call 3% "parity" — SS-B's own note on unresolvable spreads applies |

Correctness in every cell as SS-B ran it: rows in = rows out per relation,
reported as a count, not as an assertion.

---

## 6. What this changes downstream, and what it does not

**Unblocks a decision.** `crosscore.md` §9 gets W-B1's corrected number
instead of SS-B's 0.526. If the corrected ratio is near 1.00, unconditional
shipping (D6) is vindicated and the routing question narrows to placement
alone.

**Does not close** the 92–99% unaccounted reactor wall clock. That is
scheduling-group accounting, a different defect in the same file, measured
again by SS-B (§5) and unchanged by anything here.

**Does not reopen stride.** SS-B did not run B3 because B2 tracked the
seated curve, so the tail-page falsifier never triggered. The stride
workplan stays in `docs/inflight/blocked/` and this work gives no reason to
move it.

**Does not touch** the two Part A findings (assertion enforcement on peers,
dedup eviction), the 992-byte reply rule — where SS-B corrected the row
count to **99** for a three-column narrow row, not 40, and recommended the
row count live beside the byte count in `statement_ship_service.hpp` rule 1
— or any separate track.

---

## 7. The deliverable

A results file beside SS-B's, same format: header with
`git describe --tags`, commit, worktree, host and SMT note, filesystem and
device, build flags, binary provenance, date, reps; the null cell and the
statement that every ratio is divided by it; each cell with its invocation
verbatim, median and full spread, and its correctness counts; findings
tagged measured or source-read with sites; and a section stating what the
run does not measure.

Then: mint the tag, and update `docs/spec/sched.md` §4 — this is the
section the last three runs have each added an owed item to, and this work
is the first to pay one back.
