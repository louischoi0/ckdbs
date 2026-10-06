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
- BB-Q9..Q11 follow from that mark and are unmarked.

Each stage waits for its own word. BB cuts no tag.

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

**BB-R11 — a mounted volume that holds a `kUnordered` relation (on BB-Q9).**

- **Proposed: refuse the mount**, naming every such relation, with
  `Unsupported`: *"relation `t` holds keys out of order, a shape this engine
  no longer serves"*.
- **How.** Core 0 checks while it loads the catalog at mount. There is no
  superblock bump, so every v19 volume without such a relation still mounts.
- **Precedent.** A pre-M0 volume and a split relation were refused the same
  way (AM-S4(d), superblock 18).
- **Why this is a decision.** Reading the byte as ascending would serve
  those relations' `ORDER BY <pk>` wrong.

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
| BB-Q9 | **A mounted volume holding a `kUnordered` relation** (BB-R11).<br>(a) Refuse the mount, naming each such relation.<br>(b) Keep a legacy per-page emission for those relations only.<br>(c) Re-sort them at mount.<br>(d) Read them as ascending | user-visible, **[quiet-wrong] for (d)** | (a). (b) keeps alive the machinery BB-Q8 deletes, and (c) rewrites leaves whose `(page, slot)` addresses undo records, indexes and Cabins hold | unmarked |
| BB-Q10 | **The code a refused named key gets on a btree** (BB-R12).<br>(a) `AlreadyExists` when present, `OutOfRange` when absent.<br>(b) `OutOfRange` always, as a heap | user-visible | (a). It is free, and duplicate detection keeps working | unmarked |
| BB-Q11 | **`DESCRIBE`'s `key_order=` and the byte** (BB-R10).<br>(a) The field is removed with the state it reported; the byte is reserved, written 0, and read only by BB-R11's check.<br>(b) The field is kept, always `ascending` | user-visible | (a). A field that cannot vary reports nothing | unmarked |

## 5. Sequencing

1. **BB-S1 before any engine change.** The order it declares is what BB-S3's
   code relies on. An inversion found there changes BB-R4, which is the
   operator's.
2. **BB-S2, then BB-S3, then BB-S3b, then BB-S4.**
   - S3b needs BB-Q9 and BB-Q11 marked, and deletes only what S3 has left
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
