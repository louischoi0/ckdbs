# Work order BD — a named key lands where it sorts: every btree leaf kept in key order by placement, and no high-water-mark refusal

Written 2026-10-07 on `worktree-bd-sorted-leaf-named-keys` from `bf8ea937`
(`v2.7.0-652-gbf8ea937`), on the operator's words below. All are verbatim,
from one session, and recorded in `raft-marks-2026-10-07.md` §7-§15.

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
- **W15:** *"Q10, Q11, Q12 제안대로 마킹해줘"*
- **W16:** *"BD-Q0 열어줘, 워크트리는 keep-btree-leaf-slots로"*

**Status: closed 2026-10-07 at BD-S6 (W16, §16 of `raft-marks-2026-10-07.md`); not pushed.**

- §4's mark column says which items the words settle; every item is marked.
- BD runs on `worktree-keep-btree-leaf-slots`, from `50d35916`.
- BD-S0 itself moved no file under `src/`, `include/` or `tests/` (W11).

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
By BD-Q12 (b), BB closes on the measurement it has, and BD-S6 rebases BA
once, against BD (§5).

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
- Its writer has aborted, so no reader sees it. **It is not harmless
  today**, as this paragraph first said: BD-S1's cell
  `SortedLeafCrashTest.ARowPlacedAndNeverLoggedLeavesNoHoleForTheNextRecordsRedo`
  places an unlogged row by ascending keys, logs the next insert on the
  same leaf at the slot after it, and the mount is refused (*"redo names
  slot 4 on a page holding 3"*). Under BD it breaks two more things
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
  On a full rightmost leaf, a median divide would leave a left half that
  later ids never refill.
- **The rightmost leaf splits at the insertion point (BD-Q11 (a)).**
  - The keys above the incoming one move to the new leaf, and the left leaf
    stays full.
  - A named key far below the leaf's top makes the left leaf sparse
    instead. That costs space, never correctness.
  - A leaf with a right sibling divides at the median, as today.

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
  - the rightmost leaf's insertion-point split at `cores > 1` (BD-Q11).
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
  - the rightmost leaf split at the median;
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
- **Marked (BD-Q10 (a)):**
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
| BD-S2 | **Sorted placement, the shift-safe holders, the whole split** (BD-R2, BD-R3, BD-R8, BD-R12) | <ul><li>**Code:**<ul><li>insert-at-slot, and the one `lower_bound`;</li><li>the merged divide;</li><li>`BtreeInsert` sorted, with the fallback and its cell gone;</li><li>`BTREE_INSERT`, with its strict redo and no record after a split's image;</li><li>`BTREE_SPLIT`, and the rightmost leaf's insertion-point split;</li><li>recovery undo's re-find;</li><li>E5's take-back;</li><li>`ProbeBuild`'s verify, and JB6's key mark;</li><li>superblock 20, and BB-R11's check deleted.</li></ul></li><li>**Still refused:** SQL refuses below the mark until S3, so S1's storage and crash cells are what turn green here - except the E2 and divide-cut crash cells, which reach their key through SQL and turn green at S3 (BD-S1's review, finding 2). S2's own proof for those two is a redo-level `BTREE_INSERT` neighbour-check cell and a `BTREE_SPLIT` whole-or-nothing cell.</li><li>**Green:**<ul><li>the golden log, re-pinned;</li><li>the suite;</li><li>the contract suites, byte-for-byte.</li></ul></li></ul> | L |
| BD-S3 | **The gate opened, and BB-R1 deleted** (BD-R4..R7) | <ul><li>**Deleted on a btree:**<ul><li>L2, S3 and S5;</li><li>the two doors;</li><li>the issue and the admission under the hold.</li></ul></li><li>**Changed:**<ul><li>admit is advance-or-nothing;</li><li>`before_wait` as BD-R5 states it;</li><li>exhausted on the literal, with the byte;</li><li>the two texts.</li></ul></li><li>**The heap's protocol untouched.**</li><li>**Cells:**<ul><li>S1's SQL cells green;</li><li>§1.6's pinning cells flipped;</li><li>BB-S3's reshaped cells restored.</li></ul></li><li>**Green:** the suite.</li></ul> | L |
| BD-S4 | **The sim and the rigs** (BD-R9) | <ul><li>the workload and the oracle;</li><li>the rigs and the crash cells;</li><li>the mutations, each killed on every run.</li></ul> | M |
| BD-S5 | **The text** (BD-R11) | every row of BD-R11, the grep included, and `known-gaps.md:493-510` closed | M |
| BD-S6 | **The close** | <ul><li>a row per stage, and what BD carries;</li><li>BA's rows rebased (BD-Q12);</li><li>**no measurement** (BD-R10).</li></ul> | S |

## 4. Items for the operator

| item | what | class | CLA's proposal | mark |
|---|---|---|---|---|
| BD-Q0 | **Open BD** with BD-S0..S6 and BD-R1..R12 as written | process | Yes | **yes**, W16 |
| BD-Q1 | **How a leaf stays in key order** (BD-R2).<br>(a) Shift the directory and change what addresses a slot.<br>(b) Fixed slot numbers, plus a key-order array | invariant | (a) | **(a)**, W4 |
| BD-Q2 | **What keeps an id issued once** (BD-R4) | invariant, user-visible | **First offered:** a keyed tombstone for every slot that ever held a tuple. **Revised for W12:** an id is consumed when its tuple commits, the delete-marked row is today's tombstone, a rolled-back key is free, and a purge owes a keyed tombstone | **as proposed** (W6), **revised by W12** |
| BD-Q3 | **The reading of W5's two reasons** (BD-R5).<br>(a) Duplicate is a slot keyed `k` (`AlreadyExists`); exhausted is outside `[1, 2^40 − 1]` (`OutOfRange`), with `InvalidArgument` folded in.<br>(b) Present-now against used-before, with out-of-space values left `InvalidArgument` | user-visible | (a) | **(a)**, W7 |
| BD-Q4 | **The heap relation.**<br>(a) Untouched: BB-R7, its two entry points, the mark's refusal and its ordinal mark stay, and §1.5 records that SQL cannot reach a heap on a mountable volume.<br>(b) The heap follows BB-R1's deletion, and an `ORDER BY <pk>` over a heap sorts | scope | (a). SUS-1 deletes no heap code, a heap page cannot sort itself (invariant 4), and BB-R7 carries the orphaned-page fix. **Cost:** a second insert protocol stays alive for a relation type no mountable volume holds | **as proposed: (a)**, W13 |
| BD-Q5 | **The format** (BD-R8): superblock 20, with version 19 refused | format | — | **refuse**, W9 (standing) |
| BD-Q6 | **BB-R1** (BD-R6) | invariant | Keep it, and leave its retirement to BA | **deleted**, W10 |
| BD-Q7 | **The close-out measurement** (BD-R10) | process | Measured once at the close | **waived**, W10 |
| BD-Q8 | **How recovery undo re-finds a row** (BD-R3 E1).<br>(a) Scan the recorded leaf, and rightward while `min_key ≤ pk`.<br>(b) Add `rel_oid` to the undo record, and use the live locator | format, invariant | (a). It needs no catalog during undo. (b) is one mechanism for both rollbacks, and free under W9, but it needs the catalog readable during undo, which is unchecked | **as proposed: (a)**, W13 |
| BD-Q9 | **A named key meeting an undecided insert of the same key** (BD-R5).<br>(a) Wait for its decide, and answer by the outcome.<br>(b) `AlreadyExists` at once | user-visible | (a). Under W12 a rollback frees the key | **as proposed: (a)**, W13 |
| BD-Q10 | **How a split replays whole** (BD-R12).<br>(a) One `BTREE_SPLIT` record carrying every image the split writes.<br>(b) A group of records that redo applies only once the group's closing record is durable | format, **[quiet-wrong] if neither** | (a). One record is atomic by its CRC, and redo gains no buffering | **as proposed: (a)**, W15 |
| BD-Q11 | **A rightmost leaf's split at `cores > 1`** (BD-R2).<br>(a) Split a full rightmost leaf at the insertion point rather than the median, so ids arriving out of issue order leave the left leaf full.<br>(b) The median, as today | performance shape | (a). With BB-R1 deleted, the median divide on the rightmost leaf leaves half-full leaves that no later id refills | **as proposed: (a)**, W15 |
| BD-Q12 | **Which order carries BB-S5's rebase of BA.**<br>(a) BB closes first, rebasing BA against BB's rules.<br>(b) BB closes on the measurement it has, and BD-S6 rebases BA once, against BD | process | (b). BD withdraws the BB rules that rebase reads, so rebasing against BB would be done twice | **as proposed: (b)**, W15 |

## 5. Sequencing

1. **BD-S2 before BD-S3.** Opening the gate before the holders in §1.2 are
   shift-safe, and before a split replays whole, makes §1.2's silent
   recovery case, E5's misaligned redo and §1.8's half split reachable from
   SQL.
2. **BD-S1, then S2, S3, S4, S5 and S6.** S4 may start once S3 has landed.
3. **BB and BA (BD-Q12 (b)).** BB closes on the measurement it has, without
   rebasing BA. BA stays paused until then, and BD-S6 rebases these rows
   once, against BD:
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

### BD-Q10, BD-Q11 and BD-Q12 marked as proposed - 2026-10-07

On W15 (`raft-marks-2026-10-07.md` §14):

- **BD-Q10 (a):** a split is logged as one `BTREE_SPLIT` record carrying
  every image it writes (BD-R12).
- **BD-Q11 (a):** a full rightmost leaf splits at the insertion point
  (BD-R2).
- **BD-Q12 (b):** BB closes on the measurement it has, and BD-S6 rebases BA
  once (§5).

BD-Q0, BD's opening, is the one item left. No stage has started.

### BD opened - 2026-10-07

On W16 (`raft-marks-2026-10-07.md` §15), BD-Q0 is marked yes, and BD is
open with BD-S0..S6 and BD-R1..R12 as written at `50d35916`. BD-S1 starts
on `worktree-keep-btree-leaf-slots`, branched from `50d35916`.

### BD-S1 - red first, built 2026-10-07

Built on `worktree-keep-btree-leaf-slots` on `ec3acda5` (BD opened). Run on
that engine, Debug. Every cell below is red for the reason it names, and
the full suite is red on exactly these cells (and on
`TcpServerListenTest.ReusePortAdmitsASecondListenerAndItsAbsenceRefusesOne`,
whose port an outside `kds_server` process held; it fails alone at
`ec3acda5` too).

- **Through SQL**, `tests/sorted_leaf_named_keys_test.cpp`:
  - `AKeyBelowAnEarlierOneIsPlacedAndOrderByStaysElided` (W8);
  - `ADescendingMultiRowValuesIsPlacedInKeyOrder`;
  - `ARolledBackKeyIsNamedAgainAndPlaced` (W12);
  - `EveryRefusalANamedKeyGetsForItsPkIsDuplicateOrExhausted` - the census:
    each case's exact code and the token's byte, §1.4's S3 row, and a key
    named twice in one statement. It shows `2^64 + 5` placed as id 5 today,
    which §1.4 had only inferred.
- **Two sessions**, `tests/lock_family_test.cpp` (`NamedKeyWaitTest`): a
  named key waiting on an undecided inserter, both arms; a committed key
  under an undecided update or delete, `AlreadyExists` at once.
- **The storage contract**, `tests/btree_test.cpp`:
  `ALeafTakesEachIdAtTheSlotItSortsTo`,
  `ADescendingRunAcrossManyDividesLeavesEveryLeafInKeyOrder`, and
  `AFullLeafDividesToMakeRoomForALowerId` flipped to compare unsorted (one
  of §1.6's pinning cells).
- **Recovery undo**, `tests/recovery_undo_test.cpp`
  (`RecoveryUndoLeafTest`): §1.2's silent case, and a loser's row a divide
  moved to the right sibling.
- **The crash rig**, `tests/sorted_leaf_crash_test.cpp` (helpers shared with
  `insert_log_crash_rig_test.cpp` in `tests/file_rig_crash.hpp`):
  - E5's misaligned redo, through a new seam
    (`CommandDispatcher::SetAfterPlacementForTest`, which fails the
    index-maintenance exit). **Red with ascending keys at `ec3acda5`**: the
    mount is refused, so §1.2's "harmless today" is corrected above. No bug
    entry, because BD-S2 fixes it in this session.
  - E2's mid-leaf replay, and a log cut everywhere inside a divide - **both
    reach their key through SQL, so they turn green at S3**, not S2.
- **Decided**:
  - the append split's cut **reproduces** - after record 5 of 8, the parent's
    image, rows inserted after the restart are off the walk - and is
    `docs/inflight/bugs/a-log-cut-inside-an-append-split-leaves-its-leaf-off-the-walk.md`,
    which BD-R12 fixes;
  - the index split's cut (the root leaf's split) **does not reproduce**, and
    stays as a guard.
- **Guard, green**: `ADeletedKeyNamedAgainIsAlreadyExists`.

**The review** (one `critics-developer` pass, 8 correctness findings, 3
missing cells, 6 simplifications). **Applied**: the seam routed through the
index-maintenance exit; the E2 and divide-cut cells' stage corrected; the
cut mounts the whole log too, pairs the before snapshot's data file with the
cut log, and names the record kind it cut after; the bug entry's record
list; the census's exact codes; the smaller items; the S3 row, a key named
twice, and the undecided-delete arm; the shared crash helpers; the divide
cell flipped rather than copied; the out-of-range cell merged into the
census; one helper for the two inserter arms; sampled descents on each cut.
**Declined**: E5's bug entry (BD-S2 fixes it in this session, and a defect
fixed in the session that found it gets none); dropping `Walk()` for
`Ordered()` alone (the bare walk is the leaf's own order, which an elided
`ORDER BY` reports only while the elision holds); one parser for the SQL and
crash files' replies (different reply shapes, `id,qty` against `id`).

**Overhead not measured** (BD-R10, waived).


### BD-S2 - sorted placement, the shift-safe holders, the whole split, built 2026-10-07

Built on `worktree-keep-btree-leaf-slots` on `4debe8d9` (BD-S1). Run on that
engine, Debug.

- **Placement** (BD-R2): `heap::PageView::InsertTupleAt` moves the
  directory entries at and after the slot up one; `btree::SearchLeaf`, one
  binary search over the keyed slots, replaces `RefuseDuplicate`,
  `MaxLiveId` and `FindSlotForId` with its fallback, and is public for redo
  and recovery undo. The divide merges the incoming row into the sorted
  versions and cuts once: the rightmost leaf at the insertion point
  (BD-Q11 (a)), falling back to the median for an id below every key or a
  moving half no empty leaf holds; any other leaf at the median. The append
  split keeps its shape. `BtreeTest.ALookupFindsAnIdInALeafWhoseSlotsAreOutOfOrder`
  is deleted with the fallback.
- **The log** (BD-R3 E2, BD-R12): `BTREE_INSERT` (29) carries HEAP_INSERT's
  payload, and its redo places the row only at the slot `SearchLeaf`
  answers, refusing anything else `Corruption`. `BTREE_SPLIT` (30) carries
  every page a split writes as an image; analysis dirties each, redo applies
  each through its own `page_lsn` gate. A btree insert logs one of the two,
  after its spills and index entries, and a HEAP_INSERT naming a B+ tree
  leaf is refused at redo. The record fits the default ring for the deepest
  split (`static_assert`).
- **E1**: recovery undo re-finds a `kBtreeLeaf` row by its key, in the
  recorded leaf and rightward while `min_key <= pk`, and compensates only a
  version the loser wrote.
- **E3**: `ProbeBuild` verifies a btree entry and resolves a miss by pk.
- **E4**: JB6's mark is the last pk covered on a btree.
- **E5**: a btree placement is taken back at the index-maintenance,
  reservation, undo-append and spill-noting exits; a split's row is retired
  and the split logged under `kNoTxnId`, with a root it grew published.
- **BD-R8**: superblock 20; BB-R11's mount check, its cell and
  `kRetiredKeyOrderUnordered` deleted, the byte reserved.
- **Re-pinned**: the golden log (`0xd4d0579a`: the two `golden_tree` rows'
  record type), `insert_wal_test`'s record kinds, the appended-type cell,
  `BtreeTest.ASplitReportsTheNewLeafAndTheRelinkedOldOneToRedo` (the new
  leaf is an image now), `keystone_id_test`'s logged-id reader.
- **New cells**: three redo cells (`BTREE_INSERT` shifts; one out of key
  order is `Corruption`; `BTREE_SPLIT` replays and dirties every page) and
  `SortedLeafCrashTest.ATakenBackRowWhoseSplitGrewTheRootLeavesTheRootPublished`,
  red with the take-back's root publish removed.
- **Green here from BD-S1**: the storage contract's three cells, recovery
  undo's two, E5's crash cell, and the append split's log cut - so
  `docs/inflight/bugs/a-log-cut-inside-an-append-split-leaves-its-leaf-off-the-walk.md`
  is deleted. **Still red, as §3 says**: the SQL and two-session cells, and
  the E2 and divide-cut crash cells, all gated by SQL's below-mark refusal
  until BD-S3.

**The review** (one `critics-developer` pass): 2 high, 2 low, 8
simplifications, 9 stale comments. **Applied**: C1, a take-back of a split
that grew the root now publishes it (the cell above); C2, the spills are
noted before the row's trail entry and their failure is a take-back; C4's
first half, a HEAP_INSERT on a leaf refused; redo's dispatch as one applier
table over `FunctionRef`; `LogBtreeSplit` takes the placement's changes,
with no dedupe; `SearchLeaf` shared by redo and undo; `take_back` moved
above the Cabin comment; the `fits_one_leaf` comment; every stale comment
named. **Declined**: C3, raising `kMinRingCapacity` past the deepest split
(a 64 KiB ring or segment refuses a split of 8 or more pages, which takes
three full internal levels) - it moves the premise of the cells that open
at the minimum ring and the sim's segment rolls, and is recorded in
`known-gaps.md` at BD-S5 instead; C4's second half, which noted a stricter
behaviour and asked for nothing; one helper for E3 and the Cabin serve (the
serve's miss heals the entry and counts it, a helper would carry both
concerns); `LogInsert`'s span ternary as an `if` (one expression against a
re-indented loop); `Record`'s `min_key` on a btree change (a cell reads it
as the new leaf's low key).

