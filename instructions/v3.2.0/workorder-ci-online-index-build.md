# Work order CI — an online index build: `CREATE INDEX` without the relation `X` across its backfill

Written 2026-10-23 on `worktree-online-index-build` from `2f7ce828`
(`v3.4.3-1-g2f7ce828`), on the operator's word, recorded in English
(`raft-marks-2026-10-23.md` §4):

- **W0**: *"index build online, write milestone"*.

**Written 2026-10-23. Opened 2026-10-23** on the operator's standing
go-ahead (go-ahead-achieving-milestone, `raft-marks-2026-10-23.md` §7),
from `dd437a76` (`main` with this order on it). **CI-Q0..Q10 were each
adopted as CLA proposed, on the operator's standing go-ahead of 2026-10-23
(go-ahead-achieving-milestone)**: Q0 (a), Q1 (b), Q2 (a), Q3 (b), Q4 (a),
Q5 (a), Q6 (a), Q7 (a), Q8 (b) if S0 shows it sound, else (a), Q9 (a),
and Q10 (a) if S0 finds no shared state, else (b). None is the operator's
own per-item mark. Under the standing word, CI-Q0 (a)'s mark after S0 is
CLA's verdict, adopted the same way. CI-S3 is not built, since CI-Q6 is (a).
CI-S0 is a census and a premise measurement (§5), and nothing is built
before it reports.

**The opening review is applied** (`critics-developer`, 2026-10-23, after
`11cc5b53`). Each design finding was checked by an independent refuter:
the private backfill root, the waiting `S` that keeps no writer out, the
drain replacing `BorrowChain`'s proof, and the tree logged at H2 before
any slot names it all stand. The unlogged flag clear was refuted, since
the clear goes through `OverwriteLogged`, and its wording was corrected.
CI-R6 is added, CI-Q3's proposal moves from (a) to (b), and §3.2 gains
H3's refusal rule and notes the tree owed at H3.

**Claim tags**: `[operator]` for W0; `[source-read]` with `path:line` at
`2f7ce828`; `[design]` for reasoning about code not yet written;
`[quiet-wrong]` where a wrong choice turns a correct answer into a wrong
one with no error.

**The letter.** `[source-read]` CH is the last work order under
`instructions/v3.2.0/` on `origin/main` at `2f7ce828`, no worktree in
`git worktree list` names a `CI`, and no commit subject on `origin/main`
begins with the token. CLA's choice.

**CI against other orders.**
- CI changes how `CREATE INDEX` holds its relation (`index.md` §10, AT-S5e).
  It keeps the backfill (IX10a), the append-only maintenance (IX2), the
  WAL rule (IX13) and the reclaim of an owed tree (BY) as they are, and
  says where each one is asked again.
- `REINDEX INDEX` (BY-R6) is a drop and a create under the same `X`, so
  an online `REINDEX` is CI-Q9's question. CI does not change it by
  default.
- A unique index's build check (BX-R7) rests on the `X`. CI-Q6 decides
  whether an online unique build is in scope.
- Cabin's announce (`cabin.md` §6) is the engine's existing precedent:
  an unservable empty set is installed first, so a write made during the
  walk is in it. CI does the same for an index.
- Not part of AR0 §8's chain.

---

## 0. What CI is

Today `CREATE INDEX` takes the relation `X` before its first catalog write
and holds it through the backfill, the logging of the tree and the decide.
Every writer of the relation, and every positioned reader, waits on it for
however long the backfill runs (`index.md` §10). CI is a build that holds
no `X` across the walk:

1. **Announce.** The `sys.indexes` row and its anchor slot are written
   first, flagged *building*. From that moment every writer maintains the
   index, and no reader's compile chooses it.
2. **Drain.** The build waits until no writer still runs on an index list
   that predates the row.
3. **Backfill.** The existing walk (IX10a) runs while writers keep
   writing. Each of its entries is a duplicate of a writer's entry or one
   no writer will make.
4. **Ready.** The *building* flag is cleared. From the next schema word
   on, a compile may choose the index. The create then decides.

Steps 1-4 come from append-only maintenance. An entry is never removed
(IX2), a byte-identical entry is reported rather than stored twice (IX4b),
and a surplus entry is dropped at read time (IX1). So a writer's appends
and the backfill's appends compose, whatever their order, without any of
the delete bookkeeping a non-append-only index would need.

---

## 1. The tree as read `[source-read]` at `2f7ce828`

**What the `X` does today, piece by piece.**
- **Writers see the new index.** `HandleIndex` takes the `X` before the
  first catalog write (`src/server/command_dispatcher.cpp:2796`). A
  writer resolves the relation's indexes at its task boundary and asks
  `IX` after that; its first intention re-checks the schema word and runs
  the statement again if it moved (`BorrowChain`, `:10631-10655`), whose
  proof is that no DDL can take the relation until the writer decides.
  So the grant is the moment no writer is mid-statement, and a writer
  arriving later parks, then re-resolves (the comment at `:2753-2765`).
- **The leaf cursor resumes at a leaf id.** `exec::LeafCursor` keeps the
  next leaf's id across a yield. Its header says this is *"safe only
  under the relation's `X`... no writer can split, grow or retire a leaf
  of the relation meanwhile"* (`include/kds/exec/leaf_cursor.hpp:12-18`).
- **The tree is unlogged until the end.** The backfill appends with no
  record. `Finish` logs the whole tree as full page images
  (`LogBuiltTree`, `src/exec/index_ddl.cpp:628`), and only then writes the
  row (`:634`). A failed build frees the unlogged tree page by page
  (`FreeUnbuiltTree`, `:617`, and a build destroyed unfinished, `:478`;
  BX-Q6 (b)). Both depend on no writer having
  logged an entry into that tree.
- **The unique check sees a frozen relation.** `CheckBuiltKeyUnique`
  checks under an everything-visible view, *"under the build's relation
  `X`... every version on a page is committed or the build's own"*
  (`src/exec/index_ddl.cpp:70-75`, `:329`).
