# Work order BF — DROP TABLE page reclamation: a dropped relation's pages go back to the allocator once no replay and no reader can reach them

Written 2026-10-07 on `worktree-drop-table-page-reclaim` from `bc144dbf`
(`v2.7.0-668-gbc144dbf`), on the operator's word, verbatim:

- *"다음으로 drop table 페이지 회수 기능에 대한 작업 지시서"* - next, the work
  order for DROP TABLE page reclamation.
- **W1:** *"BF-Q0..Q 제안대로 마킹해줘"* - mark BF-Q0..Q as proposed.

**Opened 2026-10-07 by W1** (`raft-marks-2026-10-07.md` §22): BF-Q0..Q18
are marked as proposed, so BF-R1..R13 are rulings. The order's writing is
recorded at §21.

**Run to its close on W2** (`raft-marks-2026-10-07.md` §23): *"go ahead dont
stop until milestone, follow CLA proposal if decision needed"*. Each stage's
row in §6 says where it stands. **BF-S1 is built:** the censuses, the
premise, which held, and the red cells. **BF-S2 is built:** the free
primitive, the free list and the map write barrier, with no caller yet. **BF-S3 is built:** the carrier, the
gate, the walk and the mount-time reclaim. **BF-S4 is built:** the sim's drop op,
its owner census, and the two-core rigs.

- BF-S0 moved no file under `src/`, `include/` or `tests/`, and no suite
  ran.

**BF is not part of AR0 §8's chain.** It answers the leak `drop-table.md`
DT1 states plainly: a dropped relation's pages "stay allocated and
unreachable". BF gates no open order. It shares one surface each with BE
and BA, and BF-Q14 decides its order against BE:

- **BE** (opened, BE-S1 next) rebuilds the frame table and names three
  erasers (BE-R1, `workorder-be-bounded-pool.md:301`). BF's discard is a
  fourth (BF-R5, BF-Q14).
- **BA** (paused) takes `IsAllocated` off the hit path "because nothing
  frees a page" (BA-R5, `workorder-ba-parallelism.md:817-818`). BF falsifies
  that reason and supplies another (BF-R5).
- **BD** (closed at BD-S6, in this tree) is BF's base: superblock 20.
  BD-R4's purge obligation is about rows of a live relation, and BF does not
  touch it. A reclaimed relation's Keystone ids die with it, since K1 binds
  an id within one relation's lifetime.
- **BB.** BF touches none of BB's rules. BB-R7's heap arm is untouched.

## 0. What BF is

**Today a dropped relation's pages are leaked for the life of the volume.**
`DROP TABLE` writes catalog rows only (§1.1), and nothing clears a free-map
bit (§1.3). The drop's own log line, *"pages orphaned pending reclamation"*
(`command_dispatcher.cpp:2696-2698`), names a reclamation that does not
exist. A workload that creates and drops relations grows the file without
bound.

**Freeing is not a bit clear.** Five parts of the engine rest on "nothing
frees a page", and a bare clear breaks each one:

1. **Replay.** The free map is unlogged and written with no WAL gate, and
   redo loads a page before its `page_lsn` gate. A cleared bit on disk,
   with the dropped relation's records still in the next replay range,
   refuses the mount (§1.4).
2. **Durability.** A commit can return before it is durable, and the drop
   is atomic but not isolated. A free ahead of durability lets a crash
   restore the relation over freed, possibly reused, pages (§1.4).
3. **Readers.** A refused read borrow reads on, a statement can bind from a
   stale memo, and four walkers take no borrow at all. Each is sound today
   only because the pages stay allocated (§1.6).
4. **Remembered locations.** The one verifier checks the epoch and the
   Keystone id, never the owner, and Keystone ids collide across relations
   (§1.7).
5. **The store.** The hit path, `CreateNew`, the allocation cursor, the
   erasers and the headerless bit each state the premise (§1.3, §1.5).

**BF's answer, in one sentence.** The drop records what it owes in its own
tombstone. After the commit, a reclaim walks from those roots and frees a
page only once no replay can name it and no reader can reach it, checking
every page's owner on the way, and the allocator hands the id out again
from a free list.

**BF's rules, one line each:**

- **BF-R1.** Reclamation follows a durable commit and is never part of the
  drop. A rolled-back drop, or a loser at mount, frees nothing.
- **BF-R2.** The tombstone carries the relation's two roots. The drop's own
  retype writes them, and they are cleared once the reclaim is durable. A
  tombstone with roots is a pending reclaim.
- **BF-R3.** The pages are found by a walk from those roots, and every page
  is checked for its class and its owner. Nothing is freed through a page
  that fails the check.
- **BF-R4.** A page is freed only once no record that names it can be
  replayed again: the durable redo start is past the drop's commit. No WAL
  record is added.
- **BF-R5.** One free primitive drops the frame, dirty or not, and clears
  the bit in one hold. It defers a frame that is pinned, latched, loading
  or claimed by a writeback. `EvictClean` is deleted, and the map flush
  gains a write barrier.
- **BF-R6.** A freed id comes back through an in-memory free list that
  `CreateNew` asks first, skipping an id with a fault in flight.
  `CreateNewHeaderless` never pops it. The cursor and the recovery floor
  are untouched.
- **BF-R7.** A reclaim frees children before parents and the anchor last,
  makes the map durable through the barrier before it clears the
  tombstone, and is re-driven at every mount until it completes. A chain's
  order is BF-Q18's.
- **BF-R8.** The first deliverable reclaims at mount. Tombstones a mount
  finds pending are freed on the system tick, and no statement of that run
  can race them (BF-Q17).
- **BF-R9.** Reclaiming within the run is gated on BF-Q9: a drop's pages are
  freed once every statement that could have bound the relation has ended,
  each core publishing before it reads the schema word.
- **BF-R10.** Defence in depth: the verifier and every walk's root check the
  page's class and owner, and `ReadTuple` bounds its slot. None of these is
  the authority.
- **BF-R11.** `SHOW META` reports allocation and reclamation. There is no
  configuration key.
- **BF-R12.** Checked, not only argued: the sim drops relations, its oracle
  checks what the drop's roots reached against a ledger of stated leaks,
  and every crash cut and mutation is run.
- **BF-R13.** The text: every sentence that says nothing frees a page, or
  that rests on DT1's allocated pages, is restated.

**What BF leaves standing:**

- **Invariants 8 and 9.** A trail entry or hint that names a freed page is
  a miss, and the reader falls through to a pk descent (BF-R10).
- **Invariants 13 and 14.** No live tuple and no live var-heap value moves.
  A var-heap page is freed only together with the relation that every
  pointer into it belongs to (§1.2).
- **Invariant 1** is untouched. Separately, no page below 128 and no map
  page is ever freed (BF-R5).
- **Invariants 2 and 3.** A reused id is a new creation: `min_key` is fixed
  at that creation and immutable until the page is freed, and the text of
  both invariants says so (BF-S6).
- **DT2 and K1.** The tombstone is kept forever, so a dropped oid is still
  never reissued. Keystone ids are per relation, and K1 is untouched.

**BF does not:**

- **Reclaim a page that no structure of the dropped relation reaches.**
  That covers pages left unlinked by failed growth or splits, a failed
  `CREATE INDEX`'s tree, a failed or rolled-back `CREATE TABLE`'s pages, a
  refused catalog report's page, a previous run's undo pages (UP4), a
  dropped assertion's Bound Cabin chain, and Waystone pages. Each stays a
  stated leak (§1.2, BF-S6).
- **Reclaim a tree at `DROP INDEX`** (BF-Q8 (b)). A dropped index's tree is
  reclaimed with its relation, from the anchor slot that still names it.
- **Shrink the file or punch holes.** `page.md` §14 makes that a non-goal,
  so BF reuses ids instead.
- **Log `ALLOC` or `FREE`** (BF-Q2). Both stay reserved and refused at redo.
- **Purge deleted rows.** BD-R4's obligation stands as written.
- **Retire a dropped relation's Waystone trails, `sys.patterns` rows or
  `sys.access_stats` ghosts.** DT4 already makes them answer nothing, and
  `waystone-concpets.md` §9 says trails are never retired.
- **Lift relayout's quarantine.** `physical-optimizer.md` §4 stays for a
  mover. BF answers gate 3 for a dropped relation only (BF-R13).
- **Change `SHOW PAGE`.** It already reads any page id a client names
  (`command_dispatcher.cpp:1853`). After reuse an old id shows its new
  owner's bytes, which BF states rather than changes.
- **Reclaim the orphans of a volume written before BF.** Their tombstones
  carry no roots. Under BF-Q4 no such volume mounts.

## 1. Survey at `bc144dbf`

This section was read, not run. It draws on six read-only surveys:

- the free map, the allocator and the format;
- what a relation owns, and how `DROP TABLE` runs;
- every holder of a page id outside the relation;
- readers, MVCC and the horizon;
- the WAL, recovery and crash safety;
- the buffer pool and the neighbouring orders.

Where two surveys disagreed, CLA read the code. §1.9 lists each
disagreement and says whether the read settled it.

### 1.1 What `DROP TABLE` writes, and where the roots go

- **The handler.** `HandleDropTable` (`command_dispatcher.cpp:2592-2702`)
  does four things in order:
  1. it resolves the oid under the session's view;
  2. it runs the two RESTRICT checks, first parent-side foreign keys over an
     unfiltered `ListForeignKeys`, then assertions (`:2629-2650`);
  3. it takes the relation `X` (`BorrowRelationForDdl`, `:2667`), a
     non-queueing ask that parks where a reactor exists (`:8343-8372`);
  4. it calls `Catalog::DropTable`, forgets the Cabin sets (`:2688-2693`),
     and logs "pages orphaned pending reclamation" (`:2696-2698`).
- **The catalog write.** `Catalog::DropTable` (`catalog.cpp:1926-2106`)
  reads and writes no page of the relation:
  - it retypes the `sys.objects` row to `kTypeDroppedTable` in place, after
    its before-image goes to the undo hook (`:1964`);
  - it sweeps `sys.tables` (`:2069`), `sys.columns`, `sys.indexes`,
    child-side `sys.fkeys` and `sys.cabins`;
  - it bumps the schema word once (`:2104`).
- **Every production drop delete-marks, autocommit included.**
  - `BeginWrite` opens an implicit transaction
    (`command_dispatcher.cpp:8235-8238`).
  - `DdlScopeFor` passes its id (`:5855-5859`).
  - `DropTable` retires outright only at `kBootstrapXid`
    (`catalog.cpp:1932`).
  - DT5 and `ddl-transactional.md` §5, §5a and §5d say autocommit retires.
    §7 agrees with the code (BF-Q12).
- **The roots live in one row, and that row goes soon after the commit.**
  - `sys.tables` carries the anchor page and the var-heap root, both fixed
    at `CREATE` (`rows.hpp:58-139`). The drop delete-marks the row.
  - The horizon-gated purge retires it on core 0 at the next DDL
    resolution, which is often the drop's own (`FinishDdlStatement`,
    `command_dispatcher.cpp:5912`; `EndDdlScopeById`, `:6012`; the purge
    at `:6057`).
  - Otherwise the mount retires it unconditionally
    (`FinalizeDeleteMarksAtMount`, `catalog.cpp:994`, called at
    `expeditor.cpp:913`).
  - Each retire is logged (`catalog.cpp:980`).
- **The mount retires marks before its completion checkpoint**
  (`expeditor.cpp:913` against `:1092`). So a pass placed after that
  checkpoint never sees a dropped relation's `sys.tables` row.
- **The tombstone is what is left.** It holds the oid, namespace, type,
  `rel_id` and name (`rows.hpp:32-48`).
  - It is never retired (DT2), and `HighestIssuedUserOid` counts it
    (`catalog.cpp:822-850`).
  - `rel_id` is written 0 for every table (`catalog.cpp:1065`).
  - No view lists a tombstone (`ObjectsView` lists `kTypeTable` rows only,
    `exec/catalog_view.cpp:80-84`), but three checks read it:
    `CheckNameFree` and `CheckNamespaceEmpty` read its type and its stamp
    (`catalog.cpp:1485-1512`, `:1553-1572`), and `HighestIssuedUserOid`
    reads its oid (`:822-850`). An undecided tombstone refuses a `CREATE`
    or `RENAME` of its name and a `DROP NAMESPACE` of its namespace.
- **A rollback restores everything the drop wrote.** The retype's
  before-image and the marks are compensated, and the relation's pages need
  nothing (`catalog.cpp:1952-1995`).
- **A burned oid is not tombstoned.** A failed or rolled-back
  `CREATE TABLE` leaves no row, so its oid can be issued again after a
  restart (`catalog.cpp:822-850`, `:1216-1222`). DT2's "never reissued"
  holds for tombstoned oids only.

### 1.2 What a relation owns, and what reaches it

- **At birth, three pages** (`catalog.cpp:1220-1312`), each logged with a
  `PAGE_INIT` that carries the new oid:
  - the clustered root, a btree leaf (or a heap page before SUS-1);
  - a var-heap root, only when the schema can spill;
  - the anchor (`kAnchor`), plus an `ANCHOR_UPDATE`.
- **The anchor is the authority for every root**
  (`anchor_page.hpp:9-42`, `:59-101`).
  - It holds the current clustered root, and `{index_oid, root}` for every
    index ever seeded, at most 679.
  - **No slot is ever removed.** So the anchor still names the trees of
    dropped indexes and, by one survey's reading, of rolled-back ones
    (Census D re-checks the second).
  - `sys.tables.desc_page_id` stays the create-time root, which becomes the
    leftmost leaf (`btree.cpp:565-591`, `:600-620`).
  - A root move writes the anchor alone (`catalog.cpp:2603-2642`).
- **Every tree page is reachable by descent.** A grown-over root stays in
  the tree as the new root's leftmost child (`btree.cpp:565-591`).
- **Index pages carry the index's own oid.** That oid is a `sys.indexes`
  row id from a separate sequence (`page.md` §2a, `:49`). Only btree
  relations have indexes (`catalog.cpp:3275-3283`).
- **Each relation has one var-heap chain.** It is rooted at
  `sys.tables.varheap_page_id` and grows by tail append, and no page ever
  leaves it (`varheap.hpp:38-56`, `:82-95`). No other relation points into
  it: an index stores the covered spill pointer of its own base relation
  (`index.md:306-309`).
- **A pre-SUS-1 heap relation** is a chain from `desc_page_id` through
  write-once links (`heap_chain.cpp:10-22`). No mountable volume holds one,
  and only a test seam creates one (BD §1.5).
- **Pages that are not the relation's:**
  - **Bound Cabin pages** belong to the assertion registry and are stamped
    owner 0 (`assertion_build.cpp:133-169`). DT3 refuses a drop while an
    assertion on the relation exists.
  - **Waystone pages** belong to the pattern. One trail page holds entries
    from every relation an execution touched (`waystone.hpp:120-178`).
  - **Cabin sets** are memory-resident, and `sys.cabins` holds no page id
    (`rows.hpp:743-785`).
  - **Catalog pages** are disjoint from user pages
    (`well_known.hpp:405-440`).
  - **Relayout** allocates nothing.
- **Pages left unlinked while the relation lives:**
  - a heap batch growth whose first row does not fit
    (`heap_chain.cpp:274-286`);
  - a btree split whose tuple fits no empty leaf (`btree.cpp:956-975`);
  - a var-heap growth whose value fits no empty page
    (`varheap.cpp:363-375`);
  - a failed `CREATE INDEX` after its tree was built
    (`index_ddl.cpp:385-398`).

  No walk reaches them.
- **The mount's audit opens three pages of every live user relation**
  (`mount_recovery.cpp:131-207`). Nothing walks a whole tree, and nothing
  reads `owner_oid` for reclamation.

