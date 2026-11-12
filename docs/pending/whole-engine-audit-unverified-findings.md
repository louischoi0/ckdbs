# Pending: a whole-engine audit, stopped after 6 of 33 slices - 32 unverified findings

Status: **PENDING** - the audit is deferred by the operator. Decides nothing,
opens no stage, licenses no code.
Raised: 2026-11-12, in conversation with the operator.
Read at: `ce5347e6` (`v3.4.8-2-gce5347e6`), on worktree `audit-lean-and-correct`.
Every `path:line` below is at that commit.
Related: `docs/inflight/known-gaps.md` (each finder was told to leave out
what it records); `docs/inflight/README.md` (its rule - "verified against
the code, at a named commit, or it does not go in" - is why these findings
are here and not under `docs/inflight/bugs/`).

**Read this before any finding: nothing in section 3 is verified.** Each
entry is one reader's `[source-read]` claim. The audit's second stage -
skeptics told to refute each finding, two per defect and one per deletion or
simplification - never ran. A finding here is a place to look, with the
reader's reasoning; it is not a defect report, and a fix is not owed until
someone has reproduced or refuted it. The one item a second reader did check
is section 4.

## 1. What prompted it

`[operator]` *"Analyze the current project, look for items that are designed
and implemented with excessive complexity, code that is not needed, or logic
that may have a problem, and improve them."* While the audit ran the operator
deferred it: *"Proceed only with the tasks running now and stop for the
moment; I have no tokens"*, then *"for now, leave what was found in a
document"*.

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
- **Verification: not run for any finding.** The severity and confidence on
  each entry are the finder's own.
- **No source file was changed.** This document is the audit's only output
  in the tree.

What each finished finder says it read, in its own words:

- `expeditor`: Read in full, on worktree `audit-lean-and-correct` at `ce5347e6`: include/kds/server/expeditor.hpp (974 lines) and src/server/expeditor.cpp (2385 lines) - Config/ApplyFile, Open, Start, StopStartedCores, RunUntilStopped, ~Expeditor, ServeRuntime. Read the callees needed to judge them: CoreRuntime::Open/Run/ShutdownCheckpoint/HostHandedConnections/AcceptMoves (core_runtime.cpp 262-579), CheckpointGate and Checkpointer::RunGated, WalManager StopKicking/SetWakeRegistry, DataSyncer::Start/Stop, Scheduler::Run/Stop/SubmitEvery, TcpServer::Listen/Host/Configure/ClientSetup, SuperBlockCheckpointAnchor::EncodeLanding, TransactionManager's destructor, BackupControl::Wiring, WalArchiver::Wiring, ConfigFile getters, the dispatcher's fault-net deadline sites, and docs/inflight/known-gaps.md's expeditor entries (the cabin view latch across a build, the -j8 Start flake and the SO_REUSEPORT test port are known and not reported). Nothing was built or run; every finding is from reading plus grep over src include tests sim fuzz tools. Not traced to the bottom: RecoverCoreAtMount, ResumeAssertionsAfterRecovery, PageReclaimer, the WAL writer's kick path and TcpServer::Detach's treatment of parked statements - the teardown and mount orderings were checked against their headers' stated contracts only. Checked and found sound, so not reported: member declaration/destruction order against the writer, archiver and data-sync threads; the sticky stop flag against a peer STOP that lands before core 0 enters its reactor; the positional aggregates (StatementLimits, CabinLimits, both Wiring structs); KnownConfigKeys against the keys ApplyFile reads; the D19 fallback's `handoff_` dereference at one core (Unsupported is only raised with reuse_port set).
- `dispatcher-1`: Read src/server/command_dispatcher.cpp lines 1-3305 in full (helpers, error surface, AwaitWriteBlock/AwaitRelationLock/AwaitStatementWaits, DispatchAsync, StageStatement, deferred commits, Dispatch/DispatchInternal, RequiredRole, DispatchInner, HandleBackup, HandleSync, MetaSections, SET handlers, HandleCreateTable, SuspensionHold, BuildRun/IndexBuildRun/DeclarationBuildRun, HandleIndex, HandleCabin/RunCabinDdl, head of HandleAlter). Read callees outside the slice only as far as needed to verify claims: RefuseParkedWrite, NoteBlockingWriter, BorrowRelationForDdl, BorrowChain, TakeLockWait, EndWrite's tail, InDdlStatement/FinishDdlStatement, EndDdlScopeById, HandleSelect head, TryQueryLayer, LockTable::IntentionWriters/DropWake, Session::Poison/AdmittedWhileFailed, Catalog::CreateTable head, kwp_session.cpp statement text path, tcp_server.cpp submit path, stats/access_stats, StatementKindOf. Header: DispatchOutcome and the members my slice touches, not the whole 2859 lines. Checked and dismissed as not bugs: wake leak on AwaitWriteBlock's early exits (RefuseParkedWrite drops it), the thread-local structural-refusal note across suspensions (no interleaving reaches StructuralRerunDue with a foreign note and a live deadline), ANALYZE's DB-time line under DispatchAsync (analyze runs inline), ANALYZE/WITH as a readonly escalation route (ParseQuery reads queries only), Drain's wake re-registration. Nothing built or run; all claims are from reading on worktree audit-lean-and-correct at ce5347e6.
- `dispatcher-hpp`: In the worktree audit-lean-and-correct at ce5347e6 (read-only; nothing built or run). Read in full: include/kds/server/command_dispatcher.hpp (all 2859 lines), session.hpp, result_sink.hpp, read_borrow.hpp, statement_epoch.hpp, transaction_placement.hpp. Every declared method, data member, setter and test seam of CommandDispatcher was grep-checked for definitions and callers across src, include, tests, sim, fuzz, tools. In src/server/command_dispatcher.cpp I read the code behind the header's contracts: the dispatch and wait machinery (lines 270-1250, 1296-1720), StatementEpochScope/StageStatement, the write scope, borrow and conflict functions (11060-11900), the DDL scope functions (7520-7910), SuspensionHold/BuildRun/SelectRun (2408-2640, 5800-6100), the HandleInsert/HandleUpdate/HandleDelete/HandlePurge heads, WithReturning, ResolveForeignKeyParents, the index/spill logging helpers and LogInsert (5296-5500), HandleBegin/CommitLocal/RollbackLocal, DdlBuildLock. Not read: the bodies of InsertOneRow, UpdateParsed and DeleteInner beyond the excerpts cited, HandleIndex/HandleAssertion/HandleTransition/HandleAlter/HandleDropTable, HandleSelect/SelectBody, sys_views.cpp, kwp_session.cpp, tcp_server.cpp (only grepped). Candidates checked and dropped as used or intended: TransactionPlacer (a test implements it), StatementEpochs::shared and Session::durability()/txn_durability() (test-only one-line accessors), the bare CREATE TABLE handler (a deliberate refusal), the manager-less own_txn log path (tests run on it; the header records it as a known gap), PurgeWait::tuple_key (read by the refusal message). Seen and not reported: BorrowChain's `blocker = nullptr` default is dereferenced on a refusal (all three callers pass one); ASCII case-insensitive equality is defined ten times across the tree, one copy in session.hpp.
- `dispatcher-3`: Read src/server/command_dispatcher.cpp 6587-9907 in full (InsertOnConflict, RowHoldsUniqueKey, InsertOneRow, WalkBtreeLeaves/VisitRelation, TupleKeyLiteral/EqualityTarget, the DDL-scope family, ViewFor, SweepSettledDeleteMarks, EndDdlScopeById, RowLocatorForRollback, LocateByTupleKey, HandleShow, HandleCatalogView, RunAggregated, RunAnalyze, HandleSelect, SelectBody, Record*, NoteCabinWrite, HandleUpdate, RunScalarSubquery, LayerHostImpl, WalkForLayer, WithReturning, RunLayerQuery, TryQueryLayer, UpdateInner, head of UpdateParsed). Outside the slice I read only what the findings needed: InsertParsed 6240-6436, FinishWrite, EndWrite/AbortOwnedScope, SelectRun, UpdateParsed's arm/point/walk paths (10574-10725), exec/index_maintain.cpp unique-key code, Catalog::AdoptRoot, stats/access_stats.*, functions.hpp, plan_key/plan_stats headers, and the relevant spec lines (index.md IX6, types.md 3.2b, parser-v2.md 228-231, bulkinsert.md BI9). Nothing was built or run; all findings are from reading. Checked and judged clean or not worth a slot: RecordPlanStats mask bounds (key.n <= 4), NoteCabinWrite's previous indexing (UPDATE passes first_col_pos 0), EndDdlScopeById's null/LSN handling (production always has a WAL), bare "ERR" refusals without a Status (stated as accepted in DispatchOutcome's header), RETURNING typed after the write (spec'd). Smaller items not reported: VisitRelation is a one-arm wrapper over WalkBtreeLeaves since BZ (single caller; could be folded, ~15 lines); WalkBtreeLeaves rebuilds a std::function per leaf; InsertOneRow's omitted-key path returns on an encode failure without give_back() while the named-key path gives the borrow back (held only until the forced rollback); RowHoldsUniqueKey folds a failed ReadTuple/EncodeUniqueKey into "false". Did not read the callees in exec/ beyond the pieces named above (CheckUniqueKeyLatest, UniqueKeyHolds, BorrowOrWait bodies were taken at their declared contracts).
- `dispatcher-4`: Read src/server/command_dispatcher.cpp lines 9834-13031 in full (UpdateParsed, HandleBegin/Commit/Rollback, CommitLocal/RollbackLocal, HandleUse/SetIsolation/SetDurability/SetPriority, SnapshotFor, BeginWrite, RefuseParkedWrite, NoteBlockingWriter, BorrowRelationForDdl, BorrowChain, FoldTupleKeyWindow, DeclaredWriteBorrow, BorrowOrWait, the unique-key helpers, CheckWriteConflictBlocking, EndWrite, AbortOwnedScope, HandleDelete, PurgeConjunctRefusal, VisitKeyedSlotsFrom, HandleWaystone, HandlePurge, HandlePurgeDroppedColumns, DeleteInner). Consulted for contracts: command_dispatcher.hpp declarations, TupleKeyLiteral/NamesTupleKey/TupleKeyEqualityTarget, ResolveForeignKeyParents, CheckView, HandleUpdate, ErrorReply/StatusFromErrorReply, txn/visibility.hpp, lock_table.hpp/.cpp TryAcquire, heap_page.cpp OverwriteTuple/DeleteMark, Catalog::SetPatternWaystone, step_compiler ResolveColumn, parser ParsePurge/condition parse, docs/spec/txn.md §4.4 and §5, docs/spec/eviction.md §3.3, heap-and-tuple.md PU10, known-gaps.md (grepped for each finding). Nothing was built or run, so every bug is from reading only. Not traced to the bottom: VisitRelation/WalkCursor internals, CheckNoChildrenBeforeDelete, exec::PurgeKey/CleanDroppedCells, DispatchAsync's consumption of the wait fields, and the query-layer/RETURNING wrappers around these handlers.
- `dispatcher-2`: Read src/server/command_dispatcher.cpp lines 3250-6660 in full (HandleAlter's rename tail, ADD/DROP COLUMN, DROP TABLE, namespace, assertion and transition DDL, the foreign-key checks, the INSERT normalizers, ColumnRowOf, CREATE TABLE, the insert/split logging helpers, FinishWrite, the describe helpers, SelectRun, InsertParsed, NamedKeyVerdict/TupleKeyPlaced, ResolveConflictArbiter, BindExcluded and InsertOnConflict's head). For context I also read InDdlStatement/FinishDdlStatement/DdlScopeFor (7521-7621), BorrowRelationForDdl, BorrowChain's first-intention check and BorrowOrWait (11206-11560), InsertOneRow's head and logging tail, WalkForLayer/WithReturning's head, ErrorReply/StatusFromErrorReply, the statement head's Revalidate (960-980), CREATE INDEX's post-grant Revalidate (3020-3046), Catalog::DropColumn/AddColumnPosition/AddColumn/CreateForeignKey/Revalidate/BumpVersion, Schema::FindColumn, Parser::ParseValue, kwp_load_server.cpp's chunk path, RelationStats::Record, and tests/ddl_fence_rig_test.cpp's uncontended-grant cells. Nothing was built or run: finding 1's race is traced through the code and the existing rig's precedent, not executed. Not read: InsertOnConflict's body past 6660 and SelectBody (outside the slice). Checked and dropped as not real: a decimal literal reaching KeystoneIdOf in the DO NOTHING pre-pass (a bare `1.5` parses as kStr, int_val 0); the borrowed `ta` going stale across INSERT ... SELECT (nothing on that path invalidates the memo); lost refusal codes on bare `ERR` replies (StatusFromErrorReply recovers the spelled ones); sys.fkeys rows surviving a rollback (stated in ddl-transactional.md).

## 3. The findings, unverified

Grouped by kind, then by the finder's own severity. The `F` numbers are this
document's.

### 3.1 Logic defects (15)

#### F1. A mid-walk parked UPDATE/DELETE whose resume is refused at the archive gate strands its open scope on the session

`src/server/command_dispatcher.cpp:472` - finder's severity **high**, finder's confidence **high**, slice `dispatcher-hpp`

Symbols: `CommandDispatcher::AwaitWriteBlock`, `CommandDispatcher::DispatchInner`, `CommandDispatcher::ArchiveRefusal`, `CommandDispatcher::RefuseParkedWrite`, `Session::ParkedWrite`, `CommandDispatcher::HandleUpdate`, `CommandDispatcher::HandleDelete`

**Claim.** A write parked mid-walk (AO-S3b) keeps its scope in Session::parked_write. Only two things clear it: HandleUpdate/HandleDelete's resume branch and RefuseParkedWrite. AwaitWriteBlock's re-run goes through DispatchInner, whose ArchiveRefusal gate (line 1347) can answer before the handler runs. The outcome then has no write_block, so the loop exits and RefuseParkedWrite is never called. The scope is never ended, the session is not poisoned, and parked_write stays set for whatever UPDATE comes next.

**Evidence as the finder read it.** command_dispatcher.cpp:1344-1348: `if (IEquals(cmd,"INSERT")||IEquals(cmd,"UPDATE")||IEquals(cmd,"DELETE")...) { if (auto refused = ArchiveRefusal(); refused.has_value()) return *refused; }` runs before HandleUpdate. :471-472 `*out = DispatchAndStage(line, session);` then `while (out->write_block.has_value())` ends; :494 and :519 call RefuseParkedWrite only for a deadlock or a remaining write_block. :9263 `if (session.parked_write().has_value() && !session.parked_write()->is_delete)` resumes on any UPDATE text. grep parked_write: cleared only at :9281, :11877, :11138. WalArchiver::UpdateBlocking (wal_archive.cpp:211-223) flips blocking_ from its own thread at any time. Not in docs/inflight/known-gaps.md.

**Failure scenario.** Autocommit `UPDATE t SET v=v+1 WHERE name='x'` writes rows 1-6 and parks at row 7 on an undecided holder. The archiver starts failing with the WAL filesystem near full. The holder commits, the re-run hits ArchiveRefusal, and the client reads ERR. The owned transaction is never aborted: rows 1-6 stay X-held by an in-flight transaction until restart. Inside BEGIN the session is not poisoned, so COMMIT commits a partial UPDATE the client was told failed. The session's next UPDATE (any table) then enters the resume branch with the old cursor, snapshot and rows_done - after that COMMIT, through a released Transaction pointer.

**Proposed fix.** In AwaitWriteBlock, right after the re-run at line 472: `if (waiting_session.parked_write().has_value() && !out->parked_mid_walk) RefuseParkedWrite(*out, waiting_session, out->status.ok() ? StatusFromErrorReply(out->response) : out->status);`. A re-run that reached its handler has cleared parked_write, or re-set it together with parked_mid_walk, so the test is exactly 'the handler was never reached'. RefuseParkedWrite ends the scope through EndWrite (an owned one aborts, an explicit one poisons) and keeps the gate's refusal as the answer. Add a MidWalkWaitTest cell in tests/lock_family_test.cpp with an archiver that turns blocking during the park (tests/backup_archive_test.cpp has the fixture).

#### F2. ON CONFLICT on a unique index compares the 32-byte encoded key prefix, not the key values

`src/server/command_dispatcher.cpp:6642` - finder's severity **high**, finder's confidence **high**, slice `dispatcher-3`

Symbols: `CommandDispatcher::InsertOnConflict`, `CommandDispatcher::RowHoldsUniqueKey`, `exec::UniqueKey`, `exec::EncodeUniqueKey`, `exec::CheckUniqueKey`

**Claim.** A string key column encodes to a zero-padded 32-byte prefix (IX6), so `UniqueKey::encoded` is only a prefix test; BX-R1/R2 require full-value comparison (CheckUniqueKey phase 2 does it; index_unique_test.cpp:200 pins it). InsertOnConflict's up-front CG-Q4 check keys `uniques_seen` on `encoded`, and RowHoldsUniqueKey's re-read under `U` returns `now->encoded == key.encoded`. Both treat two distinct values sharing a 32-byte prefix (or 'a' vs 'a\0') as one key.

**Evidence as the finder read it.** 6624 `std::set<std::vector<std::byte>> uniques_seen;` 6642 `repeated = !uniques_seen.insert(p.unique->encoded).second;` 6645-6651 refuses CardinalityViolation. 6848-6850 `auto now = exec::EncodeUniqueKey(...); return now.value()->encoded == key.encoded;`. include/kds/exec/index_key.hpp:64 'two strings sharing a kIndexStringKeyBytes-byte prefix encode identically'; index_maintain.hpp:183-186 'compares full values at latest state, never the encoded key alone'; src/exec/index_maintain.cpp:357-362 is the full compare.

**Failure scenario.** Deterministic: unique index on varchar `ref`; `INSERT INTO t(ref,v) VALUES ('<32 x p>-A',1),('<32 x p>-B',2) ON CONFLICT (ref) DO UPDATE SET v = EXCLUDED.v` -> both rows encode alike -> refused `CARDINALITY_VIOLATION ... names the same unique key`, though the keys differ and both rows should be written. Race (quiet wrong write): holder h has '<p>-A'; row proposes '<p>-A' -> kHeld h; before the `U` on h is granted another txn commits `UPDATE ... SET ref='<p>-B'` on h; RowHoldsUniqueKey says true (prefix equal) -> the update arm overwrites h, which no longer conflicts; the insert arm was owed.

**Proposed fix.** Add one exec helper that compares two UniqueKeys by `values` with CompareValues per `ix.key_cols` (the loop at index_maintain.cpp:357-362, factored out). Use it in RowHoldsUniqueKey instead of the `encoded` equality, and in the up-front check keep `encoded` as a pre-filter only: map encoded -> earlier rows' UniqueKeys and set `repeated` only when the helper says the values are equal. Add a dml_forms cell with two >32-byte values sharing a prefix.

#### F3. Autocommit UPDATE/DELETE leaks its owned transaction when SnapshotFor fails (reader-lease slots full)

`src/server/command_dispatcher.cpp:9302` - finder's severity **high**, finder's confidence **high**, slice `dispatcher-hpp`

Symbols: `CommandDispatcher::HandleUpdate`, `CommandDispatcher::HandleDelete`, `CommandDispatcher::BeginWrite`, `CommandDispatcher::SnapshotFor`, `txn::AutocommitSnapshot`, `TransactionManager::RegisterReader`

**Claim.** HandleUpdate and HandleDelete call BeginWrite, which in autocommit begins an owned transaction (TXN_BEGIN logged, in-flight slot published, pushed to live_). They then call SnapshotFor and return on its failure without EndWrite. In autocommit SnapshotFor is AutocommitSnapshot, whose RegisterReader answers OutOfSpace once 256 leases are held on the core. known-gaps.md records that 256 parked SELECT walks fill those slots, but only says a further walk is refused. WriteScope is a plain struct, so nothing aborts or releases the transaction.

**Evidence as the finder read it.** :9283-9285 `auto opened = BeginWrite(session); ... scope = opened.value();` then :9299-9302 `auto snapshot = SnapshotFor(session); if (!snapshot.ok()) return {ErrorReply(snapshot.status()), false, 0, snapshot.status()};`. The same shape at :11879-11886 in HandleDelete. src/txn/manager.cpp:805-823: RegisterReader returns `Status::OutOfSpace("more than 256 registered readers on this core")`. manager.cpp:124-165: Begin appends kTxnBegin, pushes live_, PublishInFlight. known-gaps.md:846-849 and :1799-1801 describe the 256-walk state. HandlePurge (:12372 `ended`) and InDdlStatement end their scope on every exit; these two handlers do not.

**Failure scenario.** 256 clients each hold a streaming SELECT parked on one core (CE: a client that stops reading parks the walk). An autocommit `UPDATE t SET v=1 WHERE id=5` arrives on that core. BeginWrite begins T, SnapshotFor returns OutOfSpace, and the handler returns ERR. T is never aborted or released: it stays in live_ and in flight, pins the instance's snapshot bound (purge and undo recycling stop) and holds one of the core's 1,024 in-flight slots. Each further UPDATE/DELETE in that state leaks another. After 1,024, Begin refuses every transaction on the core until restart.

**Proposed fix.** End the scope on that exit in both handlers: `if (!snapshot.ok()) { (void)EndWrite(session, scope, snapshot.status()); return {ErrorReply(snapshot.status()), false, 0, snapshot.status()}; }`. An owned scope then goes through AbortOwnedScope. An explicit one cannot reach this exit, because BeginWrite already took the statement boundary. Same reply, code and byte. Add a cell that fills kMaxRegisteredReaders leases (tests/txn_manager_test.cpp shows the cap), runs an autocommit UPDATE and DELETE, and asserts that no transaction is left in flight.

#### F4. UPDATE/DELETE judge the WHERE on an invisible writer's head bytes and skip the row with no conflict (dirty read)

`src/server/command_dispatcher.cpp:10163` - finder's severity **high**, finder's confidence **medium**, slice `dispatcher-4`

Symbols: `CommandDispatcher::UpdateParsed`, `CommandDispatcher::DeleteInner`, `txn::Classify`, `txn::ResolveThroughUndo`, `CheckWriteConflictBlocking`

**Claim.** `apply` (UPDATE) and `qualifies` (DELETE) skip only `kNoVersion`; a `kNeedsUndoWalk` row (head written by an undecided or later-committed overwriter) is decoded from the page's own bytes and the WHERE is evaluated on them. If that head fails the WHERE the row is passed over before the borrow and `CheckWriteConflictBlocking`, although the version the statement's snapshot sees matches. The comment at 10107-10112 says the conflict check 'is what rejects it', but the check is never reached. txn.md §5's last table row (conflict when `cur` is invisible) is therefore not applied to a row the snapshot wanted.

**Evidence as the finder read it.** 10113: only `== Visibility::kNoVersion` returns. 10129: `payload_copy.assign(tuple.value().payload...)` (head bytes). 10144-10163: DecodeAndResolve(payload_copy) -> EvaluateConjuncts -> `if (!matched.value()) return Status::OK();` ahead of the borrow at 10190 and the conflict check at 10194. Same shape in DELETE: 12742, 12745, 12768-12771. visibility.hpp:56-58 says a kNeedsUndoWalk caller must resolve through undo. No test pins the case (tests/txn_session_test.cpp conflicts all use an unfiltered UPDATE/DELETE).

**Failure scenario.** t(id, status, bal) holds (5,'active',100). S1: BEGIN; UPDATE t SET status='frozen' WHERE id=5 (undecided). S2 autocommit: UPDATE t SET bal=bal+1 WHERE status='active' -> row 5 classifies kNeedsUndoWalk, head 'frozen' fails the WHERE, row skipped; S2 answers UPDATED 0 at once and commits. S1: ROLLBACK. Row 5 is 'active' with bal 100: a committed UPDATE skipped a row that matched in every committed state and in its own snapshot (a SELECT with the same WHERE in S2 returned it). Same for DELETE ... WHERE status='active', and for a compare-and-set `WHERE id=5 AND bal=100` (two conjuncts, scan path).

**Proposed fix.** Keep Classify's answer. When it is kNeedsUndoWalk and the head fails the WHERE, resolve the snapshot's version from `payload_copy` with `txn::ResolveThroughUndo`, decode it into the frame and evaluate the conjuncts again; no version or no match -> skip as today, match -> fall through to the existing borrow + `CheckWriteConflictBlocking`, which waits on or refuses the invisible writer (AO-S3b park). Apply at both sites (and so to both collection passes). Add a two-session test for UPDATE and DELETE with rollback and with commit of the first writer.

#### F5. A statement whose first keyword is followed by a newline is refused as an unknown command

`src/server/command_dispatcher.cpp:143` - finder's severity **medium**, finder's confidence **high**, slice `dispatcher-1`

Symbols: `SplitFirstToken`, `Trim`, `CommandDispatcher::DispatchInner`, `RequiredRole`, `stats::StatementKindOf`

**Claim.** SplitFirstToken trims with std::isspace but ends the first token only at space or tab. KWP hands the dispatcher the client's SQL text verbatim (kwp_session.cpp:544, 690; tcp_server.cpp:668), and the lexer treats every isspace byte as whitespace, so multi-line SQL is legal text. When the verb is followed by LF or CRLF the token DispatchInner routes on is "SELECT\n" (or "SELECT\n*"), which matches no IEquals arm. StatementKindOf (statement_stats.cpp:35-38) claims to split "as DispatchInner splits it" yet uses isspace, so sys.statement_stats files the refused statement under SELECT.

**Evidence as the finder read it.** command_dispatcher.cpp:96-97 Trim uses std::isspace; :143 `line.find_first_of(" \t")`; :1319 `auto [cmd, rest] = SplitFirstToken(line)`; :1331 RequiredRole(cmd) falls to Role::kAdmin for an unmatched token; :1572 `return {"ERR unknown command", false}`. src/parser/lexer.cpp:82 skips isspace. grep of tests/sim/fuzz/tools finds no statement with a newline directly after its verb; docs/inflight/known-gaps.md records only the leading `(` case (line 1438).

**Failure scenario.** A KWP client executes "SELECT\n  id, owner\nFROM accounts". SplitFirstToken returns cmd="SELECT\n". On an admin connection every route misses and the reply is `ERR unknown command`; on a readonly or readwrite connection RequiredRole answers admin and the reply is `ERR permission: SELECT\n needs admin; this connection is readonly`. The same text with a space after SELECT runs. "UPDATE\nt SET ...", "INSERT\nINTO ..." and "ROLLBACK\r\n" inside a failed transaction (AdmittedWhileFailed misses, so the session cannot leave the failed state with that text) fail the same way.

**Proposed fix.** In SplitFirstToken end the token at any whitespace byte Trim strips: `line.find_first_of(" \t\n\r\f\v")`. The returned views still point into the caller's line, so every byte offset computed from them (RefuseWhole, HandleSetAsh, SplitBeforeClause) is unchanged, and single-line statements split exactly as before. Add a command_dispatcher_test cell dispatching "SELECT\n1 ..." and a role cell. Covering tests today: tests/command_dispatcher_test.cpp (routing at 331-350, 879-888; roles at 1743, 1808), tests/kwp_session_test.cpp, tests/statement_stats_test.cpp.

#### F6. ADD/DROP COLUMN act on a stale memo and a stale name after an uncontended X grant

`src/server/command_dispatcher.cpp:3513` - finder's severity **medium**, finder's confidence **medium**, slice `dispatcher-2`

Symbols: `CommandDispatcher::AlterColumnRelation`, `CommandDispatcher::HandleDropColumn`, `CommandDispatcher::HandleAddColumn`, `CommandDispatcher::BorrowRelationForDdl`, `Catalog::Revalidate`

**Claim.** AlterColumnRelation resolves the name, takes the relation X and returns. Unlike RENAME (3299-3313), CREATE INDEX (3040), CREATE ASSERTION (4026) and CREATE TRANSITION (4115), it neither calls catalog_.Revalidate() nor asks the name again under the X. A DDL another core committed between this statement's head and an uncontended grant is therefore invisible: HandleDropColumn's InitTableAccess (3616) returns the pre-change memo. The door (Catalog::DropColumn) re-checks position and name only, not the heap's last-declared-column rule (3642), and nothing re-checks the relation's name.

**Evidence as the finder read it.** 3501-3518: no Revalidate after BorrowRelationForDdl; BorrowRelationForDdl (11206-11234) has none either, while a writer's IX does check MemoIsCurrent (11334). 3616/3642 read `access.value()->layout.live_columns` from the memo. `grep 'last declared' src include` hits only command_dispatcher.cpp:3640-3645; Catalog::DropColumn (catalog.cpp:2039-2094) has no such check. The window is the one tests/ddl_fence_rig_test.cpp drives with EpochSeam::kAfterPublish (e.g. ARenameToWhoseNameMovedBeforeItsGrantIsRefused, mutation-killed for HandleAlter). Not in docs/inflight/known-gaps.md; alter.md line 114 states the current-memo rule for rename only.

**Failure scenario.** Heap h(x, y), cached on core 0 (live_columns=3). Core 0 starts `ALTER TABLE h DROP COLUMN y`; its head revalidates. Before it asks for the X, core 1 runs `ALTER TABLE h DROP COLUMN x` to completion. Core 0's X is granted at once, the stale memo passes `live_columns == 2`, the door finds y live and drops it: the heap has no declared column (BZ-Q3 violated). Same window with core 1 running `ALTER TABLE t RENAME TO u`: core 0's `ALTER TABLE t DROP COLUMN c` resolves `t` from the stale name cache, drops the column of the relation now named u and answers `DROPPED COLUMN t.c`.

**Proposed fix.** In AlterColumnRelation, after the grant, do what HandleAlter's rename does at 3299-3313: `catalog_.Revalidate();` then call ResolveAlterTarget again and refuse `TxnConflict("relation '<name>' changed while the ALTER waited for it - retry")` if it no longer names the locked oid (its own status if it is gone). No TableAccess is held at that point (ColumnRowOf's row is a copy). Add two cells to tests/ddl_fence_rig_test.cpp on the kAfterPublish seam: two DROP COLUMNs on a two-column heap, and RENAME TO against DROP COLUMN. Existing coverage: tests/alter_column_test.cpp, tests/heap_hidden_key_test.cpp (AddAndDropColumnKeepThePositionalRowTheDeclaredWidth), tests/column_name_case_test.cpp.

