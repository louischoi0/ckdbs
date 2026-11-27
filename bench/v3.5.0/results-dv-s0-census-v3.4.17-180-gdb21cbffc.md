# DV-S0 - the volume census: leaked pages by class

Measured at `v3.4.17-180-gdb21cbffc` (`db21cbffc`), worktree
`dv-unreachable-page-reclaim`, 2026-11-27. Both runs used copies of that
commit's binaries: `ckdbs-sim` sha256 `de0ac0a3a7a63e4a...`, and
`kds_tests`. The matrix was run a first time on the same census code
before its review's fixes, which touched no counting path. That run gave
the identical 240 verdicts and per-class counts.

Work order: `instructions/v3.2.0/workorder-dv-unreachable-page-reclaim.md`
(DV-S0, DV-Q1 (a)). Measured on worktree `dv-unreachable-page-reclaim`.

**What this measures is a count, not a time**: pages a volume holds
allocated that no root reaches, by class. `bench/README.md`'s five rules
govern timings; rule 1 (Release) is waived for this file because the count
is the same in either build, and the run used the worktree's Debug `build`
(stated, not implied). Rules 2-5 are not applicable: in-memory sim devices,
no server, no port. Host: `/` on `/dev/mapper/fedora-root` (xfs), load
average 0.14 at the end of the runs, no competing build.

## The tool

`server::RunVolumeCensus` (`src/server/volume_census.cpp`), on the work
branch only: a mark from every root the catalog names - the catalog chains
and their var-heap chains, every live relation and pending tombstone by
`PageReclaimer::ReachFrom`, every assertion's Bound Cabin chain, every
pattern row's Waystone directory and its trails, this run's undo pages -
then every allocated id above the fixed range and outside the maps that no
root marked, classed by page type and `owner_oid`. A previous run's undo
page is counted apart (`unprovable`). The mark is whole or it is not a mark
(DV-R1): every run below reported `complete`.

## Run 1 - the sim matrix

`ckdbs-sim` with the census after every reconciled mount and after every
restore (`sim/loop.cpp`, `CountUnreached`). 240 runs: seeds 1-5,
20260826003, 20261127000, 20261127001; modes `clean`, `sync-crash`,
`crash`; faults `none`, `io`, `grow` at rate 40; toggles `off`,
`waystone`, `on`; `--checkpoint-every 100`; plus one `--backup --toggles
on` run per seed and mode. 1,500 ops, 2 iterations each. **240 of 240 runs
`SIM ok`; 528 censuses, 0 incomplete; 357,716 ops**; wall time 17 s at 12
in parallel.

| class | pages | per 1,000 ops |
|---|---|---|
| `waystone` (trail pages) | 1,133 pages | 3.17 pages |
| `waystone_dir` (directory pages) | 479 pages | 1.34 pages |
| `anchor` | 18 pages | 0.05 pages |
| `heap_data` | 18 pages | 0.05 pages |
| `index_leaf` | 18 pages | 0.05 pages |
| `varheap` | 18 pages | 0.05 pages |
| every other class | 0 pages | 0 pages |

By configuration (ops are each cell's own):

| toggles / faults | ops | leaked per 1,000 ops |
|---|---|---|
| off / none | 50,102 ops | 0 pages |
| off / grow | 50,102 ops | 0 pages |
| off / io | 2,334 ops | 10.3 pages (anchor, heap_data, index_leaf, varheap: 2.57 pages each) |
| waystone or on / none | 50,102 ops | 7.21 pages (5.45 trail, 1.76 directory) |
| waystone or on / grow | 50,102 ops | 4.73 pages (3.09 trail, 1.64 directory) |
| on / backup + restore | 50,102 ops | 8.30 pages (5.53 trail, 2.77 directory) |

The `io` cells' four-page set is one heap relation's anchor, data head,
Keystone index leaf and var-heap root: a `CREATE TABLE ... HEAP` that an
injected error failed after its pages were made and whose transaction was
rolled back. `io` runs fail-stop early (2,334 ops over 72 runs), so its
rate is per few ops.

## Run 2 - the cells (`tests/volume_census_test.cpp`)

| cell | leaked |
|---|---|
| every root kind on one volume (catalog chains and var-heaps, relations, an assertion's chain, a Waystone directory), nothing leaked | 0 pages of 56 allocated |
| a live relation's var-heap chain cut after its root | the mark refused: `complete` false |
| 2 opted shapes, 100 clean mounts | 900 pages: 100 directory, 800 trail (9 pages a mount; 90 after 10 mounts) |
| `WAYSTONE ON` / `OFF` x 10 | 82 pages: 10 directory, 72 trail |
| `BEGIN; CREATE TABLE; INSERT; ROLLBACK` x 10 | 30 pages: anchor, btree leaf, var-heap root each; still 30 after a mount |
| `CREATE UNIQUE INDEX` over duplicates, refused x 10 | 0 pages |
| 5 tables with an index and spills, dropped, a mount | 0 pages |

## DV-Q1's bar, read

DV-Q1 (a): a class under one page per 1,000 operations is recorded and left.

- **Waystone (directories and trails): over the bar** - 4.7 to 8.3 pages
  per 1,000 ops in the sim, 9 pages a mount for two opted shapes, about 8
  pages an `OFF`. Closed at its source in DV-S2.
- **A rolled-back or failed `CREATE TABLE`: over the bar** where creates
  roll back (2.57 pages per 1,000 ops a page class under `io`; 3 to 4 pages
  each). Closed at its source in DV-S2.
- **A failed heap growth, btree split or var-heap growth**: 0 pages under
  `grow` faults over 50,102 ops. Under the bar; recorded and left.
- **A failed `CREATE INDEX`**: 0 pages on the refused unique build, which
  BY's reclaim already frees. Under the bar; recorded and left.
- **A restore**: the restored volumes leaked only Waystone pages. The
  source's free-after-map-read and pre-DA `sys.access_stats` cases were
  not produced.
- **A refused catalog report's page, a dropped assertion's Bound Cabin
  chain, the online build's root between H2 and H3**: not produced by any
  run - no fault seam reaches them in the sim. Unmeasured, so not shown to
  exceed the bar.
- **A previous run's undo pages**: 0 to 4 per census, `unprovable` by
  construction - counted, never classed as leaked.

## What was cut

The scenario drivers against a server (`tools/`) were not run: the census
reads a quiesced volume through the engine's catalog, which the sim gives
at every mount and a live server does not before DV-S1. The sim matrix
stands in for them.
