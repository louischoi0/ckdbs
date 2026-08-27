# Instructions — what must land before statement shipping starts

Target tree: `main` at `2b00f12` or later. Same discipline as
`bench/v2.1.0`: every claim tagged **measured** (with its invocation) or
**source-read** (with `path:line` and commit id); `build-release` only for
numbers; results named by `git describe --tags`; **no constant is decided by
any task below** — findings are reported, decisions are the operator's.

Why this list exists: v2.1.0 established that the binding constraint on
multi-core ingest is the per-core group committer against the device's
single-stream `fdatasync` latency (§6, confirmed and corrected by §6a), not
CPU, not the device, not the cross-core machinery. Statement shipping will
change where commits happen — it re-concentrates foreign-class writes onto
owner cores — so shipping must be designed against a measured picture of
commit batching, not the pre-v2.1.0 intuition that per-row wire cost is the
enemy. The tasks below complete that picture and retire the engine debts
that shipping would otherwise amplify.

Order matters; each task states what downstream work consumes it.

---

## T1 — The two workload cells v2.1.0 could not measure

`bench/v2.1.0` §10 states plainly: the harness exercises N non-interfering
relations, one autocommit statement per row. Extend
`tools/multicore_benchmark.py` (or a sibling driver) with two cells:

**T1a — transaction-wrapped bulk insert.** Same relations, same rows, but
each session wraps its inserts in explicit transactions of `--batch` rows
(sweep: 1, 10, 100, 1000). One commit per batch means one `fdatasync` per
batch, which steps outside §6's law entirely. Report ips and the ratio per
batch size, `cores = 1` against `cores = 4` rotate, 5 reps.
This is the number that says whether the per-core sync cap is a real-world
constraint or an autocommit artifact — and therefore how much of shipping's
design budget should go to batching shipped statements.

**T1b — single-relation ascending-pk contention.** One relation, N sessions
all inserting ascending keys. Today every session must land on the owner
core (rotation places one owner; sessions elsewhere cannot write), so this
measures the serialized baseline the stride proposal claims to beat. Report
ips at 1, 2, 4 sessions, and insert p50/p99. Without this baseline there is
no number for stride or shipping to improve on.

Two harness facts, learned the hard way in v2.1.0 §2 and to be respected:
ckdbs refuses `INSERT` with a column list — the Keystone pk is implicit
(`tools/multicore_benchmark.py:288`) — and a peer's first INSERT answers
with a retryable lease refusal (PW1b) that must be retried and *counted*,
never folded into intended-row denominators.

Consumed by: the stride-forest go/no-go, shipping's batching design, T6.

## T2 — Locate the crossover

C2 established rotation wins 1.751× at one writing session per writer core
and 0.989× at two. The boundary between them is bracketed, not located
(§11-1). Sweep fractional sessions-per-core by table count at
`--cores 4 --placement rotate`: tables 3, 4, 5, 6 give 1.00, 1.33, 1.67,
2.00 sessions per writer core. 5 reps each, report the ratio curve.
Consumed by: any placement policy, and shipping's decision of when to ship
versus refuse under load.

## T3 — Discriminate the four-core-server effect

C1 showed `cores = 4` with everything on core 0 beats `cores = 1` by 1.071×
aggregate and 1.457× insert p50, with **nothing cross-core happening at
all** — currently a larger effect than rotation's entire contribution, and
undiscriminated (§11-3). Candidates named in the file: four WAL anchors
(`wal_anchor_count=4`), per-core extent leases, background work moving off
core 0. Design runs that separate them — e.g. `cores = 4` with peer
listeners off and background features toggled where config allows, or
`SHOW META` deltas across arms. If config cannot separate them, say so and
stop; do not patch the engine to find out.
Consumed by: honest attribution in every future multi-core ratio; if a free
1.07× lives in the four-core server, shipping's gains must be measured net
of it.

## T4 — Price the parked-coroutine spin before shipping multiplies it

Statement shipping parks a waiter on the arrival core for every shipped
statement (the 6b-2/6b-3 `IndexBuildClient` shape). v2.1.0 §8 shows what
parked coroutines do today: `Scheduler::IdleTimeoutMs` returns 0 while a
parked task occupies a ready queue (`src/sched/scheduler.cpp:196-199`,
source-read), and the trx-id refill leg spans 19,000–24,000 reactor
iterations under load — plus H1's unexplained tolerance of a 924 ms submit
stall that never reaches throughput (§11-4). Shipping turns a rare parked
coroutine into a steady-state population.

Deliverables, in two halves:
- **Measure**: CPU cost of the spin under a sustained parked population.
  A probe that holds K coroutines parked per core (K = 1, 4, 16) under the
  T1b workload and samples per-core CPU and statement p50. This bounds what
  shipping's waiters will cost before a line of shipping exists.
- **Instrument** (the one sanctioned code change in this list, owed to
  `docs/sched.md` §4 per v2.1.0 §11-5): an accessor for the per-group
  `consumed_ns_` (`include/kds/sched/scheduler.hpp:250`, currently private,
  unprinted) surfaced through `SHOW META`, so group accounting can be
  compared against wall time — the `fdatasync`-charged-to-no-group question
  becomes measurable from outside. Tests, review, and a named worktree as
  for any engine change.

Consumed by: shipping's scheduler-interaction design; whether floors or a
park-aware idle policy must precede shipping.

## T5 — Build the write-refusal counters

`docs/crosscore.md` §6 specifies per-core counters keyed
(home core, target core, relation) for retryably-refused cross-core writes;
they are specified and **unbuilt** (source-read, no implementation sites).
Build them now, exposed via `SHOW META`, with the known undercount stated
in a comment: the peer-listener guard refuses foreign writes before parsing,
so that class is invisible to a relation-keyed counter.

One instrument, two eras: read now, it baselines how often today's refusals
occur per workload; read after shipping lands, the same counter reports only
the residue shipping cannot convert — true multi-core transactions — which
is exactly the evidence base `docs/known-gaps.md` says 2PC must be designed
from. Half a day; do it before shipping so the before-era exists.

## T6 — The design memo: shipping × group commit

Analysis, not code. One page in `docs/`, stating with §6's arithmetic what
shipping is predicted to do to commit batching: shipped statements from N
arrival cores execute and commit on the owner core, so shipping
*re-concentrates* commits that rotation dispersed — the owner's post-task
hook (`src/server/expeditor.cpp:1650-1660`) batches them into one sync.
State the predicted regimes under the measured law
(`1000 × writer_cores / (470 × sessions)` and the ~0.94 ms single-stream
sync latency), including the possibility that shipping is
*throughput-positive under load for the very reason rotation was not*.
Mark every input as measured (cite the v2.1.0 cell) or predicted. T1a's
numbers slot in when they land. No constant, no commitment — the memo is
what the shipping workplan's first section will be checked against.

---

## Done means

T1–T3 reported in a results file beside `bench/v2.1.0`'s, same format,
tagged by the tag current at the run. T4's probe reported; T4's accessor
merged with tests. T5 merged with its undercount comment and a baseline
reading recorded. T6 in `docs/`. At that point the statement-shipping
workplan can be drafted against measured commit behavior, a priced waiter
population, a refusal baseline, and a stated batching prediction — instead
of against the assumption v2.1.0 already retired.
