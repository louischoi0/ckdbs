# Work order BL — no session is pinned: a balancer between connection and core, and one session axis, priority

**Amended 2026-10-12**: session affinity struck (W4); priority's values
set as four context levels (W5, W6). The file keeps its name,
`…-priority-affinity`, because the index and the marks files cite it by
path; the namespace's affinity (NS10, D18) is untouched.

Written 2026-10-11 **in the main checkout on `main` at `1d389650`**
(`v2.7.0-775-g1d389650`), with no worktree: a document-only change, pushed
to `main` directly as `raft-marks-2026-10-11.md` §1 directs for documents.
It follows the operator's words of 2026-10-11, recorded in English
(`raft-marks-2026-10-11.md` §4-§7):

- **W0:** *"I am thinking of a scheme with a balancer between the
  connection and the core, and scheduling according to the session's
  priority. That is, a session is never pinned to a core at any time."*
- **W1:** *"So the session has to be split along two axes, priority and
  affinity. Priority is how many resources are allocated, and how quickly,
  within one core; the affinity setting is how much the session moves
  between cores."*
- **W2:** *"Now write a work order for this, including the measurements,
  the work and the intent. If one already exists, amend that work order.
  Write it on the premise that BI has been completed."*
- **W3:** *"In any case, putting a balancer between the session and the
  core is recommended, right?"* CLA answered yes, on two conditions: the
  balancer is a decision taken on each core at a transaction boundary, not
  a central dispatcher every transaction passes through; and its premise is
  measured first, since BA's census never ran a balanced arm (§1.7). Both
  conditions are BL-R1 and BL-S1 below.
- **W4** (2026-10-12, `raft-marks-2026-10-12.md` §4): *"Discard only
  session affinity, and amend the BL document."* It answered CLA's
  analysis, which recommended striking the session axis and keeping the
  move's restraint as the engine's own constants, and which kept the
  namespace's affinity a separate question. W4 strikes W1's affinity half.
- **W5** (2026-10-12, `raft-marks-2026-10-12.md` §5): *"There is something
  to decide before the document is finalized: how the session priority
  levels are set. I want to design a set of levels that applies context -
  a level that puts priority on the single-row updates that carry business
  logic, a level that runs in the background, a level for query-analysis
  reads that gives way first, and so on."*
- **W6** (the same day, §5): *"Update the workplan document according to
  CLA's proposal."* It adopts CLA's answer to W5 whole, not item by item:
  the four levels of BL-R2, BL-Q4 (b), and BL-Q8, BL-Q16, BL-Q17 and
  BL-Q18 as proposed.

**Written, not opened.** W0, W1, W4 and W6 are the operator's decisions,
W5 the direction W6 settled. They stand here as BL-R1 (no session pinned
for its life) and BL-R2 (priority, as the four context levels); BL-R3
(affinity) is struck by W4. BL-Q4, BL-Q8 and BL-Q16..Q18 are adopted whole
by W6; every other BL-Q item W4 did not strike is unmarked, BL-Q0
included. Everything else is CLA's proposal, in §2 and §4. **Reviewed in
part**: the 2026-10-12 amendment had its `critics-developer` review
(§6); the review of the order as a whole is still owed before BL-Q0 is
put to the operator.

**The premise (W2).** This order is written as though BI
(`workorder-bi-checkpoint-off-the-reactor.md`) has closed: a checkpoint
run is a `system`-group task that takes one `Step` per poll and parks on
its log and data-file syncs (BI-R1, BI-R2). `[source-read]` At
`1d389650` BI is written and not opened: BI-Q0..Q11 are unmarked, and
BA's close carries the checkpoint half of P12 as waiting on BI-Q1
(`workorder-ba-parallelism.md`, BA-S17). **BL-Q0 therefore opens BL only
after BI-S5 has closed**, and BL-S1 re-reads whatever BI-R5 left on the
reactor as a measured input rather than an assumption (§1.6).

**Claim tags:**
- `[source-read]` with `path:line` at `1d389650`, read and not run;
- `[design]` for CLA's reasoning about code not yet written;
- `[measured]` with the invocation (none in this order);
- `[quiet-wrong]` marks an item where a wrong choice turns a refusal or a
  correct answer into a wrong one with no error.

The operator's words, and a rule that restates them, are decisions rather
than claims, so they carry no tag.

**The letter.** `[source-read]` At writing, the last order on `main` was
lettered BK (`workorder-bo-sys-statistics-show-sugar.md`, relettered BO on
2026-10-12 when three orders were found to carry BK); no file under
`instructions/` or `docs/` used BL, the next free letter.

**BL against other orders and notes.**
- **BI**, the premise above. BL builds nothing in the checkpointer.
- **BA, closed 2026-10-10.** BA-R13 proposed placement by load *at the
  accept only*, and BA-Q12 was marked *"as proposed; migration at a
  transaction boundary is not decided here and stays with
  `docs/pending/session-load-balancing.md`"*
  (`workorder-ba-parallelism.md:1120`). BA-S16 never opened, because the
  census did not show P11 material (§1.7). `[design]` BL takes BA-R13's
  accept placement as its first decision point (BL-R5) and the migration
  BA-Q12 left out as its second.
- **`docs/pending/session-load-balancing.md`.** W0 is the operator's
  ruling on that entry: a session is never pinned. Per
  `docs/pending/README.md`, the entry is deleted by BL-S0 and its ruling
  lives here.
- **CN-10** (`docs/conceptnotes/cn10-unbounded-sessions-bounded-transactions.md`).
  §3.4 proposes migration at a transaction boundary, triggered by §3.5's
  admission; its decision U6 is unmarked and U8 orders §3.4 last.
  `[design]` BL builds §3.4's carrier (BL-S2) with a load trigger instead
  of an admission trigger, and leaves §3.5's admission queue to CN-10.
  BL-Q11 asks whether BL's build marks U6.
- **BO** (`workorder-bo-sys-statistics-show-sugar.md`, not opened; lettered
  BK until 2026-10-12). The operator excluded `sys.sessions` from it
  (`raft-marks-2026-10-11.md`
  §2). BL needs per-session counters somewhere (BL-R8); BL-Q10 asks where.
- Not part of AR0 §8's chain.

---

## 0. What BL is

**The intent, in one sentence:** a session's core is a decision the engine
re-takes, by load alone, at every point where
moving it is free of correctness risk - and within a core, sessions share
the reactor by the context level they declare rather than
first-in-first-out.

**Three goals:**
- **G1 — no session is pinned for its life** (W0). A session is placed at
  its accept and may move at any transaction boundary thereafter.