### 1.3 The free map and the allocator

- **The map can only gain bits.**
  - It is region-based, with one bitmap page per 65,280 ids
    (`free_map.hpp:31-33`, `:97-128`).
  - The codec has `FreeMapAllocate` and no clear (`free_map.hpp:58-65`).
  - `PageStore` has no free, release or discard (`page_store.hpp:147-438`).
  - `page.md` §5 says *"Nothing frees a page"* (`:130`).
- **`ALLOC` (9) and `FREE` (15) are assigned and refused.**
  - Their codec exists, and only tests use it (`payload.cpp:496-516`).
  - Redo has no applier for either and answers `Corruption`
    (`redo.cpp:563-572`). So a `FREE` in any replay range refuses the mount
    today.
- **The map is the store's own, unlogged, and flushed with no WAL gate.**
  - Region pages are not frames and never enter the dirty table
    (`device_page_store.cpp:289-316`).
  - `FlushMaps` runs inside every `Flush`, `Sync` and checkpoint
    `FlushPages`, after the data pages (`:414-526`, `:1735`, `:1848`).
  - Every anchor publish syncs the store, which writes the map too
    (`superblock_checkpoint_anchor.cpp:132`).
  - That order is safe only because the map only gains bits: "a crash
    between them can only orphan a page" (`device_page_store.hpp:339-342`).
  - **A `Sync` does not wait for another thread's map write.** `FlushMaps`
    clears a region's dirty flag when it copies the region, and writes the
    copy after the hold (`device_page_store.cpp:438-457`). A `Sync` that
    runs between another core's copy and write finds the region clean and
    returns (`:1743-1745`; `page.md:138`).
  - `PersistMaps` writes the dirty map regions and syncs the device without
    the frames (`device_page_store.cpp:534-539`). Nothing in production
    calls it.
- **One cursor, and it only rises.**
  - `CreateNew` claims the lowest clear bit at or above `next_new_page_id_`
    and moves the cursor past it, in one hold
    (`device_page_store.cpp:958-993`).
  - Every `Open` starts the cursor at 128 (`superblock.hpp:43`;
    `expeditor.cpp:706`).
  - Recovery raises it to one past the highest page id the replay range
    names (`high_water.cpp:7-48`). `RaiseAllocationFloor` never lowers it
    (`device_page_store.cpp:1190-1209`).
  - **The floor is a guard, not a formality.** In `page_store.hpp:330-333`'s
    words, the map "can revert while the log still names pages above it,
    and an allocation afterwards would hand out a page redo has already
    written". A clear bit below the floor is not proof that an id is free.
  - So a bit cleared below the cursor is not found again in that run.
    After a clean restart, whose replay range names no page, the cursor
    starts at 128 and does find it.
- **One reuse already exists, and it bypasses the map.**
  - The undo log keeps settled pages on an in-memory `recycle_` list.
  - It logs and stamps a `PAGE_INIT` before it wipes each one, and never
    touches the page's bit.
  - A crash forgets the list, and that run's undo pages leak (UP4;
    `undo_log.cpp:72-130`; `txn.md` §4.1).
- **The file never shrinks.**
  - It grows by 64-page extents through `posix_fallocate`
    (`file_page_device.cpp:189-237`).
  - `PageDevice` has no shrink (`page_device.hpp:87-129`), and
    `page_capacity_` only rises.
  - `page.md` §14 makes shrinking a non-goal (`:219`).
- **`allocated_pages_` only rises** (`device_page_store.cpp:556-564`). It is
  printed at mount and at shutdown, not by `SHOW META`.
- **The superblock is version 20** (`superblock.hpp:246-256`), and it holds
  no allocation state (`:14-21`).
  - Its version list records three no-bump precedents, where a word written
    0 gained a meaning that 0 already stated (`:172-210`).
  - Since 2026-10-07 the standing order is that a change to an on-disk
    meaning bumps the version and refuses every older volume, with no
    migration (`raft-marks-2026-10-07.md` §11).

### 1.4 Crash safety: redo, the anchor and the warm-up

- **Redo loads a page before it gates on `page_lsn`.**
  1. A record is skipped before the load only when analysis holds no dirty
     entry for its page, or the record is below that entry's recLSN
     (`redo.cpp:393-397`).
  2. Analysis enters every page that any record in the range names, every
     page of a `BTREE_SPLIT`, and every `CHECKPOINT_BEGIN` dirty entry
     (`analysis.cpp:44-47`, `:138-165`).
  3. Then `store.Get` runs. A clear bit answers `NotFound` before any frame
     or byte is looked at (`device_page_store.cpp:1219-1232`).
  4. On `NotFound`, a `PAGE_INIT` or a whole-page image creates the page
     through `CreateAt`. **Any other record refuses the mount**
     (`redo.cpp:434-453`).
  5. Only then the gate: skip when `page_lsn >= LSN` (`redo.cpp:476-479`).
- **So a cleared bit on disk refuses the next mount** when the dead
  relation's records are still in its replay range. Where the record is an
  old image instead, it re-claims the page, which is a leak. And if the page
  was reused, an old record that passes a new owner's `page_lsn` 0 writes
  into it.
- **New pages reach disk at `page_lsn` 0 on three paths:**
  - the unstamped root and var-heap root `PAGE_INIT`s
    (`catalog.cpp:1259-1287`);
  - `CreateNew`'s zeroed dirty frame (`device_page_store.cpp:1179-1187`);
  - a backfilled index page before its image (`index_ddl.cpp:64-96`).
- **"Never written" is all-zero device bytes**
  (`page.md` §10, `:193`; `device_page_store.cpp:733-779`).
  - A freed page keeps its dead image on the device.
  - If a reused id's first record were not a whole page, it would replay
    onto that image. Today the same path refuses the mount instead.
  - One comment says DDL creates pages without a `PAGE_INIT`
    (`redo.cpp:443-447`), and `LogCatPageInit` says otherwise
    (`catalog.cpp:311-325`). No census has settled it (Census A).
- **`D`, the durable redo start** (`superblock_checkpoint_anchor.cpp:136-161`):
  - it is the redo start of an anchor whose sync returned OK;
  - it never decreases, because every publish encodes at least the highest
    anchor encoded before it (`:86-99`);
  - redo never starts below the scan start (BC-R2);
  - recycling removes the segments below it (`hpp:95-108`);
  - it is held in memory (`superblock_checkpoint_anchor.hpp:201`), starts at
    0 on every mount, and first moves at the completion checkpoint's
    publish. "Never decreases" is a statement about one process, and a
    reclaim that reads `D` before that publish sees 0, the safe direction.

  So once `D` passes an LSN, no later mount replays a record at or below
  that LSN.
- **The completion checkpoint moves `D` past the replayed log only at
  `cores = 1`.**
  - **At `cores = 1`.** Redo writes `page_lsn` directly (`redo.cpp:496`)
    and leaves the frame's recLSN unset, because only `StampPageLsn` sets it
    (`device_page_store.cpp:1286`). `RedoStartFrom` skips an unset recLSN
    (`checkpointer.cpp:183`). So the completion checkpoint
    (`expeditor.cpp:1065-1100`) publishes a redo start past the mount's
    scan end.
  - **At `cores > 1`, the warm-up.** Until every core has published in
    this process, each publish rewrites the mount anchor unchanged
    (`superblock_checkpoint_anchor.cpp:44`; `hpp:63-83`). That waits for
    each core's first cadence checkpoint: 5 s by default
    (`expeditor.hpp:189`), and, with `checkpoint_interval_ms = 0`, not
    until a clean shutdown's per-core checkpoints (`expeditor.cpp:2056-2073`).
- **A drop's commit can precede its durability.**
  - Under group durability, `X` is released and the commit published
    before `TXN_COMMIT` is on the device (`txn.md` §4.1, `:417-421`).
  - `HandleDropTable` does not call `AwaitDdlDurability`
    (`command_dispatcher.cpp:4245`).
  - A crash in that window makes the drop a loser, and mount undo restores
    the relation.
- **The sim cannot see any of this yet.**
  - Its workload has no `DROP TABLE` (`sim/workload.hpp:60-79`).
  - Its checkpoint builds a fresh anchor object each time, so `D` starts
    over (`sim/instance.cpp:143-149`).
  - Its only allocation check reads each allocated page's header
    (`sim/integrity.cpp:96-113`).
  - `DROP TABLE` crash coverage is two hand-written cells
    (`tests/sim_loop_test.cpp:490`, `:589`).

### 1.5 The buffer pool's frames

- **There are three erasers, and none removes a dirty frame:**
  - `ReleaseScanSlot` (`device_page_store.cpp:799-829`);
  - `EvictClean` (`:1782-1829`), which refuses a dirty, pinned or latched
    frame (`:1796-1800`);
  - the CLOCK sweep (`:2546-2628`), which queues a dirty victim
    (`:2602-2608`).
- **`EvictClean` has no production caller.** Only tests call it
  (`eviction_test.cpp`, `page_latch_test.cpp`,
  `am_s2_pin_protocol_test.cpp`), and its doc block names a retired purpose
  (`device_page_store.hpp:541-556`).
- **`WriteBack` rests on "no eraser removes a dirty frame".** It holds raw
  `Frame*` pointers across its copy, gate, write and clean
  (`device_page_store.cpp:1429-1433`, `:1652-1665`).
- **A writeback claims each frame from its copy to its clean**
  (`Frame::writing`, AT-S10e; `device_page_store.cpp:1521-1525`). It
  unpins after the copy (`:1590`), so between the copy and the clean a
  frame is held by its claim alone, with no pin and no latch.
- **A free would falsify four premises:**
  - **the hit path:** "free-map bits are never cleared", so no resident
    page reaches `IsAllocated`'s false arm (`:1946-1954`);
  - **`CreateNew`:** the claimed bit "was clear one instruction ago", so it
    makes no in-use test (`:1172-1177`);
  - **`InsertFrame`'s lost-race arm:** it keeps an existing frame's bytes,
    and a create accepts them (`:620-628`, `:2140-2150`);
  - **the headerless bit:** `IsHeaderless` answers the headerless bit and
    the allocated bit together (`:343-368`), and `any_headerless_` never
    resets (`device_page_store.hpp:1481-1486`).
- **A miss publishes its id to `loading_` before it reads**
  (`device_page_store.cpp:2022-2035`). So a fault in flight is visible to
  anything holding the structure latch.
- **Pinned classes** are ids below 128, map ids, and any frame whose header
  says `kCabinBound`, `kFreeMap` or `kHeaderlessMap` (`:2475-2505`).
  `DropAssertion` touches no page (`assertion_catalog.cpp:720-737`), so a
  dropped assertion's Bound Cabin frames stay resident until restart.
- **A dropped relation's frames today.**
  - Clean frames stay resident while `buffer_pool_frames` is 0
    (`expeditor.hpp:130`).
  - Dirty frames stay in the checkpoint's dirty table and are written back
    (`checkpointer.cpp:155`), and they hold the redo start until they are.

### 1.6 Who can still reach a dropped relation's pages

- **A refused read borrow reads on.** The read borrow is a non-queueing
  `TryAcquire` taken at bind. A refused ask leaves the reader holding
  nothing, and it keeps walking. Its stated soundness rests first on DT1's
  allocated pages (`read_borrow.hpp:27-36`; `txn.md` §5, `:793-806`; DT7).
- **A stale memo can bind a dropped relation.**
  - A core revalidates its memo only at a statement's head
    (`command_dispatcher.cpp:780`) and at the Cabin optimizer's tick
    (`cabin_optimizer_exec.cpp:45`).
  - Suppose a statement's head ran before the drop's bump. It can bind the
    relation after the drop has committed and released `X`, from the
    unfiltered cache (`catalog.cpp:1402-1405`, `:2197-2200`), with its `IS`
    granted, and walk it.
  - Its snapshot can postdate the drop's commit
    (`command_dispatcher.cpp:6623`, `:6738-6743`).
- **Writers are fenced.** The first relation `IX` re-checks `MemoIsCurrent`
  and refuses `TxnConflict` (`command_dispatcher.cpp:8445-8483`).
- **Four walkers take no borrow:**
  - the foreign-key reverse check walks each child whole
    (`command_dispatcher.cpp:3574-3622`; `fk_check.cpp:405-411`);
  - the Cabin optimizer's build and heal walk a relation with an
    unregistered check view and no lock (`cabin_optimizer_exec.cpp:41-48`,
    `:128-140`, `:352-433`);
  - `SHOW RELAYOUT`'s survey walks a heap chain
    (`relayout_planner.cpp:238-276`);
  - `DESCRIBE` reads `sys.tables` unfiltered and the anchor, then walks the
    whole clustered tree for `height=`/`leaves=`
    (`command_dispatcher.cpp:2029-2127`; `BtreeHeight`/`BtreeLeafCount` at
    `:2123-2124`), from a name the core's cache can still hold
    (`catalog.cpp:1402-1405`). It never binds the relation.
- **The read horizon covers none of these.**
  - `ReadHorizon()` folds live transactions and leased readers
    (`manager.cpp:683-700`, `:771-826`).
  - Check views are exempt (`txn.md` §4.1, `:494-498`).
  - §5d's argument that the sweeping core is "between resolutions" covers
    that one core only (`ddl-transactional.md` §5d).
  - A stale-memo bind mints its snapshot after the drop, so the horizon
    passes the drop while that reader walks.
  - One abandoned `BEGIN` holds the horizon for the life of the process
    (`known-gaps.md`, the lifetime-ceiling entry).
- **Anyone who resolves afresh is already safe.**
  - After the catalog write, a name lookup answers `NotFound` under every
    view (`catalog.cpp:1411-1416`). So a `REPEATABLE READ` transaction that
    predates the drop cannot reach the relation at its next statement.
  - Old row versions, and older var-heap values, are reached only through
    the relation's own tuples (`txn.md` §2, `:131-134`).
- **Whether any statement holds pages across a boundary is disputed**
  (§1.9 item 2). A resumed statement re-binds through `InitTableAccess`
  (`step_vm.cpp:299-309`), which answers from the core's memo.

### 1.7 What checks a remembered location

- **One verifier makes three checks:** the page fetches, its epoch matches,
  and the Keystone id at the slot is the expected pk
  (`tuple_verify.cpp:11-49`). It checks no page type and no owner.
- **Its own header names the gap.** The relation check "is sufficient only
  while pages are never freed and reallocated between relations - i.e.
  until `DROP TABLE` or page reuse exists" (`tuple_verify.hpp:56-60`).
- **Its callers:**
  - Waystone replay (`step_vm.cpp:661`);
  - the probe memo (`:572`), whose comment at `:2470-2481` records the
    outcome against the wrong relation: `Corruption` at differing row
    widths, and another table's row at equal widths;
  - the Cabin serve's two phases (`:1546`, `:1640`);
  - the inner-build probe's btree arm (`:1181`);
  - the reverse foreign-key Cabin path (`fk_check.cpp:217-224`);
  - the Cabin optimizer's heal (`cabin_optimizer_exec.cpp:380-390`).

  The heap inner-build bucket reads `(page, slot)` with no verification
  (`step_vm.cpp:1199-1206`).
- **The relation check is against the query.** `TrailReplay::Build` drops
  an entry whose `rel_oid` is not the step's (`trail_replay.cpp:53-57`). A
  tombstoned oid is never reissued, so no statement compiled against
  another relation consults a dead relation's trails.
- **Keystone ids collide across relations by design.** A new relation's
  ids start low, so a slot on a reused page usually carries a pk that some
  stale entry expects (invariant 11).
- **The epoch does not help.** `FormatPage` writes `relayout_epoch` 0
  (`page_header.cpp:67-80`), and most locations are recorded at 0.
