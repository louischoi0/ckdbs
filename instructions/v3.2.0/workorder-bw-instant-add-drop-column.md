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

- **W7**: *"BW-Q11 $!a include this fix in BW M"* - BW-Q11 (a) is the
  operator's mark, and the `INSERT` column list that closes it is built in
  BW as BW-S6 (`raft-marks-2026-10-14.md` §11).

The words are in `raft-marks-2026-10-14.md` §5-§7 and §11. W2 is BW's own word and
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
   **Since BW-S6** `INSERT` also takes a column list
   (`InsertStmt::columns`), mapped onto that same body by
   `NormalizeListedInsertRows`; the positional form is unchanged.
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

Each of BW-R1..R13 is CLA's proposal; its standing is §4's.

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
- **BW-R5 — an added column is nullable** (D-b (a)), **and says so**
  (revised at BW-S3, adopted on the standing go-ahead of 2026-10-14, W3).
  As written, `ADD COLUMN c int` declared a nullable column; but this
  engine reads an unmarked column as `NOT NULL` (`null.md` §2.3), so
  admitting it as nullable would enforce something other than what was
  written. The definition must carry `NULL`; `c int` and `c int NOT NULL`
  are refused `NotImplemented` (the rows that predate it hold no value, and
  there is no `DEFAULT`). `REFERENCES` and a cabin clause on it are refused
  `NotImplemented` (both are `CREATE TABLE`'s). A consequence, not a rule:
  an added column cannot be indexed (`null.md` D2) or governed by a
  Transition, since neither takes a nullable column.
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
  - the tuple key (position 0, invariant 11);
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
- **BW-R13 — `INSERT` with a column list** (W7, BW-Q12).
  - **Grammar.** `INSERT INTO <t> [ ( <col> [, <col>]* ) ] VALUES ( ... )
    [, ( ... )]*`. The list is optional, and an `INSERT` without one is
    positional as before (BW-Q9).
  - **Names.** Each name resolves exactly, byte for byte, to a live column,
    as `UPDATE ... SET` resolves. An unknown or dropped name is refused,
    and so is a name listed twice. An empty list is refused.
  - **Rows.** Each row has one value per listed column, in list order.
  - **The tuple key.** Left out of the list, it is issued. Listed, the
    row's value is the key, and `NULL` there has one issued.
  - **Omitted columns.** A nullable column left out is NULL. A `NOT NULL`
    column left out refuses the statement before any row is written, since
    no `DEFAULT` exists.
  - **No shift.** A listed row is mapped by name, so no column change can
    shift it, and BW-Q9's arity rule does not apply to it.
  - **Fingerprint.** The shape is the column list. `kFingerprintVersion`
    does not move, and `INSERT` stays unpatterned.
  - **KWP.** The load path is unchanged.
  - **Refusals.** All are `InvalidArgument` at the offending token's byte
    (BW-Q12.12).

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
- **BW-Q9** `[quiet-wrong]` — raised by BW-S3's review: `INSERT`'s two
  positional arities (§1 item 4) collide across a column-set change. After
  an `ADD`, a row written for the old columns with its key named has the
  length of the short, key-issued form; after a `DROP`, a key-issued row
  has the full form's; either would be admitted with every value one column
  over and no error, and a KWP load described before the change sends such
  rows too. Options: (a) on a relation whose column set has changed
  (`RowLayout::column_set_changed`: a column added or dropped), refuse the
  short form and take a `NULL` key at full width as the issued-key row -
  which KWP already sends (BI15) and SQL then admits on every relation; (b)
  a column list on `INSERT`; (c) state it in `known-gaps.md`. **CLA
  proposes (a)**: it turns every shifted row into a refusal naming the
  count, keeps id issue reachable, and (b) is a grammar BW does not own.
  Premise re-read against the tree before adoption: the short form is the
  only arity the change makes ambiguous, a full-width row is ambiguous with
  nothing, and the KWP load builds full rows once its NULL tuple key is passed
  through. **Adopted (a) on the standing go-ahead of 2026-10-14 (W3).**
- **BW-Q10** `[quiet-wrong]` — raised by BW-Q9's review: a KWP load
  describes the relation at LOAD BEGIN and each chunk re-resolves it by
  name, taking its `IX` only then, so a `DROP` and an `ADD` of equal net
  width between the two leave the chunk's width valid and land a dropped
  column's value in the added one. Options: (a) the load records the
  relation's oid and column shape (physical and live counts, which every
  `ADD` or `DROP` moves) and a chunk is refused, behind its `IX`, unless the
  relation is still that one in that shape; (b) take the relation's `IS` at
  LOAD BEGIN and hold it to LOAD END, which keeps a load's window open
  against every DDL. **CLA proposes (a)**: it refuses exactly the chunks
  that would land wrong and holds nothing between frames; it also closes
  the older case of a relation dropped and re-created under the name.
  **Adopted (a) on the standing go-ahead of 2026-10-14 (W3).**
