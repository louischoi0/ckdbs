# Work order BB — the id fixed under the leaf it lands on: defect A closed at its source

Written 2026-10-06 on `worktree-bb-s0-order` from `bddd450c`
(`v2.7.0-622-gbddd450c`), on the operator's *"(b)로 원천 차단하는 방식으로
진행하려고 해 일단 지금까지 마일스톤 작업은 잠시 중지하고 이 오류를 해결하기
위한 서브 마일스톤 작업 계획 & 지시서를 작성해줘"*
(`raft-marks-2026-10-06.md` §4).

**A sub-milestone of BA, and BA is paused for it.** BA-Q14 is marked (b): the
id is issued under the hold of the leaf it lands on. BB replaces BA-S1b. BA's
remaining stages wait until BB closes, and BB-S5 is where BA resumes.

**Opened 2026-10-06** (`raft-marks-2026-10-06.md` §5):

- BB-Q0..Q7 are marked as CLA proposed them.
- **BB-Q8 is marked (b): `kUnordered` is deleted.** No relation is ever out of
  key order, and a named key below the mark is refused on a btree as on a
  heap.
- BB-Q9..Q11 follow from that mark. **BB-Q9 is marked (a)** the same day
  (`raft-marks-2026-10-06.md` §6): such a volume is refused. **BB-Q10 and
  BB-Q11 are marked (a)**, as proposed (§7).

**Started whole 2026-10-06** (`raft-marks-2026-10-06.md` §7), on the
operator's *"BB 마일스톤의 지시서를 읽고 진행해줘, 발생한 이슈에 대해서는 CLA의
제안에 따름, btree가 항상 정렬 되고 파괴적인 오버헤드를 동반하지 않아야해. 해당
마일스톤을 완성해줘"*: every stage runs, S1 through S5, on
`worktree-bb-issue-under-the-leaf`. A question BB raises is settled by CLA's
proposal and recorded at the stage that raised it. The close carries a
constraint sharper than BB-Q7: no cost BB-R8 resolves is landed as a deferral
(§6). BB cuts no tag.

## 0. What BB is

**Defect A** (`docs/inflight/bugs/order-by-pk-is-elided-over-a-btree-leaf-two-cores-filled-out-of-order.md`):
two cores inserting into one btree relation can leave a leaf whose slot order
is not its key order. The relation still claims `kAscending`, so `ORDER BY
<pk>` is discarded and the rows come back out of order. Under `LIMIT` the
result can be a different set of rows. This is a quiet wrong answer.

BA-R1b offered two fixes:

- **(a)** Flip `key_order` when the misorder happens.
- **(b)** Remove the misorder.

The operator chose (b), after asking two things:

- **How wide does (a)'s flag reach?** The whole relation, for good, although
  the misorder is confined to one leaf.
- **Which paths produce the misorder?** Two do: concurrent omitted-pk
  inserts (§1.2), and a named key at or above the mark raced against either
  kind (§1.3, found while writing this order).

The operator then marked BB-Q8 (b). That removes the third source of
out-of-order pages: a named key below the mark, which until now a btree
admitted and recorded by flipping the relation to `kUnordered`.

**BB's rules:**

- **BB-R1.** A row's id is fixed under the exclusive hold of the page the
  row lands on. This covers an issued id and a named key at or above the
  mark.
- **BB-R3.** A named key below the mark is refused, on every relation.

Together they make one invariant: **every page's slot order is its key
order, on every relation, at every core count.** Three readers already
assume it (§1.10). BB makes it true, and BB-R10 deletes the state that
existed for its exceptions.

**BB does not do:**

- **Make inserts faster.** BB moves work inside the rightmost leaf's hold
  (§1.9). BA-R8's cursor (BA-S11) is where that cost is taken back.
- **Touch the system relations** beyond recording one finding (§1.8, BB-Q5).

## 1. Survey at `bddd450c`

Read, not run, and every line number is at `bddd450c`. BB-S0's review re-read
§1.1-§1.12 at `eeeff079`, which changes documents only. §1.13 was read after
BB-Q8's mark.

### 1.1 Issue and placement are two latched spans

An `INSERT` row runs these steps in `CommandDispatcher`
(`src/server/command_dispatcher.cpp`):

1. **The id.** Either issued by `catalog_.AllocateRowId(oid)` (`:5106`), or,
   for a named key, admitted by `catalog_.AdmitExplicitRowId` (`:5087`).
   Both work under the hold of page 7's `sys.tables` row
   (`catalog.cpp:2454`, `:2502`), and that hold is released on return.
