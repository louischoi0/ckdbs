# Work order DV - pages no root reaches: counted, then reclaimed

Status: **CLOSED NARROW 2026-11-27 on the work branch**, on CLA's reading
of DV-S0 adopted on the go-ahead (DV-Q7 (a)): the census (S0, S1) and the
two source fixes S0 ranked over DV-Q1's bar (S2) are built; **the sweep
(S3) is not**, and the online build's root between H2 and H3 is not closed.
Not on `main`: the landing waits for the operator's word. Opened
2026-11-27 on worktree `dv-unreachable-page-reclaim` (branch
`worktree-dv-unreachable-page-reclaim`) from `630ecc359`
(`v3.4.17-178-g630ecc359`), on the operator's standing go-ahead
(go-ahead-achieving-milestone; `raft-marks-2026-11-27.md` §5), **DV-Q0..Q6
adopted as CLA proposed, (a) in each, by that word and not as per-item
marks**; DV-Q4', DV-Q7..Q11 added by CLA and adopted the same way (§6, §7,
§9).
Written 2026-11-26 on worktree `known-gaps-milestone-orders` from
`8274c951` (`raft-marks-2026-11-26.md` §3). A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/page.md` §2a, §5; `docs/spec/drop-table.md`;
`docs/spec/waystone-concpets.md` §4c; `docs/spec/index.md` §12;
`docs/inflight/known-gaps.md`, "Page reclamation", "WAL", "Backup and
restore (BR)", "The online index build (CI)", "Keystone ids";
`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md` (BF, what a
tombstone's roots reach).

## 0. What DV is

`[source-read]` A reclaim frees what a tombstone's roots reach; a page no
root reaches stays allocated for the life of the volume
(`known-gaps.md:1050`). The free map is unlogged and recovery repairs it
(`CLAUDE.md`, "Multi-page free map"). `[design]` DV first **counts** the
unreachable pages of a volume, by class, and then **frees** the classes
whose unreachability can be proved from the volume alone.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **The leaked classes** (`:1050-1068`): a page a failed heap growth,
  btree split or var-heap growth left unlinked; a failed `CREATE INDEX`'s
  pages its freeing does not reach; a failed or rolled-back `CREATE
  TABLE`'s pages; a refused catalog report's page (`:435`); a previous
  run's undo pages (UP4); a dropped assertion's Bound Cabin chain;
  Waystone pages.
- **Waystone** (`:1069`): every mount strands every opted row's previous
  directory - a root and up to 2,048 trail pages - and every `WAYSTONE
  OFF` strands that row's; the volume grows with mounts and toggles.
- **A crash forgets the free list** (`:1088`).
- **A restore leaks** (`:1706`): a page the source freed after the
  copier read its map region, the Waystone pages, and pre-DA
  `sys.access_stats` growth pages.
- **An online build's refusal between H2 and H3 leaks the root page**
  (`:3259`).
- **Spills** (`:832`): a crash between a key's retire and its spill
  release leaks that row's spills; the mount sweep covers
  `sys.assertions` only. A spilled value an `UPDATE` re-assigns is never
  released (`:1501`). These are values inside live `kVarHeap` pages, not
  pages (DV-Q5).

`[source-read]` in the code at `8274c951`: the maps are persisted by
`DevicePageStore::PersistMaps` (`src/storage/device_page_store.cpp:745`),
called by the reclaim (`src/server/page_reclaim.cpp:446`); the mount's
advisory discard is `DiscardAdvisoryState` (`src/server/restore.cpp:296`)
calling `RenewPatternDirectories` (`:304`); every page with a header
carries `owner_oid` (`docs/spec/page.md` §2a), and a headerless page - a
Waystone directory page - carries none.

**Not measured**: how many pages any of these leaks on a real workload,
per mount or per operation. DV-S0 measures before anything is designed
further.

## 2. The mechanism `[design]`

1. **The census** is a mark from every root the catalog names - each
   relation's `desc_page_id`, each anchor slot's tree, each var-heap
   chain, the undo log, each assertion's Bound Cabin chain, each opted
   pattern's directory, the maps, the catalog chains themselves - over a
   quiescent volume. Allocated and unmarked is a leak, classed by the
   page's class and `owner_oid`.
2. **Report first.** The census's counts are a `sys` view and a mount
   log line; nothing is freed by the stage that builds it.
3. **Free at the source where the source is known.** Waystone's stranded
   directories are named by the row that owned them at the moment they
   are replaced; freeing them there - after the renewal, before the
   listener - needs no sweep. Each such class is closed where it leaks.
4. **Sweep what has no source**, as DV-Q2 marks: at the mount, after
   recovery and before the listener, or on an administrator's statement.
   The free is the existing primitive (the frame discarded and the bit
   cleared in one hold), and a refused or doubtful page is left and
   counted.

## 3. Rules

- **DV-R1. A page is freed only if no root reaches it, proved by a mark
  that visited every root without a refusal** `[quiet-wrong]`: a freed
  reachable page is lost data with no error. One unreadable page, one
  cycle cut, one root the mark could not walk, and the sweep frees
  nothing of that run.
- **DV-R2. The mark and the free run with no writer**: before the
  listener, or under a fence DV-S0 designs and the operator marks.
- **DV-R3. A page allocated after the mark began is never freed by it.**
- **DV-R4. The free map stays unlogged** and its repair at recovery
  stays the authority; DV adds no `ALLOC`/`FREE` record.
- **DV-R5. The sweep's cost is bounded and stated**: pages read per GiB,
  measured, with a mount that skips it by a boot key if DV-Q3 says so.
- **DV-R6. The simulator fails a seed that ends with an unreachable
  allocated page** of a class DV has closed.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DV-Q), CLA's proposals

| | item | options | CLA proposes | mark |
|---|---|---|---|---|
| DV-Q0 | Is DV opened, and with what scope? | (a) the census, the source fixes, then the sweep; (b) the census and the source fixes only; (c) the census only | (a), with S0's numbers deciding whether the sweep is reached | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q1 | Does DV-S0 gate? | (a) S0 measures the leak per class on scenario runs and on 100 mounts; a class leaking under one page per 1,000 operations is recorded and left. (b) every class is closed | (a) | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q2 | When the sweep runs | (a) on an administrator's statement only, offline-safe first - nothing runs it unasked; (b) at every mount; (c) at a mount when the census passes a threshold | (a): a mount's length is a recovery-time promise | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q3 | A mount's census | (a) the census runs at the mount only when a boot key asks; (b) always | (a) | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q4 | Waystone's stranded directories | (a) freed at the renewal and at `OFF`, the page list read from the directory being replaced under invariant 9's discipline; (b) left to the sweep | (a) | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q5 | Leaked spills inside live var-heap pages | (a) outside DV - a value's release is DW's and the var-heap's rule; (b) inside | (a) | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q6 | A restore's leak | (a) the restore's mount runs the sweep when asked, as any mount; (b) always after a restore | (a) | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q4' | DV-Q4 (a) revised against the tree at DV-S2 | (a) at the mount's renewal only: a stale directory's trail pages freed where each still reads `kWaystone`, its interior pages - allocated and headerless - reused as the renewal's fresh roots, the rest counted and left; `OFF` left; (b) DV-Q4 (a) as written, `OFF` included, interior pages freed | (a): `FreePage` refuses a headerless id (`device_page_store.cpp:1785`) because a reused one would read its dead image after a crash, so freeing one needs a durable zero first - a new primitive - while reuse needs none; and an `OFF` runs while another core's recorder may hold the old root in its memo, so freeing there needs a statement-epoch fence against a trail write into a reused logged page (`[quiet-wrong]`), for an operator act whose rate is far under DV-Q1's bar | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q7 | Is the sweep (DV-S3) reached? DV-Q0 (a) left it to S0's numbers | (a) no: S0 measured no class without a source over DV-Q1's bar, so S3 is not built and DV closes narrow; (b) build S3 regardless | (a): every class over the bar has a source fix (DV-S2); a failed growth or split leaked 0 pages over 50,102 `grow`-fault ops; a sweep is the one stage that can free a reachable page (DV-R1, `[quiet-wrong]`) and has nothing measured to free | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q8 | A create rolled back in the run whose queued job a crash loses | (a) a stated known gap: the transaction is decided, so the next mount's undo does not see it, and its pages stay leaked; (b) a durable owed record written at the rollback | (a): the window is a crash between a rollback and its job's gate; a durable record is a new record kind or catalog row, which DV-R4's spirit (no logged free state) argues against | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q9 | What `sys.volume_census` reads | (a) the census the mount took, memory only, empty where `volume_census_at_mount` is off; (b) a census taken at every read | (a): DV-R2 - a mark is only whole with no writer, and a read runs beside writers | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q10 | DV-R6 as built | (a) the simulator fails a seed whose census mark is not whole, and reports the unreached count; the zero per closed source is each source's cell (`VolumeCensusTest.WaystoneMountsStrandNothing`, `ACrashLosersCreateIsFreedAtTheMount`, `InRunReclaimTest.ARolledBackCreateIsFreedInTheRun`); (b) DV-R6 as written - the simulator fails a seed that ends with an unreachable page of a closed class | (a): a page's class does not name its source - a directory an `OFF` or a drop's end of an opt-in strands is `waystone` as a mount's was, and a rolled-back create's page whose queued job the seed's crash lost is `anchor` as any other - so (b) would fail seeds on the leaks DV-Q4' and DV-Q8 leave | (a), adopted on the standing go-ahead of 2026-11-27 |
| DV-Q11 | How DV-Q1's bar is read for a class that leaks only in the `io` fault cells (the close's review: the create's pages and the residual index root both read 2.57 pages a 1,000 ops there and 0.05 over the matrix) | (a) by what an operation leaks with no fault: an uncommitted `CREATE TABLE` leaks 3 pages at every rollback (`VolumeCensusTest.RolledBackCreateTable`, no fault armed) and is closed; a crash loser's own index root needs a log cut between two adjacent records of an uncommitted create, at most one page a crash, and is recorded and left; (b) by the `io` cells' rate for both: both closed; (c) by the matrix's rate for both: neither closed | (a): the `io` cells' denominator is runs that fail-stop within about 100 ops, so their rate prices a fault schedule and not a workload; and seeding the anchor slot before the root's image would move the index root's window rather than close it - a cut after the slot's record leaves a slot naming a page with no image, which the reclaim's walk passes by (`page_reclaim.cpp`, `NotFound`) - a reading no cell checks | (a), adopted on the standing go-ahead of 2026-11-27 |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DV-S0** | The census as a tool on the work branch, never landed as a statement: run over volumes produced by scenario runs, 100 mounts with opted patterns, failed DDL, a restore. Every root kind listed from the code | a results file: leaked pages by class; DV-Q1's bar read |
| **DV-S1** | The census as a `sys` view and a mount log line, report only | cells: each class of §1 produced and counted; a clean volume reads zero |
| **DV-S2** | The source fixes S0 ranks first (Waystone as DV-Q4 marks, the online build's root, a rolled-back `CREATE TABLE`) | cells and DV-R6 per class |
| **DV-S3** | The sweep, as DV-Q2 marks | cells: DV-R1's refusals each leave the volume untouched; a crash mid-sweep; the sim |
| **DV-S4** | The close: the mount's cost measured, `page.md` §5, `drop-table.md`, `known-gaps.md` | the operator's word on landing |

## 6. What DV does not do

- No logged free map, no online compaction, no page relocation
  (`kVarHeap` pages are never relocated).
- No file truncation: a freed page returns to the free list, and the
  file's length stays.
- No reclaim of a retired slot's space inside a live leaf.

## 7. Stage log (CLA, appended as stages land)

### DV-S4 - the close (2026-11-27)

**Closed narrow.** Built: the volume census as a tool, a mount's report
and `sys.volume_census` (S0, S1); an uncommitted `CREATE TABLE`'s pages
reclaimed in the run and at the mount; a mount's stale Waystone
directories taken back (S2). Not built: **DV-S3, the sweep** (DV-Q7 (a) -
S0 measured no class without a source over DV-Q1's bar), so DV-R1's
refusal cells, the crash mid-sweep and DV-Q2's statement do not exist, and
DV-Q6's restore sweep with them; **the online build's root between H2 and
H3** (produced by no S0 run). DV-R5 is answered for the census, the one
thing that reads the volume. Rules as built: DV-R1 holds for the census's
`complete` and for S2's two frees, each by its own check (a link walk from
the create's anchor; a class check per stale page); DV-R2 by where each
runs (before the listener, or past BF-R9's epoch gate); DV-R3 has no sweep
to bind; DV-R4 holds - no record kind was added; DV-R6 as DV-Q10 (a).

**The measurement**
(`bench/v3.5.0/results-dv-close-v3.4.17-182-g82235ad2c.md`; Release,
`630ecc359` against `82235ad2c`, interleaved, 11 rounds, server pinned to
CPUs 0-7, host load 3 to 5 from other sessions' servers):

| shape | `630ecc359` | `82235ad2c` | delta |
|---|---|---|---|
| keyed `INSERT`, server CPU | 6.38 us | 6.32 us | -0.9 % |
| point `SELECT`, server CPU | 6.59 us | 6.46 us | -2.0 % |
| point `UPDATE`, server CPU | 7.51 us | 7.26 us | -3.3 % |
| `CREATE TABLE` + `DROP TABLE` | 562.6 us | 555.3 us | -1.3 % |
| `BEGIN; CREATE TABLE; ROLLBACK` | 265.1 us | 257.7 us | -2.8 % |
| mount to listening, 1 GiB, 16 opted shapes (mean of 12) | 79.2 ms | 81.3 ms | +2.1 ms |

Every delta is inside the run's noise floor (about 4 %; the mount's ranges
67.7-95.2 ms and 75.4-92.4 ms): **no cost DV adds is read, and none of the
negative deltas is a gain.** `volume_census_at_mount = on` adds 179 ms at
0.25 GiB of allocated pages, 660 ms at 1 GiB and 1,878 ms at 4 GiB (about
0.5 to 0.7 s a GiB, warm cache) over a mount of 64 ms to 123 ms without
it (medians 75.4 ms to 117.7 ms). Its
census of the measurement's own volumes read 640 and 1,280 Waystone pages
reached by no root at 1 GiB and 4 GiB and none at 0.25 GiB: the three
volumes are snapshots of one file after its first, second and third mount
under `630ecc359` (the archived `vol.py`), so 0, 640 and 1,280 pages is 640
a mount of that build stranded and none added by the measured mount -
**and a leak made before DV stays, since no sweep is built.** Cut, and stated in the
file: a row-count sweep, a cold-cache mount, the PostgreSQL floor; the
drivers are scratch drivers modelled on `tools/du_close.py`, archived
beside the results, since no `tools/` driver covers these shapes. The run
also read a `CREATE TABLE` + `DROP TABLE` pair growing from about 92 us to
about 1,037 us over 3,300 pairs in both arms alike - not DV's, and not
traced.

**The suite: closed without a green run.** Three runs of
`scripts/test.sh`, each failing only cells that fail alone at the base
commit too: at `82235ad2c` 5,310 of 5,312
(`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`,
`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`); at
`8204fbec2` 5,312 of 5,314
(`ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`,
`SetExprTwoCoreRigTest...`); at `b538f30f1`, the closing code commit, 5,313
of 5,314 (`DurableKickRigTest...`). Repeated alone on this tree:
`DurableKickRigTest` 4 of 15, `SetExprTwoCoreRigTest` 9 of 60.
`ContentionCountersTest`, interleaved against a Debug build of `630ecc359`
from `git archive`: the base 3 of 32, this tree 6 of 32 (and 4 of 8 in one
earlier run alone, on a host loaded by other sessions) - both fail, and 32
rounds do not separate the rates. None is on a path DV changes (a group
commit's kick, first-updater-wins). Read as the three known two-core
flakes, CLA's reading; no run is reported as a pass.

**The sync**: `git fetch origin` at the close found `origin/main` at
`630ecc359`, the commit DV opened from: nothing to merge, nothing
resolved.

**The owning documents**: `page.md` §5 (counted, not swept; `FreePage`'s
callers) and §2a (a burned oid and the undone create); `drop-table.md`
(what stays leaked, the undone create's job, its gate and its walk);
`waystone-concpets.md` §4c (the stale directories taken back) and its
status line; `sys-views.md` §6.4a (`sys.volume_census`);
`known-gaps.md`, "Page reclamation" and the restore's leak;
`manual/server/server.md` (the key, the mount's line) and
`manual/sql/sql.md` (the view); `CLAUDE.md`'s rows for the free map,
`DROP TABLE` and Waystone.

### DV-S1 and DV-S2 - the report, and the two sources closed (2026-11-27)

On `dv-unreachable-page-reclaim`, landed together in the commit after
`5666265fa`.

**DV-S1.** The boot key `volume_census_at_mount` (default `off`, DV-Q3 (a)):
the mount runs the census after recovery, the renewal and the reclaim's
collection, before any listener, logs it (`INFO [census] ...`) and keeps it
for `sys.volume_census` (DV-Q9 (a)), whose first row is the census itself. A
census that cannot name its roots is logged and the mount still opens.
`VolumeCensusMountTest.TheBootKeyAsksTheMountForItsCensus`.

**DV-S2, a `CREATE TABLE` that does not stand.** `Catalog::CreateTable`
reports the relation's roots once its anchor exists; the dispatcher queues
them at the transaction's end unless a create that stood commits
(`PageReclaimer::EnqueueUndoneCreate`, gated as BY-R2's rolled-back index
tree), and the mount's undo hands every `sys.tables` row it retires to
`CollectAtMount`, gated on the log's end after undo. The walk and its owner
checks are the dropped relation's. Cells:
`InRunReclaimTest.ARolledBackCreateIsFreedInTheRun`,
`ARolledBackHeapCreateFreesItsKeystoneIndex`, `ACommittedCreateOwesNothing`,
`VolumeCensusTest.ACrashLosersCreateIsFreedAtTheMount`.

**A burned oid, checked against the tree** `[quiet-wrong]`: `page.md` §2a
says a rolled-back create's oid is issued again after a restart, so a
relation created before the mount's job runs carries the loser's oid on
every page and the walk's owner check passes for both. The job is sound
because it walks by links from the loser's own anchor and finishes its walk
before its first free; `ACrashLosersReclaimSparesARelationThatTookItsOid`
creates that relation - it does take the oid - and reads it back whole
after the reclaim. Found after the stage's review, not by it.

**DV-S2, Waystone (DV-Q4').** `server::DiscardAdvisoryState` takes the stale
directories back before the renewal: trail pages freed where they still
read `kWaystone`, interior pages reused as the fresh roots. 100 mounts of
two opted shapes strand 0 pages (900 before);
`VolumeCensusTest.WaystoneMountsStrandNothing`,
`AStaleDirectoryNamingLivePagesLeavesThem` (the review's, DV-R1: it fails
with the class check removed). `OFF` is left.

**The online build's root between H2 and H3** was produced by no S0 run
and is not closed: its rate is unmeasured, so DV-Q1's bar gives it no
stage.

**The sim matrix again**, the same 240 runs on this build: 240 `SIM ok`,
528 censuses whole; unreached pages 1,684 before, 41 after - 13 trail and
10 directory pages, in one seed's clean runs (a `DROP TABLE`'s end of an
opt-in strands as an `OFF` does: 9 pages in
`VolumeCensusTest.ADropTablesEndOfAnOptInStrandsItsDirectory`; that the
seed's 23 are all a drop's is CLA's reading, not traced) and 18
`kIndexLeaf` pages, each a crash loser's own index root
whose image reached the log and whose anchor slot did not (0.05 a 1,000
ops over the matrix, read as under the bar; but all 18 are in the `io`
cells, 2.57 a 1,000 of their 7,002 ops, the per-cell rate S0 read the
create's pages over the bar at; DV-Q11 (a) is how the bar is read for
both - `known-gaps.md`). **DV-R6 is built as DV-Q10 (a)**: the
sim fails a seed whose mark is not whole.

Review (`critics-developer`): no defect in the change; the harvest's
premise checked against crashes, restores, shared roots and the catalog
cache, and pinned by a cell it added. Applied: the undone create's mount
gate moved to the log's end after undo (risk 1); the view's census row
(risk 2); a census that fails no longer refuses the mount (risk 3); the
failed-abort dependency stated at the queueing site (risk 4); DV-R6's
narrowing recorded as DV-Q10 (risk 5); a page whose reformat failed counted
as left, and the leak windows written to `known-gaps.md` (risk 6); the
three `{oid, roots}` structs made one (S1); the header comment merged (S3);
the nested ternary unfolded (S4); the mount cell made to record by running
each instance twice. Rejected: S2, one shared directory walker - the three
walks differ in what a failed check means (skip, refuse, count), and a
walker with two callbacks is as long as the two loops it replaces; S5, the
hook's payload copy - a mount-only cost with no profile behind it.

**The suite** at this stage's build: 5,310 of 5,312 passed; the two
failures are the two two-core cells that fail alone at the base commit too
(`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, 4 of 15 alone
here; `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`, 9
of 60 alone here), neither on a path DV changes. Run again at the close.

