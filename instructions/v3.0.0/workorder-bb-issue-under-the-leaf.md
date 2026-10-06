# Work order BB — the id fixed under the leaf it lands on: defect A closed at its source

Written 2026-10-06 on `worktree-bb-s0-order` from `bddd450c`
(`v2.7.0-622-gbddd450c`), on the operator's *"(b)로 원천 차단하는 방식으로
진행하려고 해 일단 지금까지 마일스톤 작업은 잠시 중지하고 이 오류를 해결하기
위한 서브 마일스톤 작업 계획 & 지시서를 작성해줘"*
(`raft-marks-2026-10-06.md` §4).

**A sub-milestone of BA, and BA is paused for it.** BA-Q14 is marked (b): the
id is issued under the hold of the leaf it lands on. BB replaces BA-S1b. BA's
remaining stages wait until BB closes, and BB-S5 is where BA resumes.

**Not opened.** BB-Q0..Q7 are unmarked, and each stage then waits for its own
word. BB cuts no tag.

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
- **Which paths produce the misorder?** One path does, and it is not rare
  (§1.2).

**BB's one rule (BB-R1):** a row's id is fixed under the exclusive hold of
the page the row lands on. The rule covers an issued id and a named key at
or above the mark. Placement order within a page then equals key order
while `key_order` is `kAscending`. That is the premise the `ORDER BY <pk>`
elision already rests on (`step_compiler.cpp:2040-2081`), now made true.

**BB does not do:**

- **Remove `kUnordered`.** A named key below the mark still flips it. That is
  the btree's licence to take keys in any order (`heap-and-tuple.md` §4.1),
  and the per-page emission still serves it.
- **Make inserts faster.** BB moves work inside the rightmost leaf's hold
  (§1.9). BA-R8's cursor (BA-S11) is where that cost is taken back.
- **Touch the system relations' `sys.*` chains** (§1.8, BB-Q5).

## 1. Survey at `bddd450c`

Read, not run. Every line number below is at `bddd450c`.

### 1.1 Issue and placement are two latched spans

An `INSERT` row runs these steps in `CommandDispatcher`
(`src/server/command_dispatcher.cpp`):

1. **The id.** Either issued by `catalog_.AllocateRowId(oid)` (`:5106`), or,
   for a named key, admitted by `catalog_.AdmitExplicitRowId` (`:5087`).
   Both work under the hold of page 7's `sys.tables` row
   (`catalog.cpp:2454`, `:2502`), and that hold is released on return.
