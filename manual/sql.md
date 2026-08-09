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

There is **no `DROP TABLE` and no `ALTER TABLE`**. The catalog is append-only
apart from `DROP PATTERN`, `DROP CABIN`, `DROP INDEX` and `DROP ASSERTION`.

---

## 1. DDL

### CREATE TABLE

```sql
CREATE TABLE accounts (id int64, owner varchar, balance decimal(10,2)) BTREE;
```

Grammar (verified):

```
CREATE TABLE <name> ( <col> <type> [REFERENCES <parent>] [CABIN | CABIN AUTO | NO CABIN]
                     [, ...] ) [HEAP | BTREE];
```

- The storage clause defaults to `HEAP`. `BTREE` gives the relation a
  clustered B+ tree on the primary key; only a btree relation can carry a
  secondary index or be an FK parent.
- At least one column is required.
- The optional per-column suffixes come in a **fixed order**: `REFERENCES`
  before the cabin policy. Two optional suffixes in either order would be a
  grammar with two spellings of one declaration.

**The Keystone contract (primary key).** The first column *is* the primary
key, and it is the engine's, not the client's:

- It must be declared with an integer type (`catalog::CheckKeystoneColumn`,
  checked at CREATE TABLE).
- Its values are system-generated and autoincrement. `INSERT` must **not**
  supply a value for it (see §3) — a caller-supplied pk is a defect by
  invariant 11.
- It cannot be `UPDATE`d: it is the tuple's identity, not a field of it.
- It cannot carry a Cabin — a cabin on the primary key is refused with the
  position of the `CABIN` keyword.
- Ids are unique and monotonic, never gapless.

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
(`feat-physical-optimizer.md` Part II) but not wired to DDL.

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
  refused retryably. The one caveat: after a restart the surviving catalog
  row honestly reports `enforcing=0` until recovery (which does not exist)
  can replay the directory — `SHOW ASSERTIONS` shows it.

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

### INSERT (verified)

```sql
INSERT INTO accounts VALUES ('alice', 120.50);
```

- Grammar: `INSERT INTO <table> VALUES (<val> [, ...])` — no column list.
- **VALUES supplies the columns *after* the primary key.** Supplying a value
  for the pk column is caught with a dedicated message:
  `ERR do not supply a value for primary-key column 'id' - it is autoincrement and engine-assigned`.
- The reply is `INSERTED id=<n>`-class; the engine issues the id.
- Values are integers, strings (`'...'`, no quote escaping), bare numerics
  and `NULL`. Order of checks: FK forward check and assertion admission run
  *before* the row id is allocated, so a refused row burns nothing.
- INSERT is the one fully WAL-logged statement path; durability class comes
  from the `durability` config key (see §5).

### UPDATE (verified)

```sql
UPDATE accounts SET balance = 99.00 WHERE id = 42;
```

- Grammar: `UPDATE <table> SET <col> = <val> [, ...] [WHERE ...]`.
- The WHERE is the same production SELECT uses, subqueries included.
- The primary key cannot be assigned. An UPDATE never migrates a tuple.
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

- Subqueries are **predicate-position only**, correlated included, nested to
  at most `kMaxSubqueryDepth = 4` below the outermost block. The fifth level
  is refused with a position.
- `IN (1, 2, 3)` — a value list — does **not** exist yet (the open half of
  workplan V08). It is reported as "expected a subquery".
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

### Pagination

**There is none.** `LIMIT` / `OFFSET` are not in the grammar — a trailing
`LIMIT 10` answers `ERR unexpected token 'LIMIT' after end of statement`.
`ORDER BY` does not exist for non-aggregated output either (no output sort
exists). The designed pagination surface is KWP/1's portal suspension
(`docs/protocol.md`), of which only the frame codec is built — nothing calls
it. A result set is streamed whole; bound it with WHERE.

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
- DDL is **not** transactional: `CREATE TABLE` inside a transaction is not
  rolled back.

