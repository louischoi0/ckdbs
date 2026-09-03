# AR0 — Architecture Revision: Single WAL, Shared-Memory MVCC, Ownership → Affinity

Status: DRAFT, pending operator ratification. **The first milestone (§8
step 3, M0) has the operator's go-ahead of 2026-09-02** ("set first
milestone go ahead for it for an hour task"); work order
`instructions/v3.0.0/workorder-al-m0-single-wal.md` enacts it. Every
other D-item stays pending except those M0 consumes, which AL-2 names.
Author: CLA
Scope: architecture-level revision of `docs/blueprint-range-ownership.md`, `CLAUDE.md` guidelines G1–G3, and the SA/SB ratifications
Claim tags: every claim in this document is `[design]` — reasoned from the existing design documents and prior measurements as summarized in project memory. Nothing here is `[measured]` or `[source-read]` in this session. Items marked `[source-read required]` must be verified against the repository before implementation.

**§§0–8 below are the operator's copy of the draft, verbatim. The
source-read verification of 2026-09-02 is appended as AR0-V; where AR0-V
and the body disagree, AR0-V is what the tree says.**

---

## 0. Background

The current engine is shared-nothing: thread-per-core, per-core WAL streams, range ownership as the write-serialization unit, ring-based inter-core transport, and three guidelines (G1 no atomics outside ring indices; G2 cores=1 zero overhead; G3 LSN stream-local).

Three auxiliary features (Assertion, Cabin, physical optimizer) have accumulated blocking complexity that originates not in the features themselves but in the ownership boundary: 2PC read-participant enrolment, scoped authority (SB), the Cabin/split mutual-exclusion invariant, `RefuseAuxiliaryOnSplitRelation` (Scenario 2 cannot run on a spread relation), and mover-dependent placement deferred to R5.

Independently, v2.1.0 established that commit batching, not parallelism, governs ingest throughput; and cross-owner commit costs (~3 ms) are of a magnitude that indicates scheduling/message latency rather than coherence cost, and are plausibly the same root as the ~11 ms periodic stall and the 92–98% unattributed reactor wall clock (RW-C1, `sched.md §4`).

## 1. Decisions taken in this session (operator, verbal)

- **AR0-1** Single WAL stream. Per-core WAL streams are retired.
- **AR0-2** Shared-nothing is retired as the memory model. Thread-per-core execution and cooperative scheduling are retained.
- **AR0-3** Cut-vector snapshot design is **dropped**. With a single WAL, commit order is total and a snapshot is a scalar LSN. `docs/spec-page-lsn-cross-stream.md` is superseded.
- **AR0-4** Core ownership of relations/ranges is decomposed into three independent properties (§2); only two survive.

## 2. Ownership decomposition

| Property | Fate | Rationale |
|---|---|---|
| Write-serialization authority (only the owner may write) | **Retired** | Replaced by row locks + page latches under single-LSN MVCC |
| Execution affinity (the core that last touched a range has warm cache) | **Retained as optimizer hint** | Misplacement becomes a slow answer, never a refusal or 2PC; resolves the "user cannot see placement" usability defect |
| Allocator authority (range-unit id issuance) | **Retained** | Required by pre-issued id API; cheap; independent of data access |

Everything derived from **pk immutability** (client-side routing, ABA-free external references, cascade-free FK, range-unit consistent export, Waystone hints, pre-issued id API) survives unchanged; routing and Waystone become performance hints instead of correctness conditions. Export becomes simpler: any range at any single LSN.

## 3. Guideline revisions

- **G1 (revised):** No atomics outside ring indices and the lock/latch primitives. All latch and lock primitives compile to no-ops at cores=1.
- **G2 (unchanged):** cores=1 zero overhead. Enforced by the compile-out clause of G1.
- **G3 (retired):** LSN is global and totally ordered. Cross-stream comparison is no longer a concept.
- **Shared-nothing principle:** replaced by "shared memory, thread-per-core execution, single log appender".

## 4. Consequences per feature

### 4.1 Assertion
- SB scoped authority (owned range × observed value), ordered pre-grant discard, and the T2a–T2d atomic bundle are **retired**.
- An assertion is evaluated inside the writing transaction under locks. Correctness now depends on phantom prevention on the aggregate key (predicate/gap locks), not on distributed authority.
- Risk class changes from *refusal* to *quiet wrong answer* if gap locking is incomplete. See D1, D8.

