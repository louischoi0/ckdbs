# Work order DA - access statistics kept, their storage moved to memory: `sys.access_stats` as a memory-resident view

Status: **DA-S3 built 2026-11-20 on worktree `di-shape-identity`, in DI's format event (superblock 38), not landed** (the last section). **CLOSED 2026-11-19, DA-S3 owed, and landed on `main` on the operator's word "push no verify"** (`raft-marks-2026-11-19.md` §17, the hook skipped; suite 0 failed of 4865 at `89c26252`) (DA-S0 `75bbf9f2`, DA-S1 `89c26252`, DA-S2 the documents at `ffe7c501` and the close `bench/v3.4.14/results-da-close-v3.4.14-32-g89c26252.md`: no cell slower, +13.5 % throughput at `cores = 8`; no tag). **OPENED 2026-11-19** on worktree `da-access-stats-memory-resident`
from `origin/main` at `673f025d` (`v3.4.14-30-g673f025d`), on the
operator's standing go-ahead of 2026-11-19 (go-ahead-achieving-milestone,
`raft-marks-2026-11-19.md` §15): every unmarked item adopted as CLA
proposed, DA-Q1 and DA-Q6 as revised against the tree, and DA-Q9..Q12
added and adopted (§4). None is the operator's own mark but DA-Q8 (a).
The stage log is §7.

**Written 2026-11-18.** Written on worktree
`cz-access-stats-memory-resident` from `3b3f4851` (`v3.4.12-25-g3b3f4851`)
on the operator's word of 2026-11-18 (`raft-marks-2026-11-18.md` §18).

**The letter is DA.** The order was written as CZ in one session while
another wrote `workorder-cz-checkpoint-roll-wait.md`, which reached `main`
first under that letter. On the operator's word (*"and separate CZ"*,
`raft-marks-2026-11-18.md` §18) this order took the next letter and every
`CZ-` token in it became `DA-`; nothing else of its text changed but the
two notes below. It was brought onto `main` from `68d109d0`, the tip of
`worktree-cz-access-stats-memory-resident`, which was committed and never
pushed.

**Two facts moved under it since it was written**, and its text below was
read at `3b3f4851`, before either:

- **The superblock is 36** since CY-S1a (`59dcf2af`), which landed without
  DA-S3's removal. So "the stage that next moves the superblock, CY-S1 at
  36" is past: DA-S3 waits for the bump after 36, and DA-Q0 and DA-Q1 are
  to be read with that.
- **CY-S1a changed the code this order deletes**: `Catalog::DiscardAccessStats`
  is now a call of `Catalog::DiscardUnloggedChain`, which `sys.statistics`
  shares, and `catalog::IsUnloggedCatalogRelation` names both relations for
  the delete-mark sweep. DA removes the `sys.access_stats` half of each and
  leaves the helper; CR6 keeps `sys.statistics` as its one relation, so
  `rules.md` §5's unlogged exception is narrowed, not deleted.

**DA-Q8 added and marked (a) on 2026-11-18** (`raft-marks-2026-11-18.md`
§28): a boot key in bytes sizes each core's part, which is a flat table of
fixed-size slots. It is the one marked item (§4).

No stage is started. DA waits for the operator's word and for the marks on
§4's other items. **Only one part of it needs a superblock bump** - removing the
relation's bootstrap rows and root (DA-S3) - and that part waits for the
stage that next moves the superblock, CY-S1 at 36 as the orders stand; the
move itself changes no format (§1.4, DA-Q0, DA-Q1). Every `[source-read]`
claim below was read at `3b3f4851`, and the order was reviewed by
`critics-developer` before it was committed (§7).

**The motive is a smaller engine, not a faster one.** Two measurements in
the tree price the write DA removes at under one percent of a point
statement (§1.1), and the same optimization was proposed and discarded
twice on that number. What DA deletes is a rule, a relation and a mount
step; a gain, if the close shows one, is reported and not required.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured.

Related: `docs/spec/heap-and-tuple.md` §7 (access statistics);
`docs/spec/crosscore.md` CC12, CC13; `docs/rules/rules.md` §5 (CR6, the
unlogged exception); `docs/spec/ddl-transactional.md` §7;
`docs/spec/sys-views.md` §6 (the memory-resident views, SV3, SV5);
`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` (BO-Q5 (b),
BO-Q7, BO-Q8: the shape DA copies);
`instructions/v3.0.0/workorder-bn-opt-read-update-path.md` (BN-R6, the
per-core fold, discarded);
`instructions/v3.2.0/workorder-ca-background-advisory-optimizer.md` (CA-Q6,
the fold moved off the statement, not built);
`instructions/v3.2.0/workorder-cy-managed-statistics.md` (CY-S1, the host);
`instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md` (CD-OI-13,
the retire a reused column slot owes).

## 0. What DA is

`[operator]` Keep the feature. Move only its storage to a memory-resident
statistics view. What a client sees stays the same - the relation is
emptied at every mount today as well. The unlogged exception rule, the
mount's discard and the exclusive latch every statement takes go. The one
cost is a superblock bump, so the change is bound to the next milestone
that raises the format.

`[design]` `sys.access_stats` stays a view with the columns, the rows and
the refusals it has; `SHOW ACCESS` and `CREATE CABIN`'s no-filter warning
read what they read today. Behind the view, the catalog relation at page
11 (oid 130) is replaced by one in-memory part per core, shaped as
`sys.statement_stats`' parts are
(`include/kds/stats/statement_stats.hpp:26-37`) and owned by the instance
(DA-R6). A count is in its core's part before the act that made it
returns (DA-R2): nothing is applied later.

**What W14 took from CLA, corrected.** CLA told the operator that a
superblock bump was the one cost of the change, and W14 binds the change to
the next bump on that ground. Read against the tree, the bump is the cost
of one part: deleting the relation's bootstrap rows and its root. The move
of the storage changes no format, and the tree has withdrawn a bootstrap
relation with no bump before (§1.4). DA-Q0 and DA-Q1 put that to the
operator; W14 as given is option (b) of DA-Q0.

