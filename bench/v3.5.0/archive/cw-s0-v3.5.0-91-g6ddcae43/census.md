# CW-S0 census: the shared page fetch sites, the frame-changing acts, three facts

Census for `instructions/v3.2.0/workorder-cw-optimistic-page-read.md` §5 (CW-S0), read on
worktree `sync-stale-records`, branch `worktree-cw-optimistic-page-read`, at `6ddcae43`.
Read-only: no file in the repository edited, nothing built, nothing run but `grep`/`sed`.
Every `[source-read]` claim cites `path:line` at `6ddcae43`; `[inferred]` marks what was
derived rather than read. Line numbers have moved since the order's `7aa75760` reading
(`FetchAndPin` is `src/storage/device_page_store.cpp:2757`, not `:2439`).

## Summary

- **Part A, 55 direct shared fetch sites: copy-out 40, in-place 2, held-across 13.** 20 of the
  40 copy-out sites raise a `Status` from bytes under the hold (`chk`). A further 38 caller and
  visitor rows (A.2) say what runs under a ref a helper hands back.
- **Hot paths, 10,000-row single btree, height 2:** a point SELECT makes **4 shared fetches**
  (anchor 1, root 2, leaf 1; catalog, var-heap and undo 0). An issued-key INSERT makes **4
  fetches, 1 shared** (`sys.tables` X, root S, rightmost leaf X, undo tail X), plus a split
  one insert in about 166.
- **Part B: one steady-state site writes page bytes without X while armed.** Writeback stamps
  the CRC into the live frame under S (`src/storage/device_page_store.cpp:2228`, re-read and
  confirmed by the parent at `6ddcae43`). Two narrower sites stamp `page_lsn` after release
  (B2b, B2c). The C3-only sites are listed beside them.
- **Part C:** a Release build can arm a one-core store only through a harness that calls
  `SetLatchArmed` (`bench/frame_table_hold_bench.cpp:132`), never through the server. An armed
  read hit takes 2 structure-latch holds and 2 CAS on the word; a write hit takes 3 holds and
  2 CAS. Both also do a `hits_` `fetch_add` and the store-wide pin gauge. `Frame` is 56 bytes,
  unaligned, inside a node of a node-based `unordered_map` that is freed at eviction. The slot
  bytes live in chunks that never move.

Part A was read by the parent session. Parts B and C were read by a forked session at the same
commit (its file is `cw-s0-census-partBC.md`, merged here unchanged apart from the headings).
The parent re-read B1. Key for B and C: "DPS" = `src/storage/device_page_store.cpp`,
"DPS.h" = `include/kds/storage/device_page_store.hpp`.

## Part A - every shared page fetch site

### A.0 Classes and the rule applied

- `copy-out` (CO): what leaves the hold is a copy (a page id, a few fields, a row buffer, a
  vector); the `PageRef` dies before anything is emitted, another page fetched, or the result
  acted on. A comparison under the hold that only decides *what* to copy stays CO. Marked
  `chk` where a type/width check under the hold raises `Status` (`Corruption`) - CW-R1 would
  move that raise after validation.
- `in-place` (IP): the hold is live when the statement's answer is computed and acted on
  through the span (a `Status`/verdict returned, a row emitted), with no other fetch,
  suspension, callback or lock under it.
- `held-across` (HA): the `PageRef` is held across another page fetch, a callback/consumer,
  a lock or latch acquisition, or is returned/moved to the caller.

The primitive every site below reaches: `PageStore::GetForRead` = `Fetch(id, kRead)`
(`include/kds/storage/page_store.hpp:319`), `Fetch` maps `kRead`/`kScan`/`kScanNoWait` to
`FetchPinned(..., PinMode::kShared, for_read=true, heat)` (`page_store.hpp:302-313`), which
`DevicePageStore` overrides (`src/storage/device_page_store.cpp:2624`, into `FetchAndPin`
`:2757`). The grep `GetForRead\(|PageAccess::kRead|PageAccess::kScan|PinMode::kShared` over
`src/` and `include/` gives 96 lines in 36 files at `6ddcae43`; 41 of them are declarations,
comments, defaults, the `Fetch` dispatch or a pass-through of an `access` variable, and the
rest plus the five `store.Fetch(id, access)` sites (`heap_store.cpp:102`,
`heap_chain.cpp:160`, `catalog.cpp:186`, `index_tree.cpp:639`, `btree.cpp:1387`), the scan
ring and the writeback copy are the 55 direct sites of A.1.

### A.1 Direct fetch sites (55)