2. **The tuple borrow**, keyed by the id: `BorrowOrWait` (`:5111`; for a
   named key, inside the admit's `before_mark` hook).
3. **The encode**: `EncodeRow` writes the id into the Keystone word, and
   spills long values to the var-heap. Each spill is logged under its
   var-heap page's hold (`:5121-5128`, `SpillLogFor` at `:4175`).
4. **The placement**: `InsertIntoRelation` (`:5134`, `:5313`). For a btree,
   `BtreeInsert` descends, takes the leaf exclusive, checks duplicates and
   appends at the slot count (`btree.cpp:875-935`).

So between step 1 and step 4, another core can fix a higher id and place it
first. The leaf appends whatever arrives (`heap_page.cpp:162-163`), so the
leaf then holds the higher id in the lower slot.

Only these two paths place a user row: `InsertOneRow` and
`SortedFillInner`. There is no `INSERT ... SELECT` (`ast.hpp:485-490`), and
nothing else issues an id for a user row.

### 1.2 It is not a narrow interleaving

Two cores inserting omitted pks into one relation both descend to the
rightmost leaf, and whichever takes its latch first places first. The id
order was fixed earlier, at page 7. So at every collision the latch, not the
issue, decides the order. How often that misorders is unmeasured. Scenario
0's `cores = 8` cells run exactly this shape.

### 1.3 A named key at or above the mark has the same window

This is new: neither the bug entry nor BA-R1b names it. `AdmitExplicitRowId`
moves the mark to `k + 1` under page 7 (`catalog.cpp:2595-2610`), and the
row is placed later (step 4). The same race follows:

- core A names `k`, and the mark becomes `k + 1`;
- core B omits its pk and is issued `k + 1`;
- core B places first.

Two named keys at or above the mark race the same way.

### 1.4 What the borrow and the encode need, and when a wait survives

- **The borrow needs the id**: it is the lock key.
- **The borrow never parks inline.** `BorrowOrWait` returns a refusal and
  records the wait (`:8360-8400`). The statement then ends, `DispatchAsync`
  parks, and the statement re-runs, drawing a fresh id (the comment at
  `:5102-5105`).
- **A recorded wait survives only while the statement's trail is
  unchanged.**
  - `EndWrite` drops the wait once the trail has grown (`:8520-8536`), and
    an explicit transaction is then poisoned (`:8565`).
  - A spill is a trail entry (`SpillLogFor` → `NoteVarHeapAppend`,
    `manager.cpp:255-277`).
  - So **the borrow must precede the encode**. Otherwise a spilled row
    refused by a fence becomes `TXN_CONFLICT` where today it waits
    (`lock_family_test.cpp:1105`). A fence is any open `UPDATE` or `DELETE`
    whose declared range covers future ids (`DeclaredWriteBorrow`, `:8248`).
- **The encode under a leaf's hold is already a taken order.**
  - `UPDATE` re-encodes with its spills under the var-heap page's hold,
    inside the leaf's (`:7414-7416`).
  - The undo append already runs under the insert's leaf hold (BA §1.9).
- **A named key's borrow can precede its admission.**
  - The `before_mark` hook sits between the admit's judgement and the mark's
    move (`:5078-5086`). It is there so that a re-run never finds its own
    key below a mark its first attempt advanced.
  - A borrow taken before the admission advances nothing, so that reason
    does not apply to it.

### 1.5 The descent can already hold the leaf an omitted pk lands on

- **The rightmost leaf.**
  - `DescendTo` takes the leaf exclusive and checks that it still covers the
    key (`btree.cpp:160-200`). It restarts at most `kMaxDescentRestarts`
    times and then refuses `TxnConflict` (`:232`).
  - Descending for `kMaxKeystoneId` can return only a leaf with no right
    sibling (`LeafStillCoversKey`, `btree.cpp:100-113`), held exclusive.
  - No splice can pass that leaf while it is held: every splice writes the
    leaf it splits.
- **A stale root.** A stale root is one another core grew a level over.
  - It routes the descent to a leaf that has a right sibling. The restarts
    exhaust and the insert is refused `TxnConflict` (`:232`).
  - A split that would grow over it is refused by the grown-over mark
    (`:545-551`).
  - It is never misrouted.
- **The issued id belongs there.**
  - `next_id` is above every placed id.
  - Every leaf's `min_key` is 0 (the root leaf, `btree.cpp:869`) or an id
    once placed (a rolled-back append keeps its leaf).
  - An append split creates its new leaf with `min_key = id` for the id it
    places (`btree.cpp:990`).
  - So `next_id` is at or above the rightmost leaf's `min_key`. The existing
    invariant-3 check (`btree.cpp:899-904`) and the duplicate scan both pass
    it.
- **And a leaf with a right sibling holds only keys below the mark.** Its
  right sibling's `min_key` is a placed id, and every placed id is below
  `next_id`. BB-R3 uses this.
- **A full leaf.** The append split runs under the same hold, with
  `SecureParents` first (`:946-962`). It needs the id before it creates the
  leaf, and issuing under the hold gives it the id in time.
- **The middle divide.** `SplitLeafAndInsert` (`:966`) runs only for an id
  below its leaf's highest. After BB-R1 and BB-R3, SQL cannot reach it. It
  stays reachable through `BtreeInsert`'s own contract, which the storage
  tests drive directly.

### 1.6 Latch order today, and the one BB adds

- **The page latch is outer to the WAL stream latch** (`rules.md` §3).
- **Page 7's latch is already asked under a user relation's page**, shared,
  on a catalog-cache miss.
  - `DELETE`'s reverse foreign-key check runs inside the walk's write hold
    of the parent page (`:8830`, `:8888`).
  - It calls `InitTableAccess` per child (`:3590`), whose miss reads
    `sys.tables` through `ScanAll` (`catalog.cpp:2196`, `:1379`).
  - That is BB's direction, so it is a precedent and not an inversion. Page
    7 **exclusive** under a user page is what BA-R1b named as (b)'s cost.
- **The lock table's stated order is already false in the tree.**
  - `txn.md` §5 (`:867`) and `lock_table.hpp:136-151` (AR2-R2) say a
    partition latch is taken *"with no page latch held"*.
  - `UPDATE`'s and `DELETE`'s per-row tuple borrows run inside the walk's
    write page hold (`:7273`, `:8864`).
  - What holds is AR2-R2's intent, *"a lock wait can never park a latched
    page"*: `BorrowChain` never parks.
- **Where page pairs are declared.** `rules.md` §3 points to the lock
  order's spec rather than repeating it (`rules.md:31`). Page-against-page
  pairs live in `page.md` §6 and `device_page_store.hpp:200-238`.
- **BA-R8 goes the other way.** Its last bullet moves the `before_mark`
  hook's partition latches out from under page 7. BB-R3 removes the hook
  (§1.4), which settles that bullet.
- **BB-S1's proof obligation.** Which paths hold a `sys.tables` chain page
  and then ask for a user relation page's latch, directly or through a third
  latch? A page created under the hold does not count: no other core can
  reach it. Starting points:
  - `RegisterPattern` nests page 7 under page 9 (`catalog.cpp:2795-2800`),
    against CT7's text;
  - every name-taking DDL holds its catalog relation's root page across the
    check and the write (`catalog.md` CT7);
  - `index_ddl.cpp:294` issues before any page is walked;
  - `Catalog::RecordAccess` holds `sys.access_stats`' root.

### 1.7 Heap relations

A heap relation is creatable only before SUS-1, so it exists only on an
older volume. It has the same window, and two defects beside it.

- **Placement.**
  - `ChainInsert` (`heap_chain.cpp:61`) walks to the tail holding nothing
    (`ChainTail`). It then takes that page exclusive and does not re-check
    that it is still the tail (`:82-93`, `:166-168`). So the page it holds
    and places into can be a former tail.
  - Two cores growing the chain at once can overwrite the link and orphan a
    page (`docs/inflight/bugs/two-cores-growing-one-heap-chain-can-orphan-a-page.md`).
    That bug is not fixed, and it is a quiet wrong answer.
  - The duplicate check reads the held page alone, which is sound only while
    ids ascend across pages.
- **The race across a tail-page boundary** ends in `OutOfRange`
  (`heap-and-tuple.md:250-258`).
- **The race inside one tail page** misorders. A heap relation cannot be
  `kUnordered`, so `ORDER BY <pk>` is always dropped over it.
- **The sorted fill.**
  - It carves a block of ids and fills from the current tail, then fresh
    pages (`heap_chain.cpp:205-258`).
  - It releases each page before it takes the next, and it links a fresh
    page before it fills it.
  - So another core can take that linked, unfilled page as its tail and
    place a higher id into it first.
  - The fill is heap-gated, and its gate excludes var-heap schemas
    (`:4762`). It is unreachable for every relation created since SUS-1.

### 1.8 System relations

Six sites issue a system relation's id, and only one of them places a
Keystone-carrying row.

- **Five issue a field, not a key:** `catalog.cpp:2800` (`sys.patterns`),
  `:3067` (`sys.cabins`), `:3183` (`sys.fkeys`), and `:3405` and
  `index_ddl.cpp:294` (`sys.indexes`).
  - The issued value becomes a **field** of a catalog row.
  - The row is placed by `InsertRow` (`catalog.cpp:430`), which appends to a
    `min_key = 0` chain whose rows carry no Keystone word
    (`catalog.cpp:415-421`).
  - There is no key order there to break.
- **One issues a key: `sys.assertions`.**
  - `assertion_catalog.cpp:493` issues the id before the build.
  - After the build, `InsertAssertion` places it with `EncodeRow` and
    `ChainInsert` (`:246-251`), under the root page's hold (`:214`, CT7).
  - So two concurrent `CREATE ASSERTION`s can place out of issue order.
    Across a tail-page boundary, the one issued first and placed second is
    refused `OutOfRange` after its build.

**Not read here:** whether any reader depends on `sys.assertions`' key
order.

### 1.9 What BB costs, and where BA takes it back

- **The rightmost leaf's hold widens** (P9, BA §1.9). The hold already
  covers the placement, the Cabin witness, index maintenance, the
  reservation, the undo append and the `kHeapInsert` append. For an omitted
  pk, BB adds:
  - the descent for `kMaxKeystoneId`, which replaces the descent for the id;
  - `AllocateRowId`'s walk of the `sys.tables` chain and its logged
    overwrite - a WAL append under page 7, nested in the leaf's hold;
  - the borrow's partition latch;
  - the encode, with every spill of the row.
- **A named key's hold grows by the admission alone.** Its borrow and encode
  stay before the descent (BB-R3).
- **What takes it back.** BA-R8's cursor (P5) makes the issue a `fetch_add`,
  asking page 7 only once per 32 ids.
  - Under BB, that `fetch_add` still runs inside the leaf's hold.
  - BA-S11's cell *"every leaf in key order"* becomes an invariant BB has
    already established.
- **`cores = 1`.** One core cannot interleave, so BB reorders work there
  without adding any. Whether the reorder is free is measured, not assumed
  (BB-R8).

### 1.10 Who reads the premise BB restores

- **The `ORDER BY <pk>` elision** (`step_compiler.cpp:2040-2081`), the
  defect's visible face.
- **The Cabin serve's pk sort** (`step_vm.cpp:1595`). On a `kAscending`
  relation it sorts a served set by pk, to match the walk that recorded it.
  Defect A makes the two disagree.
- **The index step's pk sort** (`step_vm.cpp:1359-1371`), done *"because a
  scan of the same relation emits them in pk order"*. Defect A breaks that
  too.
- **Not affected:**
  - Range pruning is per page (`:1792`).
  - The index range stop works on sorted entries (`:1314-1316`).
  - That answers the bug entry's "Not checked".

### 1.11 A duplicate named key flips the relation for good, on one core

Found while writing this order. It is **not** a cross-core window.

- Below the mark, `AdmitExplicitRowId` flips `key_order` before the descent
  looks for the key (`catalog.cpp:2560-2568`).
- So an `INSERT` naming a pk that is already present - an ordinary client
  mistake - is refused `AlreadyExists` at the descent, and still leaves the
  relation `kUnordered` for good.
- It costs every later `ORDER BY <pk>` a per-page sort; it never gives a
  wrong answer.
- BB-Q8's mark removes the flip, and with it this defect.

### 1.12 Recorded, and settled by BB-Q8

**A peer's catalog memo can be stale across a flip.** This was speculative
and is not verified.

- A reader revalidates its memo at the statement boundary (`:772`).
- If another core flipped `key_order` and committed between that and the
  reader's compile, the reader could compile with `kAscending` and mint a
  view that covers the commit.
- No flip remains after BB-R10, so the question goes with the flag.

### 1.13 What deleting `kUnordered` reaches

Read after BB-Q8's mark, at `bddd450c`.

- **One writer.** `AdmitExplicitRowId`'s below-mark arm is the only code
  that sets the flag (`catalog.cpp:2560-2568`). It also publishes the flip:
  `CatalogCache::MarkKeysUnordered`, `++catalog_version_` and `BumpWord`
  (`:2610-2639`).
- **The readers to delete:**
  - the compiler's `emit_in_key_order` (`step_compiler.cpp:2075-2081`);
  - the walk's per-page emission (`step_vm.cpp:1830-1880`, `Step::emit_in_key_order`
    in `step_chain.hpp:500-509`);
  - the Cabin serve's `(page, slot)` branch (`step_vm.cpp:1595-1601`);
  - `TableAccess::key_order` (`schema.hpp:209-230`) and its fill
    (`catalog.cpp:2216`);
  - `DESCRIBE`'s `key_order=` (`command_dispatcher.cpp:2071`);
  - `KeyOrder` and `KeyOrderName` (`well_known.hpp:570-600`).
- **The byte.** `SysTableRow::key_order` sits at `kKeyOrderOffset`
  (`rows.cpp:58`, `:78-80`). It is the old key-mode byte, repurposed in 2026-08
  (`superblock.hpp:172-181`).
- **Only this version's volumes mount.** Superblock v19 refuses every older
  volume (AY-S8), and v18 refused every volume before it. So a `kUnordered`
  row on a mountable volume was written since 2026-09-30 by a btree that
  took a named key below its mark. The old `kExplicit` mapping cannot reach
  a mountable volume.
