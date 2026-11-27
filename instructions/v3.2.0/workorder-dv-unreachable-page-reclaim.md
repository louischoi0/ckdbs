# Work order DV - pages no root reaches: counted, then reclaimed

Status: **OPENED 2026-11-27** on worktree `dv-unreachable-page-reclaim`
(branch `worktree-dv-unreachable-page-reclaim`) from `630ecc359`
(`v3.4.17-178-g630ecc359`), on the operator's standing go-ahead
(go-ahead-achieving-milestone; `raft-marks-2026-11-27.md` §1), **DV-Q0..Q6
adopted as CLA proposed, (a) in each, by that word and not as per-item
marks**. Written 2026-11-26 on worktree `known-gaps-milestone-orders` from
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