## 1. The tree as read

### 1.1 What is measured `[measured]`

| measurement | reading |
|---|---|
| BN-S1 census, `v2.7.0-769-gb70f79d8`, `cores = 1`, `relaxed` (`bench/v3.0.0/results-bn-s1-census-v2.7.0-769-gb70f79d8.md:114`) | `Catalog::RecordAccess` is **0.7 %** of a point `SELECT`'s server CPU; BN-R6 (the per-core fold) "not material - discarded" |
| CA-S0, `v3.2.0-152-ga9f0f5cd`, `cores = 8` for scenarios 1-3 and `cores = 1` for scenario 4, `group` (`bench/v3.2.0/results-ca-s0-premise-v3.2.0-152-ga9f0f5cd.md`) | Waystone, access-statistics and optimizer-signal code together are 0.2-0.6 % of sampled server CPU in each scenario, a lower bound (leaf functions only); with `waystone_recording`, `waystone_replay` and `access_statistics` all off, no headline moved outside noise but scenario 1's walked shapes, which ran slower |
| BO's close, `v3.3.0-23-g86d1fe1a`, `cores = 1`, `relaxed` (`bench/v3.3.0/results-bo-close-v3.3.0-23-g86d1fe1a.md`, cited by `sys-views.md` §6) | the statement-path recording of the two BO stores a statement writes (`StatementStats::Record`, `RelationStats::Record`) costs 0.35-0.52 us of server CPU a point statement (1.2-2.2 %), consistent in sign (29-37 of 42 paired runs) but inside the largest same-binary gap |

| CA's own reading before CA-S0, `v2.7.0-787-g8679367e` (`workorder-ca-background-advisory-optimizer.md:166`) | Waystone and access statistics together take 1.6-1.7 us of 25.7-26.9 us of a point `SELECT`'s server CPU (6-7 %); BN-S1 puts 7-9 % of it on Waystone's recording and replay attempt, which CF has since made opt-in |

**What the numbers do not promise.** A memory store is not free: BO's two
statement-path records cost about what `RecordAccess` does at
`cores = 1`. DA may therefore show no gain there, and its bar is "no cell
slower" (DA-Q7), not a speed-up.

**Not measured:** the exclusive hold of page 11 alone, above one core,
under a load of short statements that saturates every core.
`RecordAccess`'s own comment says so ("What it costs, unmeasured",
`src/catalog/catalog.cpp:3478`). CA-S0's planned `select-pk` cell at
`cores = 4` with `access_statistics` on against off does not appear in its
results file. `crosscore.md` CC13 states "+1-2 % on a point lookup" and
cites `heap-and-tuple.md` §7, which no longer carries a number.

### 1.2 Why the relation is on pages `[source-read]`

History, each step at the commit that holds it:

- `292df9b4` (2026-08-03) added it as a catalog relation, logged like
  every catalog write, as the input of the relayout planner (superblock
  6 -> 7, `include/kds/server/superblock.hpp:70`).
- CR6 (2026-08-31, `git show 1769487:instructions/v2.7.0/cb-catalog-placement-buildout.md`)
  took its WAL records away and named it the sole unlogged catalog
  relation; CB8 chose discard at mount over repair.
- AT-S7 retired the peer's batch (CR7), so every core writes page 11
  itself under an exclusive hold (`include/kds/stats/access_stats.hpp:55-57`).
- BZ deleted the relayout planner and its mover
  (`access_stats.hpp:11-14`). `RecordAccess`'s comment still gives "the
  one way an advisory statistic can mislead the mover" as the hold's
  reason (`catalog.cpp:3469-3476`).
- BO built the memory-resident views and wrapped this relation as a view
  over itself, adding no persisted relation.

Nothing a page gives is still used: no reader needs the rows across a
mount, since every mount empties them.

### 1.3 What the tree does `[source-read]` at `3b3f4851`

**Writers, three.**

- `stats::RecordChainAccess` makes one `Catalog::RecordAccess` per step of
  a successful `SELECT`, its failure dropped
  (`src/stats/access_stats.cpp:17-47`; called at
  `src/server/command_dispatcher.cpp:9163` through `RecordAccessShapes`,
  `:9213-9223`).
- `CommandDispatcher::RecordFkAccess` records a foreign key's forward and
  reverse checks through the same call (`command_dispatcher.cpp:4313-4321`;
  callers `:4731`, `:4903`), so a write on a relation with a foreign key
  takes the page too.
- `Catalog::ForgetAccessStats(rel, col_pos)` retires the rows whose mask
  names a column position an `ADD COLUMN` reuses (`catalog.cpp:3428`;
  called at `command_dispatcher.cpp:3730`, CD-OI-13 (a)).

**One write, as built.** `RecordAccess` holds page 11 exclusive across a
walk to the shape's row - the whole chain when the shape is new, its live
rows bounded by `kMaxAccessShapes`, 4,096
(`include/kds/catalog/rows.hpp:781`) - then an in-place overwrite or an
insert that may grow the chain (`catalog.cpp:3453-3548`). Both writes pass
no WAL (`:3519`, `:3547`). A new shape past the cap is refused
`ResourceExhausted` (`:3532`), which every caller drops and nothing counts
(`access_stats.hpp:35-42`).

**Readers, two**, both through `Catalog::ListAccessStats`
(`catalog.cpp:3931`): the view builder (`src/server/sys_views.cpp:616-644`,
columns `rel_oid, namespace, rel_name, kind, columns, column_mask, uses,
last_seen`, rows in page order) and `CREATE CABIN`'s warning
(`src/exec/cabin_ddl.cpp:66-79`). No step, plan, trail, Cabin controller or
planner statistic reads it (`heap-and-tuple.md` §7;
`workorder-cc-plan-statistics.md:126`).

