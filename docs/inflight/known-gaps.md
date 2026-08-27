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
  rescanning the stream. ~~**except on a peer core**, which cannot write page 0
  and so still scans from whatever anchor core 0 last wrote it (costless today:
  a peer holds no transaction ids, so its stream carries no writes of its
  own)~~ — **a peer checkpoints as of 2026-08-21** (PW3,
  `docs/inflight/in-progress/workplan-peer-writer.md`): it still cannot write page 0, so it sends
  the anchor and core 0 writes it (`remote_checkpoint_anchor.hpp`), at mount
  and on the `§11` cadence. The parenthetical was retracted a day earlier by
  PW1, which is what made the gap cost anything. ~~**One of core 0's three
  checkpoint points is still missing on a peer**: the *shutdown* one. A peer's
  teardown syncs its WAL and destroys its runtime before core 0 checkpoints,
  so the "after a `STOP`, the next mount reads 2 records where it read 1205"
  property below holds for core 0 and not for a peer — a graceful restart
  replays up to one `checkpoint_interval_ms` of a peer's stream, every time.
  PW3b owns it, and it is a sequencing decision rather than a missing call:
  at the only moment the runtime is safely reachable again (after the worker
  join) both reactors are stopped, so a queued anchor send would never be
  polled.~~ — **the third point landed 2026-08-25** (PW3b, that workplan's
  §6): `Serve`'s tail runs `CoreRuntime::ShutdownCheckpoint` per peer after
  its final sync, and the anchor goes **direct** through core 0's
  `SuperBlockCheckpointAnchor` on the startup thread — which owns every core
  once the workers are joined — rather than over a ring nothing polls
  (`remote_checkpoint_anchor.hpp`'s last section carries why the hand-pumped
  drain was rejected). A peer's `SHOW META` now carries the recovery block
  below, so the bound is checkable there. Measured on a two-core server
  (200 rows on a core-1-owned relation, `STOP`, restart): the peer's mount
  reads **2 records and redoes 0**, against **810 and 404** from the same
  binary with the call disabled — core 0 reads 2 either way.
  `SHOW META` reports what the last mount's recovery did — records scanned,
  transactions committed and rolled back, per-phase timings, and the audit below
  (RC09, built the same day).
  **RC07 closed the same day**, so every task in the series is built. What keeps
  this entry struck rather than deleted is what v1 still does not promise (§6):
  the catalog is not recovered (RV3, below), nothing is purged, and `D3`'s window
  is bounded rather than zero. The findings below are what running recovery
  *produced* — three defects in code that predates it, all fixed, and one
  measured cost.