Overhead not measured; measured at the milestone's close.

### DV-S0 - the census, measured (2026-11-27)

On `dv-unreachable-page-reclaim`, the tool at `db21cbffc`
(`v3.4.17-180-gdb21cbffc`): `server::RunVolumeCensus`
(`src/server/volume_census.cpp`), the sim's count after every reconciled
mount and every restore, and `tests/volume_census_test.cpp`. Results:
`bench/v3.5.0/results-dv-s0-census-v3.4.17-180-gdb21cbffc.md`. Every root
kind was listed from the code - every `CreateNew`, `CreateNewHeaderless` and
`CreateAt` caller under `src/` - and the review confirmed none is missing.
**DV-Q1's bar, read**: Waystone pages (4.7 to 8.3 a 1,000 ops; 9 a mount
for two opted shapes) and an uncommitted `CREATE TABLE`'s pages (3 to 4
each) are over it; a failed growth, split or unique index build leaked
nothing; the refused catalog report, the dropped assertion's chain and the
online build's root between H2 and H3 were produced by no run. The sweep
is therefore not reached (DV-Q7 (a)). Review (`critics-developer`): three
defects fixed - the clean cell's assertion was never created (its prefix
matched every reply), a catalog var-heap and a Bound Cabin chain could end
short of their tails without a refusal; F1, F2, S1, S3, S4, S5, S7 applied;
S6 (one `sys.tables` scan) rejected - no public scan, and the census runs on
request; S2 (the sim's older live-relation census) left to DV-R6.

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