**The mount.** `server::DiscardAdvisoryState` runs at every mount, after
recovery and before the listener binds (`src/server/expeditor.cpp:1043`;
the simulator's at `sim/instance.cpp:115`, only when it runs recovery;
`src/server/restore.cpp:311-321`), and its second half is
`Catalog::DiscardAccessStats` (`catalog.cpp:3863`): every page of the chain
re-formatted, the chain cut at a page not its own.

**The format.** oid 130 (`include/kds/catalog/well_known.hpp:183`), fixed
root page 11 (`:344`), an entry of `kCatalogRoots` (`:410`), a bootstrap
entry (`catalog.cpp:708`), the 33-byte `SysAccessStatRow`
(`rows.hpp:717-768`), growth pages from the general supply stamped with
oid 130 (`crosscore.md` CC12), and an entry of `kAllWellKnownOids`
(`well_known.hpp:292`). Bootstrap also writes it a `sys.objects` row as a
table of `sys` (`catalog.cpp:744`), so `sys.tables` and `SHOW TABLES` list
it. The superblock is 35 (`superblock.hpp:229`).

**A fourth holder of the page.** `Catalog::RetireDeleteMarks` walks every
`kCatalogRoots` chain with `kWrite` on core 0's tick, this one included
(`catalog.cpp:1108-1111`). Three test files read `kCatalogRoots`
(`tests/catalog_page_ceiling_test.cpp:112`,
`tests/insert_log_crash_rig_test.cpp:851`,
`tests/catalog_chain_pages.hpp:47`).

**The switch.** `access_statistics`, a boot key, default on
(`include/kds/server/expeditor.hpp:305`, `src/server/expeditor.cpp:416`),
handed to every core's dispatcher (`command_dispatcher.hpp:460`,
`core_runtime.hpp:159`). The simulator draws it (`sim/loop.cpp:1334`).

**The stored kind.** `exec::StoredAccessKind` and `AccessKindOfStored`
(`src/exec/plan_printer.cpp:40`, `:57`; declared at
`include/kds/exec/step_chain.hpp:1074-1084`, with `catalog::kAccessKindUnset`
at `rows.hpp:772`) exist because the kind is persisted in this relation
(`plan_printer.cpp:48`); their callers are this relation's two writers
(`src/stats/access_stats.cpp:37`, `command_dispatcher.cpp:4321`), its view
(`src/server/sys_views.cpp:631`) and one test cell
(`tests/access_stats_test.cpp:258`).

**The shape to copy.** One part per core under `LatchKind::kStatsRing`,
the core a row describes its part's only writer, a reader taking each
part's latch in turn and merging by key; 4,096 shapes a core, a shape past
it dropped and counted in `sys.meta`
(`statement_stats.hpp:26-37`, `:60`; `command_dispatcher.cpp:2326`).
**Those stores are the process's, not the instance's**: a function-local
static array of parts (`src/stats/statement_stats.cpp:53-54`) that no
mount empties; six test files empty them through `ResetForTest`. The
Cabin store and the plan statistics are the instance's and handed to each
dispatcher (`core_runtime.hpp:503`, `command_dispatcher.hpp:1860`).

**Known gaps DA would close** (`docs/inflight/known-gaps.md`, as read): the
cycle among growth pages walked for the whole budget (`:1707`); the sim's
`skip_recovery` restore keeping `sys.access_stats` rows (`:1717`, its
access-statistics half); the access-statistics half of "the advisory work
still runs on the statement" (`:1543`), which becomes a memory add.

**Tests.** `tests/access_stats_test.cpp` (19 cells; five of them,
`AccessStatsDurabilityTest`, pin the chain discard),
`tests/access_stats_across_cores_test.cpp` (1), and 54 other files under
`tests/` that name the relation, the view or the key.

### 1.4 What needs a format event, and what does not `[source-read]`

- **The move needs none.** If bootstrap goes on creating page 11 and its
  rows and no statement writes them, both directions mount: this build on
  a version-35 volume finds a page it never reads, and a version-35 binary
  on a volume this build ran finds page 11 as bootstrap or its own last
  discard left it.
- **Removing the bootstrap needs one, in one direction.** A version-35
  binary on a volume created without the relation fails its mount -
  `DiscardAccessStats` returns `Corruption` when the head page is not its
  own (`catalog.cpp:3916`) - an opaque failure, not a refusal at the door.
  This build on an older volume would only carry a dead page and dead
  rows. The standing order of 2026-10-07 is to refuse with no migration.
- **The tree has done both.** `sys.pattern_defs` was withdrawn on
  2026-08-31 with no bump, its row bytes staying
  (`superblock.hpp:68-69`), its root page 10 "reserved and unused" and its
  oids never reissued (`well_known.hpp:328-330`). Page 15 went the other
  way: `sys.ranges`' root, retired, then given to `sys.cabin_exprs` under
  superblock 26 (`well_known.hpp:361-365`).

## 2. The design `[design]`

- **`stats::AccessStats`**, a store the instance owns and hands to every
  core's dispatcher, as the Cabin store and the plan statistics are
  handed: one part per core (`stats::kStatsParts`), each a flat table of
  fixed-size slots keyed `(exec::AccessKind, rel_id, column_mask)` holding
  `uses` and `last_seen`, under a `kStatsRing` latch, allocated whole,
  once, and never grown. **Its bytes are a boot key's** (DA-Q8 (a)): a
  part's table allocates no more than the key says. When it is allocated -
  at its core's start or at its first row - is DA-S1's (§4).
- **A record** is the executing core's add to its own part: find or admit
  the key, saturating add, `last_seen` set. It returns nothing.
- **A read** copies each part under that part's latch and merges the
  copies with no latch held: `uses` summed and saturating, `last_seen` the
  latest of the parts' readings.
- **A forget** (`ADD COLUMN` over a reused slot) takes each part's latch
  in turn and removes the relation's shapes whose mask names the position.