**Overhead not measured** (BD-R10, waived).

### BD-S3 - the gate opened, and BB-R1 deleted, built 2026-10-07

Built on `worktree-keep-btree-leaf-slots` on `62470f56` (BD-S2). Run on that
engine, Debug.

- **The two reasons** (BD-R5): a named key outside `[1, 2^40 - 1]` is
  `OutOfRange` with its token's byte, judged from the literal's digits
  (`raw_int_text`) rather than the wrapped `int_val` - zero, negative, past
  40 bits, or past 64 (`2^64 + 5` no longer lands as 5). The order said to
  reuse `row_codec.cpp`'s wrapped-literal test; that test compares the
  spelling against `std::to_string(int_val)` and would read a leading zero
  (`007`) as wrapped, so the digits are read directly instead. A duplicate is
  `AlreadyExists` with the byte, its text saying whether the row was
  deleted (`btree::DuplicateKey`). The lexer's overflow is unsigned, so
  defined.
- **The judgement of a refused borrow** (BD-R5, BD-Q9 (a)): on a btree, a
  present key whose latest writer decided is a duplicate at once; an
  in-flight writer's version is walked back while each earlier writer is in
  flight (`CommandDispatcher::UndecidedInsert`) - a chain that ends is an
  undecided insert, and the statement waits for its decide; one that reaches
  a decided writer is a committed row under an undecided update or delete,
  a duplicate at once. Absent is a wait. The mark branch is the heap's alone.
