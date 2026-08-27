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
| DDL | `CREATE TABLE`, `CREATE INDEX` / `DROP INDEX`, `CREATE ASSERTION` / `DROP ASSERTION`, `CREATE CABIN` / `DROP CABIN`, `CREATE PATTERN` / `DROP PATTERN` |
| DML | `INSERT`, `UPDATE`, `DELETE` |
| Query | `SELECT` (joins, subqueries, `GROUP BY`, aggregates), `ANALYZE <select>` |
| Transactions | `BEGIN`/`START`, `COMMIT`, `ROLLBACK`/`ABORT`, `SET ISOLATION LEVEL` |
| Introspection | `SHOW META/TABLES/PAGE/PATTERNS/ACCESS/BUDGET/CABINS/INDEXES/FKEYS/ASSERTIONS/RELAYOUT`, `DESCRIBE` |
| Session | `PING`, `SYNC`, `STOP` |

`DROP TABLE` and `ALTER TABLE` exist and are **catalog-only** (see their
sections): a drop tombstones the oid and orphans the pages — no space is
reclaimed — and a rename edits a label. The catalog is append-only apart
from those, `DROP PATTERN`, `DROP CABIN`, `DROP INDEX` and
`DROP ASSERTION`.

---

## 1. DDL

### CREATE TABLE

```sql
CREATE TABLE accounts (id int64, owner varchar, balance decimal(10,2)) BTREE;
CREATE TABLE trades   (id int64, sym varchar, qty int64) BTREE;
```

Grammar (verified):

```
CREATE TABLE <name> ( <col> <type> [NULL | NOT NULL] [REFERENCES <parent>]
                     [CABIN | CABIN AUTO | NO CABIN]
                     [, ...] ) [HEAP | BTREE] [EXPLICIT];
```

- The storage clause defaults to `HEAP`. `BTREE` gives the relation a
  clustered B+ tree on the primary key; only a btree relation can carry a
  secondary index or be an FK parent — and only a btree relation accepts a
  caller-named key that sorts *below* keys already placed (see the Keystone
  contract below).
- **There is no key mode.** Every relation takes a caller-named primary key
  or issues one when `INSERT` omits it, so nothing about keys is declared
  here. `EXPLICIT` is still accepted and **does nothing** — it states what is
  true of every relation, so old scripts keep working. `ASSIGNED` is refused:
  `ERR the ASSIGNED key mode no longer exists (byte <n>) - every relation
  takes a caller-supplied primary key or issues one when INSERT omits it, so
  there is nothing for this word to select`.
- **The words are order-free**: `BTREE EXPLICIT` and `EXPLICIT BTREE` are the
  same statement. Each *category* may appear at most once, and a repeat is
  refused with its byte (`ERR CREATE TABLE takes one storage word - HEAP or
  BTREE - and this is the second (byte <n>)`) rather than resolved
  last-one-wins: `HEAP BTREE` names two different relations.
- The words are ordinary **identifiers**, not reserved (see the appendix) — a
  column may still be named `explicit` or `btree`.
- **`EXPLICIT` does not change the storage default.** `CREATE TABLE t (...)`
  and `CREATE TABLE t (...) EXPLICIT` are both `HEAP`. Write `BTREE` when you
  want one.
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
  first column (the primary key) may not be `NULL`; neither may any index
  key or covered column (D2 — `IS NULL` answers by scan); a nullable
  column costs its relation a tail bitmap byte per 8 nullable columns,
  and a relation with none costs nothing.

**The Keystone contract (primary key).** The first column *is* the primary
key:

- It must be declared with an integer type (`catalog::CheckKeystoneColumn`,
  checked at CREATE TABLE).
- **Where its values come from is decided per `INSERT`, not per relation.**
  Supply a value in the pk's position and that value *is* the key; omit it
  and the engine issues one. Both work on every relation, and one statement
  may mix the two across its rows.
- **An issued key always clears every key you have named.** The relation
  keeps a high-water mark, `DESCRIBE`'s `next_id`, which every named key
  advances past itself. So naming 500 and then omitting the key gives 501,
  never a collision.