- **The callers** reach the store directly. `Catalog::RecordAccess`,
  `ListAccessStats` and `ForgetAccessStats` are deleted, and with them
  the stored-kind mapping and `kAccessKindUnset`: in memory the enum is
  the key.
- **Page 11 after the move** is written by no statement. The mount's
  discard can go with the move only if nothing walks the chain any more;
  the one walker found is `RetireDeleteMarks`, so page 11 leaves the set
  that walk visits, or the discard stays until DA-S3. DA-S0 confirms which.

## 3. Rules

Binding once DA opens.

- **DA-R1 - the view keeps its columns, types, key and refusals.** One row
  per `(kind, rel_id, column_mask)` for the instance, and `SHOW ACCESS`
  stays `SELECT * FROM sys.access_stats` (SV1-SV5). **What a client can
  see change, all of it**: the order of rows (DA-Q2); more than 4,096 rows
  in the view (DA-Q3, DA-Q8); a new `sys.meta` field, `access_stats_dropped`; and,
  from DA-S3 only, `sys.tables`, `sys.objects` and `SHOW TABLES` losing
  the `access_stats` row.
- **DA-R2 - a count is in its core's part before the act that made it
  returns**: a `SELECT` at its successful end, a foreign key's check when
  it runs, as today. No batch, tick or drain.
- **DA-R3 - a record never fails or reorders what it describes, and waits
  for at most one copy of its own part** by a reader. A shape past the
  bound is dropped and counted. The part's table is its one allocation
  since DA-Q8 (a): if DA-S1 makes it on a statement's path, a part whose
  table cannot be allocated drops and counts every record.
- **DA-R4 - a record writes its own core's part and nothing shared.**
- **DA-R5 - a forget reaches every part before the `ADD COLUMN` that asked
  for it answers.** It gives what page 11's hold gives today: the writes
  are ordered, the statements are not, so a `SELECT` that records after
  the forget can re-admit a shape. If DA-S0 finds every record inside its
  statement's relation borrow, the `ADD COLUMN`'s `X` closes that and the
  rule says so.
- **DA-R6 - the store is the instance's.** A restart, a restore and a
  second instance in one process each start empty, with no reset call. The
  four BO stores are not built this way and DA does not change them.
- **DA-R7 - no reader is added.** Nothing but the view and
  `CREATE CABIN`'s warning reads the rows.
- **DA-R8 - nothing is left behind that DA-Q1 does not name**: no row
  struct, discard, rule text or mapping whose only user DA removed.

## 4. Items for the operator (DA-Q), CLA's proposals

**DA-Q8 is marked (a), the operator's mark of 2026-11-18**
(`raft-marks-2026-11-18.md` §28). **Every other item is adopted (a) on the
operator's standing go-ahead of 2026-11-19 (go-ahead-achieving-milestone,
`raft-marks-2026-11-19.md` §15)**, none the operator's own mark; DA-Q1 and
DA-Q6 as revised below the table, and DA-Q9..Q12 added at DA-S0 and
DA-S1. (a) is CLA's proposal in each.