- ~~**The catalog is still not recovered**~~ — **closed 2026-08-19**
  (RV3, `docs/workplan-rv3-catalog-recovery.md`): catalog mutations log
  the ordinary record types, every DDL statement runs under a real
  transaction with undo records a crash loser's mount rolls back
  through, and `SHOW META` prints `catalog_recovered=1 ddl_durable=1`.
  Recovery's promise widens to what RC09 could never say: an
  acknowledged `CREATE TABLE` is restored like any acknowledged commit.
  ~~The two row-codec definition relations stayed outside the log~~ —
  **closed 2026-08-19, the same day it was named**: `sys.pattern_defs`
  and `sys.assertions` write through `exec/wal_row_log.hpp` now, and an
  acknowledged `CREATE ASSERTION` survives a crash and **enforces**.
  Proving it exposed two pre-existing holes, both closed: redo
  mis-formatted `kCabinBound` bodies, and every transactionless DDL
  statement (pattern, assertion, cabin, ALTER) had no commit record for
  the durability class to ride - `kStrict` **and `kGroup`, the default,
  whose documented point is D1's**, now sync at the acknowledgement.
  What stays outside the log: ALLOC/FREE and the advisory Waystone
  classes invariant 8 exempts (`wal.md` §11a). One
  contract also got *stricter*: a torn catalog page used to boot and be
  served corrupt; redo now names it, cannot heal it (§10's FPI cadence
  is unbuilt for every page class), and refuses the mount — the rule
  torn heap pages already lived under. The paragraphs below stand as
  the record of the gap while it existed. Half of it was counted:
  `recovery_relations_missing_pages` reports user relations the catalog still
  describes whose descriptor or var-heap root page the crash took, in
  O(relations). **The other half cannot be counted at all** — rows whose
  relation the catalog lost — because resolving a page to its relation needs a
  page→relation index that `page.md` does not have, and whose absence is
  already the named blocker on page reuse
  (`docs/spec/physical-optimizer.md` §6 gate 3). Building the set instead would
  mean walking every page of every relation at every mount. **Substrate built
  2026-08-13**: `docs/spec/page.md` §2a stamps the owning object's oid into the
  common header of every page created from that build on, which makes this
  census one sequential file scan needing no catalog — pages of a lost
  relation keep their attribution on the page itself. The census *scan*
  is not written, and pre-§2a pages read owner 0 forever (no backfill, by
  §2a's decision), so the half stays uncounted and this entry stands until
  the scan exists.

- **A recovered Bound Cabin's entry list is a superset of the live one, and
  `VerifyAgainstEntries` cannot be run on it** — found by review 2026-08-12,
  **half fixed**. Two independent causes:

  1. ~~the page walk and the `ASSERT_*` fold both attach the linkage for an entry
     written after the checkpoint into a group that existed at it~~ — **fixed**:
     `BoundCabin::DedupeEntryLinkage` reconciles them once at the end of a
     rebuild, and `AssertionRecoveryResult::duplicate_links_dropped` reports how
     many overlapped. A slot holds one entry, so a repeated `(page_id, index)` is
     always the duplicate.
  2. ~~**an aborted reservation's orphaned entry cannot be told from a live one.**
     `AssertionEnforcer::AbortTxn` leaves the entry bytes on the page by design —
     "the orphaned slot is the recorded leak that rides on purge" — and the
     rebuild has no way to distinguish it, so any cabin whose history includes a
     pre-checkpoint abort relinks an entry the live directory had dropped~~ —
     **decided and fixed 2026-08-12** as AS6b (`docs/spec/assertion.md` §7).
     `flags` bit 3, `kEntryOrphaned`, is set on abort by the live path and by
     `ASSERT_ROLLBACK` replay alike, and the linkage scan skips a marked entry.
     Bit 3 was free — AST04 shipped three flags — so no width moved and an older
     entry reads as "not aborted", which is what it is.

  The **aggregate was correct either way** (snapshot + folded deltas), so
  admission answered right and the constraint enforced correctly. What did not
  hold is the structural proof `docs/spec/assertion.md` §5.2 names — "the entries
  remain the authority, the snapshot is a derived cache, and
  `VerifyAgainstEntries` is what proves one against the other" — because on a
  recovered cabin that check reported `Corruption` for a directory that was
  right, i.e. the one check that catches a real divergence was disabled exactly
  after a restart.

  **It was an AS6a decision rather than a bug to pick a fix for**, and the two
  rejected options are why: letting the fold own linkage and stopping the page
  walk from attaching costs AS6a's `Unapply` ordering note — a reservation made
  before a checkpoint and rolled back after it would have no entry to remove, and
  the mount would fail — and narrowing §5.2 to live cabins only gives up the
  proof at the one moment it earns its keep. The cost taken instead: abort
  becomes a page write (one read-modify-write plus a `StampPageLsn` per aborted
  reservation).
  `AssertionRecoverTest.AnAbortBeforeTheCheckpointLeavesNoEntryForTheWalkToRelink`
  pins it and was verified to fail without the skip.

  **This entry first said that page write "is what commit was already paying to
  clear `kEntryReserved`". Measurement 2026-08-13 says otherwise**
  (`bench/results-assertion-abort.md` at `2199780`): `CommitTxn` batches by
  `(assertion, page)` and `AbortTxn` does not, so abort's per-reservation cost
  is flat where commit's falls as 1/K — 5.6 µs against 1.7 µs to settle 16
  reservations. The asymmetry **predates** this change and was widened by it,
  not created; AS6b's own share is 0.056 µs per reservation, below the noise
  floor until K=8. See the open item below.

- **Aborting a transaction's assertion reservations costs per reservation where
  committing them costs per page** — measured 2026-08-13,
  `bench/results-assertion-abort.md`. `CommitTxn` groups its pending
  reservations by `(assertion, page)` and pays one page fetch, one `Open`, one
  WAL record and one `StampPageLsn` per group; `AbortTxn` walks reservations one
  at a time and pays all four per reservation. `BoundCabinChainWriter::Append`
  always appends at the tail, so a transaction's K entries share one page
  whatever their `GROUP BY` values — the batching premise is exact, not
  incidental. Per-reservation protocol cost is flat for abort (0.200 µs at K=1,
  0.350 at K=16) and a 1/K curve for commit (0.500 at K=1, 0.106 at K=16), so
  settling 16 reservations costs 5.6 µs to abort against 1.7 µs to commit, and
  15.2 against 4.6 at K=32.

  Pre-existing — the base binary already paid an `Unapply` and a WAL `Append`
  per reservation — and AS6b's page write widened it by 0.056 µs per
  reservation, which does not clear the noise floor until K=8.

  **The blocker is a record format, and it is cheap exactly now.** The page
  write is already one named method (`BoundCabinPage::MarkOrphaned`); what
  cannot be batched is `ASSERT_ROLLBACK`, which carries one group key per record
  where `ASSERT_COMMIT` takes a repeated-index list. Batching abort means moving
  that payload — a `docs/spec/wal.md` §4.1 decision, and one that would ride the
  segment-format bump to 2 for free rather than costing a version event of its
  own later. Owned by `docs/spec/assertion.md` §7.

- **`SHOW META` under-reports an assertion-carrying mount by up to 29 ms**,
  because `exec::RecoverAssertions`' `ScanLog` is timed into no phase counter
  and lands entirely in the residual — measured 2026-08-13,
  `bench/results-assertion-abort.md`. Declaring an assertion adds a cost that
  *falls* as entries rise and tracks `recovery_analysis_us` within 11% across a
  2.3× range (+30.3 ms against 29.7 at 200 rows, +13.8 against 12.9 at 10k),
  which is the signature of a fourth full segment scan rather than of work
  proportional to the entries. It also means the scan-narrowing item below is
  worth about a third more than it is credited with there.

- **Assertion recovery is a third full `ScanLog` per mount whenever the anchor is
  zero** — noted by review 2026-08-12. `exec::RecoverAssertions` scans from the
  anchor's `checkpoint_lsn`, which is *narrower* than redo's range only once a
  checkpoint has been published; on a database that has never completed one it is
  the whole stream. Combined with the two-scan entry below, that is three passes
  and 192 MiB of reads on a default 64 MiB segment before the first statement.
  Exactly zero when no assertion is declared (the pass early-returns), and RC08
  makes the zero-anchor case a first-mount-only state. Same fix as below: read to
  the durable end, or stream in chunks.

- **A mount reads each WAL segment's whole body, once per scan, and there are
  three scans** — measured 2026-08-12 and **partly closed the same day**;
  `bench/results-wal-recovery.md` carries the numbers and the method.
  `ScanLog` reads from the anchor to the *segment's* end, so the cost tracks
  segment bytes and not record count: 0.63 ms/MiB, constant to 4% across 64.0 /
  63.6 / 56.9 / 28.1 MiB while the record count stayed at 2. **An empty log is
  therefore the worst case, which is the opposite of the intuition**, and
  `kDefaultSegmentSize` has quietly become a startup-latency knob.

  Three scans, not two: `WalStream::ScanTail` at WAL open, then analysis, then
  redo — and a fourth whenever an assertion is declared
  (`exec::RecoverAssertions`, which scans the whole stream while no anchor
  exists).

  **Closed half of it**: the scan buffer is no longer value-initialised
  (`make_unique_for_overwrite`, so `ReadAt` writes each byte exactly once), which
  measured **−24.7 to −26.0 ms per mount** in an interleaved A/B — 137 ms → 112
  ms, ~18%, at −8 ms per scan.

  **Still open, and this is the corrected number**: a mount is **112 ms** where
  the pre-recovery engine was **49.5 ms**, and ~65 ms of it is the reads
  themselves. An earlier version of this entry said "86 ms of a ~90 ms mount";
  the ~90 was wrong — a mount was 132-140 ms before the buffer fix and 49.5 ms
  before recovery existed at all, so recovery is ~64% of a mount and not ~96%.
  The remaining fix is a **narrower read** — to the durable end, or streamed in
  chunks — and it belongs beside the segment-size decision that is still
  `[OPEN]` (`docs/spec/wal.md` §15).

- **An INSERT-with-spill writes 1.8-2.0× the WAL bytes it used to, and that
  multiplies a 487 ms stall** — measured 2026-08-12,
  `bench/results-wal-recovery.md`. The `PAGE_INIT` + 8 KiB `FULL_PAGE_IMAGE` per
  var-heap chain growth is what the correctness fix above costs: 3764 B against
  2122 B per spilling INSERT at 1600-byte values, 16,886 against 8626 at 8100.
  **Per-statement latency did not regress** — the delta is inside a ~9 µs noise
  floor at 1600 B, and +2.7 µs (+3.5%) at the pathological size where every row
  grows the chain, with p0 identical on both sides.

  The cost that matters is indirect: `FileLogDevice::CreateSegment` takes
  **487 ms** on the statement thread (`posix_fallocate` + a 64 × 1 MiB prewrite +
  two fsyncs), so WAL volume decides how often a client waits for one. At 10k
  rows of 8100-byte values that moved from two segment creations to three.
  Pre-existing and unrelated to this change, but now amplified by it, and the
  reason a segment's creation cost belongs on someone's list.

  (An UPDATE-with-spill writes 10-46× more, and that number is the size of the
  hole that was there: at the previous commit it wrote 361 B, exactly what an
  *inline* update writes, because its value was not logged at all.)

- **One failed checkpoint disarms every later checkpoint on that core** —
  found by the PW3b review (2026-08-25), pre-existing and unfixed.
  `Checkpointer::in_progress_` is cleared only on `Complete()`'s success
  path (`src/wal/checkpointer.cpp:249`), while `Start()` refuses with
  `AlreadyExists` whenever it is set — so a checkpoint that fails at a
  flush, at the `CHECKPOINT_END` append, at `EnsureDurable` or at the anchor
  publish leaves the flag standing, and `RunToCompletion` (what both
  cadences and both shutdown paths call) fails at `Start()` from then on.
  The paced `Step()` retry the flag exists for has no caller: nothing
  resumes a half-finished checkpoint. One `Error` line early in a run
  therefore costs the whole run's bounded recovery, on core 0 and on every
  peer — and since PW3b it costs the graceful-restart bound as well, which
  is what makes it worth naming here. The fix is a behaviour decision that
  belongs to `wal.md` §11: reset on failure and lose the snapshot, or keep
  it and give the paced path a resumer.
- ~~**A clean shutdown publishes no anchor**~~ — **fixed 2026-08-12** for the
  graceful path: `Expeditor::Serve` now checkpoints on its way out, and
  `SimInstance::CleanShutdown` does the same so the harness stops the way the
  server does. Verified on a running server: after a `STOP`, the next mount reads
  **2 records where it read 1205**, with every row still present.

  **The order is the fix, not the call.** A checkpoint's redo start is
  `min(recLSN)` over the dirty table it snapshots at BEGIN (`wal.md` §11-3), so
  checkpointing *before* the final sync publishes an anchor pointing at the oldest
  still-dirty page — near the start of the log on any busy run. Written that way
  first, it changed 10,883 re-read records into 1205. Synced first, the dirty
  table is empty and the redo start is the `CHECKPOINT_BEGIN` LSN itself.

  **What it does not buy, measured**: mount wall time barely moves at this size
  (`recovery_analysis_us` ~34 ms either way), because the scan reads the whole
  segment body regardless of how many records are in it — the still-open entry
  above. What the anchor bounds is the *work*: records decoded, and redo actually
  applied where pages had not been flushed. It also makes the anchor honest, so
  the narrower-read fix pays off when it lands.

- ~~**A process-manager stop is not the graceful path — the server handles no
  signals at all**~~ — **fixed 2026-08-12.** `SIGTERM` and `SIGINT` are now
  blocked and delivered through a `signalfd` that `Expeditor::Serve` registers
  with its reactor (`include/kds/server/stop_signal.hpp`), so `systemctl stop`, a
  container stop and Ctrl-C all take the same path a client's `STOP` does —
  scheduler stop, worker join, final sync, shutdown checkpoint.

  A `signalfd` rather than a handler-plus-flag on purpose: a handler may do almost
  nothing safely, so the usual shape costs a polling interval and a second thing
  to get right, while this reactor already accepts arbitrary fds and turns the
  delivery into an ordinary readable event on the reactor's own thread.

  **The ordering is the part that would have failed intermittently.** The signals
  are blocked in `main` *before* `Expeditor::Open`, because Open starts the WAL
  writer thread and a signal goes to whichever thread does not block it — install
  it later and that thread still takes the default action and kills the process,
  sometimes. Blocking before the first thread means every thread inherits it.

  Verified end to end on a running server: 300 rows, `SIGTERM`, restart — the next
  mount reports **`recovery_records=2`** with all 300 rows, where the same signal
  through the same measurement harness previously left mount 1 re-reading **10,883
  records** and writing a 42 ms checkpoint. `SIGKILL` is unblockable and still an
  immediate kill, which is correct: that is the crash path, and recovery is what
  covers it.

- **`varheap::ChainAppend` walks the chain root-to-tail on every append**, with
  no tail cache, so a spilling INSERT is O(chain length) and unbounded — found
  2026-08-12 while measuring the var-heap write path, and **pre-existing**
  (identical on both sides of the change). Visible as p25 rising 71.7 µs → 107-135
  µs from 1k to 10k spilling rows while the inline control does not move. The
  heap chain solved this with a tail hint (`heap_tail_hint`); the var-heap has
  no equivalent.

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

- **`HEAP_DELETE_UNMARK` could not be written at all** — found and **fixed
  2026-08-12**, and it is the third defect recovery work exposed rather than
  introduced. RC05 added the type as 23 and left `kMaxAssignedRecordType` at 22,
  which is the bound `EncodeRecord` enforces — so every attempt to log one
  answered *"unassigned record type"*. `TransactionManager::Compensate` could not
  log the compensation for an aborted DELETE, and `txn::RecoveryUndo` could not
  either, so a mount that had to roll back a loser's DELETE **failed**. It hid
  because every test that covers those paths runs with `wal = nullptr`, where no
  record is written, and because a test asserted `IsAssignedRecordType(23) ==
  false` — agreeing with the stale bound instead of with the enum.

  The constant is now **derived from the last enumerator** rather than typed, so
  appending a type cannot leave it stale, and
  `WalRecordTest.EveryNamedTypeIsWritable` is the general guard: a type with a
  name is a type some site intends to write, so it must encode. Verified to fail
  against the old bound.

- ~~**`AssertEntryPayload`'s offsets moved with no format-version bump, and the
  argument that licensed it expired inside the same eight commits**~~ —
  **decided and fixed 2026-08-12**, `docs/spec/wal.md` §4.1. AS6a's licence was that
  both format touches are free *"today … no WAL stream has ever been read
  back"*, and `6d7b91b` in that very range is what makes streams get read back.
  `kAssertEntryFixedSize` went 16 → 20, so every byte after offset 16 shifted,
  while `kSegmentFormatVersion` was still 1.

  `kSegmentFormatVersion` is now **2**, and the bump alone would not have been
  the fix: `DecodeSegmentHeader` refuses only what is *newer* than the build, so
  a v1 segment would still have been accepted and mis-decoded. The refusal comes
  from a second constant, `kMinReadableSegmentFormatVersion`, raised alongside
  it — so a v1 stream is refused by name, naming both versions, because there is
  no migration and the operator's next step is to discard it. The floor tracks
  the current version only while no compatibility promise exists (pre-1.0); once
  one does, it stops tracking and a decoder per supported version replaces it,
  which is a decision to take then rather than a default to inherit now.
  `WalSegmentTest.AStreamOlderThanTheRecordLayoutIsRefusedNotMisparsed` pins it.

  The bump covers RC03's `UNDO_WRITE` correction too, which moved under the same
  argument. What is *not* left ambiguous is the reasoning, and it outlives this
  entry: the "free today" argument may not be reused again without checking
  whether it is still true. It was sound when written and false eight commits
  later.

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
- ~~**MVCC ships before recovery** (`docs/spec/txn.md` §8): an uncommitted row
  surviving a crash reads as **committed** on the next boot~~ — **closed for
  the mount path 2026-08-12.** Undo now runs before the listener binds, so a
  loser's rows are rolled back rather than published, and `RecoverCore`
  refuses the mount outright if it cannot do that (RV1). The gap's *shape*
  survives only where recovery is bypassed: `SimInstanceOptions::skip_recovery`
  is the harness's fault injection and boots into exactly the old behaviour,
  which is how the durability assertion is proved able to fail
  (`tests/sim_loop_test.cpp`). `docs/spec/txn.md` §8 needs amending at the source
  (RC10).
- ~~**DDL and catalog writes are unlogged**~~ — **closed 2026-08-19**
  (RV3): logged as ordinary records, replayed, and rolled back for
  losers; the durability entry above carries the details and the named
  remainder.
  The *other* half of that old entry — "DDL is not transactional,
  `CREATE TABLE` inside a transaction is not rolled back" — is **false
  for `CREATE TABLE` as of 2026-08-16** (`docs/spec/ddl-transactional.md`,
  `docs/inflight/in-progress/workplan-ddl-transactional.md`, DT1-DT4): a rolled-back create
  leaves no relation, and an uncommitted one is invisible to every other
  session by every route into it. **Atomicity and isolation only;
  durability is the sentence above, and `SHOW META` prints
  `ddl_durable=0` beside it so the pair cannot be read apart.** Never
  quote "transactional DDL" without that distinction — a reader assumes
  crash-durability and is wrong.
  **This paragraph said `DROP TABLE` and indexes were "still
  non-transactional, by name" and was stale from 2026-08-16; corrected
  2026-08-18.** What is true now: `DROP TABLE` is **atomic but not
  isolated** (DT5 shipped delete-marking for its dependent rows; other
  sessions still see the drop before it commits, because the
  `sys.objects` retype is an in-place overwrite with no undo chain —
  `ddl-transactional.md` §5a). `CREATE INDEX` is atomic and
  isolated; `DROP INDEX` is atomic and isolated **on core 0** since DT9
  taught the unfiltered catalog read that a delete-mark counts only once
  its deleter commits (§5b), which is core-0-scoped only because
  `IsInFlight` walks one core's live list and CC3 refuses cross-core
  writes.
  Delete-marked catalog rows no longer accumulate across mounts (DT10,
  §5c), ~~and within a mount they still do~~ — **closed 2026-08-19 by
  §5d**: DDL resolution now runs a horizon-gated purge, so a mark
  survives as long as some reader's view could still need the row, plus
  the wait for the *next* DDL resolution after that reader releases —
  nothing else triggers the sweep, and the mount takes any remainder.
  The price a surviving mark carries is unchanged and small: one
  comparison per mark per cold read (DT9's `live` factor left the
  per-mark term on 2026-08-18; `bench/results-ddl-catalog-read-ab.md`
  has the derivation).
  The prerequisite that closed it is **reader registration**
  (`docs/workplan-reader-registration.md`, `txn.md` §4.1): `live_` does
  not name every reader — a cross-core stage holds an
  `AutocommitSnapshot` across its parks (`remote_step_service.hpp`) —
  so every such snapshot now carries a `ReaderLease`, and
  `TransactionManager::ReadHorizon()` is the bound a purge retires
  below. The same prerequisite used to block the MVCC undo purge; what
  blocks that now is only its own §9-open retention policy.
  Also measured there and **not** DT9's: a transactional `DROP TABLE`
  costs ~517 µs against `CREATE TABLE`'s 48 and `DROP INDEX`'s 36,
  identical on both binaries — `Catalog::DropTable`'s five
  restart-from-head `ForFirstRow` sweeps.
  **Still non-transactional, by name**: `ALTER TABLE`, cabins, patterns,
  assertions, foreign keys. Each only inserts its own catalog rows, so
  each can adopt the mechanism the table statements proved; nothing new
  has to be decided for them. Isolating `DROP TABLE` is the one that
  still needs undo *records* for catalog rows — option (a) of DT5, not
  built.
- ~~**DT9's in-flight test can be fooled by a reissued transaction id
  after a crash**~~ — **closed 2026-08-18 by DT10**
  (`docs/spec/ddl-transactional.md` §5c). The exposure was real: an
  unfiltered catalog read counts a delete-mark only once its deleter is
  no longer in flight, the id ceiling is unlogged (`txn/trx_id.hpp`), so
  a crash could reissue a committed dropper's id and a live transaction
  wearing it would re-arm a dropped index whose btree is missing every
  row written since. DT10 retires every delete-marked catalog row at
  mount, before the listener binds, which deletes the question instead of
  answering it — and purges the marks that otherwise accumulated forever,
  one per column, index and foreign key of every transactionally dropped
  relation. `SHOW META` reports `catalog_marks_finalized`.
- **Keystone K1 does not hold across a crash**
  (`docs/rules/keystoneid-k0-findings.md`): the durable log names ids the
  unlogged `sys.tables.next_id` has forgotten. K-M2a/K-M2 own it.
- **The assertion checkpoint-genesis gap** (`docs/spec/assertion.md` §7):
  the group-directory fold needs records from the Bound Cabin's birth, and
  nothing durable holds headers for a checkpoint-bounded replay to start
  from. **Decided 2026-08-11 and now owned** — AS6a gives the checkpoint a
  headers-only directory snapshot and the entry a `group_id`, so replay
  folds from the last checkpoint; `docs/workplan-wal-recovery.md` RC07
  builds it. The gap stays listed until RC07 ships: today a restart still
  loses every group directory and enforcement does not resume.

- **A heap chain can be left out of page-wise order by an injected write
  fault** — found 2026-08-26 by the simulation harness on worktree
  `g1-peer-page-id-not-found` at `449804e`, on a **fresh, date-derived**
  seed rather than a committed one, and **not caused by the change it was
  run against** (the same cell fails identically with that change compiled
  out, and the sim mounts core 0 only — `sim/instance.cpp` passes
  `core_id=0` and installs no lease — so a peer-side fix cannot reach it).

  Reproduce:

      build-release/ckdbs-sim --seed 20260826003 --ops 1500 --iterations 2 \
          --mode clean --profile uniform --faults io --fault-rate 40

  and the `zipfian` profile of the same seed fails the same way. The
  finding:

      [chain-order] page 191: relation 't2': min_key 78 does not exceed a
      predecessor page's max id 183

  preceded by `page-fail-grow` at op 1418, three more armed page faults, a
  `log-fail-write`, and a transaction abandoned at op 1481. So an errored
  **page grow** appears to leave a chain whose page order no longer
  ascends, which is the ordering `docs/spec/heap-and-tuple.md` §3.1b's tail
  append and tail-page-only duplicate check rest on — the same ascent that
  invariant 11's "a named key below the high-water mark is refused on a
  heap relation" is justified by. `mode=clean` means no crash was
  injected: the fault path alone produced it, which makes it a live-engine
  question and not only a recovery one.

  **The seed is deliberately not appended to
  `tests/testdata/sim_seeds.txt`**: that corpus is regression-mandatory and
  runs on every build, so a seed joins it when the engine fix lands, not
  when the failure is found. Whoever fixes this adds it there.

## What a restart loses (without a crash)

- ~~**A peer's page ownership.** Extent leases and relation grants are
  memory-resident and carved or sent once, so a restarted peer could
  neither read (Debug) nor write (every build) the pages it had
  allocated, and could not write the creation pages core 0 had handed
  it~~ — **closed 2026-08-24** (PW1c-7, `docs/inflight/in-progress/workplan-peer-writer.md`
  §8): the PL-C stream stamp is the durable form of ownership, a leased
  store claims own-stamped pages on the fault, and a creation page never
  acquired is re-delivered on the owner's request. What a restart still
  costs: one claim per page on first touch (a stamp read on the miss
  path's own bytes), and a relation whose grant was lost takes one
  retryable refusal before the re-delivery lands. **The same test found
  and closed a crash-recovery hole**: a peer that crashed between a new
  page's PAGE_INIT and its first write-back could not remount — the
  reserved extent had the page *allocated* in the flushed map while the
  device held zeros, and redo poisoned the checksum failure instead of
  creating the page. An allocated-but-never-written page now reads
  `NotFound` and redo's PAGE_INIT arm creates it (PW1c-7's row has the
  detail).
- ~~**An extent reserved after core 0's last map flush never reached the
  device on its own.**~~ — **found and closed 2026-08-25** by PW3b's
  remount test (`docs/inflight/in-progress/workplan-peer-writer.md` §6). `DevicePageStore` marks
  its free map dirty when `free_map_bytes()` is *taken*, and
  `ExtentAllocator` held that span for its life — so every refill core 0
  granted from the drain tick set bits in a map the next flush skipped as
  clean, and `extent_lease.hpp`'s own "durable exactly when the store next
  flushes" was false. Invisible for two reasons that both ended: a crash
  could once only burn an *unspent* extent, and a peer's pages in such an
  extent were re-created at every remount by redo's `CreateAt`, which the
  cadence checkpoint (PW3) stops doing an interval later and the shutdown
  checkpoint at the first restart — PW3b's test answered `page id not
  found` for a 200-row relation. The crash-path consequence was worse than
  the remount: a run holding a peer's committed rows was free again in the
  map the next mount read, for core 0's allocator to hand out over them.
  Closed on both halves: the production allocator is built **over the
  store**, so every reservation marks the map, and the extent grant handler
  calls `ExtentAllocator::Persist()` (`DevicePageStore::PersistMaps`: the
  map pages and a device sync, not the frames) **before the grant leaves**
  — a run that cannot be made durable is not granted and the peer gets the
  zero-page reply. Cost: one map write and one fsync on core 0 per 64-page
  extent granted; not measured (the v2 amendment). Pinned by
  `AReservationAfterTheLastFlushIsLandedByPersist`.
- **Cabin entry sets** are memory-resident by design
  (`docs/spec/cabin.md` §9): the `sys.cabins` row survives, the sets
  re-observe from traffic.
- ~~**Assertion enforcement**: the registry/directory is memory-resident, so a
  surviving assertion honestly reports `enforcing=0` until recovery can replay
  the directory~~ — **closed 2026-08-12** (RC07, AS6a). A mount revives each
  surviving declaration from `sys.assertions` (§8.2 keeps `source_text` as the
  canon so the group columns can be recovered by re-parsing it), restores its
  group headers from the last checkpoint's `ASSERT_SNAPSHOT`, relinks the entries
  by scanning the cabin's own pages — bounded by the assertion's entry count, not
  the relation's rows — and folds the `ASSERT_*` records after the snapshot.
  `SHOW ASSERTIONS` reports `enforcing=1` immediately, which is what
  `docs/spec/assertion.md` §7 always claimed and the engine contradicted until
  now.

  Two things stay true and are reported rather than assumed. An assertion whose
  directory could not be rebuilt — no snapshot in range, a declaration that no
  longer parses, a group column an `ALTER` renamed — is **left out of the
  registry** and counted in `SHOW META`'s
  `recovery_assertions_unrecovered`: a cabin at zero admits every write, so
  adopting one would report `enforcing=1` for a constraint enforcing nothing. And
  the §9 counters still restart at zero, because they live and die with the
  directory by design.

- **Waystone sighting counts** restart (a performance event, never a
  correctness one — invariant 8).

## Reclamation — two purges exist, everything else still does not

Readers are **registered** as of 2026-08-19 (`txn.md` §4.1,
`docs/workplan-reader-registration.md`): `ReadHorizon()` answers the one
question every purge must ask. Two consumers exist — the catalog
delete-mark purge (`ddl-transactional.md` §5d) and the undo purge
(`docs/inflight/in-progress/workplan-undo-purge.md`, the same day: settled pages recycle into
the log's own growth, so this run's chain plateaus). Everything else
still waits on its own gate, so:

- **Every `cores = 1` versus `cores = N` ratio in `bench/` is partly a
  measurement of how awake the machine is** — found 2026-08-26
  (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §7b). A
  reactor with nothing to do still wakes about a thousand times a second (the
  1 ms WAL drain timer, **measured** at 931–947 iterations/s per idle core off
  the new `sched_iterations` field), so a `cores = N` server keeps N CPUs out
  of deep idle that a `cores = 1` server leaves asleep. **measured**: three
  helper processes doing nothing but `sleep(1 ms)`, pinned beside a
  *one-core* server, reproduce a four-core server's advantage to within 0.2%
  (1.111× against 1.109×) and seven reproduce an eight-core server's to
  within 0.1% (1.165× against 1.165×). So `bench/v2.1.0` §11-3's
  "four-core-server effect" and its three engine-side candidates — four WAL
  anchors, per-core extent leases, background work moving off core 0 — are
  **unnecessary**, not merely unseparated. Anything quoting a `cores = 1`
  baseline is quoting this too. A separate finding in the same section: the
  driver's null cell (`cores = 1` against `cores = 1`) reads **1.099**, not
  1.000, because its second arm always runs later — so every A/B ratio this
  harness has produced carries a ~10% ordering bias, `bench/v2.1.0`'s C1 and
  C2 controls included.
- ~~**Sustained shipped `CREATE INDEX` leaves a peer-owned relation
  permanently unwritable, where core 0 fails cleanly**~~ — **closed
  2026-08-26** (worktree `fix-peer-index-build`). The cause was not the
  index build at all: a peer's free-map copy is a snapshot taken at
  `DevicePageStore::Open()`, and the only thing that refreshed it was a
  *relation grant*. `sys.indexes` fills its root page and spills onto
  `kCatalogOverflowFirst` (page 15), which core 0 allocates from the map it
  owns; the peer then invalidates its catalog, re-reads the chain, follows
  `next_page_id` into page 15, and `IsAllocated` answers from a snapshot in
  which that page does not exist — `NotFound`, no retryable bit, forever,
  and every INSERT hits it because every INSERT maintains the index.
  Confirmed from the raw data file: page 15 **is** marked allocated on
  disk while the peer answered not-found. The fix is one call —
  `CoreRuntime::InvalidateCatalog()` now runs the pre-existing
  `RefreshFreeMapFromDevice()` before evicting the catalog frames, which is
  the same adoption the grant receivers already did for the case a grant
  covers. Core 0 needed no change: `BroadcastCatalogInvalidation`'s
  `FlushPages` already writes the dirty maps after the pages they describe,
  before the message leaves. Measured after: the reproduction runs 297
  builds at `cores = 4` and at `cores = 8` with the relation still
  writable, where it died at 58. Pinned by three tests, each verified to
  fail without the fix.
  **The residual this leaves, named**: four catalog relations are written
  with **no** `BumpVersion` and therefore no broadcast — `sys.patterns`,
  `sys.pattern_defs`, `sys.access_stats` and `sys.assertions`. Core 0 can
  grow any of them onto an overflow page and tell no peer. It is safe only
  because a peer's dispatcher is built with no recorder, no replay, no
  access statistics and no cabins, and its assertion registry is never
  resumed, so a peer reads none of the four. **Enable any one of them on a
  peer and this bug returns on a chain nothing invalidates** — the refresh
  unions the whole region-0 map page, so a later bumping DDL would adopt
  the bits anyway, making it a window rather than a permanent state, but
  only if such a DDL ever follows. The original report follows.
- **The original report, for the record** — found 2026-08-26 by
  T4's probe (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md`
  §8d), and **re-confirmed on the merged tree at `b85cd31`**, after
  FM2-FM5 raised the free-map ceiling. The shape, **measured** twice on each
  tree: `CREATE INDEX`/`DROP INDEX` in a loop from core 0 against a
  peer-owned relation succeeds ~58 times, consuming 1,856 pages (15 MB for
  58 usable indexes on a 3,000-row relation — refused attempts allocate too,
  ~7.7 pages each), after which **every** later write to that relation
  answers `ERR page id not found`, which is **not retryable** and does not
  clear. The identical churn on `cores = 1` runs 279 builds and then refuses
  by name — *"anchor page holds 679 index entries already; the table is
  full"* — with the relation still writable afterwards.

  **A second fix landed the same day, independently, at the other seam**
  (worktree `g1-peer-page-id-not-found`, `docs/inflight/in-progress/workplan-peer-writer.md`
  PW1c-8). It found the identical mechanism and answered it **lazily**: a
  leased store adopts the device's map once before calling a page absent,
  and the refusal names the id it withheld. The two compose, and the lazy
  half is precisely the backstop the residual above needs — it covers an
  allocation whatever did or did not announce it, so the four relations
  that write without `BumpVersion` cannot make the bug permanent again;
  they cost a miss, not a mount. Measured on its own: 400 builds clean at
  `cores = 2`, where 58 poisoned before.

  Its review closed **a second door of the same class**, which the
  multi-free-map series had just made reachable: `RefreshFreeMapFromDevice`
  walks only *resident* regions, and `free_map_bytes_for` answers an absent
  region as all zeroes — so a page core 0 placed in a region created after
  the peer mounted could not be adopted at all, not even one the peer had
  been granted. `AdoptDeviceMapOnMiss` loads an absent region first.

  **Two residues of the pair, stated rather than fixed**: a page core 0 has
  allocated but not yet flushed the map for is still refused `NotFound`,
  which carries no retryable bit — the next statement re-adopts, so it
  clears itself rather than poisoning, but a client sees a non-retryable
  error for a transient condition, and `MayWrite`'s own comment calls this
  case *"a retryable not found"*. And a peer reads core 0's catalog
  **bytes** off the device, so a catalog page core 0 holds dirty is a stale
  read on the peer; neither fix reaches it, and one writer per catalog page
  is the whole of today's discipline.

  Reproductions, unchanged: `bench/parked_coroutine_probe.py` driven in a
  loop, and the two probes archived at
  `bench/v2.1.0/archive/pretasks-v2.1.0-10-g82a2749/t4/probes/`. The
  regression is pinned at unit level by
  `APeerAdoptsTheDeviceMapBeforeCallingASystemPageAbsent` in
  `tests/device_page_store_test.cpp`.
