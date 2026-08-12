# Known Gaps

The engine-wide list of what is missing, what does not survive a restart,
and what the code does differently from what a spec or older doc claims.
Verified against code 2026-08-10; the "Storage and key modes" section and
the `ORDER BY <pk>` entry added and then closed on 2026-08-11 with the
`EXPLICIT` key mode, and the pagination entry closed the same day by the
output sort. Each
entry names the owning doc — the full argument and any workplan live
there, not here. Manuals link here instead of carrying their own copies.

Scope note: an entry here is a *known, accepted* state, usually with a
named owner. It is not a bug list; a gap whose fix is decided belongs in
the owner's workplan.

## Durability and recovery

- ~~**WAL recovery is not implemented.** The log is written and never read
  back~~ — **recovery runs at mount as of 2026-08-12** (`RV1`,
  `docs/workplan-wal-recovery.md`, `include/kds/server/mount_recovery.hpp`).
  RC01-RC06 were built earlier and **nothing called them**: `RecoverCore`
  was reachable only from `tests/wal_recovery_test.cpp`, so every crash
  still recovered nothing. `Expeditor::Open` and `CoreRuntime::Open` now run
  analysis → redo → the high-water repair → undo against their own core's
  stream before the listener binds, and `SimInstance::Boot` does the same, so
  SIM04's crash contract is armed rather than counted
  (`sim/loop.hpp`'s `kRecoveryImplemented`, RC10's first half).
  A mount ends by publishing an anchor past everything it replayed (RC08, built
  the same day), so the next crash replays only what followed rather than
  rescanning the stream — **except on a peer core**, which cannot write page 0
  and so still scans from whatever anchor core 0 last wrote it (costless today:
  a peer holds no transaction ids, so its stream carries no writes of its own).
  `SHOW META` reports what the last mount's recovery did — records scanned,
  transactions committed and rolled back, per-phase timings, and the audit below
  (RC09, built the same day).
  **What is still missing:** RC07 (Bound Cabin replay, so a restart resumes
  assertion enforcement), which is what keeps this entry struck rather than
  deleted. The three findings below are what running recovery *produced* — two
  defects in code that predates it, both fixed, and one measured cost.

- **The catalog is still not recovered, and `catalog_recovered=0` says so on
  every `SHOW META`** (RV3, RC09). A crash can still lose a `CREATE TABLE`, so
  recovery's promise is *"every acknowledged commit to a relation that survived
  is restored"* and never "nothing was lost". Half of that gap is now counted:
  `recovery_relations_missing_pages` reports user relations the catalog still
  describes whose descriptor or var-heap root page the crash took, in
  O(relations). **The other half cannot be counted at all** — rows whose
  relation the catalog lost — because resolving a page to its relation needs a
  page→relation index that `page.md` does not have, and whose absence is
  already the named blocker on page reuse
  (`docs/feat-physical-optimizer.md` §6 gate 3). Building the set instead would
  mean walking every page of every relation at every mount.