- **The backfill owns its root.** `Begin` formats a root into the build's
  own `IndexRef` (`src/exec/index_ddl.cpp:570-576`), `AppendVersion` keeps
  a root move there and nowhere else (`:197-200`), and the slot and the row
  receive the final root at `Finish` (`:632-645`). Writers take the root
  from the anchor slot (`src/catalog/catalog.cpp:2796-2810`) and publish
  their growth to it (`:2973-2981`), and a descent from a root grown over
  is refused retryable (`src/storage/index/index_tree.cpp:191-200`,
  `:285-293`).
- **The owed tree is noted after `Finish`.** `NoteOwedIndex` runs in
  `Tail` once `Finish` succeeds (`src/server/command_dispatcher.cpp:2566-2577`),
  and `EndDdlScopeById` queues only what was noted (`:7141-7150`). A refusal
  inside `Finish` after the tree is logged seeds the slot best-effort
  (`src/exec/index_ddl.cpp:636-647`).
- **Readers don't run during the build.** A positioned reader's `IS`
  waits on the `X`, so no compile runs against the relation while the
  row exists and the tree is incomplete.

**What already works without the `X`.**
- **An unfiltered catalog read sees an open create's row.**
  `ScanAll`'s null-view rule: *"an object exists from the moment its row
  is written until its removal commits"*. It was written so that a
  session's own uncommitted `CREATE INDEX` is maintained by its own
  `INSERT`s (`src/catalog/catalog.cpp:56-65`). `InitTableAccess` builds
  `TableAccess::indexes` this way (`ListIndexes()` with no view,
  `src/catalog/catalog.cpp:2785`, `:4202-4203`), and `MaintainIndexes`
  loops over that list (`src/exec/index_maintain.cpp:369`, `:376-378`),
  so another core's writer would maintain an index whose row is down and
  undecided.
- **The schema word moves when the row is written.** `CreateIndex` calls
  `BumpVersion("sys.indexes create")` at the write
  (`src/catalog/catalog.cpp:4107`), not only at the decide, so every core
  drops its memo at its next task boundary.
- **The compiler and the write hook read one list** (`TableAccess::indexes`,
  `index.md` §12, sorted by oid). The compiler also reads `index_mask`
  (`src/catalog/catalog.cpp:2826-2829`, `src/exec/step_compiler.cpp:965-983`),
  and the Cabin DDL `FindIndexOnColumn` (`src/exec/cabin_ddl.cpp:209`). The
  plan memo stores a join order only and recompiles through the full
  ladder (`include/kds/exec/plan_memo.hpp:17-27`).
- **The lock family has `IS`, `IX`, `S` and `X`, and no `SIX`**
  (`include/kds/txn/lock_table.hpp:214-219`, `:232-247`). `S` conflicts with `IX`
  and `X`, so a held `S` admits readers and keeps writers out. **A waiting
  `S` keeps nobody out**: the grant tests holders only and keeps no queue
  order (`src/txn/lock_table.cpp:213-224`, `:290-296`), so an `IX` asked
  while an `S` waits is granted. A unit's holders are in its partition
  entry (`Entry::holders`, `:186-196`).
- **A dropped or rolled-back index's tree is reclaimed** through its
  anchor slot by an index job (BY-R1..R5), gated on the durable redo
  start and every statement epoch. That path does not need the tree to be
  unlogged.

---

## 2. Rules CI keeps `[design]`

- **CI-R1 - complete or absent, for every reader.** A compile chooses an
  index only once the index holds an entry for every version any live or
  future view can reach (IX1, IX10a). Breaking this is `[quiet-wrong]`:
  a probe that misses a row looks like an empty answer.
- **CI-R2 - no write is missed.** Every row version written after the
  drain has its entry from its writer. Every version that exists at the
  drain has its entry from the backfill's copy or its undo walk. Every
  transaction granted `IX` before H3's schema-word move decides before the
  backfill starts. The drain replaces `BorrowChain`'s proof (§1), which CI
  breaks: H3 moves the word while writers hold `IX` and have passed their
  first-intention check. `[quiet-wrong]` if broken.
- **CI-R3 - the log replays what readers saw.** Every page of the building
  tree that a writer's `INDEX_INSERT` or `BTREE_SPLIT` names is described
  in the log before that record, so redo never meets a record against a
  page it has no image of. IX13's order stands.
- **CI-R4 - a failed or cancelled build owes its tree.** Once a writer may
  have logged into the tree, the tree is freed only by BY's index job:
  through the slot, after the rollback, past every statement epoch. Online,
  nothing calls `FreeUnbuiltTree` after H2: neither `Finish` (`:617`) nor
  `~IndexBuild` (`:478`). The tree is noted owed at H3, so every ending
  queues it (§3.2).
- **CI-R5 - existing paths unchanged.** Every statement that is not an
  online build answers byte for byte as at `2f7ce828`. A writer pays
  nothing: `MaintainIndexes` does not read the flag (§3.1). The compile's
  one extra branch is what the close measures.
- **CI-R6 - one root, the slot's.** The backfill holds no root of its own.
  Every append resolves the index's root from the anchor slot, as
  `InitTableAccess` does, and a divide that grows a level publishes the new
  root as a writer's does (`IndexWrite::new_root` under the anchor pin,
  through the log's `StageAnchorRoot`/`AdoptRoot`). `AppendVersion`'s
  private adoption is not used online. A stale-root `TxnConflict` at the
  backfill re-reads the slot and retries the append; it does not fail the
  build. Without this, writers and the backfill grow two roots and each
  refuses the other's keys.
- **CI-R7 - a superseder in flight at the copy is not trusted resolved
  (§3.5 A1).** `ResolvedForEveryReader` answers true for an aborted
  transaction below the floor. That answer is about a version still on a
  page. So if a row's head writer was undecided when phase 1 copied it,
  the backfill never stops at that writer or excuses a failed read below
  it on a later answer. Where it would, it reads the row again by tuple
  key under its leaf's hold and indexes what it finds, at most
  `storage::kMaxDescentRestarts` times, then refuses `TxnConflict`.
  `[quiet-wrong]` if broken.

---

## 3. Design `[design]`

