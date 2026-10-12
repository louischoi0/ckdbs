# Work order BL — no session is pinned: a balancer between connection and core, and one session axis, priority

**Amended 2026-10-12**: session affinity struck (W4); priority's values
set as four context levels (W5, W6), served in v0 by placement and a
per-level walk slice, with a weighted pick deferred to the last rung
(W7, W8). The file keeps its name,
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
- **W7** (the same day, `raft-marks-2026-10-12.md` §6): *"Is scheduling
  on our own, without leaning on the OS, useful enough to be worth
  developing? Review it from the over-engineering side."* CLA answered
  that the reactor's scheduler is not optional - the OS cannot see a
  session that is not a thread - but that the weighted pick outweighed
  its evidence, and proposed serving the levels by placement first.
- **W8** (the same day, §6): *"Amend the BL document as CLA proposed, and
  push it."* It adopts W7's answer whole: BL-R2's partition, BL-R7's four
  rungs with v0 at rung 2, BL-Q4 (b) deferred to rung 4.
- **W9** (the same day, §7): *"Rule BL-Q19 and BL-Q20 as CLA proposed
  too."* BL-Q19 (a) and BL-Q20 (a).

**Written, not opened.** BL-R1, BL-R2 and BL-R7's rungs stand on the
operator's words; BL-R3 is struck by W4; the marks are in §4, and BL-Q0
is unmarked. Everything else is CLA's proposal, in §2 and §4. **Reviewed
in part**: the 2026-10-12 amendment had its `critics-developer` review
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
re-takes, by load and by the context level the session declares, at
every point where moving it is free of correctness risk - and a level is
served first by where its sessions run, and only when that is measured
short by how a core picks among them.

**Three goals:**
- **G1 — no session is pinned for its life** (W0). A session is placed at
  its accept and may move at any transaction boundary thereafter.
- **G2 — placement sees load** (W0, W3). The kernel's `SO_REUSEPORT` hash
  stops being the only placement, and a skew that develops after the
  accept can be undone.
- **G3 — sessions are served by their declared context** (W0, W1, W5,
  W8). From its next boundary, a business-logic update does not share
  its core with an analytic walk or a background batch, and an analytic walk gives its core back
  more often (BL-R2, BL-R7).

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
- **Reactor threads at different OS priorities** (`nice`,
  `SCHED_IDLE`). OS preemption would cut what BL cannot, but a page latch
  spins 64 turns and then yields with no queue
  (`include/kds/storage/page_latch.hpp:56-61`), and the stream latch
  spans a `pwrite` or an `fsync`: a low-priority thread preempted inside
  either leaves a high-priority one spinning on a holder the OS will not
  run. W7's review rejected it on that inversion.

### 0.1 The session axis (W1, W4, W5)

`[design]` W1 named two settings per session; W4 struck affinity, so one
remains. It is defined by the mechanism that reads it, so it is not a
number without a consumer. W5 made its values context levels rather than
points on one scale, and W8 made v0 serve them by placement and slice
rather than by a weighted pick (BL-R2, BL-R7):

| axis | what it governs | read by | unit of effect |
|---|---|---|---|
| **priority** | one of four context levels, each mapped to a core partition and a walk slice | the balancer's choice of partition (BL-R4) and a walk's slice length | where a session's next transaction runs; how often its walk yields |

- **The balancer reads one thing of a session: its level's partition**
  (BL-R4). Within the partition it reads load, and the move cost and dwell
  it applies are the engine's constants. A core's pick stays FIFO in v0.
- **At `cores = 1` the balancer does not exist and there is no partition**
  (BL-R9); a level then acts by its walk slice alone.

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
  that question to the operator. (W7's review later found the weighted
  pick outweighed its evidence; BL-R7.)

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

BL-R1 and BL-R2 restate the operator's decisions (W0, W1, W5, W6, W8);
BL-R3 is struck by the operator's (W4) and keeps its number so later
citations stay valid. BL-R4..R9 are CLA's, save what W6, W8 and W9
adopted in BL-R4's partition read, BL-R7's rungs and BL-R8 (BL-Q4,
BL-Q8, BL-Q17..Q20), and BL-Q0 opens them.

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

### BL-R2 — Priority: four context levels, served by placement in v0 (W1, W5, W6, W8)

- **Priority says what work a session declares it does, and v0 serves it
  by where that work runs** (W8). The four levels are W6's and are the
  user-visible contract (BL-Q16); W8 changes how v0 serves them, not what
  they are named or who may declare them.
- **The partition.** Above one core, the cores split into **interactive**
  cores and **batch** cores. The batch cores are the highest-numbered
  `max(1, cores / 4)`, `[provisional]`, and core 0 is never one: it owns
  the log and `debug_text_port`. The split is an engine constant in v0,
  not a setting (BL-Q19).
