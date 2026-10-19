# Work order CD — cleaning dropped columns: clean in place, then reuse their slots

Written 2026-10-19 on `worktree-cd-column-slot-cleaning` from `f3ec25ea`
(`v3.3.0`), on the operator's word, recorded in English
(`raft-marks-2026-10-19.md` §2-§3):

- **W0**: *"Close that milestone and plan the BW cleaning work milestone."*
  BW was already closed (2026-10-14, again with BW-S6 at `3b5daa80`), so
  nothing was left to close; CD plans BW's D-e - the operator's words at
  BW's W0: *"cleaning (resetting the flag, moving and resizing tuples) may
  come later."*

- **W1**: *"OI-1..14 $!a"* with `/go-ahead-achieving-milestone` - every
  CD-OI item **marked (a) by the operator**, and CD opened on the standing
  go-ahead (`raft-marks-2026-10-19.md` §3). OI-14 (a) places this order in
  `instructions/v3.2.0/` beside CA-CC, against CLA's revised (b).

**Status: opened 2026-10-19.** Items that surface mid-stage are adopted on
the operator's standing go-ahead of 2026-10-19 (go-ahead-achieving-milestone)
and recorded as such.

**How the plan was reached.** A four-lens survey of the tree at `f3ec25ea`
(the BW layout and catalog state; every path that rewrites rows; every
consumer of a column position or the row size; the requirements and BW's
measurements), three independent designs (minimal slot reuse; an explicit
`REBUILD` into a new clustered tree; online in-place cleaning), three judges
(correctness, MVCC, WAL and crash; value to the operator and OLTP;
buildability), and one synthesis that grafted the judges' ideas and
re-read the tree to reject two of them. The judges scored online in-place
cleaning highest (7.5 / 6.5 / 7.5) and the rebuild lowest (5.5 / 5.5 /
3.5): it needs an invariant-13 ruling, a new transactional root-swap record
and an anchor format change.

**Claim tags**: `[source-read]` with `path:line` at `f3ec25ea`; `[design]`
for code not yet written; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error.

**The letter.** `[source-read]` `origin/main` at `f3ec25ea` names no CD
(CA, CB and CC are the last, under `instructions/v3.2.0/`). CLA's choice.

---

## 0. Overview

The design starts from Design 3 (the best score, 7.5/6.5/7.5). It cleans in place under PURGE's driver and a per-row gate, then grafts in the judges' best ideas. I also re-read the tree and corrected two of the judges' grafts:

(1) The reusing ADD keeps the dropped row's column oid. It must not take a fresh one. A sys.columns row's first 8 bytes are its oid (`SysColumnRow::kOidOffset = 0`, include/kds/catalog/rows.hpp:263), and they are its Keystone word (`KeystoneIdOfPayload` reads the first 8 bytes, src/storage/keystone.cpp:44-49). A rollback compensation refuses to write unless the slot still carries the prior image's key (src/txn/manager.cpp:440-475, and recovery's `TupleKeyAt` at src/txn/recovery_undo.cpp:27-34). An in-place overwrite that changed the oid would therefore make ROLLBACK of a reusing ADD fail with Corruption. It would also rebind a catalog row's Keystone id, against invariant 11. The fence the judges wanted from a fresh oid moves to a new `generation` field (OI-5).

(2) DROP COLUMN does not need to record a commit word for the statement-epoch gate. The pass can take the schema word it resolved its TableAccess at, behind its IX, and wait in the PurgeWait loop until `AllAtLeast` that word holds (include/kds/server/statement_epoch.hpp:107-114). Its preconditions become S0 premise P4.

What CD does, on `f3ec25ea`:
- S1: EncodeRow sets a dropped column's null bit. Today it leaves the bit clear (src/exec/row_codec.cpp:967-971).
- S2: an autocommit statement (proposed spelling `PURGE DROPPED COLUMNS FROM t`) walks the relation under its IX. For each row whose dropped cells are not yet clean and whose head writer is `ResolvedForEveryReader` (include/kds/txn/manager.hpp:721), it zeroes those cells and sets their bits in place. The write is logged as a HEAP_OVERWRITE with envelope kNoTxnId, keeping the header's writer, as `OverwriteLogged` already does for catalog rows (src/catalog/catalog.cpp:276-284). The pass then releases the spills those cells named, as PurgeKey does (src/exec/purge_key.cpp:26-58).
- S3: once an attempt finds every target clean, the statement sets the flag `kColumnSlotClean`.
- S4: a later ADD COLUMN with no vacant slot repurposes a clean dropped in-slot row at the same pos with one in-place `OverwriteForDdl` (the write DropColumn uses, src/catalog/catalog.cpp:2018-2024). The RowLayout rule that the j-th in-slot column in pos order takes slot j (src/catalog/row_layout.cpp:94-135) then gives the new column the same slot, offset and null bit.

What CD does not change: row size never changes (invariant 13 untouched), no position is renumbered, no record kind is added, and no relation X is held across the O(n) pass. The only format event is superblock 31->32 for the flag bit and the `generation` field.

Scope (OI-1; the judges asked that it be stated up front): `kColumnInSlot` is set only by AddColumn (src/catalog/catalog.cpp:1934). So only a slot freed by a column that was ADDed and then DROPped becomes reusable. A relation created with the default SPARE 0, and a dropped declared column, gain no ADD capacity from CD. They gain only the release of their dropped varchar spills (OI-8). The general lift, resizing and `REBUILD SPARE n`, is a later milestone. It has Design 2's shape and needs the invariant-13 ruling.

Preconditions:
- Fetch origin/main before taking the letter CD (memory milestone-letter-collision). On `f3ec25ea` no CD work order exists under instructions/ (CA, CB and CC are in instructions/v3.2.0/).
- OI-1, OI-3, OI-4 and OI-5 must be ruled before S2.

## 1. User surface

1. **One new admin statement**, autocommit only. The spelling is OI-2; the proposal is `PURGE DROPPED COLUMNS FROM <t>`. The new words are contextual: keywords hash as identifiers, so nothing is reserved.
   - Inside an explicit transaction it is refused NotImplemented at byte 0, PURGE's shape (src/server/command_dispatcher.cpp:10620-10623).
   - It is admitted on btrees, on heaps (SPARE with HEAP has been admitted since BZ, tests/heap_hidden_key_test.cpp:150, and the pass never touches pos 0), on Transition-governed relations and on FK children. It retires no key, so PURGE's heap and Transition refusals (:10647-10665) do not apply.
   - Success reply: `PURGED DROPPED COLUMNS <t>: <k> column(s) clean, <n> row(s) cleaned, <s> spill(s) released`.
   - If a dirty row's writer is still not resolved for every reader after the 1 s bound (OI-7), or an older statement still holds a pre-DROP word, it is refused TxnConflict. The refusal names the key (or "an older statement"), the count still blocked, and "running it again finishes it". A re-run is idempotent: clean rows are only read.
   - It is refused NotImplemented, naming the table token's byte, on a relation with no dropped column.
2. **ALTER TABLE ... ADD COLUMN** behaves as today while a vacant slot is free (catalog.cpp:1911-1933). When none is free, it takes the lowest-pos dropped in-slot column flagged clean, at that pos (OI-12).
   - If no such column exists, the NotImplemented message at catalog.cpp:1925-1931 gains the remedy when dropped slots exist: "N slot(s) are held by dropped columns; PURGE DROPPED COLUMNS FROM t frees them".
   - Older rows read the reused column as NULL, as BW-R3 guarantees for a vacant slot.
3. **sys.tables `spare_free`** (src/server/sys_views.cpp:288-293) counts vacant slots plus clean dropped slots. The extra `spare_dropped` column from Design 3 is cut.
4. **Unchanged:**
   - DESCRIBE and SELECT *.
   - Positional INSERT arity, and the BW-Q9 refusal of the short form, which stays on for good: `slots_held` never decreases, schema.hpp:214.
   - Row size.
   - A KWP load described before a DROP and a reusing ADD is refused at its next chunk, now by digest (S4).
5. **Documents:**
   - manual/sql/sql.md :397-404 and :1600-1604.
   - alter.md AL11 (:284-300).
   - known-gaps.md :1203-1207 and :1213-1217.
   - txn.md :840-852: the "no DDL moves data" premise is amended to name the clean pass and its epoch gate.
   - The stale BW-R11 text ("SPARE with HEAP is unreachable", workorder-bw:241-243).
   - The stale citation at workorder-bw:700.
   - The CLAUDE.md ALTER TABLE row.

## 2. Mechanism

All on `worktree-cd-column-slot-cleaning` at `f3ec25ea`.