### 4.2 Cabin
- Invariant `(Cabin≥1 ∧ range≤1) ∨ (range≥2 ∧ Cabin=0)` is **retired**.
- `RefuseAuxiliaryOnSplitRelation` is **removed** (previously reclassified defect; the fix by forced synchronous coalesce is no longer needed).
- `RangeEligible kCabin` gate is removed.
- Bound Cabin = latched write-coupled structure updated in the writer's transaction. Observational Cabin = read at snapshot LSN. The class distinction reduces to lock strength.
- Scenario 2 (`SELECT SUM(cbm) FROM freights WHERE operation_id = ?`) runs on any relation regardless of spread.

### 4.3 FK
- SA cross-core FK design (2PC read-participant enrolment, row-scoped reference intent) is **retired**. See D9.

### 4.4 Physical optimizer
- Placement dimension (local vs ship, fan-in cost, 2PC cost) is removed from the cost model.
- Affinity term is added (D10). Until measured, weight = 0, i.e. the optimizer is a conventional access-path/join-order optimizer with optional intra-query fork/join across cores.
- R4-S read shapes (projection, ungrouped aggregate, grouped aggregate, join) require no ownership-aware design.

### 4.5 Milestones
- R5 (range mover) as a *data* mover is **retired**. Affinity rebalancing is a table update, not data movement. See D11.
- `sys.access_stats` at fixed page `kCatalogPageAccessStats = 11` no longer blocks anything.
- XD (cross-owner 2PC) is **retired**. 2PC no longer exists inside a single node.
- The R-series blueprint is superseded; see D16 for naming.

## 5. Items requiring operator judgment

Per standing rule, CLA proposals are accepted by default except where the item fixes a constant or converts a refusal into a possible silent wrong result. Every item below is in one of those classes. Each has a CLA proposal.

**D1 — Isolation level target.**
Options: (a) Snapshot Isolation only; (b) SI + explicit gap/predicate locks on assertion and FK paths; (c) full serializable (SSI or S2PL).
Proposal: **(b)**. SI gives read-only statements latch-free progress at a snapshot LSN; the quiet-wrong surface is confined to write skew, which for this engine matters exactly on assertion and FK paths, and those get explicit locks. (c) taxes every read for a guarantee only two paths need.

**D2 — Lock manager structure.**
Options: (a) shared partitioned lock table with spinlocks (atomics); (b) lock table partitioned by key hash across cores, requests via ring to the partition owner.
Proposal: **(a)**. (b) reintroduces one ring round-trip per lock — the very cost being removed — and its latency is currently unattributed (RW-C1). Compile-out at cores=1 satisfies G2. Partition count is a constant: proposal 64 × cores, to be re-measured. `[constant]`

**D3 — Log appender.**
Options: (a) dedicated log core, other cores fan in via ring; (b) every core appends with an atomic tail; (c) rotating appender.
Proposal: **(a)** for cores≥2; at cores=1 the single thread appends directly (G2). The log core is the natural group-commit point, consistent with the v2.1.0 finding. Group-commit window is a constant: proposal keep the current `b=8`-validated value as baseline and re-measure after the WAL change. `[constant]`

**D4 — Free-map / superblock access rule** (previously deferred by operator).
Proposal: allocator authority for free-map and superblock sits on the log core; requests via ring; at cores=1 direct. This is allocator ownership under §2, not data ownership. Page LSN becomes scalar; no rule change needed for page headers beyond dropping the stream id field. `[source-read required: page header layout]`

**D5 — `range_size_ids = 4,096`.**
Range persists as id-allocation, export, and affinity unit. Proposal: **keep 4,096**; K-f validated it as the throughput optimum, and the mechanism argument (allocator batch size) is unaffected by this revision. Re-measure only if D6 arms affinity spreading. `[constant]`

**D6 — DA1 arming default.**
"Spreading" now means affinity spreading, not ownership spreading. Proposal: **disarm by default** (`kRangeSizeOff`), consistent with the earlier un-arming decision, until the affinity term (D10) has a measured non-zero weight. `[constant]`

**D7 — Cabin invariant removal.**
Mechanically a deletion, but it converts a gate into a lock-protected path. Proposal: remove the invariant and both gates in the same change as D1(b) gap locking on Cabin keys, not before. `[quiet-wrong]`