- `[design]` **The partition is active only while a batch-level session
  lives on the instance**: with none, every core is interactive and
  placement is today's (BL-R5 over all cores), so an instance whose
  sessions declare nothing pays nothing for it. When a session declares
  `analytic` or `background`, interactive-level sessions on a batch core
  move off at their next boundary (BL-R4); when the last one ends or
  declares an interactive level, the partition lapses and the balancer
  spreads sessions again under its margin and dwell. The count of live
  batch-level sessions is one instance-wide atomic, named in `rules.md`
  §3 beside the load words.

  | level | context | v0 mechanism | rung 3, if built (BL-R7) | who may set it |
  |---|---|---|---|---|
  | `transactional` | single-row updates and pk reads carrying business logic | interactive cores only, so from its next boundary it never shares a core with a batch-level session; walk slice 64 | picked ahead of `normal` on its core | the `admin` role (BL-R8) |
  | `normal` (the default) | a session that declares nothing | interactive cores, every core while the partition is inactive; walk slice 64; today's path while no session declares a level | behind `transactional` | anyone |
  | `analytic` | query-analysis reads, giving way first | batch cores; walk slice 16, so its walks reach a yield four times as often | picked ahead of `background` on its core | anyone |
  | `background` | batch and ETL work that runs on what is left | batch cores; walk slice 64 | behind `analytic`, under a floor | anyone |

  Both numbers are `[provisional]`: the slice until BL-S1's sweep sizes
  it, the partition until BL-S5 does.