- **What clients lose (BB-Q8's cost):**
  - a named key in any order;
  - a bulk `VALUES` naming keys in any order;
  - a key naming a gap below the mark.
  A client that names keys must name each one above every key that has
  been placed or issued on the relation. A named key that another core's
  issue races past is refused, not reordered. That race is new, and it is
  the price of an ordered relation (BB-R3).
- **The tests.** `tests/supplied_key_test.cpp` exercises that capability by
  name; at least fourteen of its cells depend on it, from
  `ADescendingKeyIsAccepted` (`:209`) to `TheKeyOrderSurvivesAcrossDispatchers`
  (`:710`). Thirteen other test files mention the flag or the word.
  **How many cells elsewhere name a key below the mark incidentally is
  unmeasured.** BB-S3's first act counts them.
- **The text:**
  - invariant 11 and `heap-and-tuple.md` §4.1, §5 and §8;
  - `keystoneid-invariant.md`;
  - `index.md`, `cabin.md`, `join-inner-build.md` and `txn.md`, which cite
    the flag;
  - `client-manual.md` and `manual/sql/sql.md`;
  - `CLAUDE.md`'s caller-supplied-pk row and invariant 11, once built.

## 2. Rulings — CLA's proposals; R1-R9 marked with their items 2026-10-06

**BB-R1 — the rule.**

- A row's id is fixed under the exclusive hold of the page it lands on. That
  covers an issued id, and a named key admitted at or above the mark.
- Consequence, with BB-R3, stated in `heap-and-tuple.md` §4.1 and §4.1a:
  **every page's slot order is its key order, on every relation, at every
  core count.**
- The pk stays a sequence in issue order (invariant 11). Placement order now
  equals issue order.

**BB-R2 — an omitted pk on a btree relation, in this order.**

1. **Descend and hold.** `BtreeInsert` descends for `kMaxKeystoneId`, which
   lands on the rightmost leaf, held exclusive.
2. **Issue.** Under that hold it calls a callable the caller passes in, the
   shape `VarHeapSink`'s spill log already has. The storage layer has no
   catalog, and the id is needed before the hold ends. The existing
   invariant-3 check and duplicate scan then run on the id as today (§1.5).
3. **Borrow.** A refusal releases every hold and places nothing.
   - The id is burned, which K3 licenses.
   - Nothing has entered the trail, so the wait survives (§1.4), and
     `DispatchAsync` re-runs the statement as today.
4. **Encode**, under the hold, with the issued id.
   - Spills nest under the leaf as `UPDATE`'s do (§1.4).
   - No Keystone word is ever patched, and every spill's undo carries the
     row's pk.
5. **Place, and everything after**, exactly as today, under the same holds
   (AT-S21). An append split is included.

The storage seam is one callable, run under the hold, that returns either
the encoded row or a refusal. Steps 2-4 are the dispatcher's, inside it.

**BB-R3 — a named key, in this order (rewritten for BB-Q8).**

1. **Spellability**, as today: below `kFirstRowId` or above the Keystone
   field is `InvalidArgument`.
2. **Borrow**, outside any latch. It waits as today. The `before_mark` hook
   is removed on both relation types (§1.4).
3. **Encode**, outside any latch, as today.
4. **Descend for `k` and hold** the leaf it lands on.
5. **Present?** The duplicate scan reads that held leaf. If `k` is there,
   the insert is refused `AlreadyExists` (on BB-Q10).
6. **A right sibling?** Then `k` is below the mark (§1.5). The insert is
   refused `OutOfRange` (on BB-Q10), without reading page 7.
7. **The rightmost leaf.** Admit under page 7.
   - Below the mark: `OutOfRange`.
   - At or above it: move the mark to `k + 1` under the leaf's hold, which
     closes §1.3, and place.
8. **Everything after**, as today.

A refusal at step 5, 6 or 7 comes after step 3's spills. It is a refusal,
never a wait, and a refused statement unwinds as any refused insert does
today.

**BB-R4 — the latch order, declared where each half lives.**

- **The order:**
  1. the user relation page - the leaf, then any parents a split secured;
  2. the `sys.tables` chain page;
  3. the lock table's partition latch;
  4. the WAL stream latch.
- **User page → `sys.tables` page** is declared in `page.md` §6 and beside
  `device_page_store.hpp:200-238`, with a pointer from `catalog.md` CT7.
- **AR2-R2 is amended to its intent** (§1.6).
  - `txn.md` §5 and `lock_table.hpp`'s order comment say *"taken with no
    park under a page latch"* in place of *"with no page latch held"*.
  - That states what `UPDATE` and `DELETE` already do, and what BB-R2 step 3
    adds.
- **`rules.md` §3's rows** point at those homes.
- **The proof.** BB-S1's census shows that nothing takes the reverse.

**BB-R5 — under the hold, a refusal is a release, never a wait.**

- No code between the descent and the placement parks.
- It does wait on latches other cores hold, while holding the leaf:
  - the `sys.tables` chain pages and the lock table's partition latches,
    which BB-R4 orders;
  - the var-heap pages a spill writes, as `UPDATE` already does;
  - as today, the frame table and, on a split, the parents `SecureParents`
    takes.
- A descent that cannot hold its leaf after `kMaxDescentRestarts` restarts
  is refused `TxnConflict`, as today.
- **Nothing there begins a transaction, carves a transaction-id block, or
  flushes the store** (added at BB-S1, from its census). A `kWait` flush
  waits on another core's writeback claim, and that writeback can be waiting
  on the leaf this insert holds; `HeldFrames::kWait`'s premise - no flush
  caller holds a page latch - is what keeps the claim protocol sound, and
  nothing enforces it. The reachable instance is a transaction-id carve:
  `TransactionManager::Begin` and `MaybeBurnIdleBlock` persist the ceiling
  through `store_->Sync()`, a `kWait` flush. Every such caller sits outside
  any page hold today.

**BB-R6 — the invariant is checked, not only argued.**

- **The rig.** BB-S2's rig cells drive the interleaving directly. They are
  the only check that reaches it.
- **The sim.**
  - The sim's integrity sweep (`sim/integrity.cpp`) gains a check: on every
    relation, every page's live slots ascend by Keystone id.
  - The sim drives one session on core 0 through `Dispatch`
    (`sim/instance.hpp:7-19`, `sim/instance.cpp:218`).
  - So the sweep guards the single-core and recovery paths, including
    BB-R2's new order across a crash. It cannot reach a cross-core window.

**BB-R7 — a heap relation (on BB-Q2).**

- **The tail is held as the tail.**
  - `ChainInsert` re-checks `next_page_id == kInvalidPageId` under its
    exclusive hold, and walks on if another core grew the chain.
  - This is the orphan bug's fix (§1.7). Without it, there is no "tail's
    hold" to fix an id under.
  - The re-check covers the sorted fill's `ChainAppendBatch` too, which
    orphans rows the same way (`heap_chain.cpp:197-210`, link `:253`, at
    `6dc792c9`), and the held tail on a grow is the old one: `ChainInsert`
    returns the old tail's hold, not the new page's (`:171-172`).
- **The same orders as the btree arms, with the held tail standing for the
  leaf.**
  - An omitted pk takes BB-R2's order.
  - A named key takes BB-R3's order. "A right sibling" reads as "below the
    tail's `min_key`", which a heap refuses today.
- **The sorted fill** holds each fresh page from its creation through its
  fill, and links it only once filled - `ChainInsert`'s order. It carves
  under the hold of the tail it starts from. Its encode moves under the hold
  at no cost, since its gate excludes var-heap schemas.
- **What this removes:**
  - the in-page misorder;
  - the tail-boundary `OutOfRange`;
  - the orphaned page.

  Ids then ascend across a heap chain at every core count, and
  `heap-and-tuple.md:250-258` is restated.

**BB-R8 — the cost is measured once, at BB's close.**

- **The A/B.** `ck-tester`, `build-release`, interleaved, with A =
  `bddd450c` and B = the close.
- **The cells:**
  - an omitted-pk single-row `INSERT` at `cores = 1`, with and without a
    spilled value;
  - a named-key single-row `INSERT` at `cores = 1`;
  - an omitted-pk `INSERT` at `cores = 8` into one relation;
  - the same at `cores = 8` into eight relations, so P9's share can be told
    from page 7's;
  - an `ORDER BY <pk>` over a relation that was `kUnordered` at A, for what
    BB-R10 gives back.
- **If the cost is material,** BB still lands: it closes a quiet wrong
  answer. BA-S11 is then ordered ahead of BA's other fix stages, with BB's
  number as its premise.

**BB-R9 — system relations are out.** §1.8's `sys.assertions` finding and
its open question are recorded in `known-gaps.md`, so a later order can take
them. The other system relations carry no Keystone word and need nothing.

**BB-R10 — `kUnordered` is deleted (BB-Q8).**

- **What goes:**
  - the below-mark flip and its publication;
  - `MarkKeysUnordered`;
  - `TableAccess::key_order`;
  - `emit_in_key_order` and the walk's per-page emission;
  - the Cabin serve's `(page, slot)` branch;
  - `KeyOrder` and `KeyOrderName`.

  Everything in §1.13's list goes.
- **Why nothing breaks.** Each reader's ascending arm becomes the only arm.
  The elision's premise and the index step's sort rest on BB-R1 and BB-R3
  with no exception left.
- **What stays.**
  - The `sys.tables` byte stays at its offset, so no format moves. It is
    written 0 by every row, and its one reader is BB-R11's mount check (on
    BB-Q11).
  - The clustered tree's middle divide stays in the storage contract
    (§1.5). SQL cannot reach it.

**BB-R11 — a mounted volume that holds a `kUnordered` relation (BB-Q9,
marked (a)).**

- **Refuse the mount**, naming every such relation, with
  `Unsupported`: *"relation `t` holds keys out of order, a shape this engine
  no longer serves"*.
- **How.** Core 0 checks while it loads the catalog at mount. There is no
  superblock bump, so every v19 volume without such a relation still mounts.
- **Precedent.** A pre-M0 volume and a split relation were refused the same
  way (AM-S4(d), superblock 18).
- **Why this is a decision.** Reading the byte as ascending would serve
  those relations' `ORDER BY <pk>` wrong.
- **No way back for such a volume.** The operator gave up backward
  compatibility for it: no legacy path, no re-sort at mount. Its data is
  reached by an engine before BB, or not at all.

**BB-R12 — the code a refused named key gets (on BB-Q10).**

- **Proposed:**
  - `AlreadyExists` when the key is present;
  - `OutOfRange` when it is absent and below the mark.
- **Why.** BB-R3 reads the held leaf for both, at no extra cost. A client
  that detects duplicates by `AlreadyExists` keeps working.
- **The heap.** A heap relation keeps `OutOfRange` for both, as today. Its
  duplicate check reads the tail alone and cannot prove presence below it.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BB-S0 | **The order** | this file, its review, the marks recorded | S |