#### F7. NOW() is not one value per statement in ON CONFLICT DO UPDATE and in RETURNING

`src/server/command_dispatcher.cpp:6780` - finder's severity **medium**, finder's confidence **high**, slice `dispatcher-3`

Symbols: `CommandDispatcher::InsertOnConflict`, `CommandDispatcher::WithReturning`, `StatementContextFor`, `exec::StatementContextNow`, `CommandDispatcher::UpdateParsed`

**Claim.** functions.hpp and types.md 3.2b state `NOW()`/`CURRENT_DATE` are one value per statement (`StatementContext`, taken once and carried across a park). InsertOnConflict mints a fresh context from the wall clock for every conflicting row's update arm, and WithReturning evaluates its items under `exec::StatementContextNow()` instead of the statement's context that HandleUpdate/HandleDelete already hold. The shared typer admits NOW() in both places (set_expr.cpp:902, 1731).

**Evidence as the finder read it.** 6779-6781 `UpdateParsed(arm, scope, snapshot.value().snap, WalkCursor{}, StatementContextFor(*scope.session), subqueries, *holder)` inside the per-row loop; UpdateParsed evaluates SET with that context (`EvaluateSetExpression(*compiled[i].expr, row, context)`). 9650 `const exec::StatementContext context = exec::StatementContextNow();` used at 9750. include/kds/exec/functions.hpp:63-68 'Taken once per statement, so NOW() is one value however many times a statement calls it and however many rows it filters'. HandleUpdate keeps `context` (9257, 9286) but does not pass it to WithReturning (9318).