| | item | (a) | other |
|---|---|---|---|
| DA-Q0 | How much of DA waits for a superblock bump. | only DA-S3. S0-S2 run when DA opens and land with no format change: every statement-path write, the unlogged writer CR6 names and the per-statement hold go then | (b) all of it, as W14 reads: nothing lands before the host's bump |
| DA-Q1 | The dead bootstrap - page 11, oid 130, the row struct, the `sys.objects` row. | removed by one commit in the first bump any order makes - CY-S1 at 36 as the orders stand - merged into that order's branch so it shares the number | (b) left reserved and unused with no bump, as `sys.pattern_defs` is, DA-R8 naming it; (c) a bump of DA's own |
| DA-Q2 | The order of the view's rows. Today it is page order, the order shapes were admitted in. | key order: `rel_oid`, then the kind's name, then `column_mask` | (b) admission order kept, by a sequence stamped at admission - a shared word on a record's path, against DA-R4 |
| DA-Q3 | The bound. Today 4,096 shapes for the instance, a refusal past it counted nowhere. **Narrowed by DA-Q8 (a)** to the default of the key that sizes a core's part; for (b) see below the table. | 4,096 a core (BO-Q7's number), counted as `access_stats_dropped` | (b) 4,096 for the instance, by a shared count |
| DA-Q4 | The `access_statistics` key. | kept, its meaning unchanged | (b) deleted and refused at boot by name, always on as the BO views are (BO-Q8) - which also changes the simulator's draw, its repro strings and its pinned arms (`sim/loop.cpp:1334`, `sim/minimize.cpp:117`, `:305`, `sim/sim_main.cpp:117-125`) |
| DA-Q5 | Under DA-Q1 (a) or (c): page 11 and oid 130 afterwards. | retired, as page 10 and its oids are | (b) page 11 offered to the host's new relation, as page 15 was reused |
| DA-Q6 | `rules.md` §5's exception, an operator-ratified rule (CR6). | deleted: every catalog write is logged, with no exception, and `ddl-transactional.md` §7 loses its paragraph | (b) kept with no member |
| DA-Q7 | The measurement. | DA-S0 prices today's write - a point read at `cores` 1 and 8, `access_statistics` on against off, interleaved - and the close repeats those cells before against after, with an `INSERT` into a relation with a foreign key. The bar: no cell slower outside its own spread. The standing budget | (b) the close alone |
| DA-Q8 | **Marked (a).** Whether the configuration sets the store's memory. As written the bound is a compile-time count (DA-Q3) and no key sizes it. Read at `a5581343`: none of BO's four stores has a key (`statement_stats.hpp:60`, `relation_stats.hpp:61`, `lock_wait_log.hpp:64`, `checkpoint_log.hpp:45`), and the one per-core memory key in the tree is `ash_ring_bytes_per_core` (`expeditor.hpp:287`, `expeditor.cpp:387`). | **a boot key in bytes for each core's part, and the part a flat table of fixed-size slots**, so a part's table allocates at most the bytes written; a node map's allocation is not bounded by a value written in bytes | (b) a boot key in shapes a core, exact over any structure; (c) no key, the constant kept as BO's stores keep theirs |
| DA-Q9 | *Added at DA-S0.* The mount's discard of page 11's chain, and what keeps naming page 11 until DA-S3. | **deleted in DA-S1**: S0 found no walker but `RetireDeleteMarks`, which already passes page 11 by (`IsUnloggedCatalogRelation`, `catalog.cpp:1119`), so with no statement writing the page nothing reads its chain; page 11 stays in `IsUnloggedCatalogRelation` and in DB's unlogged registration (`RegisterUnloggedClasses`) until DA-S3, because a volume a pre-DA build wrote may hold a damaged chain the sweep must still pass by, and bootstrap still makes the page | (b) the discard kept until DA-S3 |
| DA-Q10 | *Added at DA-S1* (§4's "left to DA-S1"). When a part's table is allocated. | **at the part's first record**, a failure dropping and counting every record and never retried: a core that records nothing, the `access_statistics = off` instance and a dispatcher's own fallback store (replaced by the instance's) allocate nothing | (b) at the core's start, a failure refusing the boot |
| DA-Q11 | *Added at DA-S1.* The table's probing and a forget's deletion. | linear probing from a 64-bit mix of the key, modulo the slot count, **at most half the slots live** (a part admits `slots / 2` shapes), and a forget deletes by **backward shift**, so no tombstone counts against the bound and a dropped shape's walk stays short | (b) a probe cap with tombstones |
| DA-Q12 | *Added at DA-S1.* The key's name, range and report. | `access_stats_bytes_per_core`, in `[80, 256 MiB]` (80 bytes admits one shape; `0` refused - `access_statistics` stays the switch), default 327,680 (4,096 shapes a core at 40-byte slots); a value not a whole number of slots admits the slots that fit; `sys.meta` reports `access_stats_dropped` and `access_stats_shapes_per_core` | (b) the range unbounded above |

**Adopted as revised against the tree** (the skill's rule: a proposal's
premise is re-read before it is adopted).

- **DA-Q1 (a)**: "the first bump any order makes - CY-S1 at 36" is past
  twice: CY-S1a moved the superblock to 36 and CX-S1a to 37 (`superblock.hpp:247`),
  neither carrying DA-S3. Adopted as **the first bump after 37 any order
  makes**, DA-S3's one commit merged into that order's branch.
- **DA-Q6 (a)**: "deleted: every catalog write is logged" no longer holds -
  CY-S1a made `sys.statistics` a second unlogged catalog relation under
  CR6, which DA does not touch. Adopted as **narrowed**: the exception's
  one member is `sys.statistics`, and `ddl-transactional.md` §7 says so.

**What DA-Q8 (a) settles, and what it leaves**
(`raft-marks-2026-11-18.md` §28, which carries the words and CLA's reasons).

- **Settled by the mark**: the unit is bytes, the scope is one core's part,
  the setting is a boot key, and the part is a flat table of fixed-size
  slots (§2), which allocates at most the bytes written.
- **DA-Q3**, which stays unmarked: the bound is the key's bytes a core, so
  DA-Q3 now asks the key's default - what admits 4,096 shapes a core under
  its (a). Its (b), one count for the instance, would sit on top of the
  key as a shared cap, against DA-R4, and the key's bytes would no longer
  be the bound; it stays open to the operator.
- **Left to DA-S1, CLA's proposals, unmarked**:
  - the name, `access_stats_bytes_per_core`;
  - under DA-Q4 (a) only, the key is separate from `access_statistics`,
    which stays the switch, and `0` is refused rather than read as "off";
    under DA-Q4 (b) what `0` means is open again;
  - a value below what admits one shape is refused, and a value that is
    not a whole number of slots admits the slots that fit;
  - when the table is allocated: at its core's start, a failure refusing
    the boot and nothing allocated on a statement's path, or at the first
    row, a failure dropping and counting every record (DA-R3);
  - the table's probing: with unbounded probing a full table makes every
    dropped shape's record a walk of every slot under the part's latch,
    and a schema past the bound stays in that state, so DA-S1 chooses a
    load bound or a probe cap, and a forget's deletion scheme (a tombstone
    counts against the bound). Under a load bound a part admits fewer
    shapes than it has slots, and the default is sized for the shapes;
  - the admitted capacity reported in `sys.meta` beside
    `access_stats_dropped`, so the bytes written can be read as shapes.
- **Why a key**: to raise the bound for a schema whose shapes pass it, not
  to protect memory - a slot is tens of bytes by CLA's estimate,
  unmeasured.
- **A key for the four BO stores** is outside DA (§6) and not settled.

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DA-S0 - census and premise** | No engine code. §1.3 re-read at the opening commit. **Read**: whether every record runs inside its statement's relation borrow (DA-R5); every walker of page 11's chain besides `RetireDeleteMarks` (§2); whether the cores' `last_seen` readings are one clock's; every test that pins the view's row order or a count of catalog rows. **DA-Q7's cell.** | the list, the number, and any item back to the operator |
| **DA-S1 - the store and the switch** | `stats::AccessStats`; the three writers and the two readers moved to it; `access_stats_dropped`; the boot key that sizes a part and its refusals (DA-Q8 (a)); the catalog's three functions and the stored-kind mapping deleted; the mount's discard deleted if S0 allows. No format change. | review, the full suite; at the push gate under DA-Q0 (a) |
| **DA-S2 - the close** | DA-Q7's A/B over S1; the documents below amended; the milestone row flipped, saying what DA-S3 still owes. | closed, DA-S3 owed |
| **DA-S3 - the removal** | Under DA-Q1 (a) or (c): the bootstrap entry, the root, the `kCatalogRoots` and `kAllWellKnownOids` entries, `SysAccessStatRow`, any discard S1 kept; the superblock's history entry. One commit. | review, the full suite, with the bump it rides |

**Tests DA owes.** The view's rows before against after for one fixed
workload, equal as sets; `show_sugar_contract_test`'s `SHOW ACCESS` cell;
two cores recording one shape answer one row with the sum; a shape past
the bound is dropped, counted, and fails nothing; a part's table is no
larger than the key's bytes, and a key out of range is refused at boot
(DA-Q8 (a)); a restart and a restore
answer no rows, and so does a second instance in one process; a forget on
one core removes another core's shape; an armed two-core rig reading the
view while both cores record, under `tsan`. At DA-S3: `sys.tables` no
longer lists the relation, and the superblock cell refuses the version
before. `AccessStatsDurabilityTest`'s five cells go with the discard.

**Documents DA-S2 owes**, each read at the close and amended where it
states storage: `rules.md` §5 (`:69`, and `:35`); `ddl-transactional.md` §7
(`:572`, and `:268`); `crosscore.md` CC12, CC13 and §2 (`:30`, `:31`,
`:43`), the unsourced "+1-2 %" among them; `catalog.md` (`:242`);
`heap-and-tuple.md` §7 (`:311` on, and `:56`, `:268`); `sys-views.md`
(`:106`, `:127`, and §6 gains the view); `txn.md` (`:189`); `backup.md`
(`:67`); `wal.md` §2 (`:39`); `waystone-concpets.md` (`:7`, `:77`);
`page.md` (`:49`); `drop-table.md` (`:113`, `:115`); `protocol.md` §9
(`:140`: a `readonly` `SELECT` then writes no page for a statistic);
`foreign-keys.md` (`:186`, `:203`); `known-gaps.md` (§1.3's three entries,
and `:69`, `:791`, `:1290`, `:2059` read, and DA-S3 entered as owed);
`manual/sql/sql.md`, `manual/server/server.md`,
`manual/cabin-controller/cabin-controller.md`; `CLAUDE.md`'s rows for
access statistics, backup and observability.

## 6. What DA does not do

- No change to what is counted or by which key: columns, never values;
  one call for every access kind; a failed `SELECT` records nothing, and a
  foreign key's check records when it runs.
- **CA-Q6 (a) is not built.** That item - the fold moved to a background
  drain - was adopted on the standing go-ahead of 2026-10-17
  (`raft-marks-2026-10-17.md` §4), subject to CA's test, and CA closed on
  2026-10-31 with it unbuilt. DA-R2 takes its place for this statistic.
- No persistence: the rows do not survive a restart, as they do not today.
- No retire at `DROP TABLE`: a dropped relation's shapes stay until a
  restart, as they stay until a mount today.
- Nothing to the Cabin, its controller, Waystone, the plan statistics or
  the four BO stores.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-18

Written on worktree `cz-access-stats-memory-resident` from `3b3f4851`
(`v3.4.12-25-g3b3f4851`), documents only. The letter DA is CLA's proposal.
No suite was executed: the commit changes no build input. Nothing was
measured for it; §1.1's numbers are the tree's own, at the versions named.

**Reviewed by `critics-developer` before the commit.** It opened every
cited line and results file; the citations and counts stood but for seven
it corrected in place (§1.1's CA-S0 and BO rows, §1.3's write, mount and
stored-kind paragraphs, §6's first bullet, two documents missing from
§5's list). Applied from its findings: the store is the instance's, since
the BO stores it was to copy are process-static (DA-R6); DA-R1 lists every
visible change; DA-R3 no longer says "never delays"; DA-R5 claims what
today's hold gives and no more; §1.4 states the no-bump precedent; the
proposal in DA-Q0 became "only the removal waits", against W14 as given,
which stays as (b); the rule that restated DA-Q0 was deleted and the
version-number item folded into DA-Q1; the store's stage and the switch's
were merged; DA-Q2's order names the kind's name, the enum having no
stable order; the duplicates of "no batch" and "no new reader" were cut.
**Rejected**: folding DA-Q6 (b) away - CR6 is an operator-ratified rule,
so its deletion stays an item with its alternative shown; and dropping
DA-S0 - the borrow read and DA-Q7's cell are still owed before code.


The session that wrote the order was given a push word (*"push no verify"*,
`raft-marks-2026-11-18.md` §18) and its push did not reach `main`. The
order reached `main` as DA at merge `0a91fd32`, the splitting commit merged
unchanged and pushed with the hook skipped by the session that wrote it;
documents only.

### DA-Q8 added and marked (a), 2026-11-18

On worktree `da-q8-memory-bound-key` from `a5581343`
(`v3.4.12-51-ga5581343`), documents only. DA-Q8 recorded with the
operator's mark (`raft-marks-2026-11-18.md` §28); §2's part, DA-R1, DA-R3,
DA-Q3's row, DA-S1 and the tests owed amended to carry it. DA-Q0..Q7 are
unmarked and DA is not opened. No suite was executed: the commit changes no
build input. Nothing was measured.

**Reviewed by `critics-developer` before the commit.** The citations
stood; the one for BO's stores named one store of four and now names each.
Applied from its findings: "the bytes written are the bytes allocated"
became "at most", the order's own rounding rule contradicting the first;
"settled by the mark" holds option (a)'s four facts only, and the name, the
default, the separate switch and `0` moved to CLA's unmarked proposals, the
last two stated as holding under DA-Q4 (a) alone; DA-Q3's row says it is
narrowed, and its (b) is "against DA-R4", not "does not fit"; "DA alone
gets a key" became "not settled"; DA-R3 fits a table allocated whole; the
probing claim names its assumption and a forget's deletion; three
restatements of the conversation were cut. **Not taken as a fix**: the
review's choice between allocating at core start and at the first row -
left to DA-S1 with both stated, since it changes what a statement's path
may do.

### Opened 2026-11-19

On worktree `da-access-stats-memory-resident` from `origin/main` at
`673f025d` (`v3.4.14-30-g673f025d`), on the operator's standing go-ahead
of 2026-11-19 (go-ahead-achieving-milestone, `raft-marks-2026-11-19.md`
§15). DA-Q0, DA-Q2..Q5 and DA-Q7 adopted (a); DA-Q1 (a) and DA-Q6 (a)
adopted as revised against the tree (§4, below the table); DA-Q9..Q12
added and adopted (a). None is the operator's own mark but DA-Q8 (a).

### DA-S0 - census and premise, 2026-11-19

No engine code. §1.3 re-read at `673f025d`; what moved since `3b3f4851`:

- **The superblock is 37** (`superblock.hpp:247`, CX-S1a), not 35 or 36.
- **DB added a third holder of page 11's identity**: `RegisterUnloggedClasses`
  (`catalog.cpp`) registers oid 130 and page 11 with the store so a write
  fetch logs no full-page image for them. Nothing it does reads the chain.
- **The four reads §5 asks for**:
  1. *Whether every record runs inside its statement's borrow* (DA-R5): a
     `SELECT`'s record runs at its successful end inside `SelectRun`
     (`command_dispatcher.cpp`, `RecordExecution`), under the statement's
     borrow; but a borrow the table refuses leaves the reader reading on
     (`txn.md` §5, "never refused, never waiting"), so an `ADD COLUMN`'s
     `X` does not order every such reader. **DA-R5 stays as written**: the
     forget's writes are ordered, the statements are not.
  2. *Every walker of page 11's chain besides `RetireDeleteMarks`*: none.
     `RetireDeleteMarks` already passes it by (`IsUnloggedCatalogRelation`),
     `ScanAll` in `ListAccessStats` and the mount's `DiscardAccessStats`
     are the only others, and DA-S1 deletes both. So the discard goes in
     DA-S1 (DA-Q9 (a)).
  3. *Whether the cores' `last_seen` readings are one clock's*: yes - every
     core's dispatcher reads `NowNs()` off the clock its runtime is handed,
     the expeditor's one `clock_` (`core_runtime.cpp`); the merge's
     "latest" is therefore well defined.
  4. *Tests pinning the view's row order or a count of catalog rows*: no
     cell reads `sys.access_stats`' row order; the catalog row counts do
     not move, since page 11's bootstrap stays until DA-S3. Settled by
     DA-S1's suite.
- **DA-Q7's cell** (`bench/v3.4.14/results-da-s0-premise-v3.4.14-30-g673f025d.md`,
  `ck-tester`, Release, six lifetimes an arm, interleaved, 9 min of the
  20): `access_statistics` on against off costs **+0.60 us of server CPU a
  point `SELECT` at `cores = 1`** (5 of 6 rounds, inside the 0.75-0.78 us
  same-binary spread, BO's 0.35-0.52 us order), **+8.19 us and -11.6 % of
  throughput at `cores = 8`** (6 of 6, about five times the spread - the
  page-11 exclusive hold §1.1 named as unmeasured), and nothing on an
  `INSERT` into a child relation with a foreign key (+0.08 us, 3 of 6).
  **So §0's "a smaller engine, not a faster one" holds at one core and not
  above it**: the close's bar stays DA-Q7 (a)'s "no cell slower", and a
  gain at `cores = 8` is reported, not required.

No item goes back to the operator: nothing read needs a fact only the
operator holds. Overhead not measured; measured at the milestone's close.

### DA-S1 - the store and the switch, 2026-11-19

On worktree `da-access-stats-memory-resident` over `75bbf9f2`. No format
change: the superblock stays 37 and page 11 is still bootstrapped.

- **`stats::AccessStats`** (`include/kds/stats/access_stats.hpp`): the
  instance's (`Expeditor`, emplaced at the mount beside the plan
  statistics and handed to every core through `OptimizerSurface`), a
  dispatcher handed none keeping its own, as DF-S3's `IndexStats` does.
  64 parts under `kStatsRing` latches, each a flat table of 40-byte slots
  allocated at the part's first record (DA-Q10 (a)), linear probing at a
  half-load bound with backward-shift deletion (DA-Q11 (a)).
- **The writers**: a `SELECT`'s steps at its successful end
  (`RecordChainAccess`), a foreign key's checks (`RecordFkAccess`), and the
  `ADD COLUMN` forget, all on the store. **The readers**: the view, in key
  order (DA-Q2 (a)), and `CREATE CABIN`'s warning, which `exec::CreateCabin`
  is now handed the store for.
- **The key** `access_stats_bytes_per_core` (DA-Q8 (a), DA-Q12 (a)), and
  `sys.meta`'s `access_stats` section: `access_stats_dropped`,
  `access_stats_shapes_per_core`. `access_statistics` kept (DA-Q4 (a)).
- **Deleted** (DA-R8): `Catalog::RecordAccess`, `ListAccessStats`,
  `ForgetAccessStats`, `DiscardAccessStats`; the mount's discard of page 11
  from `DiscardAdvisoryState` (DA-Q9 (a)); `exec::StoredAccessKind`,
  `AccessKindOfStored`, `catalog::kAccessKindUnset`, `kMaxAccessShapes`;
  `AccessStatsDurabilityTest`'s five cells and the stored-kind cell. **Kept
  until DA-S3**, each named by DA-Q1 or DA-Q9: the bootstrap entry,
  `SysAccessStatRow` and its codec, the `kCatalogRoots` and
  `kAllWellKnownOids` entries, and page 11's halves of
  `IsUnloggedCatalogRelation` and `RegisterUnloggedClasses`.
- **Tests DA owes, as built** (`tests/access_stats_test.cpp`,
  `tests/config_file_test.cpp`): two cores' records of one shape answer one
  row with the sum and the latest `last_seen`; rows in key order; a shape
  past the bound dropped, counted and failing nothing, the known shapes
  still counting; a part admits `bytes / 40 / 2` shapes and the default
  4,096; the key's range refused at boot, `0` among it; a forget on one core
  removes another core's shapes; the forget keeps a full table's remaining
  shapes reachable, and an oracle of random records and forgets over tables
  of 2 to 17 slots; a second store starts empty; a handed store is the one
  every record reaches; the switch off records nothing; `sys.meta`'s two
  fields. The two-core rig (`tests/two_core_rig.hpp`) hands both cores one
  store, so `AccessStatsAcrossCores` reads a peer's record through core 0.
  **Not built**: an armed two-core rig under `tsan` reading the view while
  both cores record - the latch protocol is `IndexStats`' and `OidParts`',
  and the `tsan` build is BS's nightly label, outside the push gate.
  The view's rows before against after "equal as sets" is carried by the
  dispatcher cells, which read the same shapes the page held.

**Reviewed by `critics-developer`.** It found one bug and fixed it: a
forget restarted its scan at slot 0 after every deletion, O(matches x
slots) under the part's latch - every record on that core waiting behind
it (DA-R3); now one pass from just past an empty slot, re-checking the
refilled slot. It added the oracle cell and corrected four comments that
still named `RecordAccess` or page 11's discard. **Applied from its
reported findings**: the parts are an inline array, as `OidParts` keeps
them, where a failed allocation of the array lost every record with
`Dropped()` reading 0; the key's boot refusal is tested in `ApplyFile`;
`restore.hpp` no longer lists `sys.access_stats` among what a restore
cannot trust; the one-caller key test became a member; `Snapshot` reserves
before it copies. **Rejected**: rewriting `superblock.hpp`'s history entry
that says "this build's `sys.access_stats` discard" - it records why that
version step was taken, not what the engine does; and DA-S3's list, which
the review asked to confirm, already names `SysAccessStatRow` (§5), its
codec going with it.

Overhead not measured; measured at the milestone's close.

### DA-S2 - the close, 2026-11-19

**DA-Q7 (a)'s A/B** (`bench/v3.4.14/results-da-close-v3.4.14-32-g89c26252.md`,
`ck-tester`, Release, six interleaved lifetimes an arm, nothing cut, 11 min
with the builds): `673f025d` (`v3.4.14-30-g673f025d`, the opening) against
`89c26252` (`v3.4.14-32-g89c26252`, DA-S1), `access_statistics = on`.

| cell | server CPU, after - before | throughput | verdict |
|---|---|---|---|
| point `SELECT`, `cores = 1` | -0.22 us | +0.9 % | inside the spread |
| point `SELECT`, `cores = 8` | -8.32 us (42.71 to 34.39) | **+13.5 %**, 6 of 6 | gain, about six times the spread |
| FK child `INSERT`, `cores = 1` | -0.45 us | +1.6 % | inside the spread |
| control, `cores = 8`, statistics off | -0.06 us | +0.4 % | inside the spread |

**The bar is met: no cell slower.** At `cores = 8` the store lands where
DA-S0's statistics-off engine did (34.39 against 33.98 us, 110.5k against
110.6k qps), so what DA-S0 priced was page 11's exclusive hold, not the
counting. At `cores = 1` the write's cost is below this host's ~1 us floor
before and after: met, not shown free. Limits: 10k rows, `relaxed` only,
the clients sharing the server's 8 vCPUs.