- **`ReadTuple` bounds the slot index and nothing else.** It checks neither the page's type nor
  that the slot's offset and length fit the page (`heap_page.cpp:231-253`).
  A stale location on a reused var-heap or index page can read outside the
  frame, or read user varchar bytes as a tuple whose first 8 bytes forge
  the Keystone id.
- **Descents check type, not owner.** `BtreeLookup` applies `RequireType`
  at each level (`btree.cpp:212-216`).
- **`owner_oid` is stamped on every relation page, and read in two places:**
  - the anchor's identity check, which refuses `Corruption`
    (`catalog.cpp:1090`, `:1112`, `:2265`);
  - the btree rebuilds that carry the stamp across (`btree.cpp:341`,
    `:745`).

  Bound Cabin, Waystone, undo and map pages carry 0, and directory
  interiors have no header (§1.2).
- **No mountable volume holds an unstamped user page.** Owner stamping
  landed at `1e219a7a` (2026-08-13), and version 20 refuses every older
  volume (`superblock.cpp:69-83`).

### 1.8 The texts that rest on "nothing frees a page" or on DT1

**Code:**

- `page_store.hpp:335-339` and `high_water.hpp:51-56`: the floor's cost;
- `device_page_store.hpp:339-342`: data before the map;
- `device_page_store.cpp:1172-1177` (`CreateNew`) and `:1946-1954` (the hit
  path);
- `device_page_store.hpp:1481-1486`: `any_headerless_`;
- `read_borrow.hpp:27-36`, `tuple_verify.hpp:56-60` and
  `trail_replay.hpp:56-61`;
- `free_map.hpp:20-22` and `redo.cpp:566-569`, which call `ALLOC`/`FREE`
  unbuilt work and cite a path that resolves only at `1769487`;
- `record.hpp:58` and `:64`, and `file_page_device.cpp:217-218`, which
  describe an `ALLOC` record nothing writes;
- `mount_recovery.hpp:237-239`, which says page-to-relation resolution has
  no index, where `page.md` §2a's `owner_oid` is one;
- `command_dispatcher.cpp:2660` and `:2696-2698`;
- `varheap.cpp:371-374`, `btree.cpp:972-975`, `heap_chain.cpp:130` and every
  other comment that says the store "has no free-page path yet";
- `rows.hpp:32-48`, whose `SysObjectRow` names the roots' word `rel_id`
  (BF-R2).

**Specs and the rest:**

