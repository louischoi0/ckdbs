# Pending: a whole-engine audit, stopped after 6 of 33 slices - what is fixed, and what waits

Status: **PENDING** - F4 and F26 to F32 wait for the operator; 27 slices are
unread. Decides nothing, opens no stage, licenses no code.
Raised: 2026-11-12, in conversation with the operator.
Read at: `ce5347e6` (`v3.4.8-2-gce5347e6`), on worktree `audit-lean-and-correct`.
Every `path:line` below is at that commit, before any fix.
Related: `docs/inflight/known-gaps.md` (each finder was told to leave out
what it records); `docs/inflight/README.md` (its rule - "verified against
the code, at a named commit, or it does not go in" - is why the open
findings are here and not under `docs/inflight/bugs/`);
`instructions/v3.2.0/workorder-cr-read-committed-rerun.md` (written, not
opened: it re-runs a writer refused on a decided newer version, which is
beside F4 and does not cover it - F4's row is passed over before any
refusal).

**Where the 32 findings stand, at `c6caa100`.** The operator had them taken up
one at a time, in order. Each of F1 to F25 was read again against the code
before anything was changed:

| Findings | Outcome |
|---|---|
| F1, F2, F3, F5 to F15 | fixed - fourteen defects, each with a cell but F13 and F15 |
| F16, F18 to F22, F25 | removed - dead code and one needless guard type |
| F17, F23 | applied in part |
| F4 | found as described, **not fixed**: a change of contract, the operator's to rule on |
| F24 | not applied: the re-read is kept |
| F26 to F32 | **not read again and not applied**: duplication, each a change of its own |

**What bounds that.** Only F1's cell was run against the unfixed tree and
seen to fail; the others pin the fixed behaviour and were not shown to fail
without it. F1 and F2 were reviewed (`critics-developer`); F3 to F25 were
not. The full suite passed 4429 of 4429 at `01a7e905` (F1 and F2); on
`c6caa100` only the suites the change touches were run (346 cells), and the
full suite was not. F26 to F32 remain one reader's `[source-read]` claim
each, unverified, as every finding here was when this entry was written.

## 1. What prompted it

`[operator]` *"Analyze the current project, look for items that are designed
and implemented with excessive complexity, code that is not needed, or logic
that may have a problem, and improve them."* While the audit ran the operator
deferred it: *"Proceed only with the tasks running now and stop for the
moment; I have no tokens"*, then *"for now, leave what was found in a
document"*. Later the same day: *"take what is in the findings now and
handle it one at a time, in order"*, and then *"push what was worked on in
this session to remote main, no verify"*.

## 2. What ran, and what did not

The audit was a two-stage fan-out over `src/` and `include/kds/`
(129,223 lines): 29 slice finders and 4 cross-cutting sweeps, each reading
its slice for logic defects, dead code, needless complexity and duplication,
capped at 6 findings a slice; then a refutation pass per finding. Nothing
was built or run by a finder; every claim is from reading and grep.

- **Baseline.** On worktree `audit-lean-and-correct` at `ce5347e6` the full
  suite (`scripts/test.sh`, `ctest -LE bs-nightly`) passed 4427 of 4427,
  with 2 disabled and 2 skipped cells not run.
- **Finders that finished: 6 of 33** - `dispatcher-1` to `dispatcher-4`
  (`src/server/command_dispatcher.cpp`, whole), `dispatcher-hpp`
  (`include/kds/server/command_dispatcher.hpp`, `session.hpp`,
  `result_sink.hpp`, `read_borrow.hpp`, `statement_epoch.hpp`,
  `transaction_placement.hpp`) and `expeditor`. They returned 34 findings;
  three named the same unread counters, so 32 are listed.
- **Finders that did not run: 27.** `net-session`, `wire-auth-load`,
  `server-runtime`, `superblock-backup`, `parser-1`, `parser-2`,
  `catalog-1`, `catalog-2`, `exec-vm`, `exec-compiler`, `exec-expr-query`,
  `exec-rows-index`, `exec-assertion`, `exec-cabin-transition`,
  `storage-pool`, `storage-trees`, `storage-pages`, `txn-locks`,
  `txn-mvcc-undo`, `wal-write`, `wal-recover`, `stats-rest`, `sched-base`,
  and the four sweeps (`sweep-dead-upper`, `sweep-dead-lower`,
  `sweep-vestigial`, `sweep-config-hygiene`). **Nothing outside the
  dispatcher and the expeditor has been read by this audit**; the absence of
  a finding for the parser, catalog, executor, storage, transactions, WAL,
  statistics or scheduler says nothing about them.
- **The audit's own refutation pass never ran.** The severity and
  confidence on each entry are the finder's own; what stands in its place
  for F1 to F25 is the second reading each got before it was taken up (the
  table at the top).
- **The audit itself changed no source file.** The fixes came after, one
  finding at a time, at the commits section 3 names.

What each finished finder says it read, in its own words:

- `expeditor`: Read in full, on worktree `audit-lean-and-correct` at `ce5347e6`: include/kds/server/expeditor.hpp (974 lines) and src/server/expeditor.cpp (2385 lines) - Config/ApplyFile, Open, Start, StopStartedCores, RunUntilStopped, ~Expeditor, ServeRuntime. Read the callees needed to judge them: CoreRuntime::Open/Run/ShutdownCheckpoint/HostHandedConnections/AcceptMoves (core_runtime.cpp 262-579), CheckpointGate and Checkpointer::RunGated, WalManager StopKicking/SetWakeRegistry, DataSyncer::Start/Stop, Scheduler::Run/Stop/SubmitEvery, TcpServer::Listen/Host/Configure/ClientSetup, SuperBlockCheckpointAnchor::EncodeLanding, TransactionManager's destructor, BackupControl::Wiring, WalArchiver::Wiring, ConfigFile getters, the dispatcher's fault-net deadline sites, and docs/inflight/known-gaps.md's expeditor entries (the cabin view latch across a build, the -j8 Start flake and the SO_REUSEPORT test port are known and not reported). Nothing was built or run; every finding is from reading plus grep over src include tests sim fuzz tools. Not traced to the bottom: RecoverCoreAtMount, ResumeAssertionsAfterRecovery, PageReclaimer, the WAL writer's kick path and TcpServer::Detach's treatment of parked statements - the teardown and mount orderings were checked against their headers' stated contracts only. Checked and found sound, so not reported: member declaration/destruction order against the writer, archiver and data-sync threads; the sticky stop flag against a peer STOP that lands before core 0 enters its reactor; the positional aggregates (StatementLimits, CabinLimits, both Wiring structs); KnownConfigKeys against the keys ApplyFile reads; the D19 fallback's `handoff_` dereference at one core (Unsupported is only raised with reuse_port set).
- `dispatcher-1`: Read src/server/command_dispatcher.cpp lines 1-3305 in full (helpers, error surface, AwaitWriteBlock/AwaitRelationLock/AwaitStatementWaits, DispatchAsync, StageStatement, deferred commits, Dispatch/DispatchInternal, RequiredRole, DispatchInner, HandleBackup, HandleSync, MetaSections, SET handlers, HandleCreateTable, SuspensionHold, BuildRun/IndexBuildRun/DeclarationBuildRun, HandleIndex, HandleCabin/RunCabinDdl, head of HandleAlter). Read callees outside the slice only as far as needed to verify claims: RefuseParkedWrite, NoteBlockingWriter, BorrowRelationForDdl, BorrowChain, TakeLockWait, EndWrite's tail, InDdlStatement/FinishDdlStatement, EndDdlScopeById, HandleSelect head, TryQueryLayer, LockTable::IntentionWriters/DropWake, Session::Poison/AdmittedWhileFailed, Catalog::CreateTable head, kwp_session.cpp statement text path, tcp_server.cpp submit path, stats/access_stats, StatementKindOf. Header: DispatchOutcome and the members my slice touches, not the whole 2859 lines. Checked and dismissed as not bugs: wake leak on AwaitWriteBlock's early exits (RefuseParkedWrite drops it), the thread-local structural-refusal note across suspensions (no interleaving reaches StructuralRerunDue with a foreign note and a live deadline), ANALYZE's DB-time line under DispatchAsync (analyze runs inline), ANALYZE/WITH as a readonly escalation route (ParseQuery reads queries only), Drain's wake re-registration. Nothing built or run; all claims are from reading on worktree audit-lean-and-correct at ce5347e6.
- `dispatcher-hpp`: In the worktree audit-lean-and-correct at ce5347e6 (read-only; nothing built or run). Read in full: include/kds/server/command_dispatcher.hpp (all 2859 lines), session.hpp, result_sink.hpp, read_borrow.hpp, statement_epoch.hpp, transaction_placement.hpp. Every declared method, data member, setter and test seam of CommandDispatcher was grep-checked for definitions and callers across src, include, tests, sim, fuzz, tools. In src/server/command_dispatcher.cpp I read the code behind the header's contracts: the dispatch and wait machinery (lines 270-1250, 1296-1720), StatementEpochScope/StageStatement, the write scope, borrow and conflict functions (11060-11900), the DDL scope functions (7520-7910), SuspensionHold/BuildRun/SelectRun (2408-2640, 5800-6100), the HandleInsert/HandleUpdate/HandleDelete/HandlePurge heads, WithReturning, ResolveForeignKeyParents, the index/spill logging helpers and LogInsert (5296-5500), HandleBegin/CommitLocal/RollbackLocal, DdlBuildLock. Not read: the bodies of InsertOneRow, UpdateParsed and DeleteInner beyond the excerpts cited, HandleIndex/HandleAssertion/HandleTransition/HandleAlter/HandleDropTable, HandleSelect/SelectBody, sys_views.cpp, kwp_session.cpp, tcp_server.cpp (only grepped). Candidates checked and dropped as used or intended: TransactionPlacer (a test implements it), StatementEpochs::shared and Session::durability()/txn_durability() (test-only one-line accessors), the bare CREATE TABLE handler (a deliberate refusal), the manager-less own_txn log path (tests run on it; the header records it as a known gap), PurgeWait::tuple_key (read by the refusal message). Seen and not reported: BorrowChain's `blocker = nullptr` default is dereferenced on a refusal (all three callers pass one); ASCII case-insensitive equality is defined ten times across the tree, one copy in session.hpp.
- `dispatcher-3`: Read src/server/command_dispatcher.cpp 6587-9907 in full (InsertOnConflict, RowHoldsUniqueKey, InsertOneRow, WalkBtreeLeaves/VisitRelation, TupleKeyLiteral/EqualityTarget, the DDL-scope family, ViewFor, SweepSettledDeleteMarks, EndDdlScopeById, RowLocatorForRollback, LocateByTupleKey, HandleShow, HandleCatalogView, RunAggregated, RunAnalyze, HandleSelect, SelectBody, Record*, NoteCabinWrite, HandleUpdate, RunScalarSubquery, LayerHostImpl, WalkForLayer, WithReturning, RunLayerQuery, TryQueryLayer, UpdateInner, head of UpdateParsed). Outside the slice I read only what the findings needed: InsertParsed 6240-6436, FinishWrite, EndWrite/AbortOwnedScope, SelectRun, UpdateParsed's arm/point/walk paths (10574-10725), exec/index_maintain.cpp unique-key code, Catalog::AdoptRoot, stats/access_stats.*, functions.hpp, plan_key/plan_stats headers, and the relevant spec lines (index.md IX6, types.md 3.2b, parser-v2.md 228-231, bulkinsert.md BI9). Nothing was built or run; all findings are from reading. Checked and judged clean or not worth a slot: RecordPlanStats mask bounds (key.n <= 4), NoteCabinWrite's previous indexing (UPDATE passes first_col_pos 0), EndDdlScopeById's null/LSN handling (production always has a WAL), bare "ERR" refusals without a Status (stated as accepted in DispatchOutcome's header), RETURNING typed after the write (spec'd). Smaller items not reported: VisitRelation is a one-arm wrapper over WalkBtreeLeaves since BZ (single caller; could be folded, ~15 lines); WalkBtreeLeaves rebuilds a std::function per leaf; InsertOneRow's omitted-key path returns on an encode failure without give_back() while the named-key path gives the borrow back (held only until the forced rollback); RowHoldsUniqueKey folds a failed ReadTuple/EncodeUniqueKey into "false". Did not read the callees in exec/ beyond the pieces named above (CheckUniqueKeyLatest, UniqueKeyHolds, BorrowOrWait bodies were taken at their declared contracts).
- `dispatcher-4`: Read src/server/command_dispatcher.cpp lines 9834-13031 in full (UpdateParsed, HandleBegin/Commit/Rollback, CommitLocal/RollbackLocal, HandleUse/SetIsolation/SetDurability/SetPriority, SnapshotFor, BeginWrite, RefuseParkedWrite, NoteBlockingWriter, BorrowRelationForDdl, BorrowChain, FoldTupleKeyWindow, DeclaredWriteBorrow, BorrowOrWait, the unique-key helpers, CheckWriteConflictBlocking, EndWrite, AbortOwnedScope, HandleDelete, PurgeConjunctRefusal, VisitKeyedSlotsFrom, HandleWaystone, HandlePurge, HandlePurgeDroppedColumns, DeleteInner). Consulted for contracts: command_dispatcher.hpp declarations, TupleKeyLiteral/NamesTupleKey/TupleKeyEqualityTarget, ResolveForeignKeyParents, CheckView, HandleUpdate, ErrorReply/StatusFromErrorReply, txn/visibility.hpp, lock_table.hpp/.cpp TryAcquire, heap_page.cpp OverwriteTuple/DeleteMark, Catalog::SetPatternWaystone, step_compiler ResolveColumn, parser ParsePurge/condition parse, docs/spec/txn.md §4.4 and §5, docs/spec/eviction.md §3.3, heap-and-tuple.md PU10, known-gaps.md (grepped for each finding). Nothing was built or run, so every bug is from reading only. Not traced to the bottom: VisitRelation/WalkCursor internals, CheckNoChildrenBeforeDelete, exec::PurgeKey/CleanDroppedCells, DispatchAsync's consumption of the wait fields, and the query-layer/RETURNING wrappers around these handlers.
- `dispatcher-2`: Read src/server/command_dispatcher.cpp lines 3250-6660 in full (HandleAlter's rename tail, ADD/DROP COLUMN, DROP TABLE, namespace, assertion and transition DDL, the foreign-key checks, the INSERT normalizers, ColumnRowOf, CREATE TABLE, the insert/split logging helpers, FinishWrite, the describe helpers, SelectRun, InsertParsed, NamedKeyVerdict/TupleKeyPlaced, ResolveConflictArbiter, BindExcluded and InsertOnConflict's head). For context I also read InDdlStatement/FinishDdlStatement/DdlScopeFor (7521-7621), BorrowRelationForDdl, BorrowChain's first-intention check and BorrowOrWait (11206-11560), InsertOneRow's head and logging tail, WalkForLayer/WithReturning's head, ErrorReply/StatusFromErrorReply, the statement head's Revalidate (960-980), CREATE INDEX's post-grant Revalidate (3020-3046), Catalog::DropColumn/AddColumnPosition/AddColumn/CreateForeignKey/Revalidate/BumpVersion, Schema::FindColumn, Parser::ParseValue, kwp_load_server.cpp's chunk path, RelationStats::Record, and tests/ddl_fence_rig_test.cpp's uncontended-grant cells. Nothing was built or run: finding 1's race is traced through the code and the existing rig's precedent, not executed. Not read: InsertOnConflict's body past 6660 and SelectBody (outside the slice). Checked and dropped as not real: a decimal literal reaching KeystoneIdOf in the DO NOTHING pre-pass (a bare `1.5` parses as kStr, int_val 0); the borrowed `ta` going stale across INSERT ... SELECT (nothing on that path invalidates the memo); lost refusal codes on bare `ERR` replies (StatusFromErrorReply recovers the spelled ones); sys.fkeys rows surviving a rollback (stated in ddl-transactional.md).

## 3. The findings

Grouped by kind, then by the finder's own severity. The `F` numbers are this
document's. A finding that is fixed or removed keeps its heading and one
line; its text as the finder wrote it is at `f7c312cf`.

### 3.1 Logic defects (15)

#### F1. A mid-walk parked UPDATE/DELETE whose resume is refused at the archive gate strands its open scope on the session

**Fixed** at `3402c9bb` and, after its review, `01a7e905`: `AwaitStatementWaits` ends a park still on the session once no wait is pending. Pinned by `MidWalkWaitTest.AResumeRefusedAtTheArchiveGateEndsTheParkedScope`, which was run against the unfixed tree and failed there.

#### F2. ON CONFLICT on a unique index compares the 32-byte encoded key prefix, not the key values

**Fixed** at `0bcad837`: `exec::SameUniqueKey` compares key values at the three sites that compared encodings. Pinned by `DmlFormsTest.OnConflictTellsApartStringKeysThatShareTheEncodedPrefix`. Its review found the commit correct and the finding's race arm unreachable - the row's key unit, hashed from the encoding, is held across the `U` wait - so the change at `RowHoldsUniqueKey` and at UPDATE's cleared-key guard is hardening, and only the one-row-met-twice refusal was a reachable defect.

#### F3. Autocommit UPDATE/DELETE leaks its owned transaction when SnapshotFor fails (reader-lease slots full)

**Fixed** at `c6caa100`: the refusal goes through `FinishWrite`. Pinned by `MidWalkWaitTest.AWriteRefusedAtItsSnapshotLeavesNoTransactionBehind`.

#### F4. UPDATE/DELETE judge the WHERE on an invisible writer's head bytes and skip the row with no conflict (dirty read)

**Read again and found as described; not fixed - the operator's to rule on.** `apply` and `qualifies` judge the WHERE on the page's current bytes whenever `Classify` answers anything but `kNoVersion`, so a row whose head an undecided writer changed is skipped before the conflict check. `tests/lock_family_test.cpp`'s `MidWalkWaitTest` fixture comment already states it ("`apply` evaluates the WHERE against the page's current bytes, not against the version the view would resolve"); `txn.md` §5 gives the verdict for a row the writer reaches and does not say which version selects the rows. A fix turns a statement that answers `UPDATED 0` today into one that waits or is refused, which is a change of contract, not a repair.


`src/server/command_dispatcher.cpp:10163` - finder's severity **high**, finder's confidence **medium**, slice `dispatcher-4`

Symbols: `CommandDispatcher::UpdateParsed`, `CommandDispatcher::DeleteInner`, `txn::Classify`, `txn::ResolveThroughUndo`, `CheckWriteConflictBlocking`

**Claim.** `apply` (UPDATE) and `qualifies` (DELETE) skip only `kNoVersion`; a `kNeedsUndoWalk` row (head written by an undecided or later-committed overwriter) is decoded from the page's own bytes and the WHERE is evaluated on them. If that head fails the WHERE the row is passed over before the borrow and `CheckWriteConflictBlocking`, although the version the statement's snapshot sees matches. The comment at 10107-10112 says the conflict check 'is what rejects it', but the check is never reached. txn.md §5's last table row (conflict when `cur` is invisible) is therefore not applied to a row the snapshot wanted.

**Evidence as the finder read it.** 10113: only `== Visibility::kNoVersion` returns. 10129: `payload_copy.assign(tuple.value().payload...)` (head bytes). 10144-10163: DecodeAndResolve(payload_copy) -> EvaluateConjuncts -> `if (!matched.value()) return Status::OK();` ahead of the borrow at 10190 and the conflict check at 10194. Same shape in DELETE: 12742, 12745, 12768-12771. visibility.hpp:56-58 says a kNeedsUndoWalk caller must resolve through undo. No test pins the case (tests/txn_session_test.cpp conflicts all use an unfiltered UPDATE/DELETE).

**Failure scenario.** t(id, status, bal) holds (5,'active',100). S1: BEGIN; UPDATE t SET status='frozen' WHERE id=5 (undecided). S2 autocommit: UPDATE t SET bal=bal+1 WHERE status='active' -> row 5 classifies kNeedsUndoWalk, head 'frozen' fails the WHERE, row skipped; S2 answers UPDATED 0 at once and commits. S1: ROLLBACK. Row 5 is 'active' with bal 100: a committed UPDATE skipped a row that matched in every committed state and in its own snapshot (a SELECT with the same WHERE in S2 returned it). Same for DELETE ... WHERE status='active', and for a compare-and-set `WHERE id=5 AND bal=100` (two conjuncts, scan path).

**Proposed fix.** Keep Classify's answer. When it is kNeedsUndoWalk and the head fails the WHERE, resolve the snapshot's version from `payload_copy` with `txn::ResolveThroughUndo`, decode it into the frame and evaluate the conjuncts again; no version or no match -> skip as today, match -> fall through to the existing borrow + `CheckWriteConflictBlocking`, which waits on or refuses the invisible writer (AO-S3b park). Apply at both sites (and so to both collection passes). Add a two-session test for UPDATE and DELETE with rollback and with commit of the first writer.

#### F5. A statement whose first keyword is followed by a newline is refused as an unknown command

**Fixed** at `c6caa100`: `SplitFirstToken` ends its token at any `isspace` byte. Pinned by `CommandDispatcherTest.AVerbFollowedByANewlineRoutesAsTheVerb`.

#### F6. ADD/DROP COLUMN act on a stale memo and a stale name after an uncontended X grant

**Fixed** at `c6caa100`: `AlterColumnRelation` revalidates the memo and resolves the name again under the `X`. Pinned by `DdlFenceRigTest.ADropColumnReadsTheRelationAsItIsUnderItsGrant` and `ADropColumnOfANameThatMovedBeforeItsGrantIsRefused`.

#### F7. NOW() is not one value per statement in ON CONFLICT DO UPDATE and in RETURNING

**Fixed** at `c6caa100`: `HandleInsert` and `ExecuteInsert` take one context and thread it; `WithReturning` takes the writing statement's. Pinned by `DmlFormsTest.NowIsOneInstantForAWriteItsReturningAndEveryConflictArm`, on the wall clock.

#### F8. PURGE ignores a column qualifier: `PURGE FROM a WHERE b.id = 5` frees a's key 5

**Fixed** at `c6caa100`: `HandlePurge` refuses a conjunct qualified by a name other than the relation's, `InvalidArgument` at the qualifier's byte. Two rows added to `PurgeKeyTest.EveryRefusalCarriesItsCodeAndByte`.

#### F9. The synchronous path's failed commit sync answers ERR with DispatchOutcome::status left OK

**Fixed** at `c6caa100`: the outcome carries the `Status`, and `KwpLoadServer::HandleLoadEnd` reads it. Pinned by `LockDeadlockTest.AGroupCommitWhoseInlineSyncFailsCarriesItsCodeBesideItsLine`.

#### F10. An empty KWP load chunk is answered and counted as one inserted row

**Fixed** at `c6caa100`: `InsertParsed` answers `rows=0` for a statement with no row to place. Pinned by `DmlFormsTest.AnInsertCarryingNoRowPlacesAndCountsNone`.

#### F11. ON CONFLICT (<unknown column>) subtracts a null pointer and matches the result against index columns

**Fixed** at `c6caa100`: the position is computed only for a column that exists; the refusal's code and text are unchanged. `DmlFormsTest.OnConflictRefusesWhatItDoesNotDo` now names the message, with a unique index present.

#### F12. RETURNING without a transaction manager is refused after the write is placed and committed

**Fixed** at `c6caa100`: INSERT, UPDATE and DELETE ask `ReturningWithoutTransaction` before any row work. Pinned by `CommandDispatcherTest.ReturningWithoutATransactionManagerIsRefusedBeforeTheWrite`.

#### F13. CommitLocal's failed-commit arm re-implements RollbackLocal, drops AbortTxn's Status and loses the commit failure's code on a double failure

**Fixed** at `c6caa100`: the arm calls `RollbackLocal`, and a double failure keeps the commit's code. **No cell**: nothing in the suite injects a commit failure followed by an abort failure.

#### F14. Four interval keys and the lock fault net wrap their unit conversion unchecked

**Fixed** at `c6caa100`: one helper, `IntervalNs`, bounds all six interval keys at half the nanosecond clock's range, so neither the product nor `now + interval` wraps. That bound is new for `txn_lifetime_ceiling_ms` too, which refused only a wrapping product before. Pinned by `ExpeditorConfigTest.AnIntervalTheNanosecondClockCannotHoldIsRefusedForEveryKey`.

#### F15. A failed Start() leaves the data-sync thread running with its abort handler installed

**Fixed** at `c6caa100`: `DropOnFailure` stops the data-sync thread and clears its handler. **No cell**: `DataSyncer` exposes nothing a test can ask whether its thread runs.

### 3.2 Dead code (6)

#### F16. UPDATE's appended_spills / NoteSpills / LogSpills block is dead since spills are noted and logged at their append; NoteSpills is left with one caller

**Removed** at `c6caa100`.

#### F17. Unused accessors, two members that are locals, a peer field nobody reads, always-true guards

**In part** at `c6caa100`: `checkpoint_stats()`, `log()`, `reclaim_gate_` and `instance_stop_` as a member are gone. Left as they are: `core_config.cabin_limits`' assignment and the three always-true guards (`wal_ != nullptr`, `txn_manager_.has_value()`), which cost nothing and were not worth a line of risk each.


`include/kds/server/expeditor.hpp:657` - finder's severity **low**, finder's confidence **high**, slice `expeditor`, estimated net lines removed 25

Symbols: `Expeditor::checkpoint_stats`, `Expeditor::log`, `Expeditor::reclaim_gate_`, `Expeditor::instance_stop_`, `CoreRuntime::Config::cabin_limits`, `Expeditor::Sync`, `Expeditor::ReclaimStep`

**Claim.** (1) `Expeditor::checkpoint_stats()` and `Expeditor::log()` have no caller anywhere. (2) `reclaim_gate_` is written and read only inside Open and is a copy of `recovery_.scan_end`. (3) `instance_stop_` is written and read only inside Start's cores>1 block, then left holding a pointer to a destroyed scheduler. (4) `core_config.cabin_limits = config_.CabinLimitsOf()` is never read: a peer reads the field only when `cabins_store` is null, and the Expeditor hands the store whenever `cabins` is on. (5) `wal_ != nullptr` in Sync() and ReclaimStep() and `txn_manager_.has_value()` in the writeback tick are always true after Open.

**Evidence as the finder read it.** `grep -rnF 'checkpoint_stats' src include tests sim fuzz tools` -> expeditor.hpp:657 only. `grep -rnF 'log()' src/server include/kds/server` -> expeditor.hpp:713 only; the `.log()` hits in tests are SimWakerTable's. `grep -rn reclaim_gate_ src include` -> expeditor.cpp:1043 (write), 1317 (read), hpp:933. `grep -rn instance_stop_ src/server/expeditor.cpp` -> 1874 (write), 1971 (read). core_runtime.cpp:264-267 `cabins_ = config.cabins_store; if (config.cabins && cabins_ == nullptr) cabin_store_.emplace(config.cabin_limits);` against expeditor.cpp:1954 `cabins_store = cabin_store_ ? &*cabin_store_ : nullptr` and :971 (store built iff config_.cabins); core_runtime.hpp:122-124 says the field is a fixture's. Open returns an error before `wal_` and `txn_manager_` can be unset (923, 1182).

**Proposed fix.** Delete the two accessors; pass `expeditor->recovery_.scan_end` to CollectAtMount and drop `reclaim_gate_` (keep its comment at the call); make `instance_stop_` a local `std::function<void()>` in Start's cores>1 block; delete expeditor.cpp:1922 and its half of the comment at 1917 (CabinLimitsOf keeps its one caller at 973); drop the three guards. `core_config.cabins` must stay: with `cabins = off` a peer left at the default would build a store of its own. No behaviour changes. Covered by tests/expeditor_test.cpp (TwoCoresComeUpOnOneLogAndEachHoldsTheVolumesOwnImage, TheSharedStructuresAreArmedAboveOneCoreOnly) and tests/drop_table_reclaim_test.cpp.

#### F18. AccessStatsCounters are incremented on every SELECT and read by nothing

**Removed** at `c6caa100`. `access_stats.hpp` now says a failed write is dropped and counted nowhere.

#### F19. kWritePathEnforcesAssertions is a constant true with one use that prints 1

**Removed** at `c6caa100`.

#### F20. The bare CREATE TABLE handler opens a DDL scope for a create that can never succeed

**Removed** at `c6caa100`: the handler answers `CheckKeystoneColumn`'s refusal directly.

#### F21. CommandDispatcher::LogFullPageImage has no caller

**Removed** at `c6caa100`.

### 3.3 Needless complexity (4)

#### F22. InsertOneRow / InsertOnConflict take a 'refreshable' TableAccess*& that is never refreshed

**Removed** at `c6caa100`: both functions take `const catalog::TableAccess&`.

#### F23. BorrowChain defaults `blocker` to nullptr but dereferences it unconditionally on a refusal

**In part** at `c6caa100`: the default is removed, so no caller can pass null, and the declaration names the memo check's `TxnConflict`. The parameter stays a pointer.


`src/server/command_dispatcher.cpp:11312` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-4`

Symbols: `CommandDispatcher::BorrowChain`, `CommandDispatcher::BorrowOrWait`, `DispatchOutcome::LockWait`

**Claim.** The declaration gives `std::uint64_t* blocker = nullptr`, yet both refusal arms build the `LockWait` from `*blocker` with no null test. A caller that used the default would crash the first time its relation intention or unit is refused while `may_park_` is set. All three callers pass a pointer, so the default is an option nobody selects and one the body cannot honour. The header comment also says 'the only non-OK status is the cap's', while the memo check returns `TxnConflict`.

**Evidence as the finder read it.** hpp 1136-1138: `StatusOr<bool> BorrowChain(const WriteScope&, const txn::LockKey&, std::uint64_t* blocker = nullptr, txn::LockMode mode = kExclusive);`. cpp 11312 `DispatchOutcome::LockWait{wake.key, *blocker, ...}` and 11369 `LockWait{unit_wake.key, *blocker, ...}`; 11342 returns Status::TxnConflict. grep 'BorrowChain(' in src/include/tests/sim: callers at 10190, 11501, 12826 only, each passing `&blocker`.

**Proposed fix.** Make the parameter required and non-null: `std::uint64_t& blocker` (callers pass `blocker`), forwarding `&blocker` to `TryAcquire`; drop the default. Amend the declaration's sentence to name the memo check's `TxnConflict` beside the cap. No behaviour change for any existing caller. Covered by tests/lock_family_test.cpp, tests/row_wait_wake_rig_test.cpp, tests/fk_parent_hold_test.cpp.

#### F24. DELETE re-reads the slot and heap-allocates a copy of the row per deleted row only to restamp its header

**Not applied.** `mark` fetches the page again before it writes, and the re-read is what keeps the write right if the row's cells were rewritten in place between the qualify and the mark - `PURGE DROPPED COLUMNS` does that at `kNoTxnId`. Whether the leaf hold is continuous across that span was not established, and four lines are not worth finding out by breaking it.


`src/server/command_dispatcher.cpp:12922` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-4`, estimated net lines removed 4

Symbols: `CommandDispatcher::DeleteInner`, `heap::PageView::OverwriteTuple`, `heap::PageView::DeleteMark`

**Claim.** `mark` calls `fresh.ReadTuple(slot)`, copies the payload into a new `std::vector` (`same`) and passes it to `OverwriteTuple` solely to set `trx_id`/`undo_ptr`. The statement already owns those bytes in `payload_copy`, filled from this slot by `qualifies` under the same exclusive leaf hold, and nothing in between writes either - the argument UPDATE states at 10427-10433 when it uses `payload_copy` as its before-image. The re-read adds an error path and one allocation plus one row copy per deleted row on a bulk DELETE.

**Evidence as the finder read it.** 12745 `payload_copy.assign(tuple.value().payload...)`; 12922-12926 `auto reread = fresh.ReadTuple(slot); ... const std::vector<std::byte> same(...); fresh.OverwriteTuple(slot, same, new_trx_id, new_undo_ptr)`. Between them only DecodeAndResolve (reads payload_copy), the borrow/conflict check, CheckTransitionDelete, CheckNoChildrenBeforeDelete, ReserveDelete and AppendUndo run; none takes payload_copy. heap_page.cpp:269-299 OverwriteTuple memcpy's header + payload of unchanged length.

**Proposed fix.** Delete the `reread`/`same` lines and call `fresh.OverwriteTuple(slot, payload_copy, new_trx_id, new_undo_ptr)`. Same bytes written, same statuses from OverwriteTuple/DeleteMark, same WAL record. Covered by tests/txn_session_test.cpp (DeleteMarksRatherThanRemoves..., RollbackClearsADeleteMark, TwoDeletersOnOneRowConflict) and tests/lock_family_test.cpp.

#### F25. ClearReactorBorrows is a guard type for one line ~ServeRuntime can run

**Removed** at `c6caa100`: `~ServeRuntime` withdraws the view.

### 3.4 Duplication (7)

#### F26. UPDATE's and DELETE's row walk are two hand-kept copies (qualify, claim, collection pass, park epilogue, Handle* wrapper)

`src/server/command_dispatcher.cpp:12736` - finder's severity **medium**, finder's confidence **medium**, slice `dispatcher-4`, estimated net lines removed 90

Symbols: `CommandDispatcher::UpdateParsed`, `CommandDispatcher::DeleteInner`, `CommandDispatcher::HandleUpdate`, `CommandDispatcher::HandleDelete`

**Claim.** DeleteInner repeats UpdateParsed block for block, with comments that only point back ('as UPDATE's site states'). Five pairs: (1) stage-1 qualify - ReadTuple, Classify, payload copy, key, targets test, masked decode, CheckConjuncts once, EvaluateConjuncts; (2) per-row borrow + CheckWriteConflictBlocking + the `blocker != 0 && blocking_writer_ == blocker` park test; (3) the subquery collection pass; (4) the parked-on-row epilogue; (5) HandleUpdate/HandleDelete's resume-or-begin and park-save wrapper. The visibility bug above has to be fixed in both copies, which is the cost the `apply` comment at 10091-10095 itself names.

**Evidence as the finder read it.** (1) 10097-10168 vs 12736-12773. (2) 10176-10214 vs 12822-12840. (3) 10641-10656 vs 12983-12998. (4) 10703-10717 vs 13017-13026. (5) 9253-9320 vs 11862-11903, differing only in `is_delete`, the carried subquery answers, the inner call and the RelationWrite kind.

**Proposed fix.** Lift DELETE's `qualifies` into one private helper over a small struct of the statement's scratch (ta, snapshot, predicates, schemas, frame, payload_copy, frame_spills, filter_mask, conjuncts_checked, targets) returning `StatusOr<optional<{id, trx_id, undo_ptr}>>`, and call it from UPDATE's `apply`. Add `ClaimRowForWrite(scope, ta, id, trx_id, declared, mode) -> StatusOr<bool /*parked*/>` for pair 2, `CollectWriteTargets(ta, qualify)` for pair 3, `ParkedOrVerdict(scope, cursor, rows, verdict)` for pair 4, and one `RunParkableWrite(line, session, is_delete, inner, kind)` for pair 5. Same order of checks, same statuses and messages, same lock asks and modes (mode stays a parameter: U for UPDATE, X for DELETE).

#### F27. The refusal outcome literal is written out 178 times, with the same local lambda redefined five times

`src/server/command_dispatcher.cpp:1634` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-1`, estimated net lines removed 12

Symbols: `ErrorReply`, `DispatchOutcome`, `CommandDispatcher::HandleBackup`, `CommandDispatcher::DispatchInternal`

**Claim.** `{ErrorReply(s), false, 0, s}` (response, should_stop, rows_affected, status) is spelled positionally at 178 sites in this file, 30 of them in lines 1-3300, and five handlers each define an identical `refuse` lambda for it. The positional form is how sites drift: DispatchInternal's durability refusal (line 1242) builds `{ErrorReply(s), should_stop}` and leaves status OK, where DispatchAsync's twin (line 921) carries it.

**Evidence as the finder read it.** grep -c "{ErrorReply(.*), false, 0, " src/server/command_dispatcher.cpp = 178. Identical lambdas `[](const Status& s) { return DispatchOutcome{ErrorReply(s), false, 0, s}; }` at lines 1634, 10975, 12025, 12295, 12488 (9633 is the same over two lines). In-slice sites include 1370, 1384, 1390, 1711, 1718, 2297, 2374, 2387, 2779, 2885, 2902, 2925, 2953, 2964, 3034, 3070, 3099, 3114, 3126, 3141, 3151, 3178, 3194, 3203, 3223, 3225, 3254, 3265, 3293.

**Proposed fix.** One file-scope helper beside ErrorReply: `DispatchOutcome Refused(const Status& s) { return {ErrorReply(s), false, 0, s}; }`. Replace the six local lambdas with it and convert the literal sites mechanically (`return Refused(x.status());`). Same response bytes, same status, same rows_affected at every site, so no refusal code or byte position moves. Leave line 1242 as it is in this patch: giving it the status is a behaviour change to report separately. Covered by the whole dispatcher suite, tests/command_dispatcher_test.cpp and tests/kwp_session_test.cpp first.

#### F28. InsertParsed spells the row-cap and manager-less multi-row refusals twice

`src/server/command_dispatcher.cpp:6277` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-2`, estimated net lines removed 8

Symbols: `CommandDispatcher::InsertParsed`

**Claim.** The two statement-size gates (BI3's max_insert_rows cap and the "multi-row INSERT requires the transaction manager" refusal) are written out once for VALUES rows at the top of InsertParsed and again, byte for byte, for the rows an INSERT ... SELECT produced. The two copies must stay identical in condition and message for the cap to mean one thing.

**Evidence as the finder read it.** Site 1: 6171-6175 (`written.rows.size() > max_insert_rows_` -> "ERR INSERT of N rows exceeds max_insert_rows (M)") and 6182-6186 (`written.rows.size() > 1 && txn_ == nullptr` -> "ERR a multi-row INSERT requires the transaction manager; ..."). Site 2: 6277-6281 and 6282-6286, the same two conditions and strings over `evaluated->rows.size()`. `grep -n 'exceeds max_insert_rows\|requires the transaction manager' src/server/*.cpp` shows exactly these four lines.

**Proposed fix.** Hoist one local `const auto size_refusal = [&](std::size_t rows) -> std::optional<DispatchOutcome>` holding both checks in today's order with today's strings, and call it at 6171 with written.rows.size() and at 6277 with evaluated->rows.size(), returning its value when set. Same refusal text, same order relative to name resolution and the IX, same bare (status-OK) outcome. Covered by tests/bulk_insert_test.cpp and tests/insert_column_list_test.cpp (max_insert_rows), tests/expeditor_test.cpp, and tests/dml_forms_test.cpp's INSERT ... SELECT cells.

#### F29. Two value-capturing sinks and two sink-swap wrappers repeat one mechanism

`src/server/command_dispatcher.cpp:9417` - finder's severity **low**, finder's confidence **medium**, slice `dispatcher-3`, estimated net lines removed 30

Symbols: `ScalarSubquerySink`, `LayerWalkSink`, `CommandDispatcher::RunScalarSubquery`, `CommandDispatcher::WalkForLayer`, `StatusFromErrorReply`

**Claim.** RunScalarSubquery and WalkForLayer each (a) define a ResultSink that parks rows in `rendered_` and passes the row's index as text through EncodeProjectedRow/EncodeValueRow -> Emit (`std::to_string` / `std::stoull`), (b) swap the session's sink under a local `struct Restore`, (c) run the SELECT to its end with `if (out.select_run) out = run->RunInline()`, and (d) fold a bare `ERR` reply into InvalidArgument via `substr(4)` - which StatusFromErrorReply already does. Two copies of each piece must be kept in step (the index-as-text device especially).

**Evidence as the finder read it.** Sinks: ScalarSubquerySink 9325-9370 and LayerWalkSink 9468-9505 (same rendered_/index/Emit shape; 9465 even says "`ScalarSubquerySink`'s device"). Restore guard: 9418-9424 and 9581-9587, identical. Run-to-end: 9432-9435 and 9593-9596. ERR fold: 9442-9444 and 9598; helper at cpp:283 `Status StatusFromErrorReply(std::string_view reply)`.

**Proposed fix.** One file-local base `ValueCaptureSink` owning `rendered_`, the two Encode overrides and a `Take(row_text)` that returns the parked row; ScalarSubquerySink and LayerWalkSink keep only Describe/Emit policy (one-column/one-row vs. the max_rows cap). One file-local RAII `ScopedResultSink(session, sink)` replacing both Restore structs, and one helper `DispatchOutcome RunToEnd(DispatchOutcome)` for the select_run hand-back. Keep each site's exact message handling (RunScalarSubquery's Positioned()), so replies and byte positions are unchanged.

#### F30. HandleUpdate and HandleDelete are the same open / resume / park / finish frame written twice

`src/server/command_dispatcher.cpp:11862` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-hpp`, estimated net lines removed 35

Symbols: `CommandDispatcher::HandleUpdate`, `CommandDispatcher::HandleDelete`, `Session::ParkedWrite`, `CommandDispatcher::FinishWrite`, `CommandDispatcher::WithReturning`

**Claim.** The two handlers share about 40 lines. Each has the AO-S3b resume branch (restore scope, snapshot, cursor, trail mark and context, set statement_boundary_taken_, clear the park), the first-run branch (BeginWrite, StatementContextFor, SnapshotFor after it), the park arm that stores Session::ParkedWrite, and the FinishWrite(WithReturning(...)) tail. They differ only in the is_delete flag, the subquery vector, the Inner call and the RelationWrite kind. HandleDelete's comments say "`HandleUpdate`'s branch, for its reason" four times. The snapshot-failure leak reported above sits in both copies.

**Evidence as the finder read it.** Sites: command_dispatcher.cpp:9253-9320 (HandleUpdate) and :11862-11903 (HandleDelete). Resume branches :9263-9281 against :11869-11877; first-run branches :9283-9303 against :11879-11887; park arms :9311-9317 against :11895-11900; tails :9318-9319 against :11901-11902. HandleDelete's comments at :11868, :11876, :11884 and :11892 each defer to HandleUpdate's.

**Proposed fix.** One private helper, e.g. `template <typename Inner> DispatchOutcome RunWalkWrite(std::string_view line, Session&, bool is_delete, stats::RelationWrite kind, Inner&& inner)`. It selects the resume branch by `parked->is_delete == is_delete`, or opens the scope and snapshot (with the scope ended on a snapshot failure), calls `inner(scope, snap, resume_from, context, subqueries)`, then parks or runs `FinishWrite(WithReturning(...))`. HandleUpdate and HandleDelete become one call each around UpdateInner / DeleteInner. Same order of BeginWrite and SnapshotFor, same replies, same lock order. Covered by tests/lock_family_test.cpp (MidWalkWaitTest) and the update/delete suites.

#### F31. ApplyFile repeats the same five-line read for 27 unvalidated keys

`src/server/expeditor.cpp:223` - finder's severity **low**, finder's confidence **high**, slice `expeditor`, estimated net lines removed 80

Symbols: `Expeditor::Config::ApplyFile`

**Claim.** ApplyFile is 575 lines, and 27 of its keys are the identical block `if (file.Has(k)) { auto v = file.GetX(k); if (!v.ok()) return v.status(); field = v.value(); }` with no validation. The function already uses a local lambda for the five theta keys (line 569); the plain keys never got one. A key added by copy-paste that assigns the wrong field compiles and is caught only if a test sets that key.

**Evidence as the finder read it.** Bool keys: tls 314, waystone_recording 362, ash_enabled 367, ash_capture_arg_hash 372, waystone_replay 413, access_statistics 418, cabins 423, indexes 428, scan_kernel 468, cabin_optimizer 550. String keys: data_file 223, wal_dir 246, archive_dir 254, archive_command 259, tls_cert_file 319, tls_key_file 324, users_file 353, log_dir 751, log_file 756. Plain uint keys: cabin_max_values 433, cabin_max_entries_per_value 441, aggregate_max_groups 446, aggregate_max_distinct 455, sort_max_rows 460, join_build_max_rows 485, cabin_optimizer_snapshot_interval_ms 655, max_rows_touched 662. Precedent: the `theta` lambda at 569-579.

**Proposed fix.** Three local lambdas beside `theta` - `flag(key, bool&)`, `text(key, std::string&)`, `number(key, T&)` - each `if (!file.Has(key)) return OK; auto v = file.GetX(key); if (!v.ok()) return v.status(); out = ...; return OK;`, called in the existing order so the first refusal a file produces is unchanged. Statuses are GetX's own, so codes and messages are identical. Keep every key that validates as it is. Covered by tests/config_file_test.cpp (ExpeditorConfigTest.EveryKnownKeyIsApplied, KnownKeysCoverEveryKeyTheOverlayReads, TlsKeysParseAndDefaultOff, AggregateCapsParseAndCarryTheProposedDefaults).

#### F32. Core 0 and peer wiring written twice: instance services, statement limits, the SCRAM gate factory

`src/server/expeditor.cpp:1320` - finder's severity **low**, finder's confidence **high**, slice `expeditor`, estimated net lines removed 12

Symbols: `Expeditor::Open`, `Expeditor::Start`, `CommandDispatcher::set_statement_limits`, `TcpServer::ClientSetup`, `ScramAuthGate`

**Claim.** Three small repeats in the instance assembly. (a) The five instance-service setters are called by hand for core 0's dispatcher in Open and again for each peer's in Start; a sixth service must be added at both or one side silently lacks it. (b) Core 0's dispatcher is constructed with `indexes` and `max_insert_rows`, then `set_statement_limits(config_.Limits())` overwrites both on the next statement; the peer's construction passes neither. (c) The ScramAuthGate factory lambda is built twice, and the text listener takes it through a second `#if KDS_WITH_TLS` block instead of its ClientSetup.

**Evidence as the finder read it.** (a) expeditor.cpp:1320-1322, 1326-1327 (SetReclaimCounters, set_backup, set_archiver, SetStatementEpochs, SetReclaimQueue on `dispatcher_`) and 1988-1992 (the same five on `core.value()->dispatcher()`). (b) 1197-1198 `expeditor->config_.indexes, expeditor->config_.max_insert_rows);` then 1200 `set_statement_limits(expeditor->config_.Limits())`; command_dispatcher.hpp:1839-1841 assigns `indexes_enabled_` and `max_insert_rows_` from the limits; core_runtime.cpp:274-285 omits the two arguments. (c) 1749-1751 and 1789-1791 are the same lambda; tcp_server.hpp:263 `if (setup.auth) auth_gate_factory_ = setup.auth;` is what Configure does with it.

**Proposed fix.** (a) A private `void WireInstanceServices(CommandDispatcher&)` holding the five calls, used at both sites (in Open after `statement_epochs_` is emplaced). (b) Drop the two trailing constructor arguments at 1197-1198; nothing runs between the constructor and line 1200. (c) Set `text_setup.auth = client_setup.auth;` before `text.value().Configure(text_setup)` and delete 1787-1793; without TLS the factory is empty and Configure leaves the default, as now. Covered by tests/expeditor_test.cpp (EveryPeerListensAndCarriesCoreZerosStatementLimits), tests/drop_table_reclaim_test.cpp, tests/tcp_server_test.cpp's auth cells.

## 4. One item read a second time

`[source-read]` `src/txn/visibility.cpp:25` - the baseline build warns that
the undo-chain walk's `switch` does not handle `UndoRecordType::kVarHeapAppend`
(`-Wswitch`). The record is written with `prior_trx_id = kNoTrxId` and
`prior_undo_ptr = kNoUndoPtr` (`src/txn/manager.cpp:245-249`) as a link in
the writing transaction's chain, and no tuple's version chain names one, so
meeting it in this walk means the chain is damaged. The walk today falls out
of the `switch`, keeps the payload it had and classifies the stepped-back
state, where the neighbouring `kInvalid` arm answers `Corruption`. The
candidate change is a `kVarHeapAppend` arm that answers `Corruption` too.
Not applied; nothing since has touched it.

The same warning at `src/txn/manager.cpp:504` is not a defect: the rollback
handles `TrailAction::kVarHeapAppend` at line 416, before the `switch`, and
returns.

## 5. What is left

`[design]` In the order it would run:

1. **F4**, on the operator's ruling: resolve the snapshot's version when the
   head fails the WHERE and let the conflict check answer; or evaluate the
   WHERE on the snapshot's version always; or state today's behaviour in
   `txn.md` §5 and `known-gaps.md` as accepted.
2. **F26 to F32**, if wanted: F28, F29, F31 and F32 are local; F27 rewrites
   178 refusal sites and F26 and F30 fold UPDATE's and DELETE's walks into
   one, in the file every open worktree also edits.
3. **The review F3 to F25 did not get**, and the full suite on `c6caa100`.
4. **The other 27 slices**, or the decision that the audit stops at the
   dispatcher and the expeditor.
