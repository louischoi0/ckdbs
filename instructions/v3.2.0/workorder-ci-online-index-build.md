# Work order CI — an online index build: `CREATE INDEX` without the relation `X` across its backfill

Written 2026-10-23 on `worktree-online-index-build` from `2f7ce828`
(`v3.4.3-1-g2f7ce828`), on the operator's word, recorded in English
(`raft-marks-2026-10-23.md` §4):

- **W0**: *"index build online, write milestone"*.

**Written 2026-10-23; not opened.** CI-Q0..Q10 each carry CLA's proposal
beside them, and none is marked. CI-S0 is a census and a premise
measurement (§5); nothing is built before it reports.

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

---

## 3. Design `[design]`

**3.1 The building flag.** `kIndexFlagBuilding` (0x2) goes in
`SysIndexRow::flags` (IX12), beside `kIndexFlagUnique`.
`TableAccess::IndexRef` carries it. `MaintainIndexes` ignores it: a
building index is maintained like any other. A unique one's writer check
(`CheckUniqueKeys`, `src/server/command_dispatcher.cpp:10883-10931`) is
CI-Q6's. The step compiler, `index_mask`, the join planner's cost model,
`FindIndexOnColumn` and every probe-choosing path skip an index that
carries it. `step_vm.cpp:1379` re-resolves an index by oid at execution,
so a compiled chain never meets a flag flip mid-statement. `SHOW INDEXES`
prints the flag. A superblock bump is CI-Q5's.

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
for a probe, `index_mask`, `FindIndexOnColumn` and the join planner's model
included).

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

Not opened.