- **BB-R1 deleted on a btree** (BD-R6): an omitted pk is issued, borrowed
  and encoded before the descent; a named key is borrowed, encoded and
  admitted advance-or-nothing (`Catalog::AdmitExplicitRowId`, which reads
  the relation's storage), then placed. `btree::BtreeInsert` is the one door:
  `BtreeInsertIssued`, `BtreeInsertNamed` and `duplicate_scanned` are
  deleted, and the dispatcher's `InsertIssued`/`InsertNamed` are the heap's.
  **The heap's protocol is untouched** (BD-Q4 (a)).
- **§1.6's pinning cells flipped**: the fence cell (a duplicate refused at
  once, an absent key waits on the fence and is placed), the spilled-value
  cell (the duplicate alone refuses after a spill), the Cabin cell (a key
  below the mark witnessed, a duplicate not), the bulk ordinal cell (a heap
  refuses, a btree places), the catalog cell (advance-or-nothing); the
  undecided-writer `OutOfRange` cell retired for `NamedKeyWaitTest`.
- **BB-S3's reshaped cells restored** from `1b5d252e^`: in
  `supplied_key_test.cpp` the descending load, the range scan and ORDER BY
  (with and without LIMIT) over descending keys, the interleaved backfill,
  the rollbacks across a divide (a failed statement, an aborted update), the
  bulk statement in any order, a duplicate of a descending key and a
  descending key accepted; in `insert_log_crash_rig_test.cpp` a divide under
  another core before the row is logged and the mid-chain append split
  across a crash (beside their ascending variants); in `lock_family_test.cpp`
  the resume across a divide under a park; in `insert_wal_test.cpp` the
  divide's record set, now one `BTREE_SPLIT` and no insert. Not restored, and
  why: the catalog's key-order flip cells and `DESCRIBE`'s `key_order=` cells
  pin a flag BB-S3b deleted, and the Cabin serve's `(page, slot)` branch for
  an unordered relation is gone with it.