| BB-S1 | **The latch-order census** (BB-R4) | <ul><li>**The census.** A table, in §6, of every path that holds a `sys.tables` chain page and every path that takes a lock-table latch. Each row says what the path holds, what it then asks, and any cycle through a third latch, with file:line.</li><li>**The declaration.** BB-R4's order and AR2-R2's amendment, declared in their homes.</li><li>**No engine change.** An inversion found stops BB at this row, and the operator rules.</li></ul> | M |
| BB-S2 | **Red first** | <ul><li>**Red at BB-S1's commit.** Rig cells on a test seam between fixing the id and placing it:<ul><li>omitted pk against omitted pk: `ORDER BY id` returns 101 before 100;</li><li>the `LIMIT 1` shape returning the wrong row;</li><li>a named key at or above the mark against an omitted pk (§1.3).</li></ul></li><li>**Red on one core:** a named key below the mark admitted, and a duplicate named key leaving the relation `kUnordered` (§1.11).</li><li>**Guards, green now and kept green:** an omitted-pk insert and a named-key insert, each with a spilled value, wait for an open declared-range `UPDATE` rather than refuse (§1.4).</li><li>**The cells must not hang.** After BB-S3 the seam sits inside the hold, so the other core blocks on the leaf. A cell releases the seam before it waits on that core.</li></ul> | M |
| BB-S3 | **The btree arms** (BB-R1..R6, BB-R12) | <ul><li>**First, the count.** BB-R3's refusal applied, the full suite run, and the cells that name a key below the mark counted and listed in §6 before any is rewritten. A cell that tests the withdrawn capability becomes a refusal cell; one that names keys incidentally names them ascending.</li><li>**Cells.** BB-S2's btree cells green, and its guards still green.</li><li>**Suites.** The Keystone suites, and the waystone, index, cabin and inner-build contract suites, byte-for-byte green.</li><li>**Sim.** BB-R6's sweep check in the corpus, green.</li><li>**Mutations, each repeated:**<ul><li>the id issued before the descent (the race cell);</li><li>a named key admitted before its descent (the named race cell);</li><li>a below-mark key admitted (the below-mark cell);</li><li>the borrow's refusal under the hold ignored (a fence cell);</li><li>the omitted-pk encode before the borrow (the spilled guard).</li></ul></li></ul> | L |
| BB-S3b | **`kUnordered` deleted** (BB-R10, BB-R11, on BB-Q9 and BB-Q11) | <ul><li>**Code.** Everything §1.13 lists is deleted. BB-R11's mount check is in, with a cell: a volume carrying a `kUnordered` row is refused, naming the relation. The mutation that removes the check is killed.</li><li>**Text restated:** invariant 11, `heap-and-tuple.md` §4.1, §4.1a (`:256`), §5 and §8, `btree.cpp:943-945`, `keystoneid-invariant.md`, the specs that cite the flag, `client-manual.md`, `manual/sql/sql.md`, and `CLAUDE.md`'s caller-supplied-pk row and invariant 11.</li><li>**The suite green.**</li></ul> | M |
| BB-S4 | **The heap arm** (BB-R7, on BB-Q2) | <ul><li>**Cells.** BB-S2's cells re-run on a heap relation created behind the SUS-1 seam the heap suites use, plus a sorted-fill cell against a peer's insert.</li><li>**Gone:** the tail-boundary `OutOfRange` is no longer reachable by a race.</li><li>**Text.** `heap-and-tuple.md:250-258` restated.</li><li>**Bug entries.** Both deleted: defect A's and the orphaned page's.</li></ul> | M |
| BB-S5 | **The close** | <ul><li>**The measurement.** BB-R8's measurement, with its results file under `bench/v3.0.0/`.</li><li>**The rows.** A row per stage, and what BB carries.</li><li>**BA resumes**, with four rows rebased:<ul><li>BA-R8's last bullet - the hook is gone;</li><li>BA-R8's named-key bullet - a btree now refuses below the cursor too;</li><li>BA-R11's *"who it reaches"* - an omitted-pk row's spills now follow the descent, so a structural refusal leaves its trail unchanged;</li><li>BA-S11's row, on BB-R1.</li></ul></li></ul> | S |

## 4. Items for the operator

| item | what | class | CLA's proposal | mark |
|---|---|---|---|---|
| BB-Q0 | **Open BB** as BA's sub-milestone, with BA paused until BB-S5 | process | Yes | **as proposed**, 2026-10-06 |
| BB-Q1 | **Named keys at or above the mark** are in BB (§1.3, BB-R3) | user-visible, **[quiet-wrong] for "No"** | Yes. Leaving them out leaves defect A reachable by any client that names its pks | **as proposed** |
| BB-Q2 | **The heap arm** (BB-R7).<br>(a) The same rule under a re-checked tail's hold, with the sorted fill holding each page through its fill.<br>(b) No change to placement, and every heap `ORDER BY <pk>` emits per page in key order.<br>(c) Heap left as is, with the remainder recorded | user-visible, **[quiet-wrong] for (b) and (c)** | (a). It also fixes the orphaned page. (b) leaves that orphan and prices every heap relation; (c) keeps both wrong answers | **as proposed: (a)** |
| BB-Q3 | **An omitted pk encodes under the leaf's hold** (BB-R2 step 4). The alternative is to encode first and patch the Keystone word afterwards, which turns a spilled row's fence wait into a refusal (§1.4) | user-visible (latency under the hold) | Under the hold. `UPDATE` already takes that order, and the alternative changes what a client sees | **as proposed** |
| BB-Q4 | **The latch order** user page → `sys.tables` page → lock-table latch → WAL, and **AR2-R2 amended to its intent** (BB-R4) | invariant | Yes, conditional on BB-S1 finding no inversion | **as proposed**, with its condition |
| BB-Q5 | **System relations out of BB** (BB-R9) | scope | Yes, recorded in `known-gaps.md` | **as proposed** |
| BB-Q6 | **The sim integrity check** (BB-R6) | test | Yes | **as proposed** |
| BB-Q7 | **BB lands whatever BB-R8 measures.** A material cost moves BA-S11 to the front of BA's fix stages | process | Yes | **as proposed** |
| BB-Q8 | **What "원천 차단" covers.**<br>(a) The misorder only: `kUnordered` stays, set only by a named key below the mark.<br>(b) Also refuse a named key below the mark on a btree, so no relation is ever `kUnordered` | user-visible | (a) | **(b)**, the operator's word: *"kUnordered 자체를 삭제해야해"* |
| BB-Q9 | **A mounted volume holding a `kUnordered` relation** (BB-R11).<br>(a) Refuse the mount, naming each such relation.<br>(b) Keep a legacy per-page emission for those relations only.<br>(c) Re-sort them at mount.<br>(d) Read them as ascending | user-visible, **[quiet-wrong] for (d)** | (a). (b) keeps alive the machinery BB-Q8 deletes, and (c) rewrites leaves whose `(page, slot)` addresses undo records, indexes and Cabins hold | **(a)**, the operator's word: *"give up backward compatatibility, refuse it"* |
| BB-Q10 | **The code a refused named key gets on a btree** (BB-R12).<br>(a) `AlreadyExists` when present, `OutOfRange` when absent.<br>(b) `OutOfRange` always, as a heap | user-visible | (a). It is free, and duplicate detection keeps working | **as proposed: (a)**, 2026-10-06 (§7) |
| BB-Q11 | **`DESCRIBE`'s `key_order=` and the byte** (BB-R10).<br>(a) The field is removed with the state it reported; the byte is reserved, written 0, and read only by BB-R11's check.<br>(b) The field is kept, always `ascending` | user-visible | (a). A field that cannot vary reports nothing | **as proposed: (a)**, 2026-10-06 (§7) |

## 5. Sequencing

1. **BB-S1 before any engine change.** The order it declares is what BB-S3's
   code relies on. An inversion found there changes BB-R4, which is the
   operator's.
2. **BB-S2, then BB-S3, then BB-S3b, then BB-S4.**
   - S3b needs BB-Q9 and BB-Q11 marked (both are, §6), and deletes only what S3 has left
     unreachable.
   - S4 reuses S3's seam and its cells.
3. **BB-S5 closes BB, and BA resumes there.** BA-S2..S4's census then
   measures an engine whose rightmost-leaf hold BB widened. That is the
   engine BA's fix stages act on.
4. **File overlap.** BB-S3 and BA-S11 both touch `AllocateRowId` and the
   insert pipeline. BA-S11 is written against BB-S3's shape.

## 6. Row status

### BB-S0 — written 2026-10-06

Written on `worktree-bb-s0-order` from `bddd450c`, on the operator's word
above. §1 is read, not run. BA's order records the pause and BA-Q14's mark,
and the index carries both.

**The review**, of `eeeff079` by `critics-developer`, found three holes in
the first draft. All three were applied at `e58c3b73`.

- **An encode-first pipeline turned a spilled row's fence wait into a
  refusal**, because a recorded wait survives only while the trail is
  unchanged.
  - The omitted pk now encodes under the hold, after its borrow (BB-R2).
  - A named key borrows and encodes before its descent (BB-R3).
  - The Keystone patch and the `pk = 0` spill undo went with it, and BB-Q3
    was restated.
- **The new latch order contradicted AR2-R2 as written, and named the wrong
  homes.** The stated order is already false in the tree. BB-R4 amends it to
  its intent, and declares the page pair in `page.md` §6.
- **The heap arm held no tail.** `ChainInsert` does not re-check the tail,
  which is the orphan bug, and the sorted fill links a page before filling
  it. BB-R7 fixes both, and BB-Q2 is [quiet-wrong] for (b) too.

**Also from the review:**

- the factual corrections in §1.5-§1.8;
- §1.10's third reader;
- §1.11;
- §1.12;
- replaced mutants: *"the borrow taken before the hold"* could not be killed,
  and *"patch skipped"* went with the patch;
- the sim check moved from BB-S2 to BB-S3;
- the cells' hang warning;
- BB-R8's named-key cell;
- BB-Q1's [quiet-wrong] mark;
- BB-Q8, since the marks file had only read the operator's *"unordered된
  btree라면 존재 이유가 없잖아"* and had not asked.

**Rejected:** skipping the duplicate scan for an issued id (the review's
S4). It is a performance change with no defect behind it, and BA's census
can ask for it.

### BB opened, and §4 marked - 2026-10-06

On the operator's *"BB-Q8에서 kUnordered 자체를 삭제해야해. 이외에는 CLA
제안에 따름, main push"* (`raft-marks-2026-10-06.md` §5):

- **BB-Q0..Q7 are marked as proposed.**
- **BB-Q8 is marked (b)**, against CLA's proposal (a).

**What the mark changed in this order:**

- **BB-R3 is rewritten.**
  - A named key below the mark is refused on a btree too.
  - The leaf the key's descent holds answers both refusals: a right sibling
    means below the mark (§1.5), and the duplicate scan is that leaf's.
  - The `before_mark` hook goes on both relation types.
- **New sections and rulings:** §1.13 surveys what the deletion reaches.
  BB-R10 deletes the flag, BB-R11 handles volumes that still carry it, and
  BB-R12 picks the refusal's code.
- **BB-S3b is a new stage** for the deletion.
- **§1.11 and §1.12 settle with the flag.**

**Three new items follow from the mark: BB-Q9..Q11, unmarked.** BB-Q9 is
[quiet-wrong] for (d).

### BB-Q9 marked (a) - 2026-10-06

On the operator's *"BB-Q9: give up backward compatatibility, refuse it"*
(`raft-marks-2026-10-06.md` §6). A mounted volume that holds a `kUnordered`
relation is refused, naming each such relation, and nothing reads that
relation's leaves as ordered or re-sorts them. BB-R11 states it as marked.
The scope - a check at catalog load, not a superblock bump refusing every
older volume - was put back to the operator and confirmed: *"그래 a 맞아"*.
BB-Q10 and BB-Q11 remain unmarked; BB-S3b waits on BB-Q11.

### BB started whole, BB-Q10 and BB-Q11 marked (a) - 2026-10-06

On the operator's *"BB 마일스톤의 지시서를 읽고 진행해줘, 발생한 이슈에 대해서는
CLA의 제안에 따름, btree가 항상 정렬 되고 파괴적인 오버헤드를 동반하지 않아야해.
해당 마일스톤을 완성해줘"* (`raft-marks-2026-10-06.md` §7), recorded on
`worktree-bb-issue-under-the-leaf` from `6dc792c9` (`v2.7.0-627-g6dc792c9`):