**S1, the codec.** In EncodeRow's dropped-column skip (src/exec/row_codec.cpp:967-971), set the column's null bit when `layout.null_bits[i] != kNoNullBit`. The cell is already zero from the zero-fill at :956.
- The vacant-suffix model (schema.hpp:178-185; row_layout.cpp:139-145) stays, because a dropped column and a reused one both stay in_slot at their pos. Design 1's replacement bit list is cut.
- EncodeAssigned (row_codec.cpp:987-1020) does not change: it copies the before-image, so a clean row stays clean.
- The decoders do not change: they never read a dropped cell (row_codec.cpp:1165-1176).
- In-slot columns are always nullable (the Corruption check at row_layout.cpp:122-128), so every dropped slot has a bit. A dropped declared NOT NULL column has none, and its zero cell is a kNull tag, which no decoder interprets.

**S2, the pass.** A new src/exec/clean_dropped.{hpp,cpp}, modelled on src/exec/purge_key.cpp:15-58. Its driver is a handler modelled on HandlePurge (src/server/command_dispatcher.cpp:10615-10800). Steps:
1. The whole statement runs inside `InDdlStatement`, so S3's flag and Cabin writes share one DDL transaction. Its scope takes the relation IX through BorrowOrWait, as PURGE does at :10707-10710.
2. Behind the IX, the TableAccess is resolved. Targets are its dropped columns: all of them, or in-slot only, per OI-8. W is the schema word that TableAccess was resolved at. If the word moved between the head and the IX, the statement re-runs from the top, so targets and W always agree.
3. Epoch gate: if `!epochs->AllAtLeast(W)` (statement_epoch.hpp:107-114, the BF-R9 predicate at src/server/page_reclaim.cpp:134-141), return a new PurgeWait variant, `epoch=W`. The existing loop (command_dispatcher.cpp:796-815) co_awaits it with the statement's own slot idle and re-runs. Bound: kPurgeHorizonWaitNs (include/kds/exec/purge_key.hpp:49). This keeps out every reader that bound a pre-DROP layout: one refused its IS during the DROP's X reads on (docs/spec/txn.md:840-852).
4. Phase 1 is a kRead walk with BtreeVisitFrom from the leftmost leaf (btree.hpp:269-272). Each keyed slot, live or delete-marked, is `clean` when every target cell is all zero and its bit, where it has one, is set. Dirty slots are queued, one leaf window at a time. Retired slots are skipped (NotFound, as at :10731-10735).
5. Phase 2, per dirty key: `BtreeLookup(..., kWrite)` (btree.hpp:220-221), re-read under the exclusive hold, and act:
   - clean now: skip;
   - else, if the header's trx_id is not `ResolvedForEveryReader`: note it (the first such key and a count) and skip;
   - else: collect the targets' spill refs (RowSpills restricted to the targets, src/exec/catalog_spills.cpp:14-37); zero the cells and set their bits on a copy; `OverwriteLogged` with env_trx = kNoTxnId, hdr_trx = the header's trx_id, hdr_undo = its undo_ptr (catalog.cpp:276-284, a new exec-side caller of the same shape); release the leaf; then `txn::ReleaseVarHeapSlot(store, wal, kNoTxnId, ...)` for each distinct ref (purge_key.cpp:37-56).
6. After the walk, if a key was noted, return PurgeWait{key, trx, horizon=true}. The loop waits, bounded, then re-runs; otherwise it refuses TxnConflict. The writes never span a co_await: every one is under one hold, and each wait sits between whole attempts.

**S3, flag and format.**
- `SysColumnRow::kColumnSlotClean = 0x08` sits beside 0x01, 0x02 and 0x04 (rows.hpp:255-258). It means: every version any live or future reader can reach holds this cell zero with its bit set where one exists, and no spill it named is live.
- A `std::uint32_t generation` is appended after flags (OI-5). kOnDiskSize goes 95 -> 99, and the pin at rows.hpp:284 is restated.
- Decode (src/catalog/rows.cpp:103-125) refuses 0x08 without 0x01 as Corruption.
- kSuperBlockVersion goes 31 -> 32 (include/kds/server/superblock.hpp:195), refusing 31 with no migration (standing order at :155).
- On an attempt whose phase 1 finds every target clean and whose epoch gate passed, the same DDL transaction does three things:
  (a) drops each column Cabin whose `column_no` is a target pos, through the catalog's Cabin drop and the controller's state. These are the auto-create race leftovers (known-gaps.md:1213-1217). Which function DROP CABIN calls is not verified here; S3 names it.
  (b) ORs 0x08 into each target row with OverwriteForDdl, under the sys.columns root-page hold (CT7, the DropColumn shape at catalog.cpp:1972-2024). A target that is no longer dropped or no longer at its pos is skipped.
  (c) The commit moves the schema word (MoveSchemaWordIfCatalogWriter at the decide, src/txn/manager.cpp:329, before the X is released at :336; CD-S0 P4).

**S4, reuse.**
- In Catalog::AddColumn (catalog.cpp:1911-1972), when `held >= spare_slots` and the lowest-pos row has `dropped() && in_slot() && (flags & kColumnSlotClean)`, overwrite that row in place under the hold the function already takes. The new image keeps oid, rel_id and pos, and sets: the new name, type_val, len and cabin_policy; notnull = false; flags = kColumnInSlot; generation + 1. The write goes through OverwriteForDdl, so ROLLBACK and mount undo restore the dropped, clean row with an unchanged Keystone word.
- The function returns a CatalogRowChange, as DropColumn does, in place of the inserted CatalogRowRef. HandleAddColumn (command_dispatcher.cpp:2682-2735) notes it.
- Before the write, under the relation X, HandleAddColumn asks a new `PositionDependant(oid, pos)`. It is factored out of DropColumnRestriction (command_dispatcher.cpp:2582-2645) and keeps the index, fkey, Transition, column-Cabin and Cabin-key-read arms. It leaves out the whole-relation assertion arm, because an assertion resolves names and a dropped pos has none. Any hit refuses the ADD, naming the dependant and the remedy (re-run the clean statement).
- The width check (catalog.cpp:1937-1944) and RowLayout::Build run unchanged.
- DescribedShape (include/kds/parser/ast.hpp:489-493) gains a `column_set_digest` over (pos, oid, generation, flags) of every sys.columns row of the relation. It is recorded at src/server/kwp_load_server.cpp:419-421 and compared beside the counts at command_dispatcher.cpp:5379-5388.
- The relation's sys.access_stats rows whose column_mask names the reused bit are deleted (OI-13). spare_free counts the clean slots.

**S5.**
- sim/workload.cpp gains `add-column`, `drop-column` and `purge-dropped` ops (the op table at :23-39 has none).
- sim/integrity.cpp (:240-244, :311-320) checks two things: every head's flagged dropped cell is zero with its bit set; and the oracle reads NULL for a reused column in every row that predates it, across crashes.

## 3. MVCC, WAL and crashes

Claim, on `cd-column-slot-cleaning` at `f3ec25ea`: when the flag is written, every row version that any live or future reader can reach holds each target cell zero with its bit set, and no spill it named is still live.

1. **Per-row gate.** Suppose the head's writer passes `ResolvedForEveryReader` (manager.hpp:713-721): every live and future view sees it as committed. Then every view takes the head and never walks the head's undo chain. Every older version, dirty or not, is unreachable. Cleaning the head in place, with no new version, its writer and undo_ptr kept, changes only bytes no decoder reads (row_codec.cpp:1165-1176). A delete-marked head is cleaned by the same rule, because its trx_id is the deleter's.

2. **Serialisation with writers, without a row lock.**
   - UPDATE copies `payload_copy` and writes the new version and its before-image under one live write PageRef, with no co_await in between (command_dispatcher.cpp:8833, the "no re-read after the grant" note at :8884-8887, :9128-9134 and the same-page re-fetch at :9156-9159).
   - DELETE re-reads and re-stamps under its hold (:11050-11063).
   - The pass's per-key exclusive hold therefore lands either before such a write, so the writer copies a clean image, or after it, so the head's writer is unresolved and the key is retried.
   - A rollback compensation (manager.cpp:516-534) or recovery undo (recovery_undo.cpp:233) restores a before-image only for an undecided writer. The pass never cleaned that writer's row. Its before-image is clean, if the writer copied a cleaned row, or the row is still dirty and will be retried.
   - An INSERT after S1 is clean.
   - Writer paths are enumerated by S0 premise P3 (every OverwriteTuple and EncodeRow caller on `f3ec25ea`: command_dispatcher.cpp:5794, :9159, :11062; manager.cpp:519; recovery_undo.cpp:233; catalog.cpp:282; declaration_relation.cpp:214).
   - Consequence: the set of dirty heads with unresolved writers only shrinks while the pass runs, so re-runs converge.

