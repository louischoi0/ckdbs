# Multicore Workplan

Work items to take the engine from the single-core reactor (sched.md "Phase
1", the current state: one Expeditor, one Scheduler, `core_id = 0`
everywhere) to thread-per-core operation with cross-core execution
(`docs/crosscore.md`). Companion to `docs/sched.md`, `docs/wal.md` §3,
`docs/page.md` §6, `docs/protocol.md` D3.

Already fixed by existing specs, not revisited here: one pinned worker per
core with no thread creation after startup and no work-stealing (sched.md);
N² per-core-pair SPSC rings; per-core WAL streams, buffer pools, and
checkpoints; server-side forwarding with topology-unaware clients (D3).

Ordering note: the transaction milestone (`docs/txn.md` workplan) lands
first on the single core. Everything in txn.md is core-local by design, so
multicore adds instances, not synchronization — the reverse order would make
every txn test carry cross-core variables from day one.

## 1. Decision Record

| # | Decision | Resolution |
|---|----------|-----------|
| M1 | Ownership partition | **Relation-unit ownership recorded in the catalog** (`owner_core` on the relation row), assigned at CREATE. Write-coupled auxiliaries (unique indexes, Cabin, Waystone, var-heap) always co-located with the base relation; FK-linked relations co-located in v1 (crosscore.md §6). Page/extent hashing rejected: btree descent and heap-chain walks cannot cross cores per hop |
| M2 | Cross-core statements | **Cross-core read execution now** — step pipeline per `docs/crosscore.md` (CC1–CC6). Writes stay single-core per transaction; 2PC `[OPEN]` |
| M3 | Accept distribution | **SO_REUSEPORT per-core listeners**; the kernel distributes connections; a session lives on the core that accepted it (protocol D3). No fd handoff path. Session/data skew is observed via metrics, never rebalanced in v1 |
| M4 | trx-id allocation | **Single superblock counter, per-core block leases** requested from the system core over the ring; a crash burns each core's unissued remainder (extends txn workplan T3). Ids stay globally unique with no core bits in the format |
| M5 | Shared-resource ownership | **Core 0 is the system core**: owns the superblock (page 0), free map, file growth, extent leasing, and catalog pages. Other cores hold extent leases and catalog caches; checkpoint anchors are written via message to core 0 (`SuperBlock::SetWalAnchor(core_id, …)` already takes the id) |
| M6 | Core-count changes | `cores` config key; the count is **recorded in the superblock; a mismatch at startup refuses to boot**. Stream reassignment stays `[OPEN]` (wal.md §3) |
| M7 | Ring send failure | Sends are non-blocking and fallible (sched.md §7); on ring-full the sending **task yields and retries**. Never an error to the client, never a reactor block |
| M8 | Idle policy default | **busy-poll** default (appliance deployment, sched.md §6); epoll mode selectable via config |
| M9 | Deterministic simulation | **In scope for v1**: real SPSC rings and a simulated ring behind one seam; reactors stepped round-robin on a single thread with message delay/reorder injection. Without it every cross-core test is nondeterministic |

## 2. Phases

Each phase ends green: build + full test suite at `cores ∈ {1}` until P8
adds multi-core runs. The single-core configuration must behave identically
throughout — that regression check is part of every phase, not a final step.

### P0 — Config and superblock plumbing — **built (2026-08-04)**
- Add `cores` to `ConfigFile`/`Expeditor::Config` (default 1); validate
  against `std::thread::hardware_concurrency()` at startup.
- Superblock: record `core_count` at bootstrap; refuse to start on mismatch
  (M6). Fresh-format bump per the superblock versioning rules.
- Catalog: add `owner_core` to the relation row (sys.tables); bootstrap
  assigns all system relations to core 0. Development-stage row-format
  change is permitted (no compatibility shim, per the CREATE PATTERN
  precedent).
- Assignment policy at CREATE: `[PROPOSED]` round-robin over non-system
  cores, overridden by co-location (M1): an index/Cabin/var-heap/FK-linked
  relation inherits its base relation's core.

**As built.** `cores` is an `Expeditor::Config` key, pinned into the
superblock at bootstrap and validated at every mount, naming both numbers on
a mismatch — the arrangement `inline_cell_width` already had, and the check
lives beside it in `bootstrap.cpp`. Superblock **9 → 10** and the
`sys.tables` row grew `owner_core`; every pre-existing data file stops
mounting, which is the documented development-stage policy. Two findings
worth carrying forward:

- **`cores ≤ kMaxWalCores` is a hard ceiling, not a preference.** The
  superblock's WAL anchor table is indexed directly by `core_id` and has 64
  slots, so a core above it has nowhere to publish a checkpoint from.
  `server::CheckCoreCount()` is the single test, shared by the config
  overlay, bootstrap and `SuperBlock::Decode` so the three cannot disagree.
- **Co-location needed no encoding.** A relation's unique indexes, Cabin,
  Waystone pages and var-heap hang off its own catalog row and have no owner
  field of their own, so M1's co-location rule is structural — there is no
  way to spell a relation whose var-heap is on another core. The only
  co-location that will need expressing is FK-linked relations, and
  `docs/impl-foreign-keys.md` keeps those together in v1 by deferring
  cross-core FK entirely.

The placement policy is `catalog::AssignOwnerCore()`
(`include/kds/catalog/core_placement.hpp`), deliberately a free function
outside `Catalog`: the catalog *records* ownership and does not decide it,
so the `[PROPOSED]` rotation can be replaced without the catalog acquiring a
reason to know how many cores exist. Its rotation counter is **the number of
relations already on the page**, not the oid — object oids restart at
`kUserOidStart` every boot (`docs/keystoneid-k0-findings.md`), so a
placement keyed on one would re-walk the same rotation after every restart.
`DESCRIBE` carries `owner_core=`.

### P1 — Rings and reactor phase 3 — **built (2026-08-04)**
- SPSC ring implementation: preallocated at startup, per core pair, fixed
  max message size, indices as the only atomics (sched.md §5).
- One `RingTransport` seam with two implementations: real rings and the
  simulated transport (M9). Engine code sees only the seam.
- Message header: `(src_core, dst_core, kind, session_core, request_id,
  step_id)` + POD payload. Kinds enumerated centrally (crosscore.md §3 plus
  system kinds: anchor write, extent lease, trx-id lease, catalog
  invalidation).
- Scheduler: implement reactor phase 3 (cross-core inbox drain — currently
  an explicit no-op) wrapping received messages as tasks in the
  sender-designated group.
- Send-retry helper implementing M7 (yield + retry as a task state, no
  spinning inside a task).

**As built.** `sched/spsc_ring.hpp` (the ring), `sched/ring_transport.hpp`
(the seam plus `RealRingTransport`, the N² matrix),
`sched/sim_ring_transport.hpp` (M9's delay/reorder injection over a seeded
`SplitMix64` and the injected clock), `sched/ring_message.hpp` (the header
and the central kind enum — crosscore.md §3's six step kinds plus the four
system kinds, all declared, none sent yet), and `sched/send_retry.hpp` (M7).
Phase 3 in `Scheduler::RunOnce()` is no longer a comment. Four things the
work settled or found:

- **The two transports agree per *edge*, not per inbox.** The real one
  sweeps its peers in rotation so none starves; the simulation delivers by
  deadline. Two messages sent from *different* cores therefore arrive in an
  order the two do not share — and nothing above this layer may depend on
  that order, which is exactly what the reorder injection exists to prove.
  What both guarantee is per-edge send order and no invention, loss or
  duplication. The equivalence test asserts the real property rather than
  the tidier false one.
- **A handler runs inside a task, never in the drain.** Phase 3's job is to
  move messages off the ring; doing the work there would put an unbounded
  amount of it in a phase that has to stay bounded — the contract phase 1's
  io handlers are already under. The drain has its own loop budget
  (`max_messages_per_iteration`) for the same reason phase 4 has one.
- **The scheduling group travels in the message**, designated by the sender
  (sched.md §5), rather than being derived from the kind: the same kind can
  be foreground or maintenance work depending on what asked for it.
- **A message with no handler is dropped and logged, not fatal** — the same
  situation as a message whose tag matches no live pipeline state, which
  guideline 5 calls normal operation.

M7 is implemented in its plainest form — yield and retry, no backoff, no
ceiling, no deadline — because sched.md §10 leaves the retry protocol
`[OPEN]` and each of those would settle it. They belong with crosscore.md
§4's credit accounting, which is what actually bounds per-request buffering.

**Not built, and named so it is not assumed:** nothing constructs a
transport in production yet. `Expeditor` still builds one `Scheduler` and
never calls `AttachTransport()`, so at `cores = 1` the phase costs one null
test and the whole layer is exercised only by tests. Wiring it is P2's, with
the reactors it connects.

