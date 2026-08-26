# Memo — statement shipping × group commit: what re-concentrating writes is predicted to do

**Analysis, not a decision.** Nothing here is ratified, no constant is
proposed, and no task is authorized by it. It exists so that the
statement-shipping workplan's first section can be checked against a stated
prediction rather than against the intuition `bench/v2.1.0` already retired.
Every input below is tagged **measured** (with the cell that measured it) or
**predicted** (with the reasoning that produced it). They are never mixed in
one sentence.

Written 2026-08-26 on worktree `worktree-v2.2.0-pretasks-stmtshipping`; the
measurements it rests on are `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md`
(this host) and `bench/v2.1.0/results-multicore-writers-v2.1.0.md` (a
different host — 4 logical / 2 physical AMD EPYC, where every constant it
fitted belongs).

---

## 1. The mechanism, stated once

**source-read** (worktree `worktree-v2.2.0-pretasks-stmtshipping` at
`82a2749`): the group committer is installed as a **post-task hook** —
`src/server/expeditor.cpp:1662` on core 0, `src/server/core_runtime.cpp:736`
on every peer — and runs *"once per reactor iteration, after every runnable
statement has staged whatever it is going to stage, so one device sync covers
all of them"*. A committing statement stages its record and **parks**; the
hook is what wakes it. `wal_drain_interval_ns` arms the same drain on a timer
as a backstop, and `bench/v2.1.0` §6a **measured** that the timer is not the
live path: varying it over a 10× range does not move throughput.

Two consequences follow, and everything below is one of them seen from a
different side:

1. **A core's commit throughput is `batch / sync_latency`**, where `batch` is
   however many commits were staged when the hook ran. It is *not* one
   commit per sync.
2. **`batch` grows with the number of sessions committing on that core.**
   Nothing schedules it; it is an emergent property of how many statements
   happened to be in flight.

## 2. What rotation does to it — measured, and the reason it disappointed

Rotation places relations on different cores, so N writing sessions become
N/W sessions per core over W writer cores. By §1.2 each core's batch is
divided by W, and the engine then spends W syncs where it had been spending
one.

**measured**, `bench/v2.1.0` §6/§7 (that host): every multi-core cell ran at
965–1,071 commits/s **per writer core** — the volume's single-stream
`fdatasync` rate — while the single-core arm scaled at ≈ 470 × sessions.
Rotation won 1.751× at one session per writer core, where there was no batch
to lose, and **0.989×** at two.

**measured**, this host (T1b): one relation, N sessions, all on the owner
core — throughput ≈ 490 × S on core 0 and ≈ 590 × S on a peer, past S = 2,
with insert p50 pinned at 1.7–2.0 ms whatever S is. The 470-constant
re-fits at 490 on a different CPU and a different device, which makes it a
property of the mechanism rather than of the machine.

## 3. What shipping does to it — the prediction

Statement shipping routes a write statement **whole** to the core owning its
target range (`docs/spec/crosscore.md` §6), where it executes and commits under
that core's transaction machinery. The arrival core parks a waiter; the owner
core stages the commit; the owner's post-task hook syncs it with everything
else staged there in that iteration.

**Predicted, and it is the memo's point: shipping re-concentrates exactly
what rotation dispersed.** Under rotation a session writes only what its own
core owns, so W cores each carry S/W sessions' commits. Under shipping every
session that targets one owner's range commits *on that owner*, so the
owner's batch is the number of sessions targeting it — up to S — regardless
of where those sessions were accepted.

That is the same quantity T1b measured directly. **T1b is the shipped
workload's commit side, minus the wire cost**: N sessions committing on one
core, which is what shipping produces, and which today can only be built by
hunting for sessions the kernel happened to accept on the owner.

## 4. Three regimes, and which way each moves

| regime | sessions per owner core | batching effect of shipping | predicted net |
|---|---|---|---|
| **R1** | ≤ 1 | none — there was no batch to gain | small **loss**: the ring round trip plus the parked waiter, paid for nothing |
| **R2** | ≥ 2, writes concentrated on few owners | the owner's batch grows toward S | **gain**, bounded by the owner core's execution capacity rather than by its sync rate |
| **R3** | writes spread evenly over ranges each core owns | unchanged — every commit was already local | ≈ **neutral**, minus the shipping decision's own cost |

**R1 is where rotation wins today** and where shipping has nothing to offer.
**R2 is where rotation delivers nothing** and where shipping is predicted to
be positive. The two are complements, not competitors, and a placement policy
that knows which regime it is in could have both.

**T2's curve locates the boundary between them, and it is a step.**
**measured**, this run's §6: at exactly one session per writer core rotation
runs 2.00× (`cores = 4`); the moment one core takes a second session it
collapses to ~1.1 and stays there. At `cores = 8` the same step goes *below*
one and keeps falling — 1.04, 0.80, 0.60, **0.51** at 1.00, 1.29, 1.71 and
2.00 sessions per writer core — because the seven writer cores are pinned at
the device's seven-stream sync ceiling while the single-core arm's batch
grows with every session added. **So R1 is exactly "at most one session per
owner core" and R2 is everything past it**, which is a rule a router can
evaluate, not a constant that needs tuning.

