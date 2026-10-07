# Work order BD — a named key lands where it sorts: every btree leaf kept in key order by placement, and no high-water-mark refusal

Written 2026-10-07 on `worktree-bd-sorted-leaf-named-keys` from `bf8ea937`
(`v2.7.0-652-gbf8ea937`), on the operator's words below. All are verbatim,
from one session, and recorded in `raft-marks-2026-10-07.md` §7-§13.

**The operator's words**, numbered so the rest of this order can cite them:

- **W1:** *"kUnordered를 삭제하면서도 high water mark 오류가 발생하지 않도록 하는
  방법이 있지 않니?"*
- **W2:** *"순서를 어기는 삽입이 왜 필요하지? 낮은 key를 가진 leaf를 삽입할때 항상
  끝쪽이 아닌 알맞은 순서에 넣으면 되잖아"*
- **W3:** *"kUnordered를 삭제하면서, 별도 플래그를 두지 않으면서, btree를 항상
  정렬되게 유지하면서도 high water mark 오류는 없어야해."*
- **W4:** *"(a)를 선택할거야."* CLA had offered two options:
  - (a) shift the slot directory and change what addresses a slot;
  - (b) keep slot numbers fixed and add a key-order array to the leaf.
- **W5:** *"해당 Invariant 변경에 대한 작업 지시서를 작성해줘, 추가로 named_pk
  삽입이 pk때문에 실패하는 이유는 2가지 뿐이어야해. 중복되거나, 소진되었거나."*
- **W6:** *"tombstone을 사용하는 것은 CLA제안에 따라서 구성"*
- **W7:** *"(a)로 진행해"*. This is the reading of W5's two reasons (BD-Q3).
- **W8:** *"high water mark (pk=100을 먼저 쓰면 pk=99 삽입이 안되는 현상이 있으면
  안돼"*
- **W9:** *"superblock 하위호환성은 필요없어. 내가 따로 말하기전까진 이러한 사항을
  항상 반영해"*. This is a standing order.
- **W10:** *"BB-R1 은 삭제해. 성능 측정은 skip"*
- **W11:** *"작업 지시서만 작성해서 push해"*
- **W12:** *"롤백된 아이디는 당연히 재사용되어야해"*
- **W13:** *"BD-Q4, Q8, Q9 제안대로 마킹해줘"*
- **W14:** *"리뷰 끝나면 반영해서 push해"*

**Status: written, reviewed, and not yet opened.**

- §4's mark column says which items the words settle.
- BD-Q0 and BD-Q10..Q12 wait for the operator.
- No stage has started.
- W11 asked for this order alone, so nothing under `src/`, `include/` or
  `tests/` moves with it.

**What BD withdraws** (`workorder-bb-issue-under-the-leaf.md`). On a btree:

- BB-R1 (W10);
- BB-R2, the omitted pk's order under the hold;
- BB-R3's refusal of a named key below the mark, and its order;
- BB-R12's `OutOfRange` arm;
- the half of BB-Q8 (b) that refused the key. BB-Q8 (b) was the operator's
  mark of 2026-10-06, and W3 and W8 are the later word on the same point.

On every relation, BB-R11's mount check goes with superblock 20 (BD-R8).

**What BD keeps:**

- BB-Q8 (b)'s other half: `kUnordered` stays deleted (BB-R10);
- BB-R4;
- BB-R5, restated (BD-R6);
- BB-R6, extended (BD-R9);
- BB-R7, the heap (BD-Q4).

**BB and BA.** BD is not part of AR0 §8's chain. BB is not closed: BB-S5's
rebase of BA is drafted and unreviewed, and BA is paused until BB closes.
BD-Q12 asks which order carries that rebase (§5).

## 0. What BD is

**Today a btree refuses a named key below the relation's mark,
`sys.tables.next_id`.** Since BB-S3 (`1b5d252e`), naming pk 99 after pk 100
is refused: `OutOfRange` when 99 is absent, `AlreadyExists` when it is
present.

**The refusal exists for one reason.**

- A btree leaf places every row at its slot count (`heap_page.cpp:163`).
- A leaf's slot order must equal its key order. Three readers depend on it
  (BB §1.10):
  - the `ORDER BY <pk>` elision;
  - the Cabin serve's pk sort;
  - the index step's sort.
- So the only key a leaf could take was one above every key it held.
- BB refused the others rather than flag the relation (`kUnordered`, BB-Q8).

**BD removes the reason instead of the key** (W2, W3, W4). A leaf places a
row at the slot index its key sorts to, and moves the directory entries
after it up one. Every leaf is then in key order whatever order keys arrive
in, with no refusal and no flag.

**The mark did three jobs for free, and BD gives each a new owner:**

1. **Order.** Placement keeps it (BD-R2).
2. **Issue-once (K1).** A deleted key already stays in its leaf for good,
   delete-marked, because nothing purges a user relation. A rolled-back key
   is free again (W12). So the leaf answers "duplicate", and the mark gates
   nothing (BD-R4).
3. **A cheap uniqueness proof.** Issued keys keep it. A named key's
   uniqueness is the descent's, as it was before BB.

**A shifted row changes its slot.** Five things that address a btree row by
`(page, slot)` would read the wrong row after a shift, and BD makes each of
them shift-safe before it opens the gate (BD-R3). BD also makes a split
replay whole or not at all (BD-R12), because SQL reaches the leaf divide
again.

**BD does not:**

- touch a heap relation's placement: no volume this engine mounts holds a
  heap user relation (§1.5, BD-Q4);
- build a purge (BD-R4 records what one owes);
- measure anything (W10, BD-R10).

## 1. Survey at `bf8ea937`

Everything here was read, not run. Three read-only censuses were made:

- every holder of a btree row's `(page, slot)`;
- every refusal a named key gets, and what rollback, `DELETE` and a divide
  do to a leaf row;
- every text and test that states or pins BB-R3.

BD-S0's review then re-checked them against the tree. A claim tagged
*[inferred]* was concluded from the code and is not stated or tested
anywhere in the tree.

### 1.1 A btree row's slot is its arrival position

- **Every insert appends.** `PageView::InsertTuple` appends at
  `h.nr_slots` (`heap_page.cpp:163`), and every btree placement goes
  through it (`btree.cpp:776`, `:814`, `:841-842`, `:1017`, `:1084`).
- **Slots are "stable references"** (`heap_page.hpp:36-38`). A dead slot
  keeps its position, and nothing compacts a page except a rewrite.
- **The divide is the one path that renumbers.** `SplitLeafAndInsert`:
  - sorts the moving rows (`btree.cpp:757-758`);
  - rewrites each half;
  - skips retired slots (`:736`);
  - carries delete-marked rows with their keys (`:740-743`, `:781-785`,
    `:816-820`);
  - **appends the incoming row last** in whichever half receives it
    (`:840-845`).

  So that half need not be in key order, and `btree_test.cpp:832-835` says
  so. Since BB-R3 only the storage contract `BtreeInsert` reaches the divide
  (`btree.hpp:51-57`).