### P2 — Multi-reactor fan-out
- Expeditor spawns `cores` pinned workers; each owns a full per-core stack:
  Scheduler, BufferPool, WalManager + FileLogDevice(core_id) + segment
  naming `(core_id, segment_no)`, checkpointer. The existing single-core
  wiring becomes the per-core wiring, instantiated N times (page.md §6:
  "multi-core adds instances, not synchronization").
- Core 0 additionally hosts the system services (M5): extent lease service
  over messages, superblock anchor writer, catalog page ownership.
- Buffer-pool discipline: a core faults only pages it owns; an
  ownership-violation assert in the frame-load path (debug builds) enforces
  shared-nothing mechanically.
- Recovery: per-core parallel replay (wal.md §15) now over N streams;
  anchors read per core from the superblock.

### P3 — Sessions and accept
- SO_REUSEPORT listener per core (M3); connection, session state,
  statements, portals, and open transaction live on the accepting core.
- Session context object threaded through dispatch (shared prerequisite
  with txn workplan T5 — build once).

### P4 — Cross-core read execution
- **Prerequisite, settled 2026-08-04:** build the **KWP D5 row encoder**
  first, in `wire/`. CC2 requires `STEP_BATCH` payloads in that encoding and
  it does not exist — `include/kds/wire/kwp.hpp` is the frame codec alone.
  An interim private batch format is refused: it would be the second row
  format CC2 forbids, and the hardest kind to remove, because a whole
  pipeline would be written against it.
- Implement `docs/crosscore.md` in full: pipeline table per core,
  STEP_OPEN/BATCH/EOF/CREDIT/CANCEL/ERROR handling, KWP batch
  encoder reuse, credit accounting, teardown-by-tag.
- Statement planner on the session core resolves owner cores from the
  catalog cache and picks fast path vs pipeline (crosscore.md §2).
- DML statement shipping: route a write statement whole to the owner core;
  home-core binding + retryable rejection for second-core writes
  (crosscore.md §6) with the observability counter.

### P5 — Id lease services
- trx-id block leases from the system core (M4), replacing the dispatcher's
  in-memory `next_txn_id_` and aligning with txn workplan T3 (superblock
  counter, crash burns the remainder).
- Extent leases for file growth (M5): per-core prealloc batching that
  page.md §7 deferred lands here; extent size stays the existing
  `[OPEN: size]` (64 pages) until measured.

### P6 — Catalog cache and DDL propagation
- Per-core catalog cache over core-0-owned catalog pages; version stamp per
  relation row.
- DDL executes on core 0, then broadcasts invalidation; remote stale-oid
  access surfaces as a retryable step/statement error (crosscore.md §5).
  The existing "instance-scoped coherency" caveat becomes core-scoped and
  is re-documented.

### P7 — Observability
- Per-core: everything wal.md §16 lists, now labeled by core.
- Cross-core: shipped batches/bytes per (edge, relation), credit-stall
  time, pipeline count, cancel/error counts, rejected cross-core writes by
  (home, target, relation) — the placement/2PC input (crosscore.md §6).

### P8 — Deterministic multicore simulation and tests
- Harness: N reactors stepped round-robin on one thread over the simulated
  transport; delay/reorder/drop injection; seed-stable (sched.md §6 rules:
  deterministic containers, sequential ids).
- Run at `cores ∈ {1, 2, 4}`: the full existing suite (storage, wal
  crash-recovery, txn.md §10) plus crosscore.md §8 tests 1–8.
- CI: single-core remains the default matrix entry; multicore sim runs are
  additive.

## 3. Guidelines (invariants for every phase)

1. No shared engine state between cores; the ring seam is the only
   cross-core channel. No atomics outside ring indices.
2. The single-core build path must not regress: with `cores = 1` the ring
   layer, pipeline layer, and lease services contribute zero messages and
   zero allocations on the hot path.
3. LSNs are stream-local and are never compared across cores. Nothing in
   this milestone may create a cross-stream ordering dependency — that is
   what keeps recovery per-core and the 2PC door safely closed.
4. Owner-core resolution comes from the catalog only; no code derives
   ownership from page ids, hashes, or topology.
5. All cross-core messages are POD, tagged, and processable after the
   originating request is gone (discard-by-tag is normal operation).
6. Every new mechanism lands with its simulated-transport test in the same
   change; a cross-core code path without a deterministic test does not
   merge.