- undo pages from a **previous run** leak: a restart abandons the old
  chain and the recycle list is memory-resident, so those pages stay
  allocated until UP4's mount-time reclaim exists (they always leaked;
  what is new is that the current run stops adding to the pile).
  `SnapshotTooOld` is structurally unreachable **by decision** now, not
  by omission — D1's horizon-only retention frees nothing a live view
  can reach, and the byte-cap that would make the error real was
  declined for v1;
- delete-marked tuples keep their slots; var-heap bytes of superseded
  values stay; superseded index and Cabin entries stay
  (`docs/spec/index.md` §13);
- catalog rows are never reclaimed (the column ceiling is on columns ever
  created); pages, extents and Keystone ids are never reused;
- `DROP TABLE` exists (`docs/spec/drop-table.md`) but is **catalog-scoped**:
  the relation's pages, var-heap chain and index pages orphan — leaked
  space, deliberately, because free-map reuse is gated (a reallocated page
  breaks trail validation, `physical-optimizer.md` §6 gate 3; a
  reader horizon exists now, but that gate is its own). The oid is
  tombstoned in `sys.objects` and never
  reissued, which is what keeps dead-oid advisory structures harmless.
  `ALTER TABLE` is catalog-only renames (`docs/spec/alter.md` AL1). Both
  RESTRICT on assertions; DROP also RESTRICTs on referencing foreign keys.
  Every one of these is an unlogged catalog write like all DDL: a crash
  after it can lose it.