- **A mount reads each WAL segment's whole body, twice** — measured 2026-08-12
  by RC09's own timings, which is what made a long-invisible cost visible.
  `ScanLog` allocates and reads a segment's entire body in one go, analysis and
  redo each run their own scan, and the default segment is 64 MiB: so a mount
  reads 128 MiB and allocates two 64 MiB buffers **before it can serve a
  statement, on a log holding nothing**. On a real server that is
  `recovery_analysis_us≈44000` + `recovery_redo_us≈42000` — 86 ms of a ~90 ms
  mount, reproduced on three consecutive mounts. Not a defect and not new
  (`log_scanner.cpp` anticipated it: "when segments are 64 MiB this becomes a
  streaming read"), and not recovery's to fix alone: the fix is to read only as
  far as the durable end, or to stream in chunks, and it belongs beside the
  segment-size decision that is still `[OPEN]` (`docs/wal.md` §15).

- ~~**Var-heap page growth and UPDATE's spills are not logged, and recovery
  found it**~~ — **fixed 2026-08-12**, all three holes, with the reproducer
  now a test rather than a seed. `varheap::ChainAppend` returns a
  `ChainAppendResult` naming the page it created and the tail it linked;
  `CommandDispatcher::LogSpills` logs a `kVarHeap` `PAGE_INIT`, a full page
  image of the linked tail, then the `VARHEAP_APPEND` — and **UPDATE now calls
  it**, its `VarHeapSink` having previously carried no collector at all.
  Pinned by `InsertWalTest.GrowingTheVarHeapChainLogsTheNewPageAndTheLinkThatReachesIt`
  and `InsertWalTest.AnUpdateThatSpillsLogsTheValueItSpilled`. What was
  wrong, kept because the shape recurs:

  1. `varheap::ChainAppend` grows a chain with `store.CreateNew()` +
     `FormatPage()` and logs **no `PAGE_INIT`** for the new page — while the
     heap and btree paths log one for every page they create.
  2. The **chain link edit** on the old tail is unlogged, so a replay can
     leave a value page that exists and is unreachable.
  3. **An UPDATE's spills are not logged at all**: its `VarHeapSink` is
     built with no `appended` collector, so no `VARHEAP_APPEND` is ever
     written for a value an UPDATE spilled. The INSERT path collects and
     logs; the UPDATE path does not.

  It was reachable, and loud rather than silent for (1): a crash losing a new
  var-heap page's write-back left a durable `VARHEAP_APPEND` naming a page no
  `PAGE_INIT` creates, and redo **refused the mount** — reproduced at
  `ckdbs-sim --seed 7 --ops 3000 --mode crash --iterations 3`.
  `wal::ApplyPageInit` already formatted a `kVarHeap` page (RC03 anticipated
  it), so what was missing was the record nobody wrote and never an applier.

- **A segment sealed with no room for a PAD was read as a torn tail** — found
  and **fixed 2026-08-12** (`src/wal/log_scanner.cpp`), and it is the second
  defect recovery exposed rather than introduced. `WalStream::Seal` writes its
  marker only when the tail can hold a record header; a shorter tail is left as
  the zeroes the segment was created with, and `stream.cpp`'s comment claimed a
  reader would take that to "mean exactly what the marker means". `ScanLog` did
  not: it stopped there, so **every record in every later segment was silently
  dropped** and recovery restored a truncated stream while reporting success.
  Visible as acknowledged rows missing after a restart, once a run was long
  enough to roll a segment. The fix tells a seal from a tear by the same
  `kRecordHeaderSize` bound the writer decides with. `WalStream::ScanTail` was
  never affected — it only ever reads the last segment.

  **Both defects hid behind a green suite for the same reason**: the committed
  seed corpus runs at 1500 ops, which neither rolls a 1 MiB segment nor fills a
  var-heap page. `SimLoop.ALongRunRollsASegmentAndStillRecoversEveryAcknowledgedRow`
  now runs seed 24 at 3500 ops for exactly those two boundaries, and
  `LogScannerTest.ASegmentSealedWithNoRoomForAPadStillContinuesIntoTheNext`
  lands the tail on 24 bytes deliberately — the existing boundary test used
  3000-byte payloads, which always leave room for a marker.
- ~~**MVCC ships before recovery** (`docs/txn.md` §8): an uncommitted row
  surviving a crash reads as **committed** on the next boot~~ — **closed for
  the mount path 2026-08-12.** Undo now runs before the listener binds, so a
  loser's rows are rolled back rather than published, and `RecoverCore`
  refuses the mount outright if it cannot do that (RV1). The gap's *shape*
  survives only where recovery is bypassed: `SimInstanceOptions::skip_recovery`
  is the harness's fault injection and boots into exactly the old behaviour,
  which is how the durability assertion is proved able to fail
  (`tests/sim_loop_test.cpp`). `docs/txn.md` §8 needs amending at the source
  (RC10).
- **DDL and catalog writes are unlogged**, and DDL is not transactional
  (`docs/txn.md` §7): `CREATE TABLE` inside a transaction is not rolled
  back.
- **Keystone K1 does not hold across a crash**
  (`docs/keystoneid-k0-findings.md`): the durable log names ids the
  unlogged `sys.tables.next_id` has forgotten. K-M2a/K-M2 own it.
- **The assertion checkpoint-genesis gap** (`docs/feat-assertion.md` §7):
  the group-directory fold needs records from the Bound Cabin's birth, and
  nothing durable holds headers for a checkpoint-bounded replay to start
  from. **Decided 2026-08-11 and now owned** — AS6a gives the checkpoint a
  headers-only directory snapshot and the entry a `group_id`, so replay
  folds from the last checkpoint; `docs/workplan-wal-recovery.md` RC07
  builds it. The gap stays listed until RC07 ships: today a restart still
  loses every group directory and enforcement does not resume.

## What a restart loses (without a crash)

- **Cabin entry sets** are memory-resident by design
  (`docs/feat-cabin.md` §9): the `sys.cabins` row survives, the sets
  re-observe from traffic.
- **Assertion enforcement**: the registry/directory is memory-resident, so
  a surviving assertion honestly reports `enforcing=0` until recovery can
  replay the directory (`docs/feat-assertion.md`). The durable Bound Cabin
  pages and the catalog row survive. **Partly closed 2026-08-12**: AS6a's two
  persisted formats landed (`BoundCabinEntry::group_id` in AST04's padding word,
  `AssertEntryPayload::group_id`) together with the directory primitives replay
  needs — dense per-cabin ids, `SnapshotGroups`, `RestoreGroup`, `AttachEntry`,
  and `AdoptGroupId`, which refuses to let a fold's ids drift from the ids the
  entries on the pages already carry. What is still missing is the plumbing:
  nothing writes a snapshot at a checkpoint and nothing loads one at a mount
  (`docs/workplan-wal-recovery.md` RC07 parts 3-4), so the reported state is
  unchanged.
- **Waystone sighting counts** restart (a performance event, never a
  correctness one — invariant 8).

## Reclamation — nothing purges, anywhere

There is no purge pass, and readers are deliberately unregistered
(`docs/txn.md` §9), so:

- undo pages grow monotonically; `SnapshotTooOld` is structurally
  unreachable;
- delete-marked tuples keep their slots; var-heap bytes of superseded
  values stay; superseded index and Cabin entries stay
  (`docs/feat-index.md` §13);
- catalog rows are never reclaimed (the column ceiling is on columns ever
  created); pages, extents and Keystone ids are never reused;
- `DROP TABLE` exists (`docs/spec-drop-table.md`) but is **catalog-scoped**:
  the relation's pages, var-heap chain and index pages orphan — leaked
  space, deliberately, because free-map reuse is gated (a reallocated page
  breaks trail validation, `feat-physical-optimizer.md` §6 gate 3) and no
  reader horizon exists. The oid is tombstoned in `sys.objects` and never
  reissued, which is what keeps dead-oid advisory structures harmless.
  `ALTER TABLE` is catalog-only renames (`docs/spec-alter.md` AL1). Both
  RESTRICT on assertions; DROP also RESTRICTs on referencing foreign keys.
  Every one of these is an unlogged catalog write like all DDL: a crash
  after it can lose it.

## Concurrency and multicore

- **Cross-core execution has started, and is exactly one shape wide.**
  P4a-P4c are built (2026-08-10, `docs/workplan-crosscore.md`): a
  single-relation remote read — a star `SELECT` against an `owner_core=1`
  relation — is opened as a step on its owning core, streamed back under
  credit, and framed by the session core, with the reply byte-identical to
  the local path. **Every other statement is still served by core 0**:
  `CheckReadAffinity` now refuses the shapes the pipeline cannot yet run,
  retryably, rather than all of them. What remains is **P4d** — multi-step
  wiring, join-key forwarding, and the viral `ChainRunner` coroutine
  conversion, the workplan's largest single change — and **P4e**, the
  equivalence pass plus the benchmark re-run. Until P4e runs,
  `bench/results-multicore.md`'s 1.05× is a *parity baseline*, not a
  measurement of the pipeline.
- Relation ownership is decided **and built** (CC7 + P6b handoff + P6c
  `placement` key, 2026-08-10): a rotated relation's pages are grantable
  and readable by its owner. `placement` still defaults to `creating` —
  `rotate` places relations on cores that can serve one statement shape and
  must refuse the rest, so it stays an exercise mode until P4d lands.
  Row-id leasing for peer INSERT is also built (P5-shape, 2026-08-10).
- **REPEATABLE READ is knowingly weakened across cores** (CC4): no
  cross-core ReadView; RR holds per core. Client-facing docs must say so.
- Cross-core writes are refused retryably (CC3): a transaction's writes
  bind to one home core. 2PC is an open decision, to be designed from the
  refusal counters.
- **Buffer-pool eviction is built but disarmed**: nothing calls the sweep,
  because `Get()` hands out raw spans safe only while nothing evicts — the
  `PageRef` migration (~257 call sites) is a hard prerequisite
  (`docs/spec-eviction.md`, `docs/page.md` §3).

## Storage and key modes

- ~~**Dividing a full btree *internal* node is not implemented**~~ —
  **built 2026-08-11** (`docs/workplan-key-mode.md` PK09). A separator
  promoted into a full parent now divides that node's entries when it sorts
  inside them: the median moves up, its child becomes the new node's
  leftmost, and the lower half is written back. The cheap
  right-split-with-no-movement is kept for the append case it correctly
  serves. Struck rather than deleted because the refusal it replaced was a
  named `OutOfSpace` some reader may still be holding.
- **A heap relation cannot be `EXPLICIT`**, refused at
  `Catalog::CreateTable` and at the statement layer. Not a defect: a heap
  chain grows only at its tail and has no descent to prove a supplied key
  unused. Lifting it is the heap page split policy
  (`docs/heap-and-tuple.md` §3.1b), which stays open.
- **A `DELETE`d row's primary key cannot be re-supplied** on an
  `EXPLICIT` relation. The uniqueness check scans the landing leaf's live
  slots, and a delete-marked slot is live until retirement — and nothing
  retires (see reclamation above). Consistent with K1 issue-once, and a
  restriction a caller doing delete-then-reinsert will meet.

## SQL surface and protocol

- **No NULL storage**: `NULL` parses as a literal; rows holding one are
  not storable today (`docs/client-manual.md`).
- ~~**Pagination is LIMIT/OFFSET only**~~ — **closed 2026-08-11** by the
  output sort (`docs/workplan-order-by.md`). `ORDER BY` now takes any
  column or columns, pk or not, of any relation in a non-aggregated
  top-level statement, each `ASC` or `DESC`. What remains true of that
  entry: **there are no cursors**, and KWP/1 portal suspension is still
  unbuilt — only the frame codec exists (`docs/protocol.md`).
- **A sorted statement's `LIMIT` bounds output and memory, not work.** The
  sort is blocking, so the walk cannot stop when the quota fills the way it
  does on an unsorted or pk-elided statement; the row-touch budget is what
  bounds work. Visible as ANALYZE's `examined=` being the unlimited
  statement's. Not a defect — the alternative is a wrong answer — but it is
  the one performance property a client migrating from `LIMIT` alone will
  notice.
- **A sort refuses past `sort_max_rows`; it does not spill.** No temp-file
  story exists, so an unlimited `ORDER BY` over a relation larger than the
  cap fails the statement naming the key. Under a `LIMIT` the top-N heap
  holds `offset + limit` rows, so the cap binds only the unlimited case.
- **An index still does not serve an `ORDER BY`**, and this is a finding
  rather than a gap: `docs/workplan-order-by.md` records the four reasons
  (IX8a's deliberate re-sort to pk order, append-only maintenance picking a
  stale key at dedup, 32-byte string truncation making index order a prefix
  order, and no cardinality estimate to avoid IX9's crossover).
- ~~**`ORDER BY <pk>` no longer means key order on an `EXPLICIT`
  relation**~~ — **closed 2026-08-11.** The clause used to be validated and
  discarded, on the claim that "pk order is the order the chain already
  emits": true while every id was appended in ascending order, and false
  once a caller names them. The fix is a **per-page emission order**, not an
  output sort, because the disorder was bounded by one page — ordering
  *across* pages was never at risk, since a leaf division preserves
  page-wise `min_key` ordering. `Step::emit_in_key_order` is set only when
  the statement asked for pk order *and* the relation is `EXPLICIT`; the
  walk is untouched everywhere else. Covered by emission-order tests,
  including under `LIMIT`/`OFFSET`.
- **`IN (value list)`** is unbuilt — the open half of parser workplan V08;
  it currently reports "expected a subquery".
- **Per-transaction durability class** is a KWP/1 protocol field; the text
  protocol offers only the instance-wide `durability` config key.
- **No auth, no TLS, loopback only** — by design until KWP/1's handshake
  and auth stages exist.
- **`float`** stays refused at `CREATE TABLE`: nothing settled its
  encoding (`docs/rule-fixed-length-tuple.md`).

## Advisory and optimizer structures

- **Waystone retention, decay and epoch-bump sites are unbuilt**
  (P15-P17, `docs/waystone-workplan.md`); trails grow until then.
  One validation gap remains: nothing verifies a page still belongs to the
  relation a trail recorded it from — holds until pages can be reallocated
  between relations (`docs/feat-physical-optimizer.md` §6 gate 3 owns it).
- **`CABIN AUTO` acts only under `cabin_optimizer = on`, default `off`**:
  the controller runs end to end since PHY04 and is observable since
  PHY06 (`SHOW CABIN_OPTIMIZER`, both 2026-08-10), but with the key at
  its default a column declared `auto` still behaves exactly as an
  undeclared one (`docs/feat-physical-optimizer.md` Part II). Its managed
  state and decision log are memory-resident: a restart forgets what the
  controller was managing, and re-observation rebuilds it — the stated
  crash posture, not a bug.
- **The physical optimizer is shadow-only as a finding**
  (`docs/feat-physical-optimizer.md` §6): every candidate move is blocked
  by a named gate; `physical_optimizer = on` is refused at startup naming
  all three.

## Recovery work landed uncompiled at RC06 — closed 2026-08-11

**`main` did not build at `393b5a4`**, and had not since RC06 (`c09353e`,
"the per-transaction undo chain, and a durable insert record (RV10)"). Found
while building the output sort, which could not be verified until the tree
compiled.

**Closed upstream, not here.** `c1370e8` made main build again and
`b11cc81` fixed the eleven recovery failures that became visible once it
did — two engine bugs and two wrong tests — and `28ee297` added the push
guard that refuses a commit which does not build and pass. The output-sort
branch had made its own unblocking repairs to the same files; they were
resolved away in favour of the upstream ones, which go further.

Kept as a record because the *cause* was a process gap rather than a code
one — a commit that was never compiled cannot have been tested either, and
what it hid was two real engine bugs, not just stale literals. `28ee297` is
the fix for the cause; the entry below is what it was fixing.

What was broken, all of it stale-by-one-commit rather than wrong by design:

- `include/kds/txn/undo_page.hpp`: two `static_assert`s compared `offsetof`
  on RV10's appended `txn_prev_undo_ptr` / `pk` against the **serialized**
  offsets 28 and 36. The record is unpadded by design and the encoder
  memcpy's through those constants correctly, but the C++ struct aligns its
  u64 tail to 32 and 40 — so the asserts compared a wire offset with a
  layout and could never hold. Dropped, with the reason written in place;
  the format is unchanged and the offsets below the first aligned u64 are
  still asserted. `kMaxUndoImageLen == 8108` was RV10-stale too (the header
  grew 28 → 44), now 8092, and the "~7 bytes" margin it documents is ~23.
- `src/wal/redo.cpp`: a default-constructed `std::span<std::byte,
  kPageSize>`, which a fixed-extent span has no constructor for.
- `src/server/command_dispatcher.cpp`: two unqualified `kNoTrxId`, and one
  `return {msg, false}` in a function returning `std::optional<std::string>`.
- Five `tests/wal_*` fixtures still constructing `MemoryLogDevice`
  directly after its constructor went private behind `Create`. Converted to
  the `SetUp` + `unique_ptr` shape `wal_stream_test` and `wal_manager_test`
  already use.

**15 tests failed once the tree compiled** — `UndoPageTest` ×2 and
`UndoLogTest` ×2 pinning pre-RV10 sizes, `LogScannerTest` ×2, `RedoTest` ×1
and `RecoveryUndoTest` ×8 on behaviour. None was in the output sort's path.
All fixed by `b11cc81`; the suite is green.

## Stale claims found in docs (fix at the source when touched)

- `docs/client-manual.md` §5: "exactly one accepted client connection
  served at a time" — stale; many clients are served concurrently,
  cooperatively on one thread (`include/kds/server/tcp_server.hpp`).
- Any doc or task brief claiming **there is no SQL DELETE** or that
  **assertions enforce nothing** predates the transaction work and AST07
  respectively; both are built (verified in
  `src/server/command_dispatcher.cpp`).
