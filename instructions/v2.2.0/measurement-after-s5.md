# SS-B — what to measure, why, and how the result is read

Drafted 2026-08-26 against `main` at `582d914` (SS1–SS5 built plus the SS2
review's scope fixes). Correctness verification runs on its own track and is
assumed; this file is measurement only.

Discipline: every claim **measured** (with its invocation) or
**source-read** (`path:line` + commit); `build-release` only; **every A/B
divided by a null cell** — the pretask run measured ~10% ordering bias in
this harness (`cores = 1` against `cores = 1` returned 1.099), so no ratio
is quoted raw. Results named by `git describe --tags`. This order decides
no constant and no policy.

---

## Why this run exists

`docs/known-gaps.md:910` records the engine's own position:
**"Statement shipping is built and unmeasured."** The wire, the waiter, the
owner-side execution, the dispatch fork and the counters are in; not one of
`docs/memo-shipping-and-group-commit.md` §8's three claims has been judged.

That memo was written before the build precisely so the build could be
checked against it rather than described by it. Its claims:

1. shipping is throughput-**positive** where more than one session targets
   an owner, with margin growing in the session count;
2. shipping is throughput-**negative** at or below one session per owner;
3. the binding constraint under shipping is the owner's **execution
   capacity**, not its sync rate.

Claim 2 is the uncomfortable one: this build ships **unconditionally** (D6),
so if claim 2 holds, the loss is being paid in production today and nobody
has priced it. A missed claim is the point of the exercise, not a failure
of it.

Two facts frame every cell below, both measured in the pretask run:

- **A statement costs 21–23 µs; a sync costs ~0.9 ms.** The wire is ~1/40
  of a sync. The standard objection to shipping — "it adds a round trip" —
  is arithmetically small, and the run must confirm that rather than
  assume it.
- **Commit batching, not parallelism, sets ingest throughput.** Row-per-
  commit gave 876 ips; 1000-rows-per-commit gave 69,454 ips on one core.
  Concentrating commits beat spreading them by up to 1.98× wherever any
  core carried more than one session. Shipping re-concentrates onto owners
  exactly what placement disperses — that is its whole performance thesis.

---

## Setup

Host: pretask host or better. State CPU count, model, **and whether SMT is
on** — the pretask host was 8 logical / 4 physical and every ratio there
carries that caveat. `--workdir` on a block device, never tmpfs, no
`--force`. Quiet box.

**Null cell first**: `cores = 1` vs `cores = 1`, same shape, 5 reps. If the
bias has moved from ~1.099, report the new value; do not reuse the old one.

**Valid baselines, and only these two**: the same tree with the session
seated on the owner (isolates wire + waiter), and the pre-shipping tag
(isolates what the version changed). `bench/results-multicore.md`'s 1.05×
is not a baseline for anything here.

**One harness caveat that voids cells silently.** A shipped reply is capped
at 992 bytes (1,024-byte ring payload less a 32-byte header) — roughly 40
wide rows — and SS1 refuses rather than truncates past it. So: size read
cells under the cap, and **every cell reports attempted / executed /
refused**. A cell whose refusals exceed 1% reports that number beside its
result or is void. Refusals must never become denominators.

Also re-check the client ceiling on this host (`client_ceiling_probe.py`);
the pretask host's CPython driver capped near 56k stmt/s, and any cell
within 2× of the ceiling is the driver's number, not the engine's.

Probes that already exist and should be extended rather than replaced:
`refusal_baseline_probe.py`, `single_relation_probe.py`,
`txn_batch_probe.py`, `reactor_accounting_probe.py`,
`parked_coroutine_probe.py`, `run_t1.py`, `run_t2.py`.

5 reps minimum per cell; median and spread; rows in = rows out per relation
in every cell.

---

## B1 — The R1 price

**Measure.** One writing session per owner core, arriving on a *foreign*
core and shipped, against the same session seated on the owner. `cores` 4,
one relation per writer core.

**Background.** T2 measured that rotation wins 2.00× at exactly one session
per writer core and collapses to ~1.1 the moment any core takes a second —
the crossover is a step, not a slope, and it is set by the *busiest* core.
R1 is that winning configuration, and it is the one place the memo predicts
shipping subtracts: a round trip and a waiter bought for nothing, because
the owner had no batch to gain.

**Read the result as.** The ratio and the per-statement latency delta,
divided by the null cell. State plainly whether claim 2 holds. Then record
the framing, because the number alone misleads: R1's honest "before" is a
**refusal**, not a slower statement — pre-shipping, that session simply
could not write. A small loss still converts a failure into work. That
belongs in the results file; the routing decision it feeds
(`crosscore.md` §9) is the operator's.

## B2 — The R2 curve

**Measure.** S = 2, 4, 8, 14 sessions on foreign arrival cores, all
targeting **one** owner's relation, autocommit. Paired arm: the same S
seated locally on the owner.

**Background.** T1b measured N sessions committing on one core at ≈ 590 × S
with p50 pinned at 1.7–2.0 ms — sessions share a sync trip rather than
queueing behind the device. That curve is the state shipping re-creates, so
T1b is effectively the shipped workload's commit side measured with the
wire removed. The 470→590 refit across two different CPUs and devices
suggests the constant is a property of the mechanism, not the machine.

**Read the result as.** Three verdicts:
- does throughput track ≈ 590 × S (claim 1);
- is per-statement latency near two syncs;
- **the local-vs-shipped gap is the wire + waiter cost at scale** — the
  quantity the memo's whole case rests on, since ~20 µs against ~0.9 ms
  should make it nearly invisible.

For claim 3, report owner-core CPU and the `polls`/`polled_us` block at the
top of the curve. If the owner saturates, name the resource. If it does
not, claim 3 is **unproven rather than disproven**, and the file says so —
the pretask run already judged that a single reactor's execution ceiling is
not reachable with this harness.

## B3 — The tail page

**Measure.** Only if B2's curve departs from ≈ 590 × S. Same S over **1, 2
and 4 relations on the same owner**: one relation concentrates the tail,
four spread it across four tails on the same core, same sync, same reactor.

**Background.** This is the memo's falsifier 3 and the last live remnant of
the stride question. Every shipped INSERT in B2 lands on one btree's
ascending tail; commits batch, but the *page* does not. Stride existed to
split exactly that serialization, and T1b weakened its premise by showing
the hot relation is not the throughput ceiling — B3 is where that premise
gets a second hearing on its own terms.

**Read the result as.** The delta between 1 and 4 relations is page-level
serialization with sync, reactor and wire held constant. **This cell, not
this version, decides whether stride returns.** Report the number and
recommend nothing.

## B4 — The waiter population

**Measure.** K = 1, 4, 16 concurrent shipped statements per arrival core.
Instrument: `polls` / `polled_us` on the **arrival** cores, plus arrival-core
CPU and statement p50.

**Background.** The memo's only still-open falsifier. T4 could not build a
parked population to price it; shipping builds one by construction, since K
in-flight shipped statements *are* K parked waiters.
`Scheduler::IdleTimeoutMs` returns 0 while a parked task sits in a ready
queue (`src/sched/scheduler.cpp:196-199`, source-read), which is the
mechanism that would produce a spin.

**Read the result as.** The spin signature is polls climbing while polled
stays flat. Either answer closes pretasks §8c honestly. Record what is
found; fix nothing.

## B5 — Demand conversion, and the residue

**Measure.** `refusal_baseline_probe.py` unchanged, unrouted client,
`cores` 4 and 8. Then break the remaining refusals down **by class**.

**Background.** The pretask run measured that a client which does not hunt
for the owner core has **80–92% of its write statements refused** — engine
counters and driver tallies agreeing exactly, four cells out of four. Every
one of those is a statement shipping should convert into work. The counter
`cross_core_write_refusals` keeps its exact semantics across the change, so
the series spans both eras.

**Read the result as.** Two separate readings from one run:
- **Conversion**: the 80–92% should go to ~0 shipped-and-executed for
  autocommit, with the counter flat at zero.
- **Residue**: whatever remains is not noise — it is the scope shipping
  deliberately does not carry (statements inside an explicit transaction,
  statements spanning two owners). That breakdown **is** the evidence base
  `known-gaps.md` says the 2PC/R6 decision must be designed from, and this
  is its first reading. Report it as a distribution, not a total.

## B6 — The abandoned transaction

**Measure.** Sustained shipped writes from one arrival core past several
lease blocks (≥ 20,000 statements). Report refill frequency and any latency
step at block boundaries. Instrument: PW7's refill-lag triple
(`*_refill_submit_lag_max_us` / `_grant_` / `_resume_`), which separates the
arrival core's own reactor from the ring and from core 0.

**Background.** The dispatch fork sits after `BeginWrite`, so every shipped
write opens a transaction on the **arrival** core and abandons it: one id
out of that core's 4,096-id lease block, plus a `TXN_BEGIN`/`TXN_ABORT`
pair appended to a log that shipping otherwise leaves idle. The transaction
lives microseconds, pins no read horizon and blocks no purge. The engine
documents this as deliberate and names SS-B as where the trade is priced.

The alternative — moving the fork above `BeginWrite` — costs a second parse
and catalog resolve on every **local** write, i.e. a per-statement cost on
the path this version is measured against, to save one on the path that
already pays a round trip.

**Read the result as.** Both sides, so the trade is a comparison rather than
an assertion: the shipped path's refill cost measured here, against the
local path's cost of the alternative estimated from parse/resolve timings
already in the tree. Whether the fork moves is the operator's call.

---

## The deliverable

A results file beside `bench/v2.1.0`'s, same format:

1. Header: `git describe --tags`, commit, worktree, host + SMT note, FS and
   device, build flags, date.
2. Null cell value, and the statement that every ratio is divided by it.
   Client-ceiling re-check.
3. Per cell: invocation verbatim, median and spread,
   attempted/executed/refused, rows in = rows out.
4. **The memo's three claims, each marked upheld or missed**, naming the
   cell that judged it.
5. Findings, each tagged measured or source-read, each with its site.
   Findings owned elsewhere are handed over, not fixed: §8b's 94–98%
   unaccounted reactor time (`docs/sched.md` §4), the 992-byte reply rule
   (`statement_ship_service.hpp` rule 1), anything B3 surfaces (the stride
   file).
6. **What this run does not measure**, stated plainly: explicit-transaction
   shipping and multi-owner statements (both out of scope by design), reads
   beyond the 992-byte cap, and any cell within 2× of the client ceiling.

Then, if correctness is green and the claims are judged: **mint `v2.3.0`**
and rename the file by it. Shipping is an architectural fork and every later
comparison will baseline here; it should not be remembered by an interim
name. Close `known-gaps.md`'s 80–92% entry with its number, and leave the
R6 entry open, now pointing at B5's residue breakdown.