| # | site | function | page class | class | hot? | note |
|---|---|---|---|---|---|---|
| 1 | `src/storage/btree/btree.cpp:109` | `LeafStillCoversKey` | btree leaf (right sibling) | CO chk | INSERT: no (rightmost leaf returns at `:108` before the fetch); point SELECT: miss only (`:1276`) | reads type + `min_key`; taken while the caller holds the left leaf (S on a read, X on a write) |
| 2 | `src/storage/btree/btree.cpp:208` | `DescendTo` (internal node; also the root-leaf probe of a write descent, released at `:222` before `Get`) | btree internal | CO chk | **HOT: point SELECT and INSERT** (the root) | reads `RequireType`, `level`, `ChildFor` (`:270-273`); the `StatusOr<PageRef>` is a loop-body local, so it dies before the child is fetched (no coupling, `btree.cpp:163-170`) |
| 3 | `src/storage/btree/btree.cpp:208` | `DescendTo` (leaf, `leaf_for_write=false`) | btree leaf | HA (returned in `Descent.leaf`, `:212`) | **HOT: point SELECT** | `BtreeLookup` then runs `SearchLeaf` in place (`:1270`) and returns `Location{..., held}` (`:1272`) |
| 4 | `src/storage/btree/btree.cpp:499` | `FetchParent` (re-descent, attempt > 0) | btree internal | CO chk | no (a split whose recorded parent moved) | under the split's exclusive leaf hold |
| 5 | `src/storage/btree/btree.cpp:1328` | `BtreeLeftmostLeaf` | btree internal / leaf | CO chk | no (walk start) | |
| 6 | `src/storage/btree/btree.cpp:1387` | `BtreeFetchLeaf` (wrapper, any `access`) | btree leaf | HA (returned) | via walks | callers in A.2 |
| 7 | `src/storage/btree/btree.cpp:1420` | `BtreeHeight` | btree root | CO chk | no (`sys` views) | |
| 8 | `src/storage/btree/btree.cpp:1442` | `BtreeLeafCount` | btree leaf | CO | no (`sys` views) | |
| 9 | `src/storage/index/index_tree.cpp:73` | `LeafStillCoversKey` | index leaf (right sibling) | CO chk | no | `memcmp` result only; under the caller's leaf hold |
| 10 | `src/storage/index/index_tree.cpp:162` | `DescendTo` (internal) | index internal | CO chk | no (no secondary index in `point`/`insert1`) | `CheckAgainst`, `ChildFor` (`:218-225`) |
| 11 | `src/storage/index/index_tree.cpp:162` | `DescendTo` (leaf, read) | index leaf | HA (returned, `:172`) | no | `IndexSeekLeaf` keeps only the id; `IndexFetchLeafAtOrBelow` keeps the ref |
| 12 | `src/storage/index/index_tree.cpp:291` | `FetchParent` (re-descent) | index internal | CO chk | no | |
| 13 | `src/storage/index/index_tree.cpp:349` | `LeftmostLeaf` | index internal / leaf | CO chk | no | |
| 14 | `src/storage/index/index_tree.cpp:639` | `IndexFetchLeaf` (wrapper) | index leaf | HA (returned) | no | callers in A.2 |
| 15 | `src/storage/index/index_tree.cpp:792` | `IndexHeight` | index root | CO chk | no | |
| 16 | `src/storage/index/index_tree.cpp:812` | `IndexLeafCount` | index leaf | CO | no | |
| 17 | `src/storage/heap/heap_chain.cpp:160` | `ChainVisitOnePage` (`ChainVisit`) | catalog chain page / heap page | HA (callback per slot, `:168`) | no on a warm memo | callers in A.2 |
| 18 | `src/catalog/catalog.cpp:186` | `ForFirstRow` (`access` param) | catalog chain page | HA (callback per row, `:198`) | INSERT reaches it with `kWrite` only (A.3) | read callers `:1898`, `:2111` (A.2) |
| 19 | `src/catalog/catalog.cpp:969` | `Catalog::OwedIndexTrees` (anchor) | anchor | CO chk | no (mount/reclaim) | slots copied into `out` |
| 20 | `src/catalog/catalog.cpp:1262` | `Catalog::CheckAnchorRoomForIndex` | anchor | IP | no (DDL) | the `Status` answer is computed through the span |
| 21 | `src/catalog/catalog.cpp:2741` | `Catalog::InitTableAccess` (anchor) | anchor | HA (`anchor_held`, across `ListCabins`/`ListForeignKeys`/`ListIndexes` chain walks, `:2797`, `:2831`, `:2853`) | no (memo miss only; `:2694` answers a hit with no fetch) | |
| 22 | `src/exec/heap_store.cpp:102` | `FetchHeapRow` | heap data page (DN) | HA (returned `Location`, `:127`) | no (btree relation) | |
| 23 | `src/exec/assertion_recover.cpp:43` | `AttachEntriesFromPages` | Bound Cabin page (other) | CO chk | no (mount) | |
| 24 | `src/exec/index_maintain.cpp:154` | `SlotRoot` | anchor | CO | no (index maintenance retry) | |
| 25 | `src/exec/index_maintain.cpp:249` | `AppendIndexEntry` | index leaf | CO | no | **S taken under this core's own X** on the same page (`placed.held`), admitted by `page_latch.hpp:25-31` |
| 26 | `src/exec/assertion_build.cpp:66` | `BoundCabinChainWriter` open walk | Bound Cabin page | CO chk | no | |
| 27 | `src/exec/assertion_build.cpp:196` | assertion build phase 1 | btree leaf | CO chk | no (DDL) | `TxnConflict` raised under the hold (`:213`) |
| 28 | `src/exec/index_ddl.cpp:144` | index page enumeration | index internal / leaf | CO chk | no | |
| 29 | `src/exec/index_ddl.cpp:500` | backfill phase 1 | btree leaf / heap page | CO chk | no | |
| 30 | `src/exec/index_ddl.cpp:577` | high-water scan | btree leaf / heap page | CO | no | |
| 31 | `src/exec/tuple_verify.cpp:15` | `VerifyTupleAt` | btree leaf / heap page | HA (returned in `VerifiedTuple.ref`, `:61`) | point SELECT only via memo, trail or Cabin hint | callers in A.2 |
| 32 | `src/exec/tuple_verify.cpp:68` | `CurrentPageEpoch(store, id)` | btree leaf | CO | no | |
| 33 | `src/exec/heap_release.cpp:107` | `HeapSlotRelease` scan | heap data page | HA (lock) | no (core 0 tick) | `HeapSpace::AddFree` takes `mu_` (`heap_store.cpp:44`) and reads the in-flight table under the hold |
| 34 | `src/exec/step_vm.cpp:326` | `ChainRunner::StillOwned` (anchor, then `desc_page_id` = root) | anchor + btree root | CO | **HOT: point SELECT (2 fetches)** | reads the 8-byte owner oid only (BF-R10) |
| 35 | `src/storage/varheap.cpp:331` | var-heap tail walk | var-heap | CO chk | no (no spill) | |
| 36 | `src/storage/varheap.cpp:411` | `varheap::Fetch` | var-heap | HA (pin moved to the caller, span returned, `:422-424`) | no (no var column) | its one caller copies (A.2) |
| 37 | `src/storage/varheap.cpp:435` | `ChainLength` | var-heap | CO | no | |
| 38 | `src/exec/varheap_sweep.cpp:38` | var-heap sweep | var-heap | CO | no | |
| 39 | `src/exec/wal_row_log.cpp:82` | `LogChainInsert` (chain growth) | heap chain page | CO | no | `min_key` only |
| 40 | `src/stats/plan_stats.cpp:68` | `ClusteredRoot` | anchor | CO chk | no (steward) | |
| 41 | `src/stats/plan_stats.cpp:100` | `ReadNode` | btree internal / leaf | CO chk | no (steward) | `NodeFacts`, "copied out under its hold" (`:78-80`) |
| 42 | `src/stats/plan_stats.cpp:175` | `ReadRelationPrior` | anchor | CO chk | no | |
| 43 | `src/stats/plan_stats.cpp:919` | `RelationGather::Step` | btree leaf / heap page | CO | no (`GATHER STATISTICS`) | |
| 44 | `src/stats/trail_store.cpp:105` | trail write compare (`TrailIsUnchanged`) | Waystone trail page | IP | no (opted shapes only) | the compare's answer returns while held |
| 45 | `src/stats/trail_store.cpp:158` | `ReadTrail` | Waystone trail page | CO | no | |
| 46 | `src/stats/waystone_dir.cpp:64` | `LookupWaystonePage` | Waystone directory (headerless) | CO | no | |
| 47 | `src/txn/undo_log.cpp:182` | `UndoLog::Read` | undo page | CO | no (every point row is visible) | "Copied, not viewed" (`:197`) |
| 48 | `src/server/sys_views.cpp:455` | `CurrentRootOf` | anchor | CO | no | |
| 49 | `src/server/sys_views.cpp:1903` | page inspection view | any | CO | no | rows built under the hold, emitted after |
| 50 | `src/server/mount_recovery.cpp:157` | post-recovery audit | descriptor / var-heap root / anchor | CO | no (mount) | existence only |
| 51 | `src/server/page_reclaim.cpp:246` | `PageReclaimer::VisitOne` | any relation page | CO | no | ids pushed to the frontier |
| 52 | `include/kds/storage/log_page_image.hpp:93` | `LogBtreeSplit` | page a split created and does not hold | CO | INSERT: 1 in ~166 (a split) | whole 8 KiB copied (`:96`) |
| 53 | `include/kds/storage/page_store.hpp:806` | `PlainScanFetcher::Fetch` | any | HA (kept until the next `Fetch`) | no | the non-device store's ring |
| 54 | `src/storage/device_page_store.cpp:1337` | `ScanRing::Fetch` (`FetchAndPin` kShared, `kRing`) | btree leaf | HA (pin + S until the next `Fetch`, `:1308-1320`) | no (Cabin builds) | |
| 55 | `src/storage/device_page_store.cpp:2192` | writeback copy (`AcquirePageLatch` kShared, or try-only) | any dirty page | CO | no (checkpoint / drain) | an 8 KiB copy under S; Part B |