- **Re-stated**: `IdAllocationAcrossCores` - one burned id per retry since
  the issue precedes the descent; the bound it asserts is unchanged.
  `issue_under_the_leaf_rig_test.cpp`'s cells pass by placement.
- **Green**: every BD-S1 cell; the suite, but for
  `TcpServerListenTest.ReusePortAdmitsASecondListenerAndItsAbsenceRefusesOne`
  (an outside process holds its port).

**Overhead not measured** (BD-R10, waived).

**The review** (one `critics-developer` pass): 1 medium and 3 low
findings, 2 missing items, 4 simplifications, stale texts. **Applied**:
the medium one - an omitted pk could draw the id a named key at the mark had
borrowed and then placed, and was refused `AlreadyExists`; it now burns the
id and draws again (bounded), pinned by
`IssueUnderTheLeafRig.AnIssuedIdANamedKeyPlacedFirstIsDrawnAgain` through a
new seam between the issue and its borrow (`SetAfterRowIdIssuedForTest`),
red with the reissue removed; the rig cells assert whether core 1 finished
during core 0's stop (a btree's does, a heap's does not); the burn comment
says "at most one"; a btree's `OutOfRange` placement is logged again; the
named key's seam moved before its admission, so
`ANamedKeyAndALaterIssuedIdLandInKeyOrder` replaces the BB cell that pinned
issue order; the four reshaped two-core cells restored beside their
variants (`AChildCommittedDuringTheControllersBuildIsInTheSetItBanks`,
`AnIndexRecordCarriesItsOwnRowsEntryAcrossAnotherCoresInserts`, and under
new names `AParentWrittenBackBeforeItsDivideIsLoggedDoesNotRouteToNothing`
and `TwoCoresInsertingSpillsIntoOneVarHeapPageRecoverInTheOrderTheyWrote`),
each repeated five times green; `UndecidedInsert`'s second step pinned
(`NamedKeyWaitTest.AnUndecidedInsertItsOwnWriterUpdatedIsStillWaitedOn`);
`BelowMark` deleted - the admission reads the relation's `clustered_type`;
the heap-only `InsertIssued`/`InsertNamed` inlined; `UndecidedInsert` a free
function; one helper for the duplicate's byte;
`CheckNamedRowIdSpellable` answers `OutOfRange`; the catalog's and the
dispatcher's stale texts. **Declined**: none.