**D8 — Which keys receive gap locks.**
Proposal: every column named in an assertion predicate or aggregate group key, and every FK referenced key. Assertion definitions must declare their locked keys explicitly at CREATE time so the set is inspectable; implicit derivation is rejected on quiet-wrong grounds. `[quiet-wrong]`

**D9 — FK check mechanism.**
Options: (a) shared row lock on the parent row held for the child transaction's duration; (b) SI write-write conflict via a version bump on the parent.
Proposal: **(a)**. (b) turns every child insert into a parent write, which serializes all children of one parent at the log. Cascade-free FK (pk immutability) remains: parent delete takes an exclusive lock and checks for children under the same gap lock as D8. `[quiet-wrong]`

**D10 — Affinity term in the optimizer.**
Proposal: add the term now with weight **0**; set it only after RW-C1 attribution yields a measured cross-core cost. The current ~3 ms figure must not be used as the weight; it is presumed to be scheduler latency, not coherence cost. `[constant, measurement-gated]`

**D11 — R5 mover.**
Proposal: **retire R5 as a data mover**. Replace with an affinity table (`sys.range_affinity`) updated by the log core from access statistics; no data movement. `sys.access_stats` fixed page stays as-is. `[source-read required: catalog page allocation]`

**D12 — Deadlock handling.**
Options: (a) timeout; (b) wait-for graph detection on the log core.
Proposal: **(b)** with a safety timeout. Detection preserves throughput on hot keys (operation_id); timeout alone converts contention into spurious aborts. Timeout value is a constant: proposal 1 s. `[constant]`

**D13 — Cooperative scheduler blocking model.**
Lock waits must not block a core's reactor. Proposal: lock acquisition is asynchronous; a waiting task yields and is re-enqueued by the lock manager on grant via the existing ring wake path. This is the one place the scheduler and the lock manager couple. `[source-read required: sched wake path]`

**D14 — Migration of on-disk format.**
Single-LSN pages are incompatible with per-core-stream pages. Proposal: no in-place migration; new major version mounts only volumes created by it. Online core-count change remains a non-goal; mount-time reorganization remains supported.

**D15 — Versioning and baselines.**
Proposal: this revision is **v3.0.0**. All v2.x measurements are non-comparable and must not be restamped. The first task after the WAL change is a fresh baseline on the same host as the last v2.x baseline, named by `git describe --tags`, build-release only.

**D16 — Milestone series naming.**
The R-series (R0–R6) is superseded. Proposal: **M-series** for the revision (M0 single WAL + log core; M1 shared buffer pool + latches; M2 lock manager + D1/D8/D9; M3 Cabin/Assertion/FK on the new substrate; M4 R4-S read shapes + whole-workload cores=1-vs-N cell). Work-order prefixes continue (next free letter series).

## 6. Prerequisite

**RW-C1 attribution must complete before D6 and D10 are set to non-zero and before M0 baselines are interpreted.** The revision's performance case partially rests on the claim that cross-core cost is scheduler latency; if attribution shows otherwise, D2 and D3 should be reconsidered.

## 7. Retired artifacts

- `docs/spec-page-lsn-cross-stream.md` — superseded
- SA R1–R8 (index locality, Cabin class separation, gate narrowing, FK 2PC design) — R2/R3 gate items retired; pk-immutability items retained
- SB (scoped authority, pre-grant discard, T2 bundle) — retired
- XD work order — retired
- CIP OPT-001…OPT-006 — each must be re-evaluated against the new substrate; none carries over automatically

## 8. Sequencing

1. Ratify D1–D16 (this document).
2. RW-C1 attribution (parallel, independent).
3. M0: single WAL + log core fan-in; cores=1 path unchanged; baseline.
4. M1: shared buffer pool, page latches, scalar page LSN.
5. M2: lock manager (D2), async waits (D13), deadlock detection (D12), gap locks (D8).
6. M3: Cabin invariant removal (D7), Assertion, FK (D9).
7. M4: R4-S read shapes; whole-workload cores=1-vs-N cell — the cell deferred since v2.1.0 is run here.

---

## AR0-V — Verification record (CLA, 2026-09-02, source-read at `d15b5ac`)

Every citation to a removed tree resolves at `1769487`. The body above
is untouched; this section says where the tree disagrees with it.

### AR0-V1 — The performance premises of §0 and §6 are already attributed