**3.1 The building flag.** `kIndexFlagBuilding` (0x2) goes in
`SysIndexRow::flags` (IX12), beside `kIndexFlagUnique`.
`TableAccess::IndexRef` carries it. `MaintainIndexes` ignores it: a
building index is maintained like any other. A unique one's writer check
(`CheckUniqueKeys`, `src/server/command_dispatcher.cpp:10883-10931`) is
CI-Q6's. Every probe-choosing path skips an index that carries it (§3.5
A8 names them). `step_vm.cpp:1379` re-resolves an index by oid at
execution, so a compiled chain never meets a flag flip mid-statement. A
superblock bump is CI-Q5's.

**3.2 The order of acts.** Everything runs in one autocommit transaction
(CI-Q2), and the build holds the relation `IS` from head to decide, so
no `DROP TABLE`, `ALTER` or other `X`-taking DDL lands under it:

| step | act | holds |
|---|---|---|
| H1 | `PrepareIndexDef`'s checks and the pre-issued oid, as today | `IS` |
| H2 | format the root, log it as a full page image | `IS` |
| H3 | write the anchor slot, then the row flagged building (BY-R10's order), and note the tree owed; the schema word moves | `IS` |
| D | the drain (CI-Q3), started only after H3's word move | `IS` |
| B | the backfill: `LeafCursor` slices, each append logged and each root move published (CI-Q4, CI-R6) | `IS` |
| U | the unique check, if CI-Q6 admits one | `IS` |
| R | clear the flag (CI-Q7); the schema word moves | `IS` |
| C | the decide | - |

**H3's refusal before the slot.** `CreateIndex` can refuse after H2 has
logged the root and before the slot is down: `CheckIndexDef` again
(`src/catalog/catalog.cpp:4032`) - a name another core took after H1, or
anchor room a concurrent build took (CI-Q10) - or a failed
`WriteAnchorRoot` (`:4074`). The online path keeps `Finish`'s rule: on such
a refusal it seeds the slot best-effort and owes the tree from it; a seed
that fails leaks the root page. A crash between H2 and H3 leaks the root
page, as today's window between `LogBuiltTree` and `CreateIndex` does.

**3.3 What the backfill meets that it did not meet under the `X`.** S0's
census answers each item with `path:line`:
- a leaf divided after its copy, or after the cursor read `next`:
  rows only ever move right, to a new sibling linked after the leaf, so
  the forward walk reaches them or has copied them. The census confirms
  that no path moves a row left or frees a live relation's leaf
  (compaction in place, `PURGE`'s `SLOT_RETIRE`, BZ's deletions);
- a row whose current version is undecided: its entry is appended (a
  surplus, at worst) and its undo walked as today;
- a version superseded between the copy and the walk: its writer's hook
  appended the new version's entry, and the backfill's copy covers the
  old one;
- undo recycled under the walk: IX10a's `ResolvedForEveryReader` stop,
  already written for another core's purge;
- the index's root moving under the backfill (CI-R6): a writer's divide
  growing the building tree between two backfill appends, and the backfill
  growing it under a writer whose memo names the old root - where each side
  re-reads the slot, and that neither exhausts `kMaxDescentRestarts` or the
  grown-over refusal into a failed build or a writer refused every time;
- the build's own transaction and the purge horizon: whether an open
  build pins `ReadHorizon()` for the whole walk, and so the undo log's
  growth. CI-Q8 depends on the answer.

**3.4 Logging.** Today a backfill's appends are unlogged, and `Finish`
logs the tree whole. That cannot stand (CI-R3): a writer's `INDEX_INSERT`
into a building page must not be replayed against a page whose image comes
later in the log or never. CI-Q4 chooses how the backfill logs.

**3.5 What CI-S0's census adds** `[source-read]` at `dd437a76`. These are
amendments to the design above, each one a change S1 or S2 builds. The
census is in §7.
- **A1 - a superseder that loses (CI-R7, `[quiet-wrong]`).** A writer W
  whose id is below the build's B (ids come from per-core blocks,
  `include/kds/txn/trx_id.hpp:64`) can take its `IX` after the H3
  snapshot, run while the backfill copies its row, and then abort.
  Abort restores the prior image in place and appends no index entry
  (`src/txn/manager.cpp:497-560`). Once the floor passes W,
  `ResolvedForEveryReader(W)` answers true (`manager.cpp:839`). That
  answer is sound only for a version still on a page
  (`instance_visibility.hpp:546-551`). The backfill's staged copy of a
  loser's version is not on the page. So the walk stops at W, or excuses
  a failed read of W's recycled undo, and the restored version gets no
  entry from anyone. Under the `X` no writer was undecided.