- `drop-table.md`: the preamble (`:3-4`, "reclaims no pages"); DT1; DT2's
  last argument ("with the dead relation's pages still holding their
  bytes"); DT5; DT7;
- `page.md`: §2a (`:55`, `:63`, `:65`), §5 (`:130`), §7 (`:167`), §10
  (`:193`) and §14 (`:218-219`);
- `eviction.md` §3's reclaim rules (`:105-107`);
- `index.md` (`:637-639`), "frees no page" at `DROP INDEX`;
- `waystone-concpets.md` §0's status (`:7`);
- `heap-and-tuple.md` §8 invariants 2 and 3, and `CLAUDE.md`'s copy;
- `manual/sql/sql.md` (`:27-28`, `:266-269`, `:992-995`) and
  `manual/physical-optimizer/physical-optimizer.md` (`:139`, gate 3);
- `physical-optimizer.md`: §4 (`:154-156`) and §6 gate 3 (`:203-209`);
- `txn.md`: §4.1 (the two purges) and §5 (`:793-806`);
- `catalog.md`: CT1 (`:24`) and CT3 (`:80-85`);
- `ddl-transactional.md`: §5, §5a and §5d;
- `wal.md` §5.2 (`:107`);
- `known-gaps.md`: the refused catalog report's page (`:296-299`) and the
  relation `IS` entry (`:796`);
- `CLAUDE.md`: its DROP TABLE row, the free-map row, the WAL row's "except
  ALLOC/FREE", the Buffer-pool eviction row, and the three superblock
  mentions (the B+ tree, Assertions and Ranges rows), which say 20;
- BA-R5 (`workorder-ba-parallelism.md:817-818`).

### 1.9 Where the surveys disagreed, and what the tree says

1. **Is the mount's completion checkpoint the point after which no earlier
   record can be replayed?** One survey said yes. Another said the warm-up
   holds the anchor. **Settled by reading:** yes at `cores = 1`, and no at
   `cores > 1` until every core has published once (§1.4). BF-R4 is
   written for both cases, and BF-S1's premise cell confirms it on the file
   rig.
2. **Does any statement hold a relation's pages across a task boundary?**
   - One survey says no. No read parks since AT-S10, a KWP portal runs
     whole into buffered batches, and a parked `UPDATE` or `DELETE`
     re-resolves by name on resume (`step_vm.hpp:318-321`;
     `protocol.md:105-109`; `command_dispatcher.cpp:7190-7206`).
   - Another says a resumed statement's re-bind reads the core's memo,
     which can be stale (`step_vm.cpp:299-309`).
   - **Not settled.** The question is whether a resume passes a
     `Revalidate` before its re-bind. Census B decides it, and it feeds
     BF-Q9.
3. **How an allocation finds a freed id.** The candidates are a free list,
   lowering the cursor, or splitting the floor from the cursor with logged
   `ALLOC`/`FREE`. `page_store.hpp:330-333` is why lowering is not
   proposed: a clear bit below the floor can be an id the log names
   (BF-Q6).
4. **What happens to a dirty frame.** One survey: evict only clean frames,
   and let the checkpoint write the dirty ones. Another: discard dirty
   frames too. **Reconciled:** once BF-R4's gate holds, no replay names the
   page, and its frame holds nothing anyone needs (BF-Q7).
5. **Walk or census** (BF-Q5).
6. **Where the roots wait.** The candidates are a reclaim inside the purge
   that retires the `sys.tables` mark, a durable per-drop record, or an
   in-memory queue with a census at mount. §1.1's mount order rules the
   first out at mount (BF-Q3).
7. **The predicate for a reclaim within the run.** Horizon, statement
   epoch, borrow, or owner validation (BF-Q9).
8. **Where the reclaim runs.** At the §5d site on DDL resolution, or on the
   system tick (BF-Q16).

### 1.10 Neighbouring orders and the test infrastructure

- **BE-R1** gives the frame table a slot array and names three erasers
  (`workorder-be-bounded-pool.md:301`). **BE-R4** refuses a fault at the
  cap with `ResourceExhausted`.
- **BA-R5** removes `IsAllocated` from the hit path "because nothing frees
  a page" (`workorder-ba-parallelism.md:817-818`).
- **Relayout Part I** quarantines a mover's retired pages
  (`physical-optimizer.md` §4, `:154-156`). Its planner holds no page ids,
  and `physical_optimizer=on` is refused naming gate 3
  (`relayout_planner.cpp:43-46`; `expeditor.cpp:400-410`).
- **The rigs:**
  - `TwoCoreRig` runs two cores over one `DevicePageStore`
    (`tests/two_core_rig.hpp:135-175`);
  - the file rig snapshots what a crash leaves and mounts it through
    `Expeditor::Open` (`tests/file_rig_crash.hpp:52-70`);
  - the store's seams include `LatchFrameForTest` and
    `SetAfterWritebackCopyForTest` (`device_page_store.hpp:688-710`);
  - no seam sits between a miss's `IsAllocated` and its insert, and none
    clears a free-map bit.
- **What a client can read.** `DESCRIBE` prints `root_page_id` and
  `leaves=` (`command_dispatcher.cpp:2090-2127`). `SHOW META` prints map
  regions and the undo-page counters, and no allocated or freed count
  (`:1308-1319`).
- **`PageStore` has four implementations and about seven test doubles.**
  `drop_table_test.cpp` runs over `InMemoryPageStore` (`:59`).

### 1.11 Texts the tree already contradicts

These were found on the way. BF-S6 corrects them whether or not BF changes
the behaviour they describe.

- **`wal.md` §5.2** (`:107`) calls free-map pages a logged page class. They
  are store regions, flushed with no record (§1.3).
- **`page.md` §1** (`:22`) says free-map pages carry `page_lsn` and take
  part in recovery. Theirs is always 0.
- **`page.md` §2a:**
  - it says "No consumer reads the field today" (`:65`), against the anchor
    check (§1.7);
  - it says every page-creating entry point takes the owner non-defaulted
    (`:48`), against the default of 0 on `FormatPage` and on the var-heap's
    `FormatPage` (`page_header.hpp:118-119`; `varheap.hpp:218`).
- **`page.md` §10** (`:193`) cites a peer's extent lease, struck at AW-S1b.
- **`physical-optimizer.md` §6 gate 3**'s "a page written before §2a reads
  owner 0 permanently" describes no mountable volume (§1.7).
- **`record.hpp:125-127`** says a dropped assertion's pages return "through
  ordinary FREE records". Nothing emits one.
- **`device_page_store.hpp:66-79`** says "No eviction" and one free-map
  page, and **`:733-737`** says nothing calls `EvictColdFrames`.
- **`superblock.hpp:23-28`** says one thread owns the superblock. `page.md`
  §6 and the anchor's latch say every core writes it.
- **`rows.hpp:402-405`** says a root split publishes through `SysIndexRow`.
  The split writes the anchor slot (`catalog.cpp:3525-3576`).
- **DT1** omits the anchor and the internal nodes, and it lists Bound Cabin
  pages, which DT3 rules out.
- **DT7** says a read that starts after the grant "takes no borrow at all".
  A fresh resolution answers `NotFound` (`catalog.cpp:1411-1416`), and the
  readers that read on are the ones in §1.6.
- **`catalog.md` CT1** says index and assertion builds take no relation
  lock. Since AT-S5e they take `X`.
- **`command_dispatcher.cpp:2660`** cites "DT8". The spec ends at DT7.
- **`command_dispatcher.cpp:6084-6087`** and **`catalog.cpp:1928-1931`**
  describe the retire arm as autocommit's (BF-Q12).
- **`page.md` §2a** (`:55`) calls the orphan test ABA-proof. That holds for
  tombstoned oids only (§1.1).
- **`wal.md` §10** (`:141`) says the first change after a checkpoint logs a
  full-page image. No such emitter exists, and Census A rests on what does.
- **`assertion.md` §8.3** (`:762-763`) says `DROP ASSERTION` unpins its
  pages. It touches no page (§1.5).
- **`tuple_verify.hpp:42-44`** says every epoch comparison is between two
  zeros. `btree.cpp:780` bumps the epoch at every leaf division; BF-R10
  edits that file and restates it.
- **Outside BF's scope:** the same stale epoch claim at
  `physical-optimizer.md` §7, `cabin_store.hpp:141-143` and
  `cabin_bound_page.hpp:133-135`, and the Bound Cabin writers' epoch-0
  stamp outside `tuple_verify.hpp:116-122`'s rule. None concerns reuse.

The eviction texts BE-S6 restates for slots and batches stay BE's,
`eviction.md` EV4 (`:60`) and §3 (`:82`) among them. BF-S6 restates, on top
of whatever BE-S6 left: `eviction.md` §3's reclaim rules (`:105-107`), to
add "a freed page's frame is discarded, dirty or not (BF-R5)"; `page.md` §7
`:167`, with the same exception; and `CLAUDE.md`'s Buffer-pool eviction
row, which gains the discard and `EvictClean`'s deletion.

## 2. Rulings — CLA's proposals, marked as proposed (W1)

### BF-R1 — Reclamation follows a durable commit and is never part of the drop

- **What.**
  - `HandleDropTable` and `Catalog::DropTable` stay catalog-only. Nothing
    is freed inside the drop's statement or its transaction.
  - A live `ROLLBACK`, a failed commit and a loser at mount free nothing.
    The retype's before-image clears what BF-R2 wrote, so nothing is left
    pending.
- **Why.** A free inside the transaction would let a compensation write
  into another relation's page, and the restored relation would point at
  freed pages (§1.4).
- **Hazard closed:** a drop rolled back over freed pages.

### BF-R2 — The tombstone carries what the drop owes (BF-Q3, BF-Q4)

- **What the drop writes.**
  - The drop's retype also writes the relation's anchor page and var-heap
    root into the tombstone's `rel_id`, packed by explicit shift and mask.
    `rel_id` is written 0 for every table today (§1.1).
  - `kInvalidPageId` (no var-heap) is carried as itself. A word of 0 means
    nothing is owed.
  - It is the same in-place overwrite under the same undo record, so a
    rollback and mount undo restore 0 along with the rest of the row.
  - The field is renamed in code to `pending_roots`, with explicit shift
    and mask accessors, and its on-disk offset is unchanged. `rows.hpp`'s
    `SysObjectRow` comment states both meanings: 0 on a live row, and the
    packed roots on a pending tombstone.
- **Pending and done.**
  - A tombstone whose word is nonzero is pending.
  - When BF-R7's last step is durable, the reclaim overwrites the word to
    0. That write is logged outside any transaction, as the mark retire is
    (`catalog.cpp:980`).
  - The clearing overwrite keeps the row's `trx_id` and `undo_ptr`
    (`OverwriteLogged` with `kNoTxnId` as the envelope and the row's own
    stamp, as `AllocateRowIdRange` does at `catalog.cpp:2447`). So the
    decided-tombstone reading of `CheckNameFree` and `CheckNamespaceEmpty`
    (§1.1) does not move.
  - It moves no schema word, because no name lookup resolves a tombstone.
- **The format.** This is a change to an on-disk meaning. By the standing
  order, the superblock moves 20 → 21 and version 20 is refused (BF-Q4).
  So no mountable volume carries a tombstone with no roots.
- **Why this carrier.** The `sys.tables` row is retired by the purge, often
  at the drop's own resolution, and by the mount before its completion
  checkpoint (§1.1), so it cannot wait for a reclaim. The tombstone, by
  contrast:
  - is kept forever by DT2;
  - is written by the drop and undone with it;
  - is resolved by no name lookup.
- **Cost:** one word's meaning, and a refused version 20.
- **Hazard closed:** roots lost between the commit and the reclaim, to the
  purge or to a crash.

### BF-R3 — A walk from the roots, checked page by page (BF-Q5, BF-Q8)

- **What the walk reaches:**
  - the anchor;
  - the clustered tree, by descent from the anchor's clustered root, or a
    pre-SUS-1 heap chain by its links (BF-Q8 (a') would skip the heap
    chain);
  - every tree an anchor slot names, whether current, dropped or rolled
    back;
  - the var-heap chain, from its root.
- **Every page is checked** for the class its parent says it is and for its
  owner:
  - the relation's oid, for clustered, heap, var-heap and anchor pages;
  - the slot's index oid, for index pages, with the class discriminating as
    `page.md` §2a requires.
- **A page that fails the check** is not freed and not descended through.
  It is counted and logged.
- **No page is freed twice.** Pages are collected into a visited set, so a
  cycle, or a page reached twice, ends the walk with a refusal, never a
  second free.
- **This walk never frees** ids below 128, map ids, headerless ids or
  `kCabinBound` pages. The primitive refuses them too (BF-R5).
- **Reads** release as they go, through BE-R5's cold `kScan` where it is
  built. A `ResourceExhausted` under BE-R4 defers the reclaim and never
  fails a statement.
- **Cost:** one header read per page.
- **Hazard closed:** freeing a page that is not the relation's, including
  when a re-drive meets a reused page (BF-R7).

### BF-R4 — The replay gate: `D` past the drop (BF-Q2)

- **The rule.** A page is freed only once no later mount can replay a
  record that names it:
  - **for a drop committed in this run**, once `D` exceeds the drop's
    `TXN_COMMIT` LSN;
  - **for a tombstone a mount found pending**, once `D` exceeds the LSN at
    which that mount's recovery scan ended.
- **Why that is enough:**
  - every record that names the relation's pages precedes its commit,
    because the drop's `X` waited out every writer and a stale writer is
    fenced at its `IX` (§1.6);
  - `D` never decreases, and redo never starts below the scan start (§1.4);
  - `D` is at most the durable LSN, so `D` past the commit also proves the
    commit durable.
- **No WAL record is added.** `FREE` stays reserved and refused at redo.
  The map stays unlogged, and a cleared bit may reach disk with whichever
  `Flush` comes next.
- **When it passes:**
  - for a mount-found tombstone at `cores = 1`, at the mount's completion
    checkpoint;
  - for a drop within the run, about two checkpoint intervals after its
    commit;
  - at `cores > 1`, not before every core's first checkpoint (§1.4). That
    is about 5 s by default, and with `checkpoint_interval_ms = 0` not
    until a clean shutdown, which is stated as a cost and not forced.
  - A reclaim that reads `D` before the mount's completion checkpoint has
    published reads 0, and frees nothing (§1.4).
- **Not proposed:** freeing at mount every page that the mount's replay
  range does not name (analysis's dirty table, `wal/recovery.hpp:121`).
  It would lift the `cores > 1` delay for older drops, but it is a second
  gate to prove, for a delay the 5 s default already bounds.
- **Hazards closed:**
  - a refused mount;
  - a leak from an old image re-creating the page;
  - an old record replayed onto a new owner's page (§1.4);
  - a free that ran ahead of durability.

### BF-R5 — One free primitive (BF-Q7)

- **The seam.** `PageStore` gains one virtual. Its default refuses
  `NotImplemented`, so a store that cannot free reports a leak rather than
  a free that never happened. Not `Unsupported`, though
  `RaiseAllocationFloor`'s default is (`page_store.hpp:341-351`): by
  `status.hpp:43-45`'s test, a test-double store that cannot free is one a
  later release could teach to, with no change to the architecture.
- **`DevicePageStore`'s free, in one hold, frames latch then map latch**
  (the declared order, `page.md` §5):
  1. Find the frame. If it is pinned, latched or claimed by a writeback, or
     if the id is in `loading_`, answer *deferred* and change nothing.
  2. Otherwise erase the frame, dirty or not. Debug builds poison it, as
     the sweep does (`device_page_store.cpp:2610-2616`).
  3. Clear the bit, decrement `allocated_pages_`, and mark the region
     dirty.
  4. Push the id onto BF-R6's list.
- **Refused `Corruption`:** an id below 128, a map id, a headerless id and
  a `kCabinBound` frame. A walk that reached one has read a page that is
  not the relation's. An id that is already free answers `NotFound`, which
  the re-drive reads as done.
- **The map write barrier.** `FlushMaps` is serialised across its copy and
  its write by a flush mutex that is not the map latch, or each region
  carries a write claim that a later `FlushMaps` waits on, in
  `Frame::writing`'s shape. A `Sync` or `PersistMaps` that returns has then
  written and synced every region state copied before it began. Today it
  need not have (§1.3), and BF-R7's order rests on it.
- **Why discarding a dirty frame is sound here.** BF-R4 runs first:
  - no replay names the page, so its recLSN guards nothing;
  - no reader reaches it (BF-R8, BF-R9);
  - its bytes are dead.

  Writing them back would cost I/O for nothing, and could land them over a
  reuse.
- **`EvictClean` is deleted.** It has no production caller and a retired
  purpose (§1.5). Its test callers move to the new primitive or to the
  sweep.
- **The invariant this makes true, replacing "bits are never cleared":** no
  clear bit has a resident frame, and no id is handed out while a fault on
  it is in flight (BF-R6's pop).
  - The hit path, `CreateNew` and BA-R5 are restated to it.
  - `InsertFrame`'s lost-race arm refuses a create with `Corruption`, as
    defence.
- **Not proposed:** writing zeros over a freed page on the device, which
  would restore the "never written" signature at one write per page.
  Census A is cheaper if it finds every logged creation path whole, and
  BF-R6 keeps the unlogged headerless class off reused ids. If Census A
  finds an unlogged path BF-R6 does not cover, the zero-write, durable
  before the id is pushed onto the list, is the fallback.
- **Hazards closed:**
  - a reused id serving a dead frame's bytes;
  - a writeback dereferencing an erased frame;
  - a reused page read as headerless;
  - a map sync that returns before another core's copied map state is on
    the device.

### BF-R6 — A free list ahead of the cursor (BF-Q6)

- **What.** `DevicePageStore` keeps the ids BF-R5 freed, under the map
  latch.
  - `CreateNew` pops from it before the cursor scan, re-testing the bit.
  - A pop runs under the frame-table latch then the map latch, the declared
    order, and skips (re-queues) an id that is in `loading_` or resident.
  - `CreateNewHeaderless` never pops the free list. It claims from the
    cursor only, because a headerless page is unlogged and has no checksum,
    and after a crash a reused id reads as the dead image, never as the
    all-zero `NotFound` signature (`page.md` §10). A Waystone directory
    interior on a reused id would follow the dead page's bytes as child ids
    into live pages, and the trail writer formats its target unconditionally
    (`waystone_dir.cpp:37-46`, `:70-115`; `trail_store.cpp:107-121`).
  - `CreateAt` takes an id off the list if it is there.
- **The cursor and the recovery floor do not move.** A clear bit below the
  floor can be an id the log names (`page_store.hpp:330-333`), while the
  list holds only ids BF-R3 checked and BF-R4 gated.
- **A crash forgets the list.** After a clean restart the cursor starts at
  128 and finds the cleared bits. After a crash, the floor hides those
  below it for that run. This is stated, as the undo log states its own
  list's leak.
- **Cost:** one pop on every page creation.
- **Hazards closed:** freed space that never comes back within a run; a
  stale miss winning a reused id from its new owner; an unlogged
  directory page reading a dead image after a crash.

### BF-R7 — Free order, and the re-drive

- **The map sync is `PersistMaps`** (§1.3), behind BF-R5's write barrier,
  so a sync that returns covers every bit cleared before it began. A bare
  `Sync` without the barrier does not (§1.3).
- **Order, for a tree.** Leaves first, then the internal nodes one level at
  a time, with a map sync between levels. So a parent's cleared bit never
  reaches disk before its children's, and a re-drive can always descend to
  whatever is left.
- **Order, for a chain** (the var-heap chain, or a pre-SUS-1 heap chain):
  BF-Q18's. Under CLA's proposal, (c), tail first with one map sync per
  page, so a crash leaves a prefix the re-drive still reaches.
- **Then the anchor, a map sync, and only then the tombstone's word
  cleared.**
- **The re-drive.** Every mount collects every pending tombstone, in one
  scan of `sys.objects`, and drives it again. BF-R3's check makes this
  idempotent:
  - a page that is already free answers `NotFound` and is skipped;
  - a page another relation has reused fails the owner check, and is
    neither freed nor descended through.
- **The chain's cut.** A chain's links run through its own pages. Freed
  head-first in one pass, a crash inside the flush can land an earlier
  page's cleared bit without a later page's, and the later pages are then
  out of reach. A re-drive that reads a cleared page's device bytes,
  owner-checked, to follow its link would close the gap, and so would a
  tail-first order. BF-Q18 decides which is built.
- **Hazard closed:** a reclaim cut short by a crash, which would otherwise
  leak for good or free twice.

### BF-R8 — Reclaim at mount, the first deliverable (BF-Q1)

- **Collected at mount**, after the completion checkpoint
  (`expeditor.cpp:1092`) and before the listener binds.
- **Executed on core 0's system tick**, in bounded batches (BF-Q16). So a
  mount does not grow by the size of what was dropped. A batch frees at
  most `kReclaimBatchPages` (proposed 64) pages, a named `constexpr`, and
  issues one map sync per tree level within it, or per chain page under
  BF-Q18 (c) (BF-Q13).
- **No reader gate is needed** (BF-Q17, against DT1's horizon clause). No
  statement of the run can reach a mount-found tombstone's pages:
  - the name resolves `NotFound`, `DESCRIBE`'s included;
  - every memo and name cache is built after the mount;
  - Cabin sets are memory-resident, and the relation's `sys.cabins` rows
    are gone;
  - a trail entry for the dead oid is dropped at `TrailReplay::Build`;
  - Bound Cabin hints are never read (§1.7).

  Census B confirms there is no other path.
- **Drops committed during a run are reclaimed at the next mount**, until
  BF-R9 is built.
- **Hazard closed:** the leak itself, with no new concurrency argument.

### BF-R9 — Reclaim within the run (BF-Q1, BF-Q9) `[quiet-wrong]`

- **The queue.** The drop's commit arm enqueues the tombstone's oid, its
  roots, the commit LSN, and the schema word after the commit's move. The
  commit arm is `FinishDdlStatement` after `EndWrite`, and `EndDdlScope` on
  `COMMIT`.
  - There is one queue for the instance, executed on the tick.
  - A crash loses the queue, and the next mount re-drives the tombstone
    (BF-R7).
- **The predicate, CLA's proposal: a statement epoch.**
  - Every core publishes the oldest schema-word value among its statements
    in flight. A statement counts from the `Revalidate` at its head to its
    reply, parked or not. An idle core publishes "none".
  - The Cabin optimizer's tick publishes its tick-start value the same way.
  - **Publish before read.** A statement, and the Cabin tick, first stores
    `kEntering` (which blocks every reclaim) in its core's slot, issues a
    `seq_cst` fence, then reads the word `V` in `Revalidate` and stores `V`.
    Read first and published after, a statement could read the word before
    the drop's move, find its slot read as idle by the reclaimer, and bind
    from its stale memo after the free.
  - The reclaimer reads every slot after a `seq_cst` fence that follows the
    commit's move. A drop's pages are freed once no slot holds `kEntering`
    or a value below the word the drop's commit moved to, and BF-R4 holds.
- **What it covers** (§1.6), checked against Census B's list:
  - a refused reader;
  - a stale-memo bind;
  - a parked statement, however it re-binds;
  - the reverse foreign-key walk, which runs inside a statement;
  - `SHOW RELAYOUT`, which is a statement;
  - `DESCRIBE`, a statement that never binds;
  - the Cabin optimizer's tick.
- **Why not the horizon** (§1.6): a stale-memo bind's snapshot postdates
  the drop, check views are unregistered, and one idle `BEGIN` would stop
  every reclaim for the life of the process.
- **Also in this stage:** BF-Q11's fix. BF-R10 is BF-S3's, since reuse
  begins there.
- **Cost:** two stores and a `seq_cst` fence at each statement's start and
  one store at its end, on every core, measured at the close.
- **Hazard closed:** another relation's rows served as the dropped
  relation's, or a spurious `Corruption`.

### BF-R10 — Defence in depth on every remembered location (BF-Q10)

- **The verifier.** `VerifyTupleAt` takes the expected relation oid, which
  every caller holds. It answers a miss unless the page is `kHeap` or
  `kBtreeLeaf` and carries that owner. The heap inner-build bucket goes
  through it.
- **The walks.** A descent, a chain walk and a var-heap fetch check the
  root's owner, or the page's, once per step.
- **`ReadTuple`** refuses a slot whose offset and length leave the page,
  with `Corruption`.
- **None of these is cited as the authority.** BF-R4, and BF-R8 or BF-R9,
  are. These checks turn a defect in a gate into a miss or a refusal
  instead of a wrong row.
- **Cost:** header bytes already in hand, and a branch.
- **Hazards closed:** the read outside the frame, and gate 3's collision
  for a dropped relation.

### BF-R11 — Counters, and no key (BF-Q13)

- `SHOW META` gains six counters:
  - `pages_allocated` (`allocated_pages_`);
  - `pages_freed`;
  - `pages_reused`, the creations served from the list;
  - `reclaim_pending`, the tombstones still owed;
  - `reclaim_deferred`, the frees a busy frame deferred;
  - `reclaim_skipped`, the pages BF-R3's check refused.
- They sit beside `map_regions` and `undo_pages_recycled`. A plateau plus a
  reuse count is the existing precedent for proving reuse.
- Nothing is configurable, because the gate and the cadence follow from
  what already exists. The batch bound is a constant, `kReclaimBatchPages`
  (BF-R8).

### BF-R12 — Checked, not only argued

- **The sim:**
  - a `kDropTable` op, both inside and outside a transaction, committed and
    rolled back;
  - the oracle forgets a relation at its committed drop, and keeps it at a
    rollback or a loser;
  - one anchor object per booted instance, so `D` behaves as it does in
    production (`sim/instance.cpp:143-149`);
  - an owner census after quiesce: no page reached from the tombstone's
    roots, as the sim's ledger recorded them at the drop's commit, is still
    allocated once its reclaim completes; every other allocated page
    carrying a tombstoned oid is in the sim's ledger of stated leaks
    (unlinked split or growth pages, a burned oid's pages); every page a
    live relation reaches carries its owner; and no page is reached from
    two structures.
- **The rigs, on two cores:**
  - a free on one core against creations on the other;
  - a mount-found reclaim at `cores = 2` while serving;
  - a miss parked between its `loading_` insert and its `Resolve` while
    its id is freed and popped;
  - for BF-R9, one cell per Census B path, and one with a seam between
    `Revalidate`'s read and the slot's publish.
- **Crash cells on the file rig, at every cut:**
  - before `D` passes, at `cores = 1` and at `cores = 2`;
  - between two levels of a tree, and inside a chain's pass;
  - between the last free and the final sync;
  - between that sync and the tombstone's clear;
  - after a reuse and before the clear;
  - a relaxed drop crashed before its commit is durable;
  - one core's `FlushMaps` parked between copy and write while the reclaim
    syncs: no child may be left allocated under a freed parent or a
    cleared tombstone;
  - a Waystone directory interior created after a reclaim, its parent's
    link written back and its frame not: the next recording execution
    leaves every live page byte-identical.
- **Mutations, each repeated:**
  - the gate removed;
  - the owner check removed;
  - the tombstone cleared before the sync;
  - a tree freed in one pass, with no sync between levels;
  - the map write barrier removed;
  - the frame left resident;
  - the free list skipped;
  - `CreateNewHeaderless` allowed to pop;
  - a pop that does not skip an id in `loading_`;
  - a dirty frame written back over a reuse;
  - the free run inside the drop;
  - for BF-R9, the epoch ignored, and the publish moved after the read.
- **The pre-SUS-1 heap arm,** unless BF-Q8 (a') strikes it, gets one cell
  and no crash matrix: only a test seam creates one.

### BF-R13 — The text

Every row of §1.8 and §1.11 is restated at BF-S6, except the one §1.11
marks outside BF's scope. The ones that carry the most:

- **DT1** says what is reclaimed and what stays leaked, without Bound Cabin
  pages. **DT7** and `read_borrow.hpp` give "reads on" its new reason
  (BF-R8, BF-R9).
- **`page.md` §5** drops "Nothing frees a page", and **§2a** gains its
  consumer.
- **`physical-optimizer.md` §6 gate 3** is answered for a dropped relation
  and stays shut for a mover, and DT1's horizon clause is replaced, as
  BF-Q17 marks.
- **`eviction.md` §3, `page.md` §7 and `CLAUDE.md`'s Buffer-pool eviction
  row** gain the dirty discard (§1.11's last paragraph).
- **The manual** (`sql.md`, the physical-optimizer manual) says what is
  reclaimed.
- **`txn.md` §4.1** names the reclaim beside the two purges, as a consumer
  of `D` and the epoch rather than of the horizon.
- **The superblock's version list** gains its 20 → 21 entry.
- **Done when** a grep of `src/ include/ docs/spec/ docs/rules/ manual/
  CLAUDE.md` for "nothing frees", "never cleared", "never freed", "stay
  allocated", "orphan", "reclaims no", "no space is reclaimed", "never
  reclaimed", "frees no page" and "no free-page path" finds every
  remaining hit true.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BF-S0 | **The order** | <ul><li>This file</li><li>Its `critics-developer` review (§6)</li><li>The word recorded in `raft-marks-2026-10-07.md`</li><li>The index row</li></ul> | S |
| BF-S1 | **The census, and red first** | <ul><li>**Census A.** Every page-creating path, with its first record: a `PAGE_INIT`, an image, a `BTREE_SPLIT`, or none (§1.4's signature). Also every creation path that is not logged (the Waystone directory interior and the Waystone target page), with what a crash-time read of a reused id yields there. A logged path with none gets a red crash cell, and BF-S2 fixes it; an unlogged path is kept off reused ids (BF-R6) or takes BF-R5's zero-write fallback.</li><li>**Census B.** Every path that reaches a relation's pages without holding its `IS` to the end: refused readers, stale-memo binds, a resumed statement's re-bind (§1.9 item 2), the reverse foreign-key walk, the Cabin optimizer, `SHOW RELAYOUT`, `DESCRIBE`, and the inner build. For each, whether a mount-found tombstone's pages are reachable through it.</li><li>**Census C.** Every "nothing frees" premise in code (§1.8), with the sentence that replaces it.</li><li>**Census D.** What an anchor names after a dropped, a rolled-back and a failed `CREATE INDEX`, and whether every tree page is reachable by descent.</li><li>**Red at BF-S0's commit:**<ul><li>a dropped relation's pages are still allocated after a restart;</li><li>after N = 5 create-fill-drop rounds across clean restarts, `allocated_pages()` exceeds round 1's by at least 4× one round's pages;</li><li>a new relation lands on no id a dropped relation held;</li><li>`VerifyTupleAt` answers `kOk` on another owner's page whose slot carries the expected pk;</li><li>`ReadTuple` reads a slot whose length leaves the page.</li></ul></li><li>**Guards, green and kept green:** a rolled-back drop, and an uncommitted drop at a crash, leave the relation whole (`sim_loop_test.cpp:490`).</li><li>**Decided here: the warm-up premise.** On the two-core file rig, the dropped relation's bits are cleared in the crash image the file rig snapshots (`tests/file_rig_crash.hpp:52-70`), by editing the map region's bytes, right after the completion checkpoint; a crash before every core publishes must refuse the next mount, and the same cut at `cores = 1` mounts. No store code is touched. If the `cores = 2` mount is not refused, repeated over a stated number of runs with the cadence checkpoint disabled for the cut, BF stops at this row and the operator rules on BF-R4's `cores > 1` arm. The arm is never struck by CLA.</li></ul> | M |
| BF-S2 | **The primitive and the allocator** (BF-R5, BF-R6, the store half of BF-R11) | <ul><li>**Code:**<ul><li>the seam, with its `NotImplemented` default;</li><li>the discard-and-clear hold, with its deferrals and its `Corruption` refusals;</li><li>the map write barrier, and `PersistMaps` as the map sync;</li><li>the free list in `CreateNew` and `CreateAt`, popped under the frame-table latch then the map latch, skipping an id in `loading_` or resident;</li><li>`CreateNewHeaderless` claiming from the cursor only;</li><li>`allocated_pages_` decremented;</li><li>`EvictClean` deleted;</li><li>`InsertFrame`'s create arm turned into a refusal;</li><li>each Census A path fixed;</li><li>Census C's code comments restated.</li></ul></li><li>**No caller from the drop yet.**</li><li>**Cells:** a freed id's frame is gone, and a create of it returns a zeroed page; each deferral (pinned, latched, writing, loading); each refusal, with its code; a second free answers `NotFound`; the list is asked ahead of the cursor; a headerless create never returns a freed id; a miss parked by a seam between its `loading_` insert and its `Resolve` keeps its id off the list's pop; a `PersistMaps` returns only after another core's parked `FlushMaps` has written; the count.</li><li>**Mutations, each repeated:** the frame left resident; a pinned frame erased; the list skipped; the headerless refusal dropped; `CreateNewHeaderless` allowed to pop; the pop's `loading_` skip dropped; the map write barrier removed.</li><li>**The suite green**, also under `KDS_TEST_PAGE_LATCH=1`.</li></ul> | M |
| BF-S3 | **The carrier and the mount-time reclaim** (BF-R1..R4, BF-R7, BF-R8, BF-R10) | <ul><li>**Code:**<ul><li>the retype writes the roots, into the field renamed `pending_roots`;</li><li>the mount collects pending tombstones;</li><li>the tick walks, checks, gates and frees in batches of `kReclaimBatchPages`, syncs the map, and clears the word with its stamp kept;</li><li>the chain order BF-Q18 marks;</li><li>BF-R10's checks;</li><li>superblock 21, with its version-list entry (BF-Q4).</li></ul></li><li>**Green:** BF-S1's red cells.</li><li>**Crash cells** (BF-R12) on the file rig, at `cores = 1` and `cores = 2`, the parked-`FlushMaps` cell and the Waystone directory cell among them.</li><li>**Mutations, each repeated:** the gate removed; the owner check removed; the word cleared before the sync; the free run inside the drop; a chain freed head-first in one pass, under BF-Q18 (c).</li><li>**The suite green**, and the waystone, index, cabin and inner-build contract suites byte-identical.</li></ul> | L |
| BF-S4 | **The sim and the rigs** (BF-R12) | <ul><li>the op, the oracle, one anchor per instance, and the owner census with its ledger of stated leaks;</li><li>the two-core rig cells;</li><li>`scripts/sim.sh` green;</li><li>each of BF-S3's mutations killed from the sim or a rig, or listed in this row with the reason it cannot be.</li></ul> | M |
| BF-S5 | **Reclaim within the run** (BF-R9), opened only on BF-Q1 (b) and BF-Q9 | <ul><li>**Code:** the commit-arm queue; the predicate BF-Q9 marks, published before the word is read; BF-Q11's fix.</li><li>**Cells:** one per Census B path on the two-core rig, with the path in flight across the drop's commit and the reclaim waiting until it ends; a seam between `Revalidate`'s read and the slot's publish, with the reclaim waiting; a drop rolled back with its reclaim queued frees nothing.</li><li>**Mutations, each repeated:** the predicate ignored, killed by a refused reader's cell (a wrong row or `Corruption`); the publish moved after the read, killed by the same cell; the queue filled before the decide, killed by the rollback cell.</li><li>**The suite green.**</li></ul> | L |
| BF-S6 | **The close** | <ul><li>A row per stage, and what BF carries.</li><li>**The measurement** (§5).</li><li>**The text** (BF-R13), its grep included.</li><li>**The manual:** `sql.md`'s DROP TABLE paragraphs and its "No space reclamation" bullet, and the physical-optimizer manual's gate 3 row, restated.</li><li>**The eviction texts:** `eviction.md` §3 (`:105-107`) and `page.md` §7 (`:167`) gain the dirty discard, on top of whatever BE-S6 left.</li><li>**`known-gaps.md`:**<ul><li>an entry for each remaining leak in §0's first "does not" bullet;</li><li>BF-Q18's chain leak, if (a) is marked;</li><li>the `cores > 1` delay with checkpoints off;</li><li>the free list's loss at a crash, and the ids the floor hides for that run (BF-R6);</li><li>in-run drops reclaimed only at the next mount, unless BF-S5 is built (BF-R8);</li><li>a tombstone whose walk is refused stays pending and is refused again at every mount (BF-R3);</li><li>`SHOW PAGE` of a reused id shows its new owner;</li><li>`:296-299` and `:796` restated.</li></ul></li><li>**`CLAUDE.md`'s** DROP TABLE, free-map, WAL and Buffer-pool eviction rows flipped, and its three superblock mentions (the B+ tree, Assertions and Ranges rows) moved to 21.</li></ul> | M |

## 4. Items for the operator

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BF-Q0 | **Open BF**, with BF-S0..S6 and BF-R1..R13 as written | process | Yes | **as proposed: yes**, W1 |
| BF-Q1 | **When pages are reclaimed.**<br>(a) At mount only: BF-S0..S4 and S6, with BF-S5 struck.<br>(b) At mount first, then within the run (BF-S5), opened on BF-Q9's mark.<br>(c) Within the run only | scope; **[quiet-wrong]** within the run | (b). The mount-time stages build the carrier, the walk, the gate and the primitive that BF-S5 reuses, and they need no reader argument (BF-R8). (c) still needs the mount's re-drive for a crash between the commit and the reclaim, which is (b)'s first half built last | **as proposed: (b)**, W1 |
| BF-Q2 | **What makes a free crash-safe.**<br>(a) `D` past the drop: no record that names the page can be replayed again, and no WAL record is added (BF-R4).<br>(b) Log `FREE` (15), with an applier that runs before the page load, a freed set in analysis, a gated map flush, and a segment-format bump.<br>(c) Never clear a bit: freed pages stay allocated and are reused through a logged and stamped `PAGE_INIT`, as undo pages are, after every first-write path is changed to stamp | format; **[quiet-wrong]** if neither | (a). It changes neither redo nor the log format, and it reuses the one bound recycling already trusts. (b) changes the WAL, analysis, redo and the flush order together, and still needs (a)'s durability. (c) turns every first-write path into a correctness obligation (§1.4), and loses its list at every crash | **as proposed: (a)**, W1 |
| BF-Q3 | **Where the roots wait between the commit and the reclaim.**<br>(a) In the tombstone's `rel_id`, written by the retype and cleared once the reclaim is durable (BF-R2).<br>(b) In the dropped relation's `sys.tables` mark, which §5c and §5d would skip until it is reclaimed.<br>(c) Nowhere: an `owner_oid` census of the whole file at every mount that has a tombstone.<br>(d) In a new catalog relation of pending reclaims | format | (a). (b) keeps a mark past its mount, which reopens the reissued-id ambiguity §5c exists to end. (c) reads every page of the file on every such mount, and has no "done" without new state. (d) adds a relation for a word the tombstone already has | **as proposed: (a)**, W1 |
| BF-Q4 | **The format, which follows BF-Q3.** Under (a), (b) or (d) an on-disk meaning changes - a tombstone's word, a mark that outlives its mount, or a new relation - so superblock 20 → 21 and version 20 is refused. Under (c) nothing on disk changes | format | As the standing order says (`raft-marks-2026-10-07.md` §11), so it is not offered as a choice. Its effect is listed for the operator: no volume written before BF mounts, so no tombstone without roots is left on any volume | **as proposed: superblock 21, version 20 refused**, W1 |
| BF-Q5 | **How a reclaim finds the pages.**<br>(a) A walk from the roots, checked page by page (BF-R3).<br>(b) An `owner_oid` census (`page.md` §2a, query 3).<br>(c) (a) to reclaim, and (b) only as the sim's oracle | design | (c). A census reads the whole file, and it attributes a burned oid's pages to whichever relation is issued that oid next (§1.1), so those pages can only leak. As an oracle it costs production nothing | **as proposed: (c)**, W1 |
| BF-Q6 | **How a freed id is handed out again.**<br>(i) An in-memory free list ahead of the cursor, with the floor untouched (BF-R6).<br>(ii) Lower the cursor to the lowest freed id.<br>(iii) Split the floor from the cursor, and log `ALLOC`/`FREE` so the floor can go.<br>(iv) No reuse within the run | design | (i). (ii) would also hand out a clear bit below the floor that the log names (`page_store.hpp:330-333`). (iii) is BF-Q2 (b) plus a new allocator. (iv) leaves a run's frees invisible until a clean restart | **as proposed: (i)**, W1 |
| BF-Q7 | **A freed page's resident frame.**<br>(a) Discarded, dirty or not, in the hold that clears the bit, deferring a held frame, with `EvictClean` deleted (BF-R5).<br>(b) Written back, then evicted, keeping "no eraser removes a dirty frame" | design | (a). After BF-R4's gate the bytes are dead, and a writeback could land them over a reuse. `EvictClean` has no production caller, so the discard replaces it rather than adding a second way to evict on demand | **as proposed: (a)**, W1 |
| BF-Q8 | **Which pages.**<br>(a) What BF-R3's walk reaches: the clustered tree or heap chain, every tree the anchor names, the var-heap chain and the anchor.<br>(a') As (a), but a heap root is counted `reclaim_skipped`, its tombstone cleared, and the leak stated, since only a test seam creates one (BD §1.5).<br>(b) (a), plus a tree at `DROP INDEX`'s own commit.<br>(c) (b), plus Bound Cabin and Waystone pages | scope | (a), with DT1 corrected for Bound Cabin pages, and the heap arm given one cell and no crash matrix (BF-R12). (a') saves that arm's code for a class no mountable volume holds, at the price of a leak on a volume the test seam made. (b) needs its own gate against the live relation's readers. (c) covers pages that no relation owns (owner 0, or no header) | **as proposed: (a)**, W1 |
| BF-Q9 | **What licenses a free within the run** (BF-S5).<br>(a) The read horizon, `ResolvedForEveryReader`.<br>(b) The statement epoch, with the Cabin optimizer's tick in it, each core publishing `kEntering` before it reads the schema word (BF-R9).<br>(c) A refused read borrow waits for the DDL's decide and re-binds, a granted one re-checks the memo, and every walker takes `IS` - `DESCRIBE`, which never binds, included.<br>(d) An owner check on every page fetch, as the authority | invariant; **[quiet-wrong]** | (b), checked against Census B. It covers `DESCRIBE` as a statement without a bind. (a) misses stale-memo binds, check views and `DESCRIBE`, and stops on one idle `BEGIN`. (c) ends "a reader never waits" and puts readers into the wait-for graph (`txn.md` §5). Under (d), one missed site is a wrong answer | **as proposed: (b)**, W1 |
| BF-Q10 | **The defence checks** (BF-R10).<br>(a) The verifier's class and owner check, the walks' root check, and `ReadTuple`'s bound.<br>(b) The verifier alone.<br>(c) None | design | (a). Each costs bytes already in hand, and `ReadTuple`'s missing bound becomes reachable once pages are reused | **as proposed: (a)**, W1 |
| BF-Q11 | **The RESTRICT window.** Both blockers are read before the drop's `X` and not re-read under it, and `CreateForeignKey` locks no parent (`command_dispatcher.cpp:2629-2650` against `:2667`; `catalog.cpp:3114`).<br>(a) Fixed in BF-S5: re-checked under `X`, and a foreign key's creation takes the parent's `IS`.<br>(b) A bug entry, with BF-S5 gated on it.<br>(c) Accepted | scope; **[quiet-wrong]** within the run | (a). Today the window leaves a harmless dangle on allocated pages. Once pages are reclaimed within the run, a cached parent could be descended into reused pages | **as proposed: (a)**, W1 |
| BF-Q12 | **An autocommit drop: the specs against the code.** DT5 and `ddl-transactional.md` §5, §5a and §5d say autocommit retires. The code delete-marks, and §7 agrees with the code (§1.1).<br>(a) The code governs, and the specs are restated.<br>(b) The specs govern, and autocommit retires | spec conflict | (a). `read_borrow_rig_test.cpp:196-222` already rests on the marks, and BF depends on neither reading | **as proposed: (a)**, W1 |
| BF-Q13 | **The counters** (BF-R11), with no configuration key; the batch bound is a constant, `kReclaimBatchPages` (64, BF-R8) | user-visible | Yes. The bound only paces a background task, so a key for it would name nothing an operator needs to set | **as proposed: yes**, W1 |
| BF-Q14 | **Order against BE and BA.**<br>(a) BF-S2 lands after BE-S2, so the discard is written once, against slots, and BE's census counts it. BA-R5 takes BF-R5's reason. If BE stops at BE-S1's premise gate, BF-S2 proceeds against the map-keyed frame table, which is (b), and BE-R1's eraser list, if BE later resumes, counts BF-R5's discard.<br>(b) BF-S2 first, and BE-R1's eraser list grows to four.<br>(c) The frame half folds into BE | sequencing | (a). BF-S1 touches no store code and can run beside BE now | **as proposed: (a)**, W1 |
| BF-Q15 | **The measurement** (§5), once at BF's close, per `CLAUDE.md`'s Session Workflow step 3 | process | As written | **as proposed: as written**, W1 |
| BF-Q16 | **Where the reclaim runs.**<br>(a) On core 0's system tick, gated by an instance-wide pending count, as the purge is gated by the mark count.<br>(b) Beside the §5d purge at DDL resolution, with no cadence.<br>(c) At the drop's own resolution | design | (a). (b) ties a reclaim to unrelated DDL, and its "between resolutions" argument holds for one core only (`ddl-transactional.md` §5d). (c) runs before `D` can pass, so it always defers | **as proposed: (a)**, W1 |
| BF-Q17 | **DT1's two preconditions for reuse.** DT1 makes reuse wait on `physical-optimizer.md` §6 gate 3 and on a consumer of the reader horizon (`drop-table.md:12-19`), and §6 records no decision that opens a gate (`:211`).<br>(a) BF answers gate 3 for a dropped relation only, by BF-R3's owner walk, BF-R4 and BF-R10, and replaces DT1's horizon clause with BF-R8 at mount and BF-Q9 within the run.<br>(b) The specs govern, and BF waits for a horizon consumer | spec conflict; **[quiet-wrong]** | (a). Gate 3's collision is a reused page validating a stale location, which BF-R3 and BF-R10 turn into a miss; and the horizon misses stale-memo binds, check views and `DESCRIBE`, and stops on one idle `BEGIN` (§1.6), so (b) would gate reuse on the weaker test | **as proposed: (a)**, W1 |
| BF-Q18 | **A chain cut by a crash mid-free** (BF-R7).<br>(a) A stated leak in `known-gaps.md`: the chain is freed head-first in one pass, and a crash inside the flush leaks the rest for good.<br>(b) The re-drive reads a cleared page's device bytes, owner-checked, to follow its link.<br>(c) A chain is freed tail-first, one map sync per page | design | (c). Its cost is one map sync per chain page, paid on the background tick within `kReclaimBatchPages`, and it needs no new read path. (b) costs a store read of an unallocated id, which nothing has today, and still leaks once the cleared page is reused before the re-drive, since the owner check then stops the walk. (a) costs nothing to build and leaks a chain's tail at every such crash | **as proposed: (c)**, W1 |

## 5. Sequencing and measurement

1. **BF-S1 now, beside BE.** It touches no store code.
2. **BF-S2 after BE-S2** (BF-Q14 (a)). If BE stops at BE-S1's premise
   gate, BF-S2 proceeds against the map-keyed frame table, which is (b),
   and BE-R1's eraser list, if BE later resumes, counts BF-R5's discard.
3. **BF-S3 after BF-S2, and BF-S4 after BF-S3.** Nothing reaches reuse
   before BF-S3, so BF-S2 is safe on its own.
4. **BF-S5 only once BF-Q1 (b) and BF-Q9 are marked.** It comes after BF-S4,
   with BF-Q11's fix inside it. Opening it before Census B decides BF-Q9
   would build a predicate without its list.
5. **BF-S6 last.** BA-R5's reason is restated there, and BA's order carries
   it when BA resumes.

**Measurement.** Once, at BF's close, in `build-release` (`ck-tester`), per
`CLAUDE.md`'s Session Workflow step 3. It compares the commit BF opened at
against the commit that closes it, names both by `git describe --tags`, and
goes in `bench/v3.0.0/`. Every stage before the close lands with "overhead
not measured; measured at the milestone's close".

- **Overhead, interleaved A/B,** on the series' OLTP shapes. B must not
  regress:
  - BF-R6 adds a pop to every creation;
  - BF-R10 adds a check to every verified location and every walk's root;
  - BF-R9, if built, adds three stores and a fence to every statement.
- **The leak closed.** Create, fill and drop, 50 rounds, at `cores = 1`
  and at `cores = 2`, with a clean restart between rounds, then again with
  a crash between them. Each round, after its mount, waits for
  `reclaim_pending = 0` before it creates, so the previous round's drop is
  reclaimed past the `cores > 1` warm-up. Report the file size and `pages_allocated` per
  round. A plateau after the first round is the pass.
- **The cost of reclaiming.** Pages freed per second on the tick, and mount
  time against the number of pages pending, at `cores = 1` and
  `cores = 2`.
- **The gate's delay.** From the commit to the last free, at the default
  checkpoint interval, at `cores = 2`.

## 6. Row status

### BF-S0 — written 2026-10-07

- **Where:** on `worktree-drop-table-page-reclaim` from `bc144dbf`
  (`v2.7.0-668-gbc144dbf`), on the operator's word above.
- **How §1 was made:** read, not run. It draws on six read-only surveys —
  the free map and the allocator, what a relation owns, every holder of a
  page id outside the relation, readers and the horizon, the WAL and crash
  safety, and the buffer pool with the neighbouring orders — and on CLA's
  own reads where those surveys disagreed.
- **What the reads settled:**
  - §1.9 item 1. The warm-up holds the anchor at `cores > 1`
    (`superblock_checkpoint_anchor.cpp:44`), and redo leaves the recLSN
    unset, so at `cores = 1` the completion checkpoint does move `D` past
    the replayed log.
  - §1.1's mount order. Marks are retired before the completion checkpoint
    (`expeditor.cpp:913` against `:1092`), which is what rules out a
    `sys.tables`-row carrier at mount.
  - The standing order's text (`raft-marks-2026-10-07.md` §11), which makes
    BF-Q4 follow BF-Q3 rather than stand as a choice.
- **What stays open:** §1.9 items 2-8, each an item in §4 or a census in
  BF-S1.
- **Recorded with it:** the word in `raft-marks-2026-10-07.md` §21, and
  the index row. The review is recorded below.
- **No engine file moved, and no suite ran.**

### BF-S0's review - 2026-10-07

One review pass of three lenses - survey truth, design correctness, and
scope and house rules - produced 30 findings: 3 high (findings 9, 14 and
15), 13 medium and 14 low. Each was checked against the cited code on
`worktree-drop-table-page-reclaim` at `bc144dbf` before it was applied,
and line numbers that had drifted were corrected (`UnpinFrame` after the
copy is at `:1590`, the stamp-keeping overwrite at `catalog.cpp:2447`).

**The three high findings:**
- **A reused id under an unlogged headerless page** (9). A Waystone
  directory interior on a reused id reads the dead image after a crash and
  follows it into live pages. BF-R6 now keeps `CreateNewHeaderless` off the
  free list, Census A covers unlogged paths, and BF-R5 names the zero-write
  as the fallback.
- **The text census missed the manual, `index.md`, `waystone-concpets.md`,
  `page.md:63` and the preamble of `drop-table.md`**, and the grep could
  not find them (14). §1.8, BF-R13's grep and BF-S6 now name them.
- **BF-R5's dirty discard was left to BE-S6**, which knows nothing of it
  (15). BF-S6 now restates `eviction.md` §3, `page.md` §7 and `CLAUDE.md`'s
  eviction row itself.

**Design changes:**
- a map write barrier, since a `Sync` does not wait for another core's map
  write, and `PersistMaps` named as the map sync (BF-R5, BF-R7; 8, 10);
- BF-R9 publishes before it reads the schema word (11);
- a pop skips an id with a fault in flight, and the invariant is restated
  (13);
- the sim's oracle checks the drop's ledgered roots, not every page with a
  tombstoned oid (12);
- a writeback's claim is a deferral in §0's line too (3).

**New items:** BF-Q17 (DT1's two preconditions for reuse, a spec conflict)
and BF-Q18 (a chain cut by a crash mid-free), with BF-R7, BF-R8 and BF-R13
citing them (20, 21); BF-Q8 gains (a') for the heap arm (30).

**Survey corrections:** the verifier's callers and the unverified heap
bucket (1); `DESCRIBE` as a fourth unborrowed walker (2); the three checks
that read a tombstone, and a clear that keeps its stamp (4); the var-heap
growth leak (5); `D` held in memory and reset at mount (6); `ReadTuple`'s
slot-index bound (7); `CLAUDE.md` already at 20 (23); invariant 1
mis-cited (24).

**Process and wording:** BF-S1's premise cell edits the crash image
instead of a store seam, and a non-refusal stops BF for the operator
instead of striking an arm (17, 18); BF-Q14 and §5 say what BF does if BE
stops at BE-S1 (19); BF-R10 is BF-S3's (16); BF-S6's `known-gaps.md` list
gains four gaps (22); `kReclaimBatchPages` (64) is named (25); the seam
refuses `NotImplemented` and the walk's refusals `Corruption` (26); the
leak benchmark names its core counts (27); the tombstone's word is renamed
`pending_roots` (28); two done-when clauses are made decidable (29).

**Survey conflicts.** The doc/code conflicts the six surveys reported are
named in §1.8 or §1.11. The four epoch texts that do not bear on reuse are
marked outside BF's scope there, `eviction.md` EV4 and §3 `:82` stay
BE-S6's, and the `CLAUDE.md` superblock conflict was already resolved in
this tree (23).

**Declined:** none.

### BF opened - BF-Q0..Q18 marked as proposed - 2026-10-07

On W1 (`raft-marks-2026-10-07.md` §22), every item in §4 takes CLA's
proposal: BF-Q0 yes; BF-Q1 (b), the mount first and then within the run, so
BF-S5 is in, on BF-Q9's mark; BF-Q2 (a), crash safety from `D` with no WAL
record added; BF-Q3 (a), the roots in the tombstone's word; BF-Q4,
superblock 21 with version 20 refused, by the standing order; BF-Q5 (c), the
walk, with the owner census as the sim's oracle; BF-Q6 (i), an in-memory
free list; BF-Q7 (a), a freed page's frame discarded and `EvictClean`
deleted; BF-Q8 (a), what the walk reaches; BF-Q9 (b), the statement epoch,
published before the schema word is read; BF-Q10 (a), all three defence
checks; BF-Q11 (a), the RESTRICT window fixed in BF-S5; BF-Q12 (a), the code
governs and the specs are restated; BF-Q13 yes, no key and
`kReclaimBatchPages` a constant; BF-Q14 (a), BF-S2 after BE-S2; BF-Q15 as
written; BF-Q16 (a), core 0's system tick; BF-Q17 (a), BF answers DT1's two
preconditions for a dropped relation; BF-Q18 (c), a chain freed tail-first.

Five of the marks are on `[quiet-wrong]` surfaces - BF-Q1, BF-Q2, BF-Q9,
BF-Q11 and BF-Q17 - where a wrong choice turns a refusal into a wrong
answer. BF-S1 starts on its own word; nothing here starts it.


### BF-S1 - the census, and red first - built 2026-10-07

- **Where:** on `worktree-drop-table-page-reclaim` from `df741e3c`
  (`v2.7.0-669-gdf741e3c`), on the word of `raft-marks-2026-10-07.md` §23:
  *"go ahead dont stop until milestone, follow CLA proposal if decision
  needed"*. The suite at `df741e3c` was 3223 of 3223 before the stage.
- **The cells** are `tests/drop_table_reclaim_test.cpp`. Each drives an
  `Expeditor` without `Start()`, and every mount is `Expeditor::Open`.

**The warm-up premise holds, so BF does not stop here.**
`DropTableReclaimPremiseTest.AClearedBitRefusesTheNextMountUntilEveryCoreHasPublished`
builds a volume whose log, from its anchor on, names a dropped relation's leaf
by a record that is not its creation. It then mounts the volume, copies what a
crash right after the completion checkpoint leaves, clears the leaf's bit in
that copy's map page (the bytes edited and the checksum restamped, no store
code touched), and mounts the copy. The `cores = 2` instance is an
`Expeditor` opened at two cores and never started, not §3's `TwoCoreRig`
(`file_rig_crash.hpp`): no peer reactor runs, so no peer can publish
before the cut, and the mount is production's. Five runs, with no cadence
checkpoint:
- at `cores = 2` every second mount is refused, naming the leaf;
- at `cores = 1` every second mount succeeds.

BF-R4's `cores > 1` arm stands as written.

**Red at `df741e3c`, each for the reason it names:**
- `ADroppedRelationsPagesAreFreeAfterTheNextMount`: every page of the dropped
  relation is still allocated after a clean restart.
- `FiveCreateFillDropRoundsGrowTheVolumeByLessThanOneRound`: 55 pages after
  round 1 and 223 after round 5, where one round's relation holds 40. The
  other 8 are pages no relation owns - among them the undo pages a run leaves
  behind (UP4) - which BF does not reclaim. Not counted page by page.
- `ANewRelationLandsOnIdsADroppedRelationHeld`: no page of the new relation
  lands on an id the dropped one held.
- `TheVerifierMissesOnAnotherOwnersPage`: `VerifyTupleAt` answers `kOk` for a
  location that names another owner's page.
- `ReadTupleRefusesASlotThatLeavesThePage`: a slot at offset 8184 is read past
  the page.

**The suite on BF-S1's tree: 3225 of 3230.** The five failures are exactly
these cells, and the one disabled cell is the existing
`HeapSuspensionIsLifted`.

**Green and kept green:** `ARolledBackDropLeavesTheRelationWholeAcrossARestart`.
`sim_loop_test.cpp`'s two drop cells (`:490`, `:589`) are unchanged.

**Census A - every page-creating path** (read at `df741e3c`):
- Every logged path's first record that names the new page is a `PAGE_INIT`,
  a `FULL_PAGE_IMAGE` or a `BTREE_SPLIT` image, and redo creates the page on
  `NotFound` for all three (`redo.cpp:420-453`, `:538-559`). These paths are
  the undo log, CREATE TABLE's three pages, catalog growth, every btree and
  index split, the index build, var-heap growth and Bound Cabin growth. **No
  logged path is red.**
- The Bound Cabin path is logged, which corrects §1.2's "unlogged".
- `LogCatPageInit` is right and `redo.cpp:443-447`'s comment is stale
  (BF-S6).
- **R1, red: `CreateNewHeaderless` from the cursor is not a never-written
  id.** After a clean restart the cursor starts at 128 and finds the bits
  BF cleared, and a headerless read skips both the all-zero test and the
  checksum. A directory interior on a reused id would therefore follow the
  dead image's bytes as child ids, and BF-R6's "cursor only" does not cover
  it. **CLA's proposal, taken:** BF-R5's zero-write fallback, scoped to the
  headerless create. When the claimed id's device bytes are not all zero,
  zeros are written and synced before the headerless bit is set. It costs
  one device read per directory-interior creation.
- **R2:** the Waystone target page is unlogged and comes from `CreateNew`,
  so it may pop the free list. That is benign: a reader checks the page type
  and key, and the writer formats the page unconditionally
  (`waystone_page.cpp:129-132`).
- **A1, the neighbour of R1:** a never-written directory interior reads as
  zeros, and zero is a page id - the superblock - where `kEmptyDirSlot` is
  `0xFFFFFFFF`. That holds before BF, and R1's zero-write reaches the same
  state. **CLA's proposal, taken in BF-S2:** the directory walk treats a
  child id below the first user page as an empty slot.
- **A2 - is there a window between the checkpoint's dirty-table snapshot and
  its `CHECKPOINT_BEGIN` append?** Settled, no defect
  (`checkpointer.cpp:147-183`).
  - A core appends a record, stamps its page and checkpoints on one
    thread, so none of its own records falls in its own window.
  - A record `L` another core appends in that window, on a page the
    snapshot saw clean, is bounded by that core: its latest checkpoint
    either began before `L`, so its redo start is below `L`, or began after
    `L`'s stamp, so its snapshot holds the page at a recLSN at or below `L`
    or the page was written back after `L`.
  - The publish folds the minimum over every core's latest redo start and
    raises it to the highest anchor encoded before (BC-R1,
    `superblock_checkpoint_anchor.cpp:94-99`). Each earlier anchor was
    folded under the same bound, so the raise never lifts the anchor past
    an `L` whose page still needs it.

**Census B - who reaches a relation's pages without holding its `IS` to the
end:**
- **A tombstone a mount found is reached by no reader path**
  (`expeditor.cpp:893`, `:913`; `catalog.cpp:1412`, `:2121`, `:2202`), as
  BF-R8 argues.
- **§1.9 item 2, settled: every resume passes `Revalidate` before it
  re-binds.** `DispatchAndStage` begins with it (`command_dispatcher.cpp:780`),
  and every entry and re-run reaches it. The executor cannot park
  (`step_vm.cpp:166-175`). The one page id held across a park is a mid-walk
  `UPDATE` or `DELETE` cursor, which its relation `IX` fences.
- **For BF-R9:** one slot per core, published at `:780` and cleared when
  `DispatchAndStage` returns, is enough.
- **One red, for BF-R9 only: the KWP load endpoint**
  (`kwp_load_server.cpp:312-318`, `:462`) reads outside `DispatchAndStage`.
  It is safe while the reclaim runs on core 0's tick, because the load
  server runs on core 0's reactor and both handlers are synchronous. **CLA's
  proposal, taken in BF-S5:** both handlers publish the core's slot.
- **An orphan Cabin set** banked by a probe compiled before the drop is
  reachable only through a stale memo naming its `cabin_id`, which BF-R9
  covers, and it lives until restart. BF-S6 states it.

**Census C - every code premise resting on "nothing frees a page":**
- About sixty sentences, each classified: false under BF, still true, or
  already false at `df741e3c`.
- The store's are restated in BF-S2. The ones whose code BF-S3 changes are
  restated there, and the rest at BF-S6.
- Already false before BF: the `ALLOC`-before-extend texts
  (`file_page_device.cpp`, `page_device.hpp`, `memory_page_device.hpp`,
  `payload.hpp:449-451`); `allocated_pages()`'s "printed by SHOW META"; the
  `PersistMaps` durability claim; `command_dispatcher.cpp:2660`'s DT8;
  `mount_recovery.hpp:236-239`'s "no owner index"; `btree.hpp:182-184` and
  `index_tree.hpp:174-176`'s "never linked in"; `record.hpp:125-126`'s
  "FREE records".

**Census D - what an anchor names, and what a walk reaches:**
- A committed `DROP INDEX`, a rolled-back `CREATE INDEX` and a rolled-back
  `DROP INDEX` all leave the anchor slot naming a live tree. The slot is
  never removed, and `ANCHOR_UPDATE` is redo-only.
- A failed `CREATE INDEX` leaves no durable slot in any arm, so its tree is
  §0's stated leak.
- Every page class carries its type and an owner:
  - the relation's oid on clustered, var-heap, anchor and heap pages;
  - the index's oid on index pages, which come from another sequence, so
    the class has to discriminate;
  - 0 on everything else.
- An internal node records its `level`, which is what "the class the parent
  says" checks.
- **Found:** a dividing leaf whose promotion fails is linked into the leaf
  chain and reached by no descent (`btree.cpp:772-773` against `:792`;
  `index_tree.cpp:454-455`). **CLA's proposal, taken:** BF-R3's walk also
  follows the clustered leaf chain and the index leaves' right-sibling chain
  from the leftmost leaf, into the same visited set and under the same owner
  check, so those pages are reclaimed rather than leaked.
- **Found, outside BF, and reproduced:** a root growth publishes the anchor
  in an `ANCHOR_UPDATE` after its split record. A log cut between the two
  leaves the anchor naming the grown-over root. Point lookups then miss every
  committed row the split moved: 197 of 198 in a cell where the root leaf
  divides, 680 of 1358 in one where an internal root divides. The repro is on
  `worktree-agent-a6378d577b6230ab1` at `c4115721`, and the entry is
  `docs/inflight/bugs/a-root-growths-anchor-publish-is-logged-after-its-split.md`.
  For BF it is a leak, never a wrong free: from a grown-over root, the
  descent misses the new root's right half, and the leaf-chain arm still
  reaches every leaf.
- The var-heap chain has no backward link, so BF-Q18 (c)'s tail-first free
  collects the chain forward and frees it in reverse.

**Decisions taken under §23, each CLA's proposal:**
- **BF-Q14 (b).** BE has not started (BE-S1 is its next stage), so BF-S2
  runs against the map-keyed frame table, and BE-R1's eraser list counts
  BF-R5's discard when BE resumes.
- R1's zero-write.
- A1's empty-slot reading.
- BF-R3's leaf-chain arm.
- BF-R9's slot shape, and the load endpoint's publication.

**Review** (`critics-developer`, one pass over the cells and this row):
four defects, each fixed in the cells and each re-run.
1. The "clean shutdown" was not clean. An `Expeditor` that is never started
   runs no shutdown checkpoint, and a bare `Checkpoint()` keeps the redo
   start at the oldest dirty page's recLSN. The next mount therefore replayed
   the whole round, and its high-water repair lifted the floor over every
   dropped page. `CleanShutdownCheckpoint` now syncs the store, then
   checkpoints, as `CoreRuntime::ShutdownCheckpoint` does.
2. The verifier cell could not have turned green correctly, because the
   call carried no owner. `VerifyFor` now passes one: owner 77 must verify,
   which is green today and stays green, and owner 88 must miss, which is
   red today.
3. The premise cell's refusal check matched any substring of the message.
   It now matches redo's own `names page <leaf>,`.
4. The `ReadTuple` cell now checks that the word it edits is the tuple's
   offset before it relies on it.

The review also corrected this row:
- A2's argument now names BC-R1's floor and the gap between an append and
  its stamp.
- The rig the cells use is stated: an `Expeditor` opened at two cores and
  never started, where §3's row named `TwoCoreRig`.
- The 8 pages beyond the relation are no longer called undo pages without
  a count.

Three proposals were taken: the leak cell's rename, its missing direct
includes, and switching `ClearMapBit` to BF-S2's `FreeMapRelease` (done at
BF-S2). Two were declined:
- the cells' helpers stay local rather than moving into a new shared
  header, because `file_rig_crash.hpp` pulls in `two_core_rig.hpp`;
- `AllocatedPages`' scan bound stays a margin over the allocated count,
  because the store has no high-water accessor and BF does not add one for
  a test.

**Overhead not measured;** BF-Q15 measures it at BF's close.

### BF-S2 - the primitive and the allocator - built 2026-10-07

- **Where:** on `worktree-drop-table-page-reclaim` from `970feb5f`, run
  against the map-keyed frame table under BF-Q14 (b) (§23).
- **Nothing calls `FreePage` in production yet.** BF-S3 adds the reclaim.

**Code:**
- **The seam.** `PageStore::FreePage`, whose default refuses
  `NotImplemented`, and `FreeOutcome {kFreed, kDeferred}`.
- **`DevicePageStore::FreePage`.** One hold, frame table then map:
  - It discards the frame, dirty or not, and poisons it in debug builds.
  - It clears the bit (`FreeMapRelease`, new in the codec), decrements
    `allocated_pages_`, marks the region dirty and lists the id.
  - It defers on a pin, a page latch, a writeback claim, a fault in flight
    (`loading_`) or a `CreateAt` in flight (`claiming_`).
  - It refuses `Corruption` below `Open`'s `first_new_page_id`, for a map
    id, for a headerless id and for a resident `kCabinBound` frame. An id
    already free answers `NotFound`.
- **The free list** (BF-R6). It is an ordered set under the map latch,
  popped lowest first by `CreateNew` ahead of the cursor, under both holds,
  and the pop skips an id that is resident or has a fault in flight.
  - `CreateAt` takes a listed id off the list.
  - The cursor passes over a listed id. This was the review's B2: every id
    a clean mount's reclaim frees lies above that mount's cursor.
  - `CreateNewHeaderless` never pops. Since the review's B1, it marks its
    id headerless before any frame exists.
  - On a claimed id whose device bytes are not all zero, it writes and
    syncs zeros first. This is R1's zero-write: after a clean restart the
    cursor does find the bits BF cleared.
- **`InsertFrame` refuses a create that finds a frame already resident**,
  with `Corruption`. It is defence, and its return type became `StatusOr`.
- **The map write barrier.** `map_flush_latch_` is held across
  `FlushMaps`' copy and write, outer to the map latch and null unarmed.
  `PersistMaps` is the map sync, and its comment now says so.
- **`EvictClean` is deleted.** Its callers moved:
  - `am_s2_pin_protocol_test`'s rounds now use the sweep, and count a page
    still resident afterwards as the failure;
  - the two eviction cells reopen a store over the same device;
  - the two refusal cells became `FreePage` deferral cells.
- **A1:** the Waystone directory walk reads a child id of 0 as an empty
  slot (`waystone_dir.cpp`'s `LoadChild`).
- **Census C's store sentences are restated:** `page_store.hpp`'s floor
  cost, `high_water.hpp`'s, the hit path's "never cleared", `CreateNew`'s
  "clear one instruction ago", `WriteBack`'s dirty-frame premise,
  `CreatePinned`'s, `FlushPages`' and `Flush`'s ordering notes,
  `FlushMaps`' "the rest write nothing", the eraser lists, and
  `free_map.hpp`'s unbuilt `ALLOC`/`FREE`.
- BF-S1's `ClearMapBit` now uses `FreeMapRelease`.
- **Two test seams:** `SetAfterFaultPublishedForTest` and
  `SetAfterMapCopyForTest`.

**Cells** (`tests/page_free_test.cpp`, 11):
- a freed id has no frame, and its next create returns a zeroed page;
- a dirty frame is discarded and never written back;
- a pinned, latched or writeback-claimed frame defers the free and changes
  nothing;
- a fault in flight defers it;
- the four refusals, and `NotFound` for a second free;
- the list is asked ahead of the cursor;
- the cursor passes over a listed id above it;
- a headerless create never takes a listed id, and after a restart zeroes
  the dead image it lands on;
- a pop skips an id whose fault is parked between its `loading_` publish
  and its read;
- a `PersistMaps` returns only after another thread's parked `FlushMaps`
  copy is written;
- a directory slot reading zero is empty.

**Suite:** 3235 of 3240, plain, and 3235 of 3240 with
`KDS_TEST_PAGE_LATCH=1`. In both, the five failures are exactly BF-S1's red
cells, which BF-S3 turns green. The armed run also skips its three usual
cells.

**Mutations**, each built and run five times against `PageFreeTest`, plain:

| mutation | killed by |
|---|---|
| the frame left resident | the freed-id cell, the dirty-discard cell, the list cell |
| a pinned frame erased | the held-frame cell (plain; under the latch census the latch's own deferral covers it, so it runs plain) |
| the list skipped | the freed-id cell, the pop cell |
| the headerless refusal dropped | the refusal cell |
| `CreateNewHeaderless` allowed to pop | the headerless cell, the cursor cell |
| `FreePage`'s in-flight deferral dropped | the fault-in-flight cell |
| the cursor takes a listed id | the cursor cell |
| the pop's `loading_` skip dropped | the pop cell |
| the map write barrier removed | the `PersistMaps` cell |
| the headerless zero-write dropped | the headerless cell |
| a zero directory slot read as page 0 | the directory cell |

All eleven are killed. The first hung rather than failed, because its fault
seam never fired. The cells' waits are now bounded at ten seconds
(`SpinUntil`), so a seam that never fires fails its cell, and all eleven
cells pass five runs over with the bound in place.

**Review** (`critics-developer`, one pass):
- **B1, fixed by the review.** The headerless create inserted its frame
  before setting the headerless bit, so a writeback in the gap stamped a
  checksum over child 1. The gap predates BF; R1's zero-write had widened
  it to a device read, a write and an fsync. The id is now marked
  headerless before the insert (`MarkHeaderlessBeforeInsert`).
- **B2, taken as CLA proposed under §23:** the cursor skips listed ids, and
  the list became an ordered set.
- **B3, noted:** between a claim and its insert, `FreePage` could free a
  just-claimed id. Only a reclaim's precondition rules that out - BF-R3
  reaches only a dropped relation's checked pages - and BF-S3 keeps it so.
- **Cell gaps, closed:** the deferral on a fault in flight, and the
  cursor's listed-id skip.
- **Also from the review:** text fixes (`PopFreeListLocked`'s doc,
  `EnsureHeaderlessMap`'s caller, two `EvictClean` mentions) and the
  cell's rename.
- **Declined:** none.
- **Noted as cost:** a zero-write holds the parent directory page's latch
  across an fsync, on a reused id only.
- **Left to BF-S6:** `allocated_pages()`'s "printed by SHOW META" (true
  from BF-S3), and `catalog.cpp:1226` and `eviction.md:114`, which still
  name `EvictClean`.

**Overhead not measured;** BF-Q15 measures it at BF's close.

### BF-S3 - the carrier and the mount-time reclaim - built 2026-10-07

- **Where:** on `worktree-drop-table-page-reclaim` from `805e6c9a`.

**Code:**
- **The carrier (BF-R2, BF-Q4).**
  - `SysObjectRow::rel_id` is renamed `pending_roots`, at the same offset.
    `PackPendingRoots` and `UnpackPendingRoots` pack the anchor into the
    high half and the var-heap root into the low half by shift and mask.
  - `Catalog::DropTable` reads the relation's `sys.tables` row before it
    writes anything, and its retype stores the packed roots. A rollback
    therefore restores 0 with the rest of the row.
  - `Catalog::PendingReclaims` lists every tombstone whose word is nonzero.
  - `Catalog::ClearPendingRoots` overwrites the word to 0, logged with
    `kNoTxnId` and keeping the row's stamp, and moves no schema word.
  - `sys.objects`' view keeps the column name `rel_id`. A live table, the
    only kind it lists, reads 0 there.
  - **Superblock 21**, with its version-list entry; 20 is refused.
- **The reclaim (BF-R3, R4, R7, R8),** `server/page_reclaim.{hpp,cpp}`.
  - `PageReclaimer::CollectAtMount(gate)` queues the pending tombstones.
  - `Step(D)` does nothing for a job until `D` reaches the job's gate.
    Then it walks the job, at most `kReclaimBatchPages` (64) page reads a
    step:
    - the anchor;
    - the clustered tree by descent, plus its leaf chain, which reaches a
      leaf a failed promotion left (BF-S1's Census D);
    - every tree an anchor slot names (`AnchorIndexSlots`, new), plus its
      right-sibling chain;
    - a pre-SUS-1 heap chain;
    - the var-heap chain.
  - **Every page is checked** for the class its parent names, its level
    included, and for its owner, which is the slot's index oid on an index
    page. A page that fails the check is counted `reclaim_skipped` and is
    neither freed nor descended through. A page already free is passed
    over. A page a descent reaches twice refuses the job.
  - **The plan frees children before parents:** every tree's leaves, then
    each internal level in turn, with a `PersistMaps` after each level;
    each chain tail-first, one sync a page (BF-Q18 (c)); the anchor last,
    then a sync.
  - Only then is the tombstone's word cleared. A deferred free ends the
    step, and the next step asks again.
  - A refused job is logged, counted `reclaim_refused` and left pending
    for the next mount.
  - A fault the pool refuses (`ResourceExhausted`) defers the walk to the
    next step.
  - A reused anchor reads as a completed reclaim, because the anchor is
    freed after every other page.
- **The gate for a tombstone a mount found pending** is the log's append
  point once recovery returns. Every record that names the relation's
  pages lies below it, and at `cores = 1` the completion checkpoint's redo
  start reaches it.
- **Expeditor.**
  - It collects after the completion checkpoint and before any listener
    binds.
  - `ReclaimStep()` runs on core 0's `system` tick every 50 ms (BF-Q16
    (a)), as one predicate and a return when nothing is owed.
  - `D` is the checkpoint anchor's durable redo start.
- **`SHOW META` (BF-R11)** prints `pages_allocated`, `pages_freed` and
  `pages_reused` from a new `PageStore::allocation_counters()`, and, on
  every core of an assembled instance, `reclaim_pending`,
  `reclaim_deferred`, `reclaim_skipped` and `reclaim_refused`.
  `reclaim_refused` is the fourth counter, beside BF-R11's three: a refused
  walk stays pending (BF-S6's known-gaps line), and the count is how an
  operator sees one.
- **BF-R10.**
  - A statement's bind checks that each relation's anchor and first page
    still carry its oid (`StillOwned`, once per bind).
  - `VerifyTupleAt` takes the expected relation's oid. It answers the new
    miss `kNotThisRelation` unless the page is `kHeap` or `kBtreeLeaf`
    carrying that oid. Every caller passes it: the probe memo, Waystone
    replay, the inner build's btree and heap arms, the Cabin serve's two
    phases, the reverse foreign-key Cabin path, and the optimizer's heal.
  - The heap inner-build arm now goes through the verifier and refuses
    `Corruption` on a miss: its location is this statement's own.
  - `ReadTuple` refuses `Corruption` for a slot whose offset and length
    leave the page, or whose payload is longer than its tuple.
- **The texts this stage's code changed** are restated:
  - `tuple_verify.hpp` and `trail_replay.hpp`'s "nothing can";
  - `catalog.hpp`'s "Pages are NOT reclaimed";
  - the drop's log line, now "its pages are reclaimed at the next mount".

**Cells** (`tests/drop_table_reclaim_test.cpp`):
- **BF-S1's five red cells are green.** `Settle` drives the reclaim tick,
  where BF-S1 left a stated no-op. In the leak cell, five rounds now grow
  the volume by less than one round's relation.
- **The gate at `cores = 2`:** 32 steps after a two-core mount free
  nothing, and a crash in the wait mounts.
- **Cut anywhere:**
  - A relation of two levels, with a var-heap chain and an index, is
    dropped beside a live relation.
  - Every point a crash can cut the reclaim is taken as a crash image:
    after each free, after each sync, and on each side of the clear.
  - Each image is mounted, and its mount finishes the reclaim: every page
    of the ledger is free, the live relation is whole, nothing is refused,
    and the index's pages are freed too.
  - The cell runs twice. The second pass also flushes the map at every
    free, as another core's checkpoint would.
- **Reuse before the clear:** the freed ids go to a new relation before
  the clear, and the crash comes after. The re-drive frees none of that
  relation's pages, and its rows are whole.
- **A relaxed drop crashed before its commit is durable** is a loser: its
  word is restored, nothing is owed, and the relation is whole. The cell
  asserts that recovery found the loser.
- **A heap relation's chain** is walked from its anchor and freed (BF-Q8
  (a): one cell, no crash matrix).
- **`StillOwned`** has no cell that reaches it here: no statement of a
  mount's run can reach a mount-found tombstone. BF-S5's in-run cells are
  its killers.

**Suite:** 3245 of 3245, plain, and 3245 of 3245 with
`KDS_TEST_PAGE_LATCH=1`, which skips its usual three cells. BF-S1's five red
cells are green. The waystone, index, cabin and inner-build contract suites
are among the 3245, unchanged and passing.

**Mutations**, each built and run three times against BF's cells:

| mutation | killed by |
|---|---|
| the gate removed | the `cores = 2` gate cell |
| the owner check removed (anchor, tree and chain) | the reuse-before-the-clear cell |
| the tombstone cleared before the anchor's sync | the cut cell, at "after the clear" |
| a chain freed head-first, a sync a page | the cut cell |
| a chain freed head-first, one sync at its end | the cut cell's map-flushed pass |
| the verifier's owner check removed | BF-S1's verifier cell |
| `ReadTuple`'s bound removed | BF-S1's `ReadTuple` cell |
| the bind's root check removed | **survives here**, as stated: no statement of a mount's run reaches a mount-found tombstone; BF-S5's in-run cells are its killers |

**Not expressed as a mutation:**
- **"The free run inside the drop".** Nothing in the drop frees, so there
  is no line to move. The guards that would catch such a free are
  `ARolledBackDropLeavesTheRelationWholeAcrossARestart` and the
  relaxed-drop cell, because a drop that freed its own pages would leave a
  rollback or a loser over freed pages.
- **"A tree freed in one pass"** would survive every cell here, and the
  reason is stated rather than hidden. On a volume of one map region, a map
  flush writes the region's one page whole, and the plan frees children
  before parents, so every flush lands a prefix of the plan. The per-level
  syncs matter where a tree spans regions - a volume past 65,280 pages -
  and no cell builds one.

**Review:** (`critics-developer`, one pass).
**Correctness:** sound. The gate, the walk - index oids
discriminated by class, a missing var-heap, a re-drive's freed and reused
pages, a reused anchor read as done - and the clear's stamp were each
checked against the code.
**Fixed by the review:**
- a data race: `allocation_counters()` read `allocated_pages_` with no
  latch, and `SHOW META` runs on any core; it is now read with the other two
  under one map hold;
- six comments the diff made false.

**Taken as CLA proposed under §23:**
- **BF-R10's walk check** is one root check per bind (`StillOwned` in the
  step VM's `Bind`). The relation's anchor and its first page must still
  carry its oid; otherwise the statement answers `NotFound`, "its page …
  was reclaimed", rather than reading another relation's rows. That is
  where every descent and walk starts. A per-page owner check inside the
  btree descent, the chain walks and the var-heap fetch is **not built**:
  each would take the relation's oid through an interface that does not
  carry it today. Those reads are reached only from a bind this check has
  passed, and from the verifier, which checks the owner itself.
- **A refused fault** (`ResourceExhausted`, BE-R4) defers the walk, as
  BF-R3 says, instead of refusing the job.
- **The cut cell runs twice**, the second pass flushing the map at every
  free, so a chain freed head-first with one sync at its end is caught.
- **The reuse cell asserts `reclaim_skipped > 0`**, so the owner check is
  exercised whatever ids the allocator hands out.
- **The relaxed-drop cell** is rewritten to log the drop before its
  unflushed relaxed commit, and asserts the loser it recovers. As first
  written it never put the drop in the log, and so tested nothing.
- The `by_link` test is kept in one place, and the heap root no longer
  recurses.

**Declined:**
- dropping the `kClear` and `kRefused` phases and making `Step` return
  `void`: a refused job stays in `jobs()` so a later step can read its
  state, and `Step`'s status is what BF-S5's inbox drain returns through;
- moving `ReclaimCounters` to a header of its own, which buys nothing at
  four counters;
- ending a step after each chain sync to spare core 0 the fsyncs. A chain
  page costs a sync by BF-Q18 (c), and the batch bound already caps a step
  at 64 of them.

**Recorded rather than fixed:**
- the cell gaps BF-R12 names that later stages own: a crash at `cores = 2`
  once the gate has passed (BF-S4's two-core serving cell), a parked
  `FlushMaps` (BF-S2's barrier cell is the mechanism's), and the Waystone
  directory (BF-S2's zero-write and zero-slot cells);
- the equal-sort-keys index bug, should it make two descents reach one
  child, would refuse the reclaim - never free twice - which BF-S6's
  known-gaps entry says;
- `reclaim_refused` is the fourth reclaim counter beside BF-R11's three.

**Overhead not measured;** BF-Q15 measures it at BF's close.

### BF-S4 - the sim and the rigs - built 2026-10-08

- **Where:** on `worktree-drop-table-page-reclaim` from `f41ac53d`.

**The sim (BF-R12):**
- **A `drop-table` op.** It runs inside a transaction (3% of its ops) and
  out of one (2% of the stream). It is drawn from a stream of its own
  (`drop_rng_`), so every other op of a seed is the op it always drew. It
  never drops the last live relation and takes at most two relations an
  iteration. A committed drop's name is never created again; a fresh name
  takes its place, so the next mount's reclaim has pages a later creation
  reuses.
- **The oracle forgets a relation at its committed drop.** An autocommit
  drop and a transaction's `COMMIT` both remove it from the checked set,
  and the oracle remembers what the relation held. A rollback keeps it. A
  drop answered with an error is unknown either way.
- **Reconciling a dropped relation.** It stays gone on a clean stop, and
  in every mode once a `SYNC` followed it. Without that `SYNC`, a crash may
  bring it back, because a drop is acknowledged before it is durable; then
  it holds rows it held, or rows the oracle has as unknown.
- **One anchor object per boot**, so `D` behaves as production's
  (`sim/instance.cpp`).
- **The mount's reclaim** is collected after the completion checkpoint and
  stepped once after every op, as core 0's tick runs it. A seed's crash can
  therefore land in the middle of a reclaim, and a creation can reuse a
  freed id before the clear. A reboot settles it before the harness reads.
- **The census** (`ReconcileDrops`):
  - **The ledger.** Every committed drop is ledgered at its commit with
    every page its roots reach and that page's owner, using the reclaim's
    own walk, run read-only (`PageReclaimer::ReachFrom`).
  - **An owner scan, independent of that walk.** After the reboot's
    reclaim, no allocated page may still carry a reclaimed relation's oid,
    or one of its indexes', unless a crash undid the drop. A ledgered page
    fails in every mode. An unledgered one fails a fault-free run, and is
    counted `census_leaks` under faults, where a failed growth or split may
    leave it.
  - **Nothing is left pending,** and nothing is refused.
  - **Live relations.** Every live relation's walk skips no page - a
    `NotFound` link included - and no page is reached from two of them.

**What the sim found, and what was fixed:**
- **The gate was the wrong LSN.** It had been the log's append point after
  recovery's undo. A loser's compensations stamp pages, and their recLSNs
  hold the completion checkpoint's redo start below that point, so the
  reclaim waited for a checkpoint that a mount runs only on its cadence: 15
  of 190 sim runs ended still owing pages. BF-R4 names the right bound, the
  end of the recovery scan (`MountRecovery::scan_end`), and that is the gate
  now. No compensation names a dropped relation's page, because the drop's
  `X` waited out every writer of the relation.
- **A create that fails after its claim kept its bit.** An injected growth
  refusal after the cursor's claim left an id allocated with no page ever
  written there, which reads "allocated but never written" for good.
  `ReleaseClaim` now clears the bit and restores the count. It puts a
  popped id back on the list, steps the cursor back if nothing claimed
  after it, and otherwise lists the id. The order predates BF; the sim's
  new drop stream reached it. Its cell is
  `ACreateThatFailsAfterItsClaimGivesTheClaimBack`.
- `sim_loop_test`'s pinned `ops_run` for one deterministic case moved from
  114 to 115: one drop op in that seed's stream.

**The rigs:**
- **A free on one thread against creations on another** (`page_free_test`):
  400 frees racing 400 creations. Every created id is unique and live,
  every free lands, and the allocated count closes.
- **A two-core mount's reclaim while serving**
  (`DropTableReclaimServingTest`):
  - A started instance at `cores = 2`, with a 100 ms cadence, takes
    writes through its text port until the tombstone it found is freed -
    once every core has checkpointed.
  - Every page of the dropped relation is free after the stop, and every
    row written is there.
- **A miss parked between its `loading_` publish and its read, while its
  id is freed and popped**, is BF-S2's cell.

**`scripts/sim.sh`:** 190 runs, 0 failures.

**BF-S3's mutations, killed from the sim or a rig:**

Each was run against the sim (114 runs) and the two rigs: **all seven
survive BF-S4**, and each is killed by a BF-S3 cell instead. The sim's
crashes land at op boundaries, so no seed crashes inside a reclaim's
windows, and no statement in the sim races a bind against a free.

| mutation | why the sim and the rigs cannot kill it | killed by |
|---|---|---|
| the gate removed | a mount's reclaim runs after the completion checkpoint, and no replay or compensation names a dropped relation's page, because the drop's `X` waited out its writers | the `cores = 2` gate cell |
| the owner check removed | no seed crashes after a freed id is reused and before the clear, so no re-drive meets another relation's page | the reuse-before-the-clear cell |
| the tombstone cleared before the anchor's sync | no seed crashes between the clear and the sync | the cut cell, at "after the clear" |
| a chain freed head-first | no seed crashes inside a chain's free run, and a one-region map flush writes the whole region | the cut cell and its map-flushed pass |
| the verifier's owner check removed | the sim never verifies a page that another relation reuses | BF-S1's verifier cell |
| `ReadTuple`'s bound removed | no page in the sim holds a slot that leaves the page | BF-S1's `ReadTuple` cell |
| the bind's root check removed | no statement of a mount's run reaches a tombstone that the mount found | BF-S5's in-run cells |

**Review:** (`critics-developer`, one pass).
**Fixed by the review:**
- **B1:** a transaction's drop oids outlived a transaction the loop rolled
  back - a poisoned statement, or an errored `COMMIT`/`ROLLBACK` - so a
  later `COMMIT` ledgered a drop that never happened.
- **B2:** an errored `COMMIT` kept its drops as live relations, though it
  may have committed. They are now indeterminate drops, and the oracle's
  three copies of the forgetting share one `Forget()`.
- **B3:** the census walk passed over a page that read `NotFound` - right
  for a re-drive, wrong for a census, which exists to find a live relation
  linking to a freed page. A census walk counts it now, and a walk the pool
  refused returns `ResourceExhausted` instead of a partial result.

**Taken as CLA proposed under §23:**
- **The census checks itself independently.** Its ledger came from the
  reclaim's own walk, so a walk that missed a tree would have missed it in
  both. An owner scan of every allocated page now fails a fault-free run on
  any page still carrying a reclaimed relation's oid or one of its index
  oids that the ledger does not hold. Under faults such a page is a failed
  growth's or split's, and is counted `census_leaks`.
- **The sim steps the reclaim between ops**, as core 0's tick does,
  instead of running it to the end inside the boot. A seed's crash can land
  in the middle of a reclaim, and a creation can take a freed id before
  the clear. The reboot after a crash still settles the reclaim before
  anything is read.
- `ReleaseClaim` lists an id the cursor has already passed.
- The workload's one live-index helper, and `ReclaimCounters::Reset`.

**Declined:**
- holding a returned relation to its synced rows: only "rows it held" is
  checked, and the synced subset is a stronger claim than BF owes;
- returning (page, owner) pairs from `ReachFrom`: the ledger's second read
  costs a census, not a statement.

**Recorded rather than fixed:** a reclaim treats an allocated page that
was never written (`NotFound`) as already free, so its bit stays set - a
leak, not a wrong free. Such a page has no owner to check, and it is one of
DT1's stated leaks.

**Noted:**
- the oracle's "acknowledged before durable" is conservative for the
  sim's `kGroup`, whose DDL may already wait for durability. The lenient
  arm then goes unused, and costs nothing.
- **What the sim could not kill of BF-S3's mutations** is in the table
  above, with its reason. Those mutations' killers are BF-S3's crash and
  defence cells.

**Suite:** 3248 of 3248, plain, and 3248 of 3248 with `KDS_TEST_PAGE_LATCH=1`.

**Overhead not measured;** BF-Q15 measures it at BF's close.