- **A refused `CREATE INDEX` used to keep the pages it had allocated, at 32
  a time** — found 2026-08-26 by the pretasks
  (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §8d-1),
  **fixed 2026-08-26** on worktree `g1-peer-page-id-not-found`
  (`docs/inflight/in-progress/workplan-peer-writer.md` PW1c-9), the statement-shipping work
  order's G2 gate. Kept, because two of the three leaks it names are still
  open.

  **What it was**: `CREATE INDEX` builds the whole tree and seeds the
  relation's anchor slot *afterwards*, so once the anchor's 679-entry table
  filled — and `DROP INDEX` frees no entry, `storage/anchor_page.hpp` says
  why — every further attempt allocated an index tree and threw it away.
  Nothing frees. **measured** at `cores = 2` with
  `bench/index_refusal_storm_probe.py`: 3,259 refusals in 30 s consumed
  **104,257 pages, exactly 32.0 per attempt**, past the 65,280 ids one
  free-map region covers. That last detail is the one worth carrying: it
  was FM's growth that kept the instance alive where the same storm had
  previously ended at *"free map full"* — the ceiling rising made the leak
  slower, never smaller.

  **What closed**: the refusal is asked for before either build path
  allocates (`storage::CheckAnchorRoomForIndex`, the predicate
  `SetAnchorIndexRoot` already applied, extracted so the check and the
  write share one bound and one message). The same storm now runs 27,267
  refusals at a **marginal cost of 0 pages**.

  **What did not**, each still live:

  - a build refused by a **spent extent lease** mid-way keeps its pages.
    This is D5's other half and hoisting cannot reach it — there the check
    and the allocation are the same act. Rare, and self-limiting once a
    refill lands, but not zero;
  - a build that **succeeds** and is then dropped still orphans its whole
    tree. That is the gated reclamation leak listed above, not this one;
  - **`UpdateIndexRoot` is the same violation in a smaller costume**: a
    root split (`exec/index_maintain.cpp`) allocates and *then* writes the
    anchor slot, so an index whose slot was never seeded can hit the cap
    from an ordinary `INSERT` and orphan the split's pages. Reachable only
    for a data file written before PW2-3's seed existed;
  - **the write-rights class is untouched.** The pre-check reads through
    `GetForRead`, which is deliberately more permissive than the write
    (and must be - `Get` would dirty the frame, making every refusal cost
    a write-back). So a peer holding read but not write rights on its own
    anchor passes the check, builds the whole tree, and fails at the seed.
    "A refusal allocates nothing" is true of the entry-table cap and must
    not be read wider.

  A refused attempt no longer burns an index oid on the local arm - the
  check moved into `Catalog::CheckIndexDef`, which runs before
  `PrepareIndexDef` issues one. On the shipped arm core 0 still issues the
  oid before the owner refuses, which `PrepareIndexDef` documents as
  permitted by the row-id sequence.

## Concurrency and multicore

- **Rotation divides the group-commit batch, so spreading writers over
  cores does not add write throughput** (found 2026-08-26 on the first
  host with three writer cores,
  `bench/v2.1.0/results-multicore-writers-v2.1.0.md` §6-§7, at `v2.1.0`).
  A core's commits are drained by a group committer that turns whatever has
  accumulated into **one** device `fdatasync` — a post-task hook every
  reactor iteration (`src/server/expeditor.cpp:1650-1657`), installed the
  same way on every peer (`src/server/core_runtime.cpp:718-734`). Spreading
  N writing sessions over W cores therefore divides that batch by W, and
  each of the W cores is independently capped at the volume's
  **single-stream** `fdatasync` rate. Measured: every multi-core cell runs
  at 965–1,071 commits/s **per writer core** against a probe-measured
  1,066/s single-stream rate, while the single-core arm scales linearly
  with sessions because its batch grows. The result is that at two sessions
  per writer core rotation runs **0.989×** of the same four-core server not
  rotating, and only at one session per core (no batch to lose) does it win
  — 1.751× over its own control. Nothing is CPU-bound anywhere near this:
  core 0 sits at 8.9% and the writer cores at 13–14%.
  **Not a cadence problem, and the obvious lever is the wrong one**: the
  same run varied `wal_drain_interval_us` over a 10× range and throughput
  did not move (flat at 3,554–3,643 ips), because the timer is only a
  backstop to the per-iteration hook. The cap is `fdatasync` *latency*
  (~0.94 ms), which merely coincides with the 1 ms default. **Open, and
  deliberately undecided by that run**: whether a commit batch can span
  cores at all, or whether per-core drains are inherent to thread-per-core;
  and where between one and two sessions per core the crossover actually
  sits, which is the number any placement policy would need. No constant
  was changed.