- **Every stage starts**, S1 through S5, in §5's order, each through the
  Session Workflow up to the push gate. No stage is pushed without its word.
- **BB-Q10 is (a)** and **BB-Q11 is (a)**, as proposed.
- **A question BB raises is settled by CLA's proposal**, recorded in the row
  of the stage that raised it with the alternatives it declined. BB-S1's
  *"an inversion found stops BB at this row, and the operator rules"* is read
  under that word: an inversion is resolved by CLA's proposal, and recorded.
- **The close's constraint.** *"btree가 항상 정렬 되고"* is BB-R1 and BB-R3's
  invariant as the goal. *"파괴적인 오버헤드를 동반하지 않아야해"* sharpens
  BB-Q7: at `cores = 1`, where BB reorders work and adds none (§1.9), BB is to
  cost nothing BB-R8's A/B resolves, and a cost it resolves is found and taken
  back inside BB. At `cores > 1`, a loss the widened hold causes is taken back
  inside BB where BB's rules allow it, or stated with its number and the
  proposal that takes it back - never landed as a bare deferral to BA-S11.

The baseline: the full Debug suite green at `6dc792c9` on this worktree,
`ctest -LE heap-suspended -j8`, 116.9 s, one cell disabled
(`CommandDispatcherTest.HeapSuspensionIsLifted`, as on `main`).

### BB-S1 — the latch-order census, built 2026-10-06

Built on `worktree-bb-issue-under-the-leaf` from `c566a4a4`, whose `src/`
and `include/` are `6dc792c9`'s; the census reads the tree at `6dc792c9`. No
engine change. **Labels**, used throughout: BB-R4's levels are (1) a user
relation page, (2) a `sys.tables` chain page, (3) a lock-table partition
latch, (4) the WAL stream latch; BB's edges are E1 leaf -> `sys.tables`
page (issue/admit), E2 leaf -> partition latch (borrow), E3 leaf -> var-heap
page (spill), E4 leaf -> parents (`SecureParents`).

- **Surveyed:** (A) every holder of a `sys.tables` chain page; (B) every taker of a lock-table partition latch or the wait-for latch; (C) every holder of a catalog page (pages 5-14, overflow, page 0) that then asks a user page; (D) cycles through a non-page latch; (E) var-heap, heap chain and index pages as holders, and btree page against page. Five separate surveys, every reverse candidate re-read adversarially, then a completeness pass (the critic) that re-grepped every page-7 touch, lock-table call site, `Latch`, `std::mutex` and blocking wait in `src/` and `include/` and found no reverse edge the five missed; its nine rows all agree or are not edges, and are folded in, marked (critic). Nothing built or run.
- **Tree:** read at `6dc792c9`; every line number is at that commit. B, the verify passes and the critic read at `c566a4a4`, whose `src/`, `include/` and `sim/` are `6dc792c9`'s. From `169072d7` (BB-S2's seam) on, lines after `command_dispatcher.cpp:5117` are one higher. A bare `:N` is in the last file named in the same cell, else in command_dispatcher.cpp.
- **Verdict: no inversion.** No holder of a level 2-4 latch, a var-heap page or an undo page asks a user relation page, directly or through a third latch. BB-Q4's condition is met and BB-R4 stands as proposed.
- **Declared:** the pair, user relation page then `sys.tables` chain page, with BB-R4's four levels, in `page.md` §6 - its one home - with pointers from `device_page_store.hpp`'s page-latch section, `catalog.md` CT7 and `rules.md` §3's page-latch and catalog-pages rows; AR2-R2 amended to its intent - a partition latch is taken with **no park under a page latch** - in `txn.md` §5, with pointers from `lock_table.hpp`'s order comment, `sched.md` §9-2, `rules.md` §3's lock-table row and AR2-R2's own text.
- **Found, not BB's:** a write walk's `WHERE` sub-chain reads leaves under the walk's exclusive leaf hold - a cross-core ABBA inside level (1) that predates BB and has no E1-E4 edge in it - so two `UPDATE`s or `DELETE`s on two cores can hang both reactors. Settled by CLA's proposal under §7's word: **recorded, not fixed in BB** - `docs/inflight/bugs/a-write-walks-subquery-reads-pages-under-its-exclusive-leaf-hold.md`, with its three fix candidates - and `rules.md` §3's page-latch row corrected, since it stated the opposite. BB-R4 orders no user page against another and points to `page.md` §6's owed order. BB-S5 carries it. BB-S1's review also recorded `docs/inflight/bugs/two-cores-growing-one-var-heap-chain-can-leak-a-page.md` (the var-heap row below), left out of BB: BB-R7 covers heap relations' chains only.
- **BB-R5 gains a bullet:** nothing between the descent and the placement begins a transaction, carves a transaction-id block or flushes the store. The census found the reachable instance - a transaction-id carve's `kWait` flush - outside every page hold today, and nothing enforcing it.
- **Stale:** the declared text the census contradicted is corrected with the declaration - the last table below.

#### Every path that holds a `sys.tables` chain page

| path | holds (file:line) | then asks (file:line) | through a third latch | verdict |
|---|---|---|---|---|
| AllocateRowId: omitted-pk INSERT (:5106); id issue for sys.indexes, cabins, fkeys, patterns, assertions (index_ddl.cpp:294, assertion_catalog.cpp:493, catalog.cpp:2800/:3067/:3183/:3405) | catalog.cpp:2456 -> ForFirstRow :176, X, one chain page at a time (:198-200) | OverwriteLogged :2484 -> WAL :242 (wal/manager.cpp:329); StampPageLsn catalog.cpp:244 | structure latch (device_page_store.cpp:1253, :2337); logger | agrees (2 -> 4). E1's asked half. No page is held at :5106 today. It scans every chain page X (catalog.cpp:154-156) |
| AllocateRowIdRange (sorted fill, :4821) | catalog.cpp:2415 -> :176, X | :2443 -> WAL :242; StampPageLsn :244 | structure latch | agrees (2 -> 4). Heap only, SUS-1-gated. The range borrow (:4852) comes after the release |
| AdmitExplicitRowId and its before_mark hook (named-key INSERT, :5087; the key_order flip) | catalog.cpp:2520 -> :176, X, on the row's page | hook :2558/:2594 -> command_dispatcher.cpp:5089-5096 -> :8360 -> :8129 -> TryAcquire :8187/:8238 -> lock_table.cpp:174; DropWake (command_dispatcher.cpp:8124); then catalog.cpp:2570/:2598 -> WAL | partition latch. NoteBlockingWriter :8405 and MemoIsCurrent :8209 are lock-free | agrees (2 -> 3, 2 -> 4). Never parks (catalog.hpp:691-699). BB-R3 deletes the hook |
| GetSysTableRow (ScanAll), the only shared reader | catalog.cpp:1379 -> :91 -> heap_chain.cpp:311, S, one page at a time (:289-298) | nothing; IsInFlight (instance_visibility.cpp:249) is lock-free | none. The view is null, so no window latch | not an edge |
| DELETE's reverse FK check, InitTableAccess miss (§1.6's precedent) | parent page X (:9009, :8986) -> :8888 -> :3590 -> catalog.cpp:2196 -> :1379, page 7 S | nothing. Released before catalog.cpp:2201 and the anchor (:2250) | partition latch before it (:8864, released); page 11 X after it (:3608) | agrees (1 -> 2) |
| UPDATE's self-referencing FK arm; WHERE sub-chains in UPDATE, DELETE, SELECT | user page X (:7611, :9009) or S, then page 7 S on a miss: :7328 -> :3470 -> catalog.cpp:2196/:1379, or :7230/:8803 -> step_vm.cpp:2735 -> :424 -> :285 -> :296 | nothing | none | agrees (1 -> 2). The self-referencing arm is dead code |
| SHOW CABIN_OPTIMIZER (critic) | view latch :3201, the latch expeditor.cpp:1903 takes | page 6 S (:3230 -> :3714); on a miss, page 7 S (:3235 -> catalog.cpp:2196 -> :1379), anchor S (:2250), pages 12/13/8 S; collector latch (optimizer_signals.cpp:97) | view latch, outermost | agrees: view latch before (2). Taken with no page or partition latch held |
| Cabin controller tick -> CreateCabin | view latch, expeditor.cpp:1903 | cabin_optimizer_exec.cpp:267 -> catalog.cpp:3038 (page 7 S) -> AllocateRowId :3067 (page 7 X, WAL) -> page 12 X :3075; cabin_optimizer_exec.cpp:292 | view latch, outermost | agrees: view latch before (2) |
| InsertRelationRow -> InsertRow (CreateTable catalog.cpp:1344; Bootstrap :633; BootstrapAssertions :797) | catalog.cpp:1135 -> :437, tail X (full pages released at :457) | undo hook :395 -> command_dispatcher.cpp:5605 -> :5627 -> manager.cpp:718 -> undo_log.cpp:154 (X :89, :104, :133, :173; WAL :66); catalog.cpp:404 -> WAL :242; on growth :463 -> CreateAt :359, :475, :501, FPI :504 | undo page (a sink); window latch on undo growth (undo_log.cpp:100 -> :73 -> manager.hpp:336-338 -> instance_visibility.cpp:325); structure and map latches (device_page_store.cpp:1094-1095) | agrees (2 -> 4). Nothing else is held at :1344: page 6 is scoped to catalog.cpp:1321-1341 |
| DropTable's sys.tables sweep (:2662) | catalog.cpp:2063 -> :1998 -> :2003 -> :176, X, one page at a time | undo hook :2026 -> undo page -> WAL; :2033 or :2014 -> WAL | undo page; window latch | agrees (2 -> 4). The caller holds the relation's X unit, and no page |
| RegisterPattern (trail_recorder.cpp:57 <- command_dispatcher.cpp:6763; C cites :6881) | page 9 X, catalog.cpp:2793 | :2796 (page 9, re-entrant); AllocateRowId :2800 -> page 7 X -> WAL; InsertRow :2823 | as AllocateRowId | BB-allowed catalog -> sys.tables. Page 9 ranks before page 7, as rules.md §3's catalog row already stated; RegisterPattern is outside CT7's table |
| InsertAssertion | page 14 X, assertion_catalog.cpp:214 | :216 -> ScanAssertions :99 -> OpenAssertions :51, a cache hit: :195 filled the entry before the hold; assertion_catalog.cpp:247, :250, WAL :257 | sys.assertions' own var-heap (catalog.cpp:790) | not an edge: no page 7 under page 14. The miss path (catalog.cpp:2196, page 7 S; :2316/:2335/:2357, pages 5/12/13/8 S) is not reached under the hold |
| Callers that hold nothing at page 7 | nothing | AllocateRowId: catalog.cpp:3067, :3183, :3405, index_ddl.cpp:294, assertion_catalog.cpp:493. GetSysTableRow: catalog.cpp:3453, catalog_view.cpp:60, command_dispatcher.cpp:1767, :2021, mount_recovery.cpp:166. InitTableAccess at bind: catalog.cpp:1739, :3156/:3158, :3312; command_dispatcher.cpp:5864 | none | not an edge |
| Other CT7 root holds | page 6: catalog.cpp:1328, :1814, :1863, :1608; page 5: :1651; page 8: :3433; page 12: :3075; page 11: :2950 | own chain, WAL, CreateAt | structure latch; window latch (check views) | not an edge: none of them asks page 7 |
| UpdateRelationDescPage, UpdateIndexRoot (:5260) | anchor X, catalog.cpp:1091 | WAL :1114 | none | not an edge: no sys.tables page is touched (catalog.cpp:2645-2657, :3567-3577) |
| Abort's compensation of a sys.tables row | manager.cpp:438, X | in place (:495, :512, :532), with no WAL (:491). On a mismatch it releases (:462) before :463 | none | not an edge. It runs at the decide, with no page held |
| Delete-mark purge and mount finalize (well_known.hpp:399-403) | catalog.cpp:940 -> heap_chain.cpp:302, X | catalog.cpp:958 -> manager.cpp:801; catalog.cpp:962; :970 -> WAL | window latch (instance_visibility.cpp:305), a leaf | agrees (2 -> 4). Callers: :5794 and expeditor.cpp:906 |
| Checkpoint writeback of page 7 | checkpointer.cpp:214 -> device_page_store.cpp:1824: the claims (:1482-1518), then page 7 S at :1537, released at :1581 | structure latch :1549. The WAL gate (:1607) comes after the release | writeback claim | not an edge: no page-7 X holder waits on a claim |
| Recovery, mount load, BootstrapAssertions, SHOW PAGE 7 | page 7 X (wal/redo.cpp) or S (mount_recovery.cpp:166, command_dispatcher.cpp:1856). BootstrapAssertions holds page 14 across catalog.cpp:790 and inserts into pages 6/7/5 | WAL, at mount | none | not an edge: the mount thread runs alone (device_page_store.hpp:166-170). BB-Q9's refusal is not built at 6dc792c9 |

