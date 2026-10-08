# Work order BH — PURGE: an explicit statement frees a deleted key, and K1 gains one named exception

Written 2026-10-08 on `worktree-bh-purge-key` from `0446b3b0`
(`v2.7.0-691-g0446b3b0`), on the operator's words, verbatim:

- **W1:** *"아냐 Invariant에 pk 값으로 오류가 나는 경우는 중복되거나 2^40-1이
  모두 소진되었을때 뿐이니까 이미 삭제된 pk키값으로 삽입하는건 허용되어야해"* -
  no: under the invariant a pk is refused only as a duplicate or once
  2^40 - 1 is exhausted, so an insert naming an already-deleted pk must be
  admitted.
- **W2:** *"그러면 규칙을 폐기하지 말고 pk 정보 자체를 purge하는 다른 api를
  작성하는것은 어떄? (delete와 구분 되는 구문이고 이 경우 재삽입이 가능함)"* -
  then, instead of discarding the rule, how about another API that purges the
  pk information itself (a statement distinct from DELETE, after which
  re-insertion is possible)?
- **W3:** *"제안 대로 작업 명세와 지시서를 작성해줘"* - write the
  specification and the work order as proposed.

**What the words settle, and what they leave open.**

- **W1.** CLA answered W1 by naming the conflict it raised. K1
  (`docs/rules/keystoneid-invariant.md:18-29`), invariant 11 and BD-R4 make
  a deleted key a duplicate for the life of its relation. No file was
  written on W1.
- **W2 replaced W1's direction.** K1 and `DELETE` stay as they are, and a
  separate statement frees a key. BH-Q1 is W2's direction.
- **W3.** CLA's reply to W2 proposed seven points, recorded in
  `raft-marks-2026-10-08.md` §3, and W3 directs that the order be written
  on them. They map to §5's items as follows:

  | point in the reply | item |
  |---|---|
  | 1. committed delete-marked rows only; a live row named is `InvalidArgument` | BH-Q2 |
  | 2. older snapshots waited on, then `TxnConflict` at the 1 s fault net | BH-Q3 |
  | 3. autocommit only, one record per key, no rollback, a superblock bump | BH-Q4, BH-Q5, BH-Q6 |
  | 4. `PURGE FROM t WHERE` pk equality or range | BH-Q7 |
  | 5. index entries left | BH-Q8 |
  | 6. a heap relation `Unsupported` | BH-Q9 |
  | 7. an engine-internal purge still owes a keyed tombstone | BH-Q1 |

  **Four proposals are revised** by what the survey and the review found,
  and each revision is stated at its item:
  - **BH-Q2:** a range passes over a live key.
  - **BH-Q3:** the 1 s bound is BH's own constant, not the fault net.
  - **BH-Q5:** a refusal, not only a crash, can leave a window partly
    purged.
  - **BH-Q6:** no superblock bump, because no record kind is added.

  **BH-Q10..Q16 are new.** No word has seen them.
- **BH is opened on 2026-10-08, and every item carries CLA's proposal.**
  The operator gave a standing go-ahead (`go-ahead-achieving-milestone`):
  *"go ahead until achiving milestone, I will follow CLA proposal if
  decision needed, keep going in this session"*, then *"GO AHEAD"*.
  - BH-Q0..Q16 are **adopted under that word**, each as §5's proposal
    states it. No item carries a per-item mark of the operator's own.
  - Before adopting the two `[quiet-wrong]` items, CLA re-read their
    premises against the tree at `37a7a9cd`:
    - **BH-Q1 (a)** is W2's own direction.
    - **BH-Q16 (a)** rests on the writers' skip and the insert's duplicate
      check. `UPDATE`'s `apply` (`command_dispatcher.cpp:7600`) and
      `DELETE`'s `qualifies` (`:9217`) skip a dead slot and any slot that
      `Classify` answers `kNoVersion`. The insert's duplicate check runs
      under the leaf's write hold (`btree.cpp:896-904`), where a retired
      slot is keyless.
    - Neither premise had moved, so neither proposal was revised.
  - The adoptions are recorded in `raft-marks-2026-10-08.md` §5.

**Where the specification is.** The specification W3 asks for is §2. It is
written here and not under `docs/spec/`, because that bucket holds only
what is built (`CLAUDE.md`, "How `docs/` is organized"). BH-S5 lands it in
`docs/spec/heap-and-tuple.md` (a new §4.1c), in K1 and in the manual.

- BH-S0 moved no file under `src/`, `include/` or `tests/`, and no suite
  ran.

**BH is not part of AR0 §8's chain.** It gates no open order. Its
neighbours:

