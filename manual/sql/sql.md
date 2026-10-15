# KDS SQL Reference

Everything a client can say to KDS in SQL, verified against the parser
(`src/parser/parser.cpp`, `include/kds/parser/ast.hpp`), the lexer
(`src/parser/lexer.cpp`), the dispatcher (`src/server/command_dispatcher.cpp`)
and the step compiler (`src/exec/step_compiler.cpp`) as of 2026-08-09.
Where a statement below is marked *verified*, the production was read in code;
nothing here is taken from a spec alone.

The server speaks a newline text protocol: one statement per line, one reply
per line (multi-row replies are a single response with embedded newlines).
Statements are case-insensitive. A trailing `;` is accepted and ignored.

The full statement list (the `Statement` variant in `ast.hpp`, plus the
dispatcher's own commands):

| Class | Statements |
|---|---|
| DDL | `CREATE TABLE`, `CREATE NAMESPACE` / `DROP NAMESPACE`, `CREATE INDEX` / `DROP INDEX`, `CREATE ASSERTION` / `DROP ASSERTION`, `CREATE CABIN` / `DROP CABIN` |
| DML | `INSERT`, `UPDATE`, `DELETE` |
| Query | `SELECT` (joins, subqueries, `GROUP BY`, aggregates), `ANALYZE <select>` |
| Transactions | `BEGIN`/`START`, `COMMIT`, `ROLLBACK`/`ABORT`, `SET ISOLATION LEVEL` |
| Introspection | `SHOW META/TABLES/NAMESPACES/PAGE/PATTERNS/ACCESS/BUDGET/CABINS/INDEXES/FKEYS/ASSERTIONS/RELAYOUT`, `DESCRIBE` |
| Session | `PING`, `SYNC`, `STOP` |

`DROP TABLE` and `ALTER TABLE` exist and are **catalog-only** (see their
sections): a drop tombstones the oid and its pages are reclaimed after it
commits, a rename edits a label, and `ADD`/`DROP COLUMN` change a column's
catalog row and no stored row. The catalog is append-only apart
from those, `DROP CABIN`, `DROP INDEX` and `DROP ASSERTION`.

---

## 1. DDL

### CREATE NAMESPACE / DROP NAMESPACE

```sql
CREATE NAMESPACE ledger;
CREATE TABLE ledger.accounts (id int64, owner varchar) BTREE;
DROP NAMESPACE ledger;              -- only when nothing is in it
```

Grammar (verified):

```
CREATE NAMESPACE <name>;
DROP   NAMESPACE <name>;
```

**A namespace groups relations that belong together, and nothing else.**
It is not a separate storage area, it does not scope a transaction, it does
not scope a *name* — see "Qualified names" below — and since v3.0.0's M3 it
decides nothing about cores: every statement runs on the core its session
is on, whatever namespaces its relations are in.

**Put relations that are joined, foreign-keyed or read together in one
namespace.** That is the grouping the engine records for a future use
(the relation's declared affinity) and the grouping a reader of the catalog
sees; today it changes no answer and no cost.

- `sys` and `public` are reserved and cannot be created or dropped. `sys`
  names the catalog views; `public` is where an unqualified `CREATE TABLE`
  lands.
- `DROP NAMESPACE` is **RESTRICT**: a namespace holding any relation is
  refused, and the refusal names one of the relations that blocked it.
  There is no `CASCADE`.
- A namespace is never created by being mentioned. `CREATE TABLE
  ordrs.line` when there is no `ordrs` is an error with the byte, not a new
  namespace — so a typo cannot become a silent second namespace.

```
> CREATE NAMESPACE ledger
CREATED NAMESPACE ledger oid=4001
> SHOW NAMESPACES
sys public ledger
> DROP NAMESPACE ledger
DROPPED NAMESPACE ledger oid=4001
```

### Qualified names

Any relation name may be written `<namespace>.<relation>`, everywhere a
relation is named: `FROM`, every `JOIN`, inside a subquery, `INSERT INTO`,
`UPDATE`, `DELETE FROM`, `REFERENCES`, `ALTER TABLE`, `DROP TABLE`,
`CREATE INDEX ON`, `CREATE CABIN ON`, `CREATE ASSERTION ON`, `DESCRIBE`
and `SHOW RELAYOUT`.

**A relation's name is unique across the whole instance, and a qualifier
does not change which relation a name reaches.** Two namespaces cannot hold
two relations called `orders`. What a qualifier does depends on where it is
written:

- at `CREATE TABLE ns.t` it **chooses** the namespace;
- everywhere else it is **checked**. A qualifier that disagrees with where
  the relation actually lives is refused, and the refusal says where it is:

```
> SELECT * FROM trading.accounts
ERR no relation 'trading.accounts'; 'accounts' is in namespace 'ledger' (byte 14)
```

An unqualified name means exactly what it always meant, so every statement
written before namespaces existed still means the same thing.

### CREATE TABLE

```sql
CREATE TABLE accounts (id int64, owner varchar, balance decimal(10,2)) BTREE;
CREATE TABLE trades   (id int64, sym varchar, qty int64) BTREE;
```

Grammar (verified):

```
CREATE TABLE [<namespace>.]<name>
                    ( <col> <type> [NULL | NOT NULL] [REFERENCES <parent>]
                     [CABIN | CABIN AUTO | NO CABIN]
                     [, ...] ) [HEAP | BTREE] [EXPLICIT] [SPARE <n>];
```

- **Column names are unique ignoring case.** `(id int64, a int64, A int64)`
  is refused at the later name's byte: `ERR column 'A' repeats column 'a'
  (names are unique ignoring case) (byte <n>)`, and so is an exact repeat.
  A name is at most 63 bytes; a longer one is refused at its byte.
  **Case is ignored everywhere a column is named**: `SELECT A` reads `a`,
  and so do a `WHERE`, an `ORDER BY` and a `SET` value; `UPDATE ... SET A`,
  an `INSERT` column list, `DROP COLUMN A`, `RENAME COLUMN A TO b`,
  `CREATE INDEX`, `CABIN`, `ASSERTION` and `TRANSITION` resolve `A` to `a`
  the same way. `SET ID = ...` on a relation whose tuple key is `id` is the
  tuple key, and refused. A relation's own name is not covered by this.

- **`SPARE <n>` reserves `n` column slots** (0 to 64, default 0) for
  `ALTER TABLE ... ADD COLUMN` (below). Each slot is 16 bytes plus one null
  bit in every row, used or not, so the row size is fixed for the life of
  the relation and an `ADD` writes no row. A relation created without it
  has no slot and cannot take an added column. `sys.tables` shows
  `spare_slots` and `spare_free`. `SPARE` is an identifier, not a reserved
  word; a count past 64 or a second `SPARE` is refused with its byte.

- **`HEAP` is suspended and the storage clause defaults to `BTREE`.** Writing
  `HEAP` is refused: `ERR HEAP storage is suspended (SUS-1) and no new heap
  relation is created (byte <n>); BTREE is the default and every existing
  heap relation still mounts and serves`. The suspension is a refusal at
  *creation* only — a database that already holds heap relations opens,
  reads, writes and recovers them exactly as before, and nothing about them
  is deprecated.
- `BTREE` gives the relation a clustered B+ tree on the tuple key; only a
  btree relation can carry a secondary index or be an FK parent. With `HEAP`
  suspended those are no longer distinctions between two choices you can
  make: they describe the one storage form a new relation takes.
- **There is no key mode.** Every relation takes a caller-named tuple key
  or issues one when `INSERT` omits it, so nothing about keys is declared
  here. `EXPLICIT` is still accepted and **does nothing** — it states what is
  true of every relation, so old scripts keep working. `ASSIGNED` is refused:
  `ERR the ASSIGNED key mode no longer exists (byte <n>) - every relation
  takes a caller-supplied tuple key or issues one when INSERT omits it, so
  there is nothing for this word to select`.
- **The words are order-free**: `BTREE EXPLICIT` and `EXPLICIT BTREE` are the
  same statement. Each *category* may appear at most once, and a repeat is
  refused with its byte (`ERR CREATE TABLE takes one storage word - HEAP or
  BTREE - and this is the second (byte <n>)`) rather than resolved
  last-one-wins: `HEAP BTREE` names two different relations.
- The words are ordinary **identifiers**, not reserved (see the appendix) — a
  column may still be named `explicit` or `btree`.
- **`EXPLICIT` does not change the storage default.** `CREATE TABLE t (...)`
  and `CREATE TABLE t (...) EXPLICIT` are both `BTREE`, as is every
  unqualified `CREATE TABLE` while `HEAP` is suspended.
- At least one column is required.
- The optional per-column suffixes come in a **fixed order**: nullability,
  then `REFERENCES`, then the cabin policy. Two optional suffixes in either
  order would be a grammar with two spellings of one declaration.
- **Columns are `NOT NULL` by default — the opposite of standard SQL.**
  A column that says nothing refuses `NULL`; writing `NULL` after the type
  opts it in, and writing `NOT NULL` spells the default out loud for
  standard-minded schemas. This is deliberate (null.md D1): a
  nullable-by-default rule would have silently reinterpreted every
  `CREATE TABLE` written before the feature existed. Consequences: the
  first column (the tuple key) may not be `NULL`; neither may any index
  key or covered column (D2 — `IS NULL` answers by scan); a nullable
  column costs its relation a tail bitmap byte per 8 nullable columns,
  and a relation with none costs nothing.

**The Keystone contract (tuple key).** The first column *is* the tuple
key:

- It must be declared with an integer type (`catalog::CheckKeystoneColumn`,
  checked at CREATE TABLE).
- **Where its values come from is decided per `INSERT`, not per relation.**
  Supply a value in the tuple key's position and that value *is* the key; omit it
  and the engine issues one. Both work on every relation, and one statement
  may mix the two across its rows.
- **An issued key clears every key you named before it.** The relation
  keeps a high-water mark, `DESCRIBE`'s `next_id`, which every named key at
  or above it advances past itself. So naming 500 and then omitting the key
  gives 501. An issued key that meets a key another session named at the
  same moment is drawn again; after 8 such draws the `INSERT` is refused
  `AlreadyExists` (see the error table). Issued keys ascend in the order
  they are issued.
- **A named key may be any key not already bound, in any order, on a
  `BTREE` relation.** There a row is placed where its key sorts, so after `tuple_key = 100`,
  `tuple_key = 99` is placed, and a multi-row `VALUES` may name its keys in any
  order. A named key is refused because of its value for **exactly two
  reasons**, each naming the key's byte in the statement, and nothing is
  written (one known defect adds a third: inserting, rolling back and
  re-naming one key about 600 times with the same indexed value and a new
  covered value can be refused `AlreadyExists` by a covering index, and
  `TxnConflict` after that):
  - **duplicate**, `AlreadyExists`: a row with this key exists, live
    (`ERR duplicate tuple key <k> already present at page <p> slot <s>
    (byte <n>)`) or deleted (`ERR duplicate tuple key <k>: a row with
    this key was deleted; PURGE frees its key (page <p> slot <s>)
    (byte <n>)`). A key named twice in one statement is a duplicate at its
    second row;
  - **exhausted**, `OutOfRange`: the key is outside `[1, 2^48 - 1]`
    (`ERR tuple key <k> is outside the Keystone id space [1,
    281474976710655] (byte <n>)`) — zero, negative, too large, or a literal
    too long for 64 bits.

  **A key whose insert rolled back is free**, and naming it again places
  it. **A key whose row was committed stays bound**, even after `DELETE`,
  **until a `PURGE` frees it** (§3, PURGE); then naming it again places it. A named key meeting another session's
  uncommitted insert of the same key waits for that session to commit
  (then `AlreadyExists`) or roll back (then placed); one meeting a
  committed row that another session is updating or deleting is
  `AlreadyExists` at once. A named key can also wait on another session's
  open write over a range that covers it. Every `BTREE` page's rows stay in
  key order however keys arrive, so `ORDER BY <tuple key>` costs nothing (see
  ORDER BY). A `HEAP` relation (none can be created since SUS-1) still
  refuses a named key below its high-water mark, `OutOfRange`.
- It cannot be `UPDATE`d: it is the tuple's identity, not a field of it.
  Naming a key at insert and changing one afterwards are unrelated
  permissions; only the first is granted.
- It cannot carry a Cabin — a cabin on the tuple key is refused with the
  position of the `CABIN` keyword.
- Ids are unique, never gapless. A supplied id that is already in use — 
  including one belonging to a row that has been `DELETE`d, whose slot is
  not reclaimed — is refused as a duplicate.

**Per-column cabin policy** (verified in `ParseCreateTable`):

```sql
sym varchar CABIN          -- a Cabin is created now (n=1)
sym varchar CABIN AUTO     -- the engine may decide later (named and stored; nothing consumes it yet)
sym varchar NO CABIN       -- never, by any route
sym varchar                -- unset, which reads as auto
```

Unset and `AUTO` are stored as different values so the catalog keeps the
difference between "nothing was said" and "the engine may decide".
A column declared `AUTO` currently behaves exactly as an undeclared one — the
promotion pipeline that would consume the policy is specified
(`physical-optimizer.md` Part II) but not wired to DDL.

**Foreign keys** (built and enforced, FK-M1..FK-M5):

```sql
CREATE TABLE orders (id int64, account_id int64 REFERENCES accounts, amount int64) BTREE;
```

- No parent column is written **and none may be**: the parent side is always
  the referenced relation's Keystone id. `REFERENCES accounts(id)` is refused
  (`Unsupported`) — it would spell out the only thing it could mean.
- A heap-clustered parent is refused at declaration: a parent needs a tuple key
  index for the check not to be a scan.
- v1 is RESTRICT-only and fail-fast. INSERT/UPDATE of an fk column checks the
  parent exists; DELETE of a parent checks for live children and stops at the
  first one. Violations answer `ERR FK_VIOLATION retryable=0 ...`.
- **A parent being written is waited for, not refused**: the check holds the
  statement until that transaction commits or rolls back and then runs it
  again, wherever in the instance the writer is. A `DELETE` that meets a
  child row being written still answers `ERR TXN_CONFLICT retryable=1 ...`,
  and so does a wait that runs out of time.
- `SHOW FKEYS` lists declarations. CASCADE / SET NULL do not exist (FK-M6,
  out of v1 by decision F2).

### DROP TABLE (built 2026-08-10, DT1-DT7 / DT01-DT05)

```sql
DROP TABLE <name>
```

**Catalog-scoped**: the relation becomes unreachable, its name frees
immediately, and its oid is tombstoned — never reissued — so stale
advisory structures keyed by the dead oid can never mis-attribute.

**Its pages are reclaimed after the drop commits**: the tree, every index
tree, the var-heap chain and the anchor go back to the allocator, and a
later `CREATE` reuses their ids. The file never shrinks. Two conditions
gate the free:
- **Durability.** No page is freed before the log's redo start has moved
  past the drop's commit. That is at the next checkpoint or two, and at a
  mount with more than one core it waits for every core's first
  checkpoint.
- **Readers.** No page is freed while a statement that could still read the
  relation is running.

A drop that rolls back frees nothing. A crash in the middle of a reclaim is
finished by the next mount. `SHOW META` reports `pages_allocated`,
`pages_freed`, `pages_reused` and `reclaim_pending`, and the pages no
structure of the relation reaches - a failed split's, a failed
`CREATE INDEX`'s - stay allocated.

- **It waits for a statement already reading or writing the relation**
  (2026-09-09). A scan that is already walking finishes against a live
  schema instead of failing part way, and an open transaction that has
  written a row of the relation is waited for until it commits or rolls
  back. Two things follow for a client: a `DROP TABLE` can now sit for a
  while, and one that waits more than 11 seconds is refused with a
  retryable conflict rather than dropping the table - so a drop issued
  against a busy relation may need retrying. A read that *starts* after the
  drop has begun is not waited for and still sees the drop early, which is
  the "not isolated" entry below, unchanged.
- **RESTRICT, named**: a foreign key referencing the relation as parent
  blocks (declared-level — an empty child still blocks; drop the child
  first), and so does an assertion on it (drop it first).
- **Dependents drop with it**: its indexes, Cabins (catalog rows and
  in-memory sets), columns and child-side foreign keys.
- Patterns and trails naming the dead relation die quietly (advisory);
  `sys.access_stats` ghosts stay, keyed by an oid that cannot return.
- **Atomic inside a transaction as of 2026-08-16**: `ROLLBACK` restores
  the relation and its rows. Unlogged still, so a committed drop is no
  more crash-durable than any catalog write.
- **Not isolated, and this one is visible to users.** Between the `DROP`
  and the `COMMIT`/`ROLLBACK` that resolves it, *other* sessions already
  see the relation as gone; if the transaction rolls back, it returns.
  The rows are never at risk — data pages are untouched — but the schema
  change is early. Isolating it needs undo records for catalog rows,
  which is not built (`docs/spec/ddl-transactional.md` §5a).

### ALTER TABLE (renames built 2026-08-10, AL1-AL9; `ADD`/`DROP COLUMN` BW, v3.2.0)

```sql
ALTER TABLE <t> RENAME TO <new>
ALTER TABLE <t> RENAME COLUMN <old> TO <new>
ALTER TABLE <t> ADD [COLUMN] <col> <type> NULL
ALTER TABLE <t> DROP [COLUMN] <col>
```

Every form is **catalog-only**: it changes catalog rows and no stored row,
so it costs the same on ten rows and on ten billion. A rename changes a
label. Identity is the oid, so nothing dangles — foreign keys still
enforce, indexes and Cabins still serve, all with no re-declaration.

- **A new column name - `ADD COLUMN`'s or `RENAME COLUMN`'s - may not equal
  another live column's ignoring case**: `ERR the relation already has a
  column named 'a' (names are unique ignoring case)`, with the new name's
  byte. A column may be renamed to its own case variant (`RENAME COLUMN a
  TO A`), but not to the name it already has, and a dropped column's name
  is free in any case.

- **`ADD COLUMN`** places the column in the relation's lowest free slot
  (`CREATE TABLE ... SPARE n`). Every row that predates it reads `NULL`
  there.
  - **It must say `NULL`.** A column is `NOT NULL` unless declared `NULL`,
    and the rows that predate it have no value, so `ADD COLUMN c int64` and
    `... NOT NULL` are refused `NOT_IMPLEMENTED`; there is no `DEFAULT`.
  - **One slot, at most 16 bytes.** Every integer, `bool`, `date`,
    `timestamp` and `decimal(p, s)` up to 38 digits fits, as does
    `char(N)` with N ≤ 16. A string column is `varchar(16)`, which
    holds a value of any length: up to 13 bytes are stored in the row and a
    longer value spills to the var-heap. A wider `varchar(N)` or `char(N)`
    is refused, naming the slot, and so is a bare `varchar` unless the
    instance's `kds.inline_cell_width` is 16 (it is 64 by default).
  - `REFERENCES` and a cabin clause are refused (declare a foreign key at
    `CREATE TABLE`; `CREATE CABIN` after the `ADD`). Because the column is
    nullable, it cannot be indexed or carry a Transition.
  - A relation with no free slot - none reserved, or all taken - refuses
    `ADD` `NOT_IMPLEMENTED`: adding the column would rewrite every row.
- **`DROP COLUMN`** flags the column. Its name is free for an `ADD` or a
  `RENAME` once the drop commits (at once for an `ADD` in the dropping
  transaction), and it leaves `SELECT *`, `DESCRIBE`, `sys.columns`
  and positional `VALUES`, which count the remaining columns. Its bytes stay
  in every row and its slot is never reused: nothing is reclaimed (no
  cleaning is built).
  - Refused for the tuple key, and for a column an index keys or covers,
    a foreign key names, a Transition governs or a Cabin is on, each named.
    Any column of a relation that carries an assertion is refused too,
    naming the assertion.
- **`ADD` and `DROP COLUMN` are transactional**: each takes the relation
  exclusively, waiting for open writers and positioned readers (refused
  `TXN_CONFLICT` after 1 s), and `ROLLBACK` - or a crash before the commit
  - undoes it. Another session's *read* can see the undecided column set:
  a column missing from `SELECT *`, or an added one reading `NULL`, before
  the commit. It never sees a wrong value.
- Type changes and widening answer `NOT_IMPLEMENTED` with a position: they
  rewrite every row (invariant 13). Widening is permanently out by the
  tagged-cell design.
- **Assertions RESTRICT both renames** (and `DROP COLUMN`, above): the
  stored canon is the declaration's text, so a rename of a relation
  carrying one refuses, naming it — drop, rename, re-declare. `ADD COLUMN`
  is not refused.
- **Patterns are allowed to die**: statements against the old name stop
  matching stored patterns and trails, which costs replay speed, never a
  row; new-name traffic re-registers on the ordinary path.
- The tuple key column renames like any other (identity is the Keystone word,
  not the spelling); a `sys.*` relation refuses; a taken name is
  `AlreadyExists`.
- A rename is logged and durable but **not transactional**: `ROLLBACK` does
  not undo it.

### CREATE INDEX / DROP INDEX (built, IX01-IX16)

```sql
CREATE INDEX ix_owner ON accounts(owner) COVERING (balance);
DROP INDEX ix_owner;
```

- Multi-column keys: up to `kMaxIndexKeyColumns` (4) key columns and
  `kMaxIndexCoveredColumns` (8) covered columns. The caps refuse, never
  truncate, and the parser names the byte of the offending column.
- Only a `BTREE` relation may carry an index (an entry's payload is the tuple key).
- `CREATE UNIQUE INDEX` is refused (`NOT_IMPLEMENTED`): enforcing uniqueness
  would make the index a constraint that can fail a write, and v1 is a read
  accelerator that cannot fail a write for a reason of its own (IX11).
- Maintenance is append-only; DELETE does not touch an index; there is no
  index-only scan (no visibility witness exists outside the tuple), so
  `COVERING` buys avoided base descents, nothing more.
- `CREATE INDEX` and `DROP INDEX` **wait** for every open write to the
  relation and for a statement already reading it, and hold new writes
  until they commit or roll back - on any core. Like `DROP TABLE`, a wait
  that reaches the lock family's 11 s bound is refused `TXN_CONFLICT`.
- `SHOW INDEXES` lists them.

### CREATE ASSERTION / DROP ASSERTION (built and **enforcing**, AST01-AST10)

```sql
CREATE ASSERTION seat_cap ON bookings GROUP BY (flight_no) CHECK COUNT(*) <= 180;
CREATE ASSERTION risk_cap ON positions GROUP BY (desk) CHECK SUM(exposure) < 1000000;
DROP ASSERTION seat_cap;
```

Grammar (verified — the grammar *is* the supported predicate class, AS2):

```
CREATE ASSERTION <name> ON <table> GROUP BY (<col> [, ...])
    CHECK { COUNT(*) | SUM(<col>) } { < | <= } <non-negative integer literal>
```

- `GROUP BY` is mandatory and parenthesized (unlike SELECT's). No cap on the
  list length.
- Only `COUNT(*)` and `SUM(<column>)`; only `<` and `<=`. Both operators map
  to one enforced ceiling (`< N` means `<= N-1`), computed once at parse.
- Refused with a position: `>` / `>=` (a lower bound would need DELETE and
  decreasing-UPDATE checks, which is exactly what v1 excludes), `=`
  (enforcing it needs the lower-bound half; reading it as `<=` would enforce
  something other than what was written — AS11 as revised 2026-08-08),
  `MIN`/`MAX`/`AVG` bounds, `COUNT(<column>)`, `DISTINCT`,
  `DEFERRABLE` / `NOT DEFERRABLE` / `INITIALLY ...` / `NOT VALID`.
- `!=` and a negative or non-literal bound are `InvalidArgument` (simply
  wrong, not a pending decision). `COUNT(*) <= 0` / `< 1` are refused as
  degenerate — a group exists only because it holds a row.
- **Status, plainly: declared, validated at CREATE, and enforced on the
  write paths.** A prior description of this feature as "enforcing nothing"
  is stale — since AST07 (2026-08-09) INSERT is admission-checked before the
  row id is allocated, UPDATE per its delta, and a violation answers
  `ERR ASSERTION_VIOLATION retryable=0 ...` naming the group and the
  *enforced* ceiling. CREATE scans the relation synchronously and fails on
  already-violating data; an unsettled (in-flight-written) relation is
  refused retryably. ~~The one caveat: after a restart the surviving catalog
  row honestly reports `enforcing=0` until recovery (which does not exist)
  can replay the directory~~ — **stale, corrected 2026-08-26**: recovery has
  replayed the directory at every mount since 2026-08-12 (RC07), so an
  assertion enforces immediately after a restart; `SHOW ASSERTIONS` still
  reports `enforcing=0` for the one whose directory could *not* be rebuilt,
  which is the honest remainder rather than the ordinary case.
- **On a multi-core instance an assertion is one constraint for the
  instance.** `CREATE ASSERTION` builds on the core it runs on, and a write
  arriving on any core is checked against the same aggregate; two sessions
  on two cores inserting into a group with room for one row get one row
  and one `ASSERTION_VIOLATION`. `SHOW ASSERTIONS` answers the same from
  every core. `CREATE ASSERTION`'s success line carries no
  `built_by_core=` and `SHOW ASSERTIONS` no `enforced_by_core=` field any
  more. A `CREATE ASSERTION` waits for every open write to the relation,
  and a write arriving while it builds waits for it to finish - on any
  core - so the new constraint counts every row.

### CREATE TRANSITION / DROP TRANSITION (built and **enforcing**, BV, v3.2.0)

```sql
CREATE TRANSITION order_status ON orders(status)
  ON INSERT ('NEW')
  ON UPDATE ('NEW' => 'CHECKED', 'CHECKED' => 'FINALIZED',
             'NEW' => 'CANCELLED', 'CHECKED' => 'CANCELLED')
  ON DELETE FROM ('NEW', 'CANCELLED');
DROP TRANSITION order_status;
```

A Transition is a column's allowed state changes. It is checked per row,
from the row alone (`docs/spec/transition.md`).

What each clause allows:

- **`ON INSERT`**: the states a row may be inserted in.
- **`ON UPDATE`**: the `old => new` moves an UPDATE may make. Assigning the
  current value again is allowed. An UPDATE that does not assign the column
  is not checked.
- **`ON DELETE FROM`**: the states a row may be deleted from. Leave it out
  and every DELETE is allowed.

`ON INSERT` and `ON UPDATE` are required, in that order. Together they make
a path property hold without any history being stored: with the
declaration above, a row reaches `FINALIZED` only through `CHECKED`.

Rules:

- **The state column.** It is `NOT NULL`, of an integer type, `char(N)` or
  `varchar(N)`, and not the tuple key. A column carries at most one
  Transition. A state is an integer or a string literal matching the
  column, and every state must be reachable from an `ON INSERT` state.
  `=>` is written without a space.
- **Existing rows.** `CREATE` scans the relation and refuses if a row's
  state is not named in the declaration. It does not check how a row got to
  its state.
- **Violations.** A violating write answers
  `ERR TRANSITION_VIOLATION retryable=0 transition '<name>' on column '<c>': ...`
  and, like every failed write, fails its transaction.
- **When `CREATE`/`DROP` take effect.** They wait for open writes to the
  relation and commit at the statement, never with the session's
  transaction. `SHOW TRANSITIONS` lists them.
- **`PURGE`** is refused on a relation that carries a Transition. A freed
  tuple key could restart a row's history in an initial state, so a
  governed relation's deleted keys stay bound until `DROP TRANSITION`.
- **`DROP TABLE`** removes the relation's Transitions in autocommit. Inside
  an explicit transaction it is refused while one stands: `DROP TRANSITION`
  first.
- **`ALTER TABLE RENAME COLUMN`** keeps the Transition. It names the column
  by position.

### CREATE CABIN / DROP CABIN (built, CB01-CB11; expressions and predicates since AQ)

```sql
CREATE CABIN ON accounts(owner);
CREATE CABIN ON trades (DATE(ts));
CREATE CABIN ON orders (state = 'closed' AND amount > 1000);
DROP CABIN ON trades (DATE(ts));
```

- A Cabin is named by what it is on — its table and its key — never by a
  name of its own. The key is a column, an **immutable** function of columns
  (`DATE(ts)`; `NOW()` is refused), or a **conjunction** of comparisons of a
  column or an immutable call with a literal (`=`, `<`, `<=`, `>`, `>=`, `!=`,
  `IS [NOT] NULL`). Refused, at their byte: `OR`, `NOT`, `BETWEEN` (write its
  two bounds), `IN`, `LIKE`, a subquery, a comparison of two columns, `=
  NULL`, a key reading only the tuple key or no column, a second Cabin on the
  same key (conjuncts in any order are one key), a comma-separated column
  list (multi-column keys are not built).
- Refused on the tuple key and on a `NO CABIN` column, whoever asks — as a
  column Cabin or as any input of a key.
- **What is served.** `DATE(ts) = '2026-10-01'` from a `DATE(ts)` Cabin; a
  range of `ts` bounded on both sides (`BETWEEN`, or `>=`/`>` with `<=`/`<`)
  from the sets of every day it touches, when every one of those days has
  been observed and they number no more than `cabin_max_values`; and a
  statement whose WHERE carries **every** conjunct of a predicate Cabin,
  written exactly - `amount >= 1001` is not `amount > 1000`. Every other
  statement walks; no answer changes either way. A column renamed is a key
  kept (keys name columns by position); a column a key reads cannot be
  dropped until the Cabin is.
- The expression and predicate forms wait for open writes to the relation,
  as `CREATE INDEX` does.
- `SHOW CABINS` lists them, with each one's `shape` (`column`, `expression`,
  `predicate`) and `key`. Entry sets are memory-resident and do not survive
  a restart; only the catalog row persists.
- `SHOW CABINS` no longer prints `scope_declines`: every walk covers the
  whole relation, so no probe falls through for its scope.

### CREATE PATTERN / DROP PATTERN — withdrawn

These statements existed and were removed on 2026-08-31. **A pattern is a
case the engine identifies by fingerprint and tracks by statistics**; there
is no way to declare one, and nothing replaces the grammar. `SHOW PATTERNS`
lists what traffic registered, each row identified by its hex `fetch_id`:
the statement's shape with its select list left out, so `SELECT a ...` and
`SELECT COUNT(*) ...` over the same `FROM ... WHERE` share one row and one
trail. `ANALYZE` prints a statement's `pattern_id` (its whole shape) and its
`fetch_id`, the second being the one to look for in `SHOW PATTERNS`. A row
marked `stale=v1` was recorded by a build before this keying, is listed
under the `pattern_id=` it was keyed by, and is never used. A row's
`uses=` and `last_seen=` read 0: executing a pattern writes its trail and
nothing to its row, so a repeated read makes no logged write.

`CREATE PATTERN ...` now answers the ordinary refusal for an unknown
`CREATE` target, and `$name` parameters are refused wherever they are
written — the token stays reserved for a future extended protocol's named
binds.

---

## 2. Types

Registered in `sys.types` (verified in `src/catalog/catalog.cpp`):

| Declared name | Storage | Notes |
|---|---|---|
| `int8`, `int16`, `int32`, `int64` | 1/2/4/8 bytes | first column must be one of the integer types |
| `uint64` | 8 bytes | full range preserved via the literal's digit text |
| `bool` | 1 byte | |
| `varchar` | one tagged cell of `inline_cell_width` (64) bytes | longer values spill to the var-heap; > 8144 bytes is `NotImplemented` |
| `varchar(N)` | one tagged cell of **N** bytes | `N` is this column's `inline_cell_width`, `16 ≤ N ≤ 4096`; a value longer than `N − 3` spills — `N` is a width, **not** a length cap |
| `char` | 1 byte | `char` alone is `char(1)` |
| `char(N)` | N bytes | zero-padded; a longer value is refused, and a value containing a NUL byte is refused |
| `date` | int32, days since 1970-01-01 | literal: `'2026-08-09'` |
| `timestamp` | int64, microseconds since the epoch, UTC | literal: `'2026-08-09 12:00:00'` |
| `decimal(p,s)`, p ≤ 18 | int64 unscaled + scale | TY01-TY11; both arguments **mandatory** |
| `decimal(p,s)`, 19 ≤ p ≤ 38 (DESCRIBE shows it as `decimal128(p,s)`, a name only: declare it as `decimal(p,s)`) | int128 | a separate type, never a widening of the 8-byte one |

- **A bare `decimal` is refused, never defaulted** — a default scale is a
  silent decision about someone's money, and only the parser can still tell
  "said nothing" from "said zero". A bare `char` and a bare `varchar` are
  *not* refused: `char` is the standard's `char(1)`, and a bare `varchar`
  takes the instance's `inline_cell_width` — neither decides what a stored
  value means.
- **`varchar(N)` declares a cell width, not a maximum length.** This is a
  deliberate divergence from standard SQL. `N` is the number of bytes the
  column's cell occupies in every row; a value longer than `N − 3` is stored
  out of line in the var-heap rather than refused, and reads back
  identically either way. The only length refusal is 8144 bytes, one
  var-heap page. Choose `N` to keep your common values inline, not to
  constrain what may be stored.
- **`char(N)` pads with zeros, not spaces**, and compares byte-wise over the
  unpadded value — so `'ab'` and `'ab '` are different values. There is no
  `PAD SPACE` collation. A `char` value may not contain a NUL byte, because
  the type reads back to the first one; use `varchar` for arbitrary bytes.
- **Widening is refused in every spelling.** `ALTER TABLE … TYPE
  varchar(M)` would rewrite every row of the relation, since a row's size is
  a schema constant (invariant 13).
- **`float` is refused at CREATE TABLE.** The fixed-length tuple rule
  (invariant 13) requires every column to have a decided on-disk width, and
  nothing has settled a float encoding. The type is registered in
  `sys.types` but `RowLayout::Build` answers `Unsupported` for it.
  `decimal` was refused for the same reason and no longer is — the types
  work settled its encoding.
- No arithmetic, no expressions, no cross-scale or cross-width decimal
  comparison (refused at compile, never rescaled — rescaling either drops
  digits or invents them).
- Literal coercion is a **compile-time** act through one function
  (`exec::CoerceLiteralToColumn`): `WHERE d = '2026-02-30'` fails with a
  positioned compile error, not row-by-row.
- A bare numeric literal (`12.34`) is sugar for the quoted string of its
  spelling — same AST, same fingerprint.

---

## 3. DML

### INSERT (verified; multi-row built 2026-08-10, bulkinsert.md T1)

```sql
-- Omit the tuple key and the engine issues one
INSERT INTO accounts VALUES ('alice', 120.50);
INSERT INTO accounts VALUES ('bob', 10), ('carol', 20), ('dave', 30);

-- Or name it: the tuple key comes first, any key not already bound, in any order
INSERT INTO trades VALUES (5000, 'AAPL', 10);
INSERT INTO trades VALUES (5100, 'MSFT', 3), (6200, 'NVDA', 7);

-- And one statement may do both. Row 2's key is issued above 7000
INSERT INTO trades VALUES (7000, 'AMD', 1), ('INTC', 2);

-- Or name the columns, in any order: a nullable column left out is NULL,
-- the key left out (or NULL) is issued
INSERT INTO trades (qty, sym) VALUES (4, 'TSLA'), (9, 'ORCL');
INSERT INTO trades (sym, id, qty) VALUES ('IBM', 8000, 2);
```

- Grammar: `INSERT INTO <table> [(<col> [, ...])] VALUES (<val> [, ...]) [, (<val> [, ...])]*`
  — up to `max_insert_rows` rows per statement (config key, default 1024, a
  refusal never a truncation).
- **With a column list, each value goes to the column it names** (BW-S6),
  so no `ALTER TABLE` can shift it. Each row has exactly one value per
  listed name. Names match ignoring case (`A` is `a`, so `(a, A)` lists
  one column twice); an unknown or dropped name, and a name listed twice,
  are refused at its byte. The tuple key may be listed anywhere or left out: left out, or
  `NULL`, it is issued. A column declared `NULL` that the list leaves out
  is written NULL; a `NOT NULL` one left out refuses the statement at the
  list's `(` - there is no `DEFAULT`. Every column `ADD COLUMN` creates is
  nullable, so a list written before an `ADD` stays valid after it.
  Everything below about a named key, the refusals, atomicity and the
  pipeline holds for a listed row unchanged; the arity rules below are the
  positional form's.
- **The arity is per row, and there are two of them.** Supply `n` values on
  an `n`-column relation and the first is the tuple key; supply `n - 1` and
  the engine issues one. Both are legal on every relation and a statement may
  mix them. The two counts cannot be confused — a positional `INSERT` omits
  no body column individually — so a wrong
  length is refused naming both:
  `ERR expected 3 value(s) including tuple key column 'id', or 2 to have it issued; got 4 (byte <n>)`.
- **A `NULL` tuple key asks for an issued one** (as a KWP load row's
  does): `INSERT INTO trades VALUES (NULL, 'AMD', 1)` is the issued-key row
  at full width.
- **After `ALTER TABLE ... ADD` or `DROP COLUMN`, every row is full width.**
  On a relation whose column set has changed, `n - 1` values are refused,
  because after an `ADD` that is the length of a row written for the old
  columns with its key named, and admitting it would put every value one
  column over. Name the key, or write `NULL` for it:
  `ERR expected 3 value(s), one per column, with tuple key column 'id' NULL to have it issued; got 2 (byte <n>). The relation's columns were changed by ALTER TABLE, ...`.
  The count does not catch every stale statement: after a `DROP`, a row
  written for the old columns with its key left out has the new full
  length and is admitted with its first value as the key. Re-read a
  positional `INSERT` written before an `ALTER` - or write it with a column
  list, which no column change can shift.
- **A named tuple key must be an integer literal** in `[1, 2^48 - 1]` — 0 is
  reserved for "unset". A non-integer is `InvalidArgument`; an integer
  outside the range is `OutOfRange` (exhausted). Both carry the offending
  token's byte and are checked before anything is placed, so a refused row
  burns no id (BI9).
- **A named tuple key is refused as a duplicate when its key is bound** (see the
  Keystone contract): on a `BTREE` relation the clustered descent lands on
  the only leaf that could hold the key and names a used one `AlreadyExists`.
  A `DELETE`d row still holds its key until a `PURGE` frees it, so a
  deleted tuple key cannot be re-supplied before one; a key whose insert rolled
  back, or a purged one, can.
- Replies: `INSERTED oid=<o> id=<n> page=<p> slot=<s>` for one row, where
  `slot` is the slot at placement — a later insert into the same page can
  move it; `INSERTED oid=<o> rows=<n> first_id=<f> last_id=<l>` for
  several (no id contiguity promised). `first_id`/`last_id` are the
  **first and last rows in statement order**, not the lowest and highest.
- **A statement may name its keys in any order** on a `BTREE` relation;
  each row is placed where its key sorts. A row naming a key an earlier row
  of the statement placed is a duplicate, refused with its ordinal, and the
  statement fails atomically (below).
- **Every bulk row runs the full single-row pipeline, in order** (BI2):
  FK check, assertion admission (row k sees rows 1..k-1's reservations),
  id, encode, placement, Cabin witness, index maintenance, WAL. Bulk
  amortizes parse and round trips, never authority.
- **Atomic** (BI4): any per-row refusal fails the whole statement with the
  row ordinal appended to the message — ` (row 3)` — and inserts nothing.
  A pre-id refusal burns no id; an aborted statement burns exactly its
  placed rows' ids (BI9, accepted and documented).
- Row count is not part of the pattern (BI5): a 500-row insert
  fingerprints as the 1-row insert on the same relation. A column list is
  part of it: two orders of one list are two patterns in
  `sys.statement_stats`.
- Values are integers, strings (`'...'`, no quote escaping), bare numerics
  and `NULL` — which stores only into a column declared `NULL` and is
  refused by name (with the byte) for the default `NOT NULL`. A stored
  NULL is distinct from `0` and from `''`, renders as the token `NULL`,
  is skipped by aggregates, satisfies a foreign key vacuously, and is
  found by `IS NULL`, never by `=`.
- INSERT is the one fully WAL-logged statement path; durability class comes
  from the `durability` config key (see §5). For bulk load, `relaxed` is
  the documented recommendation (BI8).

### UPDATE (verified)

```sql
UPDATE accounts SET balance = 99.00 WHERE id = 42;
```

- Grammar: `UPDATE <table> SET <col> = <value expression> [, ...] [WHERE ...]`,
  and `SET (<col>, ...) = (<value>, ...)`.
- The WHERE is the same production SELECT uses, subqueries included.
- **A SET value is an expression** (BJ). `UPDATE accounts SET balance =
  balance + 25.00 WHERE id = 42` is one statement and one lost-update-free
  step, where a client used to `SELECT`, add and `UPDATE`. What a value may
  be, and how each is typed, is `docs/spec/types.md` §3.2b; in short:
  - **Operators**: `+ - * / %` over numbers, `||` over text, unary `+ -`;
    `date + n`, `date - n`, `date - date`; the comparisons `= != < <= > >=`,
    `AND`, `OR`, `NOT` and `IS [NOT] NULL`, which give a `bool` a `bool` column
    takes. Precedence is PostgreSQL's. Write inequality `!=`; `<>` is not
    lexed.
  - **Forms**: `CASE` (simple and searched), `COALESCE`, `NULLIF`,
    `GREATEST`, `LEAST`, `CAST(x AS t)` and `x::t`.
  - **Functions**: `ABS SIGN CEIL FLOOR ROUND TRUNC MOD`, `UPPER LOWER LENGTH
    SUBSTRING TRIM LTRIM RTRIM POSITION(a IN b) REPLACE LPAD RPAD CONCAT`,
    `NOW()`, `CURRENT_DATE`, `DATE(ts)`. Strings are bytes.
  - **A scalar subquery**, `SET v = v + (SELECT SUM(v) FROM t)`: run once,
    before the first row, so every row is raised by the sum the table had
    before the statement. Unlike a WHERE subquery it does not see the rows
    the statement has already written. One column, at most one row (more is
    an error even when the WHERE matches nothing), no row is NULL, and one
    that reads the row being updated is refused.
  - **Every SET expression reads the row as it was before the statement**:
    `SET a = b, b = a` swaps. A column named twice is refused.
  - **No implicit conversion**: an operator's operands, a `CASE`'s results
    and the value assigned are each one exact type, and a literal takes the
    type its neighbour or its column has. `int32 + int64` needs a `CAST`.
    A decimal result of another scale is narrowed by `ROUND(x, s)` or `CAST`,
    which round half away from zero; a quotient is `decimal` with the
    dividend's scale and up to 16 more places. An integer literal divided
    by a decimal takes the decimal's type: `2 / price` is `2.00 / price`.
  - **Arithmetic is checked in its own type at every step**: a result
    outside it is `OutOfRange` at that row, naming the operator's byte, the
    expression's first byte and the row's key, and a division by zero is
    `InvalidArgument`. NULL propagates (`SET n = n + 1` over a NULL leaves
    it NULL and counts the row); a NULL into a `NOT NULL` column is refused
    at the row.
  - **What is not there**: `IN`, `BETWEEN`, `LIKE`, `EXISTS`, `IS TRUE`,
    boolean literals, a correlated or catalog-view subquery, an expression
    in `INSERT ... VALUES` or a select list, and an expression that reads a
    column assigned to a foreign-key column (a constant one is checked).
- **The tuple key cannot be assigned**, and the
  refusal is `Unsupported` with the column's byte — understood and
  declined, not a feature awaiting work (K2). This holds whether the key was
  named or issued: naming a key at insert and changing one afterwards are
  unrelated permissions. The id names the tuple in the clustered
  tree, in every index and Cabin entry and in every recorded trail, so an
  in-place change would retarget all of them at once. An unknown SET
  target is `InvalidArgument`, also with its byte; both are decided at
  compile, before any storage is touched, so a refused UPDATE writes
  nothing.
- An UPDATE never migrates a tuple.
- A statement whose expression does not type is refused at compile whether or
  not a row matches, naming both types and the byte. A bare literal keeps its
  per-row check, so `SET int_col = 'abc'` over a WHERE that matches nothing
  still answers `UPDATED 0`.
- Reply: `UPDATED <n>`.

### DELETE (verified — it exists)

```sql
DELETE FROM accounts WHERE balance = 0;
```

A prior finding that KDS had no SQL DELETE is **no longer true**: `DELETE
FROM <table> [WHERE ...]` parses (`ParseDelete`, `DeleteStmt` in the AST)
and executes. It arrived with the transaction work (T01-T14). Semantics:

- A DELETE is a **delete-mark**, never a physical removal — the tuple's
  bytes stay for readers whose snapshot predates the deleter. `PURGE`
  (below) is the one statement that removes a deleted row and frees its
  key.
- No column list, nothing to assign; the WHERE nests subqueries exactly as
  SELECT's does. An empty WHERE marks every row.
- DELETE deliberately does not touch Cabins or secondary indexes (removal is
  forbidden — an older snapshot may still match), and has no assertion
  check (upper bounds only make DELETE check-free), though it does write the
  departure entry that keeps assertion group headers truthful.

---

### PURGE (built 2026-10-08, BH)

```sql
DELETE FROM accounts WHERE id = 5;
PURGE FROM accounts WHERE id = 5;          -- PURGED 1
INSERT INTO accounts VALUES (5, 0);        -- placed: the key is free again
PURGE FROM accounts WHERE id BETWEEN 100 AND 199;
```

`PURGE FROM <table> WHERE <tuple key comparisons>` removes the deleted rows of a
tuple key window and **frees their keys**, so an `INSERT` that names one
is placed as if the key had never been used. It is the one exception to
"a committed key stays bound" (the Keystone contract).

- **The window.** Every conjunct compares the tuple key with a
  non-negative integer literal — `=`, `<`, `<=`, `>`, `>=`, `BETWEEN a AND
  b` — and they are ANDed into one key range. Any other conjunct is refused,
  not ignored, because ignoring one would free more than was asked
  (`id > 0 AND id != 5` would free 5).
- **What is freed.** A key whose row a committed `DELETE` removed. A live
  key is passed over in a wider window; named alone (`id = 5`, `id BETWEEN
  5 AND 5`) it is refused. A key never placed, rolled back, or already
  purged purges nothing and is no error, so a `PURGE` can always be run
  again.
- **It waits for older readers.** A row is freed only once no open
  snapshot can still read it, so a `PURGE` right after its `DELETE` may
  wait for a `REPEATABLE READ` transaction that began before the delete,
  or for an undecided writer of the key. It polls for up to 1 s, then is
  refused `TXN_CONFLICT retryable=1` naming the key, having purged nothing.
  While it waits it holds nothing, so DDL on the relation is not delayed.
- **Autocommit only, `BTREE` only, admin only.** Inside `BEGIN` it is
  refused `NOT_IMPLEMENTED` without poisoning the transaction; a system or
  heap relation is `UNSUPPORTED`; it needs the admin role.
- **Reply:** `PURGED <n>`, the keys freed; `rows_affected` on the wire. It
  is durable as any commit is, under the session's durability class.
- **Partial outcomes.** The whole window is judged before anything is
  written, then each key is freed whole, in key order. A crash or an I/O
  refusal in the middle leaves the window freed up to some key and
  untouched after it; the refusal says how many keys it freed, and running
  the statement again finishes the window.
- **What it leaves.** Secondary indexes, Cabins and Waystone trails keep
  their entries for the freed key; every read re-checks them against the
  row now there, so a freed and re-inserted key reads as the new row,
  once. Assertion counts are already right: `DELETE` removed the row's
  contribution, and `PURGE` adds no second removal. The engine never
  issues a freed key on its own; only an `INSERT` that names it takes it.
- **An identity note.** A system that recorded a key from outside the
  engine must treat a purged key as a new identity: the purge is in the
  log to say when it became one.

## 4. SELECT

```sql
SELECT a.owner, o.amount
FROM accounts AS a JOIN orders AS o ON o.account_id = a.id
WHERE a.balance > 100 AND o.amount BETWEEN 10 AND 500;
```

### Projection

- `SELECT *` — single relation only. Over more than one relation it is
  refused: which columns `*` means, and in what order, would depend on a
  join order the client is promised is its own.
- Explicit lists: `SELECT x, a.y FROM ...` — columns, qualified or bare.
  No expressions, no `AS` column aliases, no literals in the select list.

### FROM and joins

- `FROM <table> [AS <alias>]` — the bare-alias form (`FROM t a`) is
  deliberately not accepted (a typo'd keyword would read as an alias).
- `sys.<name>` reaches the catalog views (`sys.tables` etc.). `sys` is the
  only schema; `public.t` is not accepted.
- Joins: `JOIN <table> [AS <alias>] ON <rel.col> = <rel.col>` chains.
  Inner equi-join only, one equality per ON, both sides qualified.
  **Written order is execution order** — the FROM list is a client contract
  and nothing reorders it. The statement is the plan.
- Every relation's binding must be distinct; `FROM t JOIN t` is refused, and
  `FROM t AS a JOIN t AS b` is how a self-join is spelled.
- A join keyed on a relation's **tuple key** executes as a tuple key probe
  (lookup class). Any other equi-join is *not refused* — it compiles to a
  scan (or index/Cabin/filter scan) of that relation with the ON equality
  evaluated as a residual filter per row (verified in the step compiler's
  access-kind assignment). Slow is legal; wrong is not.

### WHERE

`WHERE` takes conditions combined with `AND`, `OR` and `NOT`, grouped by
parentheses (since BU-S1). `OR` binds loosest, then `AND`, then `NOT`:
`a = 1 OR b = 2 AND c = 3` is `a = 1 OR (b = 2 AND c = 3)`. Each condition
is one of (verified — the `PredicateKind` enum):

| Form | Kind | Notes |
|---|---|---|
| `col op value` | kCompareValue | `op` ∈ `=`, `!=`, `<`, `<=`, `>`, `>=` |
| `col op other.col` | kCompareValue (column RHS) | how correlation is written |
| `col op (SELECT ...)` | kCompareSubquery | scalar; >1 row is a runtime `CardinalityViolation` |
| `col IN (SELECT ...)` | kInSubquery | |
| `col NOT IN (SELECT ...)` | kNotInSubquery | tri-state, not `!IN`: a NULL in the result with no match, or a NULL probe against a non-empty result, is unknown and admits no row; against an empty result it is true |
| `EXISTS (SELECT ...)` | kExists | no column on the left |
| `NOT EXISTS (SELECT ...)` | kNotExists | |
| `col BETWEEN low AND high` | kBetween | inclusive both ends; lowers to `>= low AND <= high`, the range on the step is only a hint |
| `col IS NULL` / `col IS NOT NULL` | kCompareValue | the one spelling that finds NULLs — answered from the stored bitmap |
| `col IN (v, ...)` | kBoolTree (one item: kCompareValue) | since BU-S2: `col = v1 OR col = v2 ...`; at most 4,096 items |
| `col NOT IN (v, ...)` | kCompareValue `!=`, one per item | since BU-S2: `col != v1 AND col != v2 ...`, flattened into the AND-list where it stands at the top; a NULL item is unknown, so `NOT IN (1, NULL)` keeps no row |
| `col NOT BETWEEN low AND high` | kBoolTree | since BU-S2: `NOT (col BETWEEN low AND high)` |
| `col [NOT] LIKE 'pattern'` | kBoolTree | since BU-S2: `char`/`varchar` only; `%` any run, `_` one byte, case-sensitive bytes; no `ESCAPE` |
| `value op col` | kCompareValue | since BU-S2: a literal on the left is the mirrored comparison, `100 < amount` is `amount > 100` |

- **What `OR` and `NOT` cost and what they cannot hold.** A top-level
  `AND` - with or without parentheses - is executed exactly as it always
  was, so `WHERE id = 5 AND (a = 1 OR b = 2)` is still a keyed descent on
  `id`. A group holding `OR` or `NOT` is a filter on the rows something
  else found and **never a key, an index range or a Cabin probe**:
  `WHERE id = 1 OR id = 2` walks the relation. It is three-valued like
  every comparison: `NOT (a = 1)` does not keep a row whose `a` is NULL,
  and `a = 1 OR a != 1` does not either. A subquery predicate under `OR`
  or `NOT` is answered since BU-S3, in the same three-valued logic - `NOT
  (k IN (SELECT ...))` keeps no row the subquery's NULL leaves unknown -
  and is never run once ahead of the statement the way a top-level
  uncorrelated `EXISTS` is: `EXISTS (SELECT ...) OR a = 1` keeps the
  `a = 1` rows when the subquery is empty. `OR` over a `sys.*` view
  answers `NOT_IMPLEMENTED` with its byte; a tree nested deeper than 64 levels or holding more than
  4,096 conditions answers `UNSUPPORTED`. `NOT EXISTS (...)` and
  `col NOT IN (SELECT ...)` are the forms above and keep their meaning.
  `PURGE`'s window still refuses every conjunct that is not a tuple key
  comparison, `OR` included.
- Subqueries are **predicate-position only**, correlated included, nested to
  at most `kMaxSubqueryDepth = 4` below the outermost block. The fifth level
  is refused with a position.
- `IN (1, 2, 3)` — a value list — exists since BU-S2. Each item is coerced
  to the column's type at compile, with its byte. A list of more than one
  item, like `LIKE` and `NOT BETWEEN`, is a filter and never a key: `WHERE
  id IN (1, 2)` walks the relation (`IN` of one item is `=`, keys and all;
  `NOT IN` is a list of `!=`, which no access path keys on either).
- **No type is converted implicitly** (types.md §3.1a). A literal must be
  written in the compared column's own spelling, and a statement that
  breaks the rule is refused at compile with the literal's byte - whether
  or not a row would have matched, and before an `UPDATE` or `DELETE`
  touches anything:
  - an integer column takes an integer literal: `n = 20`, never `n = '20'`
    or `n = 20.0`;
  - `char` and `varchar` take a string literal: `s = '5'`, never `s = 5`;
  - `bool` takes `0` or `1`;
  - a `date`, `timestamp` or `decimal` is written as a string (`'2026-10-01'`,
    `'12.34'`); a date or timestamp also takes its epoch integer, and a
    decimal an integer, which are those types' own forms and not
    conversions.

  Two columns - in a `WHERE`, an `ON`, or an outer column against the value
  of `IN (SELECT ...)` or a scalar subquery - compare only inside one
  family: integers of any width and signedness, compared by value (an
  `int32` -1 is less than a `uint64` 5); decimals of the same width and
  scale; `char` and `varchar`, as the text they hold; a date with a date;
  a timestamp with a timestamp. Any other pair is refused: `date = timestamp`
  would compare epoch days with epoch microseconds.
- **Comparisons are three-valued** (null.md §4): a relational operator
  with a NULL operand is unknown, and `WHERE` keeps only true — so
  `col = NULL` matches nothing, ever (the standard's trap, kept on
  purpose); `IS NULL` is the spelling that finds them.
- A tuple key equality (`WHERE id = 42`) executes as a keyed descent; a tuple key
  window bounded at both ends (`BETWEEN`, `>=`/`<=`) as a range walk that stops
  past its high bound and, on a btree, seeks to its low one. A window with a strict end
  (`id >= a AND id < b`) is the same walk, but only when no other access path
  (an index, a Cabin, a filter scan, a join build) applies; one bounded at a single
  end is a walk from the head. A non-tuple-key equality is served
  by a secondary index, then a Cabin, then a filter scan, in that fixed
  preference order — selection is a function of shape and catalog, never of
  data statistics.

### Aggregation (built, AG01-AG10)

```sql
SELECT flight_no, COUNT(*), SUM(amount) FROM bookings GROUP BY flight_no;
SELECT COUNT(DISTINCT owner) FROM accounts;
SELECT AVG(balance) FROM accounts;
```

- Functions: `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`; `DISTINCT` accepted inside
  any of them (`MIN`/`MAX` treat it as the standard's no-op). `COUNT(*)` is
  the one star form; `COUNT(DISTINCT *)` is refused.
- `GROUP BY <col> [, ...]` — column references only, unparenthesized (the
  assertion grammar's parenthesized list is a different production).
- `SELECT b FROM t GROUP BY b` is aggregated (one row per group) even with
  no function.
- Refused with a position: `HAVING`; `ORDER BY` over aggregated output; a
  bare column not in `GROUP BY` (there is no "any row" mode); `SELECT *`
  with `GROUP BY`; an aggregate or `GROUP BY` inside a subquery (AG8,
  permanent for v1); aggregation over `sys.*` views (AG12).
- `SUM` is checked int64 — overflow fails the statement, never wraps.
  `AVG` over an integer column is refused at compile (it would invent
  digits); `AVG(DECIMAL(p,s))` answers at exactly scale `s`, half-even.
- Caps fail the statement rather than truncate (`aggregate_max_groups`,
  ratified at 65,536).

### Ordering and pagination (V09 2026-08-10; general `ORDER BY` 2026-08-11)

```sql
SELECT <list> FROM ... [WHERE ...]
    [ORDER BY <col> [ASC|DESC] [, <col> [ASC|DESC]]...]
    [LIMIT <n>] [OFFSET <m>]
```

Each clause is independently optional, in that order (`OFFSET 5 LIMIT 10`
is trailing garbage — dialect compatibility is a non-goal). `LIMIT`
without `ORDER BY` is well-defined here in a way general SQL cannot
promise: emission order is already a client contract — written order
across steps, tuple key order within one — so `LIMIT n OFFSET m` means rows
`[m, m+n)` of the reply the unlimited statement gives.

- The counts are non-negative integer literals; a value past uint64
  refuses rather than wraps; `LIMIT 0` is legal and answers no rows.
- The counts are **slots**: `LIMIT 10` and `LIMIT 20` are one pattern,
  two instances, so a limited statement fingerprints like any other.
- **NULLs sort largest** (null.md D3): `ASC` puts them last, `DESC`
  first — PostgreSQL's rule, one fixed default per direction. There is no
  `NULLS FIRST` / `NULLS LAST` grammar to override it.
- `ORDER BY` takes **any column or columns**, of any relation the
  statement names, each `ASC` (the default) or `DESC`, up to eight keys.
  Earlier keys decide; later keys break their ties. A key need not be
  projected: `SELECT a FROM t ORDER BY b` is legal.
- **Rows the clause does not distinguish keep the order they would have
  had without it** — ordering refines the emission contract rather than
  replacing it, so a tie is never resolved arbitrarily and never varies
  between runs.
- **Ordering by the tuple key ascending is free**, and only that form:
  it names the order the engine already emits, so the compiler drops the
  clause and the statement pays nothing. `ANALYZE` shows this as the
  absence of a `sort` line. Every other order — including `ORDER BY <tuple key>
  DESC` — is an output sort, which is not free; see the two notes below.
- **That order holds on every `BTREE` page, at any number of cores.** A
  row is placed at the position its key sorts to, whatever order keys
  arrive in (see the Keystone contract), so every page's rows sit in key
  order and the clause has nothing to do. A volume an older engine wrote,
  whose pages need not, does not mount (format version 20 refuses 19).
- **A sorted statement's `LIMIT` bounds what you receive, not what the
  engine reads.** The sort must see every qualifying row before it knows
  which comes first, so unlike an unsorted or tuple-key-ordered `LIMIT`, it
  cannot stop the scan early; `ANALYZE`'s `examined=` shows the full
  count. What the `LIMIT` does bound is memory — only `offset + limit`
  rows are held. For deep pagination over an ordered column, prefer keyset
  form.
- A sort holding more than `sort_max_rows` rows **fails the statement**
  naming the key. It never returns a partial ordering, and it never
  spills to disk. Adding a `LIMIT` is usually the answer: it caps what is
  held at `offset + limit` regardless of the relation's size.
- Ordering over aggregated output (`GROUP BY`) is still refused with a
  byte position.
- The tail is refused over an aggregated statement (groups emit in fold
  order, which is not a contract) and inside a subquery, each with a
  byte position.
- **Over a catalog view (`sys.*`), `LIMIT` and `OFFSET` apply and
  `ORDER BY` is refused with a byte position.** A view's rows are
  materialized by the catalog's readers rather than walked out of pages,
  so the quota applies to them unchanged — `LIMIT n OFFSET m` is rows
  `[m, m+n)` of the view's unlimited reply, as everywhere else — but the
  output sort resolves its keys against a schema a view does not have.
  **Neither clause saves any reading there**: the view is built in full
  before the quota sees a row, so a view's `LIMIT` bounds what you receive
  and what gets rendered, and the `O(offset)` note below is a statement
  about relations, not about `sys.*`.
- `OFFSET` costs what it skips — qualifying rows are examined and
  discarded, O(offset), since no head seek exists. For deep pagination
  prefer keyset form: `WHERE id > <last seen> LIMIT n`. It walks from the
  head of the relation until it passes `<last seen>`; bounding the window
  above too (`WHERE id > <last seen> AND id <= <last seen + span>`) seeks to it
  when nothing else serves the statement (see the access-path note under WHERE).
- Nothing is reserved: columns and tables may still be named `limit`,
  `offset`, `order`, `asc`, `desc`.

KWP/1's portal suspension (`docs/spec/protocol.md`) remains the designed
protocol-level pagination surface; only its frame codec is built.

### ANALYZE

`ANALYZE <select>` is a dispatcher prefix, not a parser keyword: the
remainder runs the ordinary SELECT path with statistics collection on — same
parse, same compile, same fingerprint — and reports the run that actually
happened (per-step access kinds, rows touched, pages fetched,
`index_filtered`, etc.). SELECT-class only; a write statement cannot be
wrapped.

`examined=` counts every row a step filtered, including the rows a scan
rejected on their stored bytes before building them: a simple
`column <op> literal` condition is tested on the row as stored first, and
only a row that passes is decoded and checked in full. The rows returned,
their order, `examined=` and `sel=` are the same either way; the server's
`scan_kernel = off` turns the early test off, for comparison
(`docs/spec/parser-v2.md` §7).

---

## 5. Transactions

```
BEGIN [TRANSACTION | WORK] [ISOLATION LEVEL {READ COMMITTED | REPEATABLE READ}]
COMMIT [TRANSACTION | WORK]
ROLLBACK [TRANSACTION | WORK]   (ABORT is a synonym)
SET ISOLATION LEVEL <name>
```

Verified in `HandleBegin` / `HandleCommit` / `HandleRollback` /
`HandleSetIsolation`:

- `START` is accepted for `BEGIN`; `TRANSACTION` / `WORK` are noise words.
- Replies: `BEGIN trx_id=<n> isolation=<level> durability=<class>`,
  `COMMIT trx_id=<n>`, `ROLLBACK trx_id=<n>`, `SET isolation=<level>`,
  `SET durability=<class>`.
- **`SET DURABILITY {STRICT|GROUP|RELAXED}`** (also `D1`/`D2`/`D3`) binds
  this connection's *next* transaction, never the open one — a transaction
  that has already logged writes under D3's window cannot be given D1's
  promise retroactively, because the window it spent is spent.
  `BEGIN [TRANSACTION] [ISOLATION LEVEL <level>] [DURABILITY <class>]`
  overrides for one transaction, and the two clauses may be written in
  either order. Three rungs: the server's configured class, then the
  session's, then the transaction's.
- Exactly two isolation levels: `READ COMMITTED` (default, read view per
  statement) and `REPEATABLE READ` (read view per transaction).
  `SERIALIZABLE` is refused as out of scope, not open, and `READ UNCOMMITTED`
  is refused as not supported - both by name, with their reasons. The precedence chain
  is config key → `SET ISOLATION LEVEL` (session, next transaction) →
  `BEGIN ISOLATION LEVEL` (this transaction).
- No nested transactions, no savepoints: a second `BEGIN` is an error.
  `SET ISOLATION LEVEL` inside an open transaction is an error.
- **A transaction commits or rolls back whole.** `SAVEPOINT`, `RELEASE`,
  `ROLLBACK TO ...`, `COMMIT AND CHAIN` and any other word after `COMMIT`,
  `ROLLBACK` or `ABORT` (beyond `TRANSACTION`/`WORK`) are refused
  `UNSUPPORTED` with the word's byte. A refused `ROLLBACK ...` inside a
  transaction rolls nothing back and leaves the transaction failed, so
  the work it meant to undo cannot be committed: only `ROLLBACK` ends it.
  A refused `COMMIT ...` commits nothing and leaves the transaction open.
- Write conflicts are first-updater-wins, and a writer meeting an
  undecided holder **waits for it** rather than being refused
  (`docs/spec/txn.md` §5):
  `ERR TXN_CONFLICT retryable=1 row id=<n> was written by transaction <n>`.
- A failed statement inside an explicit transaction **poisons the session**:
  only `ROLLBACK`/`ABORT`/`SYNC`/`STOP`/`PING` are admitted (a whitelist)
  until the client rolls back. An autocommit statement is its own
  transaction and unwinds fully.
- Closing a connection with a transaction open rolls it back.
- **`CREATE TABLE`, `DROP TABLE` and `CREATE INDEX` are transactional**
  (2026-08-16): inside an explicit transaction they are rolled back by
  `ROLLBACK`. For `CREATE TABLE` and `CREATE INDEX`, nothing is visible
  to another session until it commits — by any route: `SELECT`,
  `INSERT`, `UPDATE`, `DELETE`, `DESCRIBE`, `SHOW TABLES` or
  `SHOW INDEXES`.
- **`DROP TABLE` rolls back, but is *not* isolated**: other sessions see
  it before it commits (see its own entry). Since 2026-09-09 it does wait
  for a statement already reading the relation, and for an open writer of
  it - a narrower promise than isolation, and its own entry says so.
- **`DROP INDEX` is transactional too** (2026-08-18): inside an explicit
  transaction it is rolled back, and it is isolated — index maintenance
  keeps writing entries for an index whose drop has not committed, so a
  `ROLLBACK` leaves the index whole. Since AT-S5e a `DROP INDEX` or
  `CREATE INDEX` holds the relation while it is undecided, so a write to
  it from any core waits for the DDL to commit or roll back and then runs
  against the index list it left. It was refused inside a transaction
  between 2026-08-16 and 2026-08-18, for the wrong result that fix
  closes. Two limits on all of the above, both deliberate:
  - **Not crash-durable.** Catalog writes are still unlogged and the
    catalog is not recovered, so a committed `CREATE TABLE` survives a
    crash only if its page reached the device. "Transactional" here means
    atomic and isolated, not durable.
  - **A second transaction cannot create the same name** while the first
    is open: it is refused with `EXISTS`, even though it cannot see the
    relation. The refusal is deliberate — the alternative is two
    relations claiming one name — and it can be spurious if the first
    transaction rolls back.
- `ALTER TABLE ... ADD COLUMN` and `DROP COLUMN` are transactional too
  (above). Other DDL (`ALTER TABLE`'s renames, cabins, patterns,
  assertions) is **not** transactional and is not rolled back.

**Durability class.** Three classes exist — `strict` (D1, fsync per commit),
`group` (D2, default), `relaxed` (D3) — but the class is set by the
`durability` **config key**, instance-wide. The per-transaction durability
field is a KWP/1 protocol feature (`docs/spec/protocol.md`), and KWP is not
wired: **no SQL or text-protocol spelling selects a durability class per
transaction today.** Specified, not built.

---

## 6. Introspection (verified — the complete list)

Dispatcher commands, not parser statements:

| Command | Answers |
|---|---|
| `SHOW META` | instance metadata (superblock, format version, config), the WAL's own counters - including `wal_mean_group_batch`, how many commits this core's fsyncs are amortised over, where `1.000` means every commit paid its own device sync - plus what the last mount's recovery did — records scanned, transactions committed and rolled back, per-phase timings, and `catalog_recovered=0`, which is a standing statement that DDL is unlogged rather than a number that will change; on core 0, the log's recycling (`wal_first_segment`, `wal_segments_removed`, `wal_remove_failures`); on every core, the segment roll's totals (`wal_append_lsn`, `wal_rolls`, `wal_roll_*`), the same values everywhere; and `wal_stopped=1` on any core once a failed log write has stopped every write until a restart (`docs/spec/client-manual.md` has the field list) |
| `SHOW TABLES` | the relation list |
| `SHOW NAMESPACES` | the namespace list, `sys` and `public` first. Names are the **SQL spellings**, which is what a statement writes |
| `SHOW PAGE <id> [VALUES]` | one page's header and slots; `VALUES` hex-dumps tuple payloads |
| `SHOW PATTERNS` | `sys.patterns` — registered patterns, ids, Waystone state |
| `SHOW ACCESS` | `sys.access_stats` — one row per access shape `(kind, rel, columns)` |
| `SHOW BUDGET` | per-relation Keystone id consumption, `warning=`/`exhausted=` counts |
| `SHOW CABINS` | Cabins and their per-column policies |
| `SHOW INDEXES` | secondary indexes, key and covered columns |
| `SHOW FKEYS` | declared foreign keys |
| `SHOW ASSERTIONS` | assertions with `enforcing=` and live counters (checks/violations/reserved/aborted) |
| `SHOW TRANSITIONS` | Transitions: id, name, relation, governed column's position, and the declaration verbatim |
| `SHOW RELAYOUT [<table>]` | the physical optimizer's shadow report (candidate moves, each blocked by a named gate) |
| `DESCRIBE <table>` (`DESC`) | columns, types, `clustered_type=`, `next_id`, `ids_issued`/`ids_remaining`/`budget_used`. A tuple key column reports `autoincrement=if-omitted`, every other column `no` |
| `PING` | `PONG` |
| `SYNC` | forces log + store durability: `OK synced` |
| `STOP` | closes the connection: `OK bye` |

An unknown target answers `ERR unknown SHOW target`. The `sys.*` views
(`sys.tables`, `sys.types`, ...) are also readable through ordinary
`SELECT ... FROM sys.<name>` — projection, `WHERE`, `LIMIT` and `OFFSET`
all apply — but they are not aggregatable (AG12), not joinable, and not
sortable: `ORDER BY` over a view is refused with a byte position, because
a view has no schema for the sort to resolve its keys against. A column
may be written qualified (`FROM sys.tables AS t ... WHERE t.oid = 100`),
and a qualifier naming no relation in the statement is refused in the
`WHERE` exactly as it is in the projection. A view's integers compare
**unsigned**, because every one of them is stored as a `uint64` and
`sys.patterns.fetch_id` really does use the full 64-bit range. A view's
`WHERE` follows the relation's literal rule (types.md §3.1a): every integer
column is a `uint64` and every other a `varchar`, so `name = 5`, `oid =
'x'`, a negative literal and one past 2^64 - 1 are each refused with the
literal's byte, before any row is read.

There is also a legacy non-SQL form: `CREATE TABLE <name>` with **no column
list** routes to a pre-SQL handler (the dispatcher routes on the presence
of `(`). It answers `EXISTS oid=<n>` if the name is taken; otherwise the
catalog refuses the empty schema — `ERR relation has no columns; the first
column is the mandatory Keystone tuple key` — so the form can no longer
create anything.

---

## 7. What KDS refuses, and why

Every refusal below is deliberate — understood and declined, with the byte
position of the offending token, not a syntax error. `InvalidArgument` means
"simply wrong"; "understood and declined" is **two** codes since 2026-08-31,
and a client reads which one to learn whether waiting for a newer server is
worth anything:

- **`ERR NOT_IMPLEMENTED retryable=0 …`** — the design admits this form and
  this release has not built it. Feature-detect and try again against a later
  server. Most of this section is this one: CTEs, derived tables, outer
  joins, `HAVING`, `UNIQUE` indexes, `ALTER TABLE`'s row-rewriting forms (a type change, an `ADD` with no free slot), a
  value wider than one var-heap page.
- **`ERR UNSUPPORTED retryable=0 …`** — this engine's architecture cannot
  admit the form, so no later server answers it and the statement must be
  rewritten: updating a tuple key, `float`, comparing decimals of different
  width or scale, one relation named twice in a `FROM` list without distinct
  aliases, a `$name` parameter on this protocol.

Neither is retryable, and the message and its byte position are the same as
they were before the split.

- **No CTEs.** `WITH` is answered by name at the statement head: a CTE is
  table-position nesting, and the inner result would have to become a
  relation with a schema. *"common table expressions (WITH) are not
  supported; subqueries are allowed in predicate position only."*
- **No derived tables.** `FROM (SELECT ...)` is refused by the relation
  reference itself: materializing an inner result breaks tuple-key-direct probing
  into the next step and puts a temp relation in the storage layer.
- **No window functions, no expressions.** The grammar has no expression
  tree outside a `SET` value — no arithmetic in a predicate (`OR`, `NOT`
  and parentheses do exist in a WHERE since BU-S1). Outside the five aggregates
  there are two scalar functions, and they may appear only as one side of a
  WHERE comparison: `DATE(timestamp)`, the UTC day an instant falls on, and
  `NOW()`, the statement's instant (the same value for every call in one
  statement). `WHERE DATE(ts) = '2026-10-02'` and `WHERE ts < NOW()` work;
  a function in the select list, `ORDER BY`, `GROUP BY` or `HAVING`, a
  nested call or a literal argument answers `NOT_IMPLEMENTED` with its
  position, and an unknown name answers like an unknown column. A function
  comparison filters rows the statement found some other way - it never
  becomes a key, an index range or a Cabin probe. A smaller grammar every
  statement of which executes predictably was chosen over a larger one with
  unpredictable corners.
- **No outer joins.** `LEFT`/`RIGHT`/`FULL`/`OUTER` are reserved keywords
  that answer `NOT_IMPLEMENTED` with their own position, so the grammar will
  not shift if they ever land.
- **The join rule.** KDS supports inner equi-join chains, executed in
  written order, probing by tuple key where the ON allows it. There is no
  merge join and no plan search **— the query is the plan.** Written order
  is a client contract; decorrelation rewrites are forbidden, not merely
  unimplemented. A stable plan is what lets `pattern_id` name a plan and
  Waystone trust it.

  Since 2026-08-20 one join *does* hash, and the distinction is worth
  stating precisely because the sentence above used to deny it outright.
  When a join's inner side has no index, no Cabin, and no filter of its
  own that already narrows it, the executor builds an in-memory map of
  the inner rows the statement's own filters admit — **once per
  statement**, on the first outer row's read — and every later outer row
  looks its key up instead of reading again. That is a hash build in
  mechanism. It is **not** a plan choice: which relation drives, which
  joins what, and the order rows come back in are all exactly what they
  were, the map is discarded when the statement ends, and nothing is
  declared, persisted or reused. `ANALYZE` marks the step `build` and
  reports `inner_built`, and `join_build_max_rows = 0` turns it off
  without changing a single reply. What stays forbidden is what the rule
  was always about: no algorithm gets *chosen*, and no statement is
  rewritten.
- **No `HAVING`, and no `ORDER BY` over aggregated output.** `HAVING`
  answers with "filter before the fold with WHERE, or filter the result
  client-side". Ordering *aggregated* output stays refused deliberately:
  it is one half of a decision to be made with `HAVING`, and answering
  half of it would settle the rest by accident. Ordinary statements do
  have a full `ORDER BY` — any columns, `ASC`/`DESC` — since 2026-08-11;
  see Ordering and pagination.
- **No cross-dialect shims.** No `SERIAL`, no `AUTO_INCREMENT` (every tuple key is
  already both when the `INSERT` omits it, and deliberately neither when the
  `INSERT` names it), no `IF NOT EXISTS`, no `public.` schema
  qualifier, no
  bare-alias `FROM t a`, no quote-escaping in string literals. Each would be
  a second spelling of something that already has one, and every spelling
  must fingerprint identically forever.
- **No `CREATE INDEX`-by-another-name.** There is exactly one index DDL and
  it is honest about its class: an index is "a Cabin that observed
  everything", append-only, verified at read. `UNIQUE` is refused because it
  would turn a read accelerator into a constraint that can fail writes.
- **No file shrinking, and no data-moving `ALTER`.** A dropped relation's
  pages are reclaimed and reused (DT1), but the file never shrinks and no
  `VACUUM` moves live rows. `ALTER TABLE` renames and adds or drops a
  column in place; it never rewrites a row (AL1, BW). The
  RESTRICT predicates (assertions, referencing fkeys) now have their
  callers in both.
- **No `SELECT *` across a join**, no duplicate FROM bindings, no
  aggregates in subqueries, no subquery nesting past depth 4 — each refused
  with the exact byte.

---

## 8. Error surfaces

Reply shape (verified, `server::ErrorReply` — a compatibility surface, not a
diagnostic): `ERR <TOKEN> retryable=<b> <message>` for the seven codes a client
library switches on, `ERR <message>` for everything else. The seven are
`TXN_CONFLICT`, `FK_VIOLATION`, `ASSERTION_VIOLATION`, `UNKNOWN_OUTCOME`,
`TRANSITION_VIOLATION` (since BV, 2026-10-14), and — since 2026-08-31 —
`UNSUPPORTED` and `NOT_IMPLEMENTED`, which every refusal
in §7 below now carries and which both used to reach a client as a bare
`ERR <message>`.

| Situation | Exact surface |
|---|---|
| Write conflict (first-updater-wins), write to another core's relation, FK check meets in-flight writer, CREATE ASSERTION on unsettled relation, a name (table, namespace, index) held by another transaction's uncommitted DROP | `ERR TXN_CONFLICT retryable=1 row id=<n> was written by transaction <n>` (message varies; the token and `retryable=1` do not) |
| FK violation (missing parent on INSERT/UPDATE; live child on parent DELETE) | `ERR FK_VIOLATION retryable=0 <message>` |
| Assertion ceiling exceeded | `ERR ASSERTION_VIOLATION retryable=0 <message naming the group and the enforced ceiling>` (`CHECK COUNT(*) < 5` refuses at 5 saying "would exceed bound 4") |
| A Transition broken: an INSERT outside `ON INSERT`, an UPDATE along no declared edge, a DELETE outside `ON DELETE FROM` | `ERR TRANSITION_VIOLATION retryable=0 transition '<name>' on column '<c>': <what>, row id <n> (byte <b>)` |
| `PURGE` of a relation carrying a Transition | `Unsupported` — `ERR UNSUPPORTED retryable=0 '<t>' carries transition '<name>', whose state history a freed key would restart; PURGE frees no key of a relation that carries a Transition (byte <n>)` |
| Statement in a poisoned transaction | `ERR current transaction is aborted; commands are ignored until ROLLBACK` |
| `BEGIN` inside a transaction | `ERR a transaction is already open; COMMIT or ROLLBACK first` |
| `COMMIT`/`ROLLBACK` with none open | `ERR no transaction is open` |
| A row that is neither `n` nor `n - 1` values long | `ERR expected <n> value(s) including tuple key column '<name>', or <n-1> to have it issued; got <k> (byte <b>)` |
| A row that is not `n` values long, on a relation `ALTER TABLE ... ADD`/`DROP COLUMN` changed | `ERR expected <n> value(s), one per column, with tuple key column '<name>' NULL to have it issued; got <k> (byte <b>). The relation's columns were changed by ALTER TABLE, ...` |
| An `INSERT` column list naming no live column (unknown or dropped; matched ignoring case) | `InvalidArgument` — `ERR unknown column '<name>' at byte <n>` |
| A column name in `CREATE TABLE` equal, ignoring case, to an earlier one | `InvalidArgument` — `ERR column '<name>' repeats column '<earlier>' (names are unique ignoring case) (byte <n>)` (the later name's byte; nothing is created) |
| `ADD COLUMN` or `RENAME COLUMN` to a name equal, ignoring case, to a live column's | `AlreadyExists` — `ERR the relation already has a column named '<existing>' (names are unique ignoring case)`, with the new name's byte |
| Any statement - a read, a `SET` target, an `INSERT` column list, `DROP COLUMN`, `RENAME COLUMN`'s old name, `CREATE INDEX`, `CABIN`, `ASSERTION` or `TRANSITION` - naming a column that matches two or more columns ignoring case (only a relation created before names were unique ignoring case can hold them) | `InvalidArgument` — `ERR column name '<name>' is ambiguous: it matches '<a>' (position <i>) and '<b>' (position <j>) ignoring case (byte <n>)`; no statement can name either column of such a pair |
| A column name longer than 63 bytes in `CREATE TABLE`, `ADD COLUMN` or `RENAME COLUMN` | `InvalidArgument` at the name's byte |
| A column named twice in an `INSERT` column list | `InvalidArgument` — `ERR column '<name>' is listed twice in the column list at byte <n>` (the second occurrence's byte) |
| An `INSERT` column list that leaves out a `NOT NULL` column | `InvalidArgument` — `ERR column '<name>' is NOT NULL and the column list leaves it out (byte <n>); there is no DEFAULT` (the list's `(`; nothing is written) |
| A row whose length is not the `INSERT` column list's | `InvalidArgument` — `ERR expected <n> value(s), one per listed column; got <k> at byte <b>`, with ` (row <r>)` on a multi-row statement |
| A non-integer supplied tuple key | `InvalidArgument` — `ERR tuple key column '<name>' needs an integer literal (byte <n>)` |
| A supplied tuple key outside `[1, 2^48 - 1]` (zero, negative, too large, or past 64 bits) — *exhausted* | `OutOfRange` — `ERR tuple key <k> is outside the Keystone id space [1, 281474976710655] (byte <n>)` |
| A supplied tuple key already bound on a `BTREE` relation — *duplicate* | `AlreadyExists` — `ERR duplicate tuple key <k> already present at page <p> slot <s> (byte <n>)`, or for a `DELETE`d row's key, until a `PURGE` frees it, `ERR duplicate tuple key <k>: a row with this key was deleted; PURGE frees its key (page <p> slot <s>) (byte <n>)` |
| A supplied tuple key below the high-water mark of a `HEAP` relation (none can be created since SUS-1) | `OutOfRange` — `ERR tuple key <k> is below relation <oid>'s high-water mark <m>; a named key must sort above every key the relation has placed or issued, which is what keeps every page's slot order its key order`, or when the key sorts below the chain's tail `ERR tuple key <k> is below the relation's high-water mark: the chain's tail, page <p>, opens at <m>, an id already placed; a named key must sort above every key the relation has placed or issued` (neither form carries a byte) |
| An omitted tuple key on a `BTREE` relation whose issued id met a concurrently named key 8 times in a row | `AlreadyExists` — `ERR duplicate tuple key <k> already present at page <p> slot <s>` (no byte: the key was issued, not written) |
| `ASSIGNED` in CREATE TABLE | `Unsupported` — `ERR UNSUPPORTED retryable=0 the ASSIGNED key mode no longer exists (byte <n>) - ...` (removed 2026-08-25 and not coming back, which is why it is the permanent half; `EXPLICIT` is accepted and does nothing) |
| Assigning the tuple key in UPDATE | `Unsupported` — `ERR UNSUPPORTED retryable=0 tuple key column '<name>' cannot be updated at byte <n>; it is the tuple's identity, not a field of it` (K2; refused at compile, so nothing is written) |
| Unknown SET target in UPDATE | `InvalidArgument` — `ERR unknown column '<name>' at byte <n>` |
| A column assigned twice in one UPDATE's SET list | `InvalidArgument` — `ERR column '<name>' is assigned more than once at byte <n>` (the second name's byte; refused at compile, so nothing is written) |
| Unknown statement head | `ERR unknown SQL keyword '<w>' (supported: CREATE, DROP, ALTER, INSERT, SELECT, UPDATE, DELETE, PURGE)` |
| `PURGE` inside a transaction | `NotImplemented` — `ERR NOT_IMPLEMENTED retryable=0 PURGE inside a transaction is not implemented; run it in autocommit (byte 0)`; the transaction is not poisoned |
| `PURGE` of a system or heap relation | `Unsupported` — `ERR UNSUPPORTED retryable=0 '<t>' is a system relation; PURGE frees keys of user relations only (byte <n>)`, or `… is a heap relation, whose chain grows only at its tail; PURGE frees keys of btree relations only (byte <n>)` |
| `PURGE` with no `WHERE`, or a conjunct other than a tuple key comparison with an integer literal | `NotImplemented` — `ERR NOT_IMPLEMENTED retryable=0 PURGE needs a WHERE naming the tuple key; … (byte <n>)`, or `… PURGE admits only comparisons of the tuple key with an integer literal (=, <, <=, >, >=, BETWEEN); this conjunct is not one (byte <n>)` |
| `PURGE` naming one live key, or a negative tuple key literal | `InvalidArgument` — `ERR tuple key <k> is live; PURGE frees only a key whose row was deleted (byte <n>)`, or `ERR tuple key literal <k> is negative, and no key is below 1 (byte <n>)` |
| `PURGE` meeting an older snapshot, or an undecided writer, for 1 s | `TxnConflict`, retryable — `ERR TXN_CONFLICT retryable=1 PURGE cannot free tuple key <k> yet: …; nothing was purged, and running it again once that ends will` |
| Anything after a complete statement (e.g. `OFFSET 5 LIMIT 10`'s reversed tail) | `ERR unexpected token '<t>' after end of statement` |
| Bare `decimal` | `ERR column '<c>' needs a precision and a scale - decimal(p, s) - at byte <n>; there is no default scale, ...` |
| `float` column | `Unsupported` from the row-layout build: no decided on-disk encoding, and the type is refused rather than deferred |
| Scalar subquery returns >1 row | runtime `CardinalityViolation`, non-retryable — never a first-row pick |
| Statement touches more than `max_rows_touched` tuples (default 100M) | `ResourceExhausted`, non-retryable |
| Keystone id space exhausted | `OutOfRange` — ids are never wrapped |
| Value wider than one var-heap page (8144 bytes) | `NotImplemented` — a multi-page value needs a chained representation behind the same append/fetch pair, which is an open decision rather than a closed door |
| Cross-scale / cross-width decimal comparison, `SUM(uint64)`, `AVG(int)` | positioned compile-time refusal |

Retryability is one bit wide on purpose: `TXN_CONFLICT` is the only
retryable token. `retryable=0` means retrying the same statement will fail
the same way.

---

## Appendix: reserved words

Exactly eleven words are reserved (verified, `kKeywords` in `lexer.cpp`):
`JOIN`, `ON`, `AS`, `IN`, `EXISTS`, `NOT`, `BETWEEN`, `LEFT`, `RIGHT`,
`FULL`, `OUTER`. Everything else — including `SELECT`, `WHERE`, `GROUP`,
`COUNT`, `CABIN`, `CHECK`, `INDEX`, `REFERENCES` — is an ordinary
identifier matched by text where the grammar expects it, so a column may be
named `values`, `count` or `check`. Consequence: `pinned = on` in a pattern's
WITH list works even though `ON` is reserved, and a keyword hashes exactly
as an identifier does, so reserving a word never moves a fingerprint.