**Per class (direct sites): copy-out 40, in-place 2, held-across 13; total 55.**
`chk` (a `Status` raised from bytes under the hold): 20 of the 40 copy-out sites.

### A.2 Callers of a helper that hands the ref or span back, and visitor walks

What each caller does with the hold it is handed (its own class).

| caller | helper | page class | class | hot? | what runs under the hold |
|---|---|---|---|---|---|
| `src/exec/step_vm.cpp:781` `AcceptByKey` | `LocateRow` (`heap_store.cpp:166`) -> `BtreeLookup` (`btree.cpp:1261`) | btree leaf | HA | **HOT: point SELECT** | `AcceptTupleAt` (`:790`): the row is copied into `version_` at `:2877` under `PageSpanGuard` (a counter, `:55-67`, not the `PageRef`), then `DecodeAndResolve` (var-heap fetches, `:2965`), the residual, the next step, the sink's `Emit` (`command_dispatcher.cpp:7625`); the leaf drops when `found` dies at `AcceptByKey`'s return |
| `src/exec/step_vm.cpp:719` memo hit | `VerifyTupleAt` | btree leaf | HA | within one statement only | `AcceptTupleAt` |
| `src/exec/step_vm.cpp:764` trail replay | `VerifyTupleAt` | btree leaf | HA | opted shapes only | `AcceptTupleAt` |
| `src/exec/step_vm.cpp:1421` / `:1432` Cabin bucket | `VerifyTupleAt` / `LocateRow` | btree leaf | HA | no | `AcceptTupleAt` |
| `src/exec/step_vm.cpp:1732` / `:1735` `ResolveIndexedRow` | `FetchHeapRow` / `LocateRow` | heap / btree leaf | HA | no | `AcceptTupleAt` (`:1710`) |
| `src/exec/step_vm.cpp:2194` Cabin serve hint | `VerifyTupleAt` | btree leaf | CO | no | dropped at `continue` |
| `src/exec/step_vm.cpp:2209` Cabin serve miss | `LocateRow` | btree leaf | HA | no | `entries.Heal` under the Cabin partition latch (`:2236`) |
| `src/exec/step_vm.cpp:2280` / `:2292` located rows | `VerifyTupleAt` / `LocateRow` | btree leaf | HA | no | `AcceptTupleAt` |
| `src/exec/step_vm.cpp:2493` outermost and inner walks | `BtreeFetchLeaf` (`kScan`, `kScanNoWait`, `kRead`) | btree leaf | HA | no (scans) | `visit_slot` -> `AcceptTupleAt` -> sink, per slot; the coroutine suspends only between leaves (`:2546-2550`) |
| `src/exec/step_vm.cpp:2713` reverse walk | `BtreeFetchLeafAtOrBelow` | btree leaf | HA | no | `AcceptTupleAt` per slot |
| `src/exec/step_vm.cpp:1878` ordered index walk | `IndexFetchLeaf` | index leaf | CO | no | entries compared and copied into `batch`, released at the block end before rows are resolved |
| `src/exec/step_vm.cpp:1968` descending index walk | `IndexFetchLeafAtOrBelow` | index leaf | CO | no | the same |
| `src/exec/step_vm.cpp:1558` index probe | `IndexVisitFrom` | index leaf | CO | no | tuple keys copied; `CoveredRowSurvives` evaluated under the hold |
| `src/storage/btree/btree.cpp:135` `HopRight` | `BtreeFetchLeaf` | btree leaf | HA | no | moved into the caller's ref |
| `src/storage/btree/btree.cpp:1373` `BtreeFetchLeafAtOrBelow` | `BtreeFetchLeaf` | btree leaf | HA | no | `LeafStillCoversKey` fetches the sibling under it; returned |
| `src/storage/btree/btree.cpp:1401` `BtreeVisitLeafPage` | `BtreeFetchLeaf` | btree leaf | HA | no | the callback per slot |
| `src/storage/index/index_tree.cpp:136` `HopRight`, `:673` `IndexFetchLeafAtOrBelow`, `:763` `IndexVisitFrom` | `IndexFetchLeaf` | index leaf | HA | no | moved / next fetched before the previous drops / callback |
| `src/exec/heap_release.cpp:243`, `src/exec/index_purge.cpp:30` | `IndexFetchLeaf` | index leaf | CO | no | entries copied |
| `src/exec/index_maintain.cpp:432` `CheckUniqueKey` | `LocateRow` | btree leaf | CO | no | payload copied, `Release` at `:447` before the decode |
| `src/exec/fk_check.cpp:101` forward check | `LocateRow` | parent btree leaf | IP | no | verdict decided from the header, returned while held |
| `src/exec/fk_check.cpp:177` / `:187` child Cabin serve | `VerifyTupleAt` / `LocateRow` | child btree leaf | HA | no | `set.Heal` under the partition latch, the column decode |
| `src/exec/cabin_optimizer_exec.cpp:413` / `:420` | `VerifyTupleAt` / `LocateRow` | btree leaf | CO | no | epoch copied |
| `src/exec/cabin_optimizer_exec.cpp:196` | `ScanFetcher::Fetch` (ring) | btree leaf | CO | no | phase 1 staged under the ring page |
| `src/exec/index_ddl.cpp:277-278` `ReadRowAgain` | `FetchHeapRow` / `BtreeLookup` | heap / btree leaf | CO | no | `StagedRow` copied |
| `src/exec/row_codec.cpp:1214` `ResolveSpills` | `varheap::Fetch` | var-heap | CO | no | `AssignBytes` copies, pin drops per spill |
| `src/server/command_dispatcher.cpp:7903` `ReadRowByKey` | `LocateRow` | btree leaf | CO | no | payload copied |
| `src/server/command_dispatcher.cpp:8225` `NamedKeyVerdict` | `LocateRow` | btree leaf | IP | no (named-key INSERT) | a `Status` built from the header, released at `:8241` before the undo read |
| `src/server/command_dispatcher.cpp:8488`, `:8626` | `LocateRow` | btree leaf | CO | no | copied, released (`:8495`, `:8635`) |
| `src/server/command_dispatcher.cpp:8881` heap duplicate | `LocateRow` | heap page | IP | no | `DuplicateKey` from the tuple |
| `src/server/command_dispatcher.cpp:13765` `FOR UPDATE` point arm | `LocateByTupleKey(.., kRead)` (`:9798`) | btree leaf | HA | no | `walk.Qualify` and `walk.Claim` (a lock-table try) under S (`:13753-13762`) |
| `src/server/command_dispatcher.cpp:9264` `WalkBtreeLeaves`, `:9368` `WalkHeapPages` (kRead from `:12747` `CollectTargets`, `:13796` `FOR UPDATE` scan) | `BtreeVisitLeafPage` | btree leaf / heap | HA | no | `Qualify` (`:12589`): spill decode, the WHERE with sub-chains, `ResolveThroughUndo` (an undo page fetch) - all under the leaf's hold |
| `src/server/command_dispatcher.cpp:15254` `VisitKeyedSlotsFrom` (PURGE) | `BtreeVisitFrom` | btree leaf | HA | no | the `visit` judge callback |
| `src/exec/fk_check.cpp:348` reverse check | `BtreeVisit` | child btree leaf | IP | no | visibility and verdict per slot; moved rows copied |
| `src/exec/heap_store.cpp:151`, `src/exec/index_maintain.cpp:404`, `src/storage/index/index_tree.cpp:825` | `IndexVisitFrom` / `IndexVisit` | index leaf | CO | no | keys / rowids / a count copied |
| `src/catalog/catalog.cpp:90` `ScanAll` (31 call sites: every `List*`/`Get*` read) | `ChainVisit` | catalog chain | CO | no on a warm memo | rows decoded into a vector |
| `src/catalog/catalog.cpp:1652` `CheckObjects`, `:4741` index name check | `ChainVisit` | catalog chain | IP | no (DDL) | the verdict callback runs under the hold |
| `src/catalog/catalog.cpp:1898`, `:2111` | `ForFirstRow(.., kRead)` | `sys.columns` chain | CO | no (DDL) | names / positions copied |
| `src/exec/catalog_spills.cpp:59`, `src/exec/declaration_relation.cpp:94` | `ChainVisit` | catalog / declaration chain | CO | no | refs / staged rows copied |

