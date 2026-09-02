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