| §0 claim | What the tree says |
|---|---|
| the ~3 ms cross-owner commit "indicates scheduling/message latency" | `instructions/v2.7.1/measurement-xd.md` (at `1769487`) decomposes it into **three serialized device syncs**, ~1,002 µs per leg at b=1 (XD3, additive within 8.2%), the b=1 cell putting the protocol's fixed part at 1.9 ms. The 3.1 ms leg measured 2026-09-02 (`bench/v2.8.0/results-ah-t6-participant-release-cost-v2.7.0-101-ged47cfc.md`) is the protocol's **unconditional decision-durability wait plus a decide round trip**; `relaxed` does not collapse it |
| "the ~11 ms periodic stall" | `bench/v2.3.0/results-knob-sweep-cell2-v2.2.1-14-g13c6d4d.md`: a **1–2 ms stall with an ~11 ms period**, present on the *seated* arm (no ring, no wake), surviving both WAL timing knobs, handed on with kernel writeback against the WAL file as the unestablished suspect |
| "92–98% unattributed reactor wall clock (RW-C1, sched.md §4)" | `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §8b: 94–98%, attributed there to **"the WAL drain's `fdatasync` above all"** — a group-commit sync runs on the reactor thread. `sched.md` §4 carries the accounting rule and no number; RW-C1 is described only at `instructions/v2.5.0/cross-owner-protocol.md:253`, as a C-state experiment (three later hand-on lists name it bare) |

Consequence: §6's prerequisite is answered by these files, in the
direction that a single sync point off the execution cores is supported
and D2/D10's "scheduler latency" premise is not.

### AR0-V2 — Values and artifacts named as they are not

- **D5.** `range_alloc.hpp:150` sets `kRangeSizeIdsDefault = 65536`;
  `expeditor.hpp:487` ships `kRangeSizeOff` since the 2026-08-31
  amendment (`1b27d68`). K-f is `bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
  §6d, where 4,096 is the *group arm's optimum* and 65,536 the ratified
  size (16× the read ceiling). "Keep 4,096" keeps nothing.
  `manual/server/server.md:98` states the default as 65536 while the code
  ships 0 — a drift independent of AR0.
- **D6.** Consistent with `1b27d68`, which made spreading a per-relation
  option the user decides, flag unbuilt (no room on `SysTableRow`, no
  `WITH` syntax, `expeditor.hpp:471-477`). With it off the pk is a
  sequence; affinity spreading must restate that promise.
- **"G1–G3 in CLAUDE.md."** Absent from `CLAUDE.md` at `d15b5ac`,
  `7f0193b` and `1769487`. The statements live in `sched.md` §5 and §9
  (invariants 2, 7), `crosscore.md`'s "zero instructions over the
  single-core code", `wal.md` §3, `rules.md` §3. Those are the revision
  targets. "No atomics outside ring indices" is already false: 17
  `std::atomic` uses in 8 files — `wal/writer.hpp` (`requested_`,
  `durable_`, a mutex and a condition variable at the device boundary
  under `rules.md` §3), `scheduler.hpp` (`sleeping_`, counters),
  `waker.hpp` (counters), `crash_point.cpp`.
- **Paths.** `docs/blueprint-range-ownership.md` is
  `docs/inflight/in-progress/blueprint-range-ownership.md` at `1769487`.
  `docs/spec-page-lsn-cross-stream.md` is `docs/spec/page-lsn-cross-stream.md`,
  in the tree at `d15b5ac`: the PL-B handoff with the PL-C stamp.
  "Cut-vector" appears at no commit.
- **Names.** "R4-S" appears nowhere at `1769487` (R4-R and RS exist). XD
  is the commit-cost *measurement* order; XE moved the ack; the protocol
  is `docs/spec/cross-owner-txn.md`. T2a–T2d are SB's Observational
  Cabin tasks, not assertion items. "The fix by forced synchronous
  coalesce" has no referent (the only coalesce ratification is AX, on
  auxiliary DDL). **M0–M6 already name `workplan-crosscore.md`'s
  milestones at `1769487`** and SA cites M2/M5: AR0's M-series is cited
  as "AR0 M*n*", never bare (`CLAUDE.md`, "cite the file").