### BD-S4 - the sim and the rigs, built 2026-10-07

Built on `worktree-keep-btree-leaf-slots` on `07e822a6` (BD-S3). Run on that
engine, Debug.

- **The sim** (BD-R9): the workload names keys - `kInsertNamed`, in and out
  of a transaction, drawing a fresh key, one near the top, a deleted key, a
  rolled-back key and a key outside the space - and tracks the band a fresh
  key moves the mark into, so pk-targeted reads and writes keep reaching the
  rows inserted after it. The oracle answers every named insert placed, a
  duplicate, exhausted or either (`Oracle::Named`): a key consumed or pending
  is a duplicate whatever else is unknown, an unchecked key or an
  indeterminate unnamed insert makes it either. The loop checks each reply
  against that answer and absorbs only an I/O error under faults, never a
  pk refusal; the toggle pairing compares a named insert's outcome by its
  refusal's reason. `integrity.cpp`'s slot-order check covers delete-marked
  slots.
- **The rigs** (`sorted_leaf_rig_test.cpp`, new): two cores naming
  interleaved descending keys into one leaf; a shift under another core's
  undecided row surviving its rollback; appends from two cores keeping the
  tree whole and dense (a smoke cell); an inner build and a resumed prefix
  answering the same while core 1 shifts the first leaf's rows under every
  resume, its EXISTS premise asserted from `ANALYZE`.