3. **Stale layouts.** A reader whose IS was refused during the DROP's X can hold the pre-DROP layout, in which the column is live (txn.md:840-852). It would read a zeroed cell as NULL, or follow a released spill. The epoch gate (`AllAtLeast(W)`, W taken at or above the DROP's resolve bump) waits until no such statement runs, the BF-R9 argument (statement_epoch.hpp:1-20).

4. **Spills.** A spill is released only after the clean is logged and the leaf hold is released, and only under the gate in point 1. After that, no reachable version names it, which is BN-R3's rule (heap-and-tuple.md:87) and PurgeKey's licence (purge_key.cpp:26-58). A later PURGE of the row finds the cell zero (RowSpills reads a zero tag, catalog_spills.cpp:27-33), so nothing is released twice. S0 premise P2 checks that no spill pointer is shared across rows or across the cells of one version.

5. **WAL.**
   - Each clean is HEAP_OVERWRITE with envelope kNoTxnId and the header's writer in the payload. Analysis does not track kNoTxnId (src/wal/analysis.cpp:52-55, :84), so no phantom loser appears. Redo applies the payload's header fields (src/wal/redo.cpp:105-120). A HEAP_OVERWRITE on a btree leaf is already UPDATE's record.
   - Each release is VARHEAP_RELEASE at kNoTxnId.
   - The flag, the Cabin sweep and the reuse are logged catalog writes under DDL transactions (catalog.cpp:372-405).
   - No record kind is added. A restore replays all of it from the backup's start (backup.md:26-31).

6. **Crash cases.**
   - Mid-walk: the cleaned rows replay clean, no flag is set, and a re-run finishes. Between a clean and its release, spills leak; this is BH-R7's class, a leak and never a double release.
   - After the flag: every clean record precedes the flag's record in the log, so reuse never relies on an unlogged clean.
   - A rolled-back reusing ADD: compensation restores the dropped, clean image. The Keystone word is unchanged, so the identity check at manager.cpp:440-475 passes.

7. **Readers of an open reusing ADD** (BW-Q8, known-gaps.md:1208-1212) see the new column reading NULL in every row, which is true.

8. **Downgrade.** A v31 binary would write dropped cells with the bit clear into a slot flagged clean, and a later reuse would read a present 0 or a kNull tag. The 31->32 bump makes the `!=` version check refuse that mount.

## 4. Consumers

Every consumer of a column position or the row size, and what CD does to it, on `cd-column-slot-cleaning` at `f3ec25ea`.

**No position moves and the row size never changes.** So the following are untouched:
- sys.indexes key_cols and covered_cols (rows.hpp:483-484), and index entries, which hold no location (index_page.hpp:44, :107);
- sys.fkeys child_column_no (rows.hpp:934);
- sys.cabins column_no, and the sys.cabin_exprs text and expr_id (rows.hpp:820; include/kds/exec/cabin_expr.hpp:30-40);
- Transition aux column_no (src/exec/transition_catalog.cpp:17, :59);
- the 64-bit cabin_mask and index_mask (catalog.cpp:2641, :2716);
- the decoder's column mask (row_codec.cpp:1105-1119).

**Position count.** A reusing ADD adds no position, so a relation's positions stay at most declared + spare_slots for life, whichever slot ADD prefers (OI-12).

**Dependants of a reused pos.**
- At the flag write, column Cabins on a target pos are swept (S3a).
- At the reuse, `PositionDependant` refuses any index, fkey, Transition, Cabin or Cabin-key read that names the pos. This closes the dangling auto-Cabin case that BW calls harmless only because a dropped column cannot be named (known-gaps.md:1213-1217).
- CreateCabin still refuses a dropped pos at catalog.cpp:3277-3283.

**Assertions.** No position is persisted, and names are re-resolved (src/exec/assertion_catalog.cpp:102-118). A dropped pos has no name. Whether CREATE ASSERTION is admitted on a relation with a dropped column is S0 premise P5.

**Waystone.** The fingerprint is FNV over the statement's tokens (src/parser/fingerprint.cpp:9-30), so it holds names, not positions. Trails map to (page, epoch), and rows do not move. It is advisory (invariant 8), so nothing is needed.

**Plan statistics (CC).** They are keyed by rel oid and by FROM-list "written positions", not column positions (include/kds/stats/plan_key.hpp:19). LeafRowCapacity(row_size) is unchanged. Nothing is needed.

**sys.access_stats column_mask** (rows.hpp:707-715). The rows naming the reused bit are deleted at reuse (OI-13). They are diagnostic and unlogged.

**KWP load (BW-Q10).** The (physical, live) counts (ast.hpp:489-493; command_dispatcher.cpp:5379-5388) return to their old values after DROP, clean and reuse, so the counts are joined by a (pos, oid, generation, flags) digest (OI-5).

**Positional INSERT guard.** `column_set_changed()` (schema.hpp:214) stays true, because slots_held counts the reused row.

**sys.tables spare_free** (sys_views.cpp:288-293). It counts clean slots.

**RENAME COLUMN.** It refuses a dropped row (catalog.cpp:1874-1880) and writes under the same CT7 hold, so it cannot race the flag or the reuse.

**Cabin controller.** It is not fenced by the relation lock (known-gaps.md:1213-1217, 1230-1235). It is handled by the sweep plus the reuse-time refusal.

**Backup and restore.** Everything CD writes is logged. Nothing is freed except var-heap slots, and those under the per-row gate.

**Sim harness.** It gains ALTER and clean operations in S5.

**Contract suites** (waystone, index, cabin, inner-build). Each gains a configuration across DROP, clean and reusing ADD, compared byte for byte.

## 5. Cost and locks

On `cd-column-slot-cleaning` at `f3ec25ea`.