- ~~**Lease refills lag under load on a peer** (found 2026-08-25 by PW6's
  four-writer cell, `bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md` §6a-§6b): with
  four active sessions on one peer the row-id, trx-id and extent refills
  complete hundreds of milliseconds to seconds after a ring round trip that
  idle takes 2–7 ms — relations 3 and 4 wait 0.5–1.75 s for their first
  INSERT, the trx-id lease is spent with a quarter-window of headroom, and
  the 64-page extent lease is spent so the btree insert fails and **INSERTs
  are lost** (1, 13, 51 per run)~~ — **traced and closed the same day**
  (PW7, `docs/inflight/in-progress/workplan-peer-writer.md` §6): the reactor's share-proportional
  pick re-polled a parked refill up to 64 times an iteration and charged the
  `system` group for every poll, and the group's debt then kept the *next*
  refill unpolled for 395 iterations (546 ms, measured). Two floors under
  the share law in `RunReadyTasks` (`docs/spec/sched.md` §4) end it: the same
  cell's refill waits 2.7 ms, four writers run at 0.99–1.03× the single-core
  configuration, and no row is lost (measured on the tree committed as
  `v2.0.0-52-g2c6ae23`; ~~the re-measurement of PW6's matrix at the fixed
  engine is owed as a `bench/v2.0.0/` file~~ — **re-measured 2026-08-25 at
  `v2.0.0-67-g952bbb9`**,
  `bench/v2.0.0/results-multicore-writers-v2.0.0-67-g952bbb9.md`: the
  four-writer cell runs at 1.030× the single-core configuration with zero
  lost rows; the row-id refill's longest wait is 3.0–3.3 ms; the
  two-writer peer path runs at 0.990× against a 0.944× control of identical
  engines, whose 0.866 outlier that file's §3 explains as one unattributed
  485 ms stall of core 0's reactor; a point-SELECT beside a committing
  session is 1,088/1,083 µs on core 0/core 1 against 37/35 alone).
  **Independently validated at three writer cores 2026-08-26** at `v2.1.0`
  (`bench/v2.1.0/results-multicore-writers-v2.1.0.md` §8a), on the first
  host that can run them, and the validation needed the *shape* rather than
  the cell: the six-relation cell spreads two sessions over three cores and
  cannot provoke the defect at all — with floors, without floors and at
  `9c0528a` it measures 1.048 / 1.027 / 1.057, indistinguishable. At PW7's
  own shape (four sessions on one peer core) the collapse reproduces —
  **0.765× at `9c0528a` and 0.742× with the floors disabled on HEAD,
  against 1.081× with them**, matching PW7's reported 0.61–0.80× before —
  and the two independent pre-floors arms agreeing is what attributes it to
  the floors rather than to the ~20 commits between the trees. The
  instrument says it directly: trx-id submit lag **0.0 ms over 0 reactor
  iterations** with the floors against **924.4 ms over 934** without, and
  that stall is present on the six-relation cell *even where throughput does
  not move* — so the lag legs, not the ratio, are the diagnostic. Note for
  anyone repeating this: `include/kds/server/lease_refill_stats.hpp` does
  not exist at `9c0528a`, PW7 having shipped instrument and fix in one
  commit, so a pre-floors *tree* cannot report lag at all; the floors must
  be disabled in place on HEAD, and reverting `src/sched/scheduler.cpp`
  wholesale does not compile because the floors changed `PickNextGroup`'s
  signature. What
  stands from the finding: **a
  reactor with any parked coroutine spins** — `IdleTimeoutMs` counts a
  parked task as ready and drops the idle block to 0, so a peer waiting on a
  grant burns its CPU (108,150 iterations in one 39 ms refill) — and **the
  group accounting charges nobody for reactor time outside polls** (the
  drain's fdatasync above all), which is why a low-share group's debt was
  so slow to clear. Both are `docs/spec/sched.md` §4's. **The spin is fixed
  2026-08-26** (worktree `v2.3.0-reactor-wake`, the v2.3.0 order's RW3):
  `IdleTimeoutMs` no longer reads a non-empty queue as work to do. A block
  is permitted after a full iteration in which *nothing advanced* — no
  event, timer or message, no task that ran a line or finished, no task
  newly queued, and no work from the post-task hook — where "ran a line" is
  `Task::advanced_in_last_poll`, answered by `CoroTask` from whether the
  poll resumed the coroutine at all and defaulting to `true` everywhere
  else, so an untracked task type keeps the reactor awake rather than being
  slept through. The hook's clause is the load-bearing one: a committing
  statement parks on `durable_lsn` and only the hook moves it, so a hook
  that synced and reported nothing would have put the WAL drain interval on
  every commit — `SetPostTaskHook` therefore takes a `std::function<bool()>`
  and both drain sites answer with `HasPendingGroupCommits()`.
  `SHOW META`/`Scheduler::parked_idle_blocks()` counts the blocks that used
  to be spins, and **the fix is measured**: arrival-core CPU 0.862 → 0.032
  at one parked waiter, 0.923 → 0.028 at four, against a throughput ratio
  that rises 0.912 → 0.999 at four and falls 0.900× at one
  (`bench/v2.3.0/results-parked-is-not-ready-v2.2.1-12-g12c0ebb.md`,
  `bench/v2.3.0/results-hot-path-cell4-v2.2.1-14-g13c6d4d.md`). **The
  accounting gap is now readable too** — `sched_idle_block_us`, added
  2026-08-27 — so `wall - Σ polled - idle_block` separates sleep from
  uncharged work: an arrival core is 79.5% sleep and 10.3% unaccounted work
  where the two used to arrive as one ~90% lump. The gap itself is
  unchanged: an idle block still belongs to no group, and *that* decision
  stays open. The old text, for what it was:
  the spin was **unfixed**;
  the accounting gap is **unfixed but no longer invisible** — since
  2026-08-26 (T4 of the statement-shipping pretasks) `SHOW META` prints
  `sched_wall_us`, `sched_iterations` and per group
  `sched_<group>_polled_us` / `_polls` / `_consumed_us`, so
  `wall - sum(polled)` is the untracked time and a spin reads as polls
  climbing while polled time does not. `bench/v2.1.0` §11-5 recorded the
  measurement as impossible from outside the process and owed it to
  whoever next touched §4; that debt is paid, and what is owed now is a
  decision about the gap, not an instrument for it.
- ~~**Three peer refusals say "retry" without the wire's `retryable=1`** —
  the row-id, trx-id and extent leases' `ResourceExhausted`
  (`include/kds/base/status.hpp`: only `TxnConflict` is `IsRetryable`, by
  decision). A client retrying on the bit alone loses rows to the extent
  one, as PW6's driver did until it matched the messages. A protocol
  decision: extend `IsRetryable` to the lease class, or re-code the three.~~
  — **closed 2026-08-25** (worktree `lease-refusal-retryable`) by
  re-coding the three: each is `TxnConflict`, the one code `IsRetryable`
  admits, which keeps the wire's bit one code wide as `status.hpp` decided
  (the alternative, a second code `IsRetryable` also admits, widens the
  wire's bit past one code — the thing `status.hpp` declined when it split
  the assertion verdict across an existing retryable code and a new
  non-retryable one rather than mint a second retryable spelling). The dispatcher renders every site a lease can refuse at
  through `ErrorReply`: every `BeginWrite` failure, `BEGIN` itself (an
  explicit transaction draws its id there and nowhere else, so that is
  where a spent trx-id lease refuses a transactional client), and
  `InsertOneRow`'s allocation, encode — a spilled value grows the var-heap
  — placement, index maintenance and undo append, the last three all being
  the extent lease. So the three print `ERR TXN_CONFLICT retryable=1 ...`;
  pinned end to end by `ASpentLeaseRefusesWithTheWiresRetryableBit`
  (`tests/core_runtime_test.cpp`). UPDATE and DELETE already rendered
  through `ErrorReply` on both arms, and the sorted bulk fill is
  unreachable on a peer (`SortedFillEligible` requires a writable
  catalog). The lease *services'* own `ResourceExhausted` — a refill core 0
  denies — is a request outcome, not a client reply, and keeps its code.
  And a lease core 0 has **denied** answers the next statement without the
  bit (`ResourceExhausted`, `RowIdLeaseTable::Next`): the causes are
  permanent — no `sys.tables` row, an exhausted id space — so a retry loop
  stops after one round trip instead of spinning to its deadline. The
  review of this change found that case
  (`ADeniedRelationAnswersOnceWithoutTheBitThenAsksAgain`).
- ~~**Statement shipping is built and unmeasured**~~ — **measured
  2026-08-26**, `bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md`
  (SS-B, the order at `instructions/v2.2.0/measurement-after-s5.md`). The
  demand entry closes with its number: **80–92% of an unrouted client's
  writes refused → 0%**, zero CC3 refusals in 4,800 unrouted attempts at
  `cores` 4 and 8, the engine counter reading 0 from every core. What the
  run leaves open is *not* the conversion; it is the price, below.
- **The correctness order's Part A ran 2026-08-26**,
  `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md` — five
  items clean and two findings, which is the track SS-B's order assumes
  and does not test.
- ~~**Shipping costs ~2× at one session per owner, and the cause is a
  scheduler property rather than the wire**~~ (measured 2026-08-26, SS-B) —
  **closed 2026-08-27** (worktree `v2.3.0-rwc1`, the v2.3.0 order's
  RW1-RW3), *with its numbers*:
  the same cell reads **0.416 → 0.989** at `01da467`
  (`bench/v2.3.0/results-reactor-wake-r1-v2.2.1-10-g01da467.md`), the
  shipped-minus-seated delta falls from **1,059.6 µs to 20.0 µs** and stops
  depending on `wal_drain_interval_us` at all — 43.2 / 43.2 / 43.2 / 43.5 µs
  at 1000/2000/3000/5000, and 42.4 µs at **50,000**, against a pre-wake arm
  that tracks the knob 1:1 to its 10 ms ceiling
  (`bench/v2.3.0/results-knob-sweep-cell2-v2.2.1-14-g13c6d4d.md`). What is
  left is the wire, and it is a **constant** rather than a ratio: 20 µs is
  1% of a `group` statement and half of a `relaxed` one, which is the form
  `docs/spec/crosscore.md` §9's routing decision now inherits. The original
  entry, for what it was: The memo's claim 2 is upheld and by more than it predicted: 0.526
  (B1), 0.429 (B4 at K = 1), 0.531 sustained over 25,000 statements (B6),
  five reps each and an order of magnitude outside a 1.016 noise floor.
  **D6 ships unconditionally, so that penalty is being paid in the R1
  regime today** — the number `docs/spec/crosscore.md` §9's routing decision
  inherits.
  The cause is neither the round trip nor the waiter: the shipped-minus-
  seated delta is a flat **1,064 µs under `group` and 1,068 µs under
  `relaxed`** — the same constant with the device in the path and without
  it — and it tracks the reactor's idle block over a fivefold range
  (1.08 / 2.10 / 3.11 / 5.12 ms at `wal_drain_interval_us`
  1000/2000/3000/5000, seated control flat at 23 µs). **An idle reactor
  sleeps for a whole millisecond and nothing wakes it when a ring message
  arrives**: `Scheduler::IdleTimeoutMs` is an `int` of milliseconds handed
  to `epoll_wait` and rounds *up* by its own argument
  (`src/sched/scheduler.cpp:196-214`), and no wake path exists in the ring
  or the scheduler. Any cross-core message to an idle core pays it;
  shipping is only the first feature to put one on a client's critical
  path twice per statement. **Owned by `docs/spec/sched.md` §4**, not by
  shipping, and deliberately unfixed by the run that found it.
  At four sessions and above the owner is never idle, the sleep never
  happens, and shipping runs at **0.93–0.99×** — inside the floor.
- ~~**One parked waiter already burns ~89% of an arrival core**~~ (measured
  2026-08-26, SS-B B4) — **closed 2026-08-27** (the same worktree, RW3),
  *with its numbers*:
  arrival-core CPU **0.862 → 0.032** at K = 1, every rep 0.784-0.915 before
  and 0.026-0.051 after
  (`bench/v2.3.0/results-parked-is-not-ready-v2.2.1-12-g12c0ebb.md`), and
  **0.923 → 0.028** at K = 4 while the throughput ratio *rose* 0.912 → 0.999
  (`bench/v2.3.0/results-hot-path-cell4-v2.2.1-14-g13c6d4d.md`). **The cost
  is stated with it**: at K = 1 on a box with spare cores the trade is
  0.900× throughput, +31.5 µs p50 and +277 µs p99 — a reactor that spun
  noticed its reply in nanoseconds and one that sleeps must be woken — and
  it is gone by K = 4. The original entry: Polls rise 3.1M → 7.9M/s from K = 1 to K = 16 while the
  cost per poll holds at 0.059–0.068 µs: the spin signature the pretasks
  could not build a population to look for, now built by shipping's own
  waiters. No throughput cost was observed *only because CPUs were free* —
  at S = 14 two arrival cores sat at 0.921 busy against 0.058 seated.
  Handed to `docs/spec/sched.md` §4 with the idle-block finding above; both are
  the same missing wake path seen from opposite ends.
- **Claim 3 is unproven, not disproven** (2026-08-26). The owner core runs
  at 11–24% busy at the top of the curve this harness can build, with
  92–96% of its reactor wall charged to no scheduling group, so its
  execution capacity is not being tested. Two things the memo could not
  have: the resource that *does* saturate under shipping is the **arrival**
  core, and shipping moves the owner's ceiling further away rather than
  nearer — a shipped statement costs the owner's foreground group 1.8–2.2
  µs per poll against 4.4–4.9 seated at the same 2.00 polls/statement,
  because the socket and the render happen on the arrival core.
- **The R6 residue, now a measured distribution** (2026-08-26, B5) — the
  evidence base this file has been saying a 2PC decision must be designed
  from, read for the first time. Of what shipping does not convert:
  `in_explicit_txn` and `subquery_write` refuse 100% (the R6 write
  population), `two_owner_read` refuses 87.5% with **12.5% already answered
  correctly by the P4d pipeline** when the session lands on core 0, and
  `overlong_read` answers 100% `UNKNOWN_OUTCOME` (the entry below). The R6
  entry stays **open** and now points here rather than at a total.
- **A shipped write's abandoned transaction costs WAL, not the lease**
  (measured 2026-08-26, B6, refining the entry below). Six extra trx-id
  refills per 25,000 statements — 2.3 µs/statement amortised — and **no
  latency step at any 4,096-id boundary** (boundary vs elsewhere 1.02–1.03×
  on *both* arms, so the step is not the lease's). What it does cost is
  **64.02 bytes per shipped statement into an otherwise-idle stream**, a
  13% rise in instance WAL, measured by scanning the written extent of each
  core's segment. Whether the fork moves above `BeginWrite` is the
  operator's call and now has both sides of the trade in numbers.
- **A shipped read whose reply exceeds 992 bytes is answered
  `UNKNOWN_OUTCOME`, which is the wrong thing to tell a client about a
  read** (found 2026-08-26 by the SS2 review, not fixed). The ring's
  payload is 1,024 bytes and a reply header costs 32, so a shipped
  statement's answer is capped at 992 - roughly 40 wide rows. SS1 chose to
  **refuse rather than truncate** there, which is right, and to spell the
  refusal `UnknownOutcome`, which for a *write* is exactly right: the
  statement committed and its answer is lost. For a **read** it is a lie in
  the dangerous direction - `retryable=0` plus "the statement's effect
  stands", told about a statement that had no effect and can simply be run
  again. Before shipping, the same statement got a clean affinity refusal.

  Not fixed because the fix contradicts a ratified rule rather than an
  implementation: `statement_ship_service.hpp`'s rule 1 says the only
  refusal legal after `Ship` returns OK is `UnknownOutcome`, and the owner
  is the only party that knows the statement mutated nothing. Either the
  owner gains a way to say "this was a read and its answer does not fit" -
  a second non-retryable code, or the outcome carrying whether anything was
  staged - or the cap is raised, which is `docs/spec/crosscore.md` §9's ring
  sizing decision. The arithmetic and the two sites
  (`ShippedStatementReplyOf`, `StatementShipServer::Reply`) are
  unambiguous; what is undecided is which of those three it should be.
- **What shipping deliberately does not carry, and where the residue is
  read** (2026-08-26). Refused, by scope and not by omission: a statement
  **inside an explicit transaction** (nothing crosses transaction state), a
  statement **spanning two owners** (R6, which is the 2PC question), and
  any statement on a path that cannot park. The first two keep their exact
  CC3 spelling and their retryable bit, and they are what
  `cross_core_write_refusals` counts from now on — its meaning is
  unchanged, so the series spans both eras and the residue is directly
  readable as the 2PC evidence base (`docs/spec/crosscore.md` §6).
- **A shipped write opens a transaction on the arrival core and abandons
  it** (SS2, 2026-08-26). The dispatch fork sits where the relation is
  resolved, which is after `HandleInsert`/`HandleUpdate`/`HandleDelete` have
  already called `BeginWrite` — so a statement bound for another core spends
  one transaction id of the arrival core's 4,096-id lease block and appends
  a `TXN_BEGIN`/`TXN_ABORT` pair to a log that is otherwise idle under
  shipping, then ends the scope immediately. The transaction lives
  microseconds and is never held across the park, so it pins no read
  horizon and blocks no purge; what it costs is a lease refill every ~4,096
  shipped writes on a core doing no writing, and two buffered appends per
  statement.
  **Why it is here rather than fixed**: moving the fork above `BeginWrite`
  means resolving the relation before the scope exists, which for a *local*
  write on a multi-core instance is a second parse and a second catalog
  resolve on every statement — a per-statement cost on exactly the path
  this version is measured against, to save a cost on the path that already
  pays a round trip. SS-B is where the trade is priced; until then the cost
  is charged to the shipped path deliberately.
- **An assertion declared while a peer is running is invisible to that peer,
  and a shipped write to that relation is admitted unenforced** (Part A
  finding 2, 2026-08-26,
  `bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md` §5). Measured:
  `CHECK COUNT(*) <= 1` on a peer-owned relation, then a shipped `INSERT`
  puts a **second row in the same group**
  (`DISABLED_AShippedWriteToAnAssertionCoveredPeerRelationIsRefused`).
  A peer may not write an assertion-covered relation at all — its entry
  pages are the system core's and carry no write grant — and the gate that
  says so reads `enforcer_.AnyOn(oid)`, a per-core memory-resident registry
  populated at mount (RC07) that `CoreRuntime::InvalidateCatalog` does not
  refresh: it refreshes the free map, evicts the catalog frames and
  invalidates the catalog cache, and nothing else. The FK and Cabin arms of
  the same gate hold, because they read `TableAccess`, which *is* refreshed.
  **Not created by shipping** — a client on the peer's own listener could
  already reach it — and made ordinary by it, since every core-0 client's
  write for that relation now takes this path. The bound is a remount. The
  fix is not to let the peer enforce (it cannot) but to make the gate read
  what the other two arms read, which crosses `docs/spec/assertion.md`'s
  "complete and enforcing" claim and `docs/spec/crosscore.md`'s peer contract.
- **A duplicate whose dedup record the memory bound evicted early is
  executed again** (Part A finding 1, 2026-08-26, same file §1). Fill
  `kShippedDedupMaxRecords` (4,096) with distinct sessions and the oldest
  record is dropped inside its retention; the same (session, sequence)
  delivered again then runs a second time — against an engine-issued pk, a
  second row. Measured `executed` 4,097 → 4,098
  (`DISABLED_ADuplicateWhoseRecordWasEvictedEarlyIsNotReExecuted`).
  `shipped_statement_executor.hpp` already states this and `early_evictions()`
  counts it; what Part A asks for is `UnknownOutcome` instead. **Latent**:
  nothing re-sends a landed request today (`SendRetryTask` retries only a
  send the ring refused), so no live path produces a duplicate at all — this
  is the retry paths a routing layer will bring. Three fixes, each a policy
  call: refuse rather than evict (an availability cliff at 4,096 concurrent
  shipping sessions per owner), carry a retry bit on the request (five
  reserved bytes exist), or keep tombstones under a second bound.
- **Nothing reclaims a shipped statement's waiter if its coroutine is
  destroyed rather than completed** (Part A, 2026-08-26).
  `StatementShipClient::Close` is reached only from
  `FinishShippedStatement`. Unreachable today because
  `TcpServer::CloseClient` defers teardown while a statement is in flight,
  so a dropped connection's statement runs to completion and closes its
  waiter — and this is what breaks the day a cancellation path is added.
  Asserted from the other side by
  `AParkedShippedStatementDestroyedUnderItsWaiterLeaksTheWaiter`, which
  fails loudly if the leak is ever fixed without updating this entry. A
  second consequence of the same deferral: for a shipped statement it is
  bounded by the **ten-second ship deadline** rather than by the row-touch
  budget `CloseClient`'s comment cites.
- **The WAL drain's fdatasync runs on the reactor thread**, so every session
  on that core — reads included — waits out a committing session's sync:
  point-SELECT 973 µs beside one writer against 37 µs alone
  (`bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md` §7), and
  re-measured at `v2.0.0-67-g952bbb9` as 1,088/1,083 µs beside a writer on
  core 0/core 1 against 37/35 alone
  (`bench/v2.0.0/results-multicore-writers-v2.0.0-67-g952bbb9.md` §7). `docs/spec/wal.md` §6's
  non-blocking reactor is not what is built; the I/O-backend decision
  (`docs/spec/heap-and-tuple.md` §8) has its first number.

- **An indexed join column made a peer-owned join refuse instead of
  answer — closed 2026-08-18, the same day it widened.** The step
  descriptor refuses to ship any index or Cabin step, and the pipeline's
  inner-step eligibility admitted only `kProbe`/`kScan`/`kFilterScan` —
  so on a multi-core instance, `CREATE INDEX` on the join column of a
  peer-owned relation flipped that join's inner step to `kIndexProbe` and
  the statement from a pipeline run to an affinity `ERR`. Opened by
  equality propagation (`881f69a`: a literal restriction already compiled
  the inner side to an index probe), widened by IX17 (`4f304fd`) to every
  join on an indexed column. **Closed by the ship-time downgrade**
  (`ShippedForm`, `step_descriptor.cpp`): a structure-served
  step ships as the walk it would fall back to anyway — `kScan`, aux
  dropped, residual intact — which cannot change a result by the property
  `step_chain.hpp` states, and restores the pre-`881f69a` behaviour on
  every seam (the single-step open, the pipeline's leaf, and its
  consuming stage). The fix closes more than the entry named: a
  **`kCabinProbe`** on a peer-owned relation had hit the descriptor
  refusal since Cabins landed — long before `881f69a`, never recorded
  here — and ships as its walk by the same route now. What remains open:
  the peer runs the *walk*, not the structure — re-deriving the index or
  Cabin from the peer's own catalog is the recorded improvement, and the
  descriptor's refusal stays as the backstop for any caller that skips
  the sanctioned route.

- **Cross-core execution is two shapes wide, and the second is a join.**
  P4a-P4c (2026-08-10) built the single-relation remote read; **P4d
  completed 2026-08-15** (`docs/inflight/in-progress/workplan-crosscore.md`) and with it a
  **two-step join executes across cores**: the session computes the edge
  at plan time, opens the final stage, and each stage forwards its
  upstream's enclosed open; the leaf streams the forwarded columns under
  credit, the consuming stage joins per input row against its own local
  relation, and the session renders a typed projected reply. Both a
  probe inner (a pk join) and a **walked** inner (a join on a non-pk
  column, bounded by 4c's gated inner walk) ship. Proven equivalent to
  local execution byte for byte over twelve shapes, with the shipping
  itself asserted so the test cannot compare two local runs.
  **Everything else is still served by core 0**: `CheckReadAffinity`
  refuses what the pipeline cannot run, retryably — three or more steps,
  aggregates, sorts, quotas, sub-chains, `emit_in_key_order`, and an
  inner walk that does not reference the outer row (a cross product).
  **P4e closed 2026-08-15.** `bench/results-multicore.md`'s 1.05× stays
  a *parity baseline* and cannot yet become anything else — see the
  writer gap below. The pipeline's own cost is now measured
  (`bench/results-crosscore-pipeline.md`): **2.52 µs per shipped
  statement plus 0.626 µs per forwarded row**, against 0.417 µs per row
  for the same join run locally — so a shipped join runs at 2.50× local
  and 60% of a shipped row's cost is pipeline overhead. That is the
  justification for P4d-4c's per-batch runner handle, which is the main
  remaining piece of the feature.
- **A peer-owned relation has no writer, so cross-core *scaling* cannot
  be demonstrated at all** (named 2026-08-15 while closing P4e). Writes
  to a relation another core owns are refused (CC3), DML statement
  shipping is unbuilt, and ~~core 0 alone carries a listener~~ — **the
  listener half closed 2026-08-24** (PW5, `r1-peer-ddl-refusal`):
  `peer_listeners = on` binds every core's listener with `SO_REUSEPORT`,
  off by default, refused with tls/auth until those can be shared
  immutably. So
  `placement = rotate` produces relations that no connection can
  populate. The pipeline reads them correctly once they contain rows,
  which is why every cross-core test and benchmark builds its rows
  in-process. Reproduce in ten seconds with
  `tools/multicore_benchmark.py --placement rotate`, which probes and
  reports rather than erroring per row. ~~Until one of the three lands,
  every cross-core number in `bench/` is a *cost* measured with the
  parallelism removed, never a speedup.~~ **Retired by measurement
  2026-08-26** (`bench/v2.1.0/results-multicore-writers-v2.1.0.md`, at
  `v2.1.0`): the writer half landed with PW1c, and on the first host with
  three writer cores rotation measures a **speedup, not a cost** — 1.751x
  its own control at one writing session per writer core, with the
  four-core-server artifact (1.067x) subtracted. It is the first measured
  cross-core speedup in `bench/`. Two bounds go with the number and are
  not optional: the gain **only** appears at one session per core (at two
  it is 0.989x of not rotating at all, because spreading N sessions over W
  cores divides the group-commit batch by W), and the host is 4 logical /
  2 physical cores, so the 3x ceiling is bounded by SMT before it is
  bounded by anything architectural.
  **Scoped 2026-08-21, and three of its blockers closed the same day** —
  `docs/inflight/in-progress/workplan-peer-writer.md` owns the series and names what actually
  blocks it, which is not the listener. PW1 (transaction-id leases), PW1b
  (row-id leases) and PW3 (a peer checkpointer) are built; **a peer still
  cannot INSERT**, and the reason is now a probed error rather than a
  prediction: `core 1 may not write page 130`. `MayWrite` allows a peer only
  the pages its own extent lease owns, and CC7's grant is fault rights only
  by an explicit decision, because a grant is extent-granular and a superset
  is safe to fault and not to write. That is **PW1c**, and it is
  **decided 2026-08-24** (`docs/inflight/in-progress/workplan-peer-writer.md` §8): per-relation
  exact-page write rights under the ratified PL-B handoff — CC7's publish
  becomes flush → handoff record → write grant, PL-B's first consumer.
  Owner-side DDL allocation stays rejected with CC7; DML shipping is
  reframed as the session-placement answer, not this one. Unbuilt
  (PW1c-1..5). **And that refusal is Debug-only**: the whole
  `MayFault`/`MayWrite` check sits inside `#ifndef NDEBUG`, so in
  `build-release` the same peer INSERT does not refuse — it dirties core 0's
  page in the peer's own store and the last flush wins. PW1c is therefore a
  silent two-writer corruption route that opens the moment PW5 gives a peer
  a listener, invisible in exactly the build every measurement is taken in,
  which made **PW1c before PW5 an ordering requirement — answered
  2026-08-24 by the interim guard**: a peer dispatcher refuses
  INSERT/UPDATE/DELETE by name (release-mode, beside PW4's DDL guard), so
  a peer listener is read-only until PW1c-4 lands. The original scoping
  note follows:
  a peer cannot issue a **transaction id** at all (`TrxIdSequence`
  constructs spent, and a peer's persist callback refuses), two catalog
  write points ride the ordinary INSERT (a clustered root growing a level,
  a secondary index root splitting), and a peer has no checkpointer. A
  heap relation with no secondary index writes no catalog page, so the
  trx-id lease alone makes that one shape peer-writable. Its PW2 decision
  — how a root move reaches core 0 — is open and listed there.
  **Later the same day (2026-08-24)**: PW1c-1..5 built — a funded peer
  INSERTs end to end — PW2 decided and built as the anchor page (the
  btree shape lifted 2026-08-24; **the indexed shape lifted 2026-08-25** —
  the whole PW1c-6b series built (§7c, "the owner builds"): the owner
  builds a peer-owned relation's index on request in its own stream,
  refusing its own writes to it until core 0's `done` (6b-1/6b-2); core
  0's two-phase `HandleIndex` parks on the build and publishes the
  `sys.indexes` row (6b-3); the shape gate's `indexed` arm lifted so the
  owner maintains it on write (6b-4), with `DROP INDEX` on such a
  relation refused inside a transaction; `docs/spec/ddl-transactional.md`
  §5e and `docs/spec/crosscore.md` CC7's owner-builds exception carry the
  atomic/isolated semantics), and **PW1c-7** closed the restart hole the series had
  named: leases and grants are memory-resident, and the probe found a
  restart loses every page a peer allocated itself, not only its grants;
  the PL-C stamp now carries ownership (`docs/inflight/in-progress/workplan-peer-writer.md`
  §8). What still stands of this entry: ~~PW6's number is unmeasured~~ —
  **measured 2026-08-26 at `v2.1.0`**, and it says rotation does not scale
  writes on the non-interfering-relations shape (1.051x at six relations
  against a 3x ceiling, 0.989x against the same four-core server not
  rotating); the same file validates PW7's scheduler floors at the shape
  that provokes them (0.765x -> 1.081x, two independent pre-floors arms
  agreeing) and passes restart-ownership at three writer cores; a
  peer listener with tls/auth is refused at boot (PW5); and the
  owner-built `CREATE INDEX` carries §5e's two named gaps — a build
  window that could expire before a late core-0 commit (unreachable at
  the shipped 60 s/180 s timeouts; the real close is a bound on the
  commit leg or the cross-core commit oracle), and `SHOW INDEXES` on
  core 0 for a peer relation reading a maintenance-moved root as a
  subtree (diagnostics only; cross-core reads downgrade the probe to a
  scan before shipping, so answers are unaffected).
- **`Catalog::catalog_version()` is not a sound guard for a cached
  `TableAccess`** (named 2026-08-15 while designing P4d-4c's per-batch
  runner handle). `InvalidateFromPeer()` — the `kCatalogInvalidate`
  handler, and the *only* invalidation a peer ever receives — clears
  every cached fact **without bumping the version**, deliberately, since
  that counter is per-instance and means nothing across cores. So
  anything that caches a catalog borrow across a suspension and
  re-validates it with the version counter would be correct on core 0
  and wrong on every peer, with a freed schema as the failure — the
  exact use-after-free P4d-4a fixed by re-Binding unconditionally.
  **Nothing does this today**; it is recorded because the obvious
  optimization of the pipeline's per-row cost wants precisely that
  guard, and its prerequisite is a cache-generation counter every
  invalidation path bumps, `InvalidateFromPeer` included.
- Relation ownership is decided **and built** (CC7 + P6b handoff + P6c
  `placement` key, 2026-08-10): a rotated relation's pages are grantable
  and readable by its owner. `placement` still defaults to `creating`.
  P4d landing (2026-08-15) widened what a rotated relation can serve from
  one shape to two — a star read and a two-step join — but `rotate` still
  places relations on cores that must refuse everything else, so it stays
  an exercise mode until the refused list is short enough to be a
  performance choice rather than a correctness cliff.
  Row-id leasing for peer INSERT is also built (P5-shape, 2026-08-10).
- **REPEATABLE READ is knowingly weakened across cores** (CC4): no
  cross-core ReadView; RR holds per core. Client-facing docs must say so.
- **A comparison whose left side is an *outer* row's column loses its
  type, and one join orientation is refused rather than answered**
  (found at the P4d-4b-3 review, 2026-08-15). `EvaluateAll` takes the
  comparison's `type_val` from the lhs column's schema, and
  `chain_frame.cpp`'s `SchemaFor` answers null for any `up != 0`
  reference — so an outward lhs falls back to `type_val = 0`.
  `CompareValues` reads `type_val` in exactly one arm, `kTypeValUint64`,
  where values above `INT64_MAX` must compare unsigned because
  `int_val` holds them as negatives. So `WHERE a.u > b.u` over a
  `uint64` column answers one way locally and the other way through a
  shipped stage. **Today it is refused, not mis-answered**:
  `BuildTwoStepPipeline` declines a residual whose lhs is the upstream
  row and whose column is `uint64`, and the statement falls through to
  the affinity refusal. The same hole is *accepted* rather than refused
  for correlated sub-chains, where `chain_frame.cpp` documents it in
  place.
  **The real fix, which needs a decision because it reshapes the
  compiled plan**: `StepPredicate` carries its lhs `type_val`, resolved
  at compile exactly as `projection_types` and `SortKey::type_val`
  already are, and `EvaluateAll` stops asking `SchemaFor` at all. That
  closes the sub-chain case too and lets the cross-core refusal be
  deleted. Cost: every site building a `StepPredicate` in
  `step_compiler.cpp` (including the synthesized range bounds), the
  evaluator, and a **`kStepDescriptorVersion` bump** — a versioned wire
  format, so it is not a change to make in passing.
- **`Drain` holds a `Pipeline&` across `send_`** — latent, pre-existing,
  and the one place in `remote_step_service.cpp` that does not follow
  its own re-find-by-tag discipline. A synchronous `send_` that reached
  `OnStepOpen` would `push_back` onto `pipelines_` and invalidate the
  reference under the loop. Unreachable today: `Drain` sends only
  batches and EOF, and neither receiver opens a stage. Left alone
  deliberately — `Drain` carries the reentrancy latch and the
  erase-before-EOF ordering that two ASan-caught bugs produced, and
  refactoring it to chase an unreachable case risks more than it buys.
- ~~**A shipped stage reads with *every writer visible*, not latest
  committed**~~ — found at the P4d-4b-3 review and **closed the same
  day (2026-08-15)**. Every shipped stage used to execute with
  `snapshot=nullptr`, which the executor reads as `kSeesEverything`, so
  a concurrent *uncommitted* INSERT on the owning core was streamed to
  the session — and since 4b-3, joined across two stages.
  `RemoteStepServer` now takes its host's `TransactionManager` and
  mints the autocommit-shaped view itself, once per stage, held by
  value in the coroutine frame so it survives every page-boundary park
  (a `ReadView` is a POD; the undo pointer outlives the reactor). CC4's
  "the owning core's latest committed snapshot" is therefore literal:
  **no view crosses a core**, each stage takes its own, which is the
  same per-core weakening of REPEATABLE READ the entry above records.
  Pinned by `RemoteStepServiceTest.
  AStageReadsAtLatestCommittedAndNotAnInFlightWriter`, verified to fail
  without the wiring (the in-flight row appears).
- Cross-core writes are refused retryably (CC3): a transaction's writes
  bind to one home core. 2PC is an open decision, to be designed from the
  refusal counters — which are **readable from outside the process since
  2026-08-26** (T5 of the statement-shipping pretasks): `SHOW META` prints
  `cross_core_write_refusals`, `cross_core_write_refusal_keys` and a capped
  `cross_core_write_refusal_detail` of `home>target:oid=count`, per core.
  The class the counter cannot see is DDL on a peer, refused by verb before
  any relation is resolved; the two owner-core refusals
  (`RelationWriteRightsPending`, `IndexBuildPending`) are excluded by
  decision, being this core's own writes waiting on a grant rather than
  cross-core writes. `docs/spec/crosscore.md` §6's older claim — that PW5's
  pre-parse guard hid the foreign-write class — was already false when
  PW1c-5 deleted that guard, and is retired in place there.
- ~~**Buffer-pool eviction is built but disarmed**: nothing calls the sweep,
  because `Get()` hands out raw spans safe only while nothing evicts~~ —
  **closed 2026-08-13**: the `PageRef` migration is built (MG01-MG06,
  `docs/workplan-pageref.md`), every `PageStore` accessor returns a pinned
  handle, the base class keeps the raw seam `protected`, and the CLOCK sweep
  is armed on the fault path whenever a frame budget is set
  (`buffer_pool_frames` config key; 0 = unbounded, the default). The gate
  that proved it: the full suite green under `KDS_TEST_FRAME_BUDGET=8` with
  reclaimed frames poisoned 0xEF, and an ASan simulator clean in clean and
  crash modes under the same pressure. That gate caught two real bugs before
  they shipped — the first arming protected the just-faulted frame by one
  usage point, which one multi-lap sweep call could walk down and reclaim,
  and `varheap::Fetch` returned a span whose pin dropped at return, so a row
  with two spilled cells could evict the first value's page while fetching
  the second. **What stays open**: the budget defaults to unbounded until a
  sizing decision picks a number (`docs/spec/eviction.md` EV8's "pool
  undersized" telemetry is the input), and `MaintainFreeReserve`'s
  background trigger still waits on EVT02's bounded pool.

- ~~**A peer cannot enforce an assertion, and does not refuse the write
  either.**~~ — **closed 2026-08-26** (PW1c-6c,
  `docs/inflight/in-progress/workplan-peer-writer.md` §7d,
  `docs/spec/assertion.md` §6.1). Measured first
  (`bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md` Finding 2): a
  shipped write to an assertion-covered, peer-owned relation was **admitted
  unchecked** — a second row in a group under `CHECK COUNT(*) <= 1` — because
  the shape gate asked `enforcer_.AnyOn(oid)` of a registry only core 0's
  mount ever filled. The fix is ownership: the relation's owner builds the
  Bound Cabin from its own lease, own-stamped, with no handoff, holds its
  directory, and enforces every write to it, because the cabin is appended to
  by every such write and only its owner may write its pages. Three residues
  stay open and are named in §7d: (1) a write that passed its admission check
  and then parked can reserve after an adoption in between, so one row can be
  reserved unchecked — `CREATE INDEX` has the identical hole against its
  window, and both want an atomic gate-to-write span; (2) neither `done` leg
  is acknowledged, so a lost `done(committed)` leaves the owner's catalog
  cache stale until the next DDL and a lost `done(aborted)` — a `DROP`'s
  included — leaves it over-enforcing until its next mount; (3) an assertion
  in a file written *before* this change has a cabin core 0 built, which its
  owner may not append to, so the owner refuses that relation's writes by
  name (`NoteUnenforceable`) until the operator drops and re-creates it.

- **A catalog relation's var-heap is outside the range a peer may read.**
  `catalog/well_known.hpp` makes every catalog *heap* page live below
  `kFirstUserPageId` and calls that a correctness requirement, precisely so a
  peer can read the catalog; a **spilled** value does not — it takes its page
  from the general supply — so a peer faulting one is refused `may not fault
  page N`. Found by PW1c-6c, whose mount has to read `sys.assertions`'
  declarations, and closed *for that relation only* by granting the mount read
  rights over exactly the pages the rows name (`exec::AssertionSpillPages`,
  page by page — an extent grant would cover pages the core owns and cost it
  PW1c-7's stamp-claimed write rights, which a test proved). `sys.pattern_defs`
  has the same shape and no peer reader today; a general close would put
  catalog var-heap pages in the reserved low range, which is a format change.

## Storage and key modes

- ~~**An instance cannot exceed 65,280 pages — 510 MiB of data file**~~ —
  **lifted 2026-08-26** (`docs/inflight/in-progress/workplan-multi-free-map.md` FM1-FM5). The
  free map is a family of pages now, one pair per 65,280-id region at
  `N × 65,280 + 1` and `+ 2` (D1's candidate A), created as allocation
  walks into a region and loaded at mount. The four sites that read one
  bitmap page's coverage as the size of the id space — `IsAllocated`,
  `CreateAt`, `RaiseAllocationFloor`, `ExtentAllocator::Reserve` — read
  `kMaxPageCount` instead, so the ceiling is the 2^31-page / 16 TiB design
  ceiling `docs/spec/page.md` §4 always named. A database inside one region is
  unchanged: region 0's bitmaps are still ids 1 and 2, so **no superblock
  version bump and no migration**, and an existing file mounts as it did.
  **FM6-FM11 landed the same day**, so what the first version of this entry
  listed as remaining is mostly closed: the headerless bitmap is built only
  where something is headerless and `IsHeaderless` answers with no lookup at
  all when nothing is (D2(a)); a peer refreshes every resident region, not
  region 0 alone (D5(a)); the grant bitmaps grew per region, so a peer can
  hold a grant anywhere (D10(a)); `allocated_pages()` is maintained rather
  than swept (D8(a)); and `SHOW META` prints `map_regions`,
  `map_pages_resident`, `map_coverage_ids` and `headerless_pages`.
  **What actually remains**: a reservation may not cross a region (D3(a)),
  wasting at most 63 ids per 65,280, permanently, since nothing frees; a peer
  still cannot *durably* record a headerless page in a region it created
  privately, because `FlushMaps` drops a leased store's map writes as it
  always has; mount reads one page per region, which is 32,896 scattered
  reads for a full 16 TiB file and wants a batched `ReadPageRun` before
  anything can produce one; the map stays unlogged (D9 open, RC04 repairs);
  and `docs/spec/page.md` §4/§5's claim that the superblock anchors a "free-map
  root" still contradicts `superblock.hpp`, which holds no such field (D6,
  untouched because no candidate needed one).
- **The consequence of lifting it, now live**: the instance ceiling was
  **the engine's only bound on leaked space**. With nothing freeing pages
  (see reclamation above), a `DROP TABLE`/rebuild loop stopped at 510 MiB
  before 2026-08-26 and now runs to the design ceiling. Not a new defect —
  the same absent reclamation, with a bigger number in front of it.
- ~~**A peer core can hold no fault or write grant above page 65,280**~~ —
  **closed 2026-08-26** by the same day's FM7, as D10(a). `fault_rights_` and
  `write_rights_` are keyed by region now, mirroring the map, each half built
  only when something is granted into that region and neither persisted — so
  there was no format question and no migration. `GrantFaultPages` no longer
  clamps an extent to one region's coverage, and `TryClaimByStamp`'s ceiling
  is the design ceiling. It was open for the length of one commit.
- ~~**Dividing a full btree *internal* node is not implemented**~~ —
  **built 2026-08-11** (`docs/workplan-key-mode.md` PK09). A separator
  promoted into a full parent now divides that node's entries when it sorts
  inside them: the median moves up, its child becomes the new node's
  leftmost, and the lower half is written back. The cheap
  right-split-with-no-movement is kept for the append case it correctly
  serves. Struck rather than deleted because the refusal it replaced was a
  named `OutOfSpace` some reader may still be holding.
- ~~**A heap relation cannot be `EXPLICIT`**~~ — **closed 2026-08-25** by
  the key mode's removal (`docs/spec/heap-and-tuple.md` §4.1). A heap relation
  takes a caller-named key like any other; what it refuses is a key *below
  its high-water mark*, `OutOfRange` at `Catalog::AdmitExplicitRowId`. The
  entry above claimed lifting it was the **heap page split policy**, and
  that was wrong in the same way §9's ascent entry once was: a heap never
  has to place a key that sorts inside a full page, because such a key is
  refused, so the split policy is untouched and stays open on its own
  terms. What is left is the restriction below, not a gap.
- **A heap relation's caller-named keys must ascend.** Not a defect and
  not a policy choice: §3.1b's tail append, its page-wise ordering and its
  tail-page-only duplicate check are all that ascent, and the third is the
  dangerous one — a page opening below an id already on its predecessor
  would admit a duplicate pk with no error at all. `BTREE` is the storage
  that takes keys in any order, and the refusal says so.
- **A `DELETE`d row's primary key cannot be re-supplied.** On a btree
  relation the uniqueness check scans the landing leaf's live slots and a
  delete-marked slot is live until retirement — and nothing retires (see
  reclamation above). On a heap relation the mark never falls, so a
  deleted key is below it and refused for that reason instead. Consistent
  with K1 issue-once either way, and a restriction a caller doing
  delete-then-reinsert will meet.

## SQL surface and protocol

- ~~**No NULL storage**~~ — **closed 2026-08-20** by `docs/spec/null.md`
  (NU1-NU8, `docs/workplan-null.md`): a tail null bitmap sized to the
  relation's *nullable* columns, the bitmap as sole authority with the
  `kNull` tag as defined filler. Columns are **NOT NULL by default** and
  `NULL` is the opt-in (D1 — the deliberate divergence from standard SQL,
  loudly noted in `manual/sql/sql.md`), so every pre-existing relation kept
  a byte-identical row layout and the feature landed with no format bump
  and no migration — the property the old entry predicted from
  `SysColumnRow::notnull` having always existed. What remains true from the
  old entry: Oracle's representation was rejected by name, because omitting
  trailing NULLs makes the row variable-length and retracts invariant 13.
  Still refused, by decision: nullable index keys (D2, covered columns
  included; `IS NULL` answers by scan), `NULLS FIRST/LAST` grammar (D3
  fixed NULLs-largest), and `ALTER TABLE ADD COLUMN` of any kind.
- **A `sys.` qualifier is silently dropped below depth 0.** Found
  2026-08-21 by the review of the catalog-view pagination fix, unowned.
  `HandleSelect` diverts a catalog view only for the top-level `from` and
  `joins`; nothing else in the engine reads `RelationRef::schema` —
  `src/exec/step_compiler.cpp` never mentions it. So
  `SELECT v FROM t WHERE id IN (SELECT oid FROM sys.tables)` drops the
  `sys.`, resolves `tables` to the catalog's own internal relation oid,
  and answers `ERR no columns for this rel_id` — an internal message with
  no byte position. This is the same "parsed, accepted, silently dropped"
  shape the tail had over a view before 2026-08-21, one nesting level
  down. It wants either a parser refusal with the byte (a
  schema-qualified relation below depth 0) or a decision to support it;
  the entry exists so it is not found a third time.
- ~~**Pagination is LIMIT/OFFSET only**~~ — **closed 2026-08-11** by the
  output sort (`docs/workplan-order-by.md`). `ORDER BY` now takes any
  column or columns, pk or not, of any relation in a non-aggregated
  top-level statement, each `ASC` or `DESC`. What remains true of that
  entry: **there are no cursors**, and KWP/1 portal suspension is still
  unbuilt — only the frame codec exists (`docs/spec/protocol.md`).
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
  including under `LIMIT`/`OFFSET`. **Narrowed 2026-08-25**: the second
  conjunct now reads `key_order == kUnordered` — has this relation ever
  taken a key below its mark — rather than a declared mode, so a btree
  relation fed only ascending keys pays nothing where the mode charged it.
- **`IN (value list)`** is unbuilt — the open half of parser workplan V08;
  it currently reports "expected a subquery".
- **Per-transaction durability class** is a KWP/1 protocol field; the text
  protocol offers only the instance-wide `durability` config key.
- ~~**No auth, no TLS, loopback only**~~ — **TLS and authentication both
  closed 2026-08-13**: direct TLS 1.3 at the transport seam
  (`docs/spec/protocol.md` §1, `tls` key) and SCRAM-SHA-256 connection auth
  (`auth = scram` + `users_file`, provisioned with `--add-user`), both
  off by default. What remains true: the port stays **loopback only**.
  **Authorization landed 2026-08-13** — statement-class roles
  (readonly/readwrite/admin per user, enforced at the dispatcher;
  `docs/spec/protocol.md` §14) — with its own recorded limits: no
  per-relation grants (future, catalog-recovery-gated), role changes are
  re-provisioning, and `auth = off` means every session is admin.
  Deferred SCRAM hardening is listed in §14 (channel binding, SASLprep,
  mock-salt consistency, pre-auth deadline).
- **`TcpServer::Detach()` leaks the fd of a connection whose statement is
  in flight** (found 2026-08-13 in review): `CloseClient` only *marks*
  such a connection (`closing`), then `Detach` clears the map without
  `::close` or epoll unregistration — an fd leak and a stale registration
  on the shutdown path, TLS or not. Whether Detach may force-close a
  connection the dispatcher still holds a session pointer into is the
  open question; it needs a decision, not just a fix.
- **`float`** stays refused at `CREATE TABLE`: nothing settled its
  encoding (`docs/rules/rule-fixed-length-tuple.md`).

## Advisory and optimizer structures

- **Waystone retention, decay and epoch-bump sites are unbuilt**
  (P15-P17, `docs/inflight/in-progress/waystone-workplan.md`); trails grow until then.
  One validation gap remains: nothing verifies a page still belongs to the
  relation a trail recorded it from — holds until pages can be reallocated
  between relations (`docs/spec/physical-optimizer.md` §6 gate 3 owns it).
- **`CABIN AUTO` acts only under `cabin_optimizer = on`, default `off`**:
  the controller runs end to end since PHY04 and is observable since
  PHY06 (`SHOW CABIN_OPTIMIZER`, both 2026-08-10), but with the key at
  its default a column declared `auto` still behaves exactly as an
  undeclared one (`docs/spec/physical-optimizer.md` Part II). Its managed
  state and decision log are memory-resident: a restart forgets what the
  controller was managing, and re-observation rebuilds it — the stated
  crash posture, not a bug.
- ~~**A Cabin entry set banked inside a transaction outlives its ROLLBACK,
  and is then served as authoritative**~~ — **closed 2026-08-21**, the day
  after it was found, by `docs/spec/cabin.md` §6a: a recording walk banks
  nothing unless its read view carries no in-flight transaction and belongs
  to none (`view.in_flight_count == 0 && view.own_trx_id == kNoTrxId`, two
  comparisons on a path that already holds both facts).

  The entry is kept because the *shape* of the mistake outlives it. The
  store's header argued the build-by-observation hazard was structurally
  dead, since statements run to completion on the owning core — which is
  true, and answers a **write racing the scan**. What broke was a write the
  scan *could not see*, resolving afterwards: a transaction's uncommitted
  DELETE hides a row that its ROLLBACK restores, and an in-flight
  transaction's INSERT is invisible until it commits. A correctness
  argument can be sound and still be about the wrong hazard.

  Found by the simulation harness's first fault-free SIM06 sweep (seed 2,
  profile `colliding`, mode `clean`), shrunk 1200 ops → 9 by SIM07's
  minimizer, and read off as six statements. Both halves are pinned by
  `SimFindings.ACabinSetBankedInsideARolledBackTransactionServesEveryLiveRow`
  and `…IsNotBankedWhileAnotherTransactionIsInFlight`, with
  `…ACabinSetBankedAfterACommittedDeleteIsCorrect` as the control that says
  what was *not* wrong. The fix rejected: un-observing on rollback, which
  repairs the DELETE half and cannot touch the INSERT half — a transaction
  that commits rows the recorder could not see never rolls back, so there
  is no moment at which to drop the value.

- **The physical optimizer is shadow-only as a finding**
  (`docs/spec/physical-optimizer.md` §6): every candidate move is blocked
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

- `docs/spec/client-manual.md` §5: "exactly one accepted client connection
  served at a time" — stale; many clients are served concurrently,
  cooperatively on one thread (`include/kds/server/tcp_server.hpp`).
- Any doc or task brief claiming **there is no SQL DELETE** or that
  **assertions enforce nothing** predates the transaction work and AST07
  respectively; both are built (verified in
  `src/server/command_dispatcher.cpp`).