#### Every path that takes a lock-table partition latch

| path | holds (file:line) | then asks (file:line) | through a third latch | verdict |
|---|---|---|---|---|
| AcquireInner's grant (TryAcquire, the engine's only ask) | the key's partition latch, lock_table.cpp:174 (null at cores=1, :68-:75) | No latch. Vector work :177-:314; atomics :190/:205/:221; RegisterWake :262 -> :642; slot store :281. Released at :315 | none. The old wait is withdrawn at :159, before :174 | not an edge |
| Verify: FenceCoversKey, ConflictingOverlap; the unwind's ReleaseOne | lock_table.cpp:614, :654, one partition at a time; :357 | RegisterWake :628/:701. verify_hook_ (:328, :351) runs unlatched | none | not an edge. Cost: a fenced relation (:347) scans 64 x cores partitions |
| ReleaseHeld -> WakeWaiters -> KickAll | lock_table.cpp:533 (dropped at :552), then :445 (dropped at :451) | erase and FlipSlot. KickAll :455 runs after release: waker_table.hpp:103-113, eventfd (waker.cpp:37) | the rig's SimWakerTable latch (sim_waker_table.cpp:27), outside any partition | not an edge |
| DequeueWaiter, DropWake | lock_table.cpp:480, :509 | erase; FlipSlot :492. KickAll :503 after release | none | not an edge |
| LockTable::Release | lock_table.cpp:588 -> :533, one partition at a time | DequeueWaiter :596; ClearWaitFor :599 -> wait_latch_ :421 (gated at :420), after the guards close | wait_latch_, never nested with a partition latch | not an edge |
| Wait-for graph | wait_latch_, lock_table.cpp:375, :421, :430 | vector walk | none. Its callers (command_dispatcher.cpp:375, :383, :440, :611, :613, :626) hold no page | not an edge |
| WaiterCount, EntryCount (critic) | lock_table.cpp:712, :722, one partition at a time | a vector scan | none | not an edge. Test callers only (lock_table.hpp:776/:782) |
| ReleaseIf (FK give-back) | lock_table.hpp:650 -> lock_table.cpp:533 | nothing. The predicate (lock_table.hpp:649; command_dispatcher.cpp:3538-3547) runs unlatched | none | not an edge |
| Write resolve, the read borrow's IS (:4622, :7036, :8745 -> read_borrow.hpp:75 -> :97 -> :139) | no page | lock_table.cpp:174 | none | not an edge |
| Compile-time bind (:6477-6479, :7070, :8761 -> step_compiler.cpp:1540) | no page (step_compiler.cpp:1533 has returned) | partition latch | none | not an edge |
| Step VM read walk (step_vm.cpp:1954, :2011 -> read_borrow.hpp:97/:125, :108/:127) | no page. :2011 runs after the leaf's PageRef dies (btree.cpp:1191-1192) | one partition at a time | none. Nested runners get no sink (step_vm.cpp:419, :2759) | not an edge |
| ReadBorrow destructor (read_borrow.hpp:80) | no page. The point arms' leaf (:7574, :8986) dies first | partitions, then wait_latch_ | none | not an edge |
| DDL relation X (:8096 from :2244, :2647, :2848) | no page. The CT7 hold comes later | partition latch, verify | none. Parks at :625 with no page | not an edge |
| INSERT's relation IX (:4669 -> :8365 -> :8187) | no page | partition latch. MemoIsCurrent (catalog.hpp:245-248) is atomic | none | not an edge |
| FK forward hoist (:3381, from :4714, :4785, :7128) | no page (:3361 has returned) | partition latch; CheckParentPresent (:3391) runs after the grant | none | not an edge |
| FK self-referencing arm from INSERT (:3466, :3537, from :5009, :4808) | no page | partition latch | none | not an edge. Dead code |
| FK self-referencing arm from UPDATE (:7328 -> :3466, :3537) | leaf X: btree.cpp:1191 via :7611-7612, btree.cpp:191 via :5891/:7574, or heap_chain.cpp:302 | lock_table.cpp:174 (:614 when fenced). Then a descent (:3472 -> fk_check.cpp:100) | none | agrees (1 -> 3). Dead code |
| Sorted fill's range (:4852) | no page. Page 7 was released at :4821 | partition latch; lock_table.cpp:654 | none | not an edge. BB-R7 moves it under the start tail: agrees (1 -> 3) |
| INSERT named key, today (:5087-5099 -> catalog.cpp:2558/:2594 -> :5091 -> :8187, :8238) | sys.tables page X (catalog.cpp:176) | partition latch (lock_table.cpp:174, even on the already-held exit at :188-194). On refusal: :8124, and :8400 -> manager.cpp:767 (lock-free) | none | agrees (2 -> 3). Never parks. BB-R3 deletes it |
| INSERT omitted pk, today (:5112) | no page. :5106 released page 7 | partition latch | none | not an edge today. Under E1 and E2, leaf X -> page 7 X -> partition: agrees (1 -> 2 -> 3) |
| UPDATE/DELETE declared unit (:7167, :8825) | no page | partition latch, then ConflictingOverlap | none | not an edge |
| UPDATE's per-row tuple X (:7273) | leaf X: btree.cpp:1191 via :7611-7612, btree.cpp:191 via :7574 -> :5891, or heap_chain.cpp:302 | lock_table.cpp:174 (:614 when fenced); then :8413 -> IsInFlight (lock-free) | none | agrees (1 -> 3). §1.6's precedent. The park is at :403, after release |
| DELETE's per-row tuple X (:8864) | leaf X: btree.cpp:1191 via :9009, or btree.cpp:191 via :8986 | lock_table.cpp:174 | none | agrees (1 -> 3). Never parks |
| DELETE's child-row wake (AY-Q8: :8888 -> :3631 -> :3668, :3671, :8124) | the parent leaf X. The child pages (:3601) are released | lock_table.cpp:174, :533, :509 | none | agrees (1 -> 3). TryAcquire, never parks |
| Statement-end waits and parks (:375-:440, :611-:632, :872, :8007, :8534) | no page. A mid-walk refusal has already stopped with kStop | a partition latch or wait_latch_, one at a time | none | not an edge. The only two parks are :403 and :625 |
| Decide release (manager.cpp:381, :604; manager.hpp:369) | no page, WAL or window latch (manager.cpp:320, :351 have returned) | partitions, then wait_latch_ | none | not an edge (txn.md:867-871) |
| Startup (expeditor.cpp:990, :1596) | nothing | nothing | none | not an edge |

#### Third latches and the other holders