- **A2 - the walk's end.** Writers append leaves at the tail while the
  cursor chases them, and a backfill slower than the stream would run
  into `kMaxPageWalkLength` and be refused as `Corruption`
  (`include/kds/storage/visit.hpp:55-67`). After D the backfill takes a
  high-water key K: the chain's last leaf, `max(last keyed slot,
  min_key)`. A leaf whose `min_key > K` ends the walk, because every row
  past K was written after the drain by a writer that maintains the
  index. Leaves are never freed, retyped or moved left
  (`src/storage/btree/btree.cpp:185-188`, `:579-591`, `:839-881`), and
  SELECT already resumes at a leaf id under `IS`
  (`src/exec/step_vm.cpp:2096-2116`).
- **A3 - the IS-then-IX gap in `BorrowChain` (`[quiet-wrong]`).**
  `first_intention = !holdings.Holds(relation)`
  (`src/server/command_dispatcher.cpp:10990`) is true only for a
  transaction holding the relation at no mode. A transaction that took
  the relation's `IS` earlier, as a foreign-key parent read does
  (`:4285`), skips the schema-word re-check on its later `IX`. The
  drain's `IX` snapshot also misses it. The test becomes "holds no mode
  covering `IX`" (`LockHoldings::HoldsCovering`).
- **A4 - the build's `TableAccess` across H3.** `BumpVersion` drops the
  cache (`src/catalog/catalog.cpp:1042-1044`). An `access_` filled at
  `Begin` dangles after H3 unless a `MemoPin` held from before it keeps
  the generation. Only schema, layout, oid and anchor are read from it,
  and the root comes from the slot (CI-R6).
- **A5 - the rolled-back tree's LSN gate.** `EndDdlScopeById` gates an
  orphaned create at `appended_lsn()` at the rollback
  (`command_dispatcher.cpp:7481-7495`). Online, a peer writer whose
  statement began before the rollback keeps logging into the tree above
  that LSN until its statement ends. The epoch gate keeps it off a freed
  page. But the durable redo start can then fall between the gate and the
  peer's last record, and a crash replays an `INDEX_INSERT` on a freed
  page (`src/wal/redo.cpp:420-433`). The reclaimer samples the gate again
  once the epochs pass (`src/server/page_reclaim.cpp:137-145`).
- **A6 - the horizon.** The build's own transaction pins `ReadHorizon()`
  through its view (`src/txn/manager.cpp:143`, `:701-706`). Its in-flight
  id holds the floor, and with it every core's undo recycling and commit
  window, whichever CI-Q8 option is taken
  (`include/kds/txn/manager.hpp:309-323`, `src/txn/undo_log.cpp:66-77`).
  CI-Q8 (b) moves the horizon only if the build's scope runs at
  `kReadCommitted`: `StartStatement` does nothing under REPEATABLE READ
  (`manager.cpp:177`).
- **A7 - the writer's refusals during a build.** A writer whose memo
  names a root the backfill has since grown over is refused
  `TXN_CONFLICT retryable=1`. Index-tree refusals are not re-run
  (`include/kds/storage/insert_placement.hpp:70-79`), and an explicit
  transaction is poisoned. CI-Q11 decides this.
- **A8 - the read side.** The paths that must skip a building index are
  `IndexProbeOf` and `CorrelatedIndexProbeOf`
  (`src/exec/step_compiler.cpp:1118`, `:1206`), `TableAccess::IndexOn`
  (`include/kds/catalog/schema.hpp:559-563`), the join cost model
  (`src/exec/join_order.cpp:203`), the ON CONFLICT arbiter
  (`command_dispatcher.cpp:6228`) and `FindIndexOnColumn`
  (`catalog.cpp:4287-4297`). `index_mask` has no production reader
  (`schema.hpp:550`; only `tests/catalog_test.cpp` reads it), so it is
  deleted. `SHOW INDEXES` reads under the session's view, so a building
  row is never visible to another session there. §3.1's "prints the
  flag" is struck. `CheckIndexDef` refuses `kIndexFlagUnique` together
  with `kIndexFlagBuilding`, which enforces CI-Q6 (a) at the catalog.
- **A9 - the drain's machinery.** Holders are exact under the partition
  latch (`src/txn/lock_table.cpp:176-177`, `:306`, `:530-549`), and a
  relation `IX` is never released before the decide
  (`command_dispatcher.cpp:4416-4424`, `:6202`, `:6759`, `:11247`). The
  lock table gains one public read: the relation's `IX` holders other
  than the build, with a wake re-armed in the same latch section. The
  drain draws a wait-for edge to the holder it waits on, so a cycle
  through it is refused as a deadlock. Its net restarts each time the set
  shrinks.
- **A10 - the borrow.** `BorrowRelationForDdl(..., kIntentionShared)`
  already exists for `CREATE TABLE`'s parent (`command_dispatcher.cpp:4896`).
  After the grant the head calls `Revalidate()`, which no index head does
  today. `PrepareIndexDef` takes the borrowed oid instead of resolving
  the name a second time (`src/exec/index_ddl.cpp:345-353`). Both
  windows are open on the `X` path too, and both are closed there as well.
- **A11 - the cost while a build is open.** `AnyDdlOpen()` stays true
  instance-wide for the build's length, so every statement head on every
  core resolves names under a minted view (`command_dispatcher.cpp:7338`).
  Under the `X`, the relation's writers were parked and did not pay this.
  The close measures it.
- **A12 - log volume.** A logged backfill writes about `3 +
  f·align8(36+E)/E` times `LogBuiltTree`'s bytes: one `INDEX_INSERT` per
  entry, plus a three-image `BTREE_SPLIT` per new leaf. For E = 17 and
  f = 0.69 that is about 5x. This is computed from
  `include/kds/wal/record.hpp:266-284` and `payload.hpp:257-456`, not
  measured, because nothing logs a backfill yet. The close measures it.

---

## 4. Questions - not marked

**CI-Q0 - who decides after S0.**
- (a) The operator marks the build after S0's report.
- (b) S0's verdict decides, and the stages run on the standing go-ahead.

**CLA proposes (a)**: CI-R1 and CI-R2 are `[quiet-wrong]`, and S0 is
where they are shown.

**CI-Q1 - the spelling, and what happens to the `X` build.** Ranked
removal first:
- (a) **One build.** Every `CREATE INDEX` is online. Inside `BEGIN` it is
  refused `NotImplemented`, as `REINDEX` is (BY-Q4 (a)). This removes the
  `X` across the backfill. It removes `LogBuiltTree` and `FreeUnbuiltTree`
  only if CI-Q6 and CI-Q9 also go online; under their (a) a unique build
  and `REINDEX` keep the `X` build. What it reaches: `CREATE INDEX`
  inside an explicit transaction (`ddl-transactional.md`, the DT tests,
  BY-R8's `DROP INDEX x; CREATE INDEX x` in one transaction), and
  `index.md` §10 and §10.1.
- (b) **Autocommit is online, `BEGIN` keeps the `X` build.** No new
  word. An explicit transaction's `CREATE INDEX` keeps today's guarantee
  and today's code.
- (c) **A keyword**: `CREATE INDEX ONLINE ...` online, everything else
  as today. Nothing is reserved (IX10).

**CLA proposes (b)**: under (a) a transaction that creates a table and its
index together could no longer do so, which is what `ddl-transactional.md`
builds on. (b) and (c) both keep two build paths; they differ in who
picks. Under (b) the transaction's shape picks, with no word to learn;
under (c) the client has to ask for the path that does not stall it.

**CI-Q2 - the transaction.** Steps H1 to C:
- (a) One autocommit transaction. A crash or a refusal after H3 rolls the
  row back, and BY's job reclaims the tree through its slot; between H2
  and H3, §3.2's refusal rule applies.
- (b) Several transactions, PostgreSQL's `CONCURRENTLY` shape: the row
  committed building, a failure leaving an invalid index for a `DROP`.

**CLA proposes (a)**: (b) adds a durable *invalid* state that a client
has to find and drop. (a) uses BY's owed-tree path and leaves nothing
behind.

**CI-Q3 - the drain.** How the build waits out writers on an older list:
- (a) Take the relation `S` once the row is down, then release it at the
  grant. Every `IX` holder has decided by then. But a waiting `S` does not
  stop an arriving `IX` (§1), so under overlapping writers the `S` may
  never be granted: the statement's 1 s fault net is set once
  (`src/server/command_dispatcher.cpp:612-615`), writers report no holder
  progress, and the build is refused `TxnConflict` under the steady write
  load CI is for. Sound under that load only with a new primitive - an
  `IX` refused while an `S` waits.
- (b) **After H3's word move**, snapshot the relation's `IX` holders from
  their partition entries and wait for each one's decide. New arrivals are
  not blocked, and the set only shrinks, so it cannot starve. The order
  is load-bearing: a snapshot taken before the move misses a writer
  granted `IX` between the two, which then passes its first-intention
  check on the old word and writes on a list without the index
  (`[quiet-wrong]`, CI-R2). After the move, a grant the snapshot misses
  meets the moved word at its first intention and runs again.
- (c) Wait for every transaction in flight at H3, instance-wide
  (`InstanceVisibility`). No lock-table change, but it waits on writers of
  other relations.

**CLA proposes (b)**: it is the only option that neither starves under
steady writes nor waits on other relations, and the holders are already in
the unit's entry, so naming them is a read under one partition latch. S0
item 3 checks that read against a grant in flight. A holder transaction
open longer than the fault net still bounds the drain; how is CI-S2's.

**CI-Q4 - how the backfill logs.**
- (a) Each backfill append logs as a writer's does: `INDEX_INSERT`, or
  `BTREE_SPLIT` with its images, under the leaf's hold (AT-S21's rule).
  The root's image is logged at H2, and root moves are published (CI-R6).
  `LogBuiltTree` is not used online.
- (b) The backfill appends unlogged into a private tree that no writer
  touches, while writers append into the published tree. The two are
  merged at R.
- (c) As today, unlogged with a whole-tree image at the end, with writers'
  records held back until then.

**CLA proposes (a)**: (b) is a second tree and a merge, and (c) holds a
writer's commit on the build's end, which brings back the stall CI is
written to remove. (a) costs a record per entry, and every leaf the
backfill grows is born in a `BTREE_SPLIT` carrying images, so it logs more
than the whole-tree images `LogBuiltTree` writes; for a narrow entry the
record header is wider than the entry. S0 item 5 measures it.

**CI-Q5 - the superblock.** `kIndexFlagBuilding` is a new meaning for a
`flags` bit an older binary reads as an ordinary index.
- (a) Bump the superblock (34) and refuse every older volume, by the
  standing order of 2026-10-07.
- (b) No bump: a row flagged building exists only inside an undecided
  transaction, which recovery rolls back.

**CLA proposes (a)**: the standing order applies whatever (b) argues.

**CI-Q6 - a unique index online.** A writer's check during the build walks
an incomplete run, so it can admit a value an unwalked row already holds.
- (a) `CREATE UNIQUE INDEX` stays on the `X` build in CI.
- (b) Online, and U finds every duplicate. Each row's latest version is
  checked after the backfill under the `kIndexKey` unit, and a duplicate
  refuses the create, naming both tuple keys (BX-R7's message). Writers
  under the building flag check as today (`CheckUniqueKeys`) and may be
  refused by an entry
  the backfill has already appended.
- (c) Online, and writers do not check a building unique index at all.
  U alone decides.

**CLA proposes (a) in CI**, with (b) as a later stage once S0's census
has an answer for a writer and U racing on one key.

**CI-Q7 - ready.** How the flag clears:
- (a) An in-place overwrite of the row's flags byte under the create's
  transaction, through `OverwriteLogged` (`src/catalog/catalog.cpp:282`):
  an ordinary `HEAP_OVERWRITE` before the decide, as RV3 requires of every
  catalog write (`include/kds/catalog/catalog.hpp:160-167`; redo
  `src/wal/redo.cpp:331`), so no new record kind. Then the schema word
  moves. It needs no undo record: a rollback, live or at mount, retires
  the row whole through its insert's undo.
- (b) Readiness is the commit: a compile chooses an index only when its
  row's writer is decided. No overwrite, but a reader then has to tell
  "decided" from "in flight" on every compile.

**CLA proposes (a)**: the compile reads one bit. Between R and C the
index is complete and maintained, so a reader that chooses it is right.

**CI-Q8 - the build's view and the horizon.** The backfill reads no view,
only `ResolvedForEveryReader`; the question is the statement's registered
view. If S0 finds that it pins `ReadHorizon()` for the whole walk:
- (a) Accept it and state it: a long build holds undo, as a long reader
  does.
- (b) Re-mint the statement's view per slice, so the horizon moves between
  slices. `DeclarationBuildRun` does this, but justifies it under the `X`
  (`src/server/command_dispatcher.cpp:2597-2602`), so S0 has to show it
  sound under `IS`.

**CLA proposes (b) if S0 shows it sound, else (a)**.

**CI-Q9 - `REINDEX` online.**
- (a) Out of CI: `REINDEX` keeps its `X` build.
- (b) In CI: the new tree is built online under a new oid (BY-Q2 (a)),
  the old one stays chosen until R, and R clears the new row's flag and
  delete-marks the old row in one act.

**CLA proposes (a)**: (b) needs CI-Q1's path to land first, and it is a
stage of its own.

**CI-Q10 - two builds on one relation.**
- (a) Admitted: each holds `IS`, and neither drain waits on the other's
  `IS`. A second build's H3 can be refused for anchor room the first took
  (§3.2).
- (b) Refused `TxnConflict` while another online build on the relation
  is open.

**CLA proposes (a) if S0 finds no shared state between two builds,
else (b)**.

**CI-Q11 - a writer refused by a root the backfill grew (§3.5 A7).**
Written at S0. The backfill grows the building tree's root a few times,
mostly early on. Each growth can refuse one statement per writer task on
another core, `TXN_CONFLICT retryable=1`, and that poisons an explicit
transaction.
- (a) Accept it and state it: retryable, and bounded by the tree's depth
  per build.
- (b) In `AppendIndexEntry`, when `IndexInsert` refuses `TxnConflict` and
  an anchor is given, read the index's slot again. If it names another
  root, retry once from it. The refused insert wrote nothing
  (`src/storage/index/index_tree.cpp:263-268`, `:288-294`), and a moved
  slot proves that a level grew. This also ends today's
  writer-on-writer growth race with a placement instead of a refusal.

**CLA proposes (b)**: the purpose of CI is a writer that keeps writing
through a build, and (a) leaves that writer's explicit transaction to be
poisoned by a growth it cannot see. (b) costs nothing on the path that is
not refused, and turns a retryable refusal into a placement the descent
already proves correct. Under CI-R5, only statements refused by that race
change, from a refusal to a success.
**Adopted on the operator's standing go-ahead of 2026-10-23
(go-ahead-achieving-milestone).**

---

## 5. Stages

Every stage is reviewed by `critics-developer` and gated by the full
suite. The overhead is measured once, at the close, inside the budget of
2026-10-13: about an hour, building included.

**CI-S0 - the census and the premise.** No engine code reaches `main`.
1. **The premise, measured.** A writer's latency while `CREATE INDEX`
   backfills a populated relation, against the same writer with no build,
   in `build-release` at `2f7ce828`. A relation sized so the build runs
   10-30 s. The p0..p100 table, and the writer's stall, which today is
   the backfill's length. Inside 20 minutes.
2. **The census of §1 and §3.3**, item by item, with `path:line`: each
   thing the `X` provides, and what provides it without the `X`. An item
   with no answer sinks the design, or narrows it.
3. **CI-Q3's enumeration**: reading a relation unit's `Entry::holders`
   consistently with a grant in flight, after H3's word move.
4. **CI-Q8's horizon reading.**
5. **CI-Q4's log volume**: the records and split images a logged backfill
   writes, against `LogBuiltTree`'s images, on S0's relation.

The verdict goes in §7: build, narrow or drop.

**CI-S1 - the flag and the read side.** `kIndexFlagBuilding`, its
encoding, the superblock (CI-Q5), and every probe-choosing path skipping
it. Tests: a building index is maintained by every write path on every
core, and no compile chooses it (each path that reads `TableAccess::indexes`
for a probe, `FindIndexOnColumn`, the ON CONFLICT arbiter and the join
planner's model included). `index_mask` is deleted (§3.5 A8), and so is
`BorrowChain`'s IS-then-IX gap (A3).

**CI-S2 - the online build.** H1..C (§3.2), the drain (CI-Q3), the logged
backfill (CI-Q4), the ready flip (CI-Q7), and the refused and cancelled
paths through BY's job (CI-R4). Tests:
- a writer on another core inserting, updating and deleting during the
  backfill, then every row found through the index under an old view and
  a new one (CI-R1, CI-R2), in the index contract suite's configuration
  comparison;
- a leaf divided under the cursor, at the leaf being copied and at the
  next;
- a writer on another core growing the building index's root mid-backfill,
  and the backfill growing it mid-writer; afterwards the slot names the
  tree's true root and every row is found through it (CI-R6);
- a crash at each step H3..C in the simulation harness, mount, and the
  tree reclaimed or the index whole; a crash at H2 leaks at most the root
  page; a refusal at H3 before the slot leaves the tree owed and reclaimed;
- a cancel at a slice boundary, and a refused `Tail`, each with the tree
  owed and reclaimed without a mount;
- the drain under a stream of short writers, which completes, and the
  drain against a writer transaction open past the fault net.

**CI-S3 - unique online**, only if CI-Q6 (b) or (c) is marked.

**CI-S4 - the close.**
- **The close measurement**: S0's premise again on the closing commit
  (the writer's stall during a build), and an ordinary `INSERT`/`UPDATE`
  on an indexed relation with no build running, both interleaved A/B in
  `build-release` against `2f7ce828`.
- **The texts**: `index.md` §10, §10.1, §12 (IX12's flags) and §12.2,
  `ddl-transactional.md`, `sched.md` §3's build paragraph,
  `leaf_cursor.hpp`'s header, `manual/sql/sql.md`, `known-gaps.md`, and
  CLAUDE.md's secondary-index row.

---

## 6. Out of scope

What CI does not reach:
- `REINDEX` online, unless CI-Q9 (b) is marked;
- `CREATE ASSERTION` and `CREATE TRANSITION`, which share `BuildRun`'s
  `X` and are not indexes;
- reclaiming superseded entries, and any change to IX2 - CI depends on
  append-only maintenance and would have to be read again if it changed;
- an index-only scan, partial, expression or descending indexes
  (`index.md` §11, §13);
- a Cabin build, which already announces (`cabin.md` §6).

---

## 7. Stage log

**CI-S0 - the census, 2026-10-23**, on `worktree-online-index-build` at
`dd437a76`, documents only. Nine read-only census readers, one per item:
the leaf chain under `IS`, the backfill against concurrent writers, the
root (CI-R6), the drain, the horizon (CI-Q8), the logging (CI-Q4), the
read side, the failure and reclaim paths, and the dispatch and lock
shape with CI-Q10 and the superblock. **Every item came back sound with
a named change**, and §3.5 A1-A12 records each change. Two of them are
`[quiet-wrong]` holes in the design as written: A1, the superseder that
loses (CI-R7 added), and A3, the IS-then-IX gap in `BorrowChain`. A1 was
found independently by two readers. The others are a crash-time redo
refusal (A5), a use-after-free (A4), a liveness bound (A2), and costs
(A6, A11, A12).
- **CI-Q8** resolves to (b), with the scope at `kReadCommitted` (A6).
- **CI-Q10** resolves to (a). The two builds share only the name and the
  anchor room, both refused at H3 under page holds
  (`src/catalog/catalog.cpp:4085-4093`, `:3957-3965`). Neither build is
  in the other's drain, because the snapshot takes `IX` holders only.
- **PURGE and `PURGE DROPPED COLUMNS` run during the backfill.** Both
  take `IX`, so the drain waits for them. A slot retire happens in place,
  only for a row resolved for every reader, and leaves at most a surplus
  entry (`command_dispatcher.cpp:12105-12115`). CD rewrites only dropped
  cells, which no index holds.
- **CI-Q11** was written at S0 and adopted as (b).

**Verdict: build**, with A1-A12, on CI-Q0 (a), whose mark here is CLA's
verdict, adopted on the operator's standing go-ahead of 2026-10-23
(go-ahead-achieving-milestone). S0 item 5 (log volume) is computed
(A12), not measured, and is measured at the close. The premise
measurement is S0 item 1, below.

**CI-S0 item 1 - the premise, measured** at `v3.4.3-10-gdd437a76`
(`bench/v3.4.3/results-ci-s0-premise-v3.4.3-10-gdd437a76.md`, driver
`tools/ci_s0_premise.py`). Setup: `cores=4`, `group`, 10M rows, and
`CREATE INDEX` on one int64 column. **The premise holds for writers.**
- One writer statement waited out the whole build each time, released
  within 1 ms of the build's end. The stalls were 27.7, 28.4 and 34.8 s,
  against builds of 27.7, 28.4 and 34.8 s. The wait is one relation `IX`
  wait in `sys.lock_waits`.
- The writer's p100 went from 138 ms with no build to 34.8 s, while
  p50 moved from 0.96 to 1.05 ms.
- **A point `SELECT` by tuple key was not blocked**: 14-16k/s during the
  build, against 16.5k/s without one. §0's "every positioned reader
  waits" is true of a walk that takes the borrow, which this run did not
  test.
- The index came out at height 3 with about 32.9k pages for 10M entries.
- Over budget: the run took about 50 minutes against 20, because two
  attempts overlapped other sessions' compiles and were taken again
  behind a quiet gate. The row-count sweep was cut.
- `CREATED INDEX ... entries=0` disagrees with `SHOW INDEXES`'s 10M, in
  all three builds. The reply prints a field the build does not fill.
  This is not CI's to fix, and it is recorded here.

**CI-S1 - the flag and the read side, 2026-10-23**, built on
`worktree-online-index-build` from `dd437a76`:
- `kIndexFlagBuilding` (0x2). `SysIndexRow::Decode` refuses a bit nothing
  defines, and a row that is both unique and building. `CheckIndexDef`
  refuses the same pair, so CI-Q6 (a) holds at the catalog.
- `IndexRef::building`, and the skips in `IndexProbeOf`,
  `CorrelatedIndexProbeOf`, `TableAccess::IndexOn` and `FindIndexOnColumn`.
  Since no unique index can be building, the join cost model's and the ON
  CONFLICT arbiter's checks would be dead code, so none was added there.
- `TableAccess::index_mask` is deleted.
- Superblock 34.
- `BorrowChain`'s first intention is now the first *covering* hold
  (`LockHoldings::HoldsCovering`, §3.5 A3).
- Tests: a building index is chosen by no compile, through the literal,
  filter-scan and correlated arms. It is maintained by `INSERT` and by an
  `UPDATE` that moves its key. The codec and `CheckIndexDef` refusals. A
  v33 volume is refused. The covering test.

**The review** (`critics-developer`) found S1 correct.
- **Applied:**
  - The correlated-arm test reached the literal arm instead; its join now
    uses a range.
  - Three comments no longer described the code.
  - The maintenance test was missing, and is added.
  - The two unique-and-building checks could never trigger. They were
    deleted, and `Decode` refuses the pair instead (removal first).
  - Six copies of the version-refusal test are now one helper.
  - The redundant `0x4` lines are removed.
  - The `BorrowChain` comment is marked as CI-S2's.
- **Deferred to S2:** the dispatcher-level A3 test. It needs a word move
  between a statement's resolution and its `IX`, which only an online
  build makes; CI-S2's drain test (a) covers it.
- **Deferred to S2:** renaming `IndexBuild::building_`.

**Suite**: 4326/4326 at the S1 commit's build inputs (`ctest -j8 -LE
bs-nightly`). Overhead not measured; measured at the milestone's close.

**CI-S2 - the online build, 2026-10-23**, built on `worktree-online-index-build`
on top of `c31c00dd`.
- **Which builds go online.** A plain `CREATE INDEX` in autocommit, on a
  dispatcher that may park and has a lock table and a clock. It borrows
  the relation `IS`, calls `Revalidate`, refuses a relation dropped and
  created again under its name (A10), and runs its scope at READ
  COMMITTED.
- **H2 and H3.** `IndexBuild::Begin` logs the root as a full page image
  (H2). `Announce` writes the slot and the row flagged building (H3) in
  the head, with the row and the owed tree noted there, once.
- **D, the drain.** `IndexBuildRun::Drain` runs before the first slice. It
  reads the relation's `IX` holders after the word move through
  `LockTable::IntentionWriters` and waits on a wake. The net restarts each
  time the set shrinks. It draws a wait-for edge to the front holder, and
  a cancel wakes it.
- **B, the backfill.**
  - It takes the high-water key at its first slice (A2) and checks each
    leaf's owner.
  - It reads the root from the slot per append (CI-R6).
  - It appends through `AppendIndexEntry` with a sink into `LogIndexWrite`.
  - A head writer that was in flight at the copy is never trusted
    resolved later. On a failed walk with that writer decided, the row is
    read again by tuple key, bounded (CI-R7).
- **R and the writers.** `Finish` clears the flag through
  `Catalog::ClearIndexBuilding` (`OverwriteLogged`).
  `AppendIndexEntry` retries a refused append from a moved slot, for
  writers too (CI-Q11 (b)).
- **The rolled-back tree.** Nothing frees an online tree. The reclaimer
  takes a rolled-back create's gate again, one step after its epochs
  pass (A5), and does so for every rolled-back create.
- **What the change made obsolete.** The fault-net cell of the
  `X`-holding build now builds `UNIQUE`, which keeps the `X`. The slice
  rig's writer cell now asserts that the writer runs inside the build.

**The review** (`critics-developer`) found three bugs, and its edits fixed
them:
- The re-gate read the log's end before the epochs' acquire. It now takes
  the gate from the next step's read.
- The drain had no net without a clock. An online build now needs a clock.
- The drain's wake leaked when the frame was destroyed. It is now a member,
  dropped by `~IndexBuildRun`.

**Applied:**
- The re-gate is unconditional for every rolled-back create, which removed
  the `online` flag from `OwedIndex` and `NoteOwedIndex`.
- `AppendVersion` makes one `AppendIndexEntry` call.
- `HighWaterKey` takes the catalog, store and oid.
- `Announce` returns `Status`.
- The drain arms its wake on its first read.
- A cancel wakes the drain.
- The comments that still assumed the `X` were corrected.
- The root-page leak on a refusal between H2 and H3 is stated.

**Tests**, each shown to fail without its code by a mutant (killed 1 in 1):
- the drain waits for a writer that began before the build;
- a writer that never decides refuses the build at the net, and no index
  is left;
- writers during the backfill lose no row under an old view or a new one,
  including old versions held in undo alone;
- a writer asking the relation `X` during the drain is the deadlock victim;
- the CI-Q11 retry from a moved slot;
- the re-gate holds a rolled-back create's tree for a second checkpoint;
- an online build crashed between slices mounts without it and builds
  again;
- a frame destroyed in the drain leaves no wake.

**Not covered by a failing test:**
- CI-R7: it needs a seam between phase 1 and phase 2, and a writer below
  the floor.
- CI-R6's per-append slot read: its mutant survives because CI-Q11's retry
  carries correctness, so the read saves a wasted descent per append
  after a growth.
- The A3 gap at the dispatcher, which needs a statement spanning H3
  between its resolution and its `IX`. S1's covering test stands for it.
- A2's bound, which changes liveness only.

**Suite**: 4333/4333 at the S2 commit's build inputs. Overhead not
measured; measured at the milestone's close.

**CI-S4 - the close, 2026-10-23.**
- **Measured** at `v3.4.3-13-g614864c4` against the opening
  `v3.4.3-10-gdd437a76`, interleaved A/B in `build-release` on a quiet
  host (`bench/v3.4.3/results-ci-close-v3.4.3-13-g614864c4.md`, about
  30 minutes, nothing cut).
- **Arm B, the premise, 10M rows.**
  - The writer's longest stall during a `CREATE INDEX` fell from 27.2,
    28.1 and 26.8 s - each the whole build - to 0.74, 1.29 and 0.70 s.
  - No relation `IX` wait is recorded.
  - The writer runs at 155-193 statements/s during the build (about 0
    before), with p50 3.62 ms against 0.99 ms idle and p99 17.7 ms.
  - The build takes 33.3, 44.1 and 32.9 s against 27.2, 28.1 and 26.8 s,
    and writes 5.04x the log: 1,370 MB against 272 MB, 137 B per row.
    That is §3.5 A12's estimate.
- **Arm A, CI-R5, no build running.** On an indexed relation, INSERT,
  UPDATE of an unindexed column and UPDATE of the indexed column at 200,
  10K and 200K rows show deltas of -7.1 % to +1.2 % against a run-to-run
  spread of 2-22 %, with the log per statement identical. No cost is
  outside the spread.
- **Stated, not a bar missed: the build pays for the writers' freedom.**
  It runs about 20-60 % longer and logs about 5x. Writers run at a quarter
  of their idle rate during it, where before they did not run at all.
- **Not explained:**
  - A recurring band of 0.5-0.7 s writer statements during the build. It
    lines up in count with the build's 21 segment rolls, and a roll syncs
    its segment under the stream latch (`wal.md` §6, BQ). That
    attribution is not measured.
  - One build of 44.1 s.
- **Texts:** `index.md` (status, §9, §10, §10.1, the new §10.3 IX15, IX12,
  §12's list), `ddl-transactional.md` §5e, `sched.md` §3, `leaf_cursor.hpp`'s
  header, `manual/sql/sql.md`, `manual/server/server.md` and
  `backup-restore.md` (superblock 34), `known-gaps.md` (the online index
  build, CI), and CLAUDE.md's secondary-index, transactional-DDL,
  heap-relation and core-placement rows.

**Every stage landed** on `worktree-online-index-build`: S0 `d40e92f9`, S1
`c31c00dd`, S2 `614864c4`, and S4 the close. CI-S3 is not built (CI-Q6
(a)). The close is not merged to `main`, not pushed and not tagged, which
is the operator's word.

**The S4 review** (`critics-developer`).
- **Applied, 13 text errors:**
  - three misstatements in the conditions for going online and in the
    deadlock victim;
  - IX10a's "before the row exists", which is now the `X` build's alone;
  - the remaining texts that said every `CREATE INDEX` takes `X` (`foreign-keys.md`,
    `catalog.md`, `heap-and-tuple.md`, `crosscore.md` CC8, `protocol.md`
    §10, `known-gaps.md`, the manual);
  - the results file's run counts and deltas.
- **Applied, one source defect:** a `PURGE` under the build's `IS` can
  release a delete-marked row's spilled values between the copy and the
  decode, which refused the build. Such a row is now skipped as one no
  reader reaches, or read again if its deleter was in flight at the copy
  (`IndexStagedRow`). It is untested, like CI-R7 (`known-gaps.md`). It
  changes only an error path, so the close measurement still stands for
  it, and it was not measured again.
- **Applied:** the release label is dropped from the manual,
  `leaf_cursor.hpp`'s header is cut to a pointer at `index.md` §10.3, and
  CLAUDE.md's row names every condition that keeps the `X` build.
- **Rejected:** shrinking `ddl-transactional.md` §5e to a pointer. §5e owns
  what each DDL guarantees, and the online build's atomicity and isolation
  belong there.

**Suite**: 4334/4334 on the merged tree (`origin/main` at `02077d7e`,
merged at `9e7b4ce0`) with the review's fixes.