- **Index leaves already place sorted**, with a `memmove`
  (`index_page.cpp:180-203`).
- **`FindSlotForId`** (`btree.cpp:257-316`) is a binary search that steps
  over dead slots, with a linear fallback. The fallback exists because the
  storage contract can leave a leaf unsorted (`:267-273`), and
  `BtreeTest.ALookupFindsAnIdInALeafWhoseSlotsAreOutOfOrder`
  (`btree_test.cpp:527-568`) pins it.

### 1.2 Who holds a btree row's `(page, slot)`, and what a shift does to it

**Today no SQL path changes a placed btree row's `(page, slot)`.** Every
row appends on the rightmost leaf, and a full leaf grows by an append split
that moves nothing (`btree.hpp:36-49`). The holders marked "breaks" below
are safe today only because SQL never divides and never places mid-leaf.

| | holder | trust | under a shift |
|---|---|---|---|
| A | `Location`, `InsertPlacement`, `VerifiedTuple`, the `UPDATE`/`DELETE` walks, the forward FK check, and every WAL record appended under the hold | valid only while the leaf is held | safe |
| B | Waystone trails, Cabin hints, the probe memo, the Cabin serve's two phases, the reverse-FK Cabin path, the optimizer's heal | advisory: `VerifyTupleAt` (`tuple_verify.cpp:7-50`) checks the epoch, then the Keystone id against the expected pk; the caller falls back to a pk descent | safe, at the cost of a miss |
| C | lock units `(rel_oid, pk)` (`lock_table.hpp:340-387`); secondary index entries, which carry no location (`index.md:125-126`); the parked write's resume by `cut_pk` (`command_dispatcher.cpp:5598-5632`); the version chain through `undo_ptr` (`visibility.cpp:7-60`); var-heap pointers | keyed by pk or `undo_ptr` | unaffected |
| D | the live rollback trail (`TrailEntry`, `txn/manager.hpp:121-138`) | the slot is checked against the pk, and on a mismatch the row is re-found by pk with the leaf held (`txn/manager.cpp:419-469`; `RowLocatorForRollback`, `command_dispatcher.cpp:6001-6030`) | safe |
| E1 | **recovery undo** (`target_page_id`, `target_slot`) | the slot is checked against the pk, with **no way to re-find the row** | **breaks** |
| E2 | **redo of `HEAP_INSERT`** (`RedoWriteTuple`, `heap_page.cpp:386-452`) | a slot below `nr_slots` means "overwrite in place" | **breaks** |
| E3 | **the inner-build map** (JB3/JB4) | `(page, slot)` served with no identity check | **breaks under another core's shift** |
| E4 | **the resumable-prefix mark** (JB6), `(page, ordinal)` | resumes at an ordinal on a leaf it released | **breaks under another core's shift** |
| E5 | **an unlogged placement** (`command_dispatcher.cpp:5221`, `:5237`) | a row placed whose index maintenance (`:5300-5310`), assertion reservation (`:5320-5325`) or undo append (`:5347`) then fails, before `NoteInsert` (`:5350`) | **breaks** |
| F | the `INSERTED … page= slot=` reply (`command_dispatcher.cpp:4773-4776`, `manual/sql/sql.md:515`), `SHOW PAGE`, trace lines, the `AlreadyExists` text, and the assertion directory's row hint (`assertion_check.cpp:466-473`) | informational, never read for access | stale after a later shift |

**E1, recovery undo.**

- `RecoveryUndo::Compensate`:
  - reads the pk at `rec.target_slot` (`recovery_undo.cpp:92`);
  - counts an insert whose slot is dead or missing as done (`:114-117`);
  - refuses any other mismatch `Corruption`, which fails the mount
    (`:120-130`).
- The undo record carries no `rel_oid`, so the live locator cannot be used
  (`recovery_undo.hpp:39-57`).
- **The silent case under shifts** [inferred]:
  1. A loser inserts 50 at slot 3.
  2. It then inserts 40 at slot 3, which shifts 50 to slot 4.
  3. Undo retires slot 3 for the newer record.
  4. It then finds slot 3 dead for the older record, and counts it done.

  **Row 50 survives the crash**, and after a restart every pre-restart writer
  reads as decided (`txn/manager.hpp:307-318`).
- Pinned by `recovery_undo_test.cpp:289-313` and `:404-417`.

**E2, redo.** `RedoWriteTuple` reads the slot three ways:

- above `nr_slots`: `Corruption`;
- equal to it: an append;
- **below it: an overwrite in place.**

A mid-leaf insert would therefore replay as an overwrite of its neighbour
(`heap_page.hpp:352-361`).

- **How splits are logged today:** a split's pages are logged as images,
  taken after the incoming row is in. The `HEAP_INSERT` that follows then
  re-applies through the overwrite arm.
- **The index precedent:** the secondary index's split logs its images and
  **no** `INDEX_INSERT`, *"so emitting both would apply it twice"*
  (`command_dispatcher.cpp:4171-4180`).
- **`BTREE_INSERT`:** `wal.md` §5.2 proposes the name, and it is unassigned
  (`record.hpp:155-159`). The last assigned kind is `kVarHeapRelease = 28`.

**E3 and E4.**

- **E3:** `ProbeBuild` reads `entry.page_id`, then `AcceptTupleAt(entry.slot)`,
  with no `VerifyTupleAt` (`step_vm.cpp:1172-1176`).
- **E4:** the JB6 mark skips ordinals on a leaf it released
  (`step_vm.cpp:186-197`, `:1791-1809`, `:1847-1853`).
- Both reason only about the statement's own writes, but since AT-S5 another
  core can insert in between. Their comments name the fix
  (`step_vm.cpp:1101-1126`).
- A heap keeps its ordinal mark (`step_vm.cpp:1845`), since heap pages never
  shift.

**E5, the unlogged placement.**

- The row sits on its leaf with no trail entry and no record. Its own
  comment calls it *"a row a rollback would not undo"*
  (`command_dispatcher.cpp:5354-5360`).
- Today its writer has aborted, so no reader sees it, and it is harmless
  but for `RefuseDuplicate` counting it. Under BD it breaks two things
  [inferred]:
  - **W12.** A second slot can carry the same key, which makes the search,
    E1's re-find and a divide's cut ambiguous.
  - **Redo.** The unlogged row shifted its neighbours, so a later logged
    insert's slot assumes a row redo never sees. Take the on-disk leaf
    `[10, 30]`: an unlogged 20 is placed, 25 is logged at slot 2, and the
    instance crashes. Redo then gives `[10, 30, 25]`.
- **The leaf is still held at every one of those exits** (AT-S21).

**The epoch.**