- **BD** (closed at BD-S6, pushed at `bc144dbf`) is BH's base. BH amends
  BD-R4 (the delete-marked row is the tombstone) and BD-R5's duplicate
  reason (*"live or delete-marked"* becomes *"live, or delete-marked and not
  purged"*). BD's order is closed and is not edited. BH-S5 restates the
  specs that carry its rules.
- **BF** (closed at BF-S6, merged into this branch at `274b92ef`) says
  *"BD-R4's purge obligation is about rows of a live relation, and BF does
  not touch it"* (`workorder-bf-drop-table-page-reclaim.md:28-31`).
  - BH frees no page.
  - BF-S3 moved the superblock to 21. Under BH-Q6 (a) BH moves it no
    further; under (b), BH-Q15 decides the number.
  - **A `PURGE` writes a relation's pages, and BF-S5's within-run reclaim
    is built.** Its statement epoch (BF-Q9 (b)) must count a `PURGE` like
    any other statement, and BH-S3 checks that it does.
- **BG** (opened) shares nothing with BH.
- **The equal-index-sort-keys defect**
  (`docs/inflight/bugs/a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`)
  gains a path, and BH does not fix it (BH-R9).

## 0. What BH is

**Today a deleted key is bound for the life of its relation.** `DELETE`
marks the row in place (§1.1), and nothing retires a delete-marked row of a
user relation (§1.2). The duplicate check reads that row: an `INSERT`
naming the key is refused `AlreadyExists`, *"a row with this key was
deleted; a Keystone id is bound once"* (`src/storage/btree/btree.cpp:839-845`).
This is K1 working as written, and it is what W1 objects to.

**The price of freeing a key is the hazard K1 was adopted to delete.** K1's
first reason (`keystoneid-invariant.md:64-74`) is *"snapshot-safe pk
resolution"*. If a key is freed and re-placed while an old snapshot can
still see the row it deleted, that reader misses a row its snapshot is
entitled to, and no error is raised. K1 removed the hazard structurally
*"instead of gating it behind a purge-horizon rule that every future
feature would have to re-prove"*. **BH builds exactly that gate**, for one
statement (BH-R5). Two consequences follow:

- Every view that can read a superseded version of a user row must be
  visible to the horizon, now and in every later feature. Census A
  (BH-S1) proves this for the views that exist.
- K1's third reason, the audit posture (*"a row's identifier never changes
  and is never reissued"*, `:88-89`), becomes *"never reissued except by an
  explicit, logged `PURGE`"*. An external system that captured a Keystone
  id must treat a purged key as a new identity.

**BH's answer, in one sentence.** `PURGE FROM t WHERE <pk predicate>`
retires, keyless, the slot of every committed delete-marked row the
predicate names, once no reader can see the row it deleted. After that the
key is free, and an `INSERT` that names it is placed.

**BH's rules, one line each:**

- **BH-R1.** K1 gains one named exception, `PURGE`. Every other path,
  including any purge the engine runs on its own, still owes a keyed
  tombstone.
- **BH-R2.** The statement: `PURGE FROM t WHERE` pk comparisons only, a
  statement head matched by text and never reserved. It runs in autocommit
  only, on a btree only, and needs the admin role.
- **BH-R3.** A key is purged when its slot is delete-marked by a committed
  deleter that every live and future reader sees as committed.
- **BH-R4.** It takes the relation's `IS` and `IX` and no row or range
  unit. A race with a writer of the same key is settled under the leaf's
  exclusive latch, and the purge skips any slot that is no longer the
  tombstone it judged.
- **BH-R5.** The horizon gate, `[quiet-wrong]`:
  - the purge's own view is re-minted before every judging pass;
  - older readers are waited on, polled and bounded by
    `kPurgeHorizonWait`, then the statement is refused `TxnConflict`;
  - Census A shows that every view able to read a superseded user row is
    registered.
- **BH-R6.** It judges the whole window before it writes. Each key is then
  purged whole or not at all. A refusal or a crash can leave the window
  partly purged, and a refusal says how many keys it purged.
- **BH-R7.** It logs `SLOT_RETIRE` and then `VARHEAP_RELEASE`, both at
  `kNoTxnId`, through the existing helpers. Redo already applies both, so
  no record kind is added and the superblock stays where BF left it. Its
  `TXN_COMMIT` carries the reply's durability.
- **BH-R8.** It does not run `DELETE`'s per-row code: no assertion
  departure, no foreign-key check, no Cabin or index hook, no undo record.
- **BH-R9.** Secondary-index entries, Cabin entries, assertion
  directories, foreign keys and Waystone trails are left as they are, each
  for a reason the survey checked. The equal-sort-keys defect gains a
  path.
- **BH-R10.** Every refusal, with its code and its byte.
- **BH-R11.** Checked, not only argued: SQL cells, two-core rigs, crash
  cells, the sim's `kPurge` op, and every mutation run.
- **BH-R12.** The text: every sentence that says a committed key is never
  rebound is restated.

**What BH leaves standing:**

- **`DELETE`**, byte for byte: the delete-marked row is still the
  tombstone until a `PURGE` frees it.
- **K1 for every path but `PURGE`**: the allocator, delete-then-insert
  without a purge, crash recovery, and any engine-internal purge, which
  still owes a keyed tombstone (`known-gaps.md:657-669`, restated rather
  than closed).
- **K2-K5.** A purged key is never issued: `sys.tables.next_id` stays a
  high-water mark, and K4's budget does not get the key back. Only an
  `INSERT` that names the key reuses it.
- **Invariants 3 and 4.** A retired slot carries no key, so a leaf's keyed
  slots still ascend.
- **Invariants 8, 9, 13 and 14.** No live tuple and no live var-heap value
  moves. The purged version's spills are released only after no reader can
  reach them.
- **Invariant 12.** No header field is added.

**BH does not:**

- Purge in the background, or on a cadence.
- Purge a live row. There is no `DELETE` and `PURGE` in one statement.
- Purge inside an explicit transaction (BH-Q4).
- Purge a heap or a system relation (BH-Q9).
- Remove a secondary-index entry (BH-Q8), or a Cabin entry: the heal pass
  already drops dangling ones.
- Reclaim leaf space beyond what a dividing split already compacts
  (§1.2). Reclaim the spills of versions older than the tombstone, which
  stay in the undo chain's keeping (§1.4).
- Change the issue cursor, or add a configuration key.
- Add a WAL record kind, under BH-Q6 (a).
- Fix the equal-sort-keys defect.

**What bounds BH** (each goes to `known-gaps.md` at BH-S5):

- **One open old snapshot blocks every `PURGE` of a key deleted after it.**
  An idle REPEATABLE READ session refuses every such `PURGE` at the bound.
- **Nothing wakes the wait when the horizon advances.** It polls, in the
  class `known-gaps.md` already names (§1.3).
- **A refusal or a crash in the middle of a window leaves it partly
  purged.** Each key is whole, and a re-run finishes the window (BH-Q5).
- **A crash between a retire and its release leaks that row's spills.**
- **The leaf space a purge frees comes back only at a dividing split**,
  never at an append split.
- **A wide window collects every target before it writes**, so the target
  list grows with the window.
- **A `PURGE` waiting at the bound delays a DDL on its relation** for as
  long, since it holds the relation's `IX` (BH-R4).
- **The audit claim carries the exception** (§0).

## 1. Survey at `0446b3b0`

This section was read, not run. It draws on three read-only surveys:

- the delete, retire and recovery paths;
- every consumer of K1;
- the front end, the locks and the protocol.

CLA also read the code where a survey's claim carried a ruling: the
duplicate text, the fault net's meaning, drain mode, `RequiredRole`, the
BD-S1 guard and K1's reasons. The review (§7) read more and corrected
four rows.

### 1.1 What `DELETE` leaves behind

- **The mark.** `CommandDispatcher::DeleteInner`
  (`src/server/command_dispatcher.cpp:9038`) writes the mark in its `mark`
  lambda: `OverwriteTuple`, then `DeleteMark` (`:9278-9281`).
  `PageView::DeleteMark` stamps the deleter's `trx_id` and sets
  `kSlotFlagDeleted` (`src/storage/heap/heap_page.cpp:340-343`).
- **The undo record** is `UndoRecordType::kDeleteMark`, with an empty image
  (`:9248-9261`).
- **The WAL record** is `HEAP_DELETE_MARK` (7), with payload
  `{trx_id, slot}` (`include/kds/wal/payload.hpp:324-327`).
- **The assertion departure.** The same lambda reserves an assertion
  departure (`:9228-9243` → `ReserveDelete`,
  `src/exec/assertion_check.cpp:675-693`), so a deleted row's net
  contribution is already 0.
- **Nothing else.** No var-heap release, and no index or Cabin hook
  (`:9293-9302`).
- **The pre-delete payload exists only on the page.** The undo walk's
  `kDeleteMark` arm keeps the page's payload (`src/txn/visibility.cpp:32-37`).
  A reader that cannot see the deleter reads the row from the tombstone's
  own bytes.
- **A writer skips a tombstone whose deleter it can see.** `DeleteInner`
  skips a row that `Classify` answers `kNoVersion` for (`:9116-9118`), and
  UPDATE classifies first in the same way. A deleter the writer's snapshot
  cannot see answers `kNeedsUndoWalk` instead
  (`include/kds/txn/visibility.hpp:68-80`). **So "no writer touches a
  resolved tombstone" rests on BH-R5's gate, not on a lock.** Once every
  reader sees the deleter, every writer's snapshot does too.
- **The tombstone is BD-R4's.** The placement's duplicate check reads it
  (`btree.cpp:898-904`). So does the named insert's pre-check when its
  borrow is refused (`command_dispatcher.cpp:5324-5337`).
- **Observed, outside BH's scope.** Redo of `HEAP_DELETE_MARK` does not
  carry the header's new `undo_ptr`. This is inferred to be harmless after
  a mount. `PURGE` reads the deleter's `trx_id`, which the record does
  carry.

### 1.2 The retire primitive and its record

- **The primitive.** `PageView::RetireSlot` sets `kSlotFlagDead` and length
  0; the bytes stay (`heap_page.cpp:389-404`). After it, `ReadTuple` and
  `PayloadAt` answer `NotFound` (`:238`, `:262`), so the slot carries no key.
- **Its callers.** Live rollback (`src/txn/manager.cpp:494-508`), recovery
  undo (`src/txn/recovery_undo.cpp:213-226`), E5's take-back
  (`command_dispatcher.cpp:5501-5504`), the catalog
  (`src/catalog/catalog.cpp:306`, `:413`, `:971`), and `sys.assertions`'
  drop of a live row (`assertion_catalog.cpp:312`). **None retires a
  delete-marked user row.**
- **`SLOT_RETIRE` (8)** carries `{slot}`. Its envelope is the aborting
  transaction's id when a rollback emits it, and `kNoTxnId` when a purge
  emits it (`payload.hpp:372-386`).
  - **Emitters at `kNoTxnId` today:** the catalog's `LogCatRetire`
    (`catalog.cpp:273-283`) and `exec::LogSlotRetire`
    (`src/exec/wal_row_log.cpp:117-126`, called at
    `assertion_catalog.cpp:313`).
  - **Redo:** `ApplySlotRetire` treats an already-dead slot as a
    re-application (`src/wal/redo.cpp:180-195`).
  - **Analysis:** a non-zero `txn_id` is a loser until its terminal record
    (`src/wal/analysis.cpp:52-60`, `:90`), so a `kNoTxnId` record is never
    undone.
- **`VARHEAP_RELEASE` (28)** has the same split: a purge drain carries
  `kNoTxnId` (`include/kds/wal/record.hpp:218-229`). Its helper is
  `txn::ReleaseVarHeapSlot` (`include/kds/txn/varheap_release.hpp:62`).
- **The catalog's purge is the precedent.**
  - **In the run,** `Catalog::RetireDeleteMarks` retires every mark whose
    deleter `ResolvedForEveryReader` passes (`catalog.cpp:940-991`). It logs
    `SLOT_RETIRE` at `kBootstrapXid`, which `LogCatRetire` maps to
    `kNoTxnId` (`:975-983`).
  - **It runs at DDL resolution on core 0**
    (`command_dispatcher.cpp:6077-6107`), with *"deliberately **no**
    background cadence"* (`docs/spec/ddl-transactional.md:347`).
  - **At mount,** `FinalizeDeleteMarksAtMount` retires every mark with a
    predicate of `true` (`src/server/expeditor.cpp:904-915`;
    `catalog.cpp:993-1020`), since no reader exists yet.
- **Retired slots in a leaf.**
  - A retired slot *"carries no key and sits anywhere"*
    (`include/kds/storage/btree/btree.hpp:137-144`), and `SearchLeaf` steps
    over it (`btree.cpp:807-829`).
  - `InsertTupleAt` never reuses one (`heap_page.cpp:168-202`; *"nothing
    reuses space freed by DELETE"*, `btree.hpp:69-70`). Any insert into a
    leaf shifts the slots above it (`heap_page.cpp:174-178`).
  - A median or insertion-point divide rebuilds the leaf without retired
    slots and carries delete-marked rows still marked (`btree.cpp:665`,
    `:624-627`, `:726-748`).
  - An append split does not compact (inferred from `:906`, `:940`).
- **The format.** `kSuperBlockVersion` is 20
  (`include/kds/server/superblock.hpp:256`), checked by exact match
  (`src/server/superblock.cpp:69-83`). A new record kind is *"a
  format-version event"* (`record.hpp:44-47`). **Since the merge at
  `274b92ef` it is 21**: BF-S3 moved it for the drop tombstone's roots.
  Nothing in BH rests on the number.

### 1.3 The horizon, and the views it does not see

- **The test.** `TransactionManager::ResolvedForEveryReader(trx_id)` is
  true when the writer is below the floor, or committed at or below every
  live and future snapshot (`include/kds/txn/manager.hpp:689-695`;
  `src/txn/manager.cpp:807-833`). Its comment names the catalog purge as
  its caller.
- **What a premature retire does** (inference):
  - `Classify` answers `kNeedsUndoWalk` for an invisible deleter
    (`visibility.hpp:66-81`).
  - A retired slot instead answers `NotFound`, and the lookups map that to
    absent (`command_dispatcher.cpp:6203-6204`; `src/exec/step_vm.cpp:602`).
  - A re-placed key has no undo chain, so an older reader sees an invisible
    writer and no version.
  - Either way the older reader loses the row without an error. **A missing
    row is a wrong answer, so the gate is `[quiet-wrong]`.**
- **A write's own transaction is registered.**
  - `TransactionManager::Begin` mints the transaction's view
    (`manager.cpp:162`) and appends `TXN_BEGIN` (`:164-166`). The
    transaction is *"its own registration - `live_` is what ReadHorizon()
    walks"* (`command_dispatcher.cpp:8254-8256`).
  - **So a `PURGE` that begins, then waits for a `DELETE` to commit, holds
    the horizon below that deleter itself**, and the gate never opens (the
    review's finding 1).
  - A READ COMMITTED statement boundary re-mints an explicit transaction's
    view (`EnsureStatementBoundary`, `:8248-8253`), which is the mechanism
    BH-R5 reuses.
- **The views the horizon exempts** (`docs/spec/txn.md:494-498`):
  - **Latest-state check views** (`MintCheckView`, `manager.hpp:555-567`).
    They never read a superseded version.
  - **Views that never outlive one synchronous span**, *"an exemption to
    re-check whenever the executor gains a suspension point"*. An
    autocommit read's view is leased and dropped before any wait
    (`command_dispatcher.cpp:8261-8267`).
  - **Why the catalog purge is safe and `PURGE` is not, yet.** The catalog
    purge leans on running where *"no unregistered synchronous view is
    live"* (`ddl-transactional.md:340-345`): core 0, between resolutions.
    A `PURGE` runs at statement time on any core, while other cores run
    synchronous spans. This is Census A.
- **There is no wake when the horizon advances.** A wait on it joins
  *"what still polls"* (`docs/inflight/known-gaps.md:683-690`).
- **The fault net is for a lost wake, not a long reader.**
  `lock_wait_fault_net_ms` (1000) is *"how long a statement may wait before
  the engine calls the wait a fault … Not the ordinary end of a wait"*. A
  firing aborts the waiter and is logged as a fault
  (`manual/server/server.md:122`; `include/kds/txn/lock_table.hpp:432-445`;
  `command_dispatcher.cpp:631-642`, `:8667-8673`). An open old snapshot is
  ordinary, which is why BH-Q3 does not bound the wait by this key.

### 1.4 The var-heap

- **`DELETE` releases nothing.** An insert rollback releases each spill
  with `VARHEAP_RELEASE` under the aborting transaction's id
  (`manager.cpp:399-416`; `recovery_undo.cpp:49-73`).
- **`PageRelease` is idempotent** (`varheap_release.hpp:28-30`).
- **The below-horizon release is described and not built**
  (`include/kds/storage/varheap.hpp:44-48`, `:55-58`;
  `varheap_release.hpp:16`).
- **The mount sweep covers `sys.assertions` only**
  (`src/exec/varheap_sweep.cpp:62-95`;
  `include/kds/exec/catalog_spills.hpp:49-58`).
- **So a purged row's spills are orphaned for good unless the purge
  releases them itself.** Versions the deleted row superseded, which live
  in the undo chain, are a separate and existing leak.

### 1.5 The locks, the insert side and the pool

- **Units and modes.** The units are `kRelation`, `kRange`, `kSlice` and
  `kTuple`; the modes are `IS`, `IX`, `S` and `X`. A `Tuple` unit is
  `[pk, pk + 1)` (`lock_table.hpp:275-315`, `:361-363`).
- **`DELETE`:**
  - `IS` at resolve (`command_dispatcher.cpp:9071`);
  - `Range X` before the walk for a window wider than one key
    (`:9147-9154`). The window comes from `DeclaredWriteBorrow`
    (`:8564-8665`), which skips `<>`, `IS NULL` and `IS NOT NULL`
    (`:8644-8648`) and treats `hi - lo <= 1` as one key (`:8657`);
  - otherwise `Tuple X` per row through `BorrowChain`, which takes the
    relation's `IX` first (`:9190`, `:8497-8503`).
- **A range unit has a price.** Taking one *"would raise the relation's
  fence counter - which sends every other writer of that relation through
  the all-partition scan"* (`:8652-8656`). BH-R4 takes none.
- **A named `INSERT`:**
  - `IX` (`:4724-4727`), then `Tuple X` on its key through `BorrowOrWait`
    with `refuse_if_never_admissible` (`:5302-5344`).
  - A refused ask looks the key up. A decided tombstone is `AlreadyExists`
    at once; an absent key is a wait.
  - A granted ask meets the tombstone in `BtreeInsert`, under the leaf's
    exclusive latch (`btree.cpp:898-904`). Since AT-S21 an insert logs
    under that latch.
- **A descent can refuse.** `DescendTo` answers `TxnConflict` after
  `kMaxDescentRestarts` (`btree.cpp:228-234`). `LeafStillCoversKey`
  fetches the right sibling while the leaf is pinned (`:99-113`).
- **The pool.**
  - A mutation's writes run inside a `NoRefuseWindow` of `kWindowFrames`
    (64) frames (`include/kds/storage/page_store.hpp:569`, `:574-586`).
  - **Drain mode** is for *"the passes that are not a row's mutation and
    hold no page latch between their steps"* (`:362-367`). Rollback
    (`manager.cpp:563`) and the assertion loops
    (`assertion_check.cpp:699`, `:767`) use it.
  - **Drain mode drains only a fill made with no pin held**
    (`src/storage/device_page_store.cpp:741`). The sibling fetch above
    holds one, so it can still be refused `ResourceExhausted`.
- **Durability.** `WalManager::Commit` appends `TXN_COMMIT` and syncs per
  durability class (`src/wal/manager.cpp:397-412`). A commit record
  appended after every purge record covers them all.

### 1.6 What rests on K1, and whether a purged and re-placed key reads right

The survey looked for any reader that returns a wrong row, counts a row
twice or files a row under the wrong group once a pk is purged and placed
again. **It found none, under one condition:** the purge must not write a
second assertion departure.

| consumer | what it keeps of a deleted row's pk | after `PURGE` and a re-insert |
|---|---|---|
| **Secondary index** | Its entries. `DELETE` *"does nothing"* (`docs/spec/index.md:77-80`), and no removal path exists (`include/kds/storage/index/index_tree.hpp:74-80`, `index.md:736-740`). | A probe dedups pks (`step_vm.cpp:1322`, `:1369-1379`), resolves each by descent (`:1435`) and re-applies MVCC and the whole residual (`:2095-2152`). `COUNT` is never served from an index (`index.md:322-323`). **Right answer.** A byte-identical entry is not stored twice (`src/storage/index/index_tree.cpp:312-331`), but a re-insert with the same key and a different covered value lengthens a run of equal `(key, pk)`, which is the equal-sort-keys defect's refusal (BH-R9) |
| **Cabin** | Its entries. Nothing removes one on delete (`docs/spec/cabin.md:310`; `include/kds/stats/cabin_store.hpp:39-43`). | Every consumer looks the pk up and re-checks: a serve (`step_vm.cpp:1540`, `:1569-1577`), the foreign-key reverse check (`src/exec/fk_check.cpp:209`, `:237`, `:273-290`, `:305-314`) and the inner build (`step_vm.cpp:1179-1204`). Heal drops dangling entries (`src/exec/cabin_optimizer_exec.cpp:398-402`, `:429`), and the re-insert's own hook observes it (`command_dispatcher.cpp:5529`). **Right answer** |
| **Assertions** | A departure, written by `DELETE`. Groups are found by hash (`include/kds/exec/bound_cabin.hpp:325`) from the row's values at write time (`assertion_check.cpp:481-486`). Entry pks are written and never resolved. | The re-insert writes one arrival, in the group its new values choose. **Right answer, provided `PURGE` writes no departure.** A second one would understate the group, since `ApplyDeparture` guards only overflow (`src/exec/bound_cabin.cpp:358-363`), and a later insert could be admitted wrongly: `[quiet-wrong]` (BH-R8). Latent: §8.1a's pk-membership protocol (`docs/spec/assertion.md:710-712`) would misread a purged pk, and it is not built (`:727-728`) |
| **Foreign keys** | Nothing. A child cannot name a delete-marked parent (`fk_check.cpp:110-115`; `docs/spec/foreign-keys.md:479-483`), and the parent's `DELETE` is RESTRICT under `X` (`command_dispatcher.cpp:9213`). | A reader that can see a child with fk = `P` holds a snapshot older than `P`'s delete. The horizon gate then keeps `P`'s tombstone (inference). **Right answer.** A reference outside a declared foreign key points at the new row, which is what W2 asks |
| **Waystone** | Trail entries keyed `(step_id, derived pk)` (`docs/spec/waystone-concpets.md:39`). | `VerifyTupleAt` checks the pk at the slot (`src/exec/tuple_verify.cpp:30-42`), and `AcceptTupleAt` applies MVCC (`step_vm.cpp:667-674`). A replay lands where a fresh descent would. **Right answer** |
| **Statistics, budget** | Nothing by pk (`include/kds/stats/access_stats.hpp:29-35`, `instance_key.hpp:45`). A key below the mark does not move it (`keystoneid-invariant.md:136-140`). | Unchanged |

### 1.7 The front end

- **The statement.**
  - A parsed statement is a variant (`include/kds/parser/ast.hpp:946-948`),
    named by `StatementTypeName` (`src/parser/ast.cpp:6-26`).
  - `DeleteStmt` is `{table_name, schema, table_byte_offset, where}`
    (`ast.hpp:756-761`). Its comment says *"physical retirement is a purge
    pass that does not exist"* (`:747-749`).
  - `ParseDelete` (`src/parser/parser.cpp:2092-2110`) uses
    `ParseOptionalWhere`.
- **Heads are matched by text, in two places.**
  - `Parse()` takes a `kIdent` head (`parser.cpp:2166-2169`, `:2271`).
  - The dispatcher routes on the same token (`DispatchInner`,
    `command_dispatcher.cpp:1230`), and `RequiredRole` is keyed on exactly
    those tokens (`:980-1010`).
  - An unknown head is refused with a list of the supported heads
    (`parser.cpp:2288-2291`), and that list is pinned in the manual
    (`manual/sql/sql.md:1031`).
  - **`DELETE` is not reserved** (`docs/spec/parser-v2.md:232-234`). Only 11
    words are (`src/parser/lexer.cpp:38-43`), and reserving one breaks
    every name spelled that way (`parser.cpp:96-99`;
    `include/kds/parser/token.hpp:38-43`).
- **The fingerprint.** Only `SELECT`, `INSERT` and `UPDATE` get a pattern
  (`src/parser/fingerprint.cpp:129-131`). The bump rule does not move for a
  statement that has none (`include/kds/parser/fingerprint.hpp:182-197`;
  version 2 at `:222`).
- **From WHERE to pk:**
  - `PkLiteral` admits a `kCompareValue` conjunct on the first column with
    a non-negative `kInt` literal, and turns a negative one into a miss
    (`command_dispatcher.cpp:5851-5877`, `:5871`).
  - `PkEqualityTarget` handles exactly one equality (`:5879-5896`).
  - `DeclaredWriteBorrow`'s fold is §1.5's. `BETWEEN` is lowered at
    `src/exec/step_compiler.cpp:1450-1470`.
  - **Positions.** `ColumnName::byte_offset` (`ast.hpp:165`),
    `AstValue::byte_offset` (`:103`) and `FunctionCall::byte_offset`
    (`:250`). `Condition` has no offset of its own.
  - **The refusal precedent** is the catalog-view WHERE: it refuses by
    shape, before any row is read (`command_dispatcher.cpp:6326-6361`).
- **Autocommit and explicit transactions.**
  - Autocommit is `Session::State::kIdle`
    (`include/kds/server/session.hpp:33`, `:125`). `BeginWrite` opens an
    owned transaction then (`command_dispatcher.cpp:8287-8296`).
  - Today's refusals inside a transaction are bare `ERR` strings, which
    KWP reads as `InvalidArgument` (`:7993-7997`, `:8206-8207`,
    `:8236-8237`; `src/server/kwp_session.cpp:694-706`).
  - A statement-level `NotImplemented` refused before `BeginWrite` existed
    until `d014aa2e` removed it with the peer-owned route.
  - A refusal before `BeginWrite` poisons nothing (`:8333`, `:8891`).
- **Codes and the registry.**
  - `Unsupported` against `NotImplemented` is *"whether a later release
    could lift it without changing the architecture"*
    (`include/kds/base/status.hpp:43-52`, `:125-141`).
  - Reusing a code needs no registry entry (`docs/spec/protocol.md:142-145`).
  - The byte travels in the text (`include/kds/wire/error_registry.hpp:185-191`).
- **Roles.** There are three: `kReadOnly`, `kReadWrite` and `kAdmin`
  (`include/kds/server/role.hpp:32-35`). `RequiredRole` makes `INSERT`,
  `UPDATE` and `DELETE` read-write, and *"everything unclassified"* admin
  (`command_dispatcher.cpp:980-1010`).
- **The reply.** `DELETED n` (`:9360`) reaches KWP as `rows_affected`
  through the outcome's count (`kwp_session.cpp:748-758`).
- **Heap precedents.**
  - `CREATE TABLE … HEAP` is `Unsupported` (`parser.cpp:792-811`).
  - A heap foreign-key parent is `NotImplemented`
    (`src/catalog/foreign_key.cpp:20-26`).
  - An index on a heap is `InvalidArgument` (`catalog.cpp:3278-3282`).
  - **A heap refuses a named key below its mark** (BB-R12, kept by BD-Q4
    (a); `heap-and-tuple.md` §4.1), because its chain grows only at its
    tail.

### 1.8 The tests and the sim

- **BD-S1's guard.**
  `SortedLeafSqlTest.ADeletedKeyNamedAgainIsAlreadyExists`
  (`tests/sorted_leaf_named_keys_test.cpp:161-171`), with its census at
  `:174-198` (`{"1601", kAlreadyExists} // deleted: bound once`). The
  storage form is `tests/btree_test.cpp:1782`.
- **Red-first cells are committed red.** BD-S1 committed its cells red at
  `4debe8d9`, each naming the defect it shows, and later stages turned them
  green.
- **Two-session named-key waits.** `NamedKeyWaitTest`
  (`tests/lock_family_test.cpp:2812-2848`).
- **`DELETE` semantics.** `tests/txn_session_test.cpp:490-561`.
- **The sim.**
  - `Op::Kind` (`sim/workload.hpp:61-79`). The `kDelete` generator
    remembers keys, so a later `kInsertNamed` names them again
    (`sim/workload.cpp:191-206`; `workload.hpp:109-112`).
  - `Oracle::Named` judges placed, duplicate or exhausted from `consumed_`
    and `pending_` (`sim/oracle.cpp:85-100`; `oracle.hpp:133-140`,
    `:229-233`).
  - A new op touches `workload.cpp:20-50` and `sim/loop.cpp:318-328`,
    `:470-540`, `:973-989`.
  - The sim drives heap tables too (`sim/sim_main.cpp:53`).

### 1.9 The texts that rest on K1

To restate at BH-S5 (BH-R12):

- **Rules and specs:**
  - `keystoneid-invariant.md:18-29` (K1, *"owes a keyed"* at `:26`) and
    `:64-89` (its reasons).
  - `heap-and-tuple.md:109`, `:132` (*"a committed key is never bound
    again"*), `:157` (BD-R5's duplicate), `:178` and `:347` (invariant 11).
  - `namespace.md:62` (*"`(oid, pk)` stays forever-unique"*).
  - `cabin.md:101-124`, `:316-324`, `:362-364` and `:713`.
  - `assertion.md:222`.
  - `foreign-keys.md:16-21`.
  - `known-gaps.md:657-669`.
  - `CLAUDE.md`'s invariant 11, and its Keystone id and Caller-supplied pk
    rows.
- **Code:**
  - `btree.cpp:841-845`, the user-visible *"a Keystone id is bound once"*,
    and `:896-897`.
  - `step_vm.cpp:1439` and `:1574`.
  - `fk_check.cpp:241`.
  - `cabin_store.hpp:77-80` and `:117-124`.
  - `include/kds/storage/cabin_bound_page.hpp:30`.
  - `include/kds/catalog/rows.hpp:832` and
    `include/kds/catalog/well_known.hpp:239`.
  - `ast.hpp:747-749`.
- **The manual.** `sql.md:202-203` (*"stays bound for the life of the
  relation, even after `DELETE`"*), `:589-590` (*"Nothing purges."*),
  `:1031` (the supported-head list), §7 (`:907`) and §8 (`:1004-1046`).

**True as they stand, and read so the grep is not misread:**

- `ddl-transactional.md:74`. A rolled-back `CREATE TABLE`'s ids are
  catalog ids, and `PURGE` refuses system relations.
- `catalog.cpp:727` and `include/kds/exec/assertion_catalog.hpp:74`.
  `sys.assertions` is a heap catalog relation.
- `crosscore.md:275`. It is about issued ids.
- `index.md:128`. It is K2's stability under relayout.
- `src/storage/cabin_bound_page.cpp:76` and `cabin_bound_page.hpp:177`.
  They are about pk truncation.
- `page_header.hpp:180`.
- The oid and group-id hits: `bound_cabin.cpp:107`, `bound_cabin.hpp:68`,
  `:352`, `well_known.hpp:16`, `:171`, `:338`, `common.hpp:31` and
  `instance_visibility.cpp:278`.

### 1.10 Texts the tree already contradicts

- **`cabin.md:762-765`** says *"`DELETE` calls neither class's write
  hook"*. The code writes the bound class's departure
  (`command_dispatcher.cpp:9228-9243`; `include/kds/exec/assertion_check.hpp:44-49`),
  and the manual agrees with the code (`sql.md:596-598`). BH-R8 rests on
  that departure, so BH-S5 restates `cabin.md`.
- **`payload.hpp:383-385`** says *"Nothing purges yet, so today every
  SLOT_RETIRE in a stream is a rollback compensation"*. Two emitters log it
  at `kNoTxnId` (§1.2). BH-S5 restates it.
- **`catalog.hpp:324`, `:334`** call the catalog retire unlogged, and it is
  logged. Outside BH's scope; listed for `catalog.md`'s owner.
- **`varheap.hpp:44-48`** describes a below-horizon release that is not
  built. Outside BH's scope.

## 2. The specification

As BH-S5 lands it in `heap-and-tuple.md` §4.1c, K1 and
`manual/sql/sql.md` §3, on §5's proposals.

**PU1 — Syntax.**

```sql
PURGE FROM [ns.]t WHERE c1 [AND c2 ...]
```

- Each `ci` compares the relation's pk column with a non-negative integer
  literal, in one of six forms: `=`, `<`, `<=`, `>`, `>=`, or
  `BETWEEN a AND b`.
- The conjuncts fold to one window `[lo, hi)`, clamped to the key space
  `[1, 2^40 - 1]`.
- `PURGE` is a statement head matched by text, never a reserved word, so a
  relation or column named `purge` stays legal.

**PU2 — What is purged.** A key `k` in the window is purged when all three
hold:

1. Its slot is delete-marked.
2. Its deleter committed.
3. Every live and future reader sees that commit.

Purging retires the slot keyless and releases the deleted version's
var-heap cells. **From then on, an `INSERT` naming `k` is judged as if `k`
had never been placed**, with one known exception: the equal-sort-keys
defect can refuse it `AlreadyExists` (BH-R9).

**PU3 — What is not purged.**

- **A live key.** A window of one key (`hi - lo = 1`, however it is
  spelled) is refused `InvalidArgument` when that key is live. A wider
  window passes over live keys (BH-Q2).
- **An absent key** — never placed, rolled back, or already purged — is not
  a refusal. It purges nothing, so a `PURGE` can be run again.

**PU4 — Undecided writers.** When a key's latest writer is in flight — an
insert, an update or a delete — `PURGE` waits for that writer's decide and
judges the outcome. The wait is polled, under PU5's bound.

**PU5 — Older readers.**

- A delete-marked key whose deleter some live reader cannot yet see is
  waited on, polled.
- At `kPurgeHorizonWait` (1 s, a constant with no configuration key) the
  statement is refused `TxnConflict retryable=1`, naming the key and the
  reason, and nothing is purged.
- The `PURGE`'s own view never holds the wait: it is re-minted before
  every judging pass (BH-R5).

**PU6 — Judged whole, then written key by key.**

- `PURGE` judges every key in its window, waiting where PU4 and PU5 say,
  before it writes anything. A refusal while judging leaves the relation
  untouched.
- **Each key is then purged whole or not at all.** A refusal while writing
  — a descent that gives up, a full pool, an I/O error — or a crash can
  leave some of the window's keys purged and the rest not.
- **A refusal while writing keeps its code**, and its text says how many
  keys it purged first and that a re-run finishes the window.

**PU7 — The reply.** `PURGED n`, where `n` is the number of keys this
statement purged. It reaches KWP as `rows_affected`.

**PU8 — Durability.** The statement's commit record follows every purge
record, so the reply's durability is the session's durability class, as for
any commit.

**PU9 — Where it is admitted.**

- Autocommit only.
- A btree user relation only. The system-relation check runs before the
  heap check, since every system relation is a catalog heap chain. It asks
  `catalog::IsSystemNamespace` of the resolved relation's namespace
  (BH-Q17).
- The admin role.

**PU10 — The refusals.** Each is returned before the transaction is
touched, except the last three. Every byte travels in the text as
`(byte N)`.

| case | code | byte |
|---|---|---|
| inside `BEGIN` | `NotImplemented`; the transaction is not poisoned | 0 |
| a system relation | `Unsupported` (BH-Q17; Census D found `DELETE` refuses one only by accident) | the name |
| a heap relation | `Unsupported` | the name |
| no `WHERE` | `NotImplemented` | past the name |
| a conjunct outside PU1's six forms: another column, `<>`, `IS [NOT] NULL`, `IN`, a function, a subquery, a column-to-column comparison | `NotImplemented` | the conjunct's column, function or literal; a bare subquery's is the `WHERE` token |
| a negative literal, or one past 64 bits | `InvalidArgument` | the literal |
| a window of one live key | `InvalidArgument` | the literal |
| an older reader or an undecided writer at the bound | `TxnConflict retryable=1` | none; the key is named |
| a refusal while writing (PU6) | its own code | as it carries |
| the role | as today's role refusal | as today |

A window outside the key space names no key and purges nothing.

**PU11 — What `PURGE` does not touch.**

- secondary-index entries: left, and filtered at read;
- Cabin entries: left, re-checked at read, and healed;
- assertion directories: `DELETE` already wrote the departure;
- foreign keys: a purged key has no live child;
- Waystone trails: advisory;
- the issue cursor: a purged key is never issued, only named;
- the budget.

**PU12 — Against a racing `INSERT` of the same key.**

- The leaf's exclusive latch orders the two. An `INSERT` that reaches the
  leaf before the retire is refused `AlreadyExists`, and one that reaches
  it after is placed.
- A `PURGE` that then finds the slot no longer the tombstone it judged —
  re-placed, or already purged by another `PURGE` — skips it, and does not
  count it.
- The duplicate text for a delete-marked key names the way out (BH-Q13):

  > duplicate primary key k: a row with this key was deleted; PURGE frees
  > its key (page p slot s)

## 3. Rulings — CLA's proposals, not marked

### BH-R1 — K1 gains one named exception, `PURGE` (BH-Q1)

K1 is restated as follows:

> **K1 — Issue-once, with one named exception.** A Keystone id is bound to
> at most one committed tuple in the lifetime of a relation, except across a
> `PURGE` that freed it. No other path rebinds it: not the allocator, not
> delete-then-insert, not crash recovery, and not a purge the engine runs on
> its own, which still owes a keyed, never-visible tombstone. `PURGE` is the
> operator's statement. It frees one committed key at a time, visibly and
> logged, and only after no reader can see the row it deleted.

K1's three reasons are restated with their cost (§0):

1. Snapshot safety now rests on BH-R5's gate.
2. `(oid, pk)` is unique at any instant, and unique for life only up to a
   `PURGE`.
3. The audit claim carries the exception.

**K5 is the nearest precedent and differs in three ways.** K5 violates K1
*"once, atomically, and visibly"* (`keystoneid-invariant.md:220-240`):

- K5 is offline, and `PURGE` is online.
- K5 is one recoverable unit for the relation (`:232-234`), and `PURGE` is
  atomic per key.
- K5 invalidates every structure keyed on `(oid, pk)` wholesale
  (`:235-237`). `PURGE` invalidates nothing, because every such structure
  re-checks (§1.6).

Invariant 11 (`heap-and-tuple.md:347` and `CLAUDE.md`) becomes *"never
rebound except by `PURGE`"*.

### BH-R2 — The statement (PU1, PU9, PU10; BH-Q7, BH-Q9, BH-Q10)

- **Parsing.**
  - A `PurgeStmt` arm is added to the statement variant, with the same
    fields as `DeleteStmt`.
  - `ParsePurge` mirrors `ParseDelete` and is reached by a text-matched
    head, beside `DELETE` (`parser.cpp:2271`). The unknown-head list
    (`:2288-2291`) and its manual row gain `PURGE`.
  - No other production changes. A refusal's byte comes from the positions
    the AST already carries (§1.7), and a bare subquery's from the `WHERE`
    token.
- **Dispatch.** `DispatchInner` routes `PURGE` (`command_dispatcher.cpp:1230`).
  `RequiredRole` leaves it unclassified, so it is admin.
- **The window has one recognizer.** It is built from `PkLiteral` and
  `DeclaredWriteBorrow`'s fold, and never from a second recognizer for the
  same question.
  - `DeclaredWriteBorrow` skips conjuncts it cannot fold, `<>` among them,
    which is sound for a lock. `PURGE` refuses them instead (PU10), because
    a skipped conjunct would widen the purge: `id > 0 AND id <> 5` would
    purge 5.
  - `PkLiteral` turns a negative literal into a miss. The window builder
    tells a miss from a negative literal, so the second is refused
    `InvalidArgument`.
- **Order of refusals.** Every refusal in PU10 before the bound's row is
  returned before `BeginWrite`.
- **No pattern.** No fingerprint, no `sys.patterns` row and no access
  statistics.

### BH-R3 — What a purge frees (PU2, PU3; BH-Q2)

A slot is a purge target when, read under its leaf's hold:

- it is keyed `k`;
- `deleted` is set;
- its `trx_id` — the deleter, stamped by `DeleteMark` — is decided
  committed;
- `ResolvedForEveryReader(trx_id)` holds.

A slot whose latest writer is in flight is a wait (PU4), never a target. A
retired slot is absent. A live slot is PU3's case.

### BH-R4 — Locks: the relation only (PU4, PU12; BH-Q16) `[quiet-wrong]`

**What `PURGE` takes:** the relation's `IS` at resolve and its `IX`, as
every writer does, so a DDL's `X` and the `PURGE` wait for each other. **It
takes no `Tuple` or `Range` unit.**

**Why no row or range unit is needed:**

- **No writer touches a resolved tombstone.** A `DELETE` or an `UPDATE`
  classifies before it writes, and every snapshot sees a resolved deleter,
  so every writer skips the slot (§1.1).
- **A named `INSERT` of `k` meets the tombstone under the leaf's exclusive
  latch** (§1.5). The latch orders it against the retire, and the insert
  logs under that latch, so its record follows the retire's in the log.
- **Another `PURGE` of `k`** is settled by the same latch. The second one
  finds `k` absent and skips it.
- **The phase-2 verify** (BH-R6) skips any slot that is no longer the
  tombstone phase 1 judged. That is the one check this design rests on,
  which is why BH-R4 is `[quiet-wrong]`: a retire without it could retire a
  re-placed live row.

**What the absence of a range unit buys:**

- no fence counter raised for the relation's other writers (§1.5);
- no cycle hidden from the wait-for graph. A REPEATABLE READ reader that
  holds the horizon and then writes into the window would wait on a range
  unit while the `PURGE` polls on its snapshot.

**What it costs:** an undecided writer in the window is waited on by
polling `IsInFlight`, under PU5's bound, rather than by the lock family's
wake.

### BH-R5 — The horizon gate `[quiet-wrong]` (PU5; BH-Q3)

- **No slot is retired until `ResolvedForEveryReader(deleter)` holds.**
  Removing the check is BH-R11's first mutation. It is killed by a
  REPEATABLE READ reader that began before the `DELETE` and reads the row
  after the `PURGE`.
- **The `PURGE`'s own view.** Its owned transaction registers a view at
  `Begin` (§1.3). That view is re-minted before every judging pass, as a
  READ COMMITTED statement boundary re-mints, so it never predates a
  deleter the `PURGE` waited for.
  - The `PURGE` reads no row through that view: it judges slots by their
    deleter's state.
  - BH-R11's commit-arm cell kills the mutation that skips the re-mint.
- **The wait polls.** It belongs to the class `known-gaps.md` calls *"a
  refusal with no slot behind it"*, re-asked at each re-drive and bounded
  by `kPurgeHorizonWait`, a 1 s constant with no key (BH-Q3).
  `lock_wait_fault_net_ms` is left alone: it bounds a lost wake, and an
  open snapshot is not one (§1.3).
- **Census A, at BH-S1.** It covers:
  - every kind of view that is not registered with the horizon, the two
    exemptions of `txn.md:494-498` and the `PURGE`'s own view among them;
  - every site that decodes a row's spills — `DecodeRow`, `DecodeAndResolve`
    and their callers — with whether it classifies the version before it
    resolves a spill, or copies a payload under a hold and resolves it
    later.

  Each entry is **shown** unable to read a superseded version of a user
  row, or its spills, while a `PURGE` runs on another core. Otherwise it is
  **registered**, or made to classify first, before BH-S3 opens the
  statement. **BH-S3 does not start with a census row open.**

### BH-R6 — Judge first, write key by key (PU6; BH-Q5)

**Phase 1, judging. Nothing is written.**

1. Take BH-R4's units.
2. Re-mint the view (BH-R5), then walk the window under shared holds.
   Collect each target as `(k, deleter)`, and each pending key: an
   in-flight writer, or a deleter not yet resolved.
3. While any key is pending, wait as PU4 and PU5 say, then re-judge the
   pending keys from step 2.

Any refusal in this phase leaves the relation untouched.

**Phase 2, writing.** Under `DrainOnPressure` (§1.5), for each target in
key order:

1. Descend to `k` with the leaf held exclusive, and `SearchLeaf` for it.
   The row may have moved since phase 1, through a divide or through any
   insert into the leaf.
2. **If the slot is absent, or is not delete-marked by the judged deleter,
   skip it.** Another `PURGE` took it, or a racing `INSERT` re-placed it
   (PU12).
3. Copy the slot's spill references, then retire it and log the retire,
   all under the hold (BH-R7).
4. Release the hold, then release the copied spills.

A refusal in phase 2 keeps every key already purged. It is reported as
PU6 says.

### BH-R7 — Logging, recovery and durability (PU6, PU8; BH-Q6, BH-Q11)

- **The primitive** is btree-level. It finds and verifies the slot, and
  hands it back held, writing nothing (`btree::BtreeHoldTombstone`). The
  caller (`exec::PurgeKey`) reads the version's spills off the held page,
  then retires the slot and logs it under the same hold, then releases. The
  primitive cannot find spills itself, because it has no schema. Decoding
  before the retire means a cell that cannot be read refuses the key with
  the page untouched (BH-S2's review, finding 2, which changed this text
  rather than the code).
- **The retire.** `SLOT_RETIRE` at `kNoTxnId` through
  `exec::LogSlotRetire`, under the leaf's exclusive hold, as AT-S21 logs
  every mutation under the hold that placed it.
- **The release.** Then `VARHEAP_RELEASE` at `kNoTxnId` for each copied
  spill, through `txn::ReleaseVarHeapSlot`, under the var-heap page's hold.
- **Retire first, release second.**
  - A crash between the two leaks that row's spills, which is a stated
    gap.
  - The reverse order is `[quiet-wrong]`. A crash after the release leaves
    the tombstone standing, so a re-run releases its spills again. By then
    another value may occupy those var-heap slots, and the re-run frees
    it.
- **Recovery.**
  - Redo applies both records with today's appliers (§1.2).
  - Analysis never counts a `kNoTxnId` record as a loser's. The `PURGE`'s
    own transaction writes only `TXN_BEGIN` and `TXN_COMMIT`, so a crash
    before its commit undoes nothing.
  - A loser `INSERT` of `k` placed after the retire is found by key, as
    recovery undo finds any row (`recovery_undo.cpp:162-191`). Its records
    follow the retire's (BH-R4).
- **Durability.** The `PURGE`'s `TXN_COMMIT` follows every purge record,
  and `WalManager::Commit` syncs per durability class (§1.5). The reply
  needs nothing more.
- **No record kind is added**, so the superblock stays at 21, where BF-S3
  left it (BH-Q6 (a)).

### BH-R8 — Not `DELETE`'s per-row code

`PURGE` writes:

- no assertion departure;
- no foreign-key check;
- no Cabin or index hook;
- no undo record;
- no trail entry.

**A purge built on `DELETE`'s `mark` lambda would write a second
departure** (§1.6). BH-R11 carries that as a mutation, killed by an
assertion cell.

### BH-R9 — What the auxiliaries do, and the refusal BH adds (BH-Q8)

- **Every consumer reads a purged and re-placed key right** (§1.6), so BH
  changes none of them.
- **No index entry is removed.** Removing one would end AT-S15's premise
  (`index_tree.hpp:74-80`), and `LeafStillCoversKey` would have to be
  re-proved.
- **The equal-sort-keys defect gains a third path.** A key that is purged
  and placed again with a different covered value adds an equal
  `(key, pk)` entry, as an `UPDATE` does. About 600 such rounds reach the
  defect's refusal. BH-S5 adds the path to the bug entry, and to
  `heap-and-tuple.md`'s statement of the one exception to BD-R5.

### BH-R10 — The refusals

PU10's table is the list. BH-S3 builds each row with its code and byte,
and BH-R11 carries a cell per row.

### BH-R11 — Checked, not only argued

**SQL cells:**

- a deleted key is purged, then named and placed;
- an unpurged deleted key named again is still `AlreadyExists` (BD-S1's
  guard, kept green);
- an absent key purges nothing, twice;
- a live key is refused under `id = 5` and under `id BETWEEN 5 AND 5`
  alike, and passed over in a wider window;
- `id > 0 AND id <> 5` is refused, and 5 stays a tombstone;
- each row of PU10's table, with its code and byte;
- inside `BEGIN`, the refusal does not poison the transaction;
- a heap relation built by the test seam is refused;
- after a purge and a re-insert:
  - `SELECT` by pk, by an index on the old and the new value, and through a
    Cabin each answer the new row once;
  - an assertion group's `COUNT` and `SUM` match a recount (BH-R8's cell).

**Lock and horizon cells, on one core and on two:**

- a REPEATABLE READ reader that began before the `DELETE` keeps its row
  across a `PURGE` attempt, and the `PURGE` is refused at the bound;
- the same reader ends, and the `PURGE` then succeeds;
- **an undecided `DELETE` commits while a `PURGE` waits on it, and the
  `PURGE` succeeds** — the commit arm, which the `PURGE`'s own view would
  block without the re-mint;
- an undecided `DELETE` rolls back while a `PURGE` waits on it, and the
  key stays live;
- a named `INSERT` racing a `PURGE` on another core: refused before the
  retire, placed after it;
- two `PURGE`s of one key on two cores: one purges it, the other skips it;
- a `PURGE` re-judging a key that was purged and re-placed between its two
  phases skips the live row.

**Crash cells, on the file rig at every cut:**

- after the retire and before the release;
- after the release;
- in the middle of a ranged window;
- after a re-insert of the purged key, both committed and uncommitted;
- redo applied twice.

**The sim:**

- a `kPurge` op, both the equality and the ranged form;
- the oracle removes a purged key from `consumed_`;
- a `PURGE` cut by a crash or refused while writing moves its keys to
  `pending_`, so a later named insert may be placed or be a duplicate;
- `kInsertNamed` names purged keys.

**Mutations, each repeated:**

- the horizon check removed;
- the re-mint skipped, killed by the commit-arm cell;
- the deleter's commit check removed;
- the live check removed, which retires a live row;
- the phase-2 skip turned into a retire, killed by the re-judge cell;
- the release before the retire;
- the release omitted;
- `DELETE`'s departure written, killed by the assertion cell;
- `<>` folded instead of refused;
- the record's envelope given the statement's transaction id.

Each mutation is killed by a cell, or listed at BH-S4 with the reason it
cannot be.

### BH-R12 — The text

- **Restated:** every row of §1.9, and §1.10's first two rows.
- **The new specification** is §2, landed as `heap-and-tuple.md` §4.1c.
- **The manual** gains a PURGE section after DELETE, and loses *"Nothing
  purges."* and *"stays bound … even after `DELETE`"*.
- **`known-gaps.md`:**
  - its Keystone id entry is restated: an engine-internal purge still owes
    a keyed tombstone, and `PURGE` frees a key by design;
  - an entry is added for each item of §0's "What bounds BH".
- **Done when** a grep of `src/ include/ docs/spec/ docs/rules/ manual/
  CLAUDE.md` for *"never rebound"*, *"bound once"*, *"never bound again"*,
  *"stays bound"*, *"for the life of the relation"*, *"never reused"*,
  *"issue-once"*, *"never names another row"*, *"forever-unique"*,
  *"owes a keyed"*, *"Nothing purges"* and *"purge pass that does not
  exist"* finds every remaining hit true.

## 4. Stages

| stage | what | done when | size |
|---|---|---|---|
| BH-S0 | **The order** | <ul><li>This file</li><li>Its `critics-developer` review (§7)</li><li>The words recorded in `raft-marks-2026-10-08.md`</li><li>The index row</li></ul> | S |
| BH-S1 | **The census, and red first** | <ul><li>**Census A** (BH-R5): every unregistered view, the `PURGE`'s own included, and every spill-decoding site, each shown safe across a `PURGE` on another core or listed for BH-S3 to register or reorder.</li><li>**Census B** (§1.6): one cell per consumer, through a test-only seam that retires a committed tombstone keyless and re-places its key with different values: index probes on the old key, the new key, a range and a join; a Cabin serve, the foreign-key reverse check through a Cabin, and the inner build; assertion `COUNT` and `SUM` against a recount; a Waystone replay. **Expected green.** A red one stops BH at this row for a ruling.</li><li>**Census C** (§1.9): BH-R12's grep, run, with every hit classed.</li><li>**Census D:** how `DELETE` refuses a system relation, which PU10 follows.</li><li>**Red at BH-S0's commit**, committed red as BD-S1's were (§1.8): `PURGE` after `DELETE`, then the named `INSERT`, expecting it placed; the REPEATABLE READ reader's cell; the commit-arm cell.</li><li>**Guard, green and kept green:** BD-S1's `ADeletedKeyNamedAgainIsAlreadyExists`.</li></ul> | M |
| BH-S2 | **The primitive** (BH-R3, BH-R6 phase 2, BH-R7) | <ul><li>**Code:** the btree-level find and verify, handing back the held slot; the caller decodes the spills, retires the slot and logs `exec::LogSlotRetire` at `kNoTxnId` under the hold, then `txn::ReleaseVarHeapSlot` at `kNoTxnId` for each copied spill. A verify mismatch is a skip. No caller from SQL yet.</li><li>**Cells:** a purged key re-placed at the storage level; a live slot, an undecided deleter and a re-placed key, each skipped or refused as BH-R3 says; the crash cells of BH-R11 at the storage level; redo applied twice; a loser re-insert after a purge undone at mount.</li><li>**Mutations, each repeated:** the skip turned into a retire; the release before the retire; the release omitted; the envelope given a transaction id.</li><li>**The suite green**, except the red cells BH-S1 committed, and the waystone, index, cabin and inner-build contract suites byte-identical.</li></ul> | M |
| BH-S3 | **The statement** (BH-R2, BH-R4, BH-R5, BH-R6 phase 1, BH-R8, PU10) | <ul><li>**Opened only once Census A has no row open.**</li><li>**Code:** the arm, `ParsePurge`, the dispatch route, the window from the one recognizer, every refusal in PU10, the relation units, the re-mint, the polled wait under `kPurgeHorizonWait`, phase 1, the phase-2 loop under `DrainOnPressure`, the reply with its count, and the duplicate text (BH-Q13). The `PURGE` counted in BF-S5's statement epoch, with a cell: a drop's reclaim waits for a `PURGE` in flight on its relation.</li><li>**Green:** BH-S1's red cells, and BH-R11's SQL and lock cells.</li><li>**Mutations, each repeated:** the horizon check removed; the re-mint skipped; the deleter's commit check removed; the live check removed; `DELETE`'s departure written; `<>` folded.</li><li>**The suite green**, also under `KDS_TEST_PAGE_LATCH=1`.</li></ul> | L |
| BH-S4 | **The sim and the rigs** (BH-R11) | <ul><li>the `kPurge` op, both forms, with the oracle's `consumed_` and `pending_` rules;</li><li>the two-core cells;</li><li>the crash cells at the SQL level;</li><li>`scripts/sim.sh` green;</li><li>each of BH-S2's and BH-S3's mutations killed from the sim or a rig, or listed in this row with the reason it cannot be.</li></ul> | M |
| BH-S5 | **The close** | <ul><li>A row per stage, and what bounds BH.</li><li>**The measurement** (§6).</li><li>**The text** (BH-R12), its grep included: K1 and its reasons, invariant 11, `heap-and-tuple.md` §4.1c, `namespace.md`, `cabin.md`, `assertion.md`, `foreign-keys.md`, the manual, the bug entry, `payload.hpp`'s comment.</li><li>**`known-gaps.md`**, as BH-R12 lists.</li><li>**`CLAUDE.md`'s** invariant 11, Keystone id row and Caller-supplied pk row.</li></ul> | M |

## 5. Items for the operator

W3 directed the order to be written on the proposals of CLA's reply to W2
(the header's table). Every mark below was **adopted on the operator's
standing go-ahead of 2026-10-08** (`go-ahead-achieving-milestone`;
`raft-marks-2026-10-08.md` §5). Each is CLA's proposal, and none is a
per-item mark of the operator's own.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BH-Q0 | **Open BH**, with BH-S0..S5 and BH-R1..R12 as written | process | Yes | **Yes**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q1 | **K1's exception** (BH-R1). Reply points 7 and W2.<br>(a) `PURGE` is K1's one named exception; every other path, an engine-internal purge included, still owes a keyed tombstone.<br>(b) K1 is struck, and a deleted key is free at its delete's commit (W1 as first given) | invariant; **[quiet-wrong]** | (a), which is W2. (b) frees a key on every `DELETE`, so every view, Cabin and assertion path must be re-proved for every delete, where (a) proves it for one statement | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q2 | **A live key in the window** (PU3). Reply point 1: *"naming a live row is refused `InvalidArgument`"*.<br>(a) Any live key in the window refuses the statement.<br>(b) A window of one key refuses a live key, however it is spelled; a wider window passes over live keys | user-visible; **revises point 1** | (b). Under (a) a range over a relation with live and deleted keys interleaved could never run. Deciding by the folded window keeps `id = 5` and `id BETWEEN 5 AND 5` one statement | **(b)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q3 | **The wait on older readers** (PU5, BH-R5). Reply point 2: wait, then `TxnConflict` at the 1 s fault net.<br>(a) A polled wait bounded by `kPurgeHorizonWait`, a 1 s constant with no key, then `TxnConflict retryable=1`.<br>(b) Refuse at once, `TxnConflict retryable=1`.<br>(c) Bound it by `lock_wait_fault_net_ms`, re-scoped to cover a wait the engine cannot wake | design, user-visible; **revises point 2** | (a). It keeps point 2's wait and its second. (c) changes what the key means: it bounds a lost wake and logs its firing as a fault (§1.3), so one key would hold two quantities, and setting it to `0` for fast lock-fault detection would make every `PURGE` refuse while any older snapshot exists. Under OLTP load some statement's snapshot nearly always predates a `DELETE` that just committed, so (b) would refuse most `PURGE`s issued right after their `DELETE` | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q4 | **Inside an explicit transaction** (PU9). Reply point 3.<br>(a) Autocommit only; inside `BEGIN` refused `NotImplemented`, before `BeginWrite`, unpoisoned.<br>(b) Admitted, with an undo image so that a rollback restores the tombstone | scope | (a). (b) needs a new undo record, a restore through a slot the purge freed, and a re-proof of BH-R4 under a transaction that keeps writing. `NotImplemented` and not `Unsupported`, because (b) is buildable without changing the architecture | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q5 | **Atomicity** (PU6, BH-R6). Reply point 3: one record per key, no rollback.<br>(a) Judged whole before written; each key purged whole; a refusal while writing, or a crash, can leave the window partly purged; a refusal says how many keys it purged; a re-run finishes.<br>(b) All-or-nothing per statement, through a new record kind or undo images | design, user-visible; **revises point 3** | (a). The revision is the refusal: the review found three ways phase 2 can be refused after a write (§1.5), so a crash is not the only partial outcome. A partly-run `PURGE` leaves every key in a state the operator asked for, and the statement is idempotent. (b) adds a record kind (BH-Q6 (b)) for a maintenance statement | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q6 | **The format** (BH-R7). Reply point 3: *"a new record kind, so the superblock moves"*.<br>(a) Reuse `SLOT_RETIRE` and `VARHEAP_RELEASE` at `kNoTxnId`, whose purge envelopes already exist; no record kind is added; the superblock stays at 21.<br>(b) A new `BTREE_PURGE` kind; superblock 22 (BH-Q15) | format; **revises point 3** | (a). The survey found both purge envelopes already emitted and applied (§1.2). No on-disk meaning changes: an older engine reads a purged key as never placed, which is what BH means by it. So the standing order (`raft-marks-2026-10-07.md` §11) does not apply | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q7 | **The syntax** (PU1, PU10). Reply point 4.<br>(a) `PURGE FROM t WHERE` in PU1's six forms only, a text-matched head; no `WHERE` is `NotImplemented`.<br>(b) As (a), and `PURGE FROM t` with no `WHERE` purges every purgeable key | user-visible | (a). A whole-relation form is a sweep, and none was asked for. Refusing it is `NotImplemented`, so a later word can add it | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q8 | **Secondary-index entries** (BH-R9). Reply point 5.<br>(a) Left; reads re-check; the equal-sort-keys defect gains a path.<br>(b) Removed by the purge, re-proving AT-S15's leaf coverage | design | (a). §1.6 found every probe re-checks. (b) ends the premise AT-S15's coverage check stands on | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q9 | **Heap and system relations** (PU9). Reply point 6.<br>(a) A heap is `Unsupported`; a system relation is refused as `DELETE` refuses it.<br>(b) A heap is purged too | scope | (a). A heap refuses a named key below its mark because its chain grows only at its tail (§1.7). Freeing such a key would need a different chain, so this is the architecture and not a missing build. No new heap is created (SUS-1) | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q10 | **The role** (PU9). New.<br>(a) Admin: `RequiredRole`'s unclassified default.<br>(b) Read-write, beside `DELETE` | user-visible | (a). `PURGE` lifts an identity guarantee, and an admin session is the one that owns that decision | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q11 | **The tombstone's spills** (BH-R7). New.<br>(a) Released by the purge, after the retire, at `kNoTxnId`; a crash between leaks them, stated.<br>(b) Left, a stated leak per purged row | design | (a). Without it every purged row with a spill leaks for good (§1.4) | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q12 | **The reply** (PU7). New.<br>(a) `PURGED n` as `rows_affected`, and no counter.<br>(b) As (a), plus a `SHOW META` counter beside `catalog_marks_purged=` | user-visible | (a). The reply already carries the count | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q13 | **The duplicate text** (PU12). New. *"a Keystone id is bound once"* becomes *"PURGE frees its key"* | user-visible | Yes. The old text would be false, and the new one names the way out | **Yes**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q14 | **The measurement** (§6), once at BH's close, per `CLAUDE.md`'s Session Workflow step 3. New | process | As written | **As written**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q15 | **The superblock number.** New. Only needed under BH-Q6 (b), since BF-S3 already took 21 | format | Moot under BH-Q6 (a). Under (b): 22, with 21 refused by the standing order | **Moot (BH-Q6 (a))**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q16 | **Locks** (BH-R4). New, raised by the review.<br>(a) The relation's `IS` and `IX` only; races settled by the leaf latch and the phase-2 skip; an undecided writer polled.<br>(b) As `DELETE`: `Tuple X` for one key and `Range X` for a wider window, held to the end, with the view re-minted after every lock wait | design; **[quiet-wrong]** | (a). Every writer already skips a resolved tombstone (§1.1), and the leaf latch orders an `INSERT` against the retire, so a row or range unit excludes nothing that could change the answer. (b) raises the relation's fence counter for every other writer, and hides a cycle from the wait-for graph: a reader holding the horizon waits on the range unit while the `PURGE` polls on its snapshot | **(a)**, adopted 2026-10-08 on the standing go-ahead |
| BH-Q17 | **A system relation** (PU9, PU10). New at BH-S1, raised by Census D: `DELETE FROM sys.tables` is not refused by design. The name resolves, and `InitTableAccess` answers `NotFound` *"no columns for this rel_id"*, because a bootstrap relation has no `sys.columns` rows (`catalog.cpp:2199`).<br>(a) An explicit check, `catalog::IsSystemNamespace` on the resolved relation's namespace, refused `Unsupported` at the name's byte, before the heap check.<br>(b) Follow `DELETE`: the same incidental `NotFound` | user-visible | (a). A refusal that comes from a missing catalog row would change the day the catalog learns its own columns, and `NotFound` says the relation is absent when it is not. `Unsupported`, because the catalog's marks are the engine's own purge (§1.2), not the operator's to free. `DELETE`'s accident is outside BH: listed for `known-gaps.md` at BH-S5, and pinned by Census D's cell | **(a)**, adopted 2026-10-08 on the standing go-ahead |

Four items, and two rules, are `[quiet-wrong]`, where a wrong choice turns
a refusal into a wrong answer:

- **BH-Q1**, by its nature.
- **BH-Q16**, because (a) rests on the phase-2 skip.
- **BH-R5's gate** and **BH-R7's retire-first order**, which are not
  items, because no option leaves them out.

## 6. Sequencing and measurement

1. **BH-S1 now.** It touches no engine code beyond a test-only seam.
2. **BH-S2 after BH-S1.**
3. **BH-S3 only once Census A has no row open.**
4. **BH-S4 after BH-S3.**
5. **BH-S5 last.**

BF has closed. BH runs beside BG, and shares nothing with it.

**Measurement.** Once, at BH's close, in `build-release` (`ck-tester`), per
`CLAUDE.md`'s Session Workflow step 3. It compares the commit BH opened at
against the commit that closes it, names both by `git describe --tags`, and
goes in `bench/v3.0.0/`. Every stage before the close lands with "overhead
not measured; measured at the milestone's close".

- **Overhead, interleaved A/B,** on the series' OLTP shapes. B must not
  regress.
  - A statement that is not a `PURGE` gains one text compare in the
    parser's head chain and one in `DispatchInner`.
  - A running `PURGE` holds the relation's `IX` and no row unit, so it
    blocks no DML. It delays a DDL on its relation for as long as it waits.
- **The cost of a purge:**
  - keys purged per second, one key per statement and 1,000 keys per
    window, at `cores = 1` and `cores = 2`;
  - the latency from a `DELETE`'s commit to a `PURGE` that succeeds, under
    the OLTP shape's concurrent readers, against `kPurgeHorizonWait`.

## 7. Row status

### BH-S0 — written 2026-10-08

- **Where:** on `worktree-bh-purge-key` from `0446b3b0`
  (`v2.7.0-691-g0446b3b0`), on W1-W3. `origin/main` was merged in at
  `274b92ef`, bringing BF-S1..S6. The survey stays at `0446b3b0`, and the
  BF and superblock statements were restated for the merge.
- **How §1 was made:** read, not run, from three read-only surveys and
  CLA's own reads.
- **What the reads settled:**
  - Both purge envelopes exist and are applied today. This is what makes
    BH-Q6 (a) possible.
  - The fault net's text calls a firing a fault, which is why BH-Q3 uses a
    constant of its own.
  - Drain mode covers a pass that is not a row's mutation, but drains only
    a fill made with no pin (BH-R6, BH-Q5).
  - K1's own reasons name the hazard BH-R5 gates (§0).
- **What stays open:** Census A and Census D, which are BH-S1's.
- **Recorded with it:** the words in `raft-marks-2026-10-08.md` §2-§4, and
  the index row.
- **No engine file moved, and no suite ran.**

### BH-S0's review - 2026-10-08

One `critics-developer` pass of three lenses — design correctness, survey
truth, and scope and house rules — read the order against `0446b3b0`. It
produced 13 findings, 4 simplifications and 5 house-rule findings. CLA
checked the evidence for findings 1, 2, 3, 9 and 11 against the code before
applying any of them.

**The two high findings, both applied:**

- **1. A `PURGE` blocked its own horizon gate.** `Begin` registers the
  transaction's view (`manager.cpp:162`; `command_dispatcher.cpp:8254-8256`),
  so a `PURGE` that waited for a `DELETE` to commit sat below that deleter
  forever.
  - BH-R5 now re-mints the view before every judging pass, and Census A
    lists the `PURGE`'s own view.
  - BH-R11 gains the commit-arm cell and the re-mint mutation.
- **2. Phase 2 could be refused after its first write.** `DescendTo` gives
  up with `TxnConflict` (`btree.cpp:228-234`), drain mode does not drain a
  fill made under a pin (`device_page_store.cpp:741`), and any fault can be
  an `IoError`.
  - PU6, BH-R6 and BH-Q5 now say each key is whole and a refusal can leave
    the window partly purged, with the count in its text.
  - BH-Q5 is marked as revising reply point 3.

**Design changes:**

- **The row and range units are dropped** (simplification S1, now
  BH-R4 and BH-Q16). The leaf latch and the phase-2 skip settle every race.
  This removes the fence cost and the hidden cycle (finding 5), and a
  verify mismatch is now a skip rather than `Corruption`.
- **`<>`, `IS [NOT] NULL` and the rest are refused by name**, since
  `DeclaredWriteBorrow` skips them and would widen the window (3). A
  negative literal is told apart from a miss.
- **The bound is `kPurgeHorizonWait`**, a constant of BH's own, and not
  the fault net, whose meaning is a lost wake (4). BH-Q3 is marked as
  revising reply point 2.
- **Census A also lists every spill-decoding site.** BH-R6 copies the
  spill references under the hold. BH-R7's retire-first reason is now the
  quiet-wrong double release, not idempotence (6).
- **PU2 names the equal-sort-keys exception** (7).
- **A live key is judged by the folded window**, so `BETWEEN 5 AND 5`
  refuses as `= 5` does (8).
- **Durability comes from the transaction's `TXN_COMMIT`** (9), so Census
  D keeps only the system-relation question.

**Survey corrections:**

- the writer's skip of a tombstone rests on the gate, not a lock, and any
  insert moves slots (10);
- the mount-time catalog sweep retires with a predicate of `true`, and
  `exec::LogSlotRetire` is a second `kNoTxnId` emitter (11);
- four texts added to §1.9 and the grep: `heap-and-tuple.md:132`,
  `sql.md:202-203`, `namespace.md:62` and `keystoneid-invariant.md:26`
  (12);
- K5 differs from `PURGE` in three ways, not one (13).

**Simplifications:**

- **S2 is moot under S1.** Phase 1 still re-judges, because without units
  a pending key can change.
- **S3 applied:** the helpers are named, and the primitive hands back the
  leaf and a payload copy.
- **S4 partly applied:**
  - The refusal table moved into PU10, and BH-R10 points to it.
  - The per-`Condition` byte capture is dropped for the AST's existing
    positions and the `WHERE` token.
  - §0's one-line rule list is **kept**, against the finding. It is the
    house format (BF §0), and the index row and the marks file cite it.

**House rules:**

- **H1 applied.** The header maps each point of the reply to its item and
  marks BH-Q10..Q16 new. Raft-marks §4 says the same.
- **H2 applied** by this section.
- **H3 applied** as §0's "What bounds BH", which BH-R12 carries to
  `known-gaps.md`.
- **H4 applied.** The heap's `Unsupported` rests on the mark rule, and the
  system-relation check runs first.
- **H5 is split.**
  - The `DispatchInner` route is added.
  - **Holding BH-S1's red cells as `DISABLED_` is declined.** BD-S1
    committed its red cells red at `4debe8d9`, and BH follows the house
    practice (§1.8). The suite gate counts them as the stage's stated red.

**Declined:** §0's rule list (S4), and `DISABLED_` for the red cells (H5),
each for the reason above.

### BH opened - 2026-10-08

- **Where:** on `worktree-bh-purge-key` at `37a7a9cd`, on the operator's
  standing go-ahead (`raft-marks-2026-10-08.md` §5).
- **BH-Q0..Q16 are adopted as §5 proposes**, not marked per item. Before
  adopting BH-Q1 and BH-Q16, CLA re-read their premises, which the header
  states. Neither proposal was revised.

### BH-S1 — the census, and red first - 2026-10-08

- **Where:** on `worktree-bh-purge-key` from `10593366`.
- **Census B, green, as expected.** It ran six cells in
  `tests/purge_key_test.cpp`, through a test-only seam (`RetireTombstone`)
  that retires a committed tombstone keyless under its leaf's exclusive
  hold. Each cell then names the key again with different values. Each
  consumer answers the new row once and never the old one:
  - **Index:** probes on the old value, on the new value, and over a range
    spanning both, plus a join probing through the index.
  - **Cabin:** a serve on the old value and on the new one.
  - **Foreign key:** the reverse check, through a stale Cabin entry. The
    parent the child left is freed, and the parent the child joined is
    kept.
  - **Inner build:** the join buckets the row under its new key.
  - **Assertions:** a group's `COUNT(*) <= 2` and its `SUM(qty) <= 10` each
    refuse the next row, exactly as a recount says. So the retire wrote no
    second departure (BH-R8's premise).
  - **Waystone:** a replayed trail lands where a fresh descent would. The
    cell is a pk lookup, since a trail never serves a search (invariant 9);
    its first draft filtered on a non-pk column and read no trail.
  - The index, Cabin, foreign-key, inner-build and Waystone cells each
    assert that their consumer ran: `index_scanned=`, a Cabin hit,
    `inner_built=1` or `replays=`. Without that, a cell could pass on a
    plain walk. The assertion cells need no such check, because the
    refusal is the consumer's own answer.
- **Census C, run** over `src/ include/ docs/spec/ docs/rules/ manual/
  CLAUDE.md` at `10593366`, by line and across line breaks.
  - 39 sentences become false once `PURGE` exists, and each is listed for
    BH-S5 with the text it should carry. Every other hit stays true: about
    issued ids, catalog ids, oids, group ids, pages, timers or persisted
    enum numbers.
  - **Not in §1.9, and added to BH-S5's list:**
    - `manual/sql/sql.md:541-542` (*"a deleted pk cannot be re-supplied"*);
    - `include/kds/storage/btree/btree.hpp:150-152`, which quotes the
      duplicate text;
    - `CLAUDE.md:73`'s *"stays bound for the relation's life"*, which the
      grep's phrase does not match.
  - **Added to §1.10:** `catalog.cpp:2078`'s *"what nothing purges"*, which
    the catalog's own `PurgeSettledDeleteMarks` (`catalog.cpp:1059`)
    already contradicts.
  - **Line drift since `0446b3b0`:**
    - `step_vm.cpp:1473` and `:1607` (were `:1439` and `:1574`);
    - `rows.hpp:857` (was `:832`);
    - `cabin_bound_page.hpp:31` (was `:30`);
    - `sql.md:604-605` ("Nothing purges."), `:1044` (the head list) and
      `:1017-1060` (§8).
  - **BH-R12's done-when grep is widened** by *"relation's life"*,
    *"nothing reclaims"*, *"lifetime of a relation"*, *"names a
    different"*, *"mean a different row"* and *"never reissue"*. It is run
    across line breaks, because `sql.md:604-605` and `cabin.md:104` break
    inside a phrase.
- **Census D, answered.** `DELETE FROM sys.tables` is not refused by
  design.
  - The name resolves, and `InitTableAccess` answers `NotFound` *"no
    columns for this rel_id"* (`catalog.cpp:2199`), because a bootstrap
    relation has no `sys.columns` rows.
  - There is nothing deliberate for PU10 to follow, so **BH-Q17** is
    written and adopted: an explicit `IsSystemNamespace` check, refused
    `Unsupported` at the name.
  - `CensusDADeleteOfASystemRelationIsRefusedOnlyByAccident` pins the
    accident.
- **Red, committed red:**
  - `PurgeKeyTest.APurgedKeyIsNamedAgainAndPlaced`;
  - `PurgeWaitTest.AnOlderRepeatableReadReaderKeepsItsRowAndThePurgeIsRefusedAtTheBound`;
  - `PurgeWaitTest.AnUndecidedDeleteThatCommitsWhileAPurgeWaitsIsPurged`
    (the commit arm).

  Each answers *"ERR unknown command"* at `10593366`.
- **Guard, green:** `SortedLeafSqlTest.ADeletedKeyNamedAgainIsAlreadyExists`.
- **The baseline suite at `10593366`** passed 3274 of 3275 cells. The one
  failure is
  `TcpServerListenTest.ReusePortAdmitsASecondListenerAndItsAbsenceRefusesOne`:
  its fixed port 25432 is held by a `kds_server` that this session did not
  start. The failure is environmental and is repeated on every run below.
- **Census A, run** (read at `10593366`, by a read-only survey that CLA
  checked against the code before acting on it). It rests on how
  `ResolvedForEveryReader` reads a view (`manager.cpp:807-833`):
  - **A future registered mint is covered.** `MintReadView` lowers its
    core's slot before it reads the ceiling, and the gate reads
    `PendingCommitBound`.
  - **An unregistered view is not covered.** So each kind of unregistered
    view was shown safe on its own argument.

  Eleven view kinds and seven spill-decoding sites. **One row was open, and
  it is closed at this stage:**
  - **Row 5: a write parked mid-walk inside a READ COMMITTED transaction,
    then resumed.**
    - The resume is a re-dispatch. `DispatchInner` clears
      `statement_boundary_taken_` (`command_dispatcher.cpp:1046`).
    - With DDL open on the core, `ViewFor` → `EnsureStatementBoundary`
      re-mints the transaction's registered view (`:6063-6082`). The walk
      meanwhile goes on reading at the parked snapshot.
    - The horizon then no longer sees that snapshot, so a `PURGE` on another
      core could retire a row the walk is entitled to. The same gap already
      lets undo recycling pass under the walk's snapshot, since that is
      bounded by the same horizon. So this was a latent defect before BH,
      **fixed in the session that found it, so it gets no bug entry**.
    - **Fix:** both resume branches mark the boundary as taken, because the
      resume is the same statement.
    - **Cell:** `MidWalkWaitTest.AResumedWriteKeepsTheReadCommittedViewItParkedUnder`,
      over both `UPDATE` and `DELETE`. It was **killed with the fix
      removed**: both arms red, then the file was restored from a copy.
  - **Safe, each on its own argument:**
    - Registered, so the gate sees them: explicit transactions, the
      autocommit `SELECT` lease, and the `PURGE`'s own view, provided
      BH-S3 re-mints it before each judging pass.
    - Covered by the owned transaction's earlier `Begin`, which stays in
      `live_`: the autocommit `UPDATE`/`DELETE` copy of `snap`. This is
      **fragile**: it rests on `BeginWrite` running before `SnapshotFor`.
      BH-S3 pins that ordering with a comment at both sites.
    - Read only the header, or only `kLive` rows: the latest-state check
      views (foreign keys, the Cabin build, the assertion build, the
      catalog).
    - No manager, so no `PURGE`: `ReadView::Everything()`.
    - Excluded by the relation's `X`: `CREATE INDEX`'s backfill, which takes
      no view and resolves tombstone spills after releasing the leaf.
  - **What BH-S3 inherits.** The `PURGE` must hold its relation's
    `IS`/`IX` **through its last spill release**, because the backfill
    and the assertion build are safe only because their `X` excludes it.
  - **Every spill-decoding site of the step VM classifies first and reads
    under the leaf's hold**: walk, point, index, Cabin serve, Waystone
    replay and inner build. A purge's release follows its retire, which
    needs the leaf exclusive. So no site reads a released spill.

### BH-S1's review - 2026-10-08

One `critics-developer` pass over BH-S1's diff at `10593366`.

**Correctness: four findings, all applied by the reviewer and re-run green
by CLA.**

1. **The Waystone cell never read a trail.** It filtered on a non-pk
   column, and a trail serves only a lookup or a probe (invariant 9,
   `IsTrailReplayable`). It also ran the query twice, where a replay needs
   a third run. It is now a pk lookup, run three times, with `replays=`
   asserted before the retire and the trail asserted consulted after.
2. **Every other Census B cell could pass on a plain walk.** Each now
   asserts its consumer ran:
   - `index_scanned=` for the index probe;
   - the Cabin store's `hits` for the serve and for the foreign-key reverse
     check;
   - `inner_built=1` for the inner build.
3. **Refusals asserted only an `ERR` prefix.** They now assert
   `FK_VIOLATION` and `ASSERTION_VIOLATION`.
4. **`kPastTheBoundNs` was also just past the lock family's 1 s fault
   net**, so a `PURGE` bounded by the net, which BH-Q3 (c) rejects, would
   have passed unseen. `PurgeWaitTest` raises the net tenfold.

**Checked and sound:** the resume fix and its cell, the seam, the red
cells against PU1-PU12 and BH-R11, and a spot-check of §7's citations.
One citation drift was corrected.

**Simplifications:**
- **Applied:** the two assertion cells are merged into one over
  `COUNT(*) <= 2` and `SUM(qty) <= 10`.
- **Taken at BH-S3:**
  - once `PURGE` exists, Census B's cells go through the statement and the
    seam is deleted, leaving one cell set over the real writer;
  - `kPastTheBoundNs` becomes `kPurgeHorizonWait + 1`.
- **Rejected:**
  - **De-duplicating the test-local `Ids` and `Placed` helpers.** They are
    file-local, as in `sorted_leaf_named_keys_test.cpp`.
  - **`PurgeWaitTest` deriving from `LockDeadlockTest`.** That is
    `MidWalkWaitTest`'s convention.

### BH-S2 — the primitive - 2026-10-08

- **Where:** on `worktree-bh-purge-key` from `80a0c223`.
- **Code:**
  - `btree::BtreeHoldTombstone` finds and verifies a key's slot under
    the leaf's exclusive hold. It hands back `std::nullopt`, a skip, for an
    absent key, a live slot, or a slot another deleter stamped.
  - `exec::PurgeKey`, in this order:
    - decodes the version's spills off the held page;
    - retires the slot and logs `SLOT_RETIRE` at `kNoTxnId` under that
      hold;
    - releases the hold;
    - releases each spill with `VARHEAP_RELEASE` at `kNoTxnId`.
  - `exec::RowSpills` is factored out of the mount sweep's
    `ReferencedSpills`, so the sweep and the purge share one decoder.
  - The log-cut helpers move from `sorted_leaf_crash_test.cpp` to
    `file_rig_crash.hpp`, so this stage's crash cells use them rather than
    a copy.
  - No caller from SQL yet.
- **Cells** in `tests/purge_key_crash_test.cpp`, on the file rig, all green:
  - a purged key is named again and placed, and its spill released;
  - each skip: live, another deleter, never placed, and re-placed after a
    purge;
  - the records are `SLOT_RETIRE` then `VARHEAP_RELEASE`, both at
    `kNoTxnId`;
  - a crash at every record boundary mounts with the key free, and the
    image before the purge mounts with it bound;
  - a restart after a recovered purge recovers it the same way;
  - a loser `INSERT` of the purged key is undone at mount.

  `RedoTest.APurgesSlotRetireReplaysOnceAndTwiceIsANoOp` covers **"redo
  applied twice"**. A mount cannot reach the appliers' already-applied
  arms: redo skips by page LSN first, and the retire is stamped under the
  hold that wrote it. **Not a cell here: an undecided deleter.** The
  primitive checks only the slot's identity, and the judging is BH-S3's.
- **Mutations, each run twice, each killed:**

  | mutation | killed by |
  |---|---|
  | the skip turned into a retire | `ASlotThatIsNotTheJudgedTombstoneIsSkipped` |
  | the release before the retire | the record-order cell |
  | the release omitted | the spill cell and the record-order cell |
  | the envelope given the deleter's id | the record-order cell |

  Each was applied to a file copy, built and run, and the file was
  restored from its copy.
- **The suite:** the full Debug suite ran on this stage's uncommitted code
  beside the BH-S1 tree later committed as `80a0c223`, 3288/3292: the three BH-S1 red
  cells and the environmental `TcpServerListenTest`. After the review's
  refactor (below), the stage's cells and `SortedLeafCrashTest` were re-run
  green, and BH-S3's full run covers the rest. Overhead not measured;
  measured at the milestone's close.

### BH-S2's review - 2026-10-08

One `critics-developer` pass. **No correctness defect.** It checked against
the code:

- that a write lookup returns a delete-marked slot;
- that a tombstone's `trx_id` is its deleter's;
- that the retire/append/stamp order is the house order under one hold,
  and that BC-S4's fail-stop keeps an unlogged retire from being written
  back;
- that no two versions or rows share a spill, because an `UPDATE`
  re-encodes the row;
- that the release takes its own hold;
- that redo's applier and every leaf invariant already handle a retired
  slot.

**Applied:**

1. **The mount-twice cell claimed to reach the appliers' already-applied
   arms**, and it cannot. It is renamed to what it pins, a restart after a
   recovered purge. The redo-level cell is added for the claim.
2. **The order said the primitive retires.** The code's split is better,
   because decoding before any write leaves the page untouched on a
   refusal. BH-R7's text and this stage's row now say what the code does.
3. **`HeldTombstone` duplicated `Location`** with a payload copy nothing
   needed. The struct is deleted, and `PurgeKey` reads the tuple off the
   held leaf.
4. **The record-order cell re-read the segment file.** `Segment` now
   keeps each record's `txn_id`.
5. **The cells repeated their setup five times.** It is now one `Load`
   helper.
6. **The moved helpers were wrapped by hand**, since clang-format is not
   installed.

**Noted, not changed:** `BtreeHoldTombstone` reads any `NotFound` as a
skip, including a missing root. That is unreachable while the relation's
`IX` and BF's statement epoch keep a drop out.