- **v2.8.0 already dropped the range split.** `instructions/v2.8.0/ratification-ae.md`
  (2026-09-01) removes range-granular parallelism, keeps cross-owner 2PC,
  withdraws the range-shaped half of SA/SB; AK-S1..S3 landed
  (`826b37b`..`1769487`). At `d15b5ac` the `kCabin`/`kIndex` arms of
  `RangeEligible` are already down (`range_eligible.hpp:76`),
  `cabin.md` §4b holds SB-R1's scoped authority, and
  `RefuseAuxiliaryOnSplitRelation` keeps the index, FK and assertion arms
  only (`catalog.cpp:3162,:3332`, `assertion_catalog.cpp:208,:461`). Of
  §4.2's three removals one is done.
- **"b=8"** is a benchmark session batch, not an engine constant; the
  constant D3 means is `wal_drain_interval_ns` (`expeditor.hpp:495`, 1 ms).
- `CIP/` survived the compaction as a live root directory (six proposals
  with results). Scenario 2's statement is confirmed at
  `tools/scenario2_freight.py:831`; the v2.1.0 batching finding in that
  tag's message.

### AR0-V3 — The three `[source-read required]` items

- **D4, page header.** `page_header.hpp` assigns all 32 bytes
  (`page_type`, `format_version`, `flags`, `checksum`, `page_lsn`,
  `relayout_epoch`, `owner_oid`). The stream stamp is the *meaning* of
  `flags` at offset 2 (`core_id + 1`), so dropping it is a semantic change,
  not a layout change. The format that changes is the **superblock**: the
  per-core anchor table (`kMaxWalCores = 64` slots) and the pinned
  `core_count` (`superblock.hpp:255-341`). The free-map rule is not
  deferred: `crosscore.md` CC11 states it (every core reads, core 0 alone
  writes; growth, extent leasing and region creation are core 0's; the
  store enforces it). D4 must say whether the log core is core 0.
- **D11, catalog page.** `well_known.hpp:367` pins page 11 and calls it
  R5's gate; CC13/CR7 has peers batch to core 0 and `SysAccessStatRow`
  carries no `core_id`. `sys.ranges` at page 15 already holds a per-range
  `owner_core` (`rows.hpp:951`): affinity is that column re-scoped, not
  a new `sys.range_affinity` (the no-second-name rule).
- **D13, wake path.** One eventfd per reactor (`waker.hpp`), written only
  when the target's `sleeping` flag is set, with a seq_cst fence on both
  sides (`sched.md` §7); a parked task is re-polled each iteration and the
  idle block is bounded by 10 ms. A grant is a ring message on the
  grantor→waiter edge plus the wake; the D2 group-commit waiter already
  uses the same poll-a-predicate pattern (`wal/manager.hpp`). Feasible as
  written.

### AR0-V4 — Gaps the body leaves unstated

- **AR0-3 has no mechanism.** Visibility at `d15b5ac` is trx-id based
  (`txn.md` §4.1: `up_to_trx_id` + `in_flight[64]`) and the 20-byte MVCC
  header (invariant 12) carries no commit LSN. What is missing is not id
  *uniqueness* — `cross-owner-txn.md:279-281` states there is already one
  instance-wide trx-id sequence, leased per core in 4,096-blocks
  (`trx_id.hpp:74`) — but a total commit **order**: ids are issued in
  lease order, not commit order, so "below the high-water mark" is not
  "committed before me" across cores. A scalar-LSN snapshot therefore
  needs a trx-id → commit-LSN mapping, or visibility keyed on the commit
  record's LSN. `cross-owner-txn.md:287` rejects the global **commit
  sequence** that would supply it; AR0 reverses that and must say so.
- **Three per-core facts must go instance-global**: `ReadHorizon()` for
  the undo purge, the `in_flight[64]` cap ("as `kMaxWalCores`"), and
  EV4's strictly per-core buffer pools (`eviction.md`), which M1 reverses
  without citing it.
- **D1 reopens a closed sentence.** `txn.md` §1: SERIALIZABLE is "out of
  scope and not [OPEN]" because predicate locking fits no lock manager;
  D1(b) and (c) both change it. The Keystone lock byte `txn.md` §5 calls
  unused is a home for a row lock.
- **D12's 1 s constant duplicates a knob.** `in_doubt_ceiling_ms` (200 ms)
  is dead once 2PC goes; a lock-wait ceiling is its re-scope.
- **D2 and D3 accept different hop ratios**, and D3(a) as written cannot
  carry a `FULL_PAGE_IMAGE` through a ring slot. Work order AL's AL-R1
  takes this up.

---

## AR0-M — The operator's marks of 2026-09-03

