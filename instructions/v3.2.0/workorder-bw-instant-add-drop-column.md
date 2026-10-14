# Work order BW — instant `ADD COLUMN` / `DROP COLUMN` over reserved slots

Written 2026-10-14 on `worktree-bw-instant-add-drop-column` from
`750496b1` (`v3.2.0-12-g750496b1`), on the operator's words, recorded in
English:

- **W0**: "I want to implement ALTER TABLE ADD/DROP now. The core is to
  guarantee the action takes effect as instantly as possible, while
  keeping the invariants." The idea as given: every relation may declare a
  number of spare columns at `CREATE`, so every tuple is its own data plus
  (spare count x an idle slot); the catalog decides whether a slot is read;
  a per-relation field flag marks a dropped column without touching data;
  cleaning (resetting the flag, moving and resizing tuples) may come later.
- **W1**: the operator's marks on CLA's six decisions, written
  `D-a $!#1, D-b $!#1, D-c $!#1, D-d $!#1, the rest as proposed`:
  - D-a (a): spare space is a count of fixed 16-byte slots.
  - D-b (a): an added column is nullable only; rows that predate it read
    NULL.
  - D-c (a): a relation with no free slot refuses `ADD COLUMN`
    (`NotImplemented`); it is never rewritten to make room.
  - D-d (a): no schema dump is stored in tuples or pages; the catalog is
    the one authority.
  - D-e (a): cleaning (reclaiming dropped slots, resizing rows) is a
    separate milestone, not BW.
  - D-f (a): a relation created before BW has zero slots: `ADD COLUMN` is
    refused, `DROP COLUMN` is a flag.
- **W2**: "In this milestone skip every per-stage measurement, and hold the
  milestone measurement until directed." BW's stages carry no overhead
  measurement, and BW-S5 closes without one unless the operator directs it
  (§5).
- **W3**: `/go-ahead-achieving-milestone` - *"go ahead until achieving
  milestone, I will follow CLA proposal if decision needed, keep going in
  this session."* **BW-Q1..Q7 are adopted as CLA proposed on the
  operator's standing go-ahead of 2026-10-14
  (go-ahead-achieving-milestone)**, not marked per item by the operator.

The words are in `raft-marks-2026-10-14.md` §5-§7. W2 is BW's own word and
outranks the skill's close measurement.

**Claim tags**: `[source-read]` with `path:line` at `750496b1`; `[design]`
for reasoning about code not yet written; `[quiet-wrong]` where a wrong
choice turns a correct answer into a wrong one with no error.

**The letter.** `[source-read]` BV is the last letter used on `main`
(`750496b1`, "BV-S4: the close"); nothing under `instructions/` or `docs/`
uses BW. CLA's choice.