- The divide is the only production writer of `relayout_epoch`
  (`btree.cpp:834`).
- The append split bumps nothing.

### 1.3 What a removed row leaves in its leaf

- **`DELETE` delete-marks** (`command_dispatcher.cpp:9079-9125`).
  - Nothing purges a user relation (`payload.hpp:385`; `heap-and-tuple.md:169`).
    `Catalog::RetireDeleteMarks` walks catalog chains only
    (`catalog.cpp:941-990`).
  - So **a committed key stays in its leaf for the life of the relation**,
    and a divide carries it (§1.1).
- **A rollback retires the inserted row's slot keyless.**
  - The live path re-finds the row, then calls `RetireSlot` and logs
    `SLOT_RETIRE` (`txn/manager.cpp:494-509`).
  - The crash path calls `RetireSlot` too (`recovery_undo.cpp:133-147`).
  - `RetireSlot` sets the dead flag and length 0 (`heap_page.cpp:369-384`),
    and a divide later drops the slot (`btree.cpp:736`).
  - `UnInsertTuple` takes back the last slot only, and its one caller is the
    catalog (`heap_page.cpp:191-209`, `catalog.cpp:407`).
- **A rollback leaves the insert's index entries.** The rollback list has no
  index entry type (`txn/manager.hpp:110-119`).
- **`RefuseDuplicate`** (`btree.cpp:884-905`) counts every slot whose
  payload reads. It skips retired slots.
- **The mark outlives a rollback.** It is logged under `kNoTxnId`
  (`catalog.cpp:2578`, `:235-243`), so today a rolled-back named key stays
  refused (`lock_family_test.cpp:1238-1243`).

### 1.4 Every refusal a named key gets today, and its fate under BD

| | condition | code today | where | under BD |
|---|---|---|---|---|
| P1-P3 | the pk position holds no value token, a `$name`, or a function call | `InvalidArgument` / `Unsupported` / `NotImplemented` | `parser.cpp:219-233` | unchanged |
| R0 | arity | `InvalidArgument` | `command_dispatcher.cpp:5009-5018` | unchanged |
| R1 | not an integer literal (`NULL`, `'str'`, `1.5`) | `InvalidArgument` | `:5028-5033` | unchanged: any integer column refuses it |
| R2 | negative, including a literal ≥ 2^63 the lexer overflowed | `InvalidArgument` | `:5034-5038` | **exhausted** |
| R3 | zero | `InvalidArgument` | `catalog.cpp:2508-2513` | **exhausted** |
| R4 | above 2^40 − 1 | `InvalidArgument` | `catalog.cpp:2514-2518` | **exhausted** |
| — | a literal ≥ 2^64: the lexer's accumulation is signed-overflow UB (`lexer.cpp:162-165`) and `InsertOneRow` reads only `int_val` (`:5034-5039`), so 2^64 + 5 can land as key 5 [inferred] | none: the wrong key | — | **exhausted** |
| L0 | `max_locks_per_txn` reached | `ResourceExhausted` | `lock_table.cpp:204-210` | unchanged |
| L1 | borrow refused; present, writer decided | `AlreadyExists` | `command_dispatcher.cpp:5182-5193` | **duplicate** |
| L2 | borrow refused; below the mark, and absent, or its latest writer in flight, or the lookup failed | `OutOfRange` | `:5198` → `catalog.cpp:2522-2528` | **deleted on a btree** (BD-R5) |
| L3, L3a, L3b | a lock wait; a deadlock; the 1 s fault net | `TxnConflict` | `:8565`, `:382-386`, `:467-517` | unchanged |
| S1 | the descent gave up | `TxnConflict` | `btree.cpp:234-240` | unchanged |
| S2 | the key is in the held leaf | `AlreadyExists` | `btree.cpp:892-905` | **duplicate** |
| S3 | the held leaf has a right sibling | `OutOfRange` | `btree.cpp:975-981` | **deleted** |
| S4 | heap: below the tail's `min_key` | `OutOfRange` | `heap_chain.cpp:193-201` | kept (BD-Q4); unreachable from SQL (§1.5) |
| S5 | below the mark at admission | `OutOfRange` | `catalog.cpp:2567` | **deleted on a btree**; kept on a heap |
| X | the mark is past 2^40 − 1 (omitted pk) | `OutOfRange` | `catalog.cpp:2475-2481` | unchanged |

Three more facts about these refusals:

- **The code depends on lock state.** The same present key answers
  `AlreadyExists` through S2 and `OutOfRange` through L2, depending on
  whether the borrow conflicted and whether the row's latest writer has
  decided. The header names the latest writer, which can be a deleter or an
  updater (`heap_page.cpp:318-320`).
- **Byte positions.** Only the parse errors and R1-R4 carry the token's
  byte. The KWP error frame's `position` field is never filled
  (`error_registry.cpp:126`).
- **Multi-row statements.** A multi-row `VALUES` runs each row in turn,
  carrying `(row k)` (`command_dispatcher.cpp:4742-4760`).

### 1.5 The heap relation, on any volume this engine mounts

- **No new heap relation can be created.** `CREATE TABLE … HEAP` has been
  refused since SUS-1, 2026-09-05 (`parser.cpp:792-812`).
- **The bypass is a test seam** (`parser.hpp:25-43`). It is not a config
  key, and nothing on KWP or the debug port reaches it.
- **No mountable volume holds an older heap relation.** A volume from
  before SUS-1 is version 17 or older (`superblock.hpp`: 16 → 17 on
  2026-09-05). Version 18 (2026-09-30) refused every older volume, and 19
  refuses 18.
- **So the heap's below-mark refusal is reachable only through the seam.**
  No volume this engine mounts holds a heap user relation, and W8 holds for
  every relation a client can reach.
- **System relations are heap chains, and their keys are engine-issued,
  never named** (BB §1.8).

### 1.6 The texts and tests that state or pin BB-R3

**The text:**

- `CLAUDE.md`:
  - the caller-supplied-pk row (`:73`);
  - invariant 4 (`:138`);
  - invariant 11 (`:145`).
- `heap-and-tuple.md`:
  - §3.1b;
  - §4, §4.1 and §4.1a (`:109-:251`);
  - §5 (`:273-:278`);
  - §8 invariant 4, *"placement never sorts a page"* (`:317`), and invariant
    11 (`:324`).
- `keystoneid-invariant.md`: K1 and K3 (`:16-30`), §1 (`:86-103`), and §2
  (`:117-160`).
- `manual/sql/sql.md`: `:174-193`, `:483`, `:506-522`, `:713-720`, and the
  error table at `:1011-1014`.
- The specs:
  - `txn.md:740-743`;
  - `cabin.md:250-252`;
  - `waystone-concpets.md:152`;
  - `crosscore.md:225-227` and `:272-274`;
  - `wal.md` §5.2 and §12;
  - `join-inner-build.md`, JB4 and JB6.