Recorded by CLA on `worktree-commit-order-ratification` at `f027a3c`
(`v2.7.0-158-gf027a3c`). **The body above and AR0-V are untouched**; this
section records which D-items now carry the operator's mark, what each
mark carries with it, and what each inherits from AR0-V's source read.
Everything not listed stays pending — D3-D7, D10, D12, D13, D14, D16
beyond what AL-2 records M0 consuming.

### AR0-M1 — D1: (b), conditionally

**The mark.** SI plus explicit gap/predicate locks on the assertion and FK
paths — §5's option (b) — taken **on the premise that RU, RR and RC are all
deliverable**, and **withdrawn and re-decided if they are not**.

**Amended by the operator the same day.** The first mark named four levels
and counted three. SR inside the condition would have made it
self-cancelling — §5's (c) is "full serializable (SSI or S2PL)" while (b)
admits any write skew falling off the assertion and FK paths — so **SR
leaves the condition and D1 stays (b)**. The condition is three levels: RU,
RR, RC.

**SERIALIZABLE stays out of scope and the *reason* expires with this mark,
in four places rather than one.** `txn.md` §1 closes SR on two grounds — no
lock manager and no row-level read tracking — and M2 builds the first. The
conclusion stands; the justification does not, and it is written out at
`txn.md` §1, in the **user-visible refusal text** at
`src/txn/manager.cpp:64-65` ("this engine has neither a lock manager nor
row-level read tracking", which M2 makes untrue on the wire),
`manual/sql/sql.md:758` and `client-manual.md:370`. The honest post-M2
wording names what is actually missing: no row-level read tracking, and
SI's write skew closed only on the assertion and FK paths (D8, D9).
Rewriting all four belongs to M2's work order. **One is already wrong
today, independently of M2**: the comment at `src/txn/manager.cpp:59-61`
says "no lock manager and **no reader registration**", which `txn.md` §1
explicitly corrects and `txn.md` §4.1 contradicts ("Readers are
registered") — a stale comment to fix whenever that file is next opened.

**What the mark rests on, and the half of the condition nothing answers.**
The three levels do not stand alike:

- **RU does not exist in the engine, and it is not free.** `txn.md` §1 says
  "exactly two isolation levels" and `ParseIsolationLevel`
  (`src/txn/manager.cpp:54-68`) refuses every other spelling. What is cheap
  is the *mechanism*: invariant 12 carries no `xmax` and
  `PageView::OverwriteTuple` is in place, so the newest version is the one
  on the page and RU is that page read with no view taken. What it still
  costs is an enum value and its spellings, a bypass at the visibility
  choke point, a ruling on what an RU **writer**'s first-updater-wins view
  is (`txn.md` §5), and the rewrite of §1's "exactly two". **Deliverable at
  M2 and dependent on nothing else** — which is the point: of the three, RU
  is the one no other mark gates.
- **RR and RC, as levels meaning one instant across the instance, depend on
  `ratification-an-commit-order.md` AN-D4, which is unmarked.** With no
  total commit order a snapshot is a per-core high-water mark.
- **So the condition cannot be evaluated yet, and one half of it has no
  answer in either document.** If AN-D4 is declined, RR and RC survive as
  **per-core** levels — what the engine ships at `f027a3c` and what
  `crosscore.md` §5 states. **Does a per-core RR satisfy "지원 가능"?** The
  mark does not say and CLA will not guess; it is the question that decides
  whether D1 stands or returns for re-decision. AN-D9 carries the
  mechanism; the question lives here.

### AR0-M2 — D2: (a)

A shared partitioned lock table, partition count 64 x cores, compiled out
at `cores = 1`.

Two things it inherits from the tree rather than from the body:

- **AR0-V1 already answered D2's stated ground.** The premise that
  cross-core cost is scheduler latency is not what the tree attributes. The
  mark stands on (b)'s own cost — one ring round trip per lock — rather than
  on that premise.
- **The primitive the option names no longer exists.** AL-7's review record
  deleted `spin_latch.hpp` at `7839a29`, and `base/latch.hpp` is now a
  `std::mutex` reached through a nullable pointer — **two predictable
  branches, which that header is careful to call a property of the code
  rather than a build flag**, unlike D2(a)'s own genuine compile-out at
  `cores = 1`. A lock table's contention is not the WAL append's —
  nanoseconds, never a holder inside `fsync` — so the deletion does not
  refute (a). It means the implementation either re-introduces a spin
  primitive or uses `base/latch.hpp`. Not decided here.

**What `rules.md` §3 requires of it either way**, at `f027a3c` and not at
the shape that section had a week ago: a justification comment for **any**
lock, in the subsystem header *and* in the owning spec; and a partitioned
lock table read by every core is a **new row in §3's declared-shared
table**, whose rule is that "Adding a row is a spec change first and a code
change second" and that "the fourth should be argued for rather than
noticed later". D2's work order owes that argument — what serializes the
table and which spec declares it — before the code, exactly as AN-Q1 owes
it for the commit-order window.

The partition count stays `[constant]` and re-measurable.

### AR0-M3 — D8: as proposed

Locked keys are every column named in an assertion predicate or aggregate
group key, and every FK referenced key; an assertion declares its locked
keys explicitly at `CREATE`, and implicit derivation is refused on
quiet-wrong grounds.

Four things the order inherits, from a source read at `f027a3c`, recorded
so the work order does not rediscover them:

1. **The text conflates two sets.** For `SUM(col) <= N` the *trigger* set —
   what makes a write require a check — is the group columns **plus the
   summed column** (`assertion.md` §4.2 checks an `UPDATE` whose sum-column
   delta is positive with the group unchanged). The *lock* set is the group
   key. "Every column named in the predicate" is the union, and locks more
   than the constraint needs.
2. **The FK half names the wrong side for the gap gap-locks exist to
   close.** "Every FK referenced key" is the parent's pk, which D9(a)'s
   shared lock already covers on the forward path. The phantom is on the
   **child** side: a child inserted while the parent's reverse walk is in
   flight. D9's own sentence ("checks for children under the same gap lock
   as D8") reads child-side; D8's text does not.
3. **A gap lock needs a structure to name the gap in, and only one of the
   two paths has one.** An assertion does by construction: the Bound Cabin
   is pinned and full-coverage (`cabin.md` §12.1), so a group key with no
   entry yet is still a lockable slot. The FK child side does not —
   `child.fk_col` carries no required index, and `foreign-keys.md` §3's
   declared fix is `CREATE CABIN ON child(fk_col)`, an **Observational**
   Cabin authoritative only for observed values, which cannot hold a lock
   for a value nobody has probed. **The work order must say what a parent
   `DELETE` locks when the child has no covering structure**; relation
   scope is the coarse answer and is not chosen here.
4. **Explicit declaration is redundant for the v1 predicate class.** AS2's
   grammar already carries the `GROUP BY` list, so for AS1's class the
   declared set and the derived set are the same list and the derivation is
   a projection, not an analysis. The requirement earns its keep only if the
   predicate class widens; as written it risks a second, user-maintained
   name for a fact the grammar already holds (`CLAUDE.md`, no second name).

**What the mark strikes.** `assertion.md` AS4 — "Reservation protocol
combined with owner-core group-key serialization. **No latches, no waiting,
no deadlock.**" — is a decision-record entry that D8 with D12 contradicts.
It is struck by whichever work order lands D8, not by this record.

### AR0-M4 — D9: (a)

A shared row lock on the parent row, held for the child transaction's
duration.

**What it retires**, all in `foreign-keys.md`, and the list is longer than
the intent:

- the **reference intent** with the dispatch-fork park that carries it
  (§2a/§2b; its row-scoped property is stated in §3a, "A live reference
  intent on the row being deleted");
- §3's "the in-place row with a foreign `trx_id` *is* the lock record; no
  lock manager exists or is needed" — the current design's whole answer to
  this problem, replaced rather than supplemented;
- **F3, and this is the one D9(a) contradicts hardest**: "Blocking is not
  expressible on a cooperative single-writer core", so a fail-fast
  `TxnConflict` is the whole concurrency answer. A shared lock *held for the
  child transaction's duration* makes a conflicting check **wait**, which
  F3 forecloses. The item that would answer it is **D13** (async lock
  waits), which is unmarked — so D9's mark reaches past itself into D13;
- §5's first bullet, "No lock manager, no wait queues, no deadlock detector
  — F3 plus in-place `trx_id` makes the uncommitted row itself the conflict
  signal", which D9 with D12 negates clause by clause.

**What it can reuse.** The Keystone lock byte `txn.md` §5 calls unused is a
home for the row lock (AR0-V4).

**What it does not move.** `foreign-keys.md` §4's check visibility — a view
minted at check time, latest-state, an in-flight writer answered busy — is
not a snapshot question. What D9 changes is that a parent seen under the
lock cannot then disappear, which is *how* the check and the transaction's
snapshot come to agree. The check is not moved onto the snapshot; the
snapshot is defended.

### AR0-M5 — D11: the R5 mover is retired

Retired as a **data** mover; affinity rebalancing is a table update.

**The shape is AR0-V3's and not the body's.** `sys.ranges` at page 15
already carries a per-range `owner_core` (`catalog/rows.hpp`), so affinity
is that column **re-scoped from an authority to a hint**, not a new
`sys.range_affinity` relation — which would be a second name for the
quantity. `sys.access_stats` at `kCatalogPageAccessStats = 11` stops being
R5's gate and blocks nothing.

### AR0-M6 — D15: the version is v3.0.0

**The operator has named the version** (`CLAUDE.md`, Version Management).
AL-R8's `bench/v3.0.0/` is now the version's series rather than a
proposal's, and `index.md`'s "the version number is AR0 D15's *proposal*"
no longer holds.

**The annotated tag is a separate act and is not taken here.**
`git describe --tags` reads `v2.7.0-*` until one exists, and a tag message
is a durability claim that carries what bounds it.

**What it could truthfully say moved twice while this section was being
written, which is the argument for writing the state rather than the
verdict.** At `f027a3c` M0's code had landed and its baseline had not, so
no milestone was complete by its own definition of done — AR0 §8 step 3 and
AL-2 both make a fresh baseline half of what M0 *is*. Merging `origin/main`
brought `6ead2a0`: **AL-S8 is measured** — eleven cells at
`v2.7.0-157-gf6ed10c` on the 8-core EPYC with data on ext4, three results
files under `bench/v3.0.0/` — with a `critics-developer` pass over those
files still in flight, and AL-S9's prose pass built. So M0 is complete or
all but, and **M1 through M4 are not built**; M1 is written and was gated on
exactly the numbers that now exist.

A `v3.0.0` tag cut here would therefore be honest only if its message says
one milestone of five is built and names which. Cut it now with that
message, or cut it when more of the series closes — CLA takes neither
reading, and nothing is pushed either way without the word.

### AR0-M7 — What the review of this section changed

`critics-developer`, 2026-09-03, ~71 tool calls against
`worktree-commit-order-ratification` at `f027a3c`. Three claims in the
first draft of AR0-M were refuted by this tree and are corrected above.

| claim | what the tree says | where it landed |
|---|---|---|
| "AR0 M0 has landed" | at `f027a3c`, AL-S8 was **in progress** in this same directory and `bench/` held only its README, a fresh baseline being half of what AR0 §8 step 3 and AL-2 define M0 to be. It also contradicted `index.md`'s own row added in the same change | AR0-M6 rewritten — and rewritten a second time when the merge of `origin/main` brought `6ead2a0`, where AL-S8 **is** measured. Both states are kept in the paragraph, because the tag argument rests on which one holds and it moved twice in a day |
| "AL-7d deleted `spin_latch.hpp`" | the deletion is **AL-7**'s review record at `7839a29`; AL-7d (`aff4e32`) is AL-S5/S6's and touches no latch header. The substance held — no `spin_latch*` exists and `base/latch.hpp` is a `std::mutex` | AR0-M2's attribution corrected; the "compile-out" wording separated from D2(a)'s real one, since the header calls its two branches a property of the code rather than of a build flag |
| "RU needs nothing new ... the cheapest read path in the engine" | there is **no RU**: `txn.md` §1 says "exactly two isolation levels" and `ParseIsolationLevel` refuses every other spelling. The *mechanism* is cheap; the level is not built | AR0-M1's first bullet rewritten to say what RU still costs, and that it is deliverable at M2 gated by nothing — the sibling document had this right and this section did not |

Three mis-scopes also applied: `rules.md` §3 is cited at `f027a3c`'s shape,
where it requires a justification of **any** lock and makes a partitioned
lock table a new declared-shared row to be argued for; AR0-M4's retire list
gained `foreign-keys.md` F3 and §5's first bullet, which D9(a) contradicts
harder than the intent does, and F3's "blocking is not expressible" is why
D9's mark reaches into the unmarked D13; and the question "does a per-core
RR satisfy the condition" now has a home in AR0-M1 rather than being
forwarded to AN-D9, which was forwarding it back.

Bloat applied: the amendment narration cut from ten lines to five.