- **The crash cells** (`sorted_leaf_crash_test.cpp`): a loser whose rows
  shifted each other, and a loser whose rows a later divide moved - premise
  asserted (`leaves=` grew by two under the loser, its keys descending so a
  divide moves rows it placed whichever point it cuts at) - rolled back
  across a crash; a mid-leaf row placed and never logged taken back before
  the next record; a log cut inside an internal node's divide.
- **JB6's deterministic cell**
  (`ExecChainTest.AResumedPrefixCoversEveryInnerRowAcrossADivideThatDropsARetiredSlot`):
  through `exec::Execute`'s `PositionSink`, at the outer walk's first page
  boundary, `tr`'s leaf is divided ahead of the mark, compacting a retired
  slot behind it; each inner row must be bucketed once and every author
  answered.
- **The mutations**, each applied from a backup and built:

  | Mutation | Killed by | Verdict |
  |---|---|---|
  | placement at `nr_slots` | `BtreeTest.ALeafTakesEachIdAtTheSlotItSortsTo`, `SortedLeafSqlTest.*` | killed |
  | the divide appending its row last | `BtreeTest.ADescendingRun*` | killed |
  | recovery undo by slot alone | `RecoveryUndoLeafTest.*`, `SortedLeafCrashTest.ALoserRowADivide*` | killed |
  | `BTREE_INSERT` redo without its neighbour check | `RedoTest.ABtreeInsert*` | killed |
  | E5's take-back removed | `SortedLeafCrashTest.ARowPlaced*`, `.AMidLeafRow*`, `.ATakenBack*` | killed |
  | `before_wait` refusing an undecided insert | `NamedKeyWaitTest.*` (3x) | killed |
  | the exhausted check reading the wrapped value | `SortedLeafSqlTest.EveryRefusal*` | killed |
  | `ProbeBuild` without `VerifyTupleAt` | `SortedLeafRigTest.AnInnerBuild*` (3x) | killed |
  | a split replayed in part | `RedoTest.ABtreeSplit*`, `SortedLeafCrashTest.ALogCutInsideAnAppendSplit*` | killed |
  | JB6 by ordinal | `ExecChainTest.AResumedPrefixCovers…` (3x): `build_rows` 165 for 166, an author missing | killed |
  | the rightmost leaf split at the median | `BtreeTest.AFullRightmostLeafSplitsAtTheInsertionPoint` | killed |

  The last two survived the rig cells: two cores' interleave cannot force
  an ordinal resume across a dropped slot, nor tell a median divide from an
  insertion-point one by leaf count. JB6's kill is the deterministic cell
  above; the median's is the `BtreeTest` cell, and the rig cell is named a
  smoke cell. The re-timed rig cell, the reworked JB6 cell and the loser
  cell were re-run against their mutants after the review and still kill
  them.