- **G2 — placement sees load** (W0, W3). The kernel's `SO_REUSEPORT` hash
  stops being the only placement, and a skew that develops after the
  accept can be undone.
- **G3 — a core's reactor time is shared by session priority** (W0, W1,
  W5). Within the `foreground` group, a business-logic update is picked
  first when it becomes ready, an analytic read gives way first, and
  background work runs on what is left (BL-R2).

**Non-goals, stated so no stage grows into them:**
- **One session using several cores.** A statement runs on one core
  (`crosscore.md` CC1); balancing spreads *several* sessions, never one
  (`docs/pending/session-load-balancing.md` §2, which BL-S0 deletes).
  Intra-statement parallelism is a different order.
- **Moving a transaction.** A transaction stays on the core it began on
  (BL-R1's boundary).
- **Preemption.** `sched.md` §3 forbids it; priority acts only at yield
  points (BL-R7).
- **A central dispatcher on the per-transaction path** (W3's first
  condition).
- **CN-10 §3.5's admission queue** and a kernel BPF socket selector.
- **A per-session setting for how readily a session moves** (W4). The
  balancer's restraint is the engine's, the same for every session
  (BL-R4).

### 0.1 The session axis (W1, W4, W5)

`[design]` W1 named two settings per session; W4 struck affinity, so one
remains. It is defined by the mechanism that reads it, so it is not a
number without a consumer. W5 made its values context levels rather than
points on one scale (BL-R2):

| axis | what it governs | read by | unit of effect |
|---|---|---|---|
| **priority** | one of four context levels, each a bundle of a share, an optional latency class, a walk slice and an eligibility rule | the `foreground` group's pick (BL-R7) and a walk's slice length | a poll of one of the session's tasks |

- **The balancer reads no session setting**: it reads load, and the move
  cost and dwell it applies are the engine's constants (BL-R4). A core's
  pick reads priority; the balancer never does.
- **At `cores = 1` the balancer does not exist** (BL-R9); priority still
  applies, because sessions still share one reactor.

### 0.2 What CLA told the operator before this order

Recorded as context, not as a ruling. Asked whether this scheduling is
novel or over-engineering, CLA answered `[design]`, from general
knowledge not verified against sources in that reply:
- **The priority axis is standard** in commercial engines: Oracle Database
  Resource Manager, SQL Server Resource Governor (`IMPORTANCE`, CPU
  percentages), Db2 Workload Manager, Teradata TASM, MySQL Resource
  Groups, CockroachDB admission control, ScyllaDB's scheduling-group
  shares (the closest precedent: thread-per-core, share-proportional).
- **The affinity axis is rare, but not because it is new.** A
  thread-per-connection engine gets it from the OS scheduler for free;
  ckdbs lost it by choosing thread-per-core, and the balancer restores it.
- **By layer:** the walk-boundary yield and the checkpoint's release of the
  reactor are defects fixed (BA-S15, BI); the balancer and the weighted
  pick are justified mechanisms; exposing both axes as user settings before
  a workload asks for them leans toward over-engineering. BL-Q8 carries
  that question to the operator.

---

## 1. Survey at `1d389650`

### 1.1 Where a connection lives

- `[source-read]` Above one core every core binds the default port with
  `SO_REUSEPORT` (`src/server/tcp_server.cpp:90`) and the kernel's hash
  picks the accepting core. `force_listener_handoff` instead has core 0
  accept and hand each connection out round-robin
  (`src/server/expeditor.cpp:1581`;
  `include/kds/server/connection_handoff.hpp:46-75`, `NextCore` at `:60`).
- `[source-read]` The accepting core's `AdoptConnection`
  (`tcp_server.cpp:326`) builds a `Connection` in place in
  `std::unordered_map<int, Connection> clients_`
  (`include/kds/server/tcp_server.hpp:421`) and registers the fd with its
  own reactor. Each statement frame is submitted to that core's scheduler
  as one `kForeground` coroutine (`tcp_server.cpp:612-615`).
- `[source-read]` The KWP protocol state borrows its session:
  `KwpSession` holds `Session& session_` (`kwp_session.hpp:352`) into the
  `Connection`, which lives by value in a map node.
- `[source-read]` `debug_text_port` is bound by core 0 alone, so every
  newline-protocol session runs on core 0 (CLAUDE.md's KWP row; the
  pending entry's §2 at `ef03771`).

### 1.2 What is pinned to a core, and what is only data

- `[source-read]` **A transaction is pinned.** Its in-flight slot is in its
  core's table (`kInFlightSlotsPerCore = 1024`,
  `include/kds/txn/instance_visibility.hpp:306`; the refusal at
  `src/txn/manager.cpp:152-153`), and its waits and commit park on that
  core's reactor (CN-10 §3.4; `crosscore.md` CC3, CC11).
- `[source-read]` **A prepared statement is text.** `KwpSession::Statement`
  is `sql`, `pattern_id` and `param_count` (`kwp_session.hpp:282-286`), and
  a portal is the statement name plus its substituted text (`:288-291`);
  both maps are per session (`:366-367`). `[design]` A statement is
  therefore compiled per execution against the executing core's catalog,
  and `pattern_id` names a row of the instance's one `sys.patterns`
  (AT-S7). Moving a session between executions carries no compiled state.
  This narrows CN-10 §3.4's prepared-statement concern to one claim BL-S2
  must pin with a cell: no per-core state is keyed by a session's handle.
- `[source-read]` **The cancel registry is already the instance's**
  (`include/kds/server/cancel_registry.hpp:28`, BA-S15), as CN-10 §3.4
  required; a move re-homes the entry's target, not the registry.
- `[source-read]` **A session holds a clock** for the portal-idle timeout
  (`kwp_session.hpp:228`), which is the source core's.
- `[design]` **Between transactions a session holds no lock, no borrow, no
  read view, no reader lease and no statement epoch**: each of those is
  statement- or transaction-scoped. That is what makes the boundary safe,
  and BL-R1's predicate is written to it.

### 1.3 The carrier that exists

`[source-read]` `ConnectionHandoff` is `sched.md` §5's second
write-then-kick consumer: an inbox per core under its latch, `Offer` then
kick, and the destination's parked `TcpServer::Host` task `Take`s and
adopts (`connection_handoff.hpp:67`, `:75`; `tcp_server.cpp:431`). It
carries an fd at accept time only: *"A session crosses once, at placement,
and runs to completion where it lands"* (`sched.md` §5).

### 1.4 How a core shares its reactor today

- `[source-read]` Three groups, `foreground`, `maintenance`, `system`
  (`include/kds/sched/task.hpp:18-24`), picked share-proportionally with
  shares `{1000, 100, 50}` (`include/kds/sched/scheduler.hpp:62`), at most
  64 tasks per iteration (`:58`), under `sched.md` §4's two floors.
- `[source-read]` **Within a group the queue is FIFO**: one
  `std::deque<TaskPtr>` per group (`scheduler.hpp:315`), popped from the
  front (`src/sched/scheduler.cpp:264-265`). Every client statement is
  `kForeground` (`tcp_server.cpp:612-615`). **No session-level weight
  exists**, and consumed time is accounted per group only (`sched.md` §4).

### 1.5 What yields, and what does not

- `[source-read]` Since BA-S15 a `SELECT`'s outermost walk yields every 64
  pages; *"A write, a pk descent, a join's inner walk under one outer page
  and an index walk do not yield; `exec::Budget` bounds them"* (`sched.md`
  §3).