- `[design]` **Why placement first** (W7's review): without preemption a
  pick can only reorder at yields, so a `transactional` statement on a
  core with a walker still waits out the walker's slice and anything that
  does not yield; on a core with no batch session it waits for neither.
  Isolation is the stronger mechanism and the cheaper one: it is BL-R4's
  balancer with a filter, where a weighted pick was a new scheduler law
  whose first draft its review found unsound in four places (§6).
- `[design]` **What v0 does not separate**, stated so it is not read as
  built: `transactional` and `normal` share the interactive cores and are
  FIFO among each other, as `analytic` and `background` are on the batch
  cores. `transactional` still means what it says - never on a core with a
  batch-level session after its next boundary - and rung 3 is what would
  add its precedence over `normal`.
- `[design]` **The walk slice is the existing quantity, re-scoped per
  level**: `exec::SlicePolicy::pages_per_slice` (64,
  `include/kds/exec/step_vm.hpp:339`) becomes the `normal` level's value
  and each level carries its own. No second name is added. Above one
  core, `analytic`'s shorter slice yields only to batch-level sessions,
  so it serves W5's "gives way first" and protects no interactive level;
  at `cores = 1` it is the only mechanism a level has.
- `[design]` **The cost of the partition is idle batch capacity while it
  is active**: a batch core with no batch-level session on it serves
  nothing then. Whether an interactive session may borrow it is BL-Q20;
  v0 says no, and §5.2 reports the capacity left idle.
- `[design]` **Scope: reactor time only.** Buffer-pool frames, the WAL's
  sync order and the lock table's wait queues do not read the level. The
  order states this so that a declared level that does not speed a
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
  `decide(load snapshot, own core, session's level, session's dwell) →
  stay | core k`. Of the session it reads the level's partition (BL-R2)
  and nothing else: no weight, no move setting (BL-R3, struck).
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
- `[design]` **The partition comes before load.** A session on a core
  outside its level's partition moves at its next boundary to the
  least-loaded core inside it, with no margin and no dwell - the first
  boundary after it declares a level, or after the kernel's hash accepted
  it onto a batch core. Inside its partition the move rule above applies,
  and a load comparison never takes a session out of its partition.
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
- `[design]` At the accept a connection has no source core, no dwell and
  no declared level (it is declared after the handshake, BL-R8), so it is
  `normal` and lands on the least-loaded interactive core with no margin;
  a boundary applies BL-R4's partition, margin and dwell.

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

### BL-R7 — Priority is served by the cheapest rung that meets the bar; the remainder is named (W8)

- `[design]` **Four rungs.** Rung 2 is v0 unless BL-S1's slice sweep
  rules rung 1 enough (BL-Q14 (iii)); rungs 3 and 4 are each built only
  if the rung below misses §5.2's bar (v), and only on the operator's
  ruling:
  1. **One slice for every walk.** BL-S1's mixed cell sweeps
     `pages_per_slice` (64, 32, 16). If one value alone brings the point
     read inside BL-Q14's bar, BL stops for a ruling (BL-Q14 (iii)), with
     CLA's proposal that `pages_per_slice` is re-scoped to that value and
     BL-S4 is not built.
  2. **Placement and the per-level slice - v0** (BL-R2, BL-R4). The pick
     inside `foreground` stays FIFO, unchanged.
  3. **A two-tier pick with a floor**: on each core, the higher level of
     the pair sharing it (`transactional` over `normal`, `analytic` over
     `background`) is polled first, and the lower is polled at least once
     per F. Two queues and a floor; no virtual time, no lag.
  4. **The weighted pick**: a share, a latency class bounded by a lag
     limit, an idle class, the earliest virtual deadline, an ineligible
     task denied its poll. Its rules as reviewed on 2026-10-12 are BL-R7
     at `f9f0c6fa`; git keeps them, and BL-Q4 (b) is deferred to here.
- `[design]` **A pick reorders a queue; it cannot cut a running poll or the
  iteration a task arrived in.** A frame readable during phase 4 is seen
  at the next phase 1 (`sched.md` §2), so a newly ready statement waits out
  the current iteration's remaining polls - up to
  `max_tasks_per_iteration`, each a walk slice or a statement that does not
  yield. That is why rung 2 comes before rungs 3 and 4: on an interactive
  core no batch-level walk is among those polls.
- `[design]` **The remainder** (stated in the close, counted from BL-S1):
  - on an interactive core, a `normal` session's statement that does not
    yield (§1.5) holds the core from a `transactional` one, and on a batch
    core a `background` write batch holds it from an `analytic` read; rung
    3 reorders only what reaches a yield or a statement end (BL-Q13);
  - at `cores = 1` there is no partition, so a level acts by its slice
    alone (BL-R9);
  - a session waiting on a lock held by a session of another level waits
    for the holder (inversion; BL-Q12). Placement does not separate lock
    holders: a `background` writer on a batch core holding row `X` delays
    a `transactional` writer on an interactive one. `analytic` is not
    read-only (BL-Q17), so it can hold one too;
  - resources other than reactor time ignore the level (BL-R2).
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
  `auth = off` every session is admin and may declare it. In v0 it is
  placed where `normal` is; the role matters from rung 3. `normal`, `analytic` and `background` need no role: each
  takes no more than the default.
- **`analytic` is a scheduling level, not an access limit** (BL-Q17, W6):
  a write in an `analytic` session runs, at `analytic`'s level.
- `[design]` **The spelling** - the statement or handshake field that
  declares a level - is BL-S4's, under CLAUDE.md's rule that nothing new
  is reserved lightly: the level names hash as identifiers, and an
  unknown name is refused `InvalidArgument` with its byte.
- `[design]` **Observability is part of the mechanism, not a follow-up:**
  per session, its level, current core, moves, transactions since its
  last move and consumed `foreground` time; per core and level: sessions
  placed and polled time; per core: its partition, the load word, moves
  in and out (partition moves apart), moves refused by dwell. Where these
  are read is BL-Q10.

### BL-R9 — `cores = 1` pays nothing for the balancer

- `[design]` At `cores = 1` no load word is written, no boundary decision
  runs and no `Connection` is offered (`ways-of-working`: zero overhead for
  single-core configurations). The `unique_ptr<Connection>` change is the
  one structural change both configurations share, and BL-S5's A/B
  measures it.
- `[design]` At `cores = 1` there is no partition and the pick is FIFO,
  so a level acts by its walk slice alone; a core whose sessions all
  declare nothing runs today's path.

---

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BL-S0 | **The order** | <ul><li>This file</li><li>Its review (§6)</li><li>The index row</li><li>`raft-marks-2026-10-11.md` §4-§7</li><li>`docs/pending/session-load-balancing.md` deleted, its README row removed</li></ul> | S |
| BL-S1 | **The premise, measured** (W3's second condition) | <ul><li>**Code: instrumentation only, no behaviour change:**<ul><li>per core, the candidate load signals of BL-Q3, sampled each iteration (ready `foreground` length, live sessions, in-flight transactions, `foreground` polled time in a decaying window);</li><li>per session, consumed `foreground` time and polls;</li><li>per core, the reactor-hold distribution (iterations over 10 ms, 100 ms, 1 s) split by the holder's kind: a statement that does not yield, a checkpoint step, the carve, other;</li><li>per transaction boundary, whether BL-R1's predicate held, so the share of boundaries a move could use is known.</li></ul></li><li>**The measurement** (§5.1), into `bench/v3.0.0/results-bl-s1-placement-premise-<describe>.md`.</li><li>**The gate** (BL-Q14).</li><li>**The suite green.**</li></ul> | M |
| BL-S2 | **A movable connection** (BL-R1's predicate, BL-R6), with no policy | <ul><li>**Code:**<ul><li>`clients_` holds `unique_ptr<Connection>`;</li><li>`ConnectionHandoff` carries a connection record; the accept's fd-only form is its special case;</li><li>source unregister, offer, kick; destination take, register, clock re-point, cancel re-home;</li><li>the movable predicate as one function;</li><li>a move is triggered only by a test hook in this stage.</li></ul></li><li>**Cells** (two-core rig):<ul><li>a session moved between two autocommit statements answers the second on the destination, and `SHOW META`'s `core=` says so;</li><li>each clause of the predicate refuses a move when false: an open `BEGIN`, a statement in flight, a parked `strict` commit, an open portal, a half-read frame, a closing connection, an unfinished handshake;</li><li>a prepared statement prepared on the source executes on the destination with the same result and the same `pattern_id`;</li><li>a cancel sent after a move reaches the session on the destination;</li><li>a portal-idle timeout after a move fires on the destination's clock;</li><li>bytes sent during the move are answered, in order;</li><li>a TLS session and a SCRAM-authenticated session move;</li><li>a `debug_text_port` session moves, if BL-Q7 is marked (a).</li></ul></li><li>**The move-cost measurement** (§5.1's last cell), by this stage's test hook, ahead of BL-S3 sizing the margin.</li><li>**Mutations, each repeated:**<ul><li>the in-flight clause removed (killed by the `BEGIN` cell, the `[quiet-wrong]` one);</li><li>the clock not re-pointed (killed by the timeout cell);</li><li>the fd registered before the source unregisters (killed by the in-order cell).</li></ul></li><li>**The suite green.**</li></ul> | M |
| BL-S3 | **The balancer** (BL-R4, BL-R5) | <ul><li>**Code:**<ul><li>the load word per core, written at iteration end, named in `rules.md` §3 and `sched.md` §9;</li><li>`decide` behind its snapshot seam, run at the boundary where BL-R1's predicate first holds;</li><li>the hysteresis margin, sized by §5.1's move-cost cell, and the dwell, `[provisional]`; both engine constants;</li><li>the accept placement on the least-loaded core (BL-R5).</li></ul></li><li>**Cells:**<ul><li>a fixed snapshot whose imbalance exceeds the margin moves the session, and one just inside it stays;</li><li>a session that just moved does not move again inside its dwell;</li><li>two cores alternating as least loaded produce no ping-pong past the hysteresis;</li><li>`cores = 1` writes no load word and calls no `decide` (a counter stays 0);</li><li>eight connections at `cores = 4` land two per core.</li></ul></li><li>**Mutation:** the dwell removed (killed by the ping-pong cell).</li><li>**The suite green.**</li></ul> | M |
| BL-S4 | **The levels, served by placement** (BL-R2, BL-R4, BL-R7 rung 2, BL-R8); not built if BL-Q14 (iii) rules rung 1 enough | <ul><li>**Code:**<ul><li>a level per session, `normal` by default, declared after the handshake, its spelling and the `transactional` privilege check;</li><li>the partition: the batch core set of BL-R2, an engine constant, active while the instance's count of live batch-level sessions is above zero;</li><li>`decide` reads the level's partition: a session outside it moves at its next boundary to the least-loaded core inside, with no margin and no dwell, and a load move never leaves it;</li><li>`exec::SlicePolicy::pages_per_slice` taken from the walking session's level;</li><li>the pick inside `foreground` untouched;</li><li>the observability of BL-R8 where BL-Q10 puts it.</li></ul></li><li>**Cells** (two-core and four-core rigs):<ul><li>a session that declares `analytic` runs its next transaction on a batch core, and one that declares `normal` again returns to an interactive core;</li><li>while the partition is active, a `transactional` or `normal` session is never moved onto a batch core by `decide` or by the accept's placement, whatever the load snapshot says, and an `analytic` or `background` one never onto an interactive core;</li><li>with no batch-level session live, eight `normal` connections at `cores = 4` land two per core, and the first `analytic` declaration moves the `normal` sessions off the batch core at their next boundaries;</li><li>when the last batch-level session ends, the partition lapses and the balancer may place a `normal` session on a former batch core;</li><li>a session the kernel's hash accepted onto a batch core moves to an interactive core at its first boundary;</li><li>inside a partition, a load move obeys BL-R4's margin and dwell;</li><li>core 0 is never a batch core;</li><li>an `analytic` walk yields every 16 pages and a `normal` walk every 64;</li><li>at `cores = 1` no partition exists and a level changes only the slice;</li><li>declaring `transactional` without the privilege is refused with its byte, and an unknown level `InvalidArgument` with its byte.</li></ul></li><li>**Mutations, each repeated:**<ul><li>the partition filter removed (killed by the never-moved cell);</li><li>the partition held active with no batch-level session (killed by the two-per-core cell);</li><li>the per-level slice removed (killed by the yield cell).</li></ul></li><li>**The suite green.**</li></ul> | M |
| BL-S5 | **The close** | <ul><li>**The measurement** (§5.2).</li><li>**Text:**<ul><li>`sched.md` §3 (no session pinned; the transaction is), §3 (the per-level walk slice), §4 (the interactive/batch partition; the pick inside `foreground` unchanged), §5 (the handoff carries a connection at the accept and at a boundary), §9 (the load word);</li><li>`crosscore.md` and `protocol.md` (a session's core may change between transactions; `SHOW META`'s `core=` is the current one);</li><li>`client-manual.md` and `manual/` for the priority setting;</li><li>`rules.md` §3's table;</li><li>CLAUDE.md's Cross-core execution and KWP rows;</li><li>CN-10 per BL-Q11.</li></ul></li><li>**Docs:** BL-R7's remainder in `known-gaps.md`.</li></ul> | S |

## 4. Items for the operator

BL-Q5, BL-Q6 and BL-Q9 are struck by W4: their rows stay so the
numbering does not move. BL-Q4, BL-Q8 and BL-Q16..Q18 are marked by W6,
which adopted CLA's proposal whole, not item by item; W8 then deferred BL-Q4 to rung 4 and re-read BL-Q18, and W9 marked BL-Q19 and BL-Q20. Every other item is unmarked. Every
proposal is `[design]` unless a cell says otherwise.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BL-Q0 | **Open BL**, with BL-S0..S5 and BL-R4..R9 as written, **after BI-S5 closes** (W2's premise). BL-R1 and BL-R2 are the operator's (W0, W1, W5, W6, W8); BL-R7's rungs are W8's; BL-R3 is struck (W4) | process | Yes | **Opened by W10** (2026-10-12), BI-S5 having closed at `083ab01d`; §6 |
| BL-Q1 | **W0's grain.**<br>(a) The session is never pinned; a transaction stays on its core (BL-R1).<br>(b) The statement: a transaction's statements may run on different cores | scope, **[quiet-wrong] under (b)** | (a). (b) moves the in-flight slot, the waits and the commit across cores, which CC3 forbids today and which needs its own ratification; it buys nothing for short OLTP transactions | **(a)**, W10's "per transaction" (2026-10-12); built in BL-S2 |
| BL-Q2 | **The carrier.**<br>(a) The whole connection moves at a boundary (BL-R6); a session that stays pays no hop.<br>(b) An I/O core keeps every fd, and each transaction is dispatched to an execution core: two kicks per transaction.<br>(c) A central balancer thread decides every dispatch | design | (a). (b) is BL-S1-measurable as two kick round trips per transaction and is reconsidered only if (a)'s move rate is so high that every transaction moves anyway. (c) is W3's excluded shape | |
| BL-Q3 | **The load signal** in the per-core word: (a) ready `foreground` length; (b) live sessions; (c) in-flight transactions; (d) `foreground` polled time in a decaying window; (e) a combination BL-S1 picks | measurement | (e), chosen by BL-S1 as the signal whose per-core spread best predicts the per-core p99 spread in §5.1's skew cells; (b) is the fallback when none separates | **`busy_ppm`**, measured in BL-S1 (§6): the best rank correlation with the per-core p99; at `group` none predicts it |
| BL-Q4 | **Priority's components.**<br>(a) A share only.<br>(b) A share and a latency class, the class bounded by a lag limit so it cannot exceed its share over time | design | (b), revised from (a) on W5, then deferred by W8 to BL-R7's rung 4: built only if placement (rung 2) and the two-tier pick (rung 3) both miss §5.2's bar (v) | **(b)**, adopted whole on W6 (2026-10-12), not marked per item; **deferred to rung 4 by W8** (2026-10-12) |
| BL-Q5 | ~~Affinity's form~~ | - | - | **Struck by W4** (2026-10-12): no session affinity exists (BL-R3) |
| BL-Q6 | ~~Do the axes interact in v0?~~ | - | - | **Struck by W4** (2026-10-12): one session axis remains. Its surviving half stands as BL-R4: the balancer reads a level's partition, never a weight |
| BL-Q7 | **`debug_text_port` sessions**: (a) movable like any other; (b) stay on core 0 | scope | (a). W0 says no session is pinned; the connection type is the same | |
| BL-Q8 | **Is the priority setting user-visible in v0?**<br>(a) Yes: a per-session setting after the handshake, its `transactional` level gated by the administrative role.<br>(b) No: the mechanism ships with its default only, observable but not settable, and the surface waits for a workload that asks.<br>(c) Engine-derived: priority from the statement's class (point read vs walk vs load) | user-visible | (a): the session declares one of BL-Q16's four levels; `transactional` alone is gated by the administrative role. (c) is recorded as a later direction, not v0 | **(a)**, adopted whole on W6 (2026-10-12), not marked per item |
| BL-Q9 | ~~The name~~ | - | - | **Struck by W4** (2026-10-12): with no session affinity there is no collision with the namespace's (the header) |
| BL-Q10 | **Where the per-session counters are read.**<br>(a) `SHOW META` for the asking session and per-core totals only.<br>(b) A `sys` view of every session, which the operator excluded from BO (`raft-marks-2026-10-11.md` §2) | scope | (a). (b) reverses an operator mark, which only the operator can do | |
| BL-Q11 | **CN-10.** BL builds §3.4's carrier with a load trigger, not §3.5's admission. (a) BL's close marks U6 as built by BL, with the trigger noted as BL-R4's; (b) U6 stays open until §3.5 exists | process | (a) | |
| BL-Q12 | **Priority inversion.**<br>(a) None in v0; a counter of waits whose holder's level is below the waiter's.<br>(b) Inheritance: a holder's session is polled at its highest waiter's level until the release | design | (a), with (b) taken up only if §5.2 counts inversions on more than 1 % of lock waits in its mixed cell. The counter splits by the pair of levels: a `background` writer holding row `X` against a `transactional` writer is the combination BL-R7 expects | |
| BL-Q13 | **Yielding writes and index walks** (BL-R7's remainder). (a) Out of BL, named in the close; (b) in BL | scope | (a). It is a suspension-safety argument over locks and borrows, an order of its own | |
| BL-Q14 | **BL-S1's premise gate.** BL stops for a ruling when any of: (i) under §5.1's skew cells, the balanced arm improves neither aggregate throughput nor per-core p99 spread by more than the run-to-run spread; or (ii) under the mixed cell, the same-core point read's p99 is already within 2× its isolated p99, so priority has nothing to serve; or (iii) one value of the mixed cell's slice sweep brings it there, in which case CLA proposes rung 1 alone (BL-R7). The 1,024-row inserter does not yield, so no slice value moves its share of the p99; (iii) is judged against the reactor-hold split, not the raw p99 | process | As written. (i) gates BL-S2/S3; (ii) and (iii) gate BL-S4. Each gate is independent | Measured in BL-S1 (§6): (i) passes on the spread; (ii) does not stop; (iii) serves a walk alone and not the mixed cell, whose stall is the WAL's - BL-S4 stopped for a ruling |
| BL-Q15 | **The measurement** (§5.2), at BL's close, per CLAUDE.md's Session Workflow step 3 | process | As written | |
| BL-Q16 | **The level set** (W5).<br>(a) Four context levels, `transactional`, `normal` (the default), `analytic`, `background`, each a bundle of mechanisms (BL-R2).<br>(b) Points on one scale (`high`, `normal`, `low`) | user-visible | (a). A scale cannot separate W5's contexts: a share does nothing for a short update, and "gives way first" and "runs on what is left" are different rules. The names are a closed set and hash as identifiers | **(a)**, adopted whole on W6 (2026-10-12), not marked per item |
| BL-Q17 | **Is `analytic` read-only?**<br>(a) No: a scheduling level only; a write runs at `analytic`'s level.<br>(b) Yes: a write in an `analytic` session is refused | scope | (a). (b) turns a scheduling hint into an access limit, which belongs to the statement-class roles, not to a priority | **(a)**, adopted whole on W6 (2026-10-12), not marked per item |
| BL-Q18 | **`background`'s form.**<br>(a) The idle class inside `foreground`: polled only when no other level's poll advanced, under a floor of one poll per F.<br>(b) The lowest share inside `foreground`.<br>(c) The `maintenance` group | design | (a). (b) still takes a fixed fraction from a busy core, which is not "runs on what is left"; (c): BL-R2 | **(a)**, adopted whole on W6 (2026-10-12), not marked per item. **Under W8** v0 places `background` on the batch cores (BL-R2); (a)'s idle class waits for rung 4, and rung 3 gives it a floor behind `analytic` |
| BL-Q19 | **The partition's size** (BL-R2).<br>(a) An engine constant, `max(1, cores / 4)` batch cores, the highest-numbered, never core 0, sized by BL-S5.<br>(b) A setting | user-visible | (a). A setting is a second quantity with no workload asking for it yet; BL-S5 reports the idle capacity that would argue for one | **(a)**, W9 (2026-10-12), as CLA proposed |
| BL-Q20 | **While the partition is active, may an interactive session borrow a batch core that hosts no batch-level session?**<br>(a) No, in v0.<br>(b) Yes: a `normal` session may be placed there, and is moved off at its next boundary when a batch-level session arrives | design | (a). (b) recovers the idle capacity at the cost of one more input to `decide` and a move every time a batch session arrives; taken up only if §5.2 shows the idle capacity material | **(a)**, W9 (2026-10-12), as CLA proposed |

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
  reactor-hold distribution by holder kind (§1.6). **Swept over
  `pages_per_slice` 64, 32 and 16**: rung 1 of BL-R7, and BL-Q14 (iii)'s
  input.
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
- **Partition fidelity.** Under the mixed load at `cores = 4`, every
  statement's core against its session's level.
- **Mixed cell** under BL at `cores = 4`: §5.1's mixed load with the point
  readers `transactional`, the 10,000-row walker `analytic`, the
  multi-row inserter `background`, and a second 10,000-row walker at
  `normal` that shares the interactive cores with the readers, placed by
  the balancer; also with the point readers at `normal`. Report the point read's p50/p99/max against
  its isolated run, the batch-level sessions' throughput, and the batch
  cores' idle time while no batch-level session runs (BL-Q20). The same
  load at `cores = 1`, where only the slice acts, is reported beside it
  with no bar.
- **Inversions** (BL-Q12's counter, by pair of levels) in the mixed cell
  with lock contention.
- **Bars:** (i) the overhead A/B within run-to-run noise; (ii) arm D's
  per-core p99 spread under B within that of arm B (balanced at accept);
  (iii) while the partition is active, no statement of a batch-level
  session on an interactive core, nor of an interactive-level one on a
  batch core, after its first boundary;
  (iv) moves bounded by the dwell, with no session moving more often than
  once per dwell window, partition moves apart; (v) in the mixed cell at
  `cores = 4`, the `transactional` point read's p99 at most 2 × its
  isolated p99 at the same placement. Missing (v) opens rung 3 only if
  the reactor-hold distribution attributes the excess to a `normal`
  session sharing the reader's core (BL-R7); otherwise it is reported as
  BL-R7's remainder. Run counts and
  any bar not met are stated.

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

### BL-S0 amended 2026-10-12 — priority served by placement first (W7, W8)

Amended on `worktree-bl-drop-session-affinity` on top of `f9f0c6fa`
(`v2.7.0-855-gf9f0c6fa`), as a document-only change. No file under
`src/`, `include/` or `tests/` moved, and no suite ran.
`include/kds/storage/page_latch.hpp:56-61` was read at `f9f0c6fa`, not
run.

- BL-R2 keeps W6's four levels and their privileges and serves them in v0
  by an interactive/batch core partition and the per-level walk slice;
  BL-R4's `decide` reads the level's partition; BL-R5 places an accepted
  connection as `normal`; BL-R7 is four rungs (one slice; placement - v0;
  a two-tier pick; the weighted pick, whose reviewed rules stay at
  `f9f0c6fa`); BL-R8 and BL-R9 follow. The W5/W6 entry above describes a
  pick v0 no longer builds.
- BL-Q4 (b) is deferred to rung 4 and BL-Q18 re-read; BL-Q14 gains (iii),
  the slice sweep; BL-Q19 (the partition's size) and BL-Q20 (borrowing an
  idle batch core) are added, unmarked.
- BL-S4 is rewritten as the levels served by placement; §5.1's mixed cell
  sweeps the slice; §5.2's bar (iii) is the partition's; bar (v) loses
  the walker's poll term and the inserter exclusion, since placement takes
  both off the reader's core; the share and floor bars (old (iii), (vi))
  are gone with the pick.
- OS priorities for reactor threads are a non-goal (§0), on the page
  latch's spin-then-yield wait.
- `raft-marks-2026-10-12.md` gains §6, the operator's words W7 and W8.
- W9 marks BL-Q19 (a), the partition an engine constant, and BL-Q20 (a),
  no borrowing in v0 (`raft-marks-2026-10-12.md` §7).

**The W7/W8 amendment's review** (`critics-developer`, 2026-10-12, over the
uncommitted diff on `worktree-bl-drop-session-affinity` on top of
`f9f0c6fa`) found the partition, as first written, idle for every
workload that declares no level - a fixed `max(1, cores / 4)` cores
serving nothing, half the instance at `cores = 2` - which broke bar (i)'s
default-settings A/B and BL-S3's two-per-core cell by construction; it is
now active only while a batch-level session lives. It also found that
bar (v)'s cell held nothing rung 3 reorders (a `normal` walker now shares
the readers' cores, and (v) opens rung 3 only on that attribution), that
rung 1 cited the wrong gate and implied levels that do nothing (it now
stops for a ruling under BL-Q14 (iii)), and that "never shares a core"
overstated v0 before the next boundary. The stale decision lists, BL-S5's
spec sections, the slice's reach above one core and three cuts were
applied. No finding was rejected; its item 9, the per-level slice
protecting no interactive level above one core, is recorded in BL-R2
rather than changed, since W8 adopted the slice.

### BL opened 2026-10-12 (W10) - BL-S1, BL-S2, BL-S3 landed; BL-S4 stopped

**W10** (2026-10-12, `raft-marks-2026-10-12.md` §10) opened BL and set how it
runs: *"Work according to the BL milestone. Split it into work units; if a
work unit has no utility, explore alternatives thoroughly, and if none is
found, reject it, report and stop. The purpose of this milestone is to place
cores per transaction rather than per session, to optimize scheduling, and
to handle each finely by its character. Establish the four stages, but
choose the detailed implementation by experiment and measurement. Not every
concept in the order needs to be developed; what experiment and measurement
show to be meaningless may be skipped."* BI-S5 had closed at `083ab01d`, so
W2's premise held. Worked on `worktree-bl-session-balancer` from `a3208be7`.
The measurements are
`bench/v3.0.0/results-bl-s1-placement-premise-v2.7.0-857-g3552a03c.md`.

**BL-S1 - built, measured** (`3552a03c`). `sched::CoreLoad`: one slot per
core, written above one core only, read from any - busy and `foreground`
fractions over a 50 ms window, queued `foreground` tasks, sessions,
transaction heads and statements inside a transaction - printed by
`SHOW META` as `load_c<k>_*` with each core's contention slot
(`load_c<k>_waited_<latch>_us`), which is the holder split. Gates:
- **(i) passes on the spread, not on throughput.** A balanced placement
  takes the per-core p99 spread from 1.12-1.78x to 1.01-1.05x and the worst
  core's p99 down 2-25 % at `relaxed`; throughput moves -10 % to +6 %, which
  this host (clients on the reactors' SMT siblings) cannot attribute to the
  server. At `group` nothing separates the arms.
- **(ii) does not stop BL**: the one-core mixed reader's p99 is over 1,000x
  its isolated p99.
- **(iii)**: a walk alone is served by the slice (16 pages: 321 us against an
  isolated 379), but no slice moves the mixed cell, whose stalls are not
  reactor-side (BL-S4 below).
- **BL-Q3**: `busy_ppm` (rank correlation with the per-core p99 0.36, against
  0.33 `foreground_ppm`, 0.15 `sessions`, -0.07 queued tasks; level with
  `foreground_ppm` where no statement writes, 0.30 against 0.32).

**BL-S2 - built** (`3552a03c`). At a transaction's head - a statement asked
for with no transaction open - a listener asks its `TransactionPlacer`, and
a core other than its own takes the whole connection: the map node travels
through `ConnectionHandoff`'s inbox, the source unregisters the socket before
the destination registers it, a finished statement's reply is flushed first,
and the statement whose head moved it runs first on the destination. A
transaction never crosses (BL-Q1 (a), the reading W10's "per transaction"
names). Cells: a head moves, a transaction stays where `BEGIN` moved it, a
pipelined statement travels, a prepared statement executes on the other
core. **The movable predicate is one clause**, `!in_explicit_txn()`: portals,
pipelined frames, the reply tail and the handshake state travel inside the
`KwpSession`, so BL-R1's clauses 3-5 are moot rather than checked; no
dispatch reaches the placer before the handshake. **The clock is not
re-pointed**: every core's scheduler is handed the instance's one clock.
Move cost (§5.1's last cell, by a measurement build): +28 us at a
statement's p50 on two cores, +39 us on four.

**BL-S3 - built, measured** (this row's commit). `LoadPlacer`, the default
placer above one core, moves a session at a head to the least-busy core when
the gap passes `kMarginPpm` (200,000: above the widest gap read between evenly
placed cores, `[provisional]`), the move shrinks the gap (the session's share
- its core's busy fraction over the sessions active in the window - is under
the gap, so a lone session never moves), the target published within
`kStaleNs` (four windows: a core inside a hold draws nothing), no session left
this core in this window, and the session has stayed `kDwellNs` (500 ms).
**The first rule did not hold** and is recorded: requiring the share under
*half* the gap, over every connection, left 3/1 splits in place, and one run
took 88 moves in 12 s with its p99 at 1,052 us; its review found it, the rule
was corrected and the convergence re-run. From a skewed start the corrected
balancer, in every run, brings the client p99 spread from 1.49-2.11x to
1.10-1.15x, the worst client's p99 down 12-29 % and throughput up 2.3-6.5 %
over the same skew - the balanced placement's or better - at under six moves
a second across sixteen sessions. **BL-R5 (the accept placement) is not
built**: the balancer corrects the kernel's placement at a session's first
head, and the fallback's round-robin was not separately priced
(`known-gaps.md`).

**BL-S4 - stopped for a ruling.** Its premise - that a session placed on a
core holding no batch session is protected from it - is **false as the engine
stands, and the reason is outside BL**: one bulk-inserting session stalls
readers on every core for 0.25-0.8 s through the one WAL. A repeated pk
point `SELECT` makes a logged catalog write (`Catalog::TouchPattern`, the
pattern's best-effort `use_count`/`last_seen`), and a segment roll holds the
stream latch across a device sync; core 0's group-commit drain meets every
roll. A forced partition is measurably better than none (schedule p99
422-618 ms against 646-793 ms spread, five runs each) and stalls for half a
second all the same. **The alternative was found and measured**: with
Waystone recording off and the readers off core 0 and off the batch core,
the readers' schedule p99 is 0.57-0.76 ms in two runs and 41-102 ms in two
more (a checkpoint turn on a reader core), against 0.49 ms alone; with the
batch sessions on core 0 instead, 1-19 ms. Taking it needs three
rulings, none of them BL's to make:
1. **The read path's logged write** (Waystone's spec, `rules.md` §5's CR6):
   (a) delete `TouchPattern` - the heat stays as registered, and its readers
   are the heap-only relayout planner (dark since SUS-1) and `SHOW`;
   (b) make the two heat fields unlogged, a second exception to CR6 inside a
   logged page; (c) keep it logged but coalesce per core in memory and fold
   it on a `system` tick, which still meets a roll once per tick. CLA
   proposes (a).
2. **Core 0's role under a partition** (BL-R2 says core 0 is never a batch
   core): the measurement says it is the best one, since its drain meets
   every roll. CLA proposes that the batch partition be core 0 first.
3. **Whether a roll keeps the stream latch across its device I/O** (`wal.md`
   §6 step 1, AL-R1's boundary): releasing it for the roll is the structural
   fix for writers too, and a WAL order of its own. CLA proposes it be
   written as one, not taken inside BL.

**Not measured**: the overhead A/B (§5.2 bar (i)), owed at BL's close; BL
does not close while BL-S4 waits. Nothing here is a tag or a push.