- **Green**: the suite, 3223 of 3223 (one disabled); `scripts/sim.sh 4`,
  190 runs, 0 failures (Debug `ckdbs-sim`; no `build-release` in this tree).

**Overhead not measured** (BD-R10, waived).

**The reviews** (two `critics-developer` passes). The first: 1 high, 2
medium and 1 low on the sim, 4 on the cells, a deterministic JB6 kill, and
duplication. **Applied**: the high one - the first fresh key moved the mark
to 2^39 and every pk-targeted op stayed in the low band; the workload now
tracks the high band; the fault absorb excludes pk refusals; the oracle's
order puts consumed/pending first; the pairing compares outcomes, not
`IsErr`; the smoke cell reworded and renamed; the E3/E4 rig re-timed and its
EXISTS premise asserted; the loser cell's premise; the JB6 cell; the rig's
helpers moved onto `crash_rig::` (`Ids`, `Leaves`, `StartsWith`, `Ok`), and
`insert_log_crash_rig_test.cpp`'s copy of `Ids` with them. One applied
finding was wrong and corrected: the pairing compared a named refusal's
full text, and a duplicate's text names a page, which the features' own
pages move, so the pairing failed on every committed seed; it compares the
refusal's reason instead. The second pass, over those follow-ups: the JB6
cell's exact count held by page geometry with the divide behind the mark,
so the divide is anchored at the mark's author and the count is the authors
plus the rows the divide inserted; a dead `build_rows=0` assert; the loser
cell's keys made descending; includes. **Declined**: none.

### BD-S5 - the text, written 2026-10-07

Written on `worktree-keep-btree-leaf-slots` on `b6eab627` (BD-S4).

- **Every text in §1.6 restated** (BD-R11): `CLAUDE.md`'s rows and
  invariant 11, `README.md`, `heap-and-tuple.md` §3.1, §4.1, §4.1a, §5 and
  §8, `keystoneid-invariant.md` K1-K3 and §1, `txn.md`, `wal.md` §5.2,
  §11a and §12, `index.md`, `cabin.md` C2 and §5, `join-inner-build.md` JB4
  and JB6, `crosscore.md`, `waystone-concpets.md`, `assertion.md`,
  `bulkinsert.md`, `page.md`, the manual's pk section and error table, and
  the source comments in `src/` and `include/` that cited a withdrawn BB
  rule or BD's superseded reasons.
- **Corrected on the way**: `txn.md`'s delete-mark rollback
  (`HEAP_DELETE_UNMARK`, no Waystone entry cleared); `bulkinsert.md`'s
  per-row pk checks, the heap's below-mark refusal named.
- **Recorded**: BD-R4's purge obligation and §1.8's index-split question in
  `known-gaps.md`; §1.7's second path in the equal-entries bug entry;
  `known-gaps.md:493-510` closed by superblock 20 (BD-S2).