| latch or holder | taken at (file:line) | held across, or asks (file:line) | asked under | closes a cycle with E1-E4? |
|---|---|---|---|---|
| Frame-table structure latch | device_page_store.cpp:1956, :2337-2338, :1253, :2427, :2351 | map latch :1095. It never waits for a page latch while held: it drops at :2075 before :2079, and at :2149 and :2195. :2008 releases it. The sweep only tests IsHeld (:2580, :827, :1805) | every page hold | No: innermost |
| Free-map latch | device_page_store.cpp:1095 under :1094; :1344 -> :357 | nothing | a page, through CreateNew (splits, chain growth, varheap.cpp:289/:349, assertion_build.cpp:164) | No: a leaf, with its order debug-asserted |
| Device grow lock | no line cited | nothing | page allocation | No: a leaf |
| WAL stream latch | wal/manager.cpp:329 -> wal/stream.cpp:152; ring-full drain manager.cpp:343 | segments_mutex_ only (file_log_device.hpp:112) | page 7 X, var-heap X, leaf X, undo page | No. stream.cpp has no page store, and redo holds no stream latch |
| WAL writer mutex, EnsureDurable | wal/writer.cpp:34 | nothing; the device Sync runs outside it (writer.cpp:76) | no page: device_page_store.cpp:1366, :1608 (after :1581); command_dispatcher.cpp:956/1245/4229 | No |
| Writeback claim (kWait) | device_page_store.cpp:1515 | page S at :1537, released at :1581; EnsureDurable :1608 | Only kWait waits on a claim (:1378 via :1525). Every caller holds no page: checkpointer.cpp:214, superblock_checkpoint_anchor.cpp:126, expeditor.cpp:1175, core_runtime.cpp:595, command_dispatcher.cpp:1252, expeditor.cpp:2045 | Only if a kWait flush ran under BB's hold. BB-R5 forbids it |
| Trx-id carve with a kWait Sync (critic) | trx_id.cpp:26 (superblock latch, closed before :49); Begin manager.cpp:155 (from command_dispatcher.cpp:2843, :7731, :7971); MaybeBurnIdleBlock manager.cpp:611/:630 | persist trx_id.cpp:49 -> expeditor.cpp:1173 -> :1161 -> page 0 X :1162 -> Sync :1175 -> device_page_store.cpp:1733 -> WriteBack(kWait): page S :1537, AwaitWritebackClaim :1525/:1378 | nothing: the callers hold no page latch | No: an outer waiter on the insert's frames. It is live every kTrxIdBlockSize begins and on idle burns |
| Background drain (kSkip) | device_page_store.cpp:1665 (claims :1515) | TryAcquire S :1538, never waits; skips claimed frames (:1488-1493) | nothing (expeditor.cpp:1928; :1699 has no caller) | No |
| CheckpointGate | checkpointer.cpp:323 | FlushPages :214, WAL, anchor publish :278 | nothing: a try-flag (checkpointer.hpp:244) | No |
| Superblock latch | superblock_checkpoint_anchor.cpp:89, expeditor.cpp:1161, trx_id.cpp:26 | page 0 X (superblock_checkpoint_anchor.cpp:105, expeditor.cpp:1162). Sync after release | no page | No: outer to page 0, a sink |
| Visibility window latch | instance_visibility.cpp:86/271/305/314/325 | memory; kicks after release (:99-103) | page latches: read_view.hpp:128; undo growth (undo_log.cpp:100 -> instance_visibility.cpp:325); the purge (catalog.cpp:958) | No: a leaf. It contradicts the declared "never nested" text |
| In-flight tables | manager.cpp:767 -> instance_visibility.cpp:249 | atomics | pages | No: not a latch |
| Partition latches, wait_latch_ | the table above | nothing | leaf X, page 7 X | No: leaves |
| Assertion directory latch | assertion_check.cpp:172, :431, :480, :500, :538, :588, :648, :681, :703, :766, :796 | WAL only (checkpointer.cpp:122; AdoptLogged; :500). Commit and Abort release before store.Get (:766-:784, :796 -> :823) | user page X | No. Its scopes close before ReserveOne |
| Assertion chain latch, Bound Cabin pages: UPDATE (AdmitAndReserveUpdate) | assertion_check.cpp:681, chain latch :464, under UPDATE's page X | cabin page X (assertion_build.cpp:90-113, :164) -> directory latch (assertion_check.cpp:500) -> WAL | the user page (assertion.md §6.1's order) | No: agrees |
| Same, INSERT's ReserveInsert (critic) | :5194, under placed.held (leaf X and split parents, from :5134, cleared at :4319) | assertion_check.cpp:538 (closed before :563) -> :563 -> :464 -> :480 -> Place :491 (assertion_build.cpp:95/:101/:158, CreateNew :134) -> assertion_check.cpp:500 -> WAL -> :520 | the leaf | No: agrees. Census D had it outside the hold; LogInsert (:5241) clears the holds after it |
| Same, DELETE's ReserveDelete (critic) | :8912, under the leaf X (:9009, :8986, or heap_chain.cpp:302) | assertion_check.cpp:675 -> :685 -> :464 -> :491 -> :500 -> WAL | the leaf | No: agrees |
| Cabin store mutexes, collector latch: UPDATE | cabin_store.cpp:134/:138; optimizer_signals.cpp:70-109 | memory. Signals fire outside (cabin_store.cpp:141-144) | UPDATE's page X (:7502) | No: leaves |
| Same, INSERT's Cabin witness (critic) | :5156 -> :6863 (re-entrant GetForRead on the held leaf) -> :6868 | cabin_store.cpp:134/:138 | placed.held | No: leaves |
| Optimizer view latch | expeditor.cpp:1903; :3201 (critic) | page 7 S/X, page 12 X, pages 6/12/13/8 S, user leaf S, var-heap S (cabin_optimizer_exec.cpp:140, :267, :292) | nothing: both takers start with no page held | No: outermost |
| Connection handoff inbox latch (critic) | connection_handoff.cpp:26, :42 | a queue push or drain | no page, partition or WAL latch | No |
| Catalog memo, schema word, wake registry, ceiling marker | catalog_cache.hpp:47; catalog.cpp:904; waker.cpp:28-37; manager.cpp:311-363 | nothing | anything | No: not latches |
| This core's undo pages | undo_log.cpp:89, :104, :133, :173; hook command_dispatcher.cpp:5605, fired at catalog.cpp:395/:1891/:1963/:2026/:3518 | WAL (undo_log.cpp:66/:179), structure and map latches, the window latch. Read copies and releases (:186-207) | catalog pages, var-heap pages, leaves | No: a sink, held X by its own core only (expeditor.hpp:817, core_runtime.hpp:586) |
| Same, INSERT's own kInsert record (critic) | :5221 -> manager.cpp:718 -> undo_log.cpp:165 | :89, :100 -> instance_visibility.cpp:325, undo_log.cpp:104/:133, :173, WAL :66 | placed.held | No: a sink |
| Page 11 under a user page: DELETE :3608; UPDATE's self-referencing arm :7328 -> :3475 (critic) | :3301 -> catalog.cpp:2950 (page 11 X) | :2957 own chain; InsertRow :3017, unlogged; CreateAt (catalog.cpp:357-360) | the walk's leaf X | No: page 11 asks no page 7, no user page and no partition latch |
| Var-heap pages | varheap.cpp:324 (S, one hop at a time), :331 (tail X); Fetch :382 via row_codec.cpp:1074 | own new page, forward (:349, :367, link :369); undo (:4182/:4238); WAL (wal_row_log.cpp:37/:50/:63/:66) | leaves, heap pages. Readers go user -> var-heap | No: E3 agrees (UPDATE :7418). Side finding: :331 re-checks no link, so a peer's page can be orphaned (a leak only) - docs/inflight/bugs/two-cores-growing-one-var-heap-chain-can-leak-a-page.md |
| ReleaseVarHeapSlot; mount sweep | varheap_release.cpp:15; varheap_sweep.cpp:30 -> :46 (the known self-upgrade bug) | WAL :44, :47 | nothing: Abort holds no page (manager.cpp:564), and the sweep runs only at mount (expeditor.cpp:938) | No |
| Heap chain pages | heap_chain.cpp:38, :90, :210; walks :311, ring device_page_store.cpp:881-882 | forward only (heap_chain.cpp:139/:243, :168/:253) | the write walk (:5405) | No: BB-R7's tail edges agree. Side finding: ChainAppendBatch has the unchecked tail (:197-203, :210, :253), and ChainInsert returns the old tail's hold (:171-172) |
| Secondary index pages | index_tree.cpp:164 -> :167 -> :81; divide :408 -> :247, bottom-up | index pages (:419/:494/:523), WAL (index_maintain.cpp:146-172; command_dispatcher.cpp:4150-4153, :4169, :4172) | the clustered leaf (:5174, :7509) | No. Probe (step_vm.cpp:1298, then :1390) and backfill (index_ddl.cpp:165, then :113-115) are two-phase |
| Anchor page | catalog.cpp:2250 S to :2373; X at :1091 | pages 12/13/8 S; WAL :1114 | a split's leaf (catalog.cpp:2663; index_maintain.cpp:195); CreateIndex at catalog.cpp:3458, after :3441 | No: anchor -> catalog agrees |
| Btree descent, parents, splits | btree.cpp:191 (leaf X); internal S per level (:176); SecureParents :953 -> :514 | right neighbour S (:203 -> :103; skipped at :102 only while the leaf has no right sibling); re-descent above the held levels (:504); CreateNew only (:392, :618, :639, :759, :985) | point UPDATE/DELETE (:1095), BtreeInsert (:891) | Not with E1-E4. The right-neighbour read is a level-1 member of the cycle below, and an insert whose leaf another core split before its X grant takes it. LogInsert's FPI of a split's new leaf (command_dispatcher.cpp:4283) is uncontended |
| Write walk WHERE sub-chains | btree.cpp:1192 (via :5510; :7611/:9009); heap_chain.cpp:302 (via :5405); btree.cpp:191 (via :5891-5892) | :7230/:8803 -> step_vm.cpp:2747-2766: any relation's leaves, S, either side | another such walk, a descent, or the reverse-FK walk | It reverses inside level (1). It is real, but it is not an E1-E4 edge |
| DELETE reverse FK under the parent | :8888 (parent X) | child walk (fk_check.cpp:407; the visitor body is :366-370); :218, :237; page 7 S :3590; partition :3640-3660; page 11 X :3608 | nothing asks the parent under a child | No. FK edges are acyclic; it closes a cycle only with a sub-chain |
| Other catalog holds (pages 5-14, 0) | catalog.cpp:1328, :1814, :1863, :1608, :1651, :1935/:2003, :2838, :2950, :3075, :3112, :3202, :3433, :3484, :940-941; assertion_catalog.cpp:285-286 | own chain, a created page, the undo page, WAL | nothing user-side | No (dimension C) |
| DDL builds, fault eviction, rollback | index_ddl.cpp:294/:377/:379/:385; assertion_catalog.cpp:493, :502-553; cabin_optimizer_exec.cpp:267/:140/:292; device_page_store.cpp:691-695 -> :2536; manager.cpp:438 | No catalog page is held across a build. Eviction skips latched frames (device_page_store.cpp:2580), and CreateAt latches no existing id (:1042-1100). Compensate releases at manager.cpp:462 before :463 | any hold | No |

#### Reverse candidates and how each was settled

