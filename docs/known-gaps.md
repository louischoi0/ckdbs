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
  (`docs/feat-physical-optimizer.md` §6 gate 3). Building the set instead would
  mean walking every page of every relation at every mount. **Substrate built
  2026-08-13**: `docs/page.md` §2a stamps the owning object's oid into the
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
     **decided and fixed 2026-08-12** as AS6b (`docs/feat-assertion.md` §7).
     `flags` bit 3, `kEntryOrphaned`, is set on abort by the live path and by
     `ASSERT_ROLLBACK` replay alike, and the linkage scan skips a marked entry.
     Bit 3 was free — AST04 shipped three flags — so no width moved and an older
     entry reads as "not aborted", which is what it is.

  The **aggregate was correct either way** (snapshot + folded deltas), so
  admission answered right and the constraint enforced correctly. What did not
  hold is the structural proof `docs/feat-assertion.md` §5.2 names — "the entries
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
  that payload — a `docs/wal.md` §4.1 decision, and one that would ride the
  segment-format bump to 2 for free rather than costing a version event of its
  own later. Owned by `docs/feat-assertion.md` §7.

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
  `[OPEN]` (`docs/wal.md` §15).

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
  **decided and fixed 2026-08-12**, `docs/wal.md` §4.1. AS6a's licence was that
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
- ~~**DDL and catalog writes are unlogged**~~ — **closed 2026-08-19**
  (RV3): logged as ordinary records, replayed, and rolled back for
  losers; the durability entry above carries the details and the named
  remainder.
  The *other* half of that old entry — "DDL is not transactional,
  `CREATE TABLE` inside a transaction is not rolled back" — is **false
  for `CREATE TABLE` as of 2026-08-16** (`docs/spec-ddl-transactional.md`,
  `docs/workplan-ddl-transactional.md`, DT1-DT4): a rolled-back create
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
  `spec-ddl-transactional.md` §5a). `CREATE INDEX` is atomic and
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
  (`docs/spec-ddl-transactional.md` §5c). The exposure was real: an
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
- ~~**Assertion enforcement**: the registry/directory is memory-resident, so a
  surviving assertion honestly reports `enforcing=0` until recovery can replay
  the directory~~ — **closed 2026-08-12** (RC07, AS6a). A mount revives each
  surviving declaration from `sys.assertions` (§8.2 keeps `source_text` as the
  canon so the group columns can be recovered by re-parsing it), restores its
  group headers from the last checkpoint's `ASSERT_SNAPSHOT`, relinks the entries
  by scanning the cabin's own pages — bounded by the assertion's entry count, not
  the relation's rows — and folds the `ASSERT_*` records after the snapshot.
  `SHOW ASSERTIONS` reports `enforcing=1` immediately, which is what
  `docs/feat-assertion.md` §7 always claimed and the engine contradicted until
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
delete-mark purge (`spec-ddl-transactional.md` §5d) and the undo purge
(`docs/workplan-undo-purge.md`, the same day: settled pages recycle into
the log's own growth, so this run's chain plateaus). Everything else
still waits on its own gate, so:

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
  (`docs/feat-index.md` §13);
- catalog rows are never reclaimed (the column ceiling is on columns ever
  created); pages, extents and Keystone ids are never reused;
- `DROP TABLE` exists (`docs/spec-drop-table.md`) but is **catalog-scoped**:
  the relation's pages, var-heap chain and index pages orphan — leaked
  space, deliberately, because free-map reuse is gated (a reallocated page
  breaks trail validation, `feat-physical-optimizer.md` §6 gate 3; a
  reader horizon exists now, but that gate is its own). The oid is
  tombstoned in `sys.objects` and never
  reissued, which is what keeps dead-oid advisory structures harmless.
  `ALTER TABLE` is catalog-only renames (`docs/spec-alter.md` AL1). Both
  RESTRICT on assertions; DROP also RESTRICTs on referencing foreign keys.
  Every one of these is an unlogged catalog write like all DDL: a crash
  after it can lose it.

## Concurrency and multicore

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
  completed 2026-08-15** (`docs/workplan-crosscore.md`) and with it a
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
  shipping is unbuilt, and core 0 alone carries a listener — so
  `placement = rotate` produces relations that no connection can
  populate. The pipeline reads them correctly once they contain rows,
  which is why every cross-core test and benchmark builds its rows
  in-process. Reproduce in ten seconds with
  `tools/multicore_benchmark.py --placement rotate`, which probes and
  reports rather than erroring per row. Until one of the three lands,
  every cross-core number in `bench/` is a *cost* measured with the
  parallelism removed, never a speedup.
  **Scoped 2026-08-21** — `docs/workplan-peer-writer.md` owns the series
  (PW1-PW6) and names what actually blocks it, which is not the listener:
  a peer cannot issue a **transaction id** at all (`TrxIdSequence`
  constructs spent, and a peer's persist callback refuses), two catalog
  write points ride the ordinary INSERT (a clustered root growing a level,
  a secondary index root splitting), and a peer has no checkpointer. A
  heap relation with no secondary index writes no catalog page, so the
  trx-id lease alone makes that one shape peer-writable. Its PW2 decision
  — how a root move reaches core 0 — is open and listed there.
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
  refusal counters.
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
  sizing decision picks a number (`docs/spec-eviction.md` EV8's "pool
  undersized" telemetry is the input), and `MaintainFreeReserve`'s
  background trigger still waits on EVT02's bounded pool.

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

- ~~**No NULL storage**~~ — **closed 2026-08-20** by `docs/spec-null.md`
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
- ~~**No auth, no TLS, loopback only**~~ — **TLS and authentication both
  closed 2026-08-13**: direct TLS 1.3 at the transport seam
  (`docs/protocol.md` §1, `tls` key) and SCRAM-SHA-256 connection auth
  (`auth = scram` + `users_file`, provisioned with `--add-user`), both
  off by default. What remains true: the port stays **loopback only**.
  **Authorization landed 2026-08-13** — statement-class roles
  (readonly/readwrite/admin per user, enforced at the dispatcher;
  `docs/protocol.md` §14) — with its own recorded limits: no
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