**Failure scenario.** `INSERT INTO t VALUES (1,..),(2,..) ON CONFLICT (id) DO UPDATE SET updated_at = NOW()` with keys 1 and 2 present: row 1's arm reads the clock at t1, row 2's at t2 > t1, so one statement stamps two instants (a plain `UPDATE ... SET updated_at = NOW()` stamps one). `UPDATE t SET ts = NOW() WHERE id = 1 RETURNING ts, NOW()` answers two different timestamps; after a lock wait or a mid-walk park the gap is up to the wait's length. Tests pin the clock (ScopedStatementClockForTest), so no cell sees it.

**Proposed fix.** Take the context once per INSERT statement: hoist `const exec::StatementContext context = StatementContextFor(*scope.session);` above InsertOnConflict's row loop and pass it to every UpdateParsed call (better: take it in HandleInsert and thread it to EvaluateInsertExpressions at 4275 and InsertSelectRows at 6099 too). Give WithReturning a `const exec::StatementContext&` parameter fed from HandleUpdate's/HandleDelete's carried `context` and HandleInsert's, replacing the StatementContextNow() at 9650.

#### F8. PURGE ignores a column qualifier: `PURGE FROM a WHERE b.id = 5` frees a's key 5

`src/server/command_dispatcher.cpp:12349` - finder's severity **medium**, finder's confidence **high**, slice `dispatcher-4`

Symbols: `CommandDispatcher::HandlePurge`, `CommandDispatcher::FoldTupleKeyWindow`, `CommandDispatcher::TupleKeyLiteral`, `NamesTupleKey`, `PurgeConjunctRefusal`

**Claim.** PURGE has no compiler behind it (stated at 11913-11915); its WHERE is read only by `FoldTupleKeyWindow` -> `TupleKeyLiteral` -> `NamesTupleKey(schema, cond.col.name)`, which never looks at `cond.col.qualifier`. The grammar is DELETE's and admits `rel.col`, so a conjunct qualified by any name folds as the purged relation's tuple key. UPDATE and DELETE refuse the same spelling in `CompileWhere` (`ResolveColumn`: "'x.id' names no relation in scope"). PURGE is the one irreversible K1 exception, and the working rule is never to accept a spelling and enforce something else.

**Evidence as the finder read it.** command_dispatcher.cpp:133-136 NamesTupleKey uses `schema.FindColumn(name, 0)` only; 7498 TupleKeyLiteral passes `cond.col.name`; 11401 the BETWEEN arm likewise; 12349-12352 HandlePurge refuses only `window.unfolded`. parser.cpp ParseColumnName (2688-2710) fills `qualifier` for a WHERE conjunct. step_compiler.cpp:671-727 refuses an unknown qualifier for DELETE/UPDATE. Other dispatcher sites do check it (6555, 7924, 9676). tests/purge_key_test.cpp has no qualified-column case; not in known-gaps.md.