**Durability class.** Three classes exist — `strict` (D1, fsync per commit),
`group` (D2, default), `relaxed` (D3) — but the class is set by the
`durability` **config key**, instance-wide. The per-transaction durability
field is a KWP/1 protocol feature (`docs/protocol.md`), and KWP is not
wired: **no SQL or text-protocol spelling selects a durability class per
transaction today.** Specified, not built.

---

## 6. Introspection (verified — the complete list)

Dispatcher commands, not parser statements:

| Command | Answers |
|---|---|
| `SHOW META` | instance metadata (superblock, format version, config) |
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
| `DESCRIBE <table>` (`DESC`) | columns, types, `next_id`, `ids_issued`/`ids_remaining`/`budget_used` |
| `PING` | `PONG` |
| `SYNC` | forces log + store durability: `OK synced` |
| `STOP` | closes the connection: `OK bye` |

An unknown target answers `ERR unknown SHOW target`. The `sys.*` views
(`sys.tables`, `sys.types`, ...) are also readable through ordinary
`SELECT ... FROM sys.<name>` — but not aggregatable (AG12).

There is also a legacy non-SQL form: `CREATE TABLE <name>` with **no column
list** creates an empty-schema relation and answers `CREATED oid=<n>` (or
`EXISTS oid=<n>` if the name is taken). The dispatcher routes on the
presence of `(`. A debug surface predating the SQL grammar; not for use.

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
  hash join and no merge join **because there is no plan search to choose
  between algorithms — the query is the plan.** Written order is a client
  contract; decorrelation rewrites are forbidden, not merely unimplemented.
  A stable plan is what lets `pattern_id` name a plan and Waystone trust it.
- **No `ORDER BY` / `LIMIT` / `HAVING`.** No output sort exists; `HAVING`
  answers with "filter before the fold with WHERE, or filter the result
  client-side".
- **No cross-dialect shims.** No `SERIAL`, no `AUTO_INCREMENT` (the pk is
  already both), no `IF NOT EXISTS`, no `public.` schema qualifier, no
  bare-alias `FROM t a`, no quote-escaping in string literals. Each would be
  a second spelling of something that already has one, and every spelling
  must fingerprint identically forever.
- **No `CREATE INDEX`-by-another-name.** There is exactly one index DDL and
  it is honest about its class: an index is "a Cabin that observed
  everything", append-only, verified at read. `UNIQUE` is refused because it
  would turn a read accelerator into a constraint that can fail writes.
- **No `DROP TABLE`, no `ALTER TABLE`.** Nothing reclaims catalog rows or
  relation pages yet; the RESTRICT hook for assertions/FKs exists but has no
  caller. `DROP` names exactly four object kinds: PATTERN, CABIN, INDEX,
  ASSERTION.
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
| Write conflict (first-updater-wins), write to another core's relation, FK check meets in-flight writer, CREATE ASSERTION on unsettled relation | `ERR TXN_CONFLICT retryable=1 row id=<n> was written by transaction <n>` (message varies; the token and `retryable=1` do not) |
| FK violation (missing parent on INSERT/UPDATE; live child on parent DELETE) | `ERR FK_VIOLATION retryable=0 <message>` |
| Assertion ceiling exceeded | `ERR ASSERTION_VIOLATION retryable=0 <message naming the group and the enforced ceiling>` (`CHECK COUNT(*) < 5` refuses at 5 saying "would exceed bound 4") |
| Statement in a poisoned transaction | `ERR current transaction is aborted; commands are ignored until ROLLBACK` |
| `BEGIN` inside a transaction | `ERR a transaction is already open; COMMIT or ROLLBACK first` |
| `COMMIT`/`ROLLBACK` with none open | `ERR no transaction is open` |
| Supplying the pk in INSERT | `ERR do not supply a value for primary-key column '<name>' - it is autoincrement and engine-assigned` |
| Unknown statement head | `ERR unknown SQL keyword '<w>' (supported: CREATE, DROP, INSERT, SELECT, UPDATE, DELETE)` |
| Anything after a complete statement (e.g. `LIMIT`) | `ERR unexpected token '<t>' after end of statement` |
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