- **BW-Q11** `[quiet-wrong]` — raised by BW-S4's review, against BW-Q9's
  rationale: BW-Q9 (a) catches every stale row after an `ADD`, but after a
  `DROP` a row written for the old columns with its key left out has the
  new full form's length, and after one `DROP` and one `ADD` an old full
  row has the new full length - admitted, every value one column over. A
  row's length cannot tell the two apart. Options: (a) state it as a
  `[quiet-wrong]` known gap, closed by an `INSERT` column list, which
  BU-S8 owns; (b) on a relation with a dropped column, refuse a named key
  at full width, so the full form is `NULL`-key only - no stale row is
  admitted, and a caller-supplied key is unreachable on every relation
  that has ever dropped a column; (c) refuse positional `INSERT` on such a
  relation, which leaves it unwritable until BU-S8. **CLA proposes (a)**:
  (b) and (c) give up a working surface on every altered relation to cover
  a statement written before a change, which is the class every positional
  `INSERT` carries across a schema change, and the column list that closes
  it is another milestone's. **Adopted (a) on the standing go-ahead of
  2026-10-14 (W3)**; the operator's mark is the one this item most needs.
  **Marked (a) by the operator (W7)**, with the column list built in BW
  (BW-S6) rather than BU-S8.
- **BW-Q12** — BW-S6's design: `INSERT` with a column list (W7). Raised by
  W7's mark of BW-Q11 (a); the tree as read `[source-read]` at `e3450498`.
  Each sub-item carries its options and CLA's proposal; BW-R13 states the
  result.
  - **BW-Q12.1 — the grammar.** `INSERT INTO <t> [ ( <col> [, <col>]* ) ]
    VALUES ( ... ) [, ( ... )]*`. `Parser::ParseInsert`
    (`parser.cpp:2423-2467`) peeks for `(` after the qualified name; a
    positional statement has `VALUES` there, so the peek is unambiguous.
    The list is read by `ParseDeclaredColumnList` (`parser.cpp:1859-1893`)
    with no cap, each name keeping its byte. `InsertStmt`
    (`ast.hpp:506-512`) gains `std::vector<IndexColumnRef> columns` (empty
    means positional) and `columns_byte_offset`, the byte of the list's
    `(`. Options: (a) as stated; (b) a new AST type for an insert column.
    **CLA proposes (a)**: `IndexColumnRef` is already `{name, byte}`, and a
    second type with the same fields is the duplication the review rules
    refuse. `INSERT ... SELECT`, `DEFAULT VALUES` and expressions in
    `VALUES` stay BU-S8's and are not touched. **Adopted as proposed on the
    standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.2 — the parser's missing bytes.** `ExpectKeyword`
    (`parser.cpp:113-120`) and `ParseIdent` (`:136-146`) refuse with no
    byte. Because of that, a missing `VALUES` after the list, and a
    non-identifier in the list (`(1)`, `()`, `(?)`), would refuse with no
    position. Options: (a) give both helpers the token's byte, once, as
    `ExpectToken` already does (`:122-133`); (b) catch the two refusals at
    `ParseInsert`'s call sites only. **CLA proposes (a)**: the position rule
    is engine-wide, and fixing it in the helpers covers every production
    that routes through them. The verdict codes do not move, so the
    corpus's verdict column does not move. Only message text gains
    `at byte N`, and a test that matches the old text exactly is updated.
    **Adopted as proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.3 — name matching.** Options: (a) exact bytes, through
    `Schema::FindColumn` (`schema.cpp:10-17`), as `UPDATE ... SET`
    (`step_compiler.cpp:1624`, `set_expr.cpp:472`), Transitions and
    `RENAME COLUMN` resolve; (b) case-insensitive, as SELECT and WHERE
    resolve (`FindColumnPos`, `step_compiler.cpp:71-81`). **CLA proposes
    (a)**, departing from SELECT's surface for a reason the survey found.
    Column-name uniqueness is exact-case (`catalog.cpp:1764-1766`,
    `:1818`), so `a` and `A` can both exist, and (b) would have to pick one
    of the two in silence. A read that picks the wrong one shows a wrong
    column. A write that does would store a value in the wrong column,
    `[quiet-wrong]`. `INSERT` is a write that names columns, which is
    exactly the class `UPDATE ... SET` is (BJ-R2), so it takes that rule.
    SELECT's case-insensitive pick is a separate matter that BW-S6 does
    not change. **Adopted as proposed on the standing go-ahead of
    2026-10-14 (W3).**
  - **BW-Q12.4 — an unknown, dropped or repeated name.** An unknown name
    is refused `InvalidArgument` "unknown column 'x' at byte N", at the
    name's byte, the same text `UPDATE ... SET` uses. A dropped column's
    name is unknown (BW-R6). A name listed twice is refused
    `InvalidArgument` at the second occurrence's byte
    (`step_compiler.cpp:1626-1635`'s shape). Options for where these
    checks run: (a) in `NormalizeInsertRows` (`command_dispatcher.cpp:3161`),
    which runs after the `IX` (`:4888`); (b) duplicates in the parser,
    the rest in the dispatcher. **CLA proposes (a)**: the column set must be
    read behind the `IX`, because a `DROP COLUMN` overwrites its row in
    place before its decide (`:4899-4902`). Keeping every name check in one
    loop over the list, in written order, means the first refusal is the
    leftmost bad name. A list longer than the live column count must hold
    an unknown or a repeated name, so no separate cap is needed.
    **Adopted as proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.5 — an empty list.** `INSERT INTO t () VALUES ()`. Options:
    (a) refuse it `InvalidArgument` at the `)` byte, as `ParseIdent`'s
    refusal of a non-identifier (BW-Q12.2); (b) read it as "every column
    omitted". **CLA proposes (a)**: the engine has no `DEFAULT`, so the only
    rows (b) could write are an issued key with every other column NULL,
    and `VALUES ()` is already refused (corpus line `INSERT INTO t VALUES
    ()`). Admitting one empty form and refusing the other would be two
    rules for one statement. **Adopted as proposed on the standing go-ahead
    of 2026-10-14 (W3).**
  - **BW-Q12.6 — the tuple key column.** If the list leaves the tuple key
    out, an id is issued for every row. If the list names it, the row's
    value at that list position is the key, and a `NULL` there issues one,
    as a KWP load row's does (BI15) and as BW-Q9's full-width row does. The
    key may sit anywhere in the list. Every present refusal of a named key
    is unchanged and names the value's byte: integer type, `[1, 2^48 - 1]`
    `OutOfRange` and `AlreadyExists` (`command_dispatcher.cpp:5298-5316`,
    `:5423-5426`). Options: (a) as stated; (b) require the key listed.
    **CLA proposes (a)**: an omitted key and a `NULL` key both mean "issue
    one" on every other surface, and both come out of the normalizer's
    existing arity split without new code downstream. **Adopted as
    proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.7 — a column the list leaves out.** There is no `DEFAULT`. An
    omitted nullable column is written NULL. Every added column is
    nullable (BW-R5), so a list written before an `ADD` stays valid after
    it. An omitted `NOT NULL` column has two options:
    - (a) refuse the statement once, before any row is touched,
      `InvalidArgument` naming the column, "is NOT NULL and the column
      list leaves it out; there is no DEFAULT", at `columns_byte_offset`;
    - (b) stamp each NULL placeholder with a byte and let `EncodeRow`
      (`row_codec.cpp:904-909`) refuse per row.

    **CLA proposes (a)**: the refusal is a property of the list, not of a
    row. It names a real byte, where (b) would name the placeholder's byte
    0 or a borrowed one. It is the same code `EncodeRow` gives an explicit
    `NULL` into a `NOT NULL` column. A Transition column is `NOT NULL`
    (`null.md` D2), so an omitted Transition column falls under (a).
    **Adopted as proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.8 — row width.** Every row carries exactly one value per
    listed column. A row of any other length is refused `InvalidArgument`
    "expected N value(s), one per listed column; got M", at the row's
    first value's byte, with `(row k)` appended on a multi-row statement
    as the bulk refusals do. Options: (a) as stated; (b) also accept the
    list's length plus or minus the key, as positional rows do. **CLA
    proposes (a)**: the list says where every value goes, and a second
    arity would bring back the ambiguity BW-Q9 and BW-Q11 exist for.
    **Adopted as proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.9 — BW-Q9's rule and the normalized shape.** Options: (a) a
    column-listed row is never subject to BW-Q9's short-form refusal
    (`command_dispatcher.cpp:3181-3203`); (b) apply it to both kinds of
    row. **CLA proposes (a)**: BW-Q9 refuses a row whose length cannot say
    which columns it means, and a listed row names them.
    - `NormalizeInsertRows` gains a list branch that maps each value to its
      column by name and emits the shape every downstream consumer already
      reads:
      - the key value first when one is named and not `NULL`;
      - then one value per physical column;
      - a NULL placeholder at every dropped position and at every omitted
        nullable position.
    - The list branch always normalizes. Its no-copy fast path (`:3170`)
      stays positional-only, because an identity-list shortcut is an
      optimization nobody measured (W2).
    - Downstream code does not change: `InsertBodyOf`, fk extraction and
      the forward check, Transitions, assertion admission, `EncodeRow`,
      Cabin, index maintenance, and the heap-only sorted fill all read
      `column_no - 1` over the physical width, exactly as for a positional
      row. To each of them, an omitted nullable column is identical to an
      explicit `NULL`.
    - Positional `INSERT` and BW-Q9 stay as they are. BW-Q11's
      `[quiet-wrong]` gap narrows to positional rows, and its
      `known-gaps.md` entry is restated to cover them only.

    **Adopted as proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.10 — fingerprint and Waystone.** Today, BI5's suppression
    (`fingerprint.cpp:262-303`) closes the shape on the first top-level
    group of an `INSERT`-headed stream. With a list, that group is the
    list. So the shape and `fetch_id` cover `INSERT INTO t ( a , b )`, and
    every `VALUES` row folds into `arg_hash` only. Options:
    - (a) leave the algorithm as it is. Two lists, or one list in two
      orders, are two shapes. The row count, the row arity and the literal
      kinds do not enter the shape.
    - (b) close the shape on the first group after `VALUES`.

    **CLA proposes (a)**, with no `kFingerprintVersion` move. The bump rule
    (`fingerprint.hpp:162-197`) moves the version only when an
    already-storable statement hashes differently, and no column-listed
    `INSERT` has ever parsed. Under (a), no hash moves at all, the
    column-listed ones included. The corpus line `INSERT INTO t (a, b)
    VALUES (1, 2)` (`parser_corpus.txt:135`) flips its verdict to `ok`
    with every hash unchanged and is marked `[BW-S6]`, which is the
    corpus header's deliberate verdict move. `INSERT INTO t (1)`
    (`:121`) stays `InvalidArgument`. (b) would add a second shape per list
    and would tell nothing the list does not already say.
    - `INSERT` is fingerprinted but not patterned. It records no trail and
      registers no `sys.patterns` row (`bulkinsert.md` §2.4), and its
      `fetch_id` reaches only the memory-resident `sys.statement_stats`.
      So the shape choice affects statistics only and never a Waystone
      replay (invariant 8).
    - One accepted consequence: identifiers fold case in the shape, so `(a)`
      and `(A)` share a `fetch_id` though BW-Q12.3 resolves them to two
      columns. It is a statistics key, and the two statements' rows are
      still written correctly.

    **Adopted as proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.11 — KWP.** The load path (`kwp_load_server.cpp:499-520`)
    builds an `InsertStmt` with no list and its `DescribedShape` (BW-Q10),
    and stays untouched. A list in SQL text arrives on the wire as text.
    A prepared statement's `?` is spliced into `VALUES` (`kwp_session.cpp`
    `Substitute`, `:175`). A `?` in the list is not refused at `C_PARSE`,
    which only counts placeholders (`kwp_session.cpp:521`): it is refused
    at `C_EXECUTE`, by `ParseIdent` against the substituted text, at that
    text's byte (BW-Q12.2) - no bound value lexes as an identifier, so no
    parameter can become a column name (amended by BW-S6's review). Options: (a) as stated; (b) a
    described shape for SQL text. **CLA proposes (a)**: the list is the
    statement's own description, so text needs no other. **Adopted as
    proposed on the standing go-ahead of 2026-10-14 (W3).**
  - **BW-Q12.12 — refusals, in the order a statement meets them.** Every
    refusal names a real byte:
    1. **Parse.** `InvalidArgument` at the offending token: a non-identifier
       or an empty list, a missing `)`, or a missing `VALUES`.
    2. **Row cap.** `max_insert_rows`, unchanged.
    3. **Under the `IX`, the list itself, in written order.**
       `InvalidArgument` for an unknown or dropped name (at its byte) and
       for a repeated name (at the second occurrence's byte).
    4. **Under the `IX`, a `NOT NULL` column left out.** `InvalidArgument`
       at `columns_byte_offset` (BW-Q12.7).
    5. **Under the `IX`, each row's width.** `InvalidArgument` at the row's
       first value, with `(row k)` (BW-Q12.8).
    6. **Then every present per-value refusal, unchanged**: key type and
       range, `AlreadyExists`, NULL into `NOT NULL`, type and width, fk,
       Transition and assertion.

    No refusal here is `NotImplemented` or `Unsupported`. Each one is a
    statement that is simply wrong against the relation. **Adopted as
    proposed on the standing go-ahead of 2026-10-14 (W3).**

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
- **BW-S6 — `INSERT` with a column list** (W7, BW-Q11 (a), BW-R13).
  `INSERT INTO t (c1, c2, ...) VALUES (...)` names its columns, so no
  column change can shift it. Its design items are BW-Q12 above, adopted
  on W3 as CLA proposes.
  - The parser: `InsertStmt::columns` and `columns_byte_offset`, plus
    `ParseInsert`'s peek and `ParseDeclaredColumnList` (BW-Q12.1). The
    bytes `ExpectKeyword` and `ParseIdent` lack (BW-Q12.2).
  - `NormalizeInsertRows`'s list branch, under the `IX` (BW-Q12.4,
    BW-Q12.6 to BW-Q12.9). Nothing downstream of it changes.
  - The corpus: `INSERT INTO t (a, b) VALUES (1, 2)` flips to `ok` with
    its hashes unchanged, marked `[BW-S6]`. New lines for the listed forms
    and their parse refusals. No `kFingerprintVersion` move (BW-Q12.10).
  - The text:
    - `manual/sql/sql.md`: the `INSERT` grammar, "no column list", and the
      post-`ALTER` advice, which now points at the list.
    - `alter.md`: AL11, and BW-Q9's paragraph at `:215-225`.
    - `heap-and-tuple.md:197` and `bulkinsert.md` §2.4: how a listed shape
      hashes.
    - `known-gaps.md`: BW-Q11's entry narrows to positional rows.
    - The comments at `ast.hpp:487`, `command_dispatcher.hpp:559` and
      `:1366`, and `command_dispatcher.cpp:3147-3160` and `:4895-4897`.
    - §1 item 4 and §6 of this order.
  - Cells:
    - The list in another order than the relation's lands each value in
      its column. A single-row and a multi-row statement are both covered,
      and so is `max_insert_rows`.
    - The tuple key, in three cases:
      - left out: issued;
      - listed with a value: placed there, in any list position;
      - listed `NULL`: issued.
      The existing key refusals still name the value's byte.
    - Omitted columns:
      - a nullable one is written NULL;
      - a `NOT NULL` one is refused at the list's `(` byte, naming the
        column, with nothing written;
      - an omitted Transition column is refused the same way.
    - Bad names are refused at the name's byte: an unknown name, a dropped
      column's name, the second occurrence of a repeated name, and `A`
      where only `a` exists (exact case).
    - Parse refusals at their byte: `()`, `(1)`, `(?)`, a missing `)`,
      and a missing `VALUES`.
    - A row wider or narrower than the list is refused at its first value,
      with `(row k)`.
    - After an `ADD`, a list without the new column writes NULL there, and
      a list naming it writes the value.
    - BW-Q11's stale cases cannot shift:
      - after a `DROP`, a list naming a surviving column lands correctly;
      - after one `DROP` and one `ADD`, a listed row lands in the right
        columns.
    - BW-Q9's positional refusals are unchanged on the same relation.
    - Downstream consumers with a reordered list:
      - an fk column listed out of order is checked against its parent,
        and an omitted nullable fk column is NULL and not checked;
      - an index on a column listed out of order gets the right key;
      - an assertion admits the right body.
    - `ROLLBACK` of a listed insert.
    - A KWP prepared listed `INSERT` with `?` in `VALUES`, and a KWP load
      unchanged.
    - The fingerprint:
      - one list with two row counts is one `sys.statement_stats` key;
      - two orders of one list are two keys.

---

## 6. Where to start

**BW is closed again, with BW-S6** - on the operator's word of 2026-10-14
(W8, `raft-marks-2026-10-14.md` §12), at the merge `3b5daa80`. BW-S6 (the
`INSERT` column list, BW-Q12/BW-R13) landed at `e1e17abc`, its review at
`33519b28`, its guard measurement at `9f5faf50`
(`bench/v3.2.0/results-bw-s6-insert-v3.2.0-32-g33519b28.md`: the positional
`INSERT` and point `SELECT` inside the spread). The full suite last ran green
at `33519b28` (3865/3865); the merge `3b5daa80` and this record landed with
the suite not run on them (W8's `--no-verify`). BW reopened for BW-S6 (W7);
S1-S5 stand as closed. **BW was closed** - on the operator's word of 2026-10-14 (W6, `raft-marks-2026-10-14.md` §10), at `d6cb3826` (`v3.2.0-28-gd6cb3826`, on `main`). BW-S1..S4 landed (`f2a72474`, `69a31361`, `bc42a011`, `d0134573`); BW-Q1..Q11 adopted on the standing go-ahead (W3). The close measurement ran when the operator directed it (W4, W5): `bench/v3.2.0/results-bw-close-v3.2.0-22-g541ce440.md` found the every-row `UPDATE` +30 us / 1,000 rows outside the spread; the profile attributed none of it to a BW-changed line (`bench/v3.2.0/archive/bw-update-profile/`); `e8727478` hoisted the dropped-column gate out of the codec loops, and `bench/v3.2.0/results-bw-update-fix-v3.2.0-24-ge8727478.md` measured it below `541ce440` in 7 of 7 runs on both paths, while whether it equals the pre-BW tree is unresolved on a loaded host (`known-gaps.md`). Left open: BW-Q11's `[quiet-wrong]` gap for a positional `INSERT` only - BW-S6's column list closes it for a listed one (W7; built, §7); cleaning (D-e). BW-S6's review and the stage's close are what remain of the reopened BW.

---

## 7. Stage log

- **BW-S6** built on `worktree-bw-instant-add-drop-column` over
  `e3450498`: the parser's column list (`ParseInsert`'s peek,
  `ParseDeclaredColumnList`, `InsertStmt::columns`/`columns_byte_offset`),
  `ExpectKeyword` and `ParseIdent` with their byte, and
  `NormalizeListedInsertRows` under the `IX` (BW-Q12.1-Q12.12); the corpus
  line flipped with its hashes unchanged, eleven new lines; cells in
  `tests/insert_column_list_test.cpp`, `parser_test.cpp` and
  `kwp_session_test.cpp`. Overhead not measured here, measured at the
  stage's close.
- **BW-S6 review** of `e1e17abc` (three lenses: correctness, SQL surface,
  adversarial - 31 probes over drops and adds open in another session,
  drop-then-add in one transaction, case-only names, tuple key shapes,
  parse spellings and every downstream consumer). **No correctness defect
  and no `[quiet-wrong]` path in BW-S6's code**, so no failing-first cell
  was owed. Applied: the manual's example named `symbol` on a `trades`
  declared with `sym` and was refused if run (`symbol` -> `sym`), and its
  comment said "a column left out is NULL" where only a nullable one is;
  the NOT NULL refusal read "there is no DEFAULT at byte N", as if the
  byte were the DEFAULT's - now "leaves it out (byte N); there is no
  DEFAULT", in the code, its cell and the manual's error table; BW-Q12.11
  claimed a `?` in the list is refused at its byte, true of text only -
  over KWP `C_PARSE` counts placeholders and the refusal comes at
  `C_EXECUTE` against the substituted text, amended in place; one
  `InsertShellOf` for the header both normalizers copied; `InsertOneRow`'s
  comment still said INSERT has no column list; `InsertBodyOf`'s
  over-long comment line re-wrapped; `InsertStmt`'s doc comment moved off
  `DescribedShape` onto its struct. Recorded, not fixed (predate BW-S6):
  `SELECT` resolving names ignoring case and taking the first match, while
  names are unique exact-case and every write matches exactly -
  `[quiet-wrong]`, reproduced by the review, needs the operator's ruling
  between refusing case-only duplicate names and exact-match reads
  (`docs/inflight/bugs/select-reads-the-first-column-whose-name-matches-ignoring-case.md`,
  `known-gaps.md`, a caveat in the manual - fixed and deleted at `2cc15aca`
  on the operator's option (a), `raft-marks-2026-10-14.md` §13; the entry
  resolves against `f544d161`); the codec's type refusal
  carrying no byte (`known-gaps.md`). **Rejected**: one byte-suffix format
  for every `INSERT` refusal - the listed refusals' "at byte N" is the
  parser's form for the same list (BW-Q12.2), and rewriting the positional
  "(byte N)" texts is churn with no position gained; folding the listed
  branch's `optional` wrap into `InsertParsed` - one entry point for both
  shapes is worth the three lines; a KWP cell for `?` in the list and a
  cell for the `(a)`/`(A)` statistics key - the first is pinned by the
  corrected claim and `ParseIdent`'s text cells, the second is an advisory
  key BW-Q12.10 accepts. Full suite 3865/3865 (`-LE heap-suspended`, Debug;
  one run first failed a second cell pinning the old NOT NULL text, updated).

- **BW-S1** landed at `f2a72474`. Review (`critics-developer`): a cell was
  sized from its offset to the next column's, so with slots the last
  declared column's cell ran over the slot region - inline capacity, the
  `char(N)` length check and a covering index's copy all read the wrong
  span; fixed with `RowLayout::cell_ends` and a cell. Applied: one
  `sys.tables` read per cache fill, `spare_free` from the layout, one loop
  in `Build`, `Corruption` for an on-disk slot count past 64. Suite
  3818/3818. Overhead not measured (W2).
- **BW-S2** landed at `69a31361`. Review (`critics-developer`) found and fixed three defects:
  an `INSERT` counted its values against an open drop's schema before its
  `IX` (refused a row the drop's rollback would have admitted); a `RENAME
  COLUMN` could take the name of a column whose drop was still open (two
  live columns of one name after the rollback); `ColumnLabel` printed a
  dropped position's old name in `sys.access_stats`/`sys.cabins`. Applied:
  one `OverwriteForDdl` for the three transactional catalog overwrites
  (`DropTable`, `DropNamespace`, `DropColumn`), one arity refusal
  (`InsertArityRefusal`) over `RowLayout::live_columns`, rows expanded in
  one copy, the tuple key refused at the dispatcher with its byte. BW-Q8 raised
  and adopted (a). **Rejected**: (B) "an `INSERT` refused its `IS` at bind
  by an open drop that then rolls back writes with the stale schema" - the
  decide moves the schema word (`Transaction::NoteWroteCatalog`, set by
  `MarkHoldsDdl`) and the writer's first relation intention refuses a moved
  word (`MemoIsCurrent`, `BorrowOrWait`), so the statement re-runs; (C) an
  auto `CreateCabin` racing the restrict check is recorded in
  `known-gaps.md` rather than fenced, since a Cabin on a dropped column is
  never served (no statement names it).
- **BW-S4** landed at `d0134573`. Review (`critics-developer`) checked every claim against
  `bc42a011` and fixed eight: BW-Q9's protection overstated for a `DROP`
  (raised BW-Q11, a `[quiet-wrong]` known gap); AL4 and the manual saying
  assertions refuse every `ALTER` (`ADD` is not refused); a dropped name
  "free at once" (it is free once the drop commits); `null.md` §5 and
  `client-manual.md` still naming `ADD COLUMN` refused; a bare `decimal`
  listed as fitting a slot; the arity refusals' byte; the known-gaps
  commit. Not changed: the `DROP COLUMN` restrict and rename-assertion
  refusals go out as bare `ERR` text, as `DROP TABLE`'s restrict does -
  kept consistent with it; `ALTER COLUMN ... TYPE` answering
  `InvalidArgument` rather than `NotImplemented` predates BW.
- **BW-S3** landed at `bc42a011`. Review (`critics-developer`) found BW-Q9 (`[quiet-wrong]`,
  adopted (a), built: `NormalizeInsertRows`, KWP passing its NULL tuple key
  through) and proposed seven cuts. Applied: one `ResolveAlterTarget` for
  every ALTER form; `ADD`'s statement-level refusals before the `X`;
  `AddColumn` propagating `ColumnWidth`'s error, running
  `CheckDeclarableColumnTypes`, and reading `sys.tables` before its hold;
  the parser comment. BW-Q9's own review fixed a multi-row refusal losing
  its `(row k)`, raised BW-Q10 (adopted (a), built: `DescribedShape`,
  checked behind the chunk's `IX`), and folded the normalization's
  trigger into `NormalizeInsertRows`; the arity refusals carry the row's
  byte. **Rejected**: `ColumnRowOf` as `StatusOr` - the
  refusals render as `DispatchOutcome` text that CREATE TABLE's tests pin,
  and converting them is churn with no behaviour gained. The review's
  finding that BW-R5 as written admitted an unmarked column as nullable was
  CLA's own, applied before it (BW-R5's revision).
- **BW-S5** - `origin/main` merged at `93ebc2ff` (`v3.2.0-21-g93ebc2ff`;
  74b0a3dd's retirement of "pk" for "tuple key" taken into every line BW
  added), suite 3839/3839 on the merged tree. **The overhead measurement
  was not run**: held by W2 until the operator directs it. BW is not
  pushed, merged to `main` or tagged.