**The possibility this raises, stated plainly because it inverts the usual
worry**: statement shipping may be *throughput-positive under load for the
very reason rotation was not*. The standard objection to shipping is that it
adds a round trip per statement. The measured law says the round trip is
priced against a ~1 ms sync (P4e priced the pipeline at 2.52 µs + 0.626 µs
per forwarded row **measured**, on the v2.0.0 host, in process), and that
concentrating commits is worth a factor of `batch`.

## 5. The arithmetic, under the measured law

For a workload of S sessions whose writes all target ranges owned by core B:

- **Today**: sessions not accepted on B cannot write those ranges at all.
  They are refused retryably (CC3) and the application must reconnect until
  the kernel hands it a session on B — which is precisely what every driver
  in `bench/` does (`collect_connections`), and what an application that has
  never heard of core placement does not. So the honest "before" is not a
  slower throughput; it is a **refusal**, and T5's counter is what measures
  how often it happens.
- **With shipping**: the S sessions all reach B. B's commit throughput is
  T1b's curve — **measured** ≈ 590 × S inserts/s on a peer core on this host,
  with per-statement latency pinned near two syncs — less the per-statement
  ship cost and less whatever the parked waiters cost the arrival cores.

**The size of the prize is measured, not assumed.** At `cores = 8` with
fourteen writing sessions, concentrating them on one core runs **8,625
stmt/s** against **4,365** spread over seven writer cores on identical work
(§6a) — **1.98×** for doing less parallel I/O, not more. That is the
arithmetic shipping inherits: it moves a workload from the spread arm to the
concentrated arm.

**The ceiling is the owner's CPU, not the owner's sync.** This is the part
worth writing down, because it is the opposite of the pre-v2.1.0 intuition:
sync is a *shared* trip that more concurrency makes cheaper per row, while
execution is per row and does not amortise.

**And the device gives concentration a second reason.** **measured**, this
run's §3a: the volume overlaps `fdatasync` streams to a peak of **3,768/s at
four** and then *declines* — 3,326 at five, ~3,102 flat through eight — while
one stream does 1,118. So the aggregate sync ceiling on this host is
~3,100–3,770/s **however it is reached**: seven writer cores in parallel
measured 3,400/s aggregate and one core batching fourteen sessions measured
3,482/s (§4). Spreading writers past four streams buys nothing and costs
18%. **Predicted consequence for shipping**: concentrating commits onto fewer
owner cores is not merely tolerable on this class of device, it is where the
device is fastest — and a design that ships writes to owners is, by
construction, a design that uses fewer streams and bigger batches. So the question a shipping
workplan must answer is not "how many syncs will the owner do" but "how many
statements can one reactor execute", and the answer is bounded by the
`batch = 1000` cells (T1a), where the sync has been amortised away and
throughput is set by everything else.

## 6. What would falsify this

Stated so the prediction is checkable rather than merely plausible:

1. **The owner's reactor saturates before the batch grows.** If shipped
   execution costs the owner materially more than local execution — the
   waiter, the ring drain, the reply — then R2's gain is eaten before it
   arrives. Measure: shipped statements/s on one owner against T1b's curve at
   the same S.
2. **The parked waiters cost more than the batch saves.** Every shipped
   statement parks a coroutine on its arrival core, and a reactor with a
   parked task does not block (`IdleTimeoutMs` returns 0 while any queue is
   non-empty, **source-read** `src/sched/scheduler.cpp:196-199`).
   **Partly measured, and the unmeasured part is named**: T4 (§8b) finds a
   loaded writing core spending only 4–6% of its wall clock inside task polls
   — 94–98% is the sync and the idle block, charged to no scheduling group —
   and at idle every reactor takes one poll per window and blocks properly.
   So a reactor has ample poll headroom for waiters *in the population the
   engine creates today*. What nobody has priced is **K parked coroutines per
   core at K = 16**, which is what shipping would create: T4's attempt to
   build that population from outside failed for three measured reasons
   (§8c), so this falsifier is open, not answered.
3. **Concentration hits a lock or a page.** T1b's relation is one btree with
   an ascending tail; every shipped INSERT lands there. The commits batch,
   but the *tail page* does not, and nothing in this memo measures contention
   on it separately from the sync.
4. **The regime is R3 in practice.** If real workloads spread writes evenly
   over ranges, shipping's batching gain never materialises and only its cost
   is real.
5. **The machinery shipping generalises fails badly under exhaustion.** §8d
   records a defect on the shipped-DDL path — the shape shipping would extend
   to DML: sustained shipped `CREATE INDEX` leaves a peer-owned relation
   permanently unwritable with a non-retryable `page id not found` after ~58
   builds, where the identical churn on core 0 runs 279 and then refuses by
   name (*"the table is full"*) with the relation intact. Refused attempts
   allocate too. A shipped DML path built on the same machinery would inherit
   it. This is not a prediction about throughput; it is a precondition.