- `known-gaps.md:493-510`.
- `fk_check.cpp:239-241` (*"by K1 … dead forever"*) and the comment at
  `sim/oracle.hpp:119-123`.
- About 63 source-comment lines in `src/` and `include/` that cite BB-R1,
  BB-R2, BB-R3, BB-R11 or BB-R12.

**Already stale:**

- `assertion.md:714-717` contradicts K3.
- `README.md:72`, `:84`, `:266` and `:294` describe the pre-BB behaviour,
  which is BD's.

**Tests that pin the btree refusal:**

- `supplied_key_test.cpp`:
  - `AKeyInAGapBelowTheMarkIsRefusedAndOrderByStaysKeyOrder`;
  - `AnUnorderedBackfillLandsOnlyTheKeysAboveTheRunningMark`;
  - `ABulkStatementNamingKeysOutOfOrderIsRefusedWhole`;
  - `ANamedKeyBelowTheMarkIsRefusedOnABtree`;
  - `TheMarkThatRefusesAKeySurvivesAcrossDispatchers`.
- `bulk_insert_test.cpp`: the btree arm of
  `ABelowMarkKeyIsRefusedWithItsOrdinalOnEveryRelation`.
- `btree_test.cpp`:
  - `ANamedKeyUnderALeafWithARightSiblingIsRefusedWithoutAskingTheMark`;
  - `ANamedKeyTheMarkRefusesPlacesNothing`.
- `catalog_test.cpp`:
  - `ARefusedBelowMarkKeyMovesNoWordAndDropsNoCache`;
  - `ABtreeRelationIsRefusedABelowMarkKeyAsAHeapIs`.
- `lock_family_test.cpp`:
  - the absent arm of `AnIllegalKeyIsRefusedWithoutWaitingOnAFenceOnABtree`;
  - `AKeyAnUndecidedWriterPlacedIsRefusedBelowTheMarkWithoutWaiting`;
  - the absent arm of `ANamedKeyRefusedAfterItsValueSpilledLeavesTheRelationWhole`.
- `insert_log_crash_rig_test.cpp`:
  `AKeyThatWouldAppendMidChainIsRefusedAndLeavesTheChainWhole`.
- `insert_wal_test.cpp`:
  `AKeyThatWouldDivideALeafIsRefusedAndLogsNoImageNoInitAndNoInsert`.
- `cabin_contract_test.cpp`: the absent arm of
  `ANamedKeyBelowTheMarkIsRefusedAndNoCabinSetWitnessesIt`.
- `expeditor_test.cpp`: `AVolumeHoldingARelationWithKeysOutOfOrderIsRefusedByName`.

**The cells BB-S3 reshaped** are listed in BB §6's BB-S3 count table, with
what each stopped covering.

**The sim never names a key.**

- Its `INSERT` always omits the pk (`sim/workload.cpp:102-112`).
- Its oracle has no mark, no `AlreadyExists` and no `OutOfRange`
  (`sim/oracle.hpp:117-125`).
- Its one order check is `kSlotOrder` (`sim/integrity.cpp:260-265`).

### 1.7 Why a rolled-back key can be named again (W12), and what that costs

**The rolled-back tuple was never visible to another snapshot.** Everything
that keeps `(oid, pk)` past the write verifies against the row now there:

- **an index:** IX1's superset re-checks the key predicate on the resolved
  version, and defers to the base row (`index.md:38-47`, `:342`);
- **a Cabin and the FK Cabin path:** MVCC plus the key-column re-check
  (`cabin.md:150-153`, `fk_check.cpp:269-286`);
- **a Waystone trail:** it replays only the derived key's lookup (rule 0,
  `waystone-concpets.md:36-37`);
- **the inner build:** `AcceptTupleAt` re-checks the row;
- **assertions:** they are keyed by group, not by pk.

Three more things keep the reused key apart from the old one:

- **The new row starts no chain.** Its `undo_ptr` is 0, so it never reaches
  the old row's undo chain.
- **The lock is released only after the compensation**
  (`txn/manager.cpp:563-600`).
- **Only text goes stale.**

**The cost.**

- A rollback leaves the insert's index entries.
- A byte-identical entry is never stored twice (`FindExactDuplicate`,
  `index_tree.cpp:376-386`).
- But a re-insert of the same key with different covered bytes, or a run
  that straddles a leaf, grows the run of equal sort keys. That is
  `docs/inflight/bugs/a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`,
  which today only an `UPDATE` run reaches.
- **When that bug fires**, `IndexInternalView::InsertEntry` answers
  `AlreadyExists` for a key that is not a duplicate. That is the one known
  exception to BD-R5 while the bug stands, and BD does not fix it.

### 1.8 A split's records are not replayed as one

**How a split is logged:**

- **The divide** logs its new leaf's image, then the old leaf's image, and
  then each parent's image as `PromoteSeparator` writes it
  (`btree.cpp:847-863`).
- **The append split** logs the new leaf's record, then the ancestors, then
  the old leaf's image carrying the link (`btree.cpp:1148-1153`). Its
  comment says the order among the images *"is not load-bearing"*.

That holds only if the durable log never ends between two of a split's
records. Nothing in the tree makes a split's records one unit. Found by
reading, not reproduced:

- **The divide.** If the log ends after the old leaf's image and before its
  parent's, the moved half is linked but unrouted. Every point access to
  `[split_key, next separator)` restarts until `TxnConflict`, for good.
- **The append split (pre-existing).** If the log ends after the parent's
  image and before the old leaf's link, later inserts land on a leaf that
  no walk reaches. That is a quiet wrong answer.

**Not checked:** the secondary index tree's splits, which also log as
images (`command_dispatcher.cpp:4171-4180`), may share the shape. The sim
cannot cut the log inside a statement's records (`sim/faults.hpp:31-41`).

## 2. Rulings — CLA's proposals, marked where §4 says so

**BD-R1 — the invariants, restated (W3).**

- **Within a leaf.** Every slot of a btree leaf that carries a key ascends
  by Keystone id, live rows and delete-marked rows alike. That holds
  whatever order rows arrive in, and at every core count.
- **What keeps it.** Placement (BD-R2), not issue order.
- **Retired slots.** A retired slot carries no key and sits anywhere.
- **No exceptions.** No flag records one, and `kUnordered` stays deleted.
- **Invariant 4 is restated.** A heap page is unordered by contract. A
  btree leaf is sorted by placement, which is no longer *"placement never
  sorts a page"*.
- **Invariant 11, K1 and K3 are restated** by BD-R4 and BD-R7.

**BD-R2 — sorted placement (W2, W4; BD-Q1 (a)).**

- **Where a row goes.** Under the exclusive hold of the leaf the descent
  lands on, a row takes the slot index its key sorts to:
  - the directory entries at and after that index move up one (5 bytes
    each);
  - no tuple byte moves;
  - the tuple's bytes go where `InsertTuple` puts them today.