- **The grep** (BD-R11's done-when): every hit of `BB-R1`, `BB-R2`,
  `BB-R3`, `BB-R11` and `BB-R12` in `src/ include/ docs/spec/ docs/rules/
  manual/ CLAUDE.md` states history or the heap's kept rule (BD-Q4).
- No code changes: every `src/` and `include/` hunk is a comment.
- **Green**: the suite, 3223 of 3223 (one disabled).

**The reviews.** The first pass (a workflow of four reviewers, each
finding voted on by two verifiers): 27 confirmed, 3 refuted. **Applied**:
all 27 - the take-back of a split, an issued id's uniqueness on a btree
(the mark among issued ids, the placement's duplicate check against a named
key, `kMaxIssueRounds`), a btree leaf sorted in §3.1, the per-core block
paragraph, index.md's stale cross-reference to the clustered tree's images,
the Cabin's and the readers' "dead forever" reasons under W12, the heap's
rolled-back key in `txn.md`, `wal.md`'s INSERT row and ordering rule 1,
`assertion.md`'s watermark, `bulkinsert.md`'s heap check, `CLAUDE.md`,
`README.md` and the manual's unscoped or stale claims (with a new error row
for an omitted pk's eighth collision), the heap comments citing BB-R12, the
index tree's header, `LogInsert`'s and the seam's contracts, and
`PromoteSeparator`'s reach. **Refuted** (the verifiers did not hold them):
`heap-and-tuple.md`'s "below a full leaf", the write-walks bug entry's line
citations, and `log_page_image.hpp`'s size claim, which the known-gaps WAL
entry already bounds. A second pass over the applied text found one
applied fix false - the Bound Cabin's pk row said a rolled-back key can
reach a live entry; an aborted reservation's entry is removed and orphaned
before the undo frees the key (AS6b) - and eight sentences beside the fixes
still contradicting them ("never mis-attribute", "an issued id clears every
named key"), plus wording: all applied. **Declined**: none.

### BD-S6 - the close, 2026-10-07

Written on `worktree-keep-btree-leaf-slots` on `91c998a3` (BD-S5).

- **A row per stage**: BD-S1 (red cells, `4debe8d9`), BD-S2 (placement, the
  shift-safe holders, the whole split, superblock 20, `62470f56`), BD-S3
  (the gate opened, BB-R1 deleted on a btree, `07e822a6`), BD-S4 (the sim,
  the rigs, eleven mutations killed, `b6eab627`), BD-S5 (the text,
  `91c998a3`).
- **What BD carries**: a btree leaf is in key order by placement at every
  core count; a named key on a btree is refused only as a duplicate or
  exhausted, and a rolled-back key is free (W12); an issued id is drawn
  before the descent and re-drawn, bounded, when a named key took it first;
  a split is one `BTREE_SPLIT`. **The heap is untouched** (BD-Q4 (a)).
- **What bounds it**, in `known-gaps.md` and `bugs/`: a `BTREE_SPLIT` of 8
  or more pages exceeds a 64 KiB ring or segment; whether an index split's
  records replay as one is unchecked; BD-R4's purge obligation (a keyed
  tombstone must outlive any purge); the equal-sort-keys index bug, which a
  rolled-back key named again now also reaches; an omitted pk refused
  `AlreadyExists` after `kMaxIssueRounds` collisions with named keys; a
  heap placement whose record is never written is not taken back.
- **BA's rows rebased** (BD-Q12 (b)) in `workorder-ba-parallelism.md`, "BA
  rebased on BD": BA-R8's hook bullet stands, its named-key bullet's draft
  withdrawn, its placement-order bullet resting on BD-R1; BA-R11's original
  text stands and BB's draft is dropped; BA-S11 rests on BD-R6, its
  `kUnordered` clause gone. BA stays paused until BB's close entry is
  written, which waits for its own word.
- **No measurement** (BD-R10).

**Not done here, and why**: the push and BB's close entry each wait for the
operator's word.

**The review** (one `critics-developer` pass): 4 findings, all applied -
the known-gaps entry said the issue holds no page (it holds page 7; no
leaf); BA-S11's rebase said a `fetch_add` is the whole of an issue and kept
*"a named key inside [cursor, ceiling) is never issued"*, both of which
BD's bounded re-draw contradicts; §16's "Recorded at"; and BB's order now
says its BB-S5 does not rebase BA. **Declined**: none.