2. **The tuple borrow**, keyed by the id: `BorrowOrWait` (`:5111`; for a
   named key, inside the admit's hook).
3. **The encode**: `EncodeRow` writes the id into the Keystone word, and
   spills long values to the var-heap. Each spill is logged under its
   var-heap page's hold (`:5121-5128`, `SpillLogFor` at `:4175`).
4. **The placement**: `InsertIntoRelation` (`:5134`, `:5313`). For a btree,
   `BtreeInsert` descends, takes the leaf exclusive, checks duplicates and
   appends at the slot count (`btree.cpp:875-935`).

So between step 1 and step 4, another core can fix a higher id and place it
first. The leaf appends whatever arrives (`heap_page.cpp:162-163`), so the
leaf then holds the higher id in the lower slot.

### 1.2 It is not rare

Two cores inserting omitted pks into one relation both descend to the
rightmost leaf, and whichever takes its latch first places first. The id
order was fixed earlier, at page 7. So under concurrent inserts the misorder
is a coin flip at every collision. It is not a narrow interleaving. Scenario
0's `cores = 8` cells run exactly this shape.

### 1.3 A named key at or above the mark has the same window

This is new: neither the bug entry nor BA-R1b names it. `AdmitExplicitRowId`
moves the mark to `k + 1` under page 7 (`catalog.cpp:2595-2610`), and the
row is placed later (step 4). The same race follows:

- core A names `k` (the mark becomes `k + 1`);
- core B omits its pk and is issued `k + 1`;
- core B places first.

Two named keys at or above the mark race the same way. A named key *below*
the mark is the legitimate path: it flips `key_order` (`catalog.cpp:2560-2568`)
before it is placed.

### 1.4 What sits between the two spans, and what each needs the id for

- **The borrow** needs the id: it is the lock key. It **never parks
  inline**. `BorrowOrWait` returns a refusal and records the wait
  (`:8360-8400`). The statement then ends, `DispatchAsync` parks, and the
  statement re-runs, drawing a fresh id (the comment at `:5102-5105`). So it
  can be asked while a page latch is held, provided the lock table's latches
  may sit under a page latch (§1.6).
- **The Keystone word** needs the id. It is a fixed offset in the payload,
  read by `KeystoneIdOfPayload` (`include/kds/storage/keystone.hpp:83`).
  Nothing writes it in place today.
- **The spills** need the id only as a diagnostic:
  - `kVarHeapAppend`'s `pk` *"says which row's spill this was"*
    (`include/kds/txn/undo_page.hpp:142-160`);
  - recovery releases the slot without reading it
    (`src/txn/recovery_undo.cpp:46-52`);
  - the live compensation's var-heap arm returns before any pk check
    (`src/txn/manager.cpp:399`).

### 1.5 The descent can already hold the leaf an omitted pk lands on

- **The rightmost leaf.** `DescendTo` takes the leaf exclusive and checks
  that it still covers the key (`btree.cpp:160-200`, `LeafStillCoversKey`).
  It restarts at most `kMaxDescentRestarts` times and then refuses
  `TxnConflict` (`:232`). Descending for `kMaxKeystoneId` lands on the
  rightmost leaf and holds it, and no splice can pass it while it is held:
  every splice writes the leaf it splits.
- **The issued id belongs there.** `next_id` is above every placed id, and
  every leaf's `min_key` is a placed id. An append split creates its new
  leaf with `min_key = id` for the id it places (`btree.cpp:990`). So
  `next_id` is at or above the rightmost leaf's `min_key`, and the duplicate
  scan cannot find it.
- **A full leaf.** The append split runs under the same hold, with
  `SecureParents` first (`:946-962`), and needs the id before it creates the
  leaf. Issuing under the hold gives it the id in time.

### 1.6 Latch order today, and the one BB adds

- **The page latch is outer to the WAL stream latch** (`rules.md` §3).
- **Page 7's latch is never asked while a user relation's page is held.**
  BA-R1b named this as (b)'s cost. BB adds the order: user relation page
  (the leaf, and any parents a split secured), then the `sys.tables` chain
  page, then the WAL stream.
- **Two proof obligations are BB-S1's**, not this survey's:
  1. **Inversion.** Which paths hold a `sys.tables` chain page and then ask a
     user relation page's latch? A page created under the hold is not one:
     no other core can reach it.
  2. **Lock-table latches.** Do the lock table's partition latches and its
     `wait_latch_` already sit under page latches anywhere? The lock table
     declares its order in its spec, *"stated and not enforced"*
     (`rules.md` §3).
- **Known starting points** for the census:
  - `AdmitExplicitRowId`'s hook takes the borrow under page 7 (`:5087-5096`),
    which already puts the lock table under a catalog page;
  - every name-taking DDL holds its catalog relation's root page across the
    check and the write (`catalog.md` CT7);
  - `index_ddl.cpp:294` issues before any page is walked;
  - `Catalog::RecordAccess` holds `sys.access_stats`' root.

### 1.7 Heap relations

A heap relation has the same window: it is creatable only before SUS-1, so
it exists only on an older volume.

- **Placement.** `ChainInsert` places into the tail and holds it
  (`heap_chain.cpp:61`). Its duplicate check reads the tail alone, which is
  sound only while ids ascend across pages.
- **The race across a tail-page boundary** ends in `OutOfRange`
  (`heap-and-tuple.md:250-258`). **Inside one tail page** it misorders, and a
  heap relation cannot be `kUnordered`.
- **The sorted fill** carves a block of ids and fills fresh pages. It has the
  same window against another core's issue. It is heap-gated, so it is
  unreachable for every relation created since SUS-1.

### 1.8 System relations

The `sys.*` chains are heap chains, issued from and placed in the same two
spans:

- `catalog.cpp:2800`, `:3067`, `:3183`, `:3405`;
- `index_ddl.cpp:294`;
- `assertion_catalog.cpp:493`, then `ChainInsert` at `:250`.

**Not read here:**

- whether every such write already runs under one hold of its relation's
  root page (CT7 says it does for name-taking writes);
- whether any reader depends on their key order.

BB proposes leaving them out and recording the question (BB-R10, BB-Q5).

### 1.9 What BB costs, and where BA takes it back

- **The rightmost leaf's hold widens** (P9, BA §1.9). The hold already
  covers the placement, the Cabin witness, index maintenance, the
  reservation, the undo append and the `kHeapInsert` append. BB adds:
  - the descent for `kMaxKeystoneId`, which replaces the descent for the id;
  - `AllocateRowId`'s walk of the `sys.tables` chain and its logged
    overwrite: a WAL append under page 7, nested in the leaf's hold;
  - the borrow's partition latch.
- **What takes it back.** BA-R8's cursor (P5) makes the issue a `fetch_add`,
  with page 7 asked only once per 32 ids. Under BB that `fetch_add` runs
  inside the leaf's hold. BA-S11's cell *"every leaf in key order"* becomes
  an invariant BB has already established, not a race BA-S11 must avoid.
- **`cores = 1`.** One core cannot interleave, so BB reorders work there
  without adding any. Whether the reorder is free is measured, not assumed
  (BB-R9).

## 2. Rulings — CLA's proposals, not marked

**BB-R1 — the rule.**

- A row's id is fixed under the exclusive hold of the page it lands on. That
  covers an issued id, and a named key admitted at or above the mark.
- Consequence, stated in `heap-and-tuple.md` §4.1: **while `key_order` is
  `kAscending`, every page's slot order is its key order, whatever the core
  count.**
- The pk stays a sequence in issue order (invariant 11). Placement order now
  equals issue order within a page.

**BB-R2 — an omitted pk on a btree relation.**

1. `BtreeInsert` descends for `kMaxKeystoneId`, which lands on the rightmost
   leaf, held exclusive.
2. Under that hold it calls an **id source** the caller passes in. The
   storage layer has no catalog, so it is a callable; `new_root`'s
   precedent applies (`insert_placement.hpp`).
3. It checks `id >= leaf.min_key()`, which §1.5 argues is always true.
   Failing it is `Corruption`, not a retry.
4. It then runs the duplicate scan and the placement as today, an append
   split included.

**BB-R3 — a named key on a btree relation.**

- `BtreeInsert` descends for `k`, holds the leaf, and calls
  `AdmitExplicitRowId` through the same id-source seam.
- **Below the mark** it still flips `key_order` (unchanged).
- **At or above it** it moves the mark under the leaf's hold, which closes
  §1.3.
- The admit's `before_mark` hook keeps its borrow, now under the leaf and
  page 7. It still returns rather than parks (§1.4).

**BB-R4 — the insert's order.**

1. **Encode first**, with Keystone id 0 for an omitted pk. Spills are
   appended and logged under their own var-heap holds before any leaf is
   held, so no var-heap latch nests under a leaf.
   - An omitted-pk row's spill undo records `pk = 0`. Read it as *"spilled
     before its id was issued"*.
   - The diagnostic loses the row's id. Recovery and compensation lose
     nothing (§1.4).
2. **Descend and hold**, then fix the id (BB-R2 or BB-R3).
3. **Patch the Keystone word.** A new `SetKeystoneIdOfPayload` in
   `keystone.hpp`, with shift and mask only (invariant 6). It keeps the flag
   and reserved bits.
4. **Borrow the tuple.** A refusal releases every hold and places nothing;
   the id is burned, which K3 licenses. The statement ends with its wait
   recorded, and `DispatchAsync` re-runs it as today.
5. **Place, and everything after** exactly as today, under the same holds
   (AT-S21).

**BB-R5 — the latch order, spec first.**

- **The order.** User relation page (leaf, then any secured parents), then
  the `sys.tables` chain page, then the lock table's partition latch, then
  the WAL stream latch.
- **Where it is declared.** `rules.md` §3 gains it in the page-latch row and
  the catalog-pages row, and `catalog.md` states it beside CT7. The
  declaration lands before any code that relies on it.
- **The proof.** BB-S1's census shows that nothing takes the reverse.

**BB-R6 — a refusal under the hold is a release, never a wait.**

- No code between the descent and the placement parks, and none waits on
  another core's latch while holding the leaf. The one exception is page 7,
  which BB-R5 orders.
- A descent that cannot hold the rightmost leaf after
  `kMaxDescentRestarts` restarts is refused `TxnConflict`, as today.

**BB-R7 — a heap relation (on BB-Q2).**

- **The same rule, with the tail page standing for the leaf.** `ChainInsert`
  takes the tail, then fixes the id through the same seam.
- **The sorted fill** carves its block under the hold of the tail it fills
  from.
- This removes the in-page misorder, and with it the boundary `OutOfRange`:
  ids then ascend across a heap chain at every core count.
  `heap-and-tuple.md:250-258` is restated.

**BB-R8 — the invariant is checked, not only argued.**

- **The sim.** The sim's integrity sweep (`sim/integrity.cpp`) gains a
  check: on a `kAscending` relation, every page's live slots ascend by
  Keystone id. Sweeps run on every corpus seed after restart, so a future
  path that reopens the window fails the corpus, not a client.
- **The rig.** BB-S2's rig cells drive the interleaving directly.

**BB-R9 — the cost is measured once, at BB's close.**

- **The A/B.** `ck-tester`, `build-release`, interleaved, A = `bddd450c`,
  B = the close.
- **The cells.**
  - an omitted-pk single-row `INSERT` at `cores = 1`;
  - the same at `cores = 8` into one relation;
  - the same at `cores = 8` into eight relations, so P9's share can be
    told from page 7's.
- **If the cost is material,** BB still lands: it closes a quiet wrong
  answer. BA-S11 is then ordered ahead of BA's other fix stages, with BB's
  number as its premise.

**BB-R10 — system relations are out.** §1.8's two questions are recorded in
`known-gaps.md`, so a later order can take them.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BB-S0 | **The order** | this file, its review, the marks recorded | S |
| BB-S1 | **The latch-order census** (BB-R5) | **The census.** A table, in §6, of every path that holds a `sys.tables` chain page and every path that takes a lock-table latch, each with what it holds and what it then asks. The answer to §1.6's two obligations, with file:line. **The declaration.** BB-R5 declared in `rules.md` §3 and `catalog.md`. No engine change. If an inversion is found, BB stops at this row and the operator rules | M |
| BB-S2 | **Red first** | **Cells.** Rig cells on a test seam between fixing the id and placing it, red at BB-S1's commit:<ul><li>omitted pk against omitted pk: `ORDER BY id` returns 101 before 100, and `DESCRIBE` still says `ascending`;</li><li>the `LIMIT 1` shape returning the wrong row;</li><li>named-at-or-above-mark against omitted (§1.3).</li></ul>**Sim.** BB-R8's integrity check, run on the corpus. A seed that is red without the seam is recorded with the seed; a corpus that stays green is recorded as not discriminating | M |
| BB-S3 | **The btree arm** (BB-R1..R6) | <ul><li>**Cells.** BB-S2's btree cells green.</li><li>**Suites.** The Keystone suites and the waystone, index, cabin and inner-build contract suites byte-for-byte green; the sim corpus green with BB-R8's check.</li><li>**Mutations, each repeated:** the id fixed before the descent; the Keystone patch skipped; the borrow taken before the hold; the named key admitted outside the hold.</li><li>**Text restated with the code:** `heap-and-tuple.md` §4.1, `btree.cpp:943-945`, `keystoneid-invariant.md`.</li></ul> | L |
| BB-S4 | **The heap arm** (BB-R7, on BB-Q2) | BB-S2's cells re-run on a heap relation created behind the SUS-1 seam the heap suites use. The tail-boundary `OutOfRange` no longer reachable by a race. `heap-and-tuple.md:250-258` restated. The bug entry deleted | M |
| BB-S5 | **The close** | **The measurement.** BB-R9's measurement, its results file under `bench/v3.0.0/`. **The rows.** A row per stage, and what BB carries. **BA resumes.** BA's §6 records BA-S1b as superseded by BB, and BA-S11's row is rebased on BB-R1 | S |

## 4. Items for the operator

| item | what | class | CLA's proposal |
|---|---|---|---|
| BB-Q0 | **Open BB** as BA's sub-milestone, BA paused until BB-S5 | process | Yes |
| BB-Q1 | **Named keys at or above the mark** are in BB (§1.3, BB-R3) | user-visible | Yes. Leaving them out leaves defect A reachable by any client that names its pks |
| BB-Q2 | **The heap arm** (BB-R7): (a) the same rule under the tail's hold; (b) no change to placement, and every heap `ORDER BY <pk>` emits per page in key order; (c) heap left as is, the remainder recorded | user-visible, **[quiet-wrong] for (c)** | (a). It also removes the boundary `OutOfRange`. (b) prices every heap relation for a race, and (c) keeps a wrong answer |
| BB-Q3 | **The spill undo's `pk = 0`** for an omitted pk (BB-R4 step 1), against encoding under the leaf's hold, which nests var-heap latches under the leaf | invariant (on-disk diagnostic) | `pk = 0`. The field is a diagnostic, and the alternative widens the leaf's hold by every spill |
| BB-Q4 | **The latch order** user page → `sys.tables` page → lock-table latch → WAL (BB-R5) | invariant | Yes, conditional on BB-S1 finding no inversion |
| BB-Q5 | **System relations out of BB** (BB-R10) | scope | Yes, recorded in `known-gaps.md` |
| BB-Q6 | **The sim integrity check** (BB-R8) | test | Yes |
| BB-Q7 | **BB lands whatever BB-R9 measures**, and a material cost moves BA-S11 to the front of BA's fix stages | process | Yes |

## 5. Sequencing

1. **BB-S1 before any engine change.** The order it declares is what BB-S3's
   code relies on, and an inversion found there changes BB-R5, which is the
   operator's.
2. **BB-S2, then BB-S3, then BB-S4.** S4 reuses S3's seam and its cells.
3. **BB-S5 closes BB, and BA resumes there.**
   - BA-S1b is struck as superseded.
   - BA-S2..S4's census then measures an engine whose rightmost-leaf hold BB
     widened. That is the engine BA's fix stages act on.
4. **File overlaps:**
   - BB-S3 and BA-S11 both touch `AllocateRowId` and the insert pipeline.
     BA-S11 is written against BB-S3's shape.
   - BB-S3 and BA-S1c share nothing beyond `command_dispatcher.cpp`'s file.

## 6. Row status

### BB-S0 — written 2026-10-06

Written on `worktree-bb-s0-order` from `bddd450c`, on the operator's word
above. §1 is read, not run. BA's order records the pause and BA-Q14's mark,
and the index carries both.