**Failure scenario.** Relation `orders` has delete-marked key 5. An admin types `PURGE FROM orders WHERE customers.id = 5` (or a typo'd alias `o.id`). The conjunct folds to window [5,6), phase 1 targets key 5, phase 2 retires it: reply `PURGED 1`, the key is rebindable. `DELETE FROM orders WHERE customers.id = 5` is refused InvalidArgument at the qualifier's byte.

**Proposed fix.** In HandlePurge before `FoldTupleKeyWindow`, for each conjunct with `cond.col.qualified()` and `!IEquals(cond.col.qualifier, stmt.table_name)`, refuse `InvalidArgument("'q.c' names no relation in scope (byte N)")` at `cond.col.byte_offset` - DELETE's wording and code. A qualifier equal to the table name keeps working. Add the row to `PurgeKeyTest.EveryRefusalCarriesItsCodeAndByte`.

#### F9. The synchronous path's failed commit sync answers ERR with DispatchOutcome::status left OK

`src/server/command_dispatcher.cpp:1242` - finder's severity **low**, finder's confidence **medium**, slice `dispatcher-hpp`, estimated net lines removed 5

Symbols: `CommandDispatcher::DispatchInternal`, `DispatchOutcome::status`, `StatusFromErrorReply`, `CommandDispatcher::DispatchAsync`

**Claim.** DispatchOutcome::status is documented as OK on success and on the handful of refusals built from a bare message; a typed caller switches on it. When a group commit's sync fails, DispatchAsync carries it (`{ErrorReply(refused), should_stop, 0, refused}`, line 921). DispatchInternal builds `DispatchOutcome refused{ErrorReply(s), outcome.should_stop}` and drops the Status, so the only recoverable code is StatusFromErrorReply's bare-line fold to InvalidArgument. The positional refusal `{ErrorReply(s), false, 0, s}` is hand-written at 178 sites and as 5 identical `refuse` lambdas; this is the site that left a field out.

**Evidence as the finder read it.** :1239-1245 `Status s = wal_->DrainOnce(); if (s.ok()) s = wal_->EnsureDurable(outcome.pending_lsn); if (!s.ok()) { DispatchOutcome refused{ErrorReply(s), outcome.should_stop}; refused.fetch_id = outcome.fetch_id; return refused; }` against :921 `*out = {ErrorReply(refused), out->should_stop, 0, refused};`. Header :154-160 states the status contract. `grep -c 'ErrorReply(.*), false, 0, '` in command_dispatcher.cpp gives 178; identical `refuse` lambdas at :1634, :10975, :12025, :12295, :12488. kwp_load_server.cpp:552-554 is a production caller of DispatchInternal("COMMIT").

**Failure scenario.** A KWP load ends with COMMIT under `group` durability through DispatchInternal and the log's sync fails (fail-stop). The outcome is `ERR <io message>` with status OK. A caller that reads `out.status` sees no failure code, and one that falls back to StatusFromErrorReply reports the device failure to the loader as InvalidArgument, the 'simply wrong request' category, instead of the I/O refusal DispatchAsync reports for the same event.

**Proposed fix.** Add `static DispatchOutcome Refused(const Status& s, bool should_stop = false)` to DispatchOutcome, returning `{ErrorReply(s), should_stop, 0, s}`. Use it at :1242 (keeping the fetch_id copy) and at :921, and replace the five identical lambdas. The remaining positional sites can convert mechanically with no change of reply. kwp_load_server.cpp:554 should then prefer `out.status` as its chunk path already does at :534. Covered by tests/durable_kick_rig_test.cpp, tests/kwp_load_server_test.cpp, and the ErrorReply round-trip test.

#### F10. An empty KWP load chunk is answered and counted as one inserted row

`src/server/command_dispatcher.cpp:6400` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-2`

Symbols: `CommandDispatcher::InsertParsed`, `CommandDispatcher::ExecuteInsert`, `CommandDispatcher::FinishWrite`

**Claim.** InsertParsed assumes at least one row once it reaches the bulk loop. The INSERT ... SELECT arm (6287) and the DO NOTHING arm (6333) each return `rows=0` for an empty set, but a statement that arrives with no rows at all falls through: the loop runs zero times, `bulk` is false, and the single-row reply is built from the value-initialized `last` with rows_affected hard-coded to 1.

**Evidence as the finder read it.** 6349 `const bool bulk = stmt.rows.size() > 1;`, 6366-6386 loop over stmt.rows, 6400-6403 `WroteRows("INSERTED oid=... id=" + last.id + ..., 1, oid.value())`. kwp_load_server.cpp:486-530 decodes a chunk with row_count 0 (DecodeRowBatch admits zero rows; bulkinsert.md does not forbid it) and calls ExecuteInsert with empty stmt.rows. FinishWrite (5612) then calls RelationStats::Record(core, oid, kInsert, 1); relation_stats.cpp:14 skips only rows == 0.

**Failure scenario.** A loader sends C_LOAD_CHUNK with row_count = 0. ExecuteInsert -> InsertParsed replies `INSERTED oid=N id=0 page=0 slot=0` with rows_affected = 1, and sys.relation_stats' `inserts` for the relation rises by one per empty chunk although no row was placed. The load's own rows_accepted is unaffected (it adds stmt.rows.size()).

**Proposed fix.** Immediately after `const parser::InsertStmt& stmt = kept.has_value() ? *kept : shaped;` (6337) add `if (stmt.rows.empty()) return {"INSERTED oid=" + std::to_string(oid.value()) + " rows=0", false, 0};` and delete the now-subsumed block at 6333-6335 (same reply text). Leave the SELECT arm's early return at 6287 where it is, so an empty SELECT still answers before the arbiter is resolved. Add a zero-row chunk cell to tests/kwp_load_server_test.cpp beside its sys.relation_stats check (line 314); tests/dml_forms_test.cpp OnConflictDoNothing... covers the unchanged reply.

#### F11. ON CONFLICT (<unknown column>) subtracts a null pointer and matches the result against index columns

`src/server/command_dispatcher.cpp:6516` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-2`

Symbols: `CommandDispatcher::ResolveConflictArbiter`, `Schema::FindColumn`

**Claim.** ResolveConflictArbiter checks FindColumn's status but not its value. Schema::FindColumn returns an OK nullptr for a name with no live column (unknown, or dropped). The code then computes `col.value() - ta.schema.columns.data()` on nullptr, which is undefined behaviour, truncates it to uint16_t and compares it with each single-column unique index's key_cols[0]. BindExcluded (6542) and NormalizeListedInsertRows (4328) both handle the nullptr; this site does not.

**Evidence as the finder read it.** 6513-6519: `auto col = ta.schema.FindColumn(...); if (!col.ok()) return col.status(); if (col.value() == &ta.schema.columns.front()) ...; const auto at = static_cast<std::uint16_t>(col.value() - ta.schema.columns.data()); for (ix : ta.indexes) if (ix.unique && ix.nkeys == 1 && ix.key_cols[0] == at) return &ix;`. src/catalog/schema.cpp:46 `if (matches.empty()) return static_cast<const SysColumnRow*>(nullptr);`. tests/dml_forms_test.cpp:192 only expects some ERR for `ON CONFLICT (nosuch)`.

**Failure scenario.** `INSERT INTO w VALUES (...) ON CONFLICT (nosuch) DO UPDATE SET ...` on a relation with a unique index on `name`: col.value() is nullptr, `at` is the low 16 bits of (0 - columns.data())/sizeof(SysColumnRow). Normally no index matches and the statement gets the NotImplemented refusal by accident. When the truncated value equals the index's key column position (address-dependent), the index is returned as the arbiter and the statement silently arbitrates on `name`, updating a row the client never targeted. Pointer arithmetic on nullptr is UB in every case.

**Proposed fix.** After the status check add a null test that skips the subtraction and the index loop: `if (col.value() != nullptr) { if (col.value() == &ta.schema.columns.front()) return kTupleKeyArbiter; const auto at = ...; for (...) ... }`, then fall through to the existing NotImplemented refusal (same code, message and byte as today, which parser-v2.md I20's "any other target is refused NotImplemented" covers). Tighten tests/dml_forms_test.cpp OnConflictRefusesWhatItDoesNotDo to expect that message for `(nosuch)`, on a relation that has a unique index.

#### F12. RETURNING without a transaction manager is refused after the write is placed and committed

`src/server/command_dispatcher.cpp:9636` - finder's severity **low**, finder's confidence **medium**, slice `dispatcher-3`

Symbols: `CommandDispatcher::WithReturning`, `CommandDispatcher::HandleInsert`, `CommandDispatcher::FinishWrite`, `CommandDispatcher::EndWrite`

**Claim.** WithReturning runs after InsertInner/UpdateInner/DeleteInner have written. Its `txn_ == nullptr || scope.txn == nullptr` refusal therefore answers ERR for a statement whose rows are already in the page and, on the no-manager path, already committed by LogInsert's own-transaction commit. FinishWrite then calls EndWrite with a failure, whose no-manager arm only runs `enforcer_->AbortTxn(kBootstrapXid)` - nothing unwinds the row, and the assertion reservation of a row that stays is given back.

**Evidence as the finder read it.** 5596-5597 `FinishWrite(session, scope, WithReturning(line, session, scope, InsertInner(line, scope)), ...)`. 9636-9640 `if (txn_ == nullptr || scope.txn == nullptr) return refuse(Status::NotImplemented("RETURNING needs the transaction manager ..."))`. 7317-7319 `LogInsert(..., /*own_txn=*/scope.txn == nullptr)` commits (5513). 11713-11723 EndWrite's no-manager arm aborts only the enforcer. The same configuration refuses a multi-row INSERT before writing for exactly this reason (6282-6286 'cannot unwind a partially placed statement').

**Failure scenario.** Dispatcher built without a TransactionManager (the constructor's default; rigs and tools): `INSERT INTO t VALUES (1,'a') RETURNING id` -> row placed, logged and committed -> WithReturning refuses NotImplemented -> client reads ERR, yet `SELECT` shows the row; a retry gets AlreadyExists. With `CREATE ASSERTION ... COUNT(*) <= n` on t the kept row's reservation is aborted, so the registry under-counts the group.

**Proposed fix.** Refuse before the write: in InsertParsed, UpdateParsed (arm_target unset) and the DELETE twin, return the same NotImplemented (same message, `returning.byte_offset`) when `!stmt.returning.items.empty() && (txn_ == nullptr || scope.txn == nullptr)`, before any row work; keep WithReturning's check as an unreachable guard or delete it.

#### F13. CommitLocal's failed-commit arm re-implements RollbackLocal, drops AbortTxn's Status and loses the commit failure's code on a double failure

`src/server/command_dispatcher.cpp:10862` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-4`, estimated net lines removed 8

Symbols: `CommandDispatcher::CommitLocal`, `CommandDispatcher::RollbackLocal`, `AssertionEnforcer::AbortTxn`

**Claim.** The arm runs RollbackLocal's exact sequence (AbortTxn, Abort, EndDdlScope, Finish, Release) inline, but discards the enforcer's status with `(void)` while its own comment says a failed unwind 'is not swallowed', and RollbackLocal/AbortOwnedScope both fold that status in. When `Abort` also fails, the outcome is built from a bare string with no `Status`, so a typed client receives whatever `StatusFromErrorReply` recovers - InvalidArgument for every code without a spelling - instead of the commit's own code.

**Evidence as the finder read it.** 10862 `(void)enforcer_->AbortTxn(page_store_, wal_, id);` vs 10941-10943 (`if (!unwound.ok()) aborted = unwound;`) and 11833-11836. 10869-10873 returns `{ErrorReply(committed.status()) + " (and rolling it back failed: ...)", false}` with `status` left OK; DispatchOutcome::status doc (hpp 154-160) says the category is carried there because the rendered line folds codes. grep 'rolling it back failed' in tests/: no hit.

**Failure scenario.** Explicit transaction with assertion reservations; COMMIT's log append fails (IoError, log fail-stopped). AbortTxn then fails to give the reservations back: nothing reports or logs it; if `txn_->Abort` happens to succeed the client reads only the commit error. If Abort fails too, the KWP error carries category InvalidArgument ('simply wrong') for an I/O failure.

**Proposed fix.** Replace 10862-10874 with `DispatchOutcome rolled = RollbackLocal(session);` then, if `!rolled.status.ok()`, `Status both = committed.status().WithMessage(committed.status().message() + " (and rolling it back failed: " + rolled.status.message() + ")"); return {ErrorReply(both), false, 0, both};` else return the commit status as today. Response text is byte-identical (ErrorReply is prefix + message); the only changes are that AbortTxn's failure is reported and the code is carried.

#### F14. Four interval keys and the lock fault net wrap their unit conversion unchecked

`src/server/expeditor.cpp:499` - finder's severity **low**, finder's confidence **high**, slice `expeditor`

Symbols: `Expeditor::Config::ApplyFile`, `lock_wait_fault_net_ns`, `checkpoint_interval_ns`, `wal_drain_interval_ns`, `relaxed_flush_interval_ns`, `cabin_optimizer_snapshot_interval_ms`

**Claim.** `txn_lifetime_ceiling_ms` refuses a value whose nanoseconds wrap (line 512, with the comment that a wrap 'would enforce a different, possibly sub-millisecond, ceiling than the one written'). The same multiplication is unguarded for `lock_wait_fault_net_ms` (499), `checkpoint_interval_ms` (303), `wal_drain_interval_us` (702), `relaxed_flush_interval_us` (707) and, in Start, `cabin_optimizer_snapshot_interval_ms` (2088). GetUint accepts any u64, so the engine accepts a spelling and enforces another quantity, which CLAUDE.md's working rules forbid.

**Evidence as the finder read it.** expeditor.cpp:499 `lock_wait_fault_net_ns = v.value() * 1'000'000ULL;` with the comment 'No zero check' and no upper check; 303 `checkpoint_interval_ns = v.value() * 1'000'000ULL;`; 702/707 `* 1'000ULL`; 2088 `config_.cabin_optimizer_snapshot_interval_ms * 1'000'000ULL`. Contrast 512-516 (the guard) and tests/config_file_test.cpp:612-615, which pins the refusal for txn_lifetime_ceiling_ms = 18446744073710 only. Consumers add without saturation: command_dispatcher.cpp:374, 618, 945 `NowNs() + lock_wait_fault_net_ns_`.

**Failure scenario.** An operator writes `lock_wait_fault_net_ms = 18446744073709551615` (u64 max, meaning 'never give up'). (2^64-1)*10^6 mod 2^64 = 2^64-10^6, so lock_wait_fault_net_ns is that; command_dispatcher.cpp:374 computes NowNs() + it, which wraps to NowNs() - 1 ms: every lock wait and every structural re-run (945-946) meets a deadline already past and is refused TxnConflict at once. `= 18446744073710` gives a 448,384 ns net. The same value in checkpoint_interval_ms arms a 0.45 ms checkpoint cadence.

**Proposed fix.** One local helper in ApplyFile, e.g. `scaled(key, factor, out)`, that returns InvalidArgument when `v > UINT64_MAX / factor` (the message shape line 513 uses) and otherwise stores `v * factor`; use it for the five keys and for txn_lifetime_ceiling_ms after its zero check, and store the cabin optimizer interval through it too (or bound it at parse). Bound the fault net low enough that `NowNs() + net` cannot wrap either (e.g. refuse above UINT64_MAX/2 ns). Add the wrap cases beside config_file_test.cpp:612.

#### F15. A failed Start() leaves the data-sync thread running with its abort handler installed

`src/server/expeditor.cpp:1617` - finder's severity **low**, finder's confidence **medium**, slice `expeditor`

Symbols: `Expeditor::Start`, `DropOnFailure`, `Expeditor::StopStartedCores`, `storage::DataSyncer`

**Claim.** The header promises of a failed Start: 'nothing is left running: every thread this call started is stopped and joined before it returns'. Start installs the data-sync failure handler (which calls `std::abort()`) and starts the DataSyncer thread before `running_` and `DropOnFailure` exist. `DropOnFailure` runs only `StopStartedCores()` and `running_.reset()`, neither of which touches the syncer; only `~Expeditor` stops it. The instance therefore differs from one that was never started, though Start reported failure.

**Evidence as the finder read it.** expeditor.cpp:1610-1617 `store_->data_sync().SetFailureHandler([this]{... std::abort(); }); store_->data_sync().Start();` precede 1619-1627 (`running_ = make_unique`, `DropOnFailure { owner->StopStartedCores(); owner->running_.reset(); }`). StopStartedCores (2214-2244) joins workers, calls StopKicking, clears `cores_` and `handoff_` only. The stop is at 1576-1579 in `~Expeditor`. Contract: expeditor.hpp:584-588.

**Failure scenario.** Start() is refused at `TcpServer::Listen` because the port is taken (the case known-gaps.md records for this test file). It returns the IoError with the DataSyncer thread alive and the handler set. A caller that keeps the instance - tests drive `dispatcher()`, `Checkpoint()` and `Sync()` with no reactor - now has a failed data-file sync abort the process, where the same call on an instance never started returns the Status. Not traced: that every such sync routes through the thread once it runs.

**Proposed fix.** Give `DropOnFailure`'s destructor the two lines `~Expeditor` already has, after `running_.reset()`: `owner->store_->data_sync().Stop(); owner->store_->data_sync().SetFailureHandler({});`. Both are idempotent (data_syncer.cpp:14-17), so the destructor's own copy stands. Alternatively narrow the header's sentence to 'every core thread'. A cell beside ExpeditorTest.StartIsRefusedTwiceAndTheRunHalfIsRefusedWithoutIt can bind the port first and assert the syncer is stopped after the refused Start.

### 3.2 Dead code (6)

#### F16. UPDATE's appended_spills / NoteSpills / LogSpills block is dead since spills are noted and logged at their append; NoteSpills is left with one caller

`include/kds/server/command_dispatcher.hpp:2341` - finder's severity **medium**, finder's confidence **high**, slice `dispatcher-hpp`, estimated net lines removed 45

Symbols: `CommandDispatcher::NoteSpills`, `CommandDispatcher::SpillLogFor`, `CommandDispatcher::UpdateParsed`, `exec::LogSpills`, `exec::VarHeapSink`

**Claim.** Since AT-S21's review, SpillLogFor returns a callback whenever scope.txn != nullptr, and EncodeCell then calls on_append instead of pushing to `appended`. In UpdateParsed, `appended_spills` is therefore empty whenever a transaction exists, and with no transaction NoteSpills returns at once and the LogSpills gate is false. Both calls are no-ops in every configuration, and `collect_spills` fills a vector nothing reads. NoteSpills' only live caller is SpillLogFor, which wraps one spill in a temporary vector, copying the spilled bytes per append.

**Evidence as the finder read it.** src/exec/row_codec.cpp:252-256: `if (varheap.on_append != nullptr) { ...(*varheap.on_append)(spill) } else if (varheap.appended != nullptr) { varheap.appended->push_back(...) }`. command_dispatcher.cpp:5346 `if (scope.txn == nullptr) return nullptr;` (SpillLogFor). :10382-10383 `std::vector<exec::AppendedSpill> appended_spills; const bool collect_spills = wal_ != nullptr || scope.txn != nullptr;`. :10510 `NoteSpills(scope, ta.oid, id.value(), appended_spills)`; :5426 `if (scope.txn == nullptr) return Status::OK();`. :10513-10519 `if (wal_ != nullptr && scope.txn != nullptr) { exec::LogSpills(wal_, page_store_, appended_spills, ...)`. :5356 `NoteSpills(scope, rel_oid, tuple_key, {spill})`. grep NoteSpills in src: only those two calls. The code's own comment at :10386 says `appended_spills` 'then stays empty'.

**Proposed fix.** In UpdateParsed delete `appended_spills`, `collect_spills`, the NoteSpills call and the LogSpills call, and pass nullptr as VarHeapSink's `appended`. In SpillLogFor call `txn_->NoteVarHeapAppend(*scope.txn, rel_oid, spill.ptr.page_id, spill.ptr.slot, tuple_key)` directly. Delete NoteSpills' declaration, its comment and its definition (:5423-5435). The records and their order are unchanged: with a manager every spill is still noted and then logged under its page's hold, and without one UPDATE logged nothing before. Covered by tests/varheap_lifetime_test.cpp, tests/varheap_carry_crash_test.cpp, tests/insert_wal_test.cpp.

#### F17. Unused accessors, two members that are locals, a peer field nobody reads, always-true guards

`include/kds/server/expeditor.hpp:657` - finder's severity **low**, finder's confidence **high**, slice `expeditor`, estimated net lines removed 25

Symbols: `Expeditor::checkpoint_stats`, `Expeditor::log`, `Expeditor::reclaim_gate_`, `Expeditor::instance_stop_`, `CoreRuntime::Config::cabin_limits`, `Expeditor::Sync`, `Expeditor::ReclaimStep`

**Claim.** (1) `Expeditor::checkpoint_stats()` and `Expeditor::log()` have no caller anywhere. (2) `reclaim_gate_` is written and read only inside Open and is a copy of `recovery_.scan_end`. (3) `instance_stop_` is written and read only inside Start's cores>1 block, then left holding a pointer to a destroyed scheduler. (4) `core_config.cabin_limits = config_.CabinLimitsOf()` is never read: a peer reads the field only when `cabins_store` is null, and the Expeditor hands the store whenever `cabins` is on. (5) `wal_ != nullptr` in Sync() and ReclaimStep() and `txn_manager_.has_value()` in the writeback tick are always true after Open.

**Evidence as the finder read it.** `grep -rnF 'checkpoint_stats' src include tests sim fuzz tools` -> expeditor.hpp:657 only. `grep -rnF 'log()' src/server include/kds/server` -> expeditor.hpp:713 only; the `.log()` hits in tests are SimWakerTable's. `grep -rn reclaim_gate_ src include` -> expeditor.cpp:1043 (write), 1317 (read), hpp:933. `grep -rn instance_stop_ src/server/expeditor.cpp` -> 1874 (write), 1971 (read). core_runtime.cpp:264-267 `cabins_ = config.cabins_store; if (config.cabins && cabins_ == nullptr) cabin_store_.emplace(config.cabin_limits);` against expeditor.cpp:1954 `cabins_store = cabin_store_ ? &*cabin_store_ : nullptr` and :971 (store built iff config_.cabins); core_runtime.hpp:122-124 says the field is a fixture's. Open returns an error before `wal_` and `txn_manager_` can be unset (923, 1182).

**Proposed fix.** Delete the two accessors; pass `expeditor->recovery_.scan_end` to CollectAtMount and drop `reclaim_gate_` (keep its comment at the call); make `instance_stop_` a local `std::function<void()>` in Start's cores>1 block; delete expeditor.cpp:1922 and its half of the comment at 1917 (CabinLimitsOf keeps its one caller at 973); drop the three guards. `core_config.cabins` must stay: with `cabins = off` a peer left at the default would build a store of its own. No behaviour changes. Covered by tests/expeditor_test.cpp (TwoCoresComeUpOnOneLogAndEachHoldsTheVolumesOwnImage, TheSharedStructuresAreArmedAboveOneCoreOnly) and tests/drop_table_reclaim_test.cpp.

#### F18. AccessStatsCounters are incremented on every SELECT and read by nothing

`include/kds/stats/access_stats.hpp:46` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-1`, estimated net lines removed 18

Symbols: `stats::AccessStatsCounters`, `CommandDispatcher::access_counters_`, `stats::RecordChainAccess`, `CommandDispatcher::RecordAccessShapes`

Reported again at: `include/kds/server/command_dispatcher.hpp:2658` (slice `dispatcher-hpp`); `src/server/command_dispatcher.cpp:9037` (slice `dispatcher-3`)

**Claim.** CommandDispatcher::access_counters_ (steps_recorded, write_failures) is passed to stats::RecordChainAccess, which bumps it per step, and no code reads either field: there is no accessor, MetaSections prints no access-statistics section, and no test looks at them. access_stats.hpp says failures are counted "so 'the statistics quietly stopped working' is visible rather than assumed", which the tree does not deliver. The struct, the member and the optional parameter are write-only state.

**Evidence as the finder read it.** grep -rn "RecordChainAccess\|access_counters\|steps_recorded" src include tests sim fuzz tools: only access_stats.cpp:17,35 (definition, increment), access_stats.hpp:47,63, command_dispatcher.cpp:9037-9038 (the one caller), command_dispatcher.hpp:2658 (the member). `write_failures` hits elsewhere are TrailRecorder's and Log's own counters. MetaSections (command_dispatcher.cpp:1729-2259) has no access section.

**Proposed fix.** Delete struct AccessStatsCounters, the `counters` parameter of RecordChainAccess (keep the dropped Status as `(void)`), the `if (counters != nullptr)` block in access_stats.cpp, and the access_counters_ member; RecordAccessShapes calls RecordChainAccess(catalog_, chain, now). No reply, refusal or lock order changes. If the operator wants the header's promise kept instead, the other option is two tokens in MetaSections, which is a visible sys.meta change and needs the word. Covered by tests/access_stats_test.cpp and tests/access_stats_across_cores_test.cpp, which read sys.access_stats, not the counters.

#### F19. kWritePathEnforcesAssertions is a constant true with one use that prints 1

`src/server/command_dispatcher.cpp:305` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-1`, estimated net lines removed 6

Symbols: `kWritePathEnforcesAssertions`, `AssertionBuildRun::Publish`

**Claim.** The constant has been true since AST07 and has exactly one reader, the CREATED ASSERTION reply, where `(kWritePathEnforcesAssertions ? 1 : 0)` is always 1. Its comment says it is kept "as a conjunct" with the registry's answer, but no site combines it with anything: the registry-derived `enforcing` value is computed elsewhere (SHOW ASSERTIONS), and this reply's own comment (2904-2908) already argues that a create that returns is enforcing.

**Evidence as the finder read it.** grep -rn kWritePathEnforcesAssertions src include tests: command_dispatcher.cpp:305 (definition) and :2913 (`<< " enforcing=" << (kWritePathEnforcesAssertions ? 1 : 0)`), nothing else.

**Proposed fix.** Delete the constant and its comment block (lines 300-305) and write `<< " enforcing=1"` at line 2913, keeping the comment at 2904-2908 that says why 1 is true there. The reply bytes are unchanged. Covered by tests/assertion_build_test.cpp, tests/assertion_catalog_test.cpp and tests/assertion_recover_test.cpp, which match `enforcing=1`.

#### F20. The bare CREATE TABLE handler opens a DDL scope for a create that can never succeed

`src/server/command_dispatcher.cpp:2382` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-1`, estimated net lines removed 6

Symbols: `CommandDispatcher::HandleCreateTable`, `CommandDispatcher::DdlScopeFor`, `CommandDispatcher::NoteDdlRows`, `catalog::CheckKeystoneColumn`

**Claim.** HandleCreateTable passes an empty catalog::Schema to Catalog::CreateTable, whose first statement is CheckKeystoneColumn, which refuses an empty column list before anything is written. So DdlScopeFor (OpenDdl plus the undo hook), NoteDdlRows and the `CREATED oid=` reply are unreachable or no-ops, and each call still counts a DDL open, then makes FinishDdlStatement invalidate the catalog memo. manual/sql/sql.md:1787-1792 already states the form "can no longer create anything".

**Evidence as the finder read it.** command_dispatcher.cpp:2382-2389: `catalog::Schema schema; DdlScope ddl = DdlScopeFor(scope); auto oid = catalog_.CreateTable(..., schema, ...); NoteDdlRows(ddl); ... return {"CREATED oid=" ...}`. src/catalog/catalog.cpp:1335 `if (Status s = CheckKeystoneColumn(schema); !s.ok()) return s;` is CreateTable's first line; src/catalog/schema.cpp:155-158 refuses `schema.columns.empty()`. EndDdlScopeById (7752-7764) invalidates only when OpenDdl ran. tests/command_dispatcher_test.cpp:504 pins the refusal.

**Proposed fix.** Replace lines 2382-2389 with `const Status s = catalog::CheckKeystoneColumn(catalog::Schema{}); return {ErrorReply(s), false, 0, s};`, keeping InDdlStatement, the namespace resolution and ExistingRelationReply above it. Replies are byte-identical (`EXISTS oid=n`, the namespace refusal, `ERR relation has no columns...`), an explicit transaction is still poisoned by the refusal through FinishDdlStatement, and the pointless OpenDdl and memo invalidation go. Removing the route outright would change the reply to a parse error and needs the operator's word. Covered by tests/command_dispatcher_test.cpp:504, 559, 1272 and tests/catalog_name_rig_test.cpp:85.

#### F21. CommandDispatcher::LogFullPageImage has no caller

`src/server/command_dispatcher.cpp:5394` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-2`, estimated net lines removed 11

Symbols: `CommandDispatcher::LogFullPageImage`

**Claim.** The member is a one-line forward to storage::LogFullPageImage, "kept as a member only for its callers' brevity". It has no callers left: every site in the tree calls the storage:: free function directly, and the dispatcher's own split logging goes through LogTreeSplit and storage::LogBtreeSplit.

**Evidence as the finder read it.** `grep -rn 'LogFullPageImage' src/server include/kds/server tests sim fuzz tools | grep -v 'storage::LogFullPageImage'` returns only the definition (command_dispatcher.cpp:5394), the declaration (command_dispatcher.hpp:2393) and two comments in tests/page_latch_test.cpp (85, 363) about the storage helper. Inside command_dispatcher.cpp the only `storage::LogFullPageImage` call is line 5397, the body of the dead member.

**Proposed fix.** Delete the definition (command_dispatcher.cpp:5394-5398) and the declaration with its comment (command_dispatcher.hpp:2389-2393). Keep `#include "kds/storage/log_page_image.hpp"`: LogTreeSplit still uses storage::LogBtreeSplit from it. No test references the member.

### 3.3 Needless complexity (4)

#### F22. InsertOneRow / InsertOnConflict take a 'refreshable' TableAccess*& that is never refreshed

`src/server/command_dispatcher.cpp:6854` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-3`, estimated net lines removed 8

Symbols: `CommandDispatcher::InsertOneRow`, `CommandDispatcher::InsertOnConflict`, `CommandDispatcher::InsertParsed`, `catalog::Catalog::AdoptRoot`

**Claim.** Both functions take `const catalog::TableAccess*&`, documented as a live borrow the callee re-borrows because a root repoint invalidates the catalog cache. No such refresh exists: InsertOneRow binds `const TableAccess& ta = *ta_ptr` on its first line and never assigns `ta_ptr`; InsertOnConflict only dereferences it. Since PW2/BZ a root move goes through Catalog::AdoptRoot, which updates the cached entry in place. The in/out pointer and three comments describe a hazard and a protocol that are gone, and invite a reader to think `ta` can change under the bulk loop.

**Evidence as the finder read it.** grep -n ta_ptr src/server -> 6854 (param) and 6857 (`const catalog::TableAccess& ta = *ta_ptr;`) only. command_dispatcher.hpp:1649-1652 'the pointer is re-borrowed before returning'; cpp 6210-6212 'refreshed by InsertOneRow when a root repoint invalidates it'; 7230-7231 and 7245 cite 'the desc-page relink below', which no longer exists in the function. src/catalog/catalog.cpp:2977-2988 AdoptRoot: 'this core's cached entry updates in place, so the running statement's TableAccess ... stay valid'.

**Proposed fix.** Change both signatures to `const catalog::TableAccess& ta` (InsertParsed keeps its pointer or a reference and passes `*ta`), drop the `ta_ptr` alias line, and delete/reword the three stale comments (hpp 1649-1652, cpp 6210-6212, 7230-7231/7245). No behaviour, refusal or lock order changes.

#### F23. BorrowChain defaults `blocker` to nullptr but dereferences it unconditionally on a refusal

`src/server/command_dispatcher.cpp:11312` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-4`

Symbols: `CommandDispatcher::BorrowChain`, `CommandDispatcher::BorrowOrWait`, `DispatchOutcome::LockWait`

**Claim.** The declaration gives `std::uint64_t* blocker = nullptr`, yet both refusal arms build the `LockWait` from `*blocker` with no null test. A caller that used the default would crash the first time its relation intention or unit is refused while `may_park_` is set. All three callers pass a pointer, so the default is an option nobody selects and one the body cannot honour. The header comment also says 'the only non-OK status is the cap's', while the memo check returns `TxnConflict`.

**Evidence as the finder read it.** hpp 1136-1138: `StatusOr<bool> BorrowChain(const WriteScope&, const txn::LockKey&, std::uint64_t* blocker = nullptr, txn::LockMode mode = kExclusive);`. cpp 11312 `DispatchOutcome::LockWait{wake.key, *blocker, ...}` and 11369 `LockWait{unit_wake.key, *blocker, ...}`; 11342 returns Status::TxnConflict. grep 'BorrowChain(' in src/include/tests/sim: callers at 10190, 11501, 12826 only, each passing `&blocker`.

**Proposed fix.** Make the parameter required and non-null: `std::uint64_t& blocker` (callers pass `blocker`), forwarding `&blocker` to `TryAcquire`; drop the default. Amend the declaration's sentence to name the memo check's `TxnConflict` beside the cap. No behaviour change for any existing caller. Covered by tests/lock_family_test.cpp, tests/row_wait_wake_rig_test.cpp, tests/fk_parent_hold_test.cpp.

#### F24. DELETE re-reads the slot and heap-allocates a copy of the row per deleted row only to restamp its header

`src/server/command_dispatcher.cpp:12922` - finder's severity **low**, finder's confidence **high**, slice `dispatcher-4`, estimated net lines removed 4

Symbols: `CommandDispatcher::DeleteInner`, `heap::PageView::OverwriteTuple`, `heap::PageView::DeleteMark`

**Claim.** `mark` calls `fresh.ReadTuple(slot)`, copies the payload into a new `std::vector` (`same`) and passes it to `OverwriteTuple` solely to set `trx_id`/`undo_ptr`. The statement already owns those bytes in `payload_copy`, filled from this slot by `qualifies` under the same exclusive leaf hold, and nothing in between writes either - the argument UPDATE states at 10427-10433 when it uses `payload_copy` as its before-image. The re-read adds an error path and one allocation plus one row copy per deleted row on a bulk DELETE.

**Evidence as the finder read it.** 12745 `payload_copy.assign(tuple.value().payload...)`; 12922-12926 `auto reread = fresh.ReadTuple(slot); ... const std::vector<std::byte> same(...); fresh.OverwriteTuple(slot, same, new_trx_id, new_undo_ptr)`. Between them only DecodeAndResolve (reads payload_copy), the borrow/conflict check, CheckTransitionDelete, CheckNoChildrenBeforeDelete, ReserveDelete and AppendUndo run; none takes payload_copy. heap_page.cpp:269-299 OverwriteTuple memcpy's header + payload of unchanged length.

**Proposed fix.** Delete the `reread`/`same` lines and call `fresh.OverwriteTuple(slot, payload_copy, new_trx_id, new_undo_ptr)`. Same bytes written, same statuses from OverwriteTuple/DeleteMark, same WAL record. Covered by tests/txn_session_test.cpp (DeleteMarksRatherThanRemoves..., RollbackClearsADeleteMark, TwoDeletersOnOneRowConflict) and tests/lock_family_test.cpp.

#### F25. ClearReactorBorrows is a guard type for one line ~ServeRuntime can run

`src/server/expeditor.cpp:1502` - finder's severity **low**, finder's confidence **medium**, slice `expeditor`, estimated net lines removed 18

Symbols: `ClearReactorBorrows`, `Expeditor::ServeRuntime`, `CommandDispatcher::set_scheduler_view`

**Claim.** A separate RAII struct, held in an `optional` member of ServeRuntime, exists to run `dispatcher->set_scheduler_view(nullptr)` before the scheduler is destroyed. It needs an explicit constructor and a deleted copy, with a comment explaining them, only to defend against a misuse that being a separately emplaced type creates. ServeRuntime already has a destructor body, which runs before any member - the scheduler included - is destroyed, on every path the guard covers (a failed Start, the tail's `running_.reset()`, `~Expeditor`).

**Evidence as the finder read it.** expeditor.cpp:1492-1518 (the struct and its two comments), 1540 `std::optional<ClearReactorBorrows> clear_reactor_borrows;`, 1819-1820 the install, 1550-1554 `~ServeRuntime()` already detaching the three listeners. `grep -rn ClearReactorBorrows src include tests` -> expeditor.cpp only. `scheduler_view_` is read only for the sys.meta accounting block (command_dispatcher.hpp:2021-2026), and no statement runs during the destructor.

**Proposed fix.** Replace the optional with `CommandDispatcher* scheduler_view_of = nullptr;` in ServeRuntime, set it at 1820 beside `set_scheduler_view(&scheduler)`, and add `if (scheduler_view_of != nullptr) scheduler_view_of->set_scheduler_view(nullptr);` as the first line of `~ServeRuntime`. Delete the struct. The view is still withdrawn before the reactor it names is destroyed on every exit. Covered by tests/expeditor_test.cpp (StartIsRefusedTwiceAndTheRunHalfIsRefusedWithoutIt, and every Start-then-drop cell).

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
Not applied.

The same warning at `src/txn/manager.cpp:504` is not a defect: the rollback
handles `TrailAction::kVarHeapAppend` at line 416, before the `switch`, and
returns.

## 5. What taking this up would mean

`[design]` Three separate pieces, in the order they would run:

1. **Refute or confirm the 32.** Each defect in section 3.1 needs its
   failure scenario traced by a second reader, or a failing test written for
   it; the four the finders rated high (F1 to F4) first. Each deletion in
   3.2 needs a whole-tree grep; each simplification in 3.3 and 3.4 needs its
   sites compared for a difference that would defeat it.
2. **Read the other 27 slices**, or decide the audit stops at the
   dispatcher and the expeditor.
3. **Fix what survives**, one finding a step under the session workflow -
   review, full suite - and stop before the merge.

Several proposed fixes change what a client sees (a refusal where a
statement is accepted today, a different status code) and are the
operator's to rule on before any code, not a reviewer's to wave through:
at least F1 to F4 and the `PURGE` qualifier, `NOW()` and `ON CONFLICT`
entries.