- **Fault-path writeback under BB's hold - refuted.** InsertFrame's sweep (device_page_store.cpp:693) only queues dirty frames (:2592-2596), and WriteBack's callers (:1665 kSkip via expeditor.cpp:1928, :1722, :1824) hold no page. Recorded: device_page_store.hpp:170-184's bullet corrected. EVT02 owes kSkip only, and must refuse a frame its own core holds: a shared TryAcquire re-enters an own-core X hold (page_latch.hpp:236) - a lost-write risk, not a lock edge.
- **A kWait flush from Begin's carve - reachable today, never under a page hold.** Census D listed it as hypothetical; the critic found it live (the carve row above): every kTrxIdBlockSize begins and on idle burns, Sync -> WriteBack(kWait) waits on claims. Every Begin caller (:2843, :7731, :7971) runs before any page, and E1-E4 reach no flush (WalManager's ring-full Flush touches no PageStore). Nothing enforces it: a SharedHoldsHere assert would miss exclusive holds (device_page_store.cpp:2323); a check needs a thread-local count of holds in both modes. Recorded: this is why BB-R5 now forbids beginning a transaction, carving a transaction-id block or flushing the store between the descent and the placement.
- **Page against page among user leaves - real, not a BB cycle.** A WHERE sub-chain runs per row, synchronously, under the walk's leaf X (:7230/:8803 -> step_vm.cpp:2763, :166-176; step_compiler.cpp:1418-1436). It takes no partition latch (step_vm.cpp:2759), IS is compatible with IX, and a page-latch wait spins forever (page_latch.hpp:160-170). Members: mirror UPDATEs over two relations; same-relation walkers; the DELETE reverse-FK walk (fk_check.cpp:407); the right-neighbour read of a point write or BtreeInsert (btree.cpp:891, :1095) against a leftward walker. Not BB's: every member holds and asks only level-1 pages, E1-E2 ask levels 2-3, E3 a var-heap page that asks no user page, and E4 re-descends only through internal nodes above its holds (btree.cpp:467-471), which no walker holds. Recorded: docs/inflight/bugs/a-write-walks-subquery-reads-pages-under-its-exclusive-leaf-hold.md, which must drop "the inserter asks no other user leaf (btree.cpp:102)" and say TryAcquirePageLatchShared (device_page_store.cpp:2407) is a private writeback helper, its only caller :1538; rules.md §3's page-latch row corrected. Smallest fix, outside BB: a non-blocking shared fetch for a sub-chain under a write hold, a busy page returning a retryable status through AO-S3b's WalkCursor (:5480-5503); statements without sub-chains pay nothing (step_vm.cpp:2747).
- **A self-referencing FK under the leaf (:7328, :5009) - unreachable.** The key cannot be declared (:3987-3993; tests/foreign_key_test.cpp:464-471), so the arm (:3454-3472) is dead code.
- **The window latch under a page - a real nesting, one-directional.** No window-latch holder asks a page (instance_visibility.cpp:271, :305, :325). Recorded as stale text below.
- **The critic's three cycle candidates - no cycle.** The view latch: nothing holding page 7, a leaf or a partition latch asks it. The assertion chain: directory-latch scopes never nest the chain latch, and no chain, directory or Bound Cabin holder asks page 7, a user page or a partition latch. Begin's carve against BB's leaf and page 7: the flush waits on the insert, never the reverse, while Begin and MaybeBurnIdleBlock stay outside every page hold - BB-R5's new clause.

#### Stale declared text corrected with the declaration

| text | where | what the tree does | what it should say |
|---|---|---|---|
| A partition latch is taken "with no page latch held" (AR2-R2) | txn.md §5 (:867); lock_table.hpp:136-141; sched.md §9-2 | it is taken under a page at :7273, :8864, and catalog.cpp:2558/:2594 -> :5090 | "Taken with no park under a page latch" - enforced only by structure and the debug-only suspend audit |
| The page latch is held across a durability wait "only on the fault path" | device_page_store.hpp:170-184 (:176-178) | no path does; the fault sweep only queues | No path holds a page latch across a durability wait. A future fault-path writeback is sound only in kSkip, and only if it skips a frame its own core holds (page_latch.hpp:236) |
| The page latch is "never nested with the visibility window latch" | device_page_store.hpp:193-198; page.md §6 (line 156) | Visible, undo growth and the purge take it under pages (instance_visibility.hpp:202-210 agrees) | The page latch is outer to the window latch, which is a leaf, as rules.md §3's window row says |
| "Nothing holds a node and asks one to its left"; "parent before child, old before new" | rules.md:32; btree.cpp:95-96, :467-471; device_page_store.hpp:199/:225 | write-walk sub-chains read either side under a leaf X; SecureParents holds the leaf, then asks its parents | The leftward rule is true of the tree's own code; "parent before child" has been false of it since AT-S16. Both are false of a write walk's WHERE. device_page_store.hpp now names the tree's shapes: a leaf then its parents bottom-up, a leaf then its right neighbour, old before new. BB-R4 orders no user page against another. index_tree.cpp:73 holds as written |
| What nests under a CT7 hold | catalog.md CT7 | the undo page under every transactional write, unnamed; InsertAssertion's OpenAssertions under page 14 is a cache hit (assertion_catalog.cpp:195, before :214); RegisterPattern, outside CT7's table, takes page 7 under page 9, which rules.md §3's catalog row already stated | "Never another catalog relation's page" stands. CT7 names the undo page as a sink, and points to RegisterPattern's 9 -> 7 |
| The orphan bug names only ChainInsert | the orphan bug doc; BB-R7 | ChainAppendBatch (heap_chain.cpp:197-214, :253) orphans rows too; varheap::ChainAppend (varheap.cpp:319-331, :369) leaks only | BB-R7's re-check covers ChainAppendBatch, and says the held tail on a grow is the old one (heap_chain.cpp:171-172). The var-heap leak is its own entry, outside BB: docs/inflight/bugs/two-cores-growing-one-var-heap-chain-can-leak-a-page.md |

**For BB-S5** - the costs the census priced, for the close's
no-destructive-overhead constraint: `ForFirstRow` takes each chain page `X`
up to the row (`catalog.cpp:154-156`), so under E1 two relations' id issues
serialise there; a fenced relation's tuple ask scans `64 x cores`
partitions under the leaf (`lock_table.cpp:347`); the longer rightmost-leaf
hold adds wait time, not a cycle.

### BB-S2 — red first, built 2026-10-06

Built on `worktree-bb-issue-under-the-leaf` on `681fcd32` (BB-S1). Run on
that engine, Debug:

- **The seam**: `CommandDispatcher::SetAfterRowIdFixedForTest`, run once per
  `INSERT` row with its id after the id is fixed and before the row is
  placed. At this commit that point is outside every page hold.
- **Red on the two-core rig**, `tests/issue_under_the_leaf_rig_test.cpp`,
  **30/30 over ten repetitions**:
  - `AnIdIssuedFirstIsPlacedBelowALaterOne`: omitted pk against omitted pk;
    `ORDER BY id` answers `2, 1` and the bare walk `2, 1`;
  - `ALimitOneOverThePkReturnsTheLowestId`: `ORDER BY id LIMIT 1` answers row
    2 - a different row, not only a different order;
  - `ANamedKeyAtTheMarkIsPlacedBelowALaterIssuedId` (§1.3): a named key at
    the mark against an omitted pk; `5` lands after `6`.
- **Red on one core**, `tests/supplied_key_test.cpp`:
  `ANamedKeyBelowTheMarkIsRefusedOnABtree` (admitted today) and
  `ADuplicateNamedKeyLeavesTheRelationAscending` (§1.11: `DESCRIBE` reads
  `key_order=unordered` after a refused duplicate).
- **The guards, green**, `tests/lock_family_test.cpp`:
  `AnIssuedRowWithASpilledValueWaitsOnAFence` and
  `ANamedRowWithASpilledValueWaitsOnAFence` - a spilled row waits on an open
  declared-range `UPDATE` rather than being refused.
- **No hang by construction**: each rig cell gives core 1 a bounded look,
  releases core 0's seam whatever it saw, and only then waits for both.

The suite at this commit is red on exactly the five cells above.

### BB-S1's and BB-S2's reviews - 2026-10-06

**BB-S1**, `681fcd32`, reviewed by `critics-developer`, read-only. Twenty
census rows were re-read against `6dc792c9` and held, and the hunt for a missed
path found none: the census and BB-R4 stand. The findings were in the text
around it, and all were applied on `9e58dfd6`'s successor (this section's
commit):

- **Built behaviour stated as built.** BB-S1 described BB-S3's issue under the
  leaf in the present tense; it is "from BB-S3" now, and `page.md` §6 names the
  pair as the tree takes it today (`DELETE`'s reverse foreign-key check reading
  page 7 under the parent leaf).
- **Two contradictions.** "The one pair across relations that is stated" was
  false (page 9 before page 7, and a relation page before a Bound Cabin page,
  were already declared); CT7's claim that page 14 nests other catalog pages
  was false (a cache hit, filled before the hold). Both corrected.
- **Five more corrections**: "parent before child" in `device_page_store.hpp`
  (false since AT-S16); the fault-path writeback's soundness, now one sentence
  in all three homes (`kSkip` only, and only skipping a frame its own core
  holds); the window latch's takers; E1-E4 defined once; four citations.
- **Recorded**: `docs/inflight/bugs/two-cores-growing-one-var-heap-chain-can-leak-a-page.md`
  (a space leak, no value lost; left out of BB), and BB-R7 now names the sorted
  fill's `ChainAppendBatch` as orphaning rows the same way. AR2-R2's ruling
  text annotated with its amendment.
- **Simplified**: BB-R4's declaration lives in `page.md` §6 alone, with
  pointers from the header, CT7 and `rules.md`; the AR2-R2 amendment's reasoning
  in `txn.md` §5 alone; the census section's two summaries merged.

**BB-S2**, `169072d7`, reviewed by `critics-developer`, read-only. The cells
discriminate and do not hang. Applied at `9e58dfd6`:

- **Core 1 carves its transaction-id window before the race.** A carve
  persists page 0 through a full flush; inside core 1's bounded look it could
  outlast the look under `-j8` and let a mutant survive.
- **The seam's contract is adjacency to the fix**, wherever the fix sits - the
  property that makes the race mutants killable.
- **The `LIMIT 1` shape folded into the first rig cell** (one race observed
  three ways), the named-key cell given the bare walk too, the two guards
  sharing one helper, `DESCRIBE` checked positively so BB-S3b's deletion turns
  it red.
- **Carried to BB-S3 and done there**: `InsertOneRow` returns its refusal as a
  `Status`, so BB-R12's codes reach the wire.
- **Rejected**: moving the rig's one-statement runner into `two_core_rig.hpp`
  - eight files outside BB; its own commit.

Re-run on the pre-fix engine after the review: both rig cells red 10/10, both
one-core cells red 10/10, both guards green 10/10.