- **Naming a key that sorts backwards is `BTREE`-only.** On a `BTREE`
  relation a named key may be anything unused — any order, any gaps. On a
  `HEAP` relation it must be at or above the high-water mark, or it is
  refused: `ERR primary key <k> is below relation <oid>'s high-water mark
  <m>; a heap-clustered relation's ids must ascend, because its chain grows
  only at its tail - use BTREE to name keys in any order`. Named keys on a
  heap still need not be dense; they only have to go forward.
- It cannot be `UPDATE`d: it is the tuple's identity, not a field of it.
  Naming a key at insert and changing one afterwards are unrelated
  permissions; only the first is granted.
- It cannot carry a Cabin — a cabin on the primary key is refused with the
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
- A heap-clustered parent is refused at declaration: a parent needs a pk
  index for the check not to be a scan.
- v1 is RESTRICT-only and fail-fast. INSERT/UPDATE of an fk column checks the
  parent exists; DELETE of a parent checks for live children and stops at the
  first one. Violations answer `ERR FK_VIOLATION retryable=0 ...`; a check
  that meets an in-flight writer answers `ERR TXN_CONFLICT retryable=1 ...`.
- `SHOW FKEYS` lists declarations. CASCADE / SET NULL do not exist (FK-M6,
  out of v1 by decision F2).

### DROP TABLE (built 2026-08-10, DT1-DT6 / DT01-DT05)

```sql
DROP TABLE <name>
```

**Catalog-scoped**: the relation becomes unreachable, its name frees
immediately, and its oid is tombstoned — never reissued — so stale
advisory structures keyed by the dead oid can never mis-attribute. The
pages **orphan** (heap chain, var-heap, index pages): no space is
reclaimed, deliberately, until the free-map-reuse and reader-horizon
decisions land.

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

### ALTER TABLE (built 2026-08-10, AL1-AL9 / ALT01-ALT05)

```sql
ALTER TABLE <t> RENAME TO <new>
ALTER TABLE <t> RENAME COLUMN <old> TO <new>
```

v1 is **catalog-only**: a rename changes a catalog label and no tuple
bytes. Identity is the oid, so nothing dangles — foreign keys still
enforce, indexes and Cabins still serve, all with no re-declaration.

- `ADD COLUMN` / `DROP COLUMN` / type changes answer `Unsupported` with a
  position: the row size is a schema constant (invariant 13), so a
  column-set change is a relation rewrite, not a catalog edit. Widening
  is permanently out by the tagged-cell design.
- **Assertions RESTRICT both forms**: the stored canon is the
  declaration's text, so `ALTER` on a relation carrying one refuses,
  naming it — drop, rename, re-declare.
- **Patterns are allowed to die**: statements against the old name stop
  matching stored patterns and trails, which costs replay speed, never a
  row; new-name traffic re-registers on the ordinary path.
- The pk column renames like any other (identity is the Keystone word,
  not the spelling); a `sys.*` relation refuses; a taken name is
  `AlreadyExists`.
- Unlogged and non-transactional. (`CREATE TABLE` is the one DDL that is
  transactional; `ALTER` is not.)

### CREATE INDEX / DROP INDEX (built, IX01-IX16)

```sql
CREATE INDEX ix_owner ON accounts(owner) COVERING (balance);
DROP INDEX ix_owner;
```

- Multi-column keys: up to `kMaxIndexKeyColumns` (4) key columns and
  `kMaxIndexCoveredColumns` (8) covered columns. The caps refuse, never
  truncate, and the parser names the byte of the offending column.