**Pass.**
- Locks: the relation IX for the statement, inside an autocommit DDL transaction. One exclusive leaf hold per dirty-key clean. No row unit and no relation X.
- Concurrent readers (IS) and writers (IX) proceed. X-taking DDL waits and is refused TxnConflict at the 1 s fault net while the pass runs: ADD/DROP COLUMN, CREATE/DROP INDEX, DROP TABLE, CREATE ASSERTION. This is PURGE's exposure today.
- Cost: an O(rows) read walk, plus, per dirty row, one HEAP_OVERWRITE of the header, 20 bytes of fields and row_size (payload.hpp:140-152), plus one VARHEAP_RELEASE per spill. There is no undo and no version. A re-run on clean data only reads.
- It does not yield (OI-11; only a SELECT's outermost walk yields, include/kds/exec/budget.hpp:9-19). A large relation therefore holds its core for the walk, and the close measures that stall.

**Waits.** These are the epoch gate and the horizon retry. Each is parked in the PurgeWait loop (command_dispatcher.cpp:796-815), bounded by kPurgeHorizonWaitNs (1 s, purge_key.hpp:49), and adds no new setting.

**Flag write.** O(target columns) catalog overwrites, plus O(dangling Cabins) drops, under the CT7 hold, inside the same DDL transaction under the IX (OI-9).

**Reusing ADD.** Today's ADD: relation X, one sys.columns overwrite instead of an insert, a schema-word bump. It touches no row.

**Steady state.**
- INSERT pays one extra null-bit OR per dropped column with a bit, behind the existing hoisted `has_dropped` gate (row_codec.cpp:963-967). A relation with no dropped column pays nothing.
- UPDATE, SELECT and decode are unchanged.
- A KWP chunk compares one more u64. The digest is computed per TableAccess fill or memoised beside the layout, never per row.

**Space.** No second tree and no new pages. Var-heap slots are freed. The extra 4 bytes per sys.columns row are catalog-only.

## 6. Invariants

- 13 (fixed-length tuple): untouched. The row size, offsets and bitmap never change, and a reused column takes the same slot, offset and bit (row_layout.cpp:94-145).
- 11 (tuple key never rebound): untouched for user rows. The pass never writes the Keystone word, and tombstones are cleaned, never retired or removed. For sys.columns, the reuse keeps the row's oid, which is its Keystone word (rows.hpp:263; keystone.cpp:44-49). A fresh-oid overwrite would rebind it, and is rejected.
- 12 (20-byte MVCC header, no xmax): untouched. The clean keeps trx_id and undo_ptr, and the record carries them unchanged.
- 14 (var-heap immutable per version): respected. A value is released only when no reachable version names it (the per-row gate, BN-R3, heap-and-tuple.md:87), after its clean is logged and outside the leaf hold.
- 8/9 (Waystone advisory): untouched. No row moves, and the fingerprint hashes tokens.
- WAL (every data mutation logged): HEAP_OVERWRITE at kNoTxnId, VARHEAP_RELEASE at kNoTxnId, and logged catalog overwrites and Cabin drops. No new record kind.
- 6 (explicit shift/mask): kColumnSlotClean is a named mask in the existing flags byte, and `generation` is an explicit offset with static_asserts.
- null.md: the bitmap is the sole authority on NULL. Clean is defined as the bit set over a zeroed cell.
- BW-R6 amended: 'a dropped slot is never reused' becomes 'reused only once flagged clean, at its own pos'. BW-R3 extended: a dropped cell with a bit is written with its bit set. BW-R11 corrected (stale since BZ).
- txn.md:840-852 'no DDL moves data' amended: the clean pass rewrites dropped bytes in place, and the statement-epoch gate is what keeps a stale-layout reader from reading them.
- Superblock 31 -> 32 under the standing no-migration order (superblock.hpp:155, :195). sys.columns kOnDiskSize 95 -> 99 is a format event inside the same bump (rows.hpp:284).

## 7. `[quiet-wrong]` risks

- [quiet-wrong] The flag is written while a reachable version is still dirty, so a reused column reads the dropped value or a present 0. Defences: the per-row ResolvedForEveryReader gate; the flag only on an attempt whose walk finds every target clean; S1 so new inserts are clean; the writer enumeration in S0 premise P3. S2 and S3 tests hold an RR snapshot open across DROP, the pass and the ADD, force the interleavings with a barrier and repeated rounds, and kill the mutants that remove the gate or flag early (memory mutation-testing-concurrency).
- [quiet-wrong] A reader that bound the pre-DROP layout (its IS refused during the DROP's X) decodes a zeroed cell as NULL, or follows a released spill. Defence: the AllAtLeast(W) epoch gate, with W taken behind the IX at or above the DROP's decide bump (catalog.cpp:2032 at the write, manager.cpp:329 / :618 at the decide, each before the X is released; CD-S0 P4). S0 premise P4 confirms the bump precedes the X release and that the PurgeWait co_await runs with the statement's own slot idle. Without the gate nothing else catches this.
- [quiet-wrong] A writer path that copies a before-image outside one exclusive hold reinstates dirty bytes after a clean. UPDATE and DELETE are verified on f3ec25ea (command_dispatcher.cpp:8884-8887, 9128-9159, 11050-11063); every other OverwriteTuple and EncodeRow caller is S0 premise P3. A hit stops CD for Design 1's row-X variant.
- [quiet-wrong] A HEAP_OVERWRITE whose envelope names the header's writer makes that writer a phantom loser at mount, and recovery undo then rewrites a committed row (analysis.cpp:84; the B1 envelope note behind OverwriteLogged, catalog.cpp:276-279). The envelope must be kNoTxnId; a btree-leaf crash test is S0 premise P1.
- [quiet-wrong] A spill released early or twice. RowSpills ignores null bits (catalog_spills.cpp:16-33), so the cell bytes must be zeroed, not only the bit set. The release happens only under the per-row gate, after the logged clean, de-duplicated. Premise P2: if a spill pointer is ever shared across rows or cells (INSERT ... SELECT, SET a = b, KWP load, ON CONFLICT), a release frees a live value, and CD stops for a ruling.
- [quiet-wrong] A KWP load across DROP c, the clean and a reusing ADD: the (physical, live) counts return to their BEGIN values (command_dispatcher.cpp:5379-5388), so c's values land in the new column. Defence: the (pos, oid, generation, flags) digest, with generation bumped by every reuse (OI-5).
- [quiet-wrong] A fresh oid written in place on reuse (the judges' graft) would make ROLLBACK and mount undo of the ADD fail the identity check (manager.cpp:440-475; recovery_undo.cpp:27-34). A delete-mark plus insert at one pos (Design 3) shows two rows at that pos to the null-view scan while the ADD is open (catalog.cpp:53-118, 2464-2486), so schema.columns[i] != pos. Both are rejected; the reuse is one in-place overwrite that keeps the oid.
- [quiet-wrong] A dangling auto-Cabin on a dropped pos (known-gaps.md:1213-1217) would become servable on the reused column with a set built from the dropped column's values. Defence: the sweep at the flag write, and PositionDependant refusing the reuse.
- [quiet-wrong] Downgrade: a v31 binary writes bit-clear dropped cells into a slot flagged clean. Defence: superblock 32 (OI-6).
- Pre-existing, not caused by CD, PLAUSIBLE and not verified end to end: cabin_mask and the INSERT Cabin witness loop stop at column 63 (catalog.cpp:2641; command_dispatcher.cpp:7786), while CreateCabin has no < 64 guard (catalog.cpp:3277-3283). A Cabin on pos >= 64 may go unwitnessed on INSERT. S0 checks this and files a docs/inflight/bugs entry if it is real; CD's same-pos reuse adds no position, so it does not widen this.

## 8. Items - marked by the operator (W1)

| # | Question | Options | CLA's proposal and reason | Mark |
|---|---|---|---|---|
| CD-OI-1 | Which level of W0 ('resetting the flag, moving and resizing tuples', workorder-bw:13) does CD cover? | (a) Level 1 only: clean dropped cells in place, then reuse clean slots. The row size never changes. Only a slot an ADDed-then-DROPped column held is reusable (kColumnInSlot is set only at catalog.cpp:1934); SPARE-0 relations and dropped declared columns gain spill release only.<br>(b) Level 1 plus resize in one milestone.<br>(c) Resize only: ALTER TABLE t REBUILD [SPARE n] into a new clustered tree. | **(a). Resize becomes its own later milestone, in Design 2's shape: ghost kColumnReclaimed rows, rows frozen to trx 1 behind a per-row resolved gate, a fresh storage oid per clustered tree, an anchor layout fingerprint checked at fill, plus a fence on AdoptRoot's in-place cache update (catalog.cpp:2872-2883).** - Level 1 lifts the binding refusal for relations that ADD and DROP (catalog.cpp:1925-1931) online, with no invariant amended and no new record kind. Resize needs the invariant-13 and txn.md:840-852 rulings, a new transactional root-swap record, an anchor format change and owner plumbing. Bundling them would hold the sound half hostage to those rulings. | **(a)**, the operator's mark (W1) |
| CD-OI-2 | How is the cleaning statement spelled and triggered? | (a) PURGE DROPPED COLUMNS FROM <t>: PURGE's verb and its autocommit, IX and horizon-refusal discipline.<br>(b) ALTER TABLE <t> CLEAN DROPPED COLUMNS.<br>(c) A background job on core 0's tick after each DROP COLUMN commit.<br>(d) Run inline by ADD COLUMN when no slot is free. | **(a)** - It shares PURGE's discipline and wait loop exactly, and nothing is reserved either way, since keywords hash as identifiers. (c) adds an unasked writer that holds a core and needs a trigger policy. (d) puts an O(n) pass and a horizon wait under ADD's X. (b) is acceptable if the operator prefers not to widen PURGE beyond keys. | **(a)**, the operator's mark (W1) |
| CD-OI-3 `[quiet-wrong]` | How does the pass clean a row? | (a) In place, with no new version, only when the head's writer is ResolvedForEveryReader, logged as HEAP_OVERWRITE with envelope kNoTxnId and the header's writer kept (the OverwriteLogged shape, catalog.cpp:276-284).<br>(b) A new MVCC version per row through an extracted UPDATE primitive (undo plus HEAP_OVERWRITE under a chunk transaction, a row X, a global horizon wait before the flag). | **(a), subject to S0 premises P1 and P3** - (a) adds no undo, no row lock and no change to UPDATE's hot path, which BW already moved by +30.2 us (results-bw-close-v3.2.0-22-g541ce440.md:40-41). The record shape has a caller today (catalog rows), so only its btree-leaf replay is new, and P1 tests it. (b) is the fallback if P3 finds a writer that is not serialised by the leaf hold. | **(a)**, the operator's mark (W1) |
| CD-OI-4 `[quiet-wrong]` | How does a reusing ADD record the column in sys.columns? | (a) One in-place OverwriteForDdl of the clean dropped row: same oid and pos, the new definition, flags = kColumnInSlot, generation + 1.<br>(b) An in-place overwrite with a fresh oid.<br>(c) Delete-mark the dropped row and insert a new row at the same pos with a fresh oid.<br>(d) A stored slot index in a new field, and a new pos. | **(a)** - (b) changes the catalog row's Keystone word in place (oid at offset 0, rows.hpp:263; keystone.cpp:44-49), so ROLLBACK and mount undo fail the identity check (manager.cpp:440-475; recovery_undo.cpp:27-34). (c) shows two rows at one pos to the null-view scan while the ADD is open (catalog.cpp:53-118, 2464-2486), misaligning every fill. (d) grows positions and adds a field that (a) does not need. | **(a)**, the operator's mark (W1) |
| CD-OI-5 `[quiet-wrong]` | What fences a KWP load across DROP, clean and a reusing ADD, now that the reused row keeps its oid? | (a) A u32 `generation` appended to SysColumnRow (95 -> 99, inside the same 31->32 bump), incremented by every reuse. DescribedShape carries a digest over (pos, oid, generation, flags).<br>(b) A digest over (pos, oid, flags, name, type_val, len), with no new field. A re-add with the same name and type at the same slot is admitted.<br>(c) Repurpose the retired 4-byte word at sys.tables offset 97 (rows.hpp, kReservedOffset) as a per-relation column-set generation, written by every column DDL. | **(a)** - Only a counter separates the cycle's end state from its start. (b) leaves a residual BW-Q10 meant to refuse. (c) puts a transactional overwrite of the sys.tables row, whose next_id is bumped in place, into every column DDL, and reuses a word retired by ruling. The 4 bytes in (a) are catalog-only and paid inside a bump CD needs anyway. | **(a)**, the operator's mark (W1) |
| CD-OI-6 `[quiet-wrong]` | Does CD bump the superblock? | (a) 31 -> 32, refusing 31 with no migration.<br>(b) No bump. | **(a)** - A v31 binary on a cleaned volume writes bit-clear dropped cells into a slot flagged clean, and a later reuse reads them wrong. The version check refuses any version that is not equal, so only a bump blocks it. The SysColumnRow size change (OI-5 (a)) is a format event too (rows.hpp:284). | **(a)**, the operator's mark (W1) |
| CD-OI-7 | How long does the pass wait for an unresolved writer or a stale-epoch statement? | (a) kPurgeHorizonWaitNs (1 s, purge_key.hpp:49), parked in the PurgeWait loop, then TxnConflict naming the key and the count; a re-run finishes.<br>(b) Up to txn_lifetime_ceiling_ms (60 s).<br>(c) A new setting. | **(a)** - It is PURGE's constant for the same wait. (b) gives an existing setting a second meaning, and (c) adds a parallel knob; both break the no-second-name rule. A re-run only reads clean rows, and the set of dirty rows with unresolved writers only shrinks. | **(a)**, the operator's mark (W1) |
| CD-OI-8 | Does the pass also clean dropped declared (non-slot) columns: zero their cells and release their spills? | (a) Yes: one rule for every dropped column, flagged alike; only in-slot ones are reusable.<br>(b) No: in-slot columns only; declared spills wait for PURGE as today. | **(a)** - It is the same loop, gate and release with no extra mechanism. It is the only space level 1 reclaims for SPARE-0 relations, and it gives the sim one invariant (every flagged dropped cell is clean). The bytes themselves stay until a resize. | **(a)**, the operator's mark (W1) |
| CD-OI-9 | Which relation lock does the flag write take? | (a) The pass's IX, inside the same autocommit DDL transaction, under the CT7 sys.columns hold.<br>(b) The relation X, briefly, after the walk. | **(a)** - Every competing column DDL (ADD, DROP) takes X and so already excludes the IX. (b) can be refused at the 1 s fault net under steady writes after an O(n) walk, which wastes the walk. | **(a)**, the operator's mark (W1) |
| CD-OI-10 `[quiet-wrong]` | What happens to a column Cabin the auto-create race left on a dropped pos? | (a) The clean statement drops it through the catalog's Cabin drop and the controller's state, and a reusing ADD refuses while any dependant names the pos.<br>(b) Refuse the clean, naming the Cabin. | **(a)** - No statement can name the column, so DROP CABIN cannot remove it (known-gaps.md:1213-1217). A refusal would leave the operator no remedy. | **(a)**, the operator's mark (W1) |
| CD-OI-11 | Does the pass yield its core? | (a) No, as every write today; recorded in known-gaps, with the stall measured at close.<br>(b) Yield between leaf windows, holding the IX and its epoch word, with no page hold across a yield. | **(a)** - A yielding write is a new exception to a stated rule (only a SELECT's outermost walk yields), which is the operator's call. The pass holds nothing across windows, so (b) can be added later without a redesign. | **(a)**, the operator's mark (W1) |
| CD-OI-12 | When both a vacant slot and a clean dropped slot exist, which does ADD take? | (a) The vacant slot first (today's path), and a clean slot only when none is vacant.<br>(b) The lowest clean slot first. | **(a)** - Positions stay bounded by declared + spare_slots either way, since each vacant slot is taken once and a reuse adds no position, so Design 1's 63-mask argument does not separate the two. (a) leaves today's ADD bit-identical whenever a slot is free. | **(a)**, the operator's mark (W1) |
| CD-OI-13 | What happens to sys.access_stats rows whose column_mask names a reused pos? | (a) Delete them at the reuse.<br>(b) Leave them; SHOW ACCESS then attributes the dropped column's history to the new one. | **(a)** - They are diagnostic and unlogged, and deleting them is the truthful choice. Clearing a bit in place could make two shape rows collide. | **(a)**, the operator's mark (W1) |
| CD-OI-14 | Where does the CD work order go, and is the letter free? | (a) Fetch origin/main, confirm CD is unused, and write instructions/v3.2.0/workorder-cd-dropped-column-cleaning.md beside CA-CC with an index.md row.<br>(b) Open instructions/v3.3.0/ for it. | **(a) - the operator's mark**: the order sits in `instructions/v3.2.0/` beside CA-CC. CLA had revised to (b), a v3.3.0 directory, after the v3.3.0 tag; the operator kept (a). | **(a)**, the operator's mark (W1) |

## 9. Stages

| Stage | What | Gate |
|---|---|---|
| CD-S0 | Premises. Tests and reads only; no engine behaviour change.<br>- P1: a HEAP_OVERWRITE on a btree leaf, with envelope kNoTxnId and the header's writer and undo_ptr kept (the OverwriteLogged shape, catalog.cpp:276-284), replays at mount with no loser and no undo applied (analysis.cpp:52-55, :84; redo.cpp:105-120).<br>- P2: whether a var-heap pointer is ever shared across rows, or across two cells of one version: INSERT ... SELECT, UPDATE SET a = b, KWP load, ON CONFLICT.<br>- P3: every caller of OverwriteTuple, EncodeRow and EncodeAssigned on a user relation reads its before-image and writes under one exclusive hold, or compensates only an undecided writer (the list in mvcc_wal_crash point 2).<br>- P4: DROP COLUMN's schema-word bump happens before its relation X is released (catalog.cpp:2032; manager.cpp:329 / :618 - corrected at S0 from :1084), and the PurgeWait co_await (command_dispatcher.cpp:796-815) runs with the statement's epoch slot idle.<br>- P5: whether CREATE ASSERTION is admitted on a relation with a dropped column, and whether ADD COLUMN is admitted on an asserted relation.<br>- P6: the pos >= 64 Cabin witness question.<br>Also: fetch origin/main and confirm the letter CD is free; write the work order with OI rulings. | Each premise is answered with path:line, at a named commit. A red P1, P2 or P3 stops CD for a ruling before S1. P6, if real, gets a bugs/ entry outside CD. Inside the 20-minute stage budget. critics-developer review of the work order. |
| CD-S1 | Codec. EncodeRow's dropped skip (row_codec.cpp:967-971) sets the dropped column's null bit when it has one. Comments are updated: row_codec.cpp:967-968, schema.hpp:178-185, and alter.md AL11 'EncodeRow writes it zero'. No format change: a v31 volume's dropped cells are never read. | Tests:<br>- an INSERT after a DROP has the dropped slot's bit set and its cell zero, on every insert path (literal VALUES, prepared, INSERT ... SELECT, ON CONFLICT, KWP load);<br>- UPDATE carries the bit;<br>- decode is unchanged;<br>- the sim integrity check accepts the bit.<br>Full suite plus the four contract suites green. critics-developer review. Overhead not measured at this stage; it is measured at the milestone's close. |
| CD-S2 | The clean pass, with no flag yet.<br>- Parser: the OI-2 spelling.<br>- Handler: inside InDdlStatement, IX via BorrowOrWait, targets and W from the TableAccess resolved behind the IX (re-run if the word moved), the epoch gate, the phase-1 read walk, the phase-2 per-key BtreeLookup(kWrite) clean through src/exec/clean_dropped.{hpp,cpp} modelled on PurgeKey, de-duplicated spill release, and the retry through the PurgeWait loop.<br>- The PurgeWait struct gains an epoch variant. | Tests:<br>- an idempotent re-run (writes nothing the second time);<br>- an undecided writer is retried, then cleaned after its decide;<br>- a rollback during the pass leaves no dirty head once the pass completes;<br>- an RR snapshot predating a row's writer blocks that row's clean and its spill release (barrier, repeated rounds);<br>- a stale-layout reader holds the epoch gate (a yielding SELECT bound before the DROP);<br>- a crash mid-pass recovers, with no loser and the cleaned rows clean, and a re-run finishes;<br>- heap, Transition-governed and FK-child relations are admitted;<br>- refusals carry byte positions;<br>- mutants that drop the gate, the epoch wait or the zeroing are killed over repeated rounds.<br>Full suite green. critics-developer review. |
| CD-S3 | Format and flag.<br>- kColumnSlotClean 0x08 and SysColumnRow `generation` u32 (95 -> 99, rows.hpp:255-284, rows.cpp:87-125), with Decode validation.<br>- kSuperBlockVersion 31 -> 32, refusing 31.<br>- On an attempt whose walk finds every target clean: sweep the column Cabins on target positions, then OR 0x08 into each target row with OverwriteForDdl under the CT7 hold, in the statement's DDL transaction.<br>- spare_free counts clean slots (sys_views.cpp:288-293). | Tests:<br>- the flag is set only by a complete attempt, and a refused attempt sets none;<br>- ROLLBACK and mount undo of the flag and the sweep;<br>- a v31 volume is refused at mount;<br>- a dangling auto-Cabin on a dropped pos is swept;<br>- the spare_free arithmetic.<br>Full suite green. critics-developer review. |
| CD-S4 | Reuse.<br>- AddColumn takes a clean dropped in-slot row when no vacant slot is free (OI-12). It overwrites in place: same oid and pos, the new definition, flags = kColumnInSlot, generation + 1. It returns a CatalogRowChange, which HandleAddColumn notes.<br>- PositionDependant is factored out of DropColumnRestriction and asked under the X.<br>- DescribedShape gains the column-set digest (ast.hpp:489-493; kwp_load_server.cpp:419-421; command_dispatcher.cpp:5379-5388).<br>- sys.access_stats rows naming the reused bit are deleted.<br>- The ADD refusal gains its hint. | Tests:<br>- DROP, clean, ADD: every pre-existing row reads NULL in the new column, including through an RR snapshot opened after the flag and a reader filling during the open ADD;<br>- ROLLBACK of the reusing ADD restores the dropped, clean row (the identity check passes);<br>- a KWP load described before the DROP is refused after the ADD;<br>- the short positional INSERT stays refused;<br>- a dependant on the pos refuses the reuse;<br>- the position count stays at most declared + spare_slots across repeated cycles.<br>The contract suites (waystone, index, cabin, inner-build) gain a DROP-clean-ADD configuration and compare byte for byte. Full suite green. critics-developer review. |
| CD-S5 | Simulation, documents and close.<br>- sim/workload.cpp gains add-column, drop-column and purge-dropped ops. sim/integrity.cpp checks flagged cells and reused-column NULLs across crashes.<br>- Documents: alter.md AL11, known-gaps.md :1203-1207 and :1213-1217 restated (resize still not built, the pass does not yield), txn.md :840-852, workorder-bw BW-R6 and BW-R11 notes plus the :700 citation, manual/sql/sql.md, and the CLAUDE.md ALTER TABLE row.<br>- The close measurement. | scripts/sim.sh green, with seeds that crash inside the pass and inside a reusing ADD. The close measurement runs within about 1 h, or is stated as not executed; a regression outside the spread stops the close. Full suite green. critics-developer review. Then sync with origin/main and stop for the operator's word on merge and push. |

## 10. Measurement

Stages:
- No stage runs an A/B. Each lands with "overhead not measured; measured at the milestone's close".
- S0's premise checks are tests and reads inside the 20-minute stage budget.

Close, about 1 h including the build:
- In build-release, interleaved A/B. A is the commit CD opens at (v3.3.0 `f3ec25ea`, or main after the fetch); B is the close commit. Both are named by `git describe --tags`.
- Results go to bench/v3.3.0/results-cd-close-<describe>.md.
- A 60 s watchdog polls a file the driver writes, never a process pattern. An orphaned run is resumed at once.

Cells:
1. Single-row INSERT, literal and prepared, on a relation with two dropped slot columns and on one with none. This prices S1's bit OR.
2. The every-row UPDATE of 1,000 rows, literal and prepared. This is BW's +30.2 us cell (results-bw-close-v3.2.0-22-g541ce440.md:40-41), expected flat because UPDATE is untouched.
3. Point SELECT and scan per row, as controls.
4. Pass throughput in rows/s on 1M rows, all dirty and all clean (a re-run), with log bytes per cleaned row and the pass's wall time. That wall time is the non-yielding core stall (OI-11).
5. p50 and p99 of a concurrent autocommit point-UPDATE stream while the pass runs, against the same stream without it.
6. Reusing ADD latency against a vacant-slot ADD.

If the budget is short, cut in this order and say so in the results file: cell 3, then cell 6, then cell 5's p99, then cell 4's 1M size (fall back to 100k).

Caveats carried forward:
- BW's UPDATE equality with pre-BW is unresolved on a loaded host (results-bw-update-fix-v3.2.0-24-ge8727478.md:78). CD compares only against its own opening commit, so it inherits nothing from that question.
- A 2-3 % delta from 3 runs is session noise (memory bn-census-position-effect). Never overlap a run with a build.

## 11. Stage log

### CD-S0 - premises (2026-10-19, on `cd-column-slot-cleaning` at `789e77e1`)

Source identical to `f3ec25ea`; reads only, nothing built or run. P1 was read
by CLA; P2-P6 were each answered by a reader and checked by two skeptics. **No
premise is red, so CD proceeds to S1.** Every verdict is "holds with
conditions". The conditions below bind the stages named in them.

- **P1 - holds by reading; S2 pins it with a crash test.** Analysis skips a
  `kNoTxnId` envelope (`src/wal/analysis.cpp:52-55`, `:84`), redo writes the
  payload's trx_id and undo_ptr (`src/wal/redo.cpp:105-120`), and a
  HEAP_OVERWRITE on a btree leaf is UPDATE's record today.
- **P2 - holds.** Only `EncodeOneValue`'s varchar arm creates a spill pointer,
  always through a fresh `ChainAppend` (`src/exec/row_codec.cpp:245-261`). No
  path copies a pointer between rows or cells: INSERT re-encodes on every
  issue round, INSERT ... SELECT carries values, not cells, and UPDATE encodes
  assigned cells fresh. Conditions:
  - S2 zeroes a dropped cell's bytes, not only its bit: `RowSpills` keys on
    the tag and ignores null bits (`src/exec/catalog_spills.cpp:27-33`).
  - A spill named only by a superseded version leaks; that is the accepted
    class (`docs/spec/heap-and-tuple.md:87`).
  - Index entries copy covered spill pointers but never follow them
    (`index_maintain.cpp:155-158`, `step_vm.cpp:1620-1630`). They stay safe
    only while DROP refuses a covered column and no index-only scan exists.
- **P3 - holds.** No writer copies a row, releases the leaf, and writes the
  copy back over a head CD could have cleaned. The path list in §3 point 2 is
  incomplete; the full list on `789e77e1`:
  - UPDATE, scan and point arms (`src/server/command_dispatcher.cpp:8833`,
    `:9095`, `:9128-9159`), under one exclusive leaf hold with no co_await;
  - DELETE's re-read and DeleteMark (`:11050-11065`);
  - leaf compaction and split (`src/storage/btree/btree.cpp:670`, `:758`),
    which move bytes under the hold they read them in;
  - `PurgeKey` (`src/exec/purge_key.cpp:34`);
  - rollback and recovery-undo compensations, including ClearDeleteMark
    (`src/txn/manager.cpp:519`, `:539`; `src/txn/recovery_undo.cpp:233`,
    `:254`), which write stale before-images only for a writer that is not
    `ResolvedForEveryReader`;
  - redo.
  `catalog.cpp:282` and `declaration_relation.cpp:214` are not user relations
  and leave the list. Conditions:
  - S2's phase 2 (re-read, zero, `OverwriteLogged`, release) holds no
    co_await. "No park under a pin" is a convention: the page latch is
    re-entrant per core and unused at one core (`docs/spec/page.md:128`).
    S2 adds a cell with a writer and the cleaner on one core.
  - The gate stays `ResolvedForEveryReader`. A weaker gate ("not in flight",
    a horizon alone) lets a compensation restore a before-image naming a
    released spill.
- **P4 - holds; the citation is corrected.** DROP COLUMN moves the schema
  word at its write (`src/catalog/catalog.cpp:2032`, after the overwrite at
  `:2015-2020`) and again at its decide (`src/txn/manager.cpp:329` on
  commit, `:618` on abort), each before `locks_->Release` (`:336`, `:622`).
  `catalog.cpp:1084`, which §2 S3(c), §7 and the S0 row cited, is
  `InvalidateAfterCompensation`, reached after the X is released
  (`command_dispatcher.cpp:6502`, after `EndWrite` at `:6349`); those
  citations are corrected in place. The PurgeWait co_await
  (`command_dispatcher.cpp:810`) runs after `DispatchAndStage` has returned,
  so the waiter's own slot is idle. Conditions:
  - S2: W is the word the statement's head published (`cache_built_at()` at
    `StatementEpochScope`, `command_dispatcher.cpp:910`), never one taken
    from a mid-statement `Revalidate`. §2 step 2's re-run when the word moves
    between the head and the IX is load-bearing and is stated as a rule.
  - S2: with no epochs (`epochs_ == nullptr`, the synchronous `Dispatch`
    path of the sim and the fixtures, `command_dispatcher.cpp:1097`), the
    gate passes: there is no concurrent core. A copy of
    `page_reclaim.cpp:134-141`'s `epochs == nullptr ||` would refuse every
    attempt there.
  - S3: the Cabin optimizer tick holds a TableAccess across a whole-relation
    decode under no epoch slot and no relation lock
    (`src/exec/cabin_optimizer_exec.cpp:42`, `:223`, `:282`, `:296`, `:319`).
    The hole predates CD: since BW, a row inserted after a DROP decodes as
    Corruption under a stale tick's layout for any variable-width dropped
    column (`row_codec.cpp:573-581`). CD-S1 narrows it to NOT NULL var
    columns; CD-S2 extends it to pre-DROP rows. It fails ApplyCreate and
    ApplyExtend builds spuriously, for live columns too; no answer is wrong,
    because only `decoded[col_pos]` is used and an in-place drop keeps every
    offset. S3 fixes it in the tick, or records it in known-gaps.md as an
    inherited gap. S3 also fixes the stale comment at
    `command_dispatcher.cpp:6557-6560` and `docs/spec/catalog.md` CT2, which
    names only `InvalidateAfterCompensation`.
- **P5 - holds.** CREATE ASSERTION is admitted on a relation with a dropped
  column (names only resolve over visible columns,
  `src/catalog/schema.cpp:30-40`), ADD COLUMN on an asserted relation is
  admitted (`docs/spec/alter.md:84-87`), and no assertion persists a
  position. Index, FK and Transition arms hold. Conditions:
  - A column-shape CREATE CABIN takes no relation lock from **either**
    origin: the operator's (`command_dispatcher.cpp:2396`) and the
    controller's (`cabin_optimizer_exec.cpp:259-261`); `known-gaps.md:1230-1233`.
    So a column Cabin can land on a dropped position from both. S3's sweep is
    keyed by (rel oid, column_no) and covers both; S3's test covers both.
  - An unlocked column CREATE CABIN can name the column of an open ADD, and
    survives the ADD's rollback. Today the next ADD takes that position and
    inherits the Cabin silently (pre-CD). So S4 asks `PositionDependant` on
    **every** ADD, not only the reusing branch, and S3's sweep also runs over
    targets already flagged clean, so the refusal's remedy ("re-run the clean
    statement") removes it. S4 adds that test.
  - Keep the Transition arm in `PositionDependant`: a Transition compiles its
    stored `column_no` (`src/exec/transition.cpp:197-213`).
  - The CABIN AUTO controller's memory-resident entries keyed by (rel oid,
    pos) survive a drop and a reuse (`include/kds/stats/cabin_optimizer.hpp:248-255`);
    a parked BUILDING entry blocks the reused column's auto Cabin until a
    restart. Performance only; S3 clears the entry by (rel oid, pos).
  - S4 adds: CREATE ASSERTION after DROP, clean, reusing ADD, restart; the
    canon revives and enforces.
- **P6 - real, outside CD.** A column Cabin at position >= 64 is served but
  never witnessed, and the FK reverse check serves it with no mask gate, so a
  parent DELETE can orphan a committed child. `[quiet-wrong]`. Filed as
  `docs/inflight/bugs/a-column-cabin-above-position-63-is-never-witnessed.md`
  with three unruled fix options. CD's same-position reuse does not widen it.

### CD-S1 - the codec (2026-10-19, on `cd-column-slot-cleaning` from `6f4d8ff4`)

`EncodeRow` sets a dropped column's null bit where it has one; the cell
stays zero. Comments in `schema.hpp`, `alter.md` AL11 and `null.md` §2.1
follow. `AlterColumnTest.ARowWrittenAfterADropIsBornClean` reads the stored
bytes after every insert path - VALUES, multi-row, a column list, ON
CONFLICT DO NOTHING, INSERT ... SELECT and a KWP load chunk (a prepared
statement binds by text, `kwp_session.cpp:561`, so it is the literal path) -
over a dropped nullable int, spilled varchar, slot varchar and a NOT NULL
int with no bit; UPDATE carries the bit, a pre-drop row keeps its bytes, and
SELECT is unchanged. A mutant without the bit fails it; a mutant without the
`kNoNullBit` guard crashes an existing cell. "The sim integrity check
accepts the bit" holds by reading, **not executed**: `sim/integrity.cpp`
reads no null bit and the sim has no ALTER op until S5.

Full Debug suite (`ctest -j8`): 4141 of 4142 passed;
`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement` failed once
under load and passed 5 of 5 alone - it creates no dropped column, and the
change is behind `has_dropped`. The review's test and doc cuts came after it;
the AlterColumn, purge and parser filters (258 tests) passed on them.
critics-developer review: no correctness finding; applied the `alter.md`
wording, a shared `LoadChunk` and one table lookup in the test, a dropped NOT
NULL column in it, and `null.md`'s clause.

Overhead not measured; measured at the milestone's close.

### CD-S2 - the clean pass (2026-10-19, on `cd-column-slot-cleaning` from `41761260`)

`PURGE DROPPED COLUMNS FROM t` (OI-2 (a)): a `dropped_columns` flag on
`PurgeStmt`, so no new statement variant; `HandlePurgeDroppedColumns` inside
`InDdlStatement` (S3's flag writes will share its transaction; with no
catalog write it moves no word and opens no DDL count), the relation `IS`
and `IX` as PURGE takes them, the `TableAccess` resolved behind the `IX`.
`BorrowChain`'s first-intention memo check (`command_dispatcher.cpp:9988`)
is §2 step 2's "re-run if the word moved", so `W = cache_built_at()` is the
word the head published and every drop the layout shows is committed at or
below it. The epoch gate passes with no epochs (the synchronous path, CD-S0
P4's condition). The walk is batched (512 dirty keys) over one keyed-slot
walk now shared with `HandlePurge` (`VisitKeyedSlotsFrom`); each dirty key is
cleaned by `exec::CleanDroppedCells` (`src/exec/clean_dropped.{hpp,cpp}`)
under one exclusive leaf hold with no suspension point (a plain function),
logged by `exec::LogHeapWrite` (`LogChainInsert`'s tail folded into it) at
`kNoTxnId`, its spills released after the hold through `ColumnSpills`
(factored out of `RowSpills`). `PurgeWait` gained `epoch`, and the wait loop
an arm for it. Every dropped column is a target (OI-8 (a)).

Tests (`alter_column_test.cpp`, `lock_family_test.cpp`
`CleanDroppedWaitTest`): every dropped cell kind cleaned once with its
spills released and its header and delete mark kept, a re-run writing
nothing; 1,100 rows across two batch boundaries; refusals with their bytes
and codes; a heap, a Transition-governed relation and an FK child admitted;
an undecided writer refused then cleaned after a rollback and after a
commit, the rollback writing the dirty image back; an older RR snapshot
keeping its row dirty and its spill live; the epoch gate on both paths; a
crash that keeps the log through a strict commit and loses every page since
a checkpoint, so redo replays each clean, with exactly one loser (CD-S0 P1
pinned). Mutants killed: the per-row gate, the zeroing, the epoch gate, and
the envelope naming the header's writer. **Not covered**: a rollback between
phase 1 and phase 2 (the single-dispatcher fixture cannot run a statement
inside the seam; phase 2's re-judge under the hold is what the gate mutant
pins), and a real BA-S15 yielding SELECT as the stale reader (core 1's slot
is driven by hand; the gate, not the reader, is what is tested).

critics-developer review: one bug, fixed by the reviewer - a stored payload
of the wrong length was read rather than refused (invariant 13); now
`Corruption` under the hold. Applied: the crash cell rebuilt so it replays
the cleans (as first written, a `SYNC` put the pages on disk and redo
skipped them), the batch cell, scope guards in the epoch cells, the shared
walk, the logger fold, the dead null check in the wait arm. Rejected: folding
the dispatcher's INSERT and UPDATE `HEAP_*` sites (`:4636`, `:9233`) into
`LogHeapWrite` - outside S2's change and on the paths the close measures;
the catalog and txn copies sit below `exec`.

Overhead not measured; measured at the milestone's close.

### CD-S3 - format and flag (2026-10-19, on `cd-column-slot-cleaning` from `6ef1a7a4`)

`SysColumnRow::kColumnSlotClean` (0x08) and a `generation` word at the
unaligned on-disk offset 95 (99 bytes; pinned by offset value, as
`SysTableRow`'s late fields are); `Decode` refuses clean without dropped as
`Corruption`. Superblock 31 -> 32, refused with no migration
(`SuperBlockTest.AVersion31VolumeDoesNotMount`). The golden log re-pinned
(every column row image is four bytes longer; the determinism cell passed
first). `Catalog::MarkColumnsSlotClean` ORs the flag through one
`RewriteColumnRow` that `DropColumn` now shares, `OverwriteForDdl` under the
CT7 hold. A complete attempt - epoch gate passed, no dirty row left - sweeps
every column Cabin on a target position (one `ListCabins`), then, unless
every target is flagged already, flags them in its DDL transaction.

The Cabin tick (CD-S0 P4): a seeded build whose decode or spill resolve
fails while `!MemoIsCurrent()` is this tick's stale layout and is deferred
to the next tick (`aborted`), not counted a failure; a failure under a
current memo is still one. This covers the gap inherited from BW (a row
inserted after a DROP) as well as the one CD widens. Not tested: a stale tick
needs a second core committing a DROP inside the tick.

The stale comment in `NoteCatalogRowChanges` and `catalog.md` CT2 (the word
moves at the decide, before the `X` is released) are corrected;
`heap-and-tuple.md` §3.1b and `manual/server/server.md` name 32.

Tests: the row round trip and the clean-without-dropped refusal; a refused
attempt flags nothing and a complete one flags every dropped column, slot
or declared, and a re-run flags nothing more; a Cabin of either origin
landed between a DROP's restrict and its write (the drop seam) is swept and
named in the reply; a commit that fails (`FailedCommitTest`, an injected
sync failure) leaves the column unflagged and nothing held. A mutant that
flags past an unresolved row is killed.

**Departures, adopted on the operator's standing go-ahead of 2026-10-19
(go-ahead-achieving-milestone):**
- The sweep is transactionless, as DROP CABIN is, not part of the flag's
  transaction (§2 S3 said "the same DDL transaction"): a Cabin on a dropped
  position is never served, so dropping it is right whatever the flag's
  transaction does. "ROLLBACK and mount undo of the sweep" therefore has
  nothing to test.
- **Mount undo of the flag is not tested**; the runtime rollback is. The flag
  is an `OverwriteForDdl` of a `sys.columns` row, the write DROP COLUMN's
  mount-undo cell already covers (`ACommittedDropSurvivesACrashAndAnOpenOneIsUndone`).
- The CABIN AUTO controller's memory-resident entry keyed by (relation,
  position) is not cleared (CD-S0 P5 had said S3 would): the dispatcher holds
  the controller `const`, an operator's DROP CABIN of an auto Cabin leaves
  the same stale entry, and `ApplyDrop` tolerates `NotFound`. Performance
  only; S5 records it in `known-gaps.md`.
- `spare_free` moves to S4, beside the ADD that takes a clean slot, so the
  view never reports a slot ADD refuses.

critics-developer review: no correctness finding. Applied: the soundness
comment's premise (a clean head's older versions are clean because only the
gated pass turns a dirty image clean - not "every head's writer resolved"),
the over-claiming sweep comment, `spare_free` to S4, the two stale "31"
documents, `RewriteColumnRow`, one Cabin scan, and no DDL transaction for a
re-run with every target flagged. Not applied: the five test locals kept
only for an `ASSERT_NE` (cosmetic).

Overhead not measured; measured at the milestone's close.

### CD-S4 - reuse (2026-10-19, on `cd-column-slot-cleaning` from `bbea35d2`)

`Catalog::AddColumnPosition` picks the position an ADD takes - the next one
while a vacant slot is free, else the lowest dropped slot column flagged
clean (OI-12 (a)); with neither, `NotImplemented`, naming PURGE DROPPED
COLUMNS when dropped slots hold the slots. `Catalog::AddColumn` takes that
position, asks again under the CT7 hold (a move is `TxnConflict`), and for a
reuse rewrites the dropped row in place through `RewriteColumnRow`: oid and
position kept, the new definition, `kColumnInSlot`, `generation` + 1
(OI-4 (a)); `HandleAddColumn` notes it as a row change, so a rollback
restores the dropped clean row. `RowLayout`'s j-th-slot rule gives the new
column the dropped one's offset and bit. `PositionDependant` is
`DropColumnRestriction` less its assertion arm, asked by every ADD of the
position it takes. `catalog::ColumnSetDigest` replaces the KWP load's two
counts (`DescribedShape{oid, column_set}`). `Catalog::ForgetAccessStats`
retires a reused position's shapes (OI-13 (a)), unlogged. `spare_free` counts
a clean dropped slot (moved here from S3). One `SweepColumnCabins` serves the
clean and the ADD.

Tests: a reusing ADD refused until the clean, with the remedy in the
refusal, then reading NULL in every older row - through a snapshot opened
before it too - with no position added and `spare_free` moving 0 -> 1 -> 0;
a rolled-back reuse restoring the dropped clean row, its oid and generation,
and the next reuse moving the generation once; a middle slot reused while
the next slot's live column keeps its offset and bit, read by a reader
filling inside the open ADD; a KWP load described before the DROP refused
after the reuse with both counts equal; a Cabin left by a rolled-back ADD
dropped by the next ADD and named in its reply; five DROP-clean-ADD cycles
adding no position and forgetting the access shapes; an assertion declared
after a DROP enforcing across the reuse and a restart. The waystone, index,
cabin and inner-build contract suites each gained a reused-slot
configuration (`tests/column_cycle.hpp`): waystone and cabin compare it with
a column added through a vacant slot, `SELECT *` included; index and
inner-build with their plain baselines. A mutant without the generation bump
is killed (four cells, the load's among them).

**Departures, adopted on the operator's standing go-ahead of 2026-10-19
(go-ahead-achieving-milestone):**
- A column Cabin on the position an ADD takes is **dropped, not a refusal**
  (§2 S4 said any dependant refuses): the position is vacant or a dropped
  column's, so no statement made the Cabin for the column about to land, and
  CD-S0 P5's rolled-back-ADD leftover sits on a vacant position the clean
  never targets - a refusal there would have no remedy. Any other dependant
  still refuses; none is reachable today, so that arm is **not tested**.
- `ColumnSetDigest` mixes the column's type, width and name beside OI-5
  (a)'s (pos, oid, generation, flags), the clean bit left out: a rollback
  moves the generation back, so a second reuse after a rolled-back one can
  repeat it, and the extra fields leave only a same-named, same-typed column
  - the same column to the load - admitted. A RENAME COLUMN now also refuses
  a load's next chunk, which says so ("columns were changed by ALTER
  TABLE").
- An ADD now answers its position before its name, so a duplicate name on a
  relation with no free slot is `NotImplemented` where it was
  `AlreadyExists`.
- Mount undo of an open reusing ADD is **not tested**: the reuse is an
  `OverwriteForDdl` of a `sys.columns` row, the path DROP COLUMN's mount-undo
  cell covers.

critics-developer review: no correctness bug. Applied: the digest's extra
fields (above), `SweepColumnCabins` - making every sweep durable on its own
(the ADD's could be lost by a crash after a rolled-back explicit
transaction) - `AddColumnPosition` without a hold, the unreachable
"vanished" case as `Corruption`, the duplicated digest comment, and the
middle-slot and open-ADD reader cells. Full Debug suite on S4 before the
review: 4167/4167; after it, 4168/4168 on S4 with S5's sim edits beside it
in the tree (not in this commit).

Overhead not measured; measured at the milestone's close.