- **One search answers three questions.** A single `lower_bound` over the
  keyed slots answers whether the key is present, the insertion index, and
  whether this is an append or a divide. It replaces:
  - `RefuseDuplicate`'s linear scan (`btree.cpp:892-905`);
  - `MaxLiveId` (`:245-255`);
  - `FindSlotForId` and its fallback (`:279-316`).
- **The divide** merges the incoming row into its already-sorted vector and
  cuts once. Its `std::sort` and its "incoming row last" branch go. As
  today, it carries delete-marked rows and drops retired slots.
- **The storage contract `BtreeInsert`** places sorted too, so no leaf is
  ever unsorted. `BtreeTest.ALookupFindsAnIdInALeafWhoseSlotsAreOutOfOrder`
  is deleted with the fallback.
- **Cost at `cores = 1`.** An append moves nothing. At `cores = 1` every
  omitted pk appends, and so does every ascending named key.
- **Cost at `cores > 1`, with BB-R1 deleted.** An id issued before a higher
  one that was placed first lands below that one and shifts it [inferred].
  On a full rightmost leaf that is a divide: two 8 KiB images, an epoch
  bump, and a left half that later ids never refill. BD-Q11 asks for the
  remedy.

**BD-R3 — every holder of a btree row's slot made shift-safe (§1.2).** This
lands before the gate opens (§5).