### A.3 The two hot statements

Relation: `CREATE TABLE t (id int64, c0 int64, c1 int64) BTREE`, 10,000 rows loaded by
issued-key INSERTs (`tools/ba_census.py:164`, `:201-202`, `:241`, `:274`).

**Height** `[inferred]` from the constants: a leaf holds `(8192 - 32 - 16 - 4) / (5 + 20 + 24)`
= 166 rows (`src/stats/plan_stats.cpp:159-166` `LeafRowCapacity`; `heap_page.hpp:76,116,176`;
`page_header.hpp:68`); 10,000 issued keys append to the rightmost leaf, which an append split
leaves full (`btree.cpp:1101-1105`), so 61 leaves; an internal node holds `(8192 - 32 - 16) / 12`
= 678 entries (`btree_page.hpp:81,109`), so the root is one level-1 node over 61 leaves:
**height 2**.

**Point SELECT** (`SELECT * FROM t WHERE id = k`), call chain `[source-read]`:
`SelectRun::Execute` `co_await exec::ExecuteAsync` (`src/server/command_dispatcher.cpp:7647`)
-> `ExecuteChain` (`src/exec/step_vm.cpp:3638`) -> `ChainRunner::Run` (`:291`) -> `Bind`
(`:296`, `:303`) -> `Catalog::InitTableAccess` memo hit (`src/catalog/catalog.cpp:2694`, no fetch)
-> `StillOwned` (`step_vm.cpp:323`): `GetForRead(anchor)`, `GetForRead(desc_page_id)` (`:326`)
-> `RunStep` (`:625`) -> `AcceptByKey` (`:679`, `:704`) -> `exec::LocateRow` (`:781`) ->
`btree::BtreeLookup` (`src/exec/heap_store.cpp:170`, `btree.cpp:1261`) -> `DescendTo`
(`btree.cpp:1265`): `GetForRead(root)` (`:208`, internal, released at the loop body's end),
`GetForRead(leaf)` (`:208`, kept, `:212`) -> `SearchLeaf` in place (`:1270`) -> `Location`
(`:1272`) -> `AcceptTupleAt` (`step_vm.cpp:790`) with the leaf held to `AcceptByKey`'s return.

| page | fetches | mode | site |
|---|---|---|---|
| anchor | 1 | S | `step_vm.cpp:326` (StillOwned) |
| root (internal, level 1) | **2** | S | `step_vm.cpp:326` (StillOwned) and `btree.cpp:208` (descent) |
| leaf | 1 | S | `btree.cpp:208`, held to the emit |
| catalog / system pages | 0 | - | memo hit (`catalog.cpp:2694`); `Revalidate` reads a word (`catalog.cpp:1100-1110`) |
| var-heap | 0 | - | no var column |
| undo / visibility | 0 | - | `txn::Classify` on the snapshot answers `kVisible` (`step_vm.cpp:2842-2877`); no undo walk, no page |
| **total** | **4 shared fetches** | | the order's DO row records "a `point` statement 12 holds before DO" = 4 x 3 holds (`workorder-cw-optimistic-page-read.md` §1.1), consistent |

**INSERT with an issued key** (`INSERT INTO t VALUES (i, n)`, the key omitted), call chain
`[source-read]`: `CommandDispatcher::InsertInner` (`command_dispatcher.cpp:7764`) ->
`InitTableAccess` memo (`:7988`) -> `Catalog::AllocateRowId` (`:8958`, `catalog.cpp:2926`) ->
`ForFirstRow<SysTableRow>(kCatalogPageTables = 7, kWrite)` (`catalog.cpp:2928`,
`well_known.hpp:386`) -> `store.Fetch(7, kWrite)` = `Get` (`catalog.cpp:186`), the row
overwritten in place and logged (`:2958`) -> `place()` -> `btree::BtreeInsert`
(`command_dispatcher.cpp:8877`, `btree.cpp:1031`) -> `DescendTo(leaf_for_write)`
(`btree.cpp:1037`): `GetForRead(root)` (`:208`, internal; `child_is_leaf` from its level,
`:272`), `Get(rightmost leaf)` (`:208`, X at once, DK's D1) -> `LeafStillCoversKey` returns at
`:108` (no right sibling) -> `PlaceUnderHold` -> `SearchLeaf` (`:1067`) -> `InsertTupleAt`
(`:1079`) -> `TransactionManager::AppendUndo` (`command_dispatcher.cpp:9132`,
`manager.cpp:716`) -> `UndoLog::TailFor` -> `Get(tail_)` (`undo_log.cpp:85`) -> `LogInsert`
(`command_dispatcher.cpp:9144`): `StampPageLsn(leaf)` (`:7135`, the structure latch, no page
fetch), the holds dropped at `:7145`.

| page | fetches | mode | site |
|---|---|---|---|
| `sys.tables` (catalog chain page 7) | 1 `[inferred]`: one per chain page until the relation's row | X (`kWrite`, marks dirty) | `catalog.cpp:186` |
| root (internal) | 1 | S | `btree.cpp:208` |
| rightmost leaf | 1 | X | `btree.cpp:208` |
| undo tail page | 1 | X | `undo_log.cpp:85` |
| var-heap / index / assertion | 0 | - | no spill, no secondary index, no assertion |
| **total (no split)** | **4 fetches, 1 of them shared** | | plus 3 `StampPageLsn` structure-latch holds `[inferred]`: the `sys.tables` overwrite, the undo write, the insert record |
| one insert in ~166 (a split) | + parent `Get` (`btree.cpp:509`), the new leaf's create, `LogBtreeSplit` (`log_page_image.hpp:88-97`; held pages are not refetched) | X | |

### A.4 What Part A finds

1. **Half the point SELECT's fetches are the bind's ownership check.** `StillOwned`
   (`step_vm.cpp:323-335`) fetches the anchor and the root to read one 8-byte owner oid each,
   then the descent fetches the root again: the root is pinned twice and the anchor once per
   point statement, every statement, every core. This is the cheapest optimistic read in the
   tree (one word, copy-out) and, separately, a fetch the bind could fold into the descent
   `[design]`.
2. **Every internal-node read is copy-out already**, in both trees (`btree.cpp:208`, `:499`,
   `:1328`; `index_tree.cpp:162`, `:291`, `:349`): C3a needs no restructuring of the descent,
   only CW-R1's deferral of the `RequireType`/`CheckAgainst` `Corruption` raised under the hold.
3. **The point lookup's leaf is held across the emit, though the row is already copied.**
   `AcceptTupleAt` copies the row into `version_` (`step_vm.cpp:2877`) before it decodes, so
   C3b's copy exists; what holds the leaf is `Location`'s `PageRef` (`btree.cpp:1272`), the
   memo's epoch read (`step_vm.cpp:789`), and the scope of `found` - the sink's `Emit` and any
   var-heap fetch run under the leaf's shared latch today.
4. **A shared hold under the same core's exclusive hold exists** (`index_maintain.cpp:249`;
   the catalog's root X across a read of its own chain, `page_latch.hpp:25-31`). An optimistic
   reader on the X holder's own task would read its own unreleased writes; CW-S2 has to say
   whether such a site may go optimistic.
5. **Write-path walks run whole statements under a leaf's hold** (`command_dispatcher.cpp:12589`
   `Qualify`: spills, sub-chains, an undo walk; `:13753-13762` a lock try under S). They stay
   latched under CW-Q3 (a), and they are why a scan leaf is not a C3 candidate.

## Part B - every act that changes a frame's bytes or its identity

Holds key: **SL** = structure latch `frames_latch_` (DPS.h:1677); **ML** =
free-map latch (DPS.h:1699); **X / S** = the frame's page latch word held
exclusive / shared; **pin** = `Frame::pins` counted. At `cores = 1` every
latch is null (`structure_latch()` DPS.h:1678, `map_latch()` DPS.h:1700,
`AcquirePageLatch` acts only inside `if (latch_armed_)` DPS:3080): one thread, no
latch at all, so every row below is "no latch" unarmed and CW-R5 makes that
irrelevant.

| # | site | what changes | holds (armed) | armed-unsafe? | note |
|---|---|---|---|---|---|
| B1 | **`WriteBack` checksum stamp**, DPS:2228 (`StampIfHeadered(page_id, *page.frame->bytes)`), copy DPS:2236 | **bytes**: the 4-byte CRC at `kPageChecksumOffset` (page_header.cpp:159-161), written **into the frame**, not into the scratch copy | SL (DPS:2208) + **S only** (DPS:2192 `kWait`, or `TryAcquirePageLatchShared` in `kSkip`) + pin + `writing` claim | **YES - flagged.** A page-byte write under a *shared* page latch: any other core's shared reader of the page (every `GetForRead`, the scan ring) can be reading it at that instant; a data race today, and a write an optimistic copy-validate reader would not see move a version. | Today no hit-path reader interprets the checksum (verified only on a fault, DPS:1246) `[inferred: no reader of the field on a hit found]`; a whole-page copy under S elsewhere (log_page_image.hpp:93-96) can capture it mid-write. S2 must either exclude the field from what an optimistic reader trusts or move the stamp to the scratch copy. |
| B2 | **`StampPageLsn`**, DPS:1876-1906 (`SetPageLsn(*frame.bytes)` DPS:1894) | **bytes**: `page_lsn` (8 B); metadata `dirty`, `dirty_gen`, `rec_lsn`, `stamped_since_copy` | SL only (DPS:1886); the page latch is the caller's ("made safe by the caller's page latch", DPS:1883-1884) | Safe only where the caller holds X. Caller census below (B2a-B2c). | 24 `src/`/`include/` call sites checked. |
| B2a | callers holding X at the stamp | page_lsn | X held by the caller | no | assertion_build.cpp:92 (`Placed.page`, assertion_build.hpp:113-117); assertion_check.cpp:663 (`placed` alive from :613); assertion_check.cpp:994 (`page` = `Get`, :941); wal_row_log.cpp:117 via heap_release.cpp:155 (`Get` :146), heap_release.cpp:285 (`Get` :273), declaration_relation.cpp:295 (`ChainVisit kWrite` :276-277), purge_key.cpp:65 (`LocateRow kWrite`); wal_row_log.cpp:143 via clean_dropped.cpp:92 (`LocateRow kWrite`) and wal_row_log.cpp:101 (`ChainInsertResult::held`, heap_chain.hpp:89-92); wal_row_log.cpp:66 via the `on_append` sink (row_codec.hpp:131-142, row_codec.cpp:252-254; declaration_relation.cpp:211-226; command_dispatcher.cpp:6996-7012); row_compensation.cpp:17 (caller's `heap::PageView&`); undo_log.cpp:33 (`bytes`/`created` held, :121, :140), undo_log.cpp:64 (`tail` held, :160-173); varheap_release.cpp:47 (`page` held); catalog.cpp:243/264/277 (callers pass a held `PageView&`, :285-302; `ReportPlacedRow` "a page this holds pinned and exclusive" :427-428; the sweep's `ChainVisit kWrite` :1133); catalog.cpp:333 via :1275 (`staged` PageRef) and :1500 (`created_anchor`); catalog.cpp:999 (`anchor` = `Get` :987); log_page_image.hpp:43 (own `Get` :35); DPS:2695 (`LogTakenImage`, under the write fetch's X, DPS:2647-2650); command_dispatcher.cpp:7135 (insert, `placed.held`, :7138-7140); :13469 (`page_again` = `Get` :13397, alive); :16873 (`again` = `Get` :16848); :6993 when reached through the transaction's sink (index_maintain.cpp:241-264, `IndexWriteLogFor` :7018-7023) |
| B2b | **`LogBtreeSplit`'s created pages**, log_page_image.hpp:93-106 | page_lsn of a page the split created | **none** on that page: fetched `GetForRead`, copied, released at the loop iteration's end (:93-96), then stamped (:106) | **YES - flagged (narrow).** A byte write with no page latch. Reachable by a latched reader only through pages the caller holds X (`held`, :56-62), so no latched reader can be on it; a C3 pin-less reader with a stale or reused id could. | The comment accepts the eviction case (`NotFound` tolerated, :103-109). |
| B2c | **the no-manager dispatcher's deferred logs**: index writes kept with `held = {}` (command_dispatcher.cpp:7029-7043) and logged by `LogIndexWrites` (:7111 → :6993, or `LogTreeSplit` with no holds → B2b's `GetForRead` path for every page); spills collected on `appended` and logged by `LogSpills` (:7105 → wal_row_log.cpp:66) | page_lsn of index leaves/nodes and var-heap pages | **none**: the index holds die with `IndexInsertResult` (index_maintain.cpp:262-266), and `AppendedSpill` carries no PageRef (row_codec.hpp:98-112) | **YES - flagged (scope-limited).** Stamps after release. Reached only when `scope.txn == nullptr` (`SpillLogFor` :6998, `IndexWriteLogFor` :7018), i.e. "a dispatcher with no manager returns an empty scope" (command_dispatcher.hpp:897-899). | Production dispatchers carry a TransactionManager `[inferred: not traced to Expeditor's construction]`; unverified, checked by reading where `txn_` is set for the server's dispatcher. |
| B3 | `FetchAndPin` exclusive hit, DPS:2800-2827 → `HitLocked` DPS:1142-1153 → `PinFrameAndRelease` DPS:2893-2910 → `AcquirePageLatch(kExclusive)` DPS:3119, re-mark DPS:3148-3155 | metadata before X (`dirty`, `dirty_gen` DPS:1147, `usage` :1151, `pins`); bytes then written by the caller | SL for the mark and pin; X by CAS after SL is released; SL again for the re-mark | no | The dirty mark precedes X (DPS.h:1005-1016); writeback's generation check covers the gap. |
| B4 | `FaultIn` device read into a reserved slot, DPS:1190 / :1204 | **bytes** of a slot | **none**: the slot is reserved (`ReserveFrame` DPS:901), on neither the free list nor the table (DPS.h:1817-1820), so no `find` reaches it | no for a pinned reader (no frame names the slot); **YES for C3** - a pin-less reader holding a memo of that slot reads the new bytes with no latch | Published by `InsertFrame` (B6) under SL. |
| B5 | `PublishFreshPage` zero fill, DPS:868 (every create: `CreateNamedIdUnpinned` :1586, `CreateNewFrom` :1641, `CreateClaimedUnpinned` :1732) | **bytes** zeroed | **none** (reserved slot, not in the table) | **C3 only**, as B4 | Then `InsertFrame(kCreate)`; X is taken only after the publish (`CreatePinned` DPS:2942-2971), so between publish and the X grant another core finding the id reads a zeroed page `[inferred: no id is known to anyone yet, except a stale reused id]`. |
| B6 | `InsertFrame`, DPS:789-864 (`try_emplace` :841, `SlotOwner` :843, `bytes` :844) | **identity**: a Frame node created and pointed at a slot; `dirty`, `dirty_gen` (:851), `usage`, `owes_image` | SL (DPS:814) | no | The lost-race arm returns the reserved slot (:822, :828). C3 needs a version move here (CW-R3). |
| B7 | `ReleaseFrameLocked`, DPS:1119-1127 (`frames_.erase` :1125, `ReturnSlotLocked` :1126) | **identity**: the node - and with it the latch word, `pins`, `usage`, `dirty_gen` - is **freed**; the slot goes to the free list | SL; caller has refused `pins != 0` (asserted :1123) | no for pinned readers; **C3: the Frame address dies**, only the slot memory survives | Three callers: sweep DPS:3439, `FreePage` DPS:1814, `ReleaseScanSlot` DPS:1286. |
| B8 | `ReturnSlotLocked`, DPS:1107-1117 | **bytes** (debug only: `memset 0xEF` :1112; ASan poison :1114); slot owner → invalid (:1115) | SL | **C3 only** (and debug-only for the bytes) | Reached from B7 and from every `ReservedFrame` given back unpublished (DPS.h:1534-1538). |
| B9 | CLOCK sweep `SweepLocked`, DPS:3386-3444 | metadata `usage` decrement (:3422), `queued` (:3430); **identity** via B7 (:3439) | SL; refuses `pins`, pinned class, `IsHeld(frame.latch)` (:3415-3417), dirty | no | Run by `EvictColdFrames` DPS:3374-3384 (SL :3382), `InlineBatchLocked` from `ReserveFrame` (SL :919), `MaintainFreeReserve` (SL :2381). Under no pool pressure it never runs. |
| B10 | `ReleaseScanSlot`, DPS:1263-1287 | identity via B7 | SL (:1270); refuses dirty, pinned, `usage > 0`, pinned class, a held latch (:1284-1285) | no | The ring unpins first (`DropHeld` → `UnpinFrame`, DPS:1358-1362). |
| B11 | `FreePage`, DPS:1767-1830 | identity (B7) + the free-map bit cleared + the id onto `free_list_` (:1816-1822) | SL then ML (:1777-1778); deferred while pinned, `writing`, latched (:1801-1803), loading, claiming, claimed (:1793-1795) | no | A reused id is re-created through B5/B6. |
| B12 | slot reuse: `TakeSlotLocked` DPS:969-1000 (new chunk `make_unique_for_overwrite` :986, pop :997), `free_slots_` | identity of a slot (owner invalid until published) | SL (the caller's, `ReserveFrame` :919) | C3 only | Chunks never move or free while the store lives (DPS.h:1801-1809). |
| B13 | `CreatePinned`, DPS:2912-2985 (all five kinds: `kAt`, `kNew`, `kNewHeaderless`, `kClaimed`, `kOverTorn`) | identity + bytes via B5/B6 | raw create outside SL (:2926-2937); then SL for `CountPin` (:2942-2955); X by CAS after (:2971); evicted-meanwhile arm refetches via `FetchPinned(kExclusive)` (:2980) | no | `drop_always` drops the image debt in the X grant's hold. |
| B14 | Redo, `wal/redo.cpp:429` (`Get`), `:441` (`CreateOverTorn`), `:449` (`CreateAt`); writes `SetPageLsn` :513, `FULL_PAGE_IMAGE` `memcpy` :520, `PACKED_PAGE_IMAGE` unpacked to a buffer then the same `memcpy` (:555-565) | bytes (whole page for an image or PAGE_INIT heal) | through the held PageRef: X when armed; mount thread, no second thread alive (DPS.h:164-170) | no | Redo never writes a frame directly; torn-page healing is `CreateOverTorn` → B5 → image. |
| B15 | In-place leaf compaction, `src/storage/btree/btree.cpp:736-786` (`CreateEmptyAs` over `descent.leaf.bytes()` :767, epoch +1 :784); leaf division epoch, btree.cpp:947 | bytes (whole leaf rebuilt in place); `page_epoch` | X: the write descent's leaf | no | `PURGE INDEX`'s rewrite is the write descent's held leaf (index_tree.cpp:679, `leaf_for_write=true` :696) and its image `LogFullPageImage` takes its own `Get` (log_page_image.hpp:35). |
| B16 | Writeback copy (read), DPS:2236 | none (reads); metadata `dirty_gen` read, `stamped_since_copy` reset, `owes_image` set (:2215-2222), `writing` claim (:2170, cleared :2326 and :2112) | SL + S + pin | the read itself is sound; its neighbour B1 is the flag | `HighestDirtyPageLsn` (DPS:1947-1975) reads `page_lsn` under SL with no page latch - a read, ordered against B2 by SL. |
| B17 | `LogOwedImage` / `LogTakenImage`, DPS:2658-2697 | `owes_image`; page_lsn via B2 | X (the write fetch, `FetchPinned` DPS:2647-2650) | no | Unarmed: `LogOwedImage` looks the debt up. |
| B18 | `MarkFrameDirty` / `PageRef::MarkDirty`, DPS:3253-3260, page_store.hpp:903-905 | metadata only | SL | no | **No caller in `src/` or `include/`** (grep at `6ddcae43`): no engine path marks a shared-fetched page dirty, so a write through a `GetForRead` handle would be a lost write by the header's own rule (DPS.h:79-80). Whether any shared-fetch site writes through its span is Part A's question. |
| B19 | `GetUnpinned` / `GetForReadUnpinned`, DPS:1862-1868 → `Resolve` DPS:1129-1140 | metadata: `HitLocked`'s dirty mark and usage bump, or a fault | **none** - no SL around `Resolve`'s `find` and `HitLocked` | **unsafe if called armed** (an unlatched table read and frame write) | **No caller in `src/`** outside the store's own seams (the base `FetchPinned` default page_store.hpp:280, which `DevicePageStore` overrides). Public on `DevicePageStore` (DPS.h:281-286, :304); tests call them (tests/btree_test.cpp and 8 others). DPS.h:1660-1664 says so. |
| B20 | Backup and restore | none on frames | - | no | Backup reads the device through its write fence (`ReadPageRunFenced`, src/server/backup.cpp:183; `ArmedFence` :210, :284) and the maps by `MapImages` (DPS:2550-2565, ML; map regions are not frames, DPS.h:1716-1724). Restore writes the device before any store opens (src/server/restore.cpp:47-69, :140-205). |
| B21 | Page 0, the anchor and the transaction-id ceiling | bytes | `Get` (X): src/server/expeditor.cpp:1512, src/server/superblock_checkpoint_anchor.cpp:124 | no | The unlogged writes the X re-mark exists for (DPS:3128-3141). |
| B22 | `IsPinnedClassFrame` (DPS:3320) and `FreePage`'s type test (DPS:1805) | none (reads the page-type byte) | SL, no page latch | read-only | A read that can race an X holder formatting the page - a tsan-visible race, not a wrong write. |

**Flagged (writes page bytes without X while armed):**

1. **B1, DPS:2228** - writeback stamps the CRC into the live frame under a
   shared page latch. The one byte-writer on the steady-state path.
2. **B2b, log_page_image.hpp:106** - a split's created pages stamped after
   their `GetForRead` copy released them; latched readers cannot reach them
   (every route is X-held), a C3 reader could.
3. **B2c, command_dispatcher.cpp:7029-7043 → :6993 / :7105** - stamps after
   release, only in a dispatcher with no transaction manager.
4. **C3-only (a pin-less reader with a slot memo):** B4 FaultIn's read
   (DPS:1190, :1204), B5 the create's zero fill (DPS:868), B8 the debug
   poison (DPS:1112), and identity changes B6/B7/B12 - none takes a page latch
   because no table lookup can reach the slot; each is a CW-R3 version-move
   site.
5. **B19** - the unpinned `Resolve` path is unlatched; no `src/` caller.


**B18 answered from Part A:** none of the 55 direct sites or the A.2 callers writes through its
shared span. Each was read under the hold. `trail_store.cpp:105` compares under S and takes a
separate `Get` (`:110`) before it writes. `PageRef::MarkDirty` has no caller (B18).

## Part C

### C1. Can a Release build arm the page latch on a one-core store?

**Not through the server; yes through a direct harness.**
`latch_forced_` and the `KDS_TEST_PAGE_LATCH` read exist only under
`#ifndef NDEBUG` (DPS:405-428, the env read :422-426, `latch_forced_ = true`
:424). In NDEBUG the only arming call is `SetLatchArmed`, public and
unconditional (DPS.h:764-767: `latch_armed_ = armed || latch_forced_`). The
server calls it with `core_count > 1` only: core 0 at
src/server/expeditor.cpp:966-967 and an owned peer store at
src/server/core_runtime.cpp:190. A Release binary that links `kds` and
opens a store itself can arm it at one core:
`bench/frame_table_hold_bench.cpp:132` does `store.SetLatchArmed(true,
readers + 1)`, built when `KDS_BUILD_BENCH=ON` (CMakeLists.txt:264-274).
So M5 is buildable as a store-level harness, not as a one-core server cell.
Two caveats for reading M5: arming switches **all four** latches (page word,
SL, ML, map flush latch: DPS.h:1203, :1678, :1700), and the unarmed hit
path is a different path - three hash lookups and an `IsAllocated` test
(`Resolve` DPS:1135-1137, `PinFrame` DPS:3001-3005, `UnpinFrame`
DPS:3166-3175) against the armed path's one lookup (DPS:2807) - so
armed-vs-unarmed at one core is not "the words alone". An alternative
`[inferred]`: a `cores = 2` instance with both reactors on one CPU is armed
by the server's own rule.

### C2. Shared writes on one armed hit, after DO

`PageStore::Fetch`/`Get` (page_store.hpp:288-311) → `DevicePageStore::FetchPinned`
(DPS:2624-2656) → `FetchAndPin` (DPS:2757-2891); release by
`PageRef::Release` (page_store.hpp:891-901), which calls `UnpinFrameAt`
whenever the handle carries a frame - always, armed (`*frame = pinned`,
DPS:2640-2641, :2654) - and `UnpinFrame` only for a frameless handle.

| | SL holds | page-word atomic RMWs | other shared atomic RMWs | plain shared writes under SL |
|---|---|---|---|---|
| **armed read hit** | **2**: FetchAndPin's `unique_lock` (DPS:2800, dropped in `PinFrameAndRelease` :2905), `UnpinFrameAt` (DPS:3210) | **2** CAS: `TryAcquire` (page_latch.hpp:134-150, one `compare_exchange_weak` :145 uncontended), `Release` (page_latch.hpp:173-190, CAS :186); each after a load; a contended acquire loops and counts `Contention` (DPS:3118-3122) | **1** `hits_.Add()` (DPS:1146), a relaxed `fetch_add` on a store-wide counter (DPS.h:1851-1861) - a locked RMW on x86 `[inferred]`; plus each SL hold's `std::mutex` try_lock and unlock, 2 RMWs a hold `[inferred: glibc]` | `frame.usage` ++ only while below `kClockUsageCap` = 5 (DPS:1151; a hot page saturates and stops writing it); `frame.pins` ++ (`CountPin` DPS:3029) and -- (`UncountPin` DPS:3229); store `live_pins_` ++/-- (DPS:3031, :3232); `pin_high_water_` only when it rises (DPS:3032) |
| **armed write hit** | **3**: DPS:2800; the X re-mark in `AcquirePageLatch` (DPS:3148); `UnpinFrameAt` (DPS:3210). A logged mutation adds 1 more per stamp (`StampPageLsn` DPS:1886) and an owed image adds `LogTakenImage`'s append | **2** CAS (acquire X, release) | as the read hit | the read hit's, plus `dirty`/`dirty_gen` twice (DPS:1147, :3149) and the store's `dirty_gens_` twice, `owes_image` when owed (DPS:3150-3154) |
| scan-ring hit (for comparison) | **3**: DPS:2800, then `DropHeld` → `UnpinFrame` (find hold DPS:3167, then `UnpinFrameAt` DPS:3210) | 2 | as above | no usage bump (`kRing`, DPS:1151) |

So a read hit is **7 locked RMWs on shared lines** (4 on the mutex, 2 on the
frame's word, 1 on `hits_`) `[inferred instruction count]`, and writes at
least **four distinct shared cache lines**: `frames_latch_`'s mutex
(DPS.h:1677, no `alignas`, declared directly before `map_latch_` :1699),
the store's pin accounting (`live_pins_` DPS.h:1786, beside `clock_hand_`,
`capacity_`, `window_promised_`, `dirty_gens_` :1779-1797), the
`hits_` counter (DPS.h:1859, 15 contiguous 8-byte counters), and the frame's
node (latch word, `pins`, `usage`). The work order's "three holds and two
CAS" (§1.2, at `7aa75760`) is now two holds and two CAS (DO), and it did not
count `hits_` or the store-wide pin gauge. The hit path asks no free-map bit
in release (asserted in debug only, DPS:2822).

### C3. The Frame and the table

- `struct Frame` DPS.h:942-1027, `static_assert(sizeof(Frame) == 56)`
  DPS.h:1029; no `alignas` anywhere in the header, no padding to a line.
  Field order and offsets `[inferred by layout rules, alignof 8]`:
  `bytes` 0 (:944), `page_id` 8, `slot` 12, `dirty` 16, `queued` 17,
  **`latch` 20** (:956), `rec_lsn` 24, `stamped_since_copy` 32,
  **`pins` 40** (:978), **`usage` 44** (:985), `writing` 45, `owes_image`
  46, **`dirty_gen` 48** (:1017). The latch word is a plain `uint32_t`
  accessed through `std::atomic_ref` (DPS.h:952-956, page_latch.hpp:140, :174);
  `pins`, `usage`, `dirty_gen` are plain, under SL (DPS.h:973-978).
- `frames_` is `std::unordered_map<PageId, Frame>` (DPS.h:1824): node-based,
  the Frame **inside the node** (DPS.h:934-940), reserved at `Open` to the
  configured capacity so it never rehashes (DPS:381, asserted :838). Under
  libstdc++ 14 (`/usr/bin/c++`, build-release/CMakeCache.txt) with
  `std::hash<uint32_t>` no hash is cached, so a node is `next` (8) + key (4,
  padded to 8) + Frame (56) = 72 bytes, the Frame at node+16, the latch at
  node+36, `pins` node+56, `usage` node+60, `dirty_gen` node+64
  `[inferred: /usr/include/c++/14/bits/hashtable_policy.h:315-334,
  hashtable.h:48-52]`. Nodes come from `malloc` (16-byte aligned, 80-byte
  chunks): latch, `pins` and `usage` share one 64-byte line for 3 of the 4
  possible alignments and straddle two for the fourth, and neighbouring
  frames' nodes can share a line `[inferred]`.
- The node, and so the latch word, is **freed at eviction or free**
  (`frames_.erase` DPS:1125). The page bytes live in **slots**: chunks of
  `kFrameChunk` = 1,024 slots (DPS.h:1829), one
  `make_unique_for_overwrite<std::byte[]>` per chunk (DPS:986), stride
  `kPageSize + 64` (DPS.h:1522), plus a per-slot `owners` array
  (DPS.h:1810-1813); the chunk array is sized at `Open` (DPS:376-377) and no
  chunk moves or is freed while the store lives (DPS.h:1801-1809).
  `Frame::bytes` points into the slot (DPS:844).
- Consequence for S2 `[design]`: a C3 version word cannot live in `Frame`
  (its memory is freed and reused by `malloc` at B7); it has to live in
  slot-owned memory beside `owners`, the only memory that outlives a frame.
  C2 (pin kept) could put it in `Frame`.

## Not verified (and how to check)

- The `point` and `insert1` fetch counts are read off the code, not counted. M1 counts them.
  The one-page `sys.tables` walk in `AllocateRowId` is `[inferred]`: it holds for a catalog
  whose `sys.tables` row for the relation sits on page 7.
- The three `StampPageLsn` holds an INSERT makes are `[inferred]`. `LogCatRow` and
  `LogUndoWrite` were not read to the stamp.
- The 10,000-row height (2) is arithmetic on the layout constants, not a reading of a built
  tree. The `sys` view that calls `BtreeHeight` (`src/server/sys_views.cpp:900`) on the bench instance would check it.
- The 31 `ScanAll` callers were classed by `ScanAll`'s own callback, not read one by one.
- Whether production dispatchers can ever run with `scope.txn == nullptr`
  (B2c): read where the server's `CommandDispatcher` gets its
  `TransactionManager`.
- Instruction-level RMW counts (glibc mutex, relaxed `fetch_add`) and the
  node/cache-line layout: inferred, not disassembled or measured; `pahole`
  or an `offsetof` probe on `build-release` would check them (a build, out
  of this census's scope).