**Documents** amended at `ffe7c501` (§5's list, reviewed by
`critics-developer`, six corrections applied, the cost figures given one
home in `sys-views.md` §6.2d). The milestone row says what DA-S3 owes.

**DA closes with DA-S3 owed**: the bootstrap's removal waits for the first
superblock bump after 37 any order makes (DA-Q1 (a) as revised).

## DA-S3 - the removal, 2026-11-20

On worktree `di-shape-identity` (branch `worktree-di-shape-identity`), as
the one commit DA-Q1 (a) bound to "the first bump after 37 any order
makes": DI's, for its `sys.patterns` row
(`instructions/v3.2.0/workorder-di-shape-identity.md` DI-Q11 (a), adopted
by CLA on the operator's standing go-ahead of 2026-11-20). **Superblock
38.**

**Removed**: bootstrap's `access_stats` entry, so no page 11 and no
`sys.objects` or `sys.tables` row for oid 130; `kSysAccessStatsTable` and
its `kAllWellKnownOids` entry - **130 is retired, never reused**, as 115
and 120 through 124; `kCatalogPageAccessStats` and its `kCatalogRoots`
entry - **page 11 is reserved and unused**, as page 10; `SysAccessStatRow`
and its codec (`ColumnBit` stays, it is the memory store's);
`IsUnloggedCatalogRelation`'s and `RegisterUnloggedClasses`' page-11
halves, which leaves `sys.statistics` the one unlogged catalog chain.

**What a client sees** (DA-R1): `sys.tables`, `sys.objects` and
`SHOW TABLES` no longer list `access_stats`; `sys.access_stats` and
`SHOW ACCESS` answer as before. A version-37 volume does not mount, and
this build's volume does not mount on a version-37 binary: no migration,
the standing order of 2026-10-07.

**Tests**: `SuperBlockTest.AVersion37VolumeDoesNotMount`;
`CommandDispatcherTest.AccessStatsIsAViewAndNoRelationSinceSuperblock38`;
the two cells that fetched page 11 no longer do
(`statistics_persistence_test.cpp`, `core_runtime_test.cpp`); the WAL
golden log is re-pinned, bootstrap's stream being two rows shorter.

**The suite**: 1 failed of 4893 at `-j8` before the re-pin - the golden
log, by the bytes that were meant to move - and that cell passes with the
new pin; the whole suite is run again with the rest of DI-S3 over it.
Overhead not measured; measured at DI's close.