**BW against other orders.** BW replaces AL1's refusal of `ADD`/`DROP
COLUMN` (`docs/spec/alter.md` §1) for the class this order admits and
leaves the rest of AL1 standing: widening, type changes, constraint and
default surfaces stay refused. Not part of AR0 §8's chain.

---

## 0. What BW is

**The intent:** `ALTER TABLE t ADD COLUMN` and `ALTER TABLE t DROP COLUMN`
write catalog rows and no tuple byte, so either one costs the same on a
relation of ten rows and of ten billion.

- **G1 — instant.** Neither statement reads, writes or logs a tuple of the
  relation. Its cost is the relation `X` wait (§2, BW-R7) plus one or two
  catalog row writes.
- **G2 — invariant 13 holds.** A relation's row size is fixed at `CREATE`
  and never moves under BW: the slots are part of it from the first row.
- **G3 — the same answers.** A row written before an `ADD` reads NULL in
  the new column; a dropped column is unreachable by name, by `SELECT *`,
  by `DESCRIBE`, by `sys.columns` and by positional `VALUES`, and every
  other column reads exactly what it read before.

**Non-goals.** Cleaning (D-e). A column wider than one slot. A `DEFAULT`
or a `NOT NULL` added column (D-b). Rewriting a relation that has no slot
(D-c). `ALTER ... TYPE`, `SET DEFAULT`, `ADD CONSTRAINT` (AL1 stands).

---

## 1. The tree as read `[source-read]` at `750496b1`

1. **The layout is a tight pack.** `RowLayout::Build`
   (`src/catalog/row_layout.cpp:51-111`) places the Keystone word at 0 and
   each column at the running offset with no alignment, then appends the
   null bitmap (`ceil(nullable / 8)` bytes, one bit per nullable column in
   `pos` order, `:94`). The bitmap's position is `row_size -
   null_bitmap_bytes` (`schema.hpp:142`), so appending a column after
   `CREATE` would move both the bitmap and the row size.
2. **Column position is identity.** `Schema::columns` is every
   `sys.columns` row sorted by `pos`, and the vector index is the `pos`
   (`catalog.cpp:2195-2218`). Index key and covered columns, foreign keys'
   `column_no`, Cabin ids, Transition `column_no` and assertion columns
   all store that position.
3. **One row producer.** `EncodeRow` (`src/exec/row_codec.cpp:944-975`)
   zero-fills a `row_size` buffer and encodes each body column. It is the
   only code that builds a payload from nothing: `EncodeAssigned`
   (`:977-1008`) copies the before-image (BN-R3), and no other site sizes
   a buffer to `row_size`.
4. **Positional `VALUES` only.** `INSERT` has no column list
   (`ast.hpp:490`, `parser.cpp:2386`). A row is `ncols` or `ncols - 1`
   values (`command_dispatcher.cpp:4946-4958`, `row_codec.cpp:953-957`),
   and fk, Transition and assertion admission index the body by
   `column_no - 1` (`command_dispatcher.cpp:5007`, `:5021`, `:5052`).
5. **An all-zero varchar cell is `kNull`** (`tagged_cell.hpp:95`), and a
   decode that finds `kNull` where the bitmap says present is `Corruption`
   (`row_codec.cpp:568-576`).
6. **The catalog rows have no spare byte.** `SysColumnRow::kOnDiskSize`
   is 94 and `SysTableRow::kOnDiskSize` 106, each checked exactly
   (`rows.cpp:10`). `kSuperBlockVersion` is 24
   (`include/kds/server/superblock.hpp:146`), checked for equality at
   mount (`superblock.cpp:72-79`).
7. **`RENAME COLUMN` takes no relation lock** and is transactionless
   (`command_dispatcher.cpp:2330-2405`, `catalog.cpp:1690-1734`).
   `CREATE INDEX`/`DROP INDEX` run in `InDdlStatement` with the relation
   `X` from `BorrowRelationForDdl` (`command_dispatcher.cpp:2177-2210`).
   `DROP TABLE`'s retype is a transactional overwrite with the prior image
   handed to `ddl_undo_hook_` (`catalog.cpp:2020-2040`).
8. **The var-heap chain exists only if the schema can spill**
   (`catalog.cpp:1319`), and is cached as fixed.
9. **The `sys.*` views are hand-defined** (`src/server/sys_views.cpp`,
   `Define(...)`). `DESCRIBE` is `sys.columns WHERE rel_name =`
   (`command_dispatcher.cpp:6116`).
10. **SQL-surface readers of the column list**, each of which a dropped
    column must not reach:
    - name resolution: `Schema::FindColumn` (`schema.cpp:10`),
      `FindColumnPos` (`step_compiler.cpp:71`), and the index, assertion
      and Cabin resolvers (`index_ddl.cpp:27`, `assertion_catalog.cpp:108`,
      `cabin_ddl.cpp:38`);
    - `SELECT *`: `step_compiler.cpp:2182` and `DescribeStar`
      (`command_dispatcher.cpp:4224`, which KWP `C_DESCRIBE` replays);
    - `INSERT` arity: `command_dispatcher.cpp:2933`, `:4565`, `:4727`,
      `:4946`;
    - the KWP load stream's descriptor (`kwp_load_server.cpp:394`, `:487`)
      and the wire descriptor (`wire/row_codec.cpp:120`);
    - `sys.columns` and `DESCRIBE` (`sys_views.cpp:274-358`);
    - `RenameColumn`'s collision scan (`catalog.cpp:1700`);
    - the positional catalog doors `CreateCabin`, `CreateForeignKey` and
      `CheckIndexDef` (`catalog.cpp:3025`, `:3139`, `:3344`).

---

## 2. Rulings

Each of BW-R1..R12 is CLA's proposal; its standing is §4's.

- **BW-R1 — the grammar.** `CREATE TABLE t (...) [BTREE] SPARE <n>`, where
  `SPARE` is a trailing word in the storage clause's loop
  (`parser.cpp:1694`), matched as an identifier and never reserved, and
  `n` is an integer in `[0, 64]` (BW-Q1). Absent means 0. `ALTER TABLE t
  ADD [COLUMN] <column definition>` takes `CREATE TABLE`'s column
  definition grammar. `ALTER TABLE t DROP [COLUMN] <name>`. Neither
  statement takes `IF [NOT] EXISTS`, `CASCADE` or `RESTRICT`. Refusals
  carry their token's byte.
- **BW-R2 — the layout.** For `N` slots:
  `[Keystone word][declared columns, packed][N x 16-byte slots][null bitmap]`.
  - Declared columns pack exactly as today.
  - Slot `j` starts at `S + 16j`, with `S` the end of the declared region.
  - The bitmap gives declared nullable columns bits `0..k-1` as today, then
    slot `j` bit `k + j`, whether or not a column holds it:
    `null_bitmap_bytes = ceil((k + N) / 8)`.
  - With `N = 0` the layout is byte-identical to today's (`null.md` §2.2's
    pay-per-use).
  - A relation's `N` never changes, so neither does its row size.
- **BW-R3 — a vacant slot is written NULL** `[quiet-wrong]`. `EncodeRow`
  sets the null bit of every slot no column holds. That is what makes a row
  written before an `ADD` read NULL in the new column. Without the bit, the
  zero-filled slot reads as a present `0`, empty `char`, `1970-01-01`, or as
  `Corruption` for a varchar. `EncodeAssigned` copies the bits with the
  before-image. The bitmap stays the sole authority (`null.md` §3), with
  one polarity for every bit.
- **BW-R4 — a slot holds one column whose cell fits it.** The added
  column's width (`RowLayout::ColumnWidth`) must be at most 16. Every
  integer, `bool`, `date`, `timestamp`, `decimal` and `decimal128` fits,
  as do `char(N)` with `N <= 16` and `varchar(16)`. `varchar(16)` holds
  any length: up to 13 bytes inline, longer values spill (rule-fixed-length
  §3-§4). A bare `varchar` (the instance width), a wider `varchar(N)` and a
  wider `char(N)` are refused `NotImplemented` naming the 16-byte slot. It
  is never narrowed silently, which the truthfulness rule forbids. The
  column takes the lowest free slot.
- **BW-R5 — an added column is nullable** (D-b (a)). `ADD COLUMN c int`
  declares a nullable column. `NOT NULL` written on it is refused
  `NotImplemented`, because no row that predates it holds a value.
  `REFERENCES` on it is refused `NotImplemented` (a foreign key is declared
  at `CREATE TABLE` only, `foreign-keys.md`).
- **BW-R6 — a dropped column stays a physical column.** Its `sys.columns`
  row stays at its `pos` with a dropped flag, so every stored position
  stays valid, and its bytes stay in every row. Storage loops still walk
  it: the layout, `RowSpills` (a dropped varchar's spills are still
  released at purge) and the BN-R3 copy. The SQL surface does not see it
  (§1 item 10): no name resolves to it, `SELECT *`, `DESCRIBE` and
  `sys.columns` omit it, positional `VALUES` counts live columns only (the
  dispatcher expands a row to physical width with NULL placeholders before
  any positional consumer runs), and a decode never interprets its cell.
  `EncodeRow` writes it as zero with its bit clear. Its name is free for an
  `ADD` or a `RENAME`. A dropped slot is never reused, since its bytes are
  still in rows (D-e).
- **BW-R7 — isolation and atomicity: `CREATE INDEX`'s shape.** Both
  statements run in `InDdlStatement` and take the relation `X` with
  `BorrowRelationForDdl` before their first catalog write. They wait for
  open writers and positioned readers, a wait at the 1 s fault net is
  refused `TxnConflict`, and they are admitted inside an explicit
  transaction.
  - `ADD` inserts its `sys.columns` row under the transaction.
  - `DROP` overwrites its row transactionally, handing the prior image to
    `ddl_undo_hook_` as `DROP TABLE`'s retype does.
  - `ROLLBACK` and mount recovery undo either one.
  - The decide moves the schema word before the release (AT-S5e), so a
    parked writer re-resolves.
  - This departs from AL6 for these two forms only; `RENAME` stays
    transactionless.
- **BW-R8 — `DROP COLUMN` restricts.** It is refused, naming the
  dependant, when the column is:
  - the pk (position 0, invariant 11);
  - a key or covered column of an index;
  - either end of a foreign key;
  - under a Transition;
  - carrying a Cabin.

  Any assertion on the relation also refuses it (AL4's predicate, by
  relation, conservatively). Each check runs under the `X`.
- **BW-R9 — the format.** `SysColumnRow` gains a `flags` byte
  (`kColumnDropped`, `kColumnInSlot`; explicit masks, invariant 6), 94 →
  95. `SysTableRow` gains `spare_slots u16`, 106 → 108. The superblock
  goes 24 → 25 and refuses 24 by name, with no migration (the standing
  order of 2026-10-07).
- **BW-R10 — a relation with slots gets its var-heap chain at `CREATE`.**
  `SPARE n > 0` allocates the chain whether or not a declared column can
  spill, so an `ADD` of a `varchar(16)` allocates nothing and
  `TableAccess::varheap_page_id` keeps its fixed-at-`CREATE` property
  (§1 item 8).
- **BW-R11 — heap relations.** `SPARE` with `HEAP` is unreachable (SUS-1
  refuses `HEAP`). An existing heap relation has 0 slots (D-f (a)): `ADD`
  is refused and `DROP` is the flag.
- **BW-R12 — observability.** The `sys.tables` view gains `spare_slots`
  (declared `N`) and `spare_free` (slots no column holds; a dropped
  column's slot is not free). `DESCRIBE` and `sys.columns` list live
  columns only.

---

## 3. What does not move

- Invariant 13 and `rule-fixed-length-tuple.md` §2: one row size per
  relation, for its whole life.
- Invariants 2-5 and 11: no tuple moves and no key changes.
- The tagged cell's format and widths (rule-fixed-length §3-§4).
- `kFingerprintVersion`: `ADD`, `DROP`, `COLUMN` and `SPARE` hash as
  identifiers, and `ALTER`/`CREATE` are not patternable.
- Waystone: a statement's shape is unchanged by a column it does not name.
  A trail of a `SELECT *` replays row locations, never values (invariant 9).

---

## 4. Questions — adopted (W3)

Each item carries CLA's proposal as BW-R1..R12 state it. W1's "the rest
as proposed" covered D-e and D-f; these items arose after W1. **All seven
adopted as proposed on the standing go-ahead of 2026-10-14 (W3)**; BW-Q3's
premise - `EncodeRow` the one payload producer - re-read at `750496b1`
before its adoption, and it holds.

- **BW-Q1** — `SPARE <n>` as the spelling and `[0, 64]` as the range
  (BW-R1). 64 keeps every slot inside `cabin_mask`/`index_mask`'s 64 bits
  for any relation of up to 64 declared columns.
- **BW-Q2** — transactional, under the relation `X` (BW-R7), against
  AL6's transactionless shape.
- **BW-Q3** `[quiet-wrong]` — vacant slots written NULL (BW-R3).
- **BW-Q4** — one slot per column, cells of at most 16 bytes, with
  `varchar(16)` as the string form (BW-R4).
- **BW-Q5** — the `DROP` restrict set (BW-R8), assertions by relation.
- **BW-Q6** — the format and the superblock bump (BW-R9, BW-R10).
- **BW-Q7** — `spare_slots`/`spare_free` on the `sys.tables` view (BW-R12).
- **BW-Q8** — raised by BW-S2's review: a reader on another session sees an
  open `ADD`/`DROP COLUMN`'s schema before it decides, because the catalog
  row is rewritten in place and a read's refused `IS` reads on (`DROP
  TABLE`'s §5a shape). Options: (a) state it - the two forms are atomic and
  writer-isolated, and a reader can see an undecided column set: `SELECT *`
  without a column an open drop flags or with one an open add places (NULL
  in every row), a name refused while the drop is open; never a wrong value
  in a column it shows; (b) filter the schema by the session's view, which
  every catalog read of a relation's columns would have to take. **CLA
  proposes (a)**: (b) is a change to every schema read, outside BW's scope,
  and (a)'s exposure is a column set, never a value. **Adopted (a) on the
  standing go-ahead of 2026-10-14 (W3).**

---

## 5. Stages

Every stage runs the full suite and gets a `critics-developer` review
(the Session Workflow). **No stage measures (W2).**

- **BW-S1 — format and layout.**
  - BW-R9's rows and superblock 25; `Schema::spare_slots`.
  - `RowLayout::Build` per BW-R2.
  - `EncodeRow`'s vacant bits (BW-R3).
  - `CREATE TABLE ... SPARE n` (BW-R1, BW-R10).
  - Cells:
    - the layout's offsets and bits;
    - `N = 0` byte-identical;
    - a vacant slot's bit set on insert and carried by an `UPDATE`;
    - `SPARE` refused out of range;
    - a 24 volume refused.
- **BW-S2 — `DROP COLUMN`.**
  - The parser, the dispatcher under the `X` (BW-R7) and BW-R8's
    restricts.
  - The transactional overwrite.
  - Every §1 item 10 site.
  - The decode skip and the `VALUES` expansion.
  - Cells:
    - a dropped column gone from every surface;
    - the other columns unchanged;
    - a dropped varchar's spill still released at purge;
    - `ROLLBACK` restores it;
    - a crash mid-transaction restores it at mount;
    - each restrict refused with its dependant's name;
    - the name re-addable and renamable.
- **BW-S3 — `ADD COLUMN`.**
  - The parser (the column definition), BW-R4/R5's refusals, the slot
    choice and the transactional insert.
  - Cells:
    - rows before the `ADD` read NULL in every type, `varchar(16)`
      included;
    - rows after carry the value, spilled and inline;
    - `ADD` after `DROP` takes the next slot, never the dropped one;
    - slots exhausted refused `NotImplemented` (D-c);
    - a relation from before BW refused (D-f);
    - `ROLLBACK` and crash restore;
    - an index, a Cabin and a Transition on an added column work.
- **BW-S4 — the text.**
  - `alter.md`: AL1 rewritten for BW's class, AL6's exception, AL7's
    grammar and refusals, AL10.
  - `rule-fixed-length-tuple.md` §2 (the slot region).
  - `null.md` §2 (slot bits).
  - `ddl-transactional.md` §5 (the two forms in scope).
  - `heap-and-tuple.md`'s format version.
  - The manual's `ALTER TABLE` and `CREATE TABLE`.
  - CLAUDE.md's ALTER TABLE row.
- **BW-S5 — the close.** The overhead measurement is held (W2). The close
  report says it was not run.

---

## 6. Where to start

Nothing is built at the writing. BW-Q1..Q7 adopted (W3); BW-S1 is next.

---

## 7. Stage log

- **BW-S1** landed at `f2a72474`. Review (`critics-developer`): a cell was
  sized from its offset to the next column's, so with slots the last
  declared column's cell ran over the slot region - inline capacity, the
  `char(N)` length check and a covering index's copy all read the wrong
  span; fixed with `RowLayout::cell_ends` and a cell. Applied: one
  `sys.tables` read per cache fill, `spare_free` from the layout, one loop
  in `Build`, `Corruption` for an on-disk slot count past 64. Suite
  3818/3818. Overhead not measured (W2).
- **BW-S2** - review (`critics-developer`) found and fixed three defects:
  an `INSERT` counted its values against an open drop's schema before its
  `IX` (refused a row the drop's rollback would have admitted); a `RENAME
  COLUMN` could take the name of a column whose drop was still open (two
  live columns of one name after the rollback); `ColumnLabel` printed a
  dropped position's old name in `sys.access_stats`/`sys.cabins`. Applied:
  one `OverwriteForDdl` for the three transactional catalog overwrites
  (`DropTable`, `DropNamespace`, `DropColumn`), one arity refusal
  (`InsertArityRefusal`) over `RowLayout::live_columns`, rows expanded in
  one copy, the pk refused at the dispatcher with its byte. BW-Q8 raised
  and adopted (a). **Rejected**: (B) "an `INSERT` refused its `IS` at bind
  by an open drop that then rolls back writes with the stale schema" - the
  decide moves the schema word (`Transaction::NoteWroteCatalog`, set by
  `MarkHoldsDdl`) and the writer's first relation intention refuses a moved
  word (`MemoIsCurrent`, `BorrowOrWait`), so the statement re-runs; (C) an
  auto `CreateCabin` racing the restrict check is recorded in
  `known-gaps.md` rather than fenced, since a Cabin on a dropped column is
  never served (no statement names it).