- Only a `BTREE` relation may carry an index (an entry's payload is the pk).
- `CREATE UNIQUE INDEX` is refused (`Unsupported`): enforcing uniqueness
  would make the index a constraint that can fail a write, and v1 is a read
  accelerator that cannot fail a write for a reason of its own (IX11).
- Maintenance is append-only; DELETE does not touch an index; there is no
  index-only scan (no visibility witness exists outside the tuple), so
  `COVERING` buys avoided base descents, nothing more.
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
- **On a multi-core instance an assertion is built and enforced by the core
  that owns its relation** (2026-08-26). Two consequences a client sees:
  `CREATE ASSERTION` on such a relation is refused **inside an explicit
  transaction** and must be run in autocommit — the reply says so and names
  the owner — and its success line carries `built_by_core=<n>`. On any other
  core `SHOW ASSERTIONS` prints `enforcing=0 enforced_by_core=<n>` for it:
  that is a statement about which core holds the directory, not about
  whether the constraint runs. A relation whose assertion its owner *cannot*
  enforce — a database file written before this change — refuses writes on
  that core by name until the assertion is dropped and re-created.

### CREATE CABIN / DROP CABIN (built, CB01-CB11)

```sql
CREATE CABIN ON accounts(owner);
DROP CABIN ON accounts(owner);
```

- A Cabin is named by what it is on — `(table, column)` — never by a name of
  its own. One column only in v1; a comma in the column list is refused with
  a message naming decision C3.
- Refused on the primary key and on a `NO CABIN` column, whoever asks.
- `SHOW CABINS` lists them. Entry sets are memory-resident and do not survive
  a restart; only the catalog row persists.

### CREATE PATTERN / DROP PATTERN (built)

```sql
CREATE PATTERN hot_account ($acct int64) WITH (pinned = on)
    OF SELECT * FROM accounts WHERE id = $acct;
DROP PATTERN hot_account;
```

- Parameters are `$`-sigiled and their type annotation is **mandatory**
  (inference was rejected: it would make the contract depend on predicate
  order).
- `()` is legal — a pattern with exactly one instance.
- `WITH` comes **before** `OF` so the body runs to end of statement.
- The body must be a SELECT. `$params` are legal only inside a pattern body;
  anywhere else the token is reserved and refused by name.
- A declaration is not an execution: the body is type-checked and
  fingerprinted, never run.

---

## 2. Types

Registered in `sys.types` (verified in `src/catalog/catalog.cpp`):

| Declared name | Storage | Notes |
|---|---|---|
| `int8`, `int16`, `int32`, `int64` | 1/2/4/8 bytes | first column must be one of the integer types |
| `uint64` | 8 bytes | full range preserved via the literal's digit text |
| `bool` | 1 byte | |
| `varchar` | one tagged cell of `inline_cell_width` (64) bytes | longer values spill to the var-heap; > 8144 bytes is `Unsupported` |
| `char` | 1 byte | |
| `date` | int32, days since 1970-01-01 | literal: `'2026-08-09'` |
| `timestamp` | int64, microseconds since the epoch, UTC | literal: `'2026-08-09 12:00:00'` |
| `decimal(p,s)`, p ≤ 18 | int64 unscaled + scale | TY01-TY11; both arguments **mandatory** |
| `decimal(p,s)`, 19 ≤ p ≤ 38 (also spelled `decimal128(p,s)`) | int128 | a separate type, never a widening of the 8-byte one |

- **A bare `decimal` is refused, never defaulted** — a default scale is a
  silent decision about someone's money, and only the parser can still tell
  "said nothing" from "said zero".
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
-- Omit the pk and the engine issues one
INSERT INTO accounts VALUES ('alice', 120.50);
INSERT INTO accounts VALUES ('bob', 10), ('carol', 20), ('dave', 30);

-- Or name it: the pk comes first. On a BTREE relation it need not ascend
INSERT INTO trades VALUES (5000, 'AAPL', 10);
INSERT INTO trades VALUES (900, 'MSFT', 3), (4200, 'NVDA', 7);

-- And one statement may do both. Row 2's key is issued above 7000
INSERT INTO trades VALUES (7000, 'AMD', 1), ('INTC', 2);
```

- Grammar: `INSERT INTO <table> VALUES (<val> [, ...]) [, (<val> [, ...])]*`
  — no column list; up to `max_insert_rows` rows per statement (config key,
  default 1024, a refusal never a truncation).
- **The arity is per row, and there are two of them.** Supply `n` values on
  an `n`-column relation and the first is the primary key; supply `n - 1` and
  the engine issues one. Both are legal on every relation and a statement may
  mix them. The two counts cannot be confused — `INSERT` is positional with
  no column list and no body column may be omitted individually — so a wrong
  length is refused naming both:
  `ERR expected 3 value(s) including primary-key column 'id', or 2 to have it issued; got 4`.
- **A named pk must be a non-negative integer literal**, and must fit
  `[1, 2^40 - 1]` — 0 is reserved for "unset". A non-integer or negative
  value is refused with the offending token's byte; an unspellable one with
  the bound it missed. All of these are checked before anything is placed,
  so a refused row burns no id (BI9).
- **A named pk that is already in use is refused**
  (`ERR duplicate primary key <n> already present at page <p> slot <s>`). On
  a `BTREE` relation uniqueness is proved by the clustered descent, which
  lands on the only leaf that could hold the key. On a `HEAP` relation it is
  proved by the high-water mark instead — which is why a heap relation
  refuses a key below it (see the Keystone contract). A `DELETE`d row still
  holds its key until its slot is reclaimed, and nothing reclaims today, so a
  deleted pk cannot be re-supplied.
- Replies: `INSERTED oid=<o> id=<n> page=<p> slot=<s>` for one row;
  `INSERTED oid=<o> rows=<n> first_id=<f> last_id=<l>` for several (no id
  contiguity promised). `first_id`/`last_id` are the **first and last rows
  in statement order**, not a minimum and maximum — on a `BTREE` relation,
  where named keys may descend, `last_id` can be below `first_id`.
- **Every bulk row runs the full single-row pipeline, in order** (BI2):
  FK check, assertion admission (row k sees rows 1..k-1's reservations),
  id, encode, placement, Cabin witness, index maintenance, WAL. Bulk
  amortizes parse and round trips, never authority.
- **Atomic** (BI4): any per-row refusal fails the whole statement with the
  row ordinal appended to the message — ` (row 3)` — and inserts nothing.
  A pre-id refusal burns no id; an aborted statement burns exactly its
  placed rows' ids (BI9, accepted and documented).
- Row count is not part of the pattern (BI5): a 500-row insert
  fingerprints as the 1-row insert on the same relation.
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

- Grammar: `UPDATE <table> SET <col> = <val> [, ...] [WHERE ...]`.
- The WHERE is the same production SELECT uses, subqueries included.
- **The primary key cannot be assigned**, and the
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
- Reply: `UPDATED <n>`.

### DELETE (verified — it exists)

```sql
DELETE FROM accounts WHERE balance = 0;
```

A prior finding that KDS had no SQL DELETE is **no longer true**: `DELETE
FROM <table> [WHERE ...]` parses (`ParseDelete`, `DeleteStmt` in the AST)
and executes. It arrived with the transaction work (T01-T14). Semantics:

- A DELETE is a **delete-mark**, never a physical removal — the tuple's
  bytes stay for readers whose snapshot predates the deleter. Nothing
  purges.
- No column list, nothing to assign; the WHERE nests subqueries exactly as
  SELECT's does. An empty WHERE marks every row.
- DELETE deliberately does not touch Cabins or secondary indexes (removal is
  forbidden — an older snapshot may still match), and has no assertion
  check (upper bounds only make DELETE check-free), though it does write the
  departure entry that keeps assertion group headers truthful.

---

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
- A join keyed on a relation's **primary key** executes as a pk probe
  (lookup class). Any other equi-join is *not refused* — it compiles to a
  scan (or index/Cabin/filter scan) of that relation with the ON equality
  evaluated as a residual filter per row (verified in the step compiler's
  access-kind assignment). Slow is legal; wrong is not.

### WHERE

`WHERE <cond> [AND <cond>]*` — AND-combined conjuncts, no `OR`, no `NOT`
over expressions, no parenthesized nesting. Each conjunct is one of
(verified — the `PredicateKind` enum):

| Form | Kind | Notes |
|---|---|---|
| `col op value` | kCompareValue | `op` ∈ `=`, `!=`, `<`, `<=`, `>`, `>=` |
| `col op other.col` | kCompareValue (column RHS) | how correlation is written |
| `col op (SELECT ...)` | kCompareSubquery | scalar; >1 row is a runtime `CardinalityViolation` |
| `col IN (SELECT ...)` | kInSubquery | |
| `col NOT IN (SELECT ...)` | kNotInSubquery | tri-state, not `!IN` — stays correct when NULLs land |
| `EXISTS (SELECT ...)` | kExists | no column on the left |
| `NOT EXISTS (SELECT ...)` | kNotExists | |
| `col BETWEEN low AND high` | kBetween | inclusive both ends; lowers to `>= low AND <= high`, the range on the step is only a hint |
| `col IS NULL` / `col IS NOT NULL` | kCompareValue | the one spelling that finds NULLs — answered from the stored bitmap |

- Subqueries are **predicate-position only**, correlated included, nested to
  at most `kMaxSubqueryDepth = 4` below the outermost block. The fifth level
  is refused with a position.
- `IN (1, 2, 3)` — a value list — does **not** exist yet (the open half of
  workplan V08). It is reported as "expected a subquery".
- **Comparisons are three-valued** (null.md §4): a relational operator
  with a NULL operand is unknown, and `WHERE` keeps only true — so
  `col = NULL` matches nothing, ever (the standard's trap, kept on
  purpose); `IS NULL` is the spelling that finds them.
- A pk equality (`WHERE id = 42`) executes as a keyed descent; a pk
  `BETWEEN` as a range walk with tail pruning; a non-pk equality is served
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
across steps, pk order within one — so `LIMIT n OFFSET m` means rows
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
- **Ordering by the primary key ascending is free**, and only that form:
  it names the order the engine already emits, so the compiler drops the
  clause and the statement pays nothing. `ANALYZE` shows this as the
  absence of a `sort` line. Every other order — including `ORDER BY <pk>
  DESC` — is an output sort, which is not free; see the two notes below.
- **`ORDER BY <pk>` costs slightly more once a relation has taken a key
  below its high-water mark** — which only a `BTREE` relation admits, and
  which `DESCRIBE` reports as `key_order=unordered`. Such a key lands in the
  slot it was inserted into rather than the one it sorts to, so the engine
  emits each page's rows in key order instead of slot order. Exact, not
  approximate, and still not an output sort — the disorder is bounded by one
  page. A relation whose keys have only ever gone forward, named or issued,
  pays nothing.
- **A sorted statement's `LIMIT` bounds what you receive, not what the
  engine reads.** The sort must see every qualifying row before it knows
  which comes first, so unlike an unsorted or pk-ordered `LIMIT`, it
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
  prefer keyset form: `WHERE id > <last seen> LIMIT n`.
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

---

## 5. Transactions

```
BEGIN [TRANSACTION | WORK] [ISOLATION LEVEL {READ COMMITTED | REPEATABLE READ}]
COMMIT
ROLLBACK          (ABORT is a synonym)
SET ISOLATION LEVEL <name>
```

Verified in `HandleBegin` / `HandleCommit` / `HandleRollback` /
`HandleSetIsolation`:

- `START` is accepted for `BEGIN`; `TRANSACTION` / `WORK` are noise words.
- Replies: `BEGIN trx_id=<n> isolation=<level>`, `COMMIT trx_id=<n>`,
  `ROLLBACK trx_id=<n>`, `SET isolation=<level>`.
- Exactly two isolation levels: `READ COMMITTED` (default, read view per
  statement) and `REPEATABLE READ` (read view per transaction).
  `SERIALIZABLE` is refused as out of scope, not open. The precedence chain
  is config key → `SET ISOLATION LEVEL` (session, next transaction) →
  `BEGIN ISOLATION LEVEL` (this transaction).
- No nested transactions, no savepoints: a second `BEGIN` is an error.
  `SET ISOLATION LEVEL` inside an open transaction is an error.
- Write conflicts are first-updater-wins, no waiting:
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
  it before it commits (see its own entry).
- **`DROP INDEX` is transactional too** (2026-08-18): inside an explicit
  transaction it is rolled back, and it is isolated — index maintenance
  keeps writing entries for an index whose drop has not committed, so a
  `ROLLBACK` leaves the index whole, including rows another session
  wrote while the drop was open. The isolation is proved for writers on
  one core, which is every writer this engine has today (cross-core
  writes are refused). It was refused inside a transaction
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
- Other DDL (`ALTER TABLE`, cabins, patterns, assertions) is **not**
  transactional and is not rolled back.

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
| `SHOW META` | instance metadata (superblock, format version, config), plus what the last mount's recovery did — records scanned, transactions committed and rolled back, per-phase timings, and `catalog_recovered=0`, which is a standing statement that DDL is unlogged rather than a number that will change (`docs/spec/client-manual.md` has the field list) |
| `SHOW TABLES` | the relation list |
| `SHOW PAGE <id> [VALUES]` | one page's header and slots; `VALUES` hex-dumps tuple payloads |
| `SHOW PATTERNS` | `sys.patterns` — registered patterns, ids, Waystone state |
| `SHOW ACCESS` | `sys.access_stats` — one row per access shape `(kind, rel, columns)` |
| `SHOW BUDGET` | per-relation Keystone id consumption, `warning=`/`exhausted=` counts |
| `SHOW CABINS` | Cabins and their per-column policies |
| `SHOW INDEXES` | secondary indexes, key and covered columns |
| `SHOW FKEYS` | declared foreign keys |
| `SHOW ASSERTIONS` | assertions with `enforcing=` and live counters (checks/violations/reserved/aborted) |
| `SHOW RELAYOUT [<table>]` | the physical optimizer's shadow report (candidate moves, each blocked by a named gate) |
| `DESCRIBE <table>` (`DESC`) | columns, types, `clustered_type=`, `key_order=ascending\|unordered`, `next_id`, `ids_issued`/`ids_remaining`/`budget_used`. A pk column reports `autoincrement=if-omitted`, every other column `no` |
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
`sys.patterns.pattern_id` really does use the full 64-bit range. One
consequence, the same one a `uint64` column on an ordinary relation has:
a negative literal is not a `uint64`, so `WHERE oid > -1` is a non-match
for every row rather than a comparison that succeeds.

There is also a legacy non-SQL form: `CREATE TABLE <name>` with **no column
list** routes to a pre-SQL handler (the dispatcher routes on the presence
of `(`). It answers `EXISTS oid=<n>` if the name is taken; otherwise the
catalog refuses the empty schema — `ERR relation has no columns; the first
column is the mandatory Keystone primary key` — so the form can no longer
create anything.

---

## 7. What KDS refuses, and why

Every refusal below is deliberate — `StatusCode::kUnsupported` with the byte
position of the offending token, not a syntax error. The distinction is
policy: `Unsupported` means "understood and declined, here is the decision it
waits on"; `InvalidArgument` means "simply wrong".

- **No CTEs.** `WITH` is answered by name at the statement head: a CTE is
  table-position nesting, and the inner result would have to become a
  relation with a schema. *"common table expressions (WITH) are not
  supported; subqueries are allowed in predicate position only."*
- **No derived tables.** `FROM (SELECT ...)` is refused by the relation
  reference itself: materializing an inner result breaks pk-direct probing
  into the next step and puts a temp relation in the storage layer.
- **No window functions, no expressions.** The grammar has no expression
  tree at all — no arithmetic, no `OR`/`NOT` nesting, no function calls
  outside the five aggregates. A smaller grammar every statement of which
  executes predictably was chosen over a larger one with unpredictable
  corners.
- **No outer joins.** `LEFT`/`RIGHT`/`FULL`/`OUTER` are reserved keywords
  that answer `Unsupported` with their own position, so the grammar will not
  shift if they ever land.
- **The join rule.** KDS supports inner equi-join chains, executed in
  written order, probing by primary key where the ON allows it. There is no
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
- **No cross-dialect shims.** No `SERIAL`, no `AUTO_INCREMENT` (every pk is
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
- **No space reclamation, and no data-moving `ALTER`.** `DROP TABLE`
  exists but is catalog-scoped (DT1): the pages orphan, because returning
  them to the free map is gated on trail-validation and reader-horizon
  decisions owned elsewhere. `ALTER TABLE` is renames only (AL1). The
  RESTRICT predicates (assertions, referencing fkeys) now have their
  callers in both.
- **No `SELECT *` across a join**, no duplicate FROM bindings, no
  aggregates in subqueries, no subquery nesting past depth 4 — each refused
  with the exact byte.

---

## 8. Error surfaces

Reply shape (verified, `server::ErrorReply` — a compatibility surface, not a
diagnostic): `ERR <TOKEN> retryable=<b> <message>` for the three codes a
client library switches on, `ERR <message>` for everything else.

| Situation | Exact surface |
|---|---|
| Write conflict (first-updater-wins), write to another core's relation, FK check meets in-flight writer, CREATE ASSERTION on unsettled relation, a peer core's spent row-id / transaction-id / extent lease before its refill lands | `ERR TXN_CONFLICT retryable=1 row id=<n> was written by transaction <n>` (message varies; the token and `retryable=1` do not) |
| FK violation (missing parent on INSERT/UPDATE; live child on parent DELETE) | `ERR FK_VIOLATION retryable=0 <message>` |
| Assertion ceiling exceeded | `ERR ASSERTION_VIOLATION retryable=0 <message naming the group and the enforced ceiling>` (`CHECK COUNT(*) < 5` refuses at 5 saying "would exceed bound 4") |
| Statement in a poisoned transaction | `ERR current transaction is aborted; commands are ignored until ROLLBACK` |
| `BEGIN` inside a transaction | `ERR a transaction is already open; COMMIT or ROLLBACK first` |
| `COMMIT`/`ROLLBACK` with none open | `ERR no transaction is open` |
| A row that is neither `n` nor `n - 1` values long | `ERR expected <n> value(s) including primary-key column '<name>', or <n-1> to have it issued; got <k>` |
| A non-integer or negative supplied pk | `ERR primary-key column '<name>' needs an integer literal (byte <n>)` / `ERR primary key <n> is negative (byte <n>)` |
| A supplied pk outside `[1, 2^40 - 1]` | `InvalidArgument` — names the bound it missed (`0 is reserved for "unset"`, or the 40-bit ceiling) |
| A supplied pk already in use (including a `DELETE`d row's, whose slot is never reclaimed) | `AlreadyExists` — `ERR duplicate primary key <n> already present at page <p> slot <s>` |
| A supplied pk below a **heap** relation's high-water mark | `OutOfRange` — `ERR primary key <k> is below relation <oid>'s high-water mark <m>; a heap-clustered relation's ids must ascend ... - use BTREE to name keys in any order` |
| `ASSIGNED` in CREATE TABLE | `Unsupported` — `ERR the ASSIGNED key mode no longer exists (byte <n>) - ...` (removed 2026-08-25; `EXPLICIT` is accepted and does nothing) |
| Assigning the pk in UPDATE | `Unsupported` — `ERR primary-key column '<name>' cannot be updated at byte <n>; it is the tuple's identity, not a field of it` (K2; refused at compile, so nothing is written) |
| Unknown SET target in UPDATE | `InvalidArgument` — `ERR unknown column '<name>' at byte <n>` |
| Unknown statement head | `ERR unknown SQL keyword '<w>' (supported: CREATE, DROP, ALTER, INSERT, SELECT, UPDATE, DELETE)` |
| Anything after a complete statement (e.g. `OFFSET 5 LIMIT 10`'s reversed tail) | `ERR unexpected token '<t>' after end of statement` |
| Bare `decimal` | `ERR column '<c>' needs a precision and a scale - decimal(p, s) - at byte <n>; there is no default scale, ...` |
| `float` column | `Unsupported` from the row-layout build: no decided on-disk encoding |
| Scalar subquery returns >1 row | runtime `CardinalityViolation`, non-retryable — never a first-row pick |
| Statement touches more than `max_rows_touched` tuples (default 100M) | `ResourceExhausted`, non-retryable |
| Keystone id space exhausted | `OutOfRange` — ids are never wrapped |
| Value wider than one var-heap page (8144 bytes) | `Unsupported` |
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