## 7. Inputs, tagged

| input | value | source |
|---|---|---|
| Group committer is a post-task hook, once per reactor iteration | — | **source-read**, `expeditor.cpp:1662`, `core_runtime.cpp:736` at `82a2749` |
| The drain *timer* is a backstop, not the live path | — | **measured**, `bench/v2.1.0` §6a |
| Per-writer-core commit cap ≈ single-stream `fdatasync` rate | 965–1,071 /s | **measured**, `bench/v2.1.0` §6 (EPYC host) |
| One core's throughput against sessions | ≈ 470 × S (that host), ≈ 490 × S core 0 / ≈ 590 × S peer (this host) | **measured**, `bench/v2.1.0` §6; T1b here |
| Per-statement latency on one core, S ≥ 2 | 1.7–2.0 ms, flat in S | **measured**, T1b |
| Batching lifts one core's insert rate | 876 → 69,454 ips (batch 1 → 1000, 2 sessions) | **measured**, T1a |
| Pipeline cost per forwarded row | 2.52 µs + 0.626 µs/row | **measured**, P4e (v2.0.0 host, in process) |
| Device sync overlap: peak, and the decline past it | 3,768/s at 4 streams; ~3,102/s at 6-8 | **measured**, §3a here |
| Aggregate sync ceiling is the same by either route | 3,400/s (7 cores) vs 3,482/s (1 core, 14 sessions) | **measured**, §3a/§4 here |
| Concentration is where this device is fastest | — | **predicted**, §5 |
| The crossover is a step at one session per writer core | 2.00× at 1.00, ~1.1 above it (`cores = 4`) | **measured**, §6 here |
| Spreading is *negative* at 7 writer cores | 0.51× at 2.00 sessions/core | **measured**, §6 here |
| Concentrating beats spreading on identical work | 8,625 vs 4,365 stmt/s = 1.98× | **measured**, §6a here |
| A loaded reactor's poll headroom | 94–98% of wall time is outside every group | **measured**, §8b here |
| Writes refused today when the client does not route | 80–92% of write statements | **measured**, §9b here |
| K = 16 parked coroutines per core | — | **not measured**, §8c says why |
| Shipping re-concentrates commits onto the owner | — | **predicted**, §3 |
| R2 is throughput-positive | — | **predicted**, §4 |
| The ceiling under shipping is the owner's CPU | — | **predicted**, §5 |

## 8. What this memo does not do

It does not choose a batching strategy for shipped statements, propose a
`ship` threshold, or say when a statement should be refused instead of
shipped. T2's crossover curve is the input to that decision and this memo
deliberately stops short of making it: the memo is what the workplan is
checked against, not the workplan.

**The three claims the workplan's first section should be checked against**,
stated so a later reader can mark each right or wrong.

**Judged 2026-08-26 —
`bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md`.** SS1–SS4 are
built and SS-B has run against them. The pointer this section was owed is
that file; the verdicts, in the order the claims are stated below:

1. **Upheld on this memo's own test, missed literally.** Throughput tracks
   521–547 × S over S = 2…14 and the local-vs-shipped gap is 1–7%, inside a
   1.016 noise floor — but shipping is never *faster* at any S and the
   ratios show no trend in S. Both readings are recorded there; the
   literal form is rescued only by the fact that the "before" for these
   sessions was a refusal, and against zero throughput any of it is
   positive.
2. **Upheld, by more than this memo predicted.** Not "a small net loss from
   a round trip and a waiter" — a factor of two, in three independent cells
   (0.526, 0.429, 0.531). And the cause is neither the round trip nor the
   waiter: it is that an idle reactor sleeps a whole millisecond and
   nothing wakes it on a ring message. §3's arithmetic — the wire at ~1/40
   of a sync — is right and its conclusion does not follow from it.
3. **Unproven, not disproven.** The owner runs at 11–24% busy at the top of
   the curve this harness can build, so its execution capacity is not
   tested. Two things this memo could not have anticipated: the resource
   that saturates under shipping is the **arrival** core, and shipping
   moves the owner's ceiling *further away* — a shipped statement costs the
   owner 1.8–2.2 µs per poll against 4.4–4.9 seated, because the socket and
   the render happen elsewhere.

1. Shipping is **throughput-positive** where more than one session targets an
   owner core, and the margin grows with the session count — because it moves
   the workload from §6's spread arm to its concentrated arm.
2. Shipping is **throughput-negative and should not be used** at or below one
   session per owner core, where rotation already wins 2.00× and shipping can
   only add a round trip and a waiter.
3. The binding constraint on a shipped workload is the **owner core's
   execution capacity**, not its sync rate — so the number a workplan needs
   next is what one reactor can execute once the sync has been amortised
   away, which §3b says this harness cannot resolve.