- `[design]` **A level pick can only act at a yield or a statement
  end.** A multi-row `INSERT`, an `UPDATE`/`DELETE` walk or an index walk
  holds its reactor to the end whatever its session's priority. BL-R7
  names this as the priority axis's remainder; BL-S1 measures how much of
  a mixed load it is.

### 1.6 What still holds a reactor once BI has closed (the premise)

`[design]` Under W2's premise, a checkpoint run holds its reactor for one
`Step` plus the remainders BI-R5 names (a step's page writes, the bounded
batch gate, the per-run gate under BI-Q4 (b), the claim wait's sleeps).
What else still holds a reactor, read from CLAUDE.md's rows and BA's close:
- the statements of §1.5 that do not yield;
- the `relaxed` transaction-id carve's stall (BA-S17's "Carve and
  checkpoint stalls", `known-gaps.md`);
- core 0's role: the log's owner, its drain and `debug_text_port`.

BL-S1 measures each as a per-core reactor-hold distribution, because each
is load the balancer will see and priority cannot reorder.

### 1.7 What BA measured about placement

`[source-read]` BA's census read P11 as **"not shown"**, not "immaterial":
*"no balanced arm was run"*; in the proxy, runs with a session on every
core were faster by over 10 % in 8 of 30 cells and slower in 3, with one
or two runs per arm (`workorder-ba-parallelism.md`, BA-S4's census row on
BA-S16; BA-S17: *"P11 is not shown material"*). §1.11 of BA inferred, not
measured, that eight loopback sessions on eight cores occupy about 5.25
cores on average. **No placement premise is measured**, which is why
BL-S1 exists (W3's second condition).

### 1.8 What a move leaves behind on its source core

Read at `1aebdfe8` (`v2.7.0-854-g1aebdfe8`) for W4; it is the ground of
BL-R3's strike.
- **Instance-wide, so a move loses none of it** (CLAUDE.md's eviction,
  Waystone and Cabin rows): the buffer pool (AM-S2 step 3), Waystone
  trails and `sys.patterns`, and the Cabin store (AT-S7). A prepared
  statement is text (§1.2).
- `[source-read]` **Per core, but keyed by relation, not by session:** each
  core's catalog serves reads from its own in-memory cache, revalidated
  against the instance's schema word at task boundaries
  (`include/kds/catalog/catalog.hpp:71`, `:206-223`). A destination core
  that already serves the relation holds it warm whichever session moves.
- `[design]` **What is left** is the handoff itself (one reactor turn on
  the destination, §5.1's kick cost) and CPU-cache warmth. Neither differs
  by session, so neither gives a per-session setting a quantity to carry.

---

## 2. Rulings

BL-R1 and BL-R2 restate the operator's decisions (W0, W1, W5, W6); BL-R3
is struck by the operator's (W4) and keeps its number so later citations
stay valid. BL-R4..R9 are CLA's, save what W6 adopted in BL-R7 and BL-R8
(BL-Q4, BL-Q8, BL-Q17, BL-Q18), and BL-Q0 opens them.

### BL-R1 — No session is pinned for its life; the transaction is (W0)

- **A session is placed at its accept and may move at any transaction
  boundary.** No path keeps a session on one core for its whole life.
- `[design]` **W0's "never pinned at any time" is read at the session's
  grain** (BL-Q1). A transaction stays on its core from `Begin` to its
  decide and release, because §1.2's pins are the transaction's. Reading
  W0 at the statement's grain would mean moving a transaction's in-flight
  slot, waits and commit between cores, which is CC3's re-ratification and
  out of BL's scope.
- `[design]` **The movable predicate**, all of it read on the session's
  current core in one poll:
  1. no explicit transaction open (`Session::in_explicit_txn()` false,
     `session.hpp:128`);
  2. no statement in flight (`Connection::in_flight` false), which covers
     a deferred `strict` commit still parked;
  3. no portal open or suspended, and no expired portal awaiting its
     report (`kwp_session.hpp:367`, `:380`);
  4. no frame partially read or reply partially written (the connection's
     inbox holds no whole frame, its outbox is empty);
  5. the connection is not closing, and its TLS/SCRAM handshake has
     finished.
  `[quiet-wrong]` A move that misses (1) or (2) would run a transaction's
  next statement on a core whose in-flight table does not hold it.
  BL-S2's cells pin every clause.

### BL-R2 — Priority: four context levels (W1, W5, W6)

- **Priority governs how much of one core's reactor time a session gets
  when sessions on that core compete, and how soon.**
- **Its values are a closed set of four context levels** (W5; BL-Q16),
  each named for the work a session declares it does and each defined as
  a bundle of mechanisms, not a point on one scale. A number would not
  separate W5's contexts: a single-row update gains nothing from a larger
  share, because it never saturates a core (§1.5, BL-Q4).

  | level | context | mechanism (BL-R7) | bound | who may set it |
  |---|---|---|---|---|
  | `transactional` | single-row updates and pk reads carrying business logic | the latency class: the shortest requested slice r, so a newly queued task's deadline is early and it is picked ahead of the other levels while eligible; share 4 | the lag limit, as every level: a session more than L past its entitlement is not eligible until it falls back inside it | the administrative statement-class role (BL-R8) |
  | `normal` (the default) | a session that declares nothing | share 1; FIFO when every candidate is one level, today's path (BL-R7) | the lag limit, as every level | anyone |
  | `analytic` | query-analysis reads, giving way first | share 1/4, and a walk slice of 16 pages instead of 64, so its walks reach a yield four times as often | the lag limit, as every level | anyone |
  | `background` | batch and ETL work that runs on what is left | the idle class: polled after every other candidate and only when none of them advanced, under a floor of at least one poll per F ms | the floor | anyone |

  Each level also carries a requested time slice r for the pick (BL-R7);
  `transactional`'s is the shortest. The walk slice is in pages and is
  what makes a walk yield; r is in reactor time and is what orders the
  pick. Every number - share 4 and 1/4, slice 16, each r, L and F - is
  `[provisional]` until BL-S1 and BL-S5 size it.
- `[design]` **Two components inside the levels** (BL-Q4 (b), W6): a
  **share** (a weight; under saturation, poll time in proportion) and a
  **latency class** (an earlier pick for a newly ready task), the class
  bounded by the lag limit so it cannot exceed its share over time. **The
  lag limit is the only bound on a mislabelled `transactional` session**:
  a per-statement demotion would act at the same yields and do the same
  thing, so it is not built as a second mechanism. **The share is enforced
  by the polls it denies** (BL-R7 rule 4): with at most one poll per task
  per iteration (`sched.md` §4), a pick that only reorders would leave
  every session one poll an iteration and the share without effect.
- `[design]` **The walk slice is the existing quantity, re-scoped per
  level**: `exec::SlicePolicy::pages_per_slice` (64,
  `include/kds/exec/step_vm.hpp:339`) becomes the `normal` level's value
  and each level carries its own. No second name is added.
- `[design]` **`background` is not the `maintenance` group.** Client batch
  work placed there would compete with the engine's own purge and
  writeback under `sched.md` §4's group shares; the idle class keeps it
  inside `foreground`, below every other client level.
- `[design]` **Scope: reactor poll time only.** Buffer-pool frames, the
  WAL's sync order and the lock table's wait queues do not read priority.
  The order states this so that a raised priority that does not speed a
  session up is explainable (BL-R7).

### BL-R3 — Struck (W4): no session affinity

- **There is no per-session setting for how readily a session moves.**
  W1's affinity half is struck by W4; the move's restraint is the
  engine's, one rule for every session (BL-R4).
- **What it was:** a per-session move cost the balancer charged, in three
  levels `free`, `sticky` (the default) and `strong`, none of them
  "never" (W0), with BL-Q5, BL-Q6 and BL-Q9 deciding its form, its
  coupling to priority and its name.
- `[design]` **Why it went**, as CLA put it to the operator before W4:
  1. the state the cost stood for is mostly not on the source core (§1.8):
     the pool, Waystone and Cabin are the instance's, a prepared statement
     is text, and the per-core catalog cache is keyed by relation, so it
     does not travel with a session. What is left, the handoff and
     CPU-cache warmth, does not differ by session;
  2. with "never" excluded, a level was a multiple of the hysteresis
     margin, which BL-R4 already applies; two names for one quantity is
     what CLAUDE.md's "never add a second name" rule forbids;
  3. no workload asked for it, and §0.2 already called a second user
     setting over-engineering.
- **Restoring it is cheap if a measurement asks:** it would be one more
  input to `decide`'s seam (BL-R4). The trigger is §5.1's move-cost cell
  showing a cost that differs by session, which none of §1.8's state
  predicts.

### BL-R4 — The balancer is a decision on each core, at the boundary

- `[design]` **No balancer thread and no coordinator** (W3; `sched.md` §4:
  *"There is no global coordinator"*). The balancer is a pure function run
  by the session's own core when BL-R1's predicate becomes true:
  `decide(load snapshot, own core, session's dwell) → stay | core k`. It
  reads no session setting (BL-R3, struck).
- `[design]` **The load snapshot** is one word per core, written only by
  that core at its iteration end and read by others with relaxed loads
  (BL-Q3 picks the signal). A single-writer word per core is shared state,
  so `rules.md` §3 and `sched.md` §9 invariant 2 gain one entry naming it,
  as the wake flag's entry is named today.
- `[design]` **The move rule:** a session moves at a boundary only if the
  load it leaves minus the load it joins exceeds a **hysteresis margin**,
  which is the move's cost: the handoff and the warmth it gives up (§1.8).
  The margin is one engine constant, `[provisional]` until §5.1's
  move-cost cell sizes it, and not a setting.
- `[design]` **Anti-oscillation:** a minimum dwell (a session that moved
  does not move again for N of its own transactions or T of the injected
  clock, `[provisional]`) beside the margin, both read by the same
  function. A move rate per core is counted (BL-R8), and BL-S5's bar
  bounds it.
- `[design]` **The decision costs the boundary a few loads and compares.**
  It runs only at boundaries, never per statement inside a transaction.
- `[design]` **A deterministic seam.** `decide` reads its snapshot through
  an interface the two-core rig supplies (`sched.md` §8), so cells fix the
  snapshot rather than race for it.

### BL-R5 — Placement at the accept

- `[design]` BA-R13 as written becomes BL's first decision: above one core
  the handoff arm places each accepted connection on the least-loaded core
  by BL-R4's snapshot (not round-robin), re-scoping
  `force_listener_handoff` rather than adding a setting. `SO_REUSEPORT`
  stays as the other mode (BL-Q2).
- `[design]` At the accept a connection has no source core and no dwell,
  so it lands on the least-loaded core with no margin; a boundary applies
  BL-R4's margin and dwell.

### BL-R6 — A move carries the whole connection

- `[design]` **CN-10 §3.4 option (b), not (c)**: the fd, the channel, the
  auth state, the KWP state and the session move together, and a session
  that does not move pays no hop. The alternative, an I/O core holding
  every fd and a per-transaction dispatch to an execution core (option (c)
  of the pending entry), adds two kicks to every transaction; BL-Q2 keeps
  it on the table with that price named.
- `[design]` **What the move does:**
  1. `clients_` holds `std::unique_ptr<Connection>`, so a `Connection`
     never moves in memory and `KwpSession`'s `Session&` stays valid
     (CN-10 §3.4's first option);
  2. the source unregisters the fd from its reactor, then `Offer`s the
     `Connection` (not an fd) to the destination's inbox and kicks;
  3. the destination `Take`s, registers the fd with its reactor, re-points
     the session's clock to its own, and re-homes the cancel entry's
     target;
  4. bytes that arrived during the move are in the kernel's socket buffer
     and are read by the destination's first readiness.
- `[design]` **`ConnectionHandoff` is extended, not duplicated**: its inbox
  carries a connection record whose fd-only form is the accept's. One
  carrier, one name (`sched.md` §5's consumer list keeps three entries).
- `[design]` `debug_text_port` connections are the same `Connection` type
  and move by the same path (BL-Q7).

### BL-R7 — Priority acts at yield points and statement ends; the remainder is named

- `[design]` **The level pick replaces FIFO inside `foreground`** and
  nowhere else: groups keep `sched.md` §4's share law and both floors.
  Each `foreground` task carries its session's level. A **candidate** is a
  queued task not yet polled this iteration, parked or not (`sched.md`
  §4, §7: a suspended task goes back on its queue). The pick, in order:
  1. **the idle class waits**: a `background` task is polled after every
     other candidate, and only if no poll of another level in this
     iteration advanced (`Task::advanced_in_last_poll`,
     `include/kds/sched/task.hpp:65`); when its floor F is due it is
     polled first;
  2. **eligibility**: a session's virtual time advances by its consumed
     time over its share while it has a task queued; a task entering the
     queue starts at no less than the lowest virtual time queued, so idle
     time banks no credit. Its lag is the queued sessions' share-weighted
     mean virtual time minus its own, times its share; a session whose
     lag is below -L (it has run more than L past its entitlement) is not
     eligible. The counters decay with the group's (`sched.md` §4);
  3. **the earliest virtual deadline among the eligible wins**: virtual
     time plus the level's requested time slice r over its share, so
     `transactional`'s short r puts a newly queued task ahead and
     `analytic`'s small share puts it behind (the shape of Linux's EEVDF,
     from general knowledge not verified in the reply that proposed it);
  4. **an ineligible task waits for a later iteration** once any
     `foreground` task has been polled in this one, so a share is
     enforced by the polls it denies; only when none has been polled is
     the earliest deadline among the ineligible picked (`sched.md` §4's
     second floor). A parked ineligible task therefore sees its wake one
     iteration later, which is the price of the share.
  Per-session consumption is charged from the poll's existing clock
  reads, so the pick adds no clock read.
- `[design]` **When every candidate is one level, rules 2-4 are skipped
  and the queue is popped from the front as today**; consumption is still
  charged, so the order resumes when a second level is queued. A core
  whose sessions all declare nothing - every session `normal` - runs the
  existing path.
- `[design]` **The latency class reorders the queue; it cannot cut a
  running poll or the iteration it arrived in.** A frame readable during
  phase 4 is seen at the next phase 1 (`sched.md` §2), so a newly ready
  `transactional` statement waits out the current iteration's remaining
  polls - up to `max_tasks_per_iteration`, each a walk slice or a
  statement that does not yield - and is then the first eligible pick.
  The levels' slices are the part of that wait BL sets; §1.5's statements
  are the part it does not.
- `[design]` **The remainder** (stated in the close, counted from BL-S1):
  - a statement that does not yield (§1.5) holds its core whatever its
    level, and that includes a `background` session's multi-row writes,
    so v0's `background` level orders reads and statement ends, not a
    write batch's rows (BL-Q13);
  - a session waiting on a lock held by a lower-level session waits at
    the holder's level (inversion; BL-Q12). The combination that reaches
    it is any lower-level writer holding row `X` against a higher-level
    one - `analytic` included, since it is not read-only (BL-Q17);
    `background` against `transactional` is the one BL-Q12's counter
    expects most;
  - resources other than reactor time ignore priority (BL-R2).
- `[design]` **Extending the yield to writes and index walks is not BL's**
  (BL-Q13): a write yielding between rows holds locks, borrows and page
  state across the suspension that `sched.md` §3's suspension safety must
  be argued for, which is an order of its own.

### BL-R8 — The setting, and who may set it

- **Per session, declared by the session after the handshake** (BL-Q8
  (a), W6): one of the four levels of BL-R2, `normal` when it declares
  none. Priority is the only session setting BL has (BL-R3, struck). A
  level derived by the engine from a statement's class (BL-Q8 (c)) is a
  later direction, not v0.
- **`transactional` requires a privilege**: the `admin` statement-class
  role (`docs/spec/protocol.md` §10's authorization paragraph). With
  `auth = off` every session is admin and may declare it, so the lag
  limit is then the only bound. `normal`, `analytic` and `background`
  need no role: each takes no more than the default.
- **`analytic` is a scheduling level, not an access limit** (BL-Q17, W6):
  a write in an `analytic` session runs, at `analytic`'s level.
- `[design]` **The spelling** - the statement or handshake field that
  declares a level - is BL-S4's, under CLAUDE.md's rule that nothing new
  is reserved lightly: the level names hash as identifiers, and an
  unknown name is refused `InvalidArgument` with its byte.
- `[design]` **Observability is part of the mechanism, not a follow-up:**
  per session, its level, current core, moves, transactions since its
  last move and consumed `foreground` time; per core and level: picks,
  picks lost to the lag limit, `background` floor polls; per core: the
  load word, moves in and out, moves refused by dwell. Where these are
  read is BL-Q10.

### BL-R9 — `cores = 1` pays nothing for the balancer

- `[design]` At `cores = 1` no load word is written, no boundary decision
  runs and no `Connection` is offered (`ways-of-working`: zero overhead for
  single-core configurations). The `unique_ptr<Connection>` change is the
  one structural change both configurations share, and BL-S5's A/B
  measures it.
- `[design]` The level pick applies at `cores = 1` (several sessions
  still share one reactor), and BL-R7's one-level-is-FIFO clause is what
  keeps the default path unchanged there.

---

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BL-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The index row</li><li>`raft-marks-2026-10-11.md` §4-§7</li><li>`docs/pending/session-load-balancing.md` deleted, its README row removed</li></ul> | S |
| BL-S1 | **The premise, measured** (W3's second condition) | <ul><li>**Code: instrumentation only, no behaviour change:**<ul><li>per core, the candidate load signals of BL-Q3, sampled each iteration (ready `foreground` length, live sessions, in-flight transactions, `foreground` polled time in a decaying window);</li><li>per session, consumed `foreground` time and polls;</li><li>per core, the reactor-hold distribution (iterations over 10 ms, 100 ms, 1 s) split by the holder's kind: a statement that does not yield, a checkpoint step, the carve, other;</li><li>per transaction boundary, whether BL-R1's predicate held, so the share of boundaries a move could use is known.</li></ul></li><li>**The measurement** (§5.1), into `bench/v3.0.0/results-bl-s1-placement-premise-<describe>.md`.</li><li>**The gate** (BL-Q14).</li><li>**The suite green.**</li></ul> | M |
| BL-S2 | **A movable connection** (BL-R1's predicate, BL-R6), with no policy | <ul><li>**Code:**<ul><li>`clients_` holds `unique_ptr<Connection>`;</li><li>`ConnectionHandoff` carries a connection record; the accept's fd-only form is its special case;</li><li>source unregister, offer, kick; destination take, register, clock re-point, cancel re-home;</li><li>the movable predicate as one function;</li><li>a move is triggered only by a test hook in this stage.</li></ul></li><li>**Cells** (two-core rig):<ul><li>a session moved between two autocommit statements answers the second on the destination, and `SHOW META`'s `core=` says so;</li><li>each clause of the predicate refuses a move when false: an open `BEGIN`, a statement in flight, a parked `strict` commit, an open portal, a half-read frame, a closing connection, an unfinished handshake;</li><li>a prepared statement prepared on the source executes on the destination with the same result and the same `pattern_id`;</li><li>a cancel sent after a move reaches the session on the destination;</li><li>a portal-idle timeout after a move fires on the destination's clock;</li><li>bytes sent during the move are answered, in order;</li><li>a TLS session and a SCRAM-authenticated session move;</li><li>a `debug_text_port` session moves, if BL-Q7 is marked (a).</li></ul></li><li>**The move-cost measurement** (§5.1's last cell), by this stage's test hook, ahead of BL-S3 sizing the margin.</li><li>**Mutations, each repeated:**<ul><li>the in-flight clause removed (killed by the `BEGIN` cell, the `[quiet-wrong]` one);</li><li>the clock not re-pointed (killed by the timeout cell);</li><li>the fd registered before the source unregisters (killed by the in-order cell).</li></ul></li><li>**The suite green.**</li></ul> | M |
| BL-S3 | **The balancer** (BL-R4, BL-R5) | <ul><li>**Code:**<ul><li>the load word per core, written at iteration end, named in `rules.md` §3 and `sched.md` §9;</li><li>`decide` behind its snapshot seam, run at the boundary where BL-R1's predicate first holds;</li><li>the hysteresis margin, sized by §5.1's move-cost cell, and the dwell, `[provisional]`; both engine constants;</li><li>the accept placement on the least-loaded core (BL-R5).</li></ul></li><li>**Cells:**<ul><li>a fixed snapshot whose imbalance exceeds the margin moves the session, and one just inside it stays;</li><li>a session that just moved does not move again inside its dwell;</li><li>two cores alternating as least loaded produce no ping-pong past the hysteresis;</li><li>`cores = 1` writes no load word and calls no `decide` (a counter stays 0);</li><li>eight connections at `cores = 4` land two per core.</li></ul></li><li>**Mutation:** the dwell removed (killed by the ping-pong cell).</li><li>**The suite green.**</li></ul> | M |
| BL-S4 | **The level pick** (BL-R2, BL-R7, BL-R8) | <ul><li>**Code:**<ul><li>a level per session carried by its `foreground` tasks, `normal` by default;</li><li>per-session consumed time and lag from the poll's existing clock reads;</li><li>the pick of BL-R7 inside `foreground`: the idle class and its floor, eligibility by the lag limit, the earliest virtual deadline, and an ineligible task denied its poll; FIFO when every candidate is one level;</li><li>`exec::SlicePolicy::pages_per_slice` taken from the walking session's level;</li><li>the level's declaration, its spelling and the `transactional` privilege check (BL-R8);</li><li>the observability of BL-R8 where BL-Q10 puts it.</li></ul></li><li>**Cells:**<ul><li>with N `analytic` tasks ready, a `transactional` task made ready is the next pick;</li><li>a `transactional` session saturating its core loses the early pick once it is more than L past its entitlement, and regains it inside L;</li><li>two saturating sessions, `normal` and `analytic`, at one walk slice (the `analytic` slice pinned to 64 through the test seam), receive polled time within ±10 % of 4:1 over a fixed number of iterations;</li><li>a `background` task is not polled in an iteration where a poll of another level advanced, and is polled at least once per F under saturation;</li><li>an `analytic` walk yields every 16 pages and a `normal` walk every 64;</li><li>with every candidate one level and sessions of unequal consumed time, the pick order equals FIFO, task for task;</li><li>declaring `transactional` without the privilege is refused with its byte, and an unknown level `InvalidArgument` with its byte;</li><li>`sched.md` §4's two floors still hold with the pick in place.</li></ul></li><li>**Mutations, each repeated:**<ul><li>the one-level-is-FIFO clause removed (killed by the order cell);</li><li>the lag limit removed (killed by the saturating `transactional` cell);</li><li>the `background` floor removed (killed by the floor cell);</li><li>the shares equalised (killed by the share cell).</li></ul></li><li>**The suite green.**</li></ul> | M |
| BL-S5 | **The close** | <ul><li>**The measurement** (§5.2).</li><li>**Text:**<ul><li>`sched.md` §3 (no session pinned; the transaction is), §4 (the pick inside `foreground`), §5 (the handoff carries a connection at the accept and at a boundary), §9 (the load word);</li><li>`crosscore.md` and `protocol.md` (a session's core may change between transactions; `SHOW META`'s `core=` is the current one);</li><li>`client-manual.md` and `manual/` for the priority setting;</li><li>`rules.md` §3's table;</li><li>CLAUDE.md's Cross-core execution and KWP rows;</li><li>CN-10 per BL-Q11.</li></ul></li><li>**Docs:** BL-R7's remainder in `known-gaps.md`.</li></ul> | S |

## 4. Items for the operator

BL-Q5, BL-Q6 and BL-Q9 are struck by W4: their rows stay so the
numbering does not move. BL-Q4, BL-Q8 and BL-Q16..Q18 are marked by W6,
which adopted CLA's proposal whole, not item by item. Every other item is unmarked. Every
proposal is `[design]` unless a cell says otherwise.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BL-Q0 | **Open BL**, with BL-S0..S5 and BL-R4..R9 as written, **after BI-S5 closes** (W2's premise). BL-R1 and BL-R2 are the operator's (W0, W1, W5, W6); BL-R3 is struck (W4) | process | Yes | |
| BL-Q1 | **W0's grain.**<br>(a) The session is never pinned; a transaction stays on its core (BL-R1).<br>(b) The statement: a transaction's statements may run on different cores | scope, **[quiet-wrong] under (b)** | (a). (b) moves the in-flight slot, the waits and the commit across cores, which CC3 forbids today and which needs its own ratification; it buys nothing for short OLTP transactions | |
| BL-Q2 | **The carrier.**<br>(a) The whole connection moves at a boundary (BL-R6); a session that stays pays no hop.<br>(b) An I/O core keeps every fd, and each transaction is dispatched to an execution core: two kicks per transaction.<br>(c) A central balancer thread decides every dispatch | design | (a). (b) is BL-S1-measurable as two kick round trips per transaction and is reconsidered only if (a)'s move rate is so high that every transaction moves anyway. (c) is W3's excluded shape | |
| BL-Q3 | **The load signal** in the per-core word: (a) ready `foreground` length; (b) live sessions; (c) in-flight transactions; (d) `foreground` polled time in a decaying window; (e) a combination BL-S1 picks | measurement | (e), chosen by BL-S1 as the signal whose per-core spread best predicts the per-core p99 spread in §5.1's skew cells; (b) is the fallback when none separates | |
| BL-Q4 | **Priority's components.**<br>(a) A share only.<br>(b) A share and a latency class, the class bounded by a lag limit so it cannot exceed its share over time | design | (b), revised from (a) on W5; BL-R2 says why. §5.2's mixed cell is now (b)'s bar, not its trigger | **(b)**, adopted whole on W6 (2026-10-12), not marked per item |
| BL-Q5 | ~~Affinity's form~~ | - | - | **Struck by W4** (2026-10-12): no session affinity exists (BL-R3) |
| BL-Q6 | ~~Do the axes interact in v0?~~ | - | - | **Struck by W4** (2026-10-12): one session axis remains. Its surviving half stands as BL-R4: the balancer never reads priority |
| BL-Q7 | **`debug_text_port` sessions**: (a) movable like any other; (b) stay on core 0 | scope | (a). W0 says no session is pinned; the connection type is the same | |
| BL-Q8 | **Is the priority setting user-visible in v0?**<br>(a) Yes: a per-session setting after the handshake, its `transactional` level gated by the administrative role.<br>(b) No: the mechanism ships with its default only, observable but not settable, and the surface waits for a workload that asks.<br>(c) Engine-derived: priority from the statement's class (point read vs walk vs load) | user-visible | (a): the session declares one of BL-Q16's four levels; `transactional` alone is gated by the administrative role. (c) is recorded as a later direction, not v0 | **(a)**, adopted whole on W6 (2026-10-12), not marked per item |
| BL-Q9 | ~~The name~~ | - | - | **Struck by W4** (2026-10-12): with no session affinity there is no collision with the namespace's (the header) |
| BL-Q10 | **Where the per-session counters are read.**<br>(a) `SHOW META` for the asking session and per-core totals only.<br>(b) A `sys` view of every session, which the operator excluded from BO (`raft-marks-2026-10-11.md` §2) | scope | (a). (b) reverses an operator mark, which only the operator can do | |
| BL-Q11 | **CN-10.** BL builds §3.4's carrier with a load trigger, not §3.5's admission. (a) BL's close marks U6 as built by BL, with the trigger noted as BL-R4's; (b) U6 stays open until §3.5 exists | process | (a) | |
| BL-Q12 | **Priority inversion.**<br>(a) None in v0; a counter of waits whose holder's level is below the waiter's.<br>(b) Inheritance: a holder's session is polled at its highest waiter's level until the release | design | (a), with (b) taken up only if §5.2 counts inversions on more than 1 % of lock waits in its mixed cell. The counter splits by the pair of levels: a `background` writer holding row `X` against a `transactional` writer is the combination BL-R7 expects | |
| BL-Q13 | **Yielding writes and index walks** (BL-R7's remainder). (a) Out of BL, named in the close; (b) in BL | scope | (a). It is a suspension-safety argument over locks and borrows, an order of its own | |
| BL-Q14 | **BL-S1's premise gate.** BL stops for a ruling when either: (i) under §5.1's skew cells, the balanced arm improves neither aggregate throughput nor per-core p99 spread by more than the run-to-run spread; or (ii) under the mixed cell, the same-core point read's p99 is already within 2× its isolated p99, so priority has nothing to reorder | process | As written. (i) gates BL-S2/S3; (ii) gates BL-S4. Each gate is independent | |
| BL-Q15 | **The measurement** (§5.2), at BL's close, per CLAUDE.md's Session Workflow step 3 | process | As written | |
| BL-Q16 | **The level set** (W5).<br>(a) Four context levels, `transactional`, `normal` (the default), `analytic`, `background`, each a bundle of mechanisms (BL-R2).<br>(b) Points on one scale (`high`, `normal`, `low`) | user-visible | (a). A scale cannot separate W5's contexts: a share does nothing for a short update, and "gives way first" and "runs on what is left" are different rules. The names are a closed set and hash as identifiers | **(a)**, adopted whole on W6 (2026-10-12), not marked per item |
| BL-Q17 | **Is `analytic` read-only?**<br>(a) No: a scheduling level only; a write runs at `analytic`'s level.<br>(b) Yes: a write in an `analytic` session is refused | scope | (a). (b) turns a scheduling hint into an access limit, which belongs to the statement-class roles, not to a priority | **(a)**, adopted whole on W6 (2026-10-12), not marked per item |
| BL-Q18 | **`background`'s form.**<br>(a) The idle class inside `foreground`: polled only when no other level's poll advanced, under a floor of one poll per F.<br>(b) The lowest share inside `foreground`.<br>(c) The `maintenance` group | design | (a). (b) still takes a fixed fraction from a busy core, which is not "runs on what is left"; (c): BL-R2 | **(a)**, adopted whole on W6 (2026-10-12), not marked per item |

## 5. Measurement

### 5.1 BL-S1

`[design]` In `build-release` at BL-S1's instrumented commit, named by
`git describe --tags`, after BI-S5. `bench/README.md`'s five rules apply.

- **Skew cells (placement premise).** `cores` {2, 4, 8}, `durability`
  {`group`, `relaxed`}, a pool of long-lived sessions (16 and 64) running
  a pk point `SELECT` / pk `UPDATE` mix:
  - **arm K**: the kernel's `SO_REUSEPORT` placement, as shipped;
  - **arm B**: a balanced placement forced by the driver, as
    `tools/multicore_benchmark.py` does by reading `core=`;
  - **arm D**: arm B, then a third of the sessions closed unevenly and
    reopened, so a skew develops after the accept - the case an
    accept-only balancer cannot fix.
  Ten runs per arm, interleaved. Report throughput, per-core session
  count, per-core p99, and BL-Q3's candidate signals per core.
- **Mixed cell (priority premise).** One core (`cores = 1`, and one core
  of `cores = 4` with sessions forced onto it): one session running
  10,000-row walks, one running 1,024-row multi-row `INSERT`s, and four
  running pk point reads at a fixed open-loop rate. Report the point
  read's p50/p99/max against an isolated run of the same reads, and the
  reactor-hold distribution by holder kind (§1.6).
- **Boundary cell.** For each of the above, the share of transaction
  boundaries at which BL-R1's predicate held, which bounds how often a
  move is possible.
- **Kick cost.** The existing handoff path at the accept
  (`force_listener_handoff`), its offer-to-adopt latency, as the lower
  bound on a move's cost.
- **Move cost** (sizes BL-R4's margin; BL-R3's restore trigger), **run at
  BL-S2**, since it needs BL-S2's carrier. Sessions moved by BL-S2's test
  hook between autocommit transactions, under the skew cells' mix: the
  first transaction's latency on the destination against the same
  session's next ones, split by whether the destination already served
  the session's relations. A cost that differs by session beyond
  run-to-run spread is reported as such (BL-R3).
- **Host.** `/proc/loadavg`, `/proc/pressure/io` and `/proc/pressure/cpu`,
  per cell.

### 5.2 BL-S5

`[design]` A = BL-S1's commit, B = the close, plus §5.1's cells:
- **The overhead A/B**, interleaved, at `cores = 1` and `cores = 4`, with
  every session at default settings: pk point statements and a whole-walk
  cell. The expectation is flat; the `unique_ptr<Connection>` indirection
  and the boundary decision are the only per-transaction additions above
  one core.
- **Convergence.** Arm D's skew under the balancer: the per-core load
  word's max/min ratio over time, moves per thousand transactions, and
  the ping-pong count.
- **Share fidelity.** Two saturating sessions, `normal` and `analytic`,
  on one core: consumed-time ratio against the configured 4:1.
- **Mixed cell** under BL, §5.1's mixed load with its sessions given
  levels: the point readers `transactional`, the 10,000-row walker
  `analytic`, the multi-row inserter `background`, run with and without
  the inserter; also with the point readers at `normal`. Report the point read's p50/p99/max against its
  isolated run, the non-yielding remainder's share beside it, and the
  `background` session's throughput and floor polls.
- **Inversions** (BL-Q12's counter, by pair of levels) in the mixed cell
  with lock contention.
- **Bars:** (i) the overhead A/B within run-to-run noise; (ii) arm D's
  per-core p99 spread under B within that of arm B (balanced at accept);
  (iii) share fidelity within ±10 %; (iv) moves bounded by the dwell, with
  no session moving more often than once per dwell window; (v) in the
  mixed cell run without the inserter, the `transactional` point read's
  p99 at most 2 × its isolated p99 + the `analytic` walker's p99 poll
  duration, both measured in the same run; the cell with the inserter
  `background` is reported beside it with no bar (BL-R7's remainder); (vi) `background`
  polled at least once per F under saturation. Run counts and any bar not
  met are stated.

## 6. Row status

### BL-S0 — written 2026-10-11

Written in the main checkout on `main` at `1d389650`
(`v2.7.0-775-g1d389650`), with no worktree, as a document-only change. No
file under `src/`, `include/` or `tests/` moved, and no suite ran. Every
`[source-read]` citation was read at `1d389650`, not run.

With it:
- `instructions/v3.0.0/index.md` gains BL's row;
- `raft-marks-2026-10-11.md` gains §4-§7, the operator's words W0-W3;
- `docs/pending/session-load-balancing.md` is deleted, as
  `docs/pending/README.md` requires once the operator rules, and its row
  leaves the README's table. Its content is carried by §1.1, §1.7 and
  BL-Q2; git keeps the entry.

**Owed:** the `critics-developer` review of this order, before BL-Q0 is
put to the operator.

### BL-S0 amended 2026-10-12 — session affinity struck (W4)

Amended on `worktree-bl-drop-session-affinity` from `1aebdfe8`
(`v2.7.0-854-g1aebdfe8`), as a document-only change. No file under
`src/`, `include/` or `tests/` moved, and no suite ran. §1.8's
`[source-read]` citations were read at `1aebdfe8`, not run.

- BL-R3 is struck and keeps its number; BL-R4 carries the move rule as
  one engine-wide hysteresis margin; BL-R5 and BL-R8 lose their affinity
  clauses; §0.1 has one axis, and a per-session move setting is a
  non-goal.
- BL-Q5, BL-Q6 and BL-Q9 are struck; BL-Q8 asks about priority alone.
- §1.8 is rewritten as what a move leaves behind; §5.1 gains the
  move-cost cell, run at BL-S2 by that stage's test hook.
- `raft-marks-2026-10-12.md` gains §4, the operator's word W4;
  `raft-marks-2026-10-11.md` §5 points at it; `index.md`'s BL row is
  restated.
- §0.1's table cited the pick as BL-R6; it is BL-R7, corrected in passing.
- The namespace's affinity (NS10, D18) is untouched.

### BL-S0 amended 2026-10-12 — the four context levels (W5, W6)

Amended on `worktree-bl-drop-session-affinity` from `1aebdfe8`, in the
same uncommitted change as W4's, as a document-only change. No file under
`src/`, `include/` or `tests/` moved, and no suite ran.
`exec::SlicePolicy::pages_per_slice` (`include/kds/exec/step_vm.hpp:339`)
was read at `1aebdfe8`, not run.

- BL-R2 defines priority as four context levels, `transactional`,
  `normal`, `analytic` and `background`, each a bundle of a share, a
  latency class, a walk slice and an eligibility rule; BL-R7 states the
  pick (idle class, lag-limit eligibility, earliest virtual deadline,
  FIFO at one level) and that the latency class cannot cut a running
  slice; BL-R8 states who may declare each level and that `analytic` is
  not an access limit.
- BL-Q4 is marked (b), revised from CLA's earlier (a); BL-Q8 is marked
  (a) with four levels; BL-Q16 (the set), BL-Q17 (`analytic` not
  read-only) and BL-Q18 (`background` as the idle class) are added and
  marked; BL-Q12's counter splits by pair of levels. The per-statement
  demotion CLA first proposed for `transactional` is folded into the lag
  limit and not built.
- BL-S4's code, cells and mutations and §5.2's mixed cell and bars (v)
  and (vi) follow the levels; G3 and §0.1 restate the axis.
- `raft-marks-2026-10-12.md` gains §5, the operator's words W5 and W6.

**The amendment's review** (`critics-developer`, 2026-10-12, over the
uncommitted W4-W6 diff on `worktree-bl-drop-session-affinity` from
`1aebdfe8`) found BL-R7's first pick unsound against `sched.md` §4: with
at most one poll per task an iteration the share never denied a poll, so
the 4:1 cell measured the walk slice; "ready" included parked tasks, which
shut `background` out; the lag's sign and reference were undefined; and
bar (v) added pages to a latency. BL-R7's rules, BL-R2's table and r, the
BL-S4 cells and bar (v) were rewritten to its findings; the move-cost cell
moved to BL-S2, where the carrier exists (its option (a) over (b), which
would have timed a new session rather than a move); BL-R5's accept clause,
BL-R8's `auth = off` case and the inversion pairs were corrected, and
seven cuts taken. No finding was rejected.