- **E1 — recovery undo re-finds the row by pk** (BD-Q8 (a)).
  - **Where it looks:** on a `kBtreeLeaf` page only, in the recorded leaf,
    then rightward along the chain while `min_key ≤ pk`. Keys only move
    right, and leaves never merge (`btree.cpp:1218-1223`).
  - **Heap and catalog pages** keep today's exact identity check, since they
    never shift.
  - **The premise.** Each key is in at most one slot of a leaf, once E5 is
    closed, and the WAL keeps order. A re-insert after the loser's live
    rollback cannot be on disk while the loser is still a loser.
  - **What it compensates:** only a version the loser wrote, judged by the
    header's `trx_id`. That check is also the idempotence rule for a crash
    during undo: a version that does not name the loser was already
    compensated, and counts as done.
  - **When an insert counts as done:** when its key is in no leaf on that
    path, or when the recorded page is missing (H1's arm, kept).
  - **Scope:** this applies to `kInsert`, `kOverwrite` and `kDeleteMark`
    alike.
- **E2 — a record kind of its own, and a strict redo.**
  - `BTREE_INSERT` is appended to the enum as 29, completing `wal.md` §5.2's
    name. Its payload is `{trx_id, undo_ptr, slot, tuple_len}` plus the
    tuple.
  - **What a leaf insert logs.** It logs a `BTREE_INSERT`, unless the same
    insert's split imaged the leaf. Then it logs no insert record, as the
    index split logs no `INDEX_INSERT`. Redo then has no re-application
    branch.
  - **Its redo** checks that `slot ≤ nr_slots` and that the key sorts
    strictly between its new neighbours, and refuses `Corruption`
    otherwise. Only then does it move the entries up and write the row.
  - `HEAP_INSERT` stays the heap pages' record, and its redo is unchanged.
- **E3 — the inner-build probe** goes through `VerifyTupleAt`, with a pk
  lookup on a miss, on a btree.
- **E4 — the JB6 mark** becomes a key on a btree, resuming after the last pk
  visited.
- **E5 — an unlogged placement is taken back in place.**
  - **Where:** at every failure exit between the placement and its record:
    index maintenance, the assertion reservation, the undo append.
  - **How:** the still-held leaf loses that slot (its entry removed, the
    entries after it moved down) and gives back the tuple's bytes. Those are
    the last bytes allocated on that leaf under the hold, as
    `UnInsertTuple` assumes for its last slot.
  - **What is not the fix:** a logged `SLOT_RETIRE` for a row that was never
    logged. Under shifts its slot would name another row at redo.
- **The epoch.** A shift does not bump `relayout_epoch`. The pk check
  catches a shifted hint, and a bump would also miss the hints left of the
  insertion point.
- **F.** The reply's `slot=` is documented as the slot at placement. The
  comment at `btree.cpp:690-692` is corrected: recovery reads `target_slot`.

**BD-R4 — what keeps an id issued once (W6, revised by W12; BD-Q2).**

- **An id is consumed when a tuple carrying it commits.**
  - A committed key stays in its leaf for the life of the relation: live,
    then delete-marked (§1.3).
  - **The delete-marked row is today's tombstone**, so BD writes no new
    on-page state.
- **A rolled-back key is free (W12).**
  - Both rollbacks retire its slot keyless, and a divide compacts it.
  - BD-R5's duplicate check does not see it.
  - §1.7 is why that is sound.
- **An id issued and burned without placement is free too.**
- **What a purge owes, when one is built.** It must leave a keyed,
  never-visible tombstone in the row's key position, or reclaim nothing.
  - The duplicate check and the placement order read that tombstone.
  - Walks and `VerifyTupleAt` treat it as absent.
  - This is recorded in `heap-and-tuple.md` §4.1, in K1, and in
    `known-gaps.md`.
- **K1, restated:** *"A Keystone id is bound to at most one committed tuple
  in the lifetime of a relation."*

**BD-R5 — the two reasons (W5, W7, W8; BD-Q3 (a), BD-Q9 (a)).**

A named key's `INSERT` is refused because of its pk for **exactly two
reasons**. Each has one code, and each refusal carries the pk token's byte
in its message, as `(byte N)`, and `(row k)` in a multi-row statement. The
KWP `position` field stays as it is (§1.4).

1. **Duplicate — `AlreadyExists`.**
   - **When the borrow is granted:** with this statement holding the row
     unit, any slot keyed `k` in the held leaf is a committed row or the
     transaction's own. Either is a duplicate.
   - **The text says which:** *"duplicate primary key k"*, or *"… k: a row
     with this key was deleted; a Keystone id is bound once"*.
   - **When the borrow is refused:** `before_wait` (`refuse_if_never_admissible`,
     `command_dispatcher.cpp:5177-5199`) keeps today's `IsInFlight` test and
     loses only its mark branch.
     - A slot keyed `k` whose latest writer has decided is a duplicate at
       once.
     - Where that writer is in flight, the row's undo chain tells an
       undecided **insert** from an undecided update or delete of a
       committed row. That walk runs on the refusal path only.
     - **An undecided insert is not yet a duplicate**, because its rollback
       frees `k` (W12). The named insert waits for its decide, as an
       ordinary lock wait on the row unit. The re-run answers
       `AlreadyExists` if it committed, or places the row if it rolled
       back (BD-Q9).
     - **A committed row under an undecided update or delete** is a
       duplicate at once.
     - **Every other refused borrow becomes a wait**: a fence over `k`, for
       example.
   - **Lock-family rules.** The wait is compatible with AO-S6c-c (a key
     that can never be admitted never waits), with `REPEATABLE READ`
     (`txn.md:736-743` lets an `INSERT` wait whatever the blocker), and with
     deadlock detection.
2. **Exhausted — `OutOfRange`.** `k` is outside the Keystone id space,
   `[kFirstRowId, kMaxKeystoneId]` = `[1, 2^40 − 1]`:
   - zero, a negative number, a number above `2^40 − 1`, or a literal the
     lexer cannot hold;
   - R2-R4 move from `InvalidArgument`;
   - the lexer's overflow becomes defined, and the check reuses
     `src/exec/row_codec.cpp:655-666`'s wrapped-literal test rather than the
     wrapped value;
   - the omitted-pk exhaustion (X) is the same reason, and keeps its code.

**What else holds:**

- **Nothing else about the key refuses it, on a btree.** L2, S3 and S5 are
  deleted there.
  - W8 holds: after `pk = 100`, `pk = 99` is placed.
  - A multi-row `VALUES` names its keys in any order.
  - A key named twice in one statement is a duplicate at its second
    occurrence.
- **Not because of the pk, and unchanged:** P1-P3, R0, R1, L0, L3-L3b, S1,
  a foreign key, an assertion, and `OutOfSpace`.
- **The known exception:** the equal-entries bug's `AlreadyExists` (§1.7),
  until that bug is fixed.
- **A key below the mark can now meet a fence.** An open `DELETE … WHERE
  id < 50` makes a named insert of 10 wait.

**BD-R6 — BB-R1 deleted (W10; BD-Q6), and what that reaches.**

- **The rule.** *"A row's id is fixed under the exclusive hold of the page
  it lands on"* is withdrawn on a btree. Its one reason was a leaf's order
  (BB §0, defect A), which BD-R2 now keeps.
- **An omitted pk:**
  1. issue (`AllocateRowId`);
  2. borrow;
  3. encode;
  4. descend for the id;
  5. place.

  A refused borrow burns the id, which is then free (BD-R4).
- **A named key:**
  1. the exhausted check;
  2. borrow;
  3. encode;
  4. admit, which moves the mark past `k` if `k` is at or above it, and never
     refuses;
  5. descend;
  6. the duplicate check;
  7. place.

  **Admitting before the descent is safe.** Every placed id is below the
  mark, so a key at or above it cannot be a duplicate, and two statements
  naming one key serialise on its row unit.
- **What goes, on a btree:**
  - `BtreeInsertIssued` and `BtreeInsertNamed`. Both reduce to `BtreeInsert`
    (`btree.cpp:918-928`), so they are deleted, not folded, and
    `PlaceUnderHold`'s `duplicate_scanned` parameter goes with them.
  - the descent for `kMaxKeystoneId`;
  - the issue and the admission under the leaf's hold.
- **What the heap keeps** (BD-Q4 (a)):
  - `IssueUnderHold` and `AdmitUnderHold`, its entry points
    (`heap_chain.cpp:168`, `:183`);
  - `Catalog::RowIdMark`;
  - `RefuseRowIdBelowMark`, its S5 and L2.

  Without them a heap tail would take a key below its highest id, against
  BB-R7's per-page order. **The cost:** a second insert protocol stays alive
  for a relation type no mountable volume holds.
- **What else stays:**
  - **BB-R4's declared order.** The reverse-FK path still asks `sys.tables`
    under a user page (BB §1.6).
  - **BB-R5's "nothing under the insert's hold parks"**, restated without
    the issue.
  - **BB-R6's sweep**, extended (BD-R9).
- **BB-S2's rig cells keep their subjects** and pass by placement. When 101
  is placed before 100, `ORDER BY id` still answers 100, 101.
  `IdAllocationAcrossCores`'s per-retry burn assertion is re-pinned to the
  restored order.

**BD-R7 — the mark, after BD.**

- **What it stays:**
  - the issue cursor for omitted keys, above every id ever placed, so an
    issued id needs no page read;
  - K4's budget;
  - what `DESCRIBE` and `SHOW BUDGET` read.
- **What still moves it:** a named key at or above it, to `k + 1`, logged
  outside the transaction.
- **What it no longer does:** it refuses nothing on a btree.
- **Invariant 11 and K3, restated:**
  - Issued keys are a sequence in issue order.
  - Named keys carry no order.
  - A leaf's slot order is its key order by placement.
  - "Uniqueness needs no page read" holds for issued keys only.

**BD-R8 — superblock 20 (W9; BD-Q5).**

- **The bump.** `kSuperBlockVersion` moves 19 → 20, and a version-19 volume
  is refused at mount with no migration. `superblock.hpp` gains the 19 → 20
  note.
- **The reason.** A version-19 volume an engine before `1b5d252e` wrote at
  `cores > 1` can hold a leaf out of key order (`known-gaps.md:493-510`).
  Sorted placement and a fallback-free search are unsafe on such a leaf.
  The bump closes that entry at BD-S5, and W9 asks no more of it.
- **BB-R11's check is deleted:** `Catalog::RefuseRelationsHoldingKeysOutOfOrder`
  (`expeditor.cpp:917-924`) and its cell. No version-20 volume can carry the
  byte it reads set. The byte stays reserved, written 0.

**BD-R9 — checked, not only argued.**

- **The sim.**
  - **Workload:** it names keys ascending, descending, random, below the
    mark, a deleted key again, and a rolled-back key again.
  - **Oracle:** a committed key is `AlreadyExists`, a key outside the space
    is `OutOfRange`, and a rolled-back key is placed.
  - **Sweep:** `kSlotOrder` covers every keyed slot.
- **The rigs, on two cores:**
  - named keys interleaved into one leaf;
  - a shift under the other core's undecided row, then its rollback;
  - a named insert waiting on an undecided inserter, both arms;
  - a committed row under an undecided update, refused at once;
  - E3 and E4 under a concurrent shift;
  - BD-R2's rightmost-leaf divide at `cores > 1`.
- **Crash cells:**
  - §1.2's silent case: 50, then 40, then the crash;
  - a loser moved by a divide;
  - E5: a mid-leaf placement whose index maintenance fails, then the crash;
  - **a log cut inside a split's records**, for the append split, the divide
    and an internal node's divide (BD-R12), and the same cut for an index
    split (§1.8).
- **Mutations, each repeated:**
  - placement at `nr_slots`;
  - the divide appending its row last;
  - recovery undo by slot alone;
  - `BTREE_INSERT` redo without its neighbour check;
  - E5's take-back removed;
  - `before_wait` refusing an undecided insert;
  - the exhausted check reading the wrapped value;
  - `ProbeBuild` without `VerifyTupleAt`;
  - JB6 by ordinal;
  - a split replayed in part.
- **§1.6's pinning cells** are flipped, and BB-S3's reshaped cells get their
  subjects back.

**BD-R10 — not measured (W10; BD-Q7).**

- The close-out measurement is waived.
- **Nothing BD lands is measured.** Each stage says so as a fact, and never
  as a pass.

**BD-R11 — the text.**

- **Every text in §1.6 is restated.**
- **Corrected on the way:**
  - `txn.md:937-939`: a delete-mark rollback logs `HEAP_DELETE_UNMARK`
    (`txn/manager.cpp:538-550`), and no rollback clears a Waystone entry;
  - `bulkinsert.md:96-98`'s removed pk-column wording.
- **Recorded:**
  - BD-R4's purge obligation, and §1.8's index-split question, in
    `known-gaps.md`;
  - §1.7's second path, in the equal-entries bug entry.
- **Done when** a grep of `src/ include/ docs/spec/ docs/rules/ manual/
  CLAUDE.md` for `BB-R1`, `BB-R2`, `BB-R3`, `BB-R11` and `BB-R12` finds
  each remaining hit stating history rather than a rule.

**BD-R12 — a split replays whole or not at all (§1.8; BD-Q10).**

- **What it covers:** every split of the clustered tree, whether an append
  split, a mid-chain append split, a leaf divide or an internal node's
  divide. Under BD each one is reachable from SQL.
- **Proposed:**
  - one `BTREE_SPLIT` record, appended as kind 30, carrying every page image
    the split writes;
  - redo applies it whole, with each page gated by its own `page_lsn`;
  - its CRC makes it atomic: a torn record ends the log before it.
- **Size.** The record must fit a segment, as an `ASSERT_SNAPSHOT` chunk
  must.
- **The append split's exposure predates BD.** BD-S1's cut-off cell decides
  whether it reproduces. If it does, it gets a bug entry at that stage, and
  BD-R12 fixes it.
- **The index tree's splits** get the same cell (§1.8). If it reproduces, a
  bug entry is filed, and its fix is a separate order.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BD-S0 | **The order** | this file, its review, and the marks recorded | S |
| BD-S1 | **Red first** | <ul><li>**Red at BD-S0's commit, through SQL:**<ul><li>`pk = 100`, then `pk = 99`, placed, and `ORDER BY id` elided and ascending (W8);</li><li>a descending multi-row `VALUES` placed;</li><li>a rolled-back key named again and placed (W12);</li><li>a named insert waiting on an undecided inserter, both arms;</li><li>a committed key under an undecided update, `AlreadyExists` at once (L2 answers `OutOfRange` today);</li><li>`0`, `-1`, `2^40` and `2^64 + 5`, each `OutOfRange` with the token's byte;</li><li>the census cell: every pk-caused refusal is `AlreadyExists` or `OutOfRange`.</li></ul></li><li>**Red through the storage contract and the crash rig:**<ul><li>the divide's incoming row out of order;</li><li>§1.2's silent recovery case;</li><li>E5's misaligned redo;</li><li>a mid-leaf `HEAP_INSERT` replayed as an overwrite;</li><li>a log cut inside a divide's records.</li></ul></li><li>**Decided here:**<ul><li>the append split's and the index split's cut-off cells (§1.8), with a bug entry filed for each that reproduces.</li></ul></li><li>**Guard, green and kept green:**<ul><li>a deleted key named again is `AlreadyExists`.</li></ul></li></ul> | M |
| BD-S2 | **Sorted placement, the shift-safe holders, the whole split** (BD-R2, BD-R3, BD-R8, BD-R12) | <ul><li>**Code:**<ul><li>insert-at-slot, and the one `lower_bound`;</li><li>the merged divide;</li><li>`BtreeInsert` sorted, with the fallback and its cell gone;</li><li>`BTREE_INSERT`, with its strict redo and no record after a split's image;</li><li>`BTREE_SPLIT`;</li><li>recovery undo's re-find;</li><li>E5's take-back;</li><li>`ProbeBuild`'s verify, and JB6's key mark;</li><li>superblock 20, and BB-R11's check deleted.</li></ul></li><li>**Still refused:** SQL refuses below the mark until S3, so S1's storage and crash cells are what turn green here.</li><li>**Green:**<ul><li>the golden log, re-pinned;</li><li>the suite;</li><li>the contract suites, byte-for-byte.</li></ul></li></ul> | L |
| BD-S3 | **The gate opened, and BB-R1 deleted** (BD-R4..R7) | <ul><li>**Deleted on a btree:**<ul><li>L2, S3 and S5;</li><li>the two doors;</li><li>the issue and the admission under the hold.</li></ul></li><li>**Changed:**<ul><li>admit is advance-or-nothing;</li><li>`before_wait` as BD-R5 states it;</li><li>exhausted on the literal, with the byte;</li><li>the two texts.</li></ul></li><li>**The heap's protocol untouched.**</li><li>**Cells:**<ul><li>S1's SQL cells green;</li><li>§1.6's pinning cells flipped;</li><li>BB-S3's reshaped cells restored.</li></ul></li><li>**Green:** the suite.</li></ul> | L |
| BD-S4 | **The sim and the rigs** (BD-R9) | <ul><li>the workload and the oracle;</li><li>the rigs and the crash cells;</li><li>the mutations, each killed on every run.</li></ul> | M |
| BD-S5 | **The text** (BD-R11) | every row of BD-R11, the grep included, and `known-gaps.md:493-510` closed | M |
| BD-S6 | **The close** | <ul><li>a row per stage, and what BD carries;</li><li>BA's rows rebased (BD-Q12);</li><li>**no measurement** (BD-R10).</li></ul> | S |

## 4. Items for the operator

| item | what | class | CLA's proposal | mark |
|---|---|---|---|---|
| BD-Q0 | **Open BD** with BD-S0..S6 and BD-R1..R12 as written | process | Yes | — |
| BD-Q1 | **How a leaf stays in key order** (BD-R2).<br>(a) Shift the directory and change what addresses a slot.<br>(b) Fixed slot numbers, plus a key-order array | invariant | (a) | **(a)**, W4 |
| BD-Q2 | **What keeps an id issued once** (BD-R4) | invariant, user-visible | **First offered:** a keyed tombstone for every slot that ever held a tuple. **Revised for W12:** an id is consumed when its tuple commits, the delete-marked row is today's tombstone, a rolled-back key is free, and a purge owes a keyed tombstone | **as proposed** (W6), **revised by W12** |
| BD-Q3 | **The reading of W5's two reasons** (BD-R5).<br>(a) Duplicate is a slot keyed `k` (`AlreadyExists`); exhausted is outside `[1, 2^40 − 1]` (`OutOfRange`), with `InvalidArgument` folded in.<br>(b) Present-now against used-before, with out-of-space values left `InvalidArgument` | user-visible | (a) | **(a)**, W7 |
| BD-Q4 | **The heap relation.**<br>(a) Untouched: BB-R7, its two entry points, the mark's refusal and its ordinal mark stay, and §1.5 records that SQL cannot reach a heap on a mountable volume.<br>(b) The heap follows BB-R1's deletion, and an `ORDER BY <pk>` over a heap sorts | scope | (a). SUS-1 deletes no heap code, a heap page cannot sort itself (invariant 4), and BB-R7 carries the orphaned-page fix. **Cost:** a second insert protocol stays alive for a relation type no mountable volume holds | **as proposed: (a)**, W13 |
| BD-Q5 | **The format** (BD-R8): superblock 20, with version 19 refused | format | — | **refuse**, W9 (standing) |
| BD-Q6 | **BB-R1** (BD-R6) | invariant | Keep it, and leave its retirement to BA | **deleted**, W10 |
| BD-Q7 | **The close-out measurement** (BD-R10) | process | Measured once at the close | **waived**, W10 |
| BD-Q8 | **How recovery undo re-finds a row** (BD-R3 E1).<br>(a) Scan the recorded leaf, and rightward while `min_key ≤ pk`.<br>(b) Add `rel_oid` to the undo record, and use the live locator | format, invariant | (a). It needs no catalog during undo. (b) is one mechanism for both rollbacks, and free under W9, but it needs the catalog readable during undo, which is unchecked | **as proposed: (a)**, W13 |
| BD-Q9 | **A named key meeting an undecided insert of the same key** (BD-R5).<br>(a) Wait for its decide, and answer by the outcome.<br>(b) `AlreadyExists` at once | user-visible | (a). Under W12 a rollback frees the key | **as proposed: (a)**, W13 |
| BD-Q10 | **How a split replays whole** (BD-R12).<br>(a) One `BTREE_SPLIT` record carrying every image the split writes.<br>(b) A group of records that redo applies only once the group's closing record is durable | format, **[quiet-wrong] if neither** | (a). One record is atomic by its CRC, and redo gains no buffering | — |
| BD-Q11 | **A rightmost leaf's split at `cores > 1`** (BD-R2).<br>(a) Split a full rightmost leaf at the insertion point rather than the median, so ids arriving out of issue order leave the left leaf full.<br>(b) The median, as today | performance shape | (a). With BB-R1 deleted, the median divide on the rightmost leaf leaves half-full leaves that no later id refills | — |
| BD-Q12 | **Which order carries BB-S5's rebase of BA.**<br>(a) BB closes first, rebasing BA against BB's rules.<br>(b) BB closes on the measurement it has, and BD-S6 rebases BA once, against BD | process | (b). BD withdraws the BB rules that rebase reads, so rebasing against BB would be done twice | — |

## 5. Sequencing

1. **BD-S2 before BD-S3.** Opening the gate before the holders in §1.2 are
   shift-safe, and before a split replays whole, makes §1.2's silent
   recovery case, E5's misaligned redo and §1.8's half split reachable from
   SQL.
2. **BD-S1, then S2, S3, S4, S5 and S6.** S4 may start once S3 has landed.
3. **BB and BA.** BA stays paused until BB closes (BD-Q12). BA's rows
   rebased against BD:
   - **BA-R8's last bullet:** the hook is gone. That stands.
   - **BA-R8's named-key bullet:** *"a btree now refuses below the cursor
     too"* is withdrawn.
   - **BA-R11's "who it reaches":** the encode precedes the descent again
     (BD-R6). BA's original text stands, and BB's draft rebase of it is
     dropped.
   - **BA-S11's row:** it rests on BD-R6, not BB-R1. BA-R8's cursor no
     longer runs inside a leaf's hold.
4. **File overlap.** BD-S3 and BA-S11 both touch `AllocateRowId` and the
   insert pipeline. Whichever lands second is written against the first.

## 6. Row status

### BD-S0 — written 2026-10-07

- **Where:** on `worktree-bd-sorted-leaf-named-keys` from `bf8ea937`, on
  W1-W14.
- **How §1 was made:** read, not run. It comes from three censuses and the
  review below. *[inferred]* marks where a conclusion is CLA's.
- **The marks:**
  - W4, W6 and W7 settle BD-Q1, BD-Q2 and BD-Q3;
  - W9 settles BD-Q5;
  - W10 settles BD-Q6 and BD-Q7;
  - W12 revises BD-Q2;
  - W13 settles BD-Q4, BD-Q8 and BD-Q9.

  All are in `raft-marks-2026-10-07.md` §7-§13.
- **Waiting:** BD-Q0, BD-Q10, BD-Q11 and BD-Q12.
- **No engine file moved, and no suite ran** (W11).

### BD-S0's review - 2026-10-07

One `critics-developer` pass over the uncommitted order, read against the
tree. It found **3 high, 9 medium and 9 low findings, plus 5
simplifications**, and corrected four line numbers in §1.6 and BD-R8 itself.

**Applied:**

- **H1, the unlogged placement under shifts:** it became E5, with
  `BTREE_INSERT`'s strict redo.
- **H2, the split's records are not one unit:** it became §1.8, BD-R12 and
  BD-Q10. The pre-existing append-split exposure was found by reading, is
  not reproduced, and is left to BD-S1's cell.
- **H3, the heap's protocol would have been deleted under BD-Q4 (a):** each
  deletion is scoped to a btree, and BD-Q4 states the cost.
- **M1, the median divide at `cores > 1`:** BD-R2 and BD-Q11.
- **M2, BD-R8's load-bearing reason:** `known-gaps.md:493-510`.
- **M3, E1's precision:** the btree leaf only, the `trx_id` idempotence
  rule, and the premise.
- **M4, E2 by the index precedent:** no re-application branch.
- **M5, `before_wait`, in part:** the own-transaction clause goes, and the
  granted borrow needs no commit-state test.
- **M6:** the index cost is corrected, its `AlreadyExists` exception is
  named, and the stale texts are listed.
- **M7, invariant 4:** restated.
- **M8, BB and BA:** BB-R2 and BB-R3's order are named, there are four BA
  rows, and BD-Q12 is added.
- **M9:** the stage table.
- **Every low finding.**
- **Simplifications:**
  - the two doors are deleted, not folded;
  - one `lower_bound` replaces three scans;
  - the divide's merge;
  - no insert record after a split's image;
  - five document cuts.

**Declined:**

- **M5's other half: waiting on every in-flight latest writer.** BD-R5
  answers a committed row under an undecided update or delete `AlreadyExists`
  at once. The undo-chain walk that tells an insert apart runs only on the
  refusal path, and BD-Q9's mark is about an undecided *insert*.
- **Keeping only one of the header's status and §6.** The header carries
  three status lines, as every order's does, because other orders and the
  index read it. §6 carries the history.
