# Work order BJ — expressions in `UPDATE ... SET`: a value computed from the row it replaces

Written 2026-10-10 on `worktree-bj-expression-update` from `080cd55`
(`v2.7.0-723-g080cd555`). It follows the operator's words of 2026-10-10, recorded in
English (`raft-marks-2026-10-10.md` §1-§2):

- **W0:** *"kdbs has to support expression update now."*
- **W1 (E1):** *"The scope of expressions is the same as a general
  commercial DB."*
- **W2 (E2):** *"Follow the SQL standard."* (every SET expression reads the
  row as it was before the statement changed it)
- **W3 (E3):** *"No implicit conversion."*
- **W4 (E4):** *"Follow the standard."* (NULL)
- **W5 (E5):** *"No pk update."*
- **W6 (E6):** *"The expression's structure (operators, referenced
  columns) is part of the statement's shape, and a literal is treated as a
  value."*
- **W7 (E7):** *"No measurement gate; the overhead is measured."*
- **W8 (E8):** *"Write CN-7 to match the changed spec (not deleted, not
  closed)."*

E1-E8 are the item numbers CLA's proposal of the same session used; the
words answer them one for one.

**Written 2026-10-10, opened the same day on a standing go-ahead.** W1-W8
are the operator's decisions and stand here as BJ-R1..R8. What they leave
open is in §4. BJ-Q0..Q12 were unmarked when the order was written; the
operator then gave the standing word *"go ahead until achieving milestone,
I will follow CLA proposal if decision needed"* (the
`go-ahead-achieving-milestone` skill, 2026-10-10), and CLA's proposals were
adopted under it. **Each is recorded in §4's mark column as adopted on that
word, never as the operator's own per-item mark** (`raft-marks-2026-10-10.md`
§3), and the two items whose proposals the review revised (BJ-Q6, BJ-Q10) and
the one CLA revised on adoption (BJ-Q2) say so in place.

**Claim tags:**
- `[source-read]` with `path:line` at `080cd55`;
- `[design]` for CLA's reasoning about code not yet written;
- `[measured]` with the invocation (none in this order);
- `[quiet-wrong]` marks an item where a wrong choice turns a refusal or a
  correct answer into a wrong one with no error.

The operator's words, and a rule that restates them, are decisions rather
than claims, so they carry no tag.

**The letter.** `[source-read]` BI is the last order on `main`
(`workorder-bi-checkpoint-off-the-reactor.md`); no branch on `origin` and
no file under `instructions/` uses BJ. BJ is the next free letter.

**BJ against other orders.**
- **CN-7** (`docs/conceptnotes/cn7-delta-verbs.md`) excluded `SET col =
  <expression>` as "a parser question this note does not open" (§6). W8
  restates CN-7 for the spec BJ makes; it is revised beside this order and
  stays a concept note. Its ruling R1 (no implicit `UPDATE` → `INC`
  conversion) binds BJ: an expression SET is an ordinary overwrite.
- **BA** (`workorder-ba-parallelism.md`): the order is on `main`; its
  stages from BA-S2 on are built on `worktree-ba-open-marks` and not merged
  at `080cd55`. BA-S14 there re-runs inside the engine a statement whose
  structural refusal came before it wrote anything (`DispatchAsync`), by
  the same trail test as the park (§1.5). BJ-R9 depends on that test being
  exact. The file overlap is `command_dispatcher.cpp`, so BJ-S4 starts
  after BA's code lands on `main` (§3's note).
- **AQ** (`workorder-aq-expression-cabins.md`, opened 2026-10-06, no stage
  started): an expression Cabin evaluates its function expression `F` at
  the two write sites, UPDATE's included, over the row the write hook is
  handed (AQ §1.4). `[design]` BJ's evaluator and AQ's `F` evaluate the
  same thing - a typed expression over one decoded row, its functions from
  `include/kds/exec/functions.hpp` - so there is one evaluator and one
  function catalog, owned by whichever order lands its first evaluation
  stage, and the other consumes it. The hook already receives the row
  after the SET list is applied (`command_dispatcher.cpp:7803-7810` then
  the hook), so a computed value reaches AQ's `F` as a literal does.
- **CN-7's delta verbs, CN-11 and every other note** are not opened by BJ.

## 0. What BJ is

`[source-read]` Today a SET value is a literal. `Assignment` carries an
`AstValue` (`include/kds/parser/ast.hpp:736-749`), and `ParseUpdate` reads
`<ident> = ParseValue()` (`src/parser/parser.cpp:2141-2188`), which takes an
integer, a number, a string or `NULL` and refuses a function call by name
(`:226-230`). So `UPDATE accounts SET balance = balance - 100 WHERE id = 7`
does not parse, and a client applies a delta by `SELECT`, arithmetic, then
`UPDATE ... SET` the computed literal: two round trips, and under
`READ COMMITTED` a **lost update** whenever another writer commits between
the two statements, because the `UPDATE` takes a fresh snapshot that sees
that commit and overwrites it with a value computed from the older one.
`[source-read]` First-updater-wins refuses only inside one statement, in
the window between its snapshot and its write (`docs/spec/txn.md:677-691`).

BJ gives `SET` a value expression, evaluated per qualifying row against
that row's values before the statement changed them, with the operators,
functions and forms a general commercial database offers (W1), under the
SQL standard's rules (W2, W4), with no implicit conversion between types
(W3).

**BJ's rules, one line each:**

- **BJ-R1 (W1).** A SET value is a value expression from the reference
  dialect BJ-Q1 names, staged by family (§3); a form not yet built is
  refused `NotImplemented` at its byte, never parsed and dropped.
- **BJ-R2 (W2).** Every SET expression reads the row as it was when the
  statement reached it; assignments do not see each other.
  `SET a = b, b = a` swaps. A column assigned twice in one SET list is
  refused. `[quiet-wrong]`
- **BJ-R3 (W3).** No implicit conversion. Every operator and function has
  declared signatures; operands that match none, or an expression whose
  type is not its target's, are refused at compile naming the types and
  the byte. A literal takes the type its context gives it, as a
  predicate's literal takes its column's (`docs/spec/types.md` §3.1). An
  explicit `CAST` is the only conversion.
- **BJ-R4 (W4).** NULL follows the standard: an operator or function with a
  NULL operand yields NULL unless the standard says otherwise
  (`COALESCE`, `IS NULL`, `CASE`); three-valued logic for the boolean
  operators. A NULL result into a `NOT NULL` column is refused.
- **BJ-R5 (W5).** The primary key is not a SET target, as today (K2). It
  may be read inside an expression.
- **BJ-R6 (W6).** The expression's structure - operators, function names,
  referenced columns, parentheses - is part of the statement's shape; a
  literal is a value. `SET v = v + 1` and `SET v = v + 2` share a
  `pattern_id`; `SET v = v + 1` and `SET v = v - 1` do not.
- **BJ-R7 (W7).** No measurement gate opens any stage. The overhead is
  measured once, at the close (§5).
- **BJ-R8 (W8).** CN-7 is restated for the spec BJ makes, kept a concept
  note, neither deleted nor closed.
- **BJ-R9.** A qualifying row is written at most once per statement, on
  every path: the walk, a park and its resume, and an in-engine re-run.
  `[quiet-wrong]`
- **BJ-R10.** An expression's errors - overflow, division by zero, a value
  out of its target's range - are statement errors raised at the row that
  produced them, after the row qualified, with the expression's byte.
  An `UPDATE` matching no row raises none, as today.

**BJ does not do:**
- **Expressions anywhere but a SET value.** `INSERT ... VALUES`, the
  select list, `ORDER BY`, `GROUP BY`, and a WHERE beyond what
  `parser-v2.md` I10 already admits keep their refusals. The expression
  grammar BJ builds is the one they would use; admitting them is an order
  of its own each (I10's classification argument is per position).
- **`UPDATE ... FROM`** and multi-relation updates. They are not value
  expressions; BJ-Q7.
- **A delta record or a commutative write.** CN-7's; R1 there.
- **New column types.** `FLOAT64` stays refused (TY1); no `INTERVAL`.
- **Change anything on disk.** No record, page or catalog format moves.

## 1. Survey at `080cd55`

Read, not run.

### 1.1 The grammar and the lexer

- `[source-read]` **The SET list.** `ParseUpdate` (`src/parser/parser.cpp:2141-2188`)
  loops `ParseIdent`, `'='`, `ParseValue`, `','` (`:2151-2175`). There is
  no duplicate-target check here or in `CompileAssignments`
  (`src/exec/step_compiler.cpp:1355-1382`), and the apply loop assigns in
  list order (`src/server/command_dispatcher.cpp:7803-7810`), so
  `SET a = 1, a = 2` writes `2` today with no refusal.
- `[source-read]` **On a foreign-key column that is a live defect, not a
  last-wins.** The hoist pairs each fk column with its **first**
  assignment and stops (`command_dispatcher.cpp:7550-7554`), the per-row
  check applies that pair's verdict (`:7780-7784`), and the write keeps the
  **last** (`:7803-7810`). So `SET fk = <existing parent>, fk = <absent
  parent>` passes the check and writes a child pointing at an absent
  parent, with no error. Filed at `fdad71e4` as
  `docs/inflight/bugs/a-column-assigned-twice-checks-its-first-foreign-key-value-and-writes-its-last.md`,
  **fixed at `586ffa5f` by BJ-Q12's adoption** (`CompileAssignments` refuses
  a column named twice, `InvalidArgument` at the second name's byte) and the
  entry deleted at `bfdd8680`, as `docs/inflight/bugs/README.md` asks;
  `git show fdad71e4:<that path>` retrieves it. The paragraph above is the
  state at `080cd55`.
- `[source-read]` **No arithmetic tokens.** The punctuation is `(`, `)`,
  `,`, `;`, `*`, `.`, and the comparisons (`include/kds/parser/token.hpp:147-166`).
  `+`, `/`, `%` and `||` lex as `kError`.
- `[source-read]` **A minus before a digit is part of the literal.** The
  lexer reads `-7` as one `kIntLit` (`src/parser/lexer.cpp:125-131`), and
  `--` opens a line comment (`:73-77`). `[design]` So `v-1` lexes as `v`,
  `-1` and `v - 1` as `v`, `kError`, `1`; `v--1` is `v` and a comment.
- `[source-read]` **Most SQL words are identifiers to the lexer.** Only
  eleven are `kKeyword` - `JOIN`, `ON`, `AS`, `IN`, `EXISTS`, `NOT`,
  `BETWEEN`, `LEFT`, `RIGHT`, `FULL`, `OUTER` (`src/parser/lexer.cpp:38-43`);
  `AND`, `WHERE`, `LIMIT`, `OFFSET` are `kIdent`, and `BETWEEN`'s `AND` is
  matched by text (`parser.cpp:375-381`). `[design]` So the lexer cannot
  tell `x BETWEEN -5 AND -1` (a signed literal after `AND`) from `a AND -1`
  in an expression by the previous token's type; BJ-Q9 says where the
  minus is decided instead.
- `[source-read]` **No parenthesized expression.** `ParseSubquery` relies on
  it: *"this grammar has no parenthesized expressions (spec I10) - a '(' in
  predicate position can only open a SELECT or a value list"*
  (`parser.cpp:266-270`). `[design]` A SET value is not a predicate
  position, so this lookahead is untouched; a `(` in a SET value opens
  either an expression or a scalar subquery, told apart by `SELECT`.
- `[source-read]` **Scalar functions exist in one position.** AP-S4 admits
  a call in a WHERE comparison (`docs/spec/parser-v2.md:128`), with
  `DATE(timestamp)` and `NOW()` (`docs/spec/types.md` §3.2a;
  `include/kds/exec/functions.hpp`). A call in a SET value is refused
  `NotImplemented` (`parser.cpp:226-230`).

### 1.2 The fingerprint

- `[source-read]` The shape is folded from the token stream, fed by the
  lexer as each token is read (`src/parser/lexer.cpp:99`): an identifier
  or keyword hashes its folded text after `ShapeTag::kIdent`; every
  literal and placeholder is `ShapeTag::kValue`, except `NULL`, which is
  `ShapeTag::kNull`; punctuation and operators are their tag alone
  (`src/parser/fingerprint.cpp:133-190`, `:342-364`).
- `[design]` So BJ-R6 needs no new mechanism: each new operator token gets
  a tag of its own, and a column reference already hashes by name. A token
  that lexes today as `kError` makes the fingerprint invalid
  (`fingerprint.cpp:342-345`), so no statement containing a new operator
  ever had a storable hash, and adding the tokens moves no stored
  `pattern_id` - the argument `kNumLit` used (`:164-173`).
- `[design]` **The one hash that could move is a negative literal's.**
  `WHERE x = -7` and `x BETWEEN -5 AND -1` are one `kValue` per bound
  today. Splitting every `-` into its own token would make each `kMinus`,
  `kValue`, moving the `pattern_id` of every stored statement with a
  negative literal - a `kFingerprintVersion` bump under `fingerprint.hpp`'s
  rule. BJ-Q9 proposes the rule that moves none.
- `[design]` **A reserved word moves nothing only if it is not reserved.**
  `CASE`, `WHEN`, `THEN`, `ELSE`, `END`, `CAST`, `POSITION`,
  `CURRENT_DATE` are legal column names today. Reserving one would refuse
  every schema that uses it, against `CLAUDE.md`'s "nothing new is
  reserved lightly"; BJ-S2 matches them by text in context, as `AND` is.

### 1.3 How an UPDATE writes a row

- `[source-read]` **Compile before storage.** `UpdateInner` resolves the
  relation, then `CompileAssignments` (unknown column `InvalidArgument`,
  pk `Unsupported`), then `CompileWhere`, before any page is touched
  (`command_dispatcher.cpp:7442-7531`).
- `[source-read]` **Per row**, inside the walk's callback `apply`
  (`:7630-8028`): classify the version (`:7647-7649`); copy the payload out
  of the page (`:7663`); decode only the WHERE's columns and test it
  (`:7678-7686`); decode the whole row and resolve its spills
  (`:7694-7696`); take the tuple borrow and run first-updater-wins
  (`:7704-7770`); the forward foreign-key check (`:7780-7784`); keep
  `previous` when a Cabin, an index or an assertion needs it (`:7797-7801`);
  overwrite the SET targets in the decoded row with the assignment's
  `AstValue` (`:7803-7810`); encode the body (`:7880-7884`); then the
  assertion check, undo, overwrite in place, indexes and Cabin.
- `[design]` **The evaluation point is already there.** Between `:7801` and
  `:7803` the row is decoded whole, spills resolved, the borrow held and
  the conflict decided - which is exactly where a standard UPDATE evaluates
  its SET list. The before-image BJ-R2 needs is `row` itself at that
  point, not `previous`, which is filled only when a Cabin, an index or an
  assertion wants it (`:7797-7801`): every assignment is evaluated from
  `row` into a vector of new values, then written in. It must also run
  before the no-refuse window opens and the assertion reserves
  (`:7833`, `:7837`), because an expression can fail and a scalar
  subquery fetches pages, and neither may happen inside the window
  (BE-Q11).
- `[source-read]` **Types are checked at the encode, per row.** TY7 makes
  `EncodeOneValue` the only gate (`docs/spec/types.md:20`;
  `src/exec/row_codec.cpp:106`). `[design]` So today
  `UPDATE t SET int_col = 'abc' WHERE <matches nothing>` answers
  `UPDATED 0`. BJ-R3 types an expression at compile, so a mistyped
  expression is refused whether or not a row matches. A bare literal keeps
  today's behaviour (BJ-Q3 (c)).
- `[source-read]` **A decoded value does not carry its column's type.**
  `uint64` decodes into `kInt` with `int_val` cast lossily above
  `INT64_MAX` and the digits in `raw_int_text` (`row_codec.cpp:489-507`);
  `bool` decodes into `kInt` 0 or 1 (`:509-515`); `DATE`/`TIMESTAMP` are
  `kInt` too (TY5). `[design]` The evaluator therefore types by the
  column's `type_val`, resolved at compile, never by the `AstValue` kind.

### 1.4 The foreign-key hoist assumes a literal

- `[source-read]` The fk columns a SET list touches are found once
  (`command_dispatcher.cpp:7546-7556`), and their parents resolved once
  before any row: *"A SET assigns a literal, so the parent set is a
  property of the statement"* (`:7563-7586`; `docs/spec/foreign-keys.md`
  §2a, which hoists *"an UPDATE's from its SET body"*). The per-row check
  applies the held verdict (`:7780-7784`).
- `[design]` `[quiet-wrong]` With an expression the parent is a per-row
  value. Checking the hoisted parent of an expression that is not
  constant would check a key the row never gets - a child row written
  pointing at an absent parent, with no error. BJ-Q10.

### 1.5 What keeps a row from being written twice

`[design]` A literal SET is idempotent, so a path that applied it twice
to one row has always been invisible. `SET v = v + 1` is not. Three facts
keep a row single-written today; BJ makes each load-bearing:

- `[source-read]` **No Halloween problem.** An UPDATE overwrites in place
  and never migrates a tuple (`command_dispatcher.cpp:7887-7900`,
  invariant 13), and the pk is not a SET target (K2), so the walk, which
  advances in key order on a btree, never meets a row it already wrote.
- `[source-read]` **A park resumes past what it wrote.** `WalkCursor`
  carries the key the walk stopped at, *"which it has not written"*, and
  the statement's row count (`include/kds/server/session.hpp:92-110`); the
  resume re-enters the same snapshot and scope (`command_dispatcher.cpp:7363-7387`).
- `[source-read]` **A whole re-run happens only for a statement that wrote
  nothing.** `EndWrite` drops the blocker of a statement whose trail grew,
  and its comment names this very case: *"re-applying `SET v = v + 1` to
  the rows it did write would be a second increment"*
  (`command_dispatcher.cpp:9022-9035`); the stop arm answers the refusal
  rather than parking when nothing was written (`:8114-8116`); BA-S14's
  in-engine re-run on `worktree-ba-open-marks` uses the same trail test.
- `[source-read]` **The resume skips by key or by position.** A btree
  resume descends by the cursor's key (`WalkBtreeLeaves`, `:5845` on), a
  heap resume by `(page, slot)`, and a row that appears behind the cursor
  during the park is invisible to the statement's snapshot
  (`include/kds/server/session.hpp:68-75`).

So the guard exists and was written with a non-idempotent SET in mind;
what is missing is a cell that would see a second application, because
no write before BJ could produce one. BJ-S1 adds them (BJ-R9).

### 1.6 What the specs say that BJ changes

- `[source-read]` `parser-v2.md` I10 (`:128`): a call in *"any value
  position: ... a `SET` value"* is refused, and *"arbitrary expression
  trees are excluded"*.
- `[source-read]` `types.md` §3.2: *"There is none - no `+`, no `*`, no
  expressions (the grammar has none)"*; TY8: *"`NOW()`, `CURRENT_DATE`,
  arithmetic on dates: none"* (`types.md:21`).
- `[source-read]` `manual/sql/sql.md:579`: `SET <col> = <val>`;
  `docs/spec/client-manual.md:310-311` says a SET value is a literal and
  not an expression.
- `[source-read]` `foreign-keys.md` §2a: the hoist from the SET body.
- `[design]` Each is restated at the stage that makes it false (§3), not
  before.

## 2. Rulings

### BJ-R1 — The scope is the reference dialect's value expression (W1)

The forms, by family, each a stage (§3), each refused `NotImplemented` at
its byte until its stage lands:

1. **Core:** column references to the target relation, literals, `NULL`,
   parentheses, unary `+`/`-`, binary `+ - * / %`, string `||`.
2. **Conditional and null-handling:** `CASE` (simple and searched),
   `COALESCE`, `NULLIF`, `GREATEST`, `LEAST`; the comparisons, `AND`, `OR`,
   `NOT`, `IS [NOT] NULL` as boolean values.
3. **Conversion:** `CAST(x AS type)` and the reference dialect's `::type`
   if BJ-Q1's dialect has it.
4. **Scalar functions:** the reference dialect's numeric and string
   functions over the engine's types (`ABS`, `ROUND`, `TRUNC`, `MOD`,
   `UPPER`, `LOWER`, `LENGTH`, `SUBSTRING`, `TRIM`, `POSITION`, `REPLACE`,
   ...), the list fixed by BJ-Q5; and BJ-Q4's time arithmetic.
5. **Scalar subquery:** `SET a = (SELECT ...)`, correlated or not, under
   the statement's snapshot (BJ-Q6).

`DEFAULT` as a SET value and the row-value form `SET (a, b) = (...)` are
BJ-Q7's.

### BJ-R2 — The before-image, and one assignment per column (W2) `[quiet-wrong]`

- Every SET expression is evaluated against the row as the walk read it -
  `row` after its spills are resolved and before the first assignment.
  The new values are collected, and only then written into the row. A
  later assignment never reads an earlier one's result.
- A column named twice in one SET list is refused `InvalidArgument` at the
  second name's byte, at compile. This changes today's silent last-wins
  for literals too (§1.1), which is the standard's rule and a refusal, not
  a wrong answer.

### BJ-R3 — Types, without implicit conversion (W3)

- **Operators and functions have declared signatures**, and an
  expression's type must be its target's. The types are the column types
  the catalog defines (`include/kds/catalog/well_known.hpp:511-548`):
  `int8`, `int16`, `int32`, `int64`, `uint64`, `bool`, `varchar(N)`,
  `char(N)`, and `types.md` §1's `DATE`, `TIMESTAMP`, `DECIMAL(p, s)`,
  `DECIMAL128(p, s)`. A signature may mix types where the reference
  dialect's does - `DATE + int` is BJ-Q4's - and mixing types is otherwise
  refused. Whether two integer widths, or two decimal precisions at one
  scale, count as one type is BJ-Q2.
- **A literal has no type until its context gives one**: the other
  operand's, or the target's for a bare literal, coerced at compile by the
  routine `EncodeOneValue` uses (types.md §3.1), so a literal that does
  not fit is a positioned compile error. `[design]` **An integer literal
  that scales a decimal is scale 0**: in `price * 2` over `DECIMAL(10,2)`
  the `2` is an integer, the product keeps scale 2, and BJ-Q3 has nothing
  to narrow; the reference dialect does the same. **A bare literal SET
  value keeps today's per-row gate** (BJ-Q3 (c), **conditional on its
  mark**), so it is the one literal not coerced at compile.
- **Results**, `[design]`, each to be checked against BJ-Q1's dialect at
  BJ-S3: integer arithmetic is checked (`OutOfRange` on overflow, never a
  wrap); integer `/` truncates toward zero and `%` takes the dividend's
  sign; `uint64` arithmetic is unsigned and a negative result is
  `OutOfRange`; `DECIMAL` `+`/`-` need one scale and keep it, `*` of two
  decimals adds the scales, `/` and a scale change are BJ-Q3's; a division
  by zero is refused; `||` yields a string whose length must fit a
  `char(N)` target's `N`, or the row is refused - nothing is truncated (a
  `varchar`'s N is a cell width, so a longer value spills, BJ-S3's review).
- **Only `CAST` converts**, with the reference dialect's rules for which
  pairs it admits, and its rounding for a scale it narrows (BJ-Q3).

### BJ-R4 — NULL (W4)

The standard's propagation and three-valued logic, as `docs/spec/null.md`
already holds them for predicates. `SET counter = counter + 1` on a NULL
`counter` leaves it NULL and counts the row as updated; the manual says
so beside `COALESCE`. A NULL into a `NOT NULL` target is refused at the
row.

### BJ-R5 — The pk is read, never written (W5)

`CompileAssignments`' refusal stands unchanged. `SET v = id * 10` is
admitted.

### BJ-R6 — Structure in the shape, literals as values (W6)

- Each new operator token gets its own `ShapeTag`. A column reference and
  a function name already hash by folded text; a literal is `kValue` and
  `NULL` keeps `kNull`, as today (§1.2).
- **No stored `pattern_id` moves**, by BJ-Q9 (a)'s lexing rule
  (**conditional on its mark**; (c) would bump) and §1.2's argument, so `kFingerprintVersion` does not move. If BJ-S2's corpus
  check finds a stored hash that does move, the stage stops for a ruling
  rather than bumping.

### BJ-R7 — The overhead is measured, nothing gates (W7)

§5.

### BJ-R8 — CN-7 restated (W8)

`docs/conceptnotes/cn7-delta-verbs.md` is revised with BJ-S0: §1's
"a delta cannot be written in one statement" becomes what BJ makes true,
and the note's case for `INC`/`DEC` is restated as what remains after BJ -
the delta record, the defined inverse, the commutative intent and a
future ledger. Its status, its rulings R1-R3 and its open items stay.

### BJ-R9 — At most once per row per statement `[quiet-wrong]`

The three facts of §1.5 become rules with cells: the walk never revisits
a written row, a resume never re-offers one, and an in-engine re-run runs
only a statement that wrote nothing. A path that would break any of them
is refused, not repaired, in BJ.

### BJ-R10 — Where an expression fails

After the row qualified and its conflict is decided, before anything of
the row is written. The refusal names the expression's first byte and the
row's pk. In autocommit the statement's transaction aborts; inside
`BEGIN` the transaction is poisoned, as every mid-statement write failure
is (the AS9 resolution).

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BJ-S0 | **The order** | This file, its review, `raft-marks-2026-10-10.md`'s sections, `index.md`'s row, CN-7 revised (BJ-R8) | S |
| BJ-S1 | **Red first, and the census** | Cells red for: a duplicate SET target (BJ-R2); `SET a = b, b = a`, parsed from BJ-S2 and pending until BJ-S4 can run it; BJ-R9's three paths with a write that counts its applications (a test seam), so a second application is visible. The census: every reader of `Assignment::val` (the fk hoist is one), every caller of `CompileAssignments`, and every spec sentence of §1.6 | S |
| BJ-S2 | **Lexer, AST, parser, fingerprint** (BJ-R1 family 1, BJ-R6) | The operator tokens and BJ-Q9's minus rule; an expression node in the AST, `Assignment` holding one; precedence and associativity per the reference dialect; the fingerprint corpus re-run, no stored hash moved; a statement with a family-2..5 form refused `NotImplemented` at its byte; **a statement with a parsed family-1 expression is refused `NotImplemented` at execution until BJ-S4**, because the fk hoist and the apply loop read `Assignment::val` directly and a half-built evaluator must never write a row | M |
| BJ-S3 | **Typing at compile** (BJ-R3, BJ-R4) | A typed expression tree resolved against the relation's schema; the rules of BJ-R3 as a table in `types.md`; every refusal at compile with both types and the byte; literal coercion through the existing routine | M |
| BJ-S4 | **Evaluation and the write** (BJ-R2, BJ-R5, BJ-R9, BJ-R10) | The evaluator over a decoded row, typed by `type_val`; `UpdateInner` evaluates every assignment against the before-image, then writes; BJ-Q10's fk path; BJ-S1's cells green; cells for overflow, division by zero, NULL, `NOT NULL`, the swap, a varchar overflow, and an expression over a spilled value; the two-core rig: two autocommit `READ COMMITTED` sessions `SET v = v + 1 WHERE id = k` N times each, the final value 2N plus nothing lost, each refusal counted and retried by the driver (a parked multi-row walk resumes under its old view, so a refusal there is the contract, not a loss); the equal-index-sort-keys bug named where `v` is a covered index column, since a counter updated ~600 times reaches it (`docs/inflight/bugs/a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`) | M |
| BJ-S5 | **Families 2 and 3**: conditional, null-handling, boolean values, `CAST` | Each form's cells, NULL included; `CAST`'s admitted pairs and its rounding per BJ-Q3; BJ-Q7's `SET (a, b) = (x, y)` | M |
| BJ-S6 | **Family 4**: scalar functions and time arithmetic | BJ-Q5's list, BJ-Q4's time forms, each function's purity in `functions.hpp`; `NOW()` one value per statement, carried across a park as today (`StatementContext`) | M |
| BJ-S7 | **Family 5**: the scalar subquery | An uncorrelated one evaluated once before the walk, under the statement's snapshot; more than one row refused; no row is NULL; every correlated one refused `NotImplemented` (BJ-Q6, revised) | M |
| BJ-S8 | **The close** | §5's measurement; the text of §1.6 restated (`parser-v2.md` I10 for the SET position only, `types.md` §3.2 and TY8, `foreign-keys.md` §2a, `sql.md`'s UPDATE, `client-manual.md`, `CLAUDE.md`'s row); `known-gaps.md` for what BJ leaves (§0's list) | S |

**Order.** S1..S8 in sequence. S4 starts only after BA's code from
`worktree-ba-open-marks` is on `main`, because both change
`UpdateInner` and `DispatchAsync` in `command_dispatcher.cpp` and BJ-R9's
cells must run against BA-S14's re-run. AQ's first evaluation stage and
BJ-S4 share the evaluator; whichever comes first builds it (header).

## 4. Items for the operator

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BJ-Q0 | **Open BJ**, with BJ-S0..S8 and BJ-R1..R10 as written | process | Yes | **Adopted** on the operator's standing go-ahead of 2026-10-10 (go-ahead-achieving-milestone): BJ opens |
| BJ-Q1 | **The reference dialect** W1's "general commercial DB" means, for every rule BJ does not state itself | scope | **PostgreSQL 18**, the engine's comparison floor (BA) and the one whose source the tree already cites; its operator precedence, its function names and its error conditions, restricted to the engine's types. Where it converts implicitly, W3 wins and BJ refuses | **Adopted** (standing go-ahead 2026-10-10): PostgreSQL 18 |
| BJ-Q2 | **What "one type" is under W3** for two members of one family: (a) the family is one type - `int32 + int64` is admitted and widens to `int64`, `DECIMAL(10,2) + DECIMAL(18,2)` is one scale; (b) the exact type - every width or precision difference needs a `CAST` | design | **(a) for width and precision within a family, never across families or scales.** A width difference loses no value and every commercial engine admits it; a scale difference changes what the digits mean, which TY6 already refuses. (b) makes `SET small = small + 1` over an `int32` column depend on the literal's typing alone and `SET a = a + b` over mixed widths unwritable without a cast. **Ask: is widening within a family an implicit conversion under W3?** | **Adopted, revised** (standing go-ahead 2026-10-10): **(b), the exact type**, not CLA's (a). W3 reads *no implicit conversion*, and a width or precision widening is one by its letter; (a) cannot be taken back once clients rely on it, (b) can be relaxed later with no wrong answer. A literal still takes its operand's type, so `SET small = small + 1` needs no cast. Reconsidered if the operator reads W3 as admitting widening |
| BJ-Q3 | **Decimal results and rounding.** (a) A result whose scale differs from its target's is refused, and `ROUND(x, s)` or `CAST` is how to narrow it, rounding half away from zero (PostgreSQL's `numeric`); `/` yields the dividend's scale plus the reference dialect's extra digits, then must be narrowed explicitly. (b) as (a), rounding half-even, the rule `AVG` uses (`aggregate.md` §3.4). (c) Separately: a bare literal SET keeps today's per-row gate (TY7) rather than becoming a compile-time refusal | design, **[quiet-wrong] if a narrowing were implicit** | **(a), and (c) yes.** (a) follows BJ-Q1's dialect for an explicit narrowing and keeps every implicit one refused; the engine's `AVG` rule is a statement about an aggregate's own answer, not about a conversion the client wrote. (c) keeps `UPDATE ... SET int_col = 'abc'` matching nothing answering `UPDATED 0`, as it does today, so no client sees a new refusal from BJ | **Adopted** (standing go-ahead 2026-10-10): (a) and (c) yes; premise re-read at `080cd55`, TY7's per-row gate is unchanged |
| BJ-Q4 | **Time arithmetic**, against TY8: (a) `DATE ± int` (days) → `DATE`, `DATE - DATE` → `int32` days, `NOW()` and `CURRENT_DATE` in a SET value; no `TIMESTAMP` arithmetic, since there is no `INTERVAL` type (TY1); (b) none in BJ, TY8 stands | scope | **(a).** It is what the reference dialect offers over the types the engine has; TY8's reason - *"a value function imports an evaluation-time question"* - is answered by AP-S4's statement instant, which a SET reuses. `TIMESTAMP` arithmetic waits for an `INTERVAL` type, which is its own decision. **(Added by BJ-S0's review.)** `CURRENT_DATE` takes no parentheses, so in a SET value it cannot be told from a column of that name by context: **a column of the target relation with that name wins, so the function is unreachable in a SET value for a relation that has such a column** - the manual says so; `NOW()` has parentheses and has no such case | **Adopted** (standing go-ahead 2026-10-10): (a) |
| BJ-Q5 | **The function list for BJ-S6** | scope | The reference dialect's numeric (`ABS`, `ROUND`, `TRUNC`, `MOD`, `SIGN`, `CEIL`, `FLOOR`) and string (`UPPER`, `LOWER`, `LENGTH`, `SUBSTRING`, `TRIM`, `LTRIM`, `RTRIM`, `POSITION`, `REPLACE`, `LPAD`, `RPAD`, `CONCAT`) functions over the engine's types, plus BJ-Q4's. Anything else is refused by name as an unknown function, as AP-S4 refuses one today | **Adopted** (standing go-ahead 2026-10-10): as listed |
| BJ-Q6 | **A scalar subquery in a SET value** | design, **[quiet-wrong]** | `[source-read]` A view always sees its own transaction's writes (`include/kds/txn/read_view.hpp:75-77`), and there is no per-statement command id, so a subquery evaluated per row would see the rows this statement already wrote - a wrong answer the standard forbids. **(Revised by BJ-S0's review.)** A per-row sub-chain also walks its relation shared under the write walk's exclusive leaf, which over *another* relation is the page-latch cycle of `docs/inflight/bugs/a-write-walks-subquery-reads-pages-under-its-exclusive-leaf-hold.md` (two reactors hang, no wrong answer). **So: an uncorrelated subquery, over any relation, is evaluated once before the walk (that bug's fix (c)) and its value used for every row; every correlated subquery is refused `NotImplemented` at its byte**, whichever relation it reads, until a statement-level visibility and that bug's fix exist, each its own order. **Stated difference:** a WHERE-position subquery today is re-run per row and sees the statement's earlier rows (that bug's fix (c) note); a SET-position one sees the pre-statement state, and the manual says so. The cell is `SET v = v + (SELECT SUM(v) FROM t)` over N rows, every row raised by the pre-statement sum | **Adopted as revised** (standing go-ahead 2026-10-10); premise re-read at `080cd55`, `read_view.hpp:75-77` unchanged |
| BJ-Q7 | **Forms outside a value expression**: `DEFAULT` as a SET value, `SET (a, b) = (x, y)`, `UPDATE ... FROM` | scope | **`SET (a, b) = (x, y)` in BJ-S5** (standard SQL, and BJ-R2 already gives it its meaning); **`DEFAULT` refused** until column defaults exist (no `DEFAULT` clause in the DDL today); **`UPDATE ... FROM` refused**, not a value expression and not standard SQL, its own order if wanted | **Adopted** (standing go-ahead 2026-10-10) |
| BJ-Q8 | **Other positions** - `INSERT ... VALUES`, the select list | scope | **Out of BJ**, each its own order on the same grammar (§0). `parser-v2.md` I10 keeps its refusals there | **Adopted** (standing go-ahead 2026-10-10) |
| BJ-Q9 | **The minus.** (a) The lexer keeps fusing `-` before a digit into a signed literal, as today, and lexes any other `-` as the operator; the parser, in binary-operator position, reads a signed literal as minus and its magnitude. (b) The lexer decides by the previous token's type. (c) Every `-` is a token, and `kFingerprintVersion` moves | design | **(a).** Every statement that parses today keeps its tokens, so no stored `pattern_id` moves. Its cost is that one meaning has two spellings with two shapes: `v -1` hashes as identifier, value and `v - 1` as identifier, minus, value - two patterns, one answer, no correctness question; the manual says to write the space. (b) breaks `x BETWEEN -5 AND -1`, `LIMIT -1` and a `THEN -1`, because `AND`, `LIMIT` and `THEN` are identifiers to the lexer (§1.1); (c) is a format bump for no gain. Cells pin `BETWEEN -5 AND -1`, `v -1` = `v - 1` in value, `v--1` as a comment, and `? - 1`. **(Added by BJ-S0's review.)** The rule covers a signed decimal literal (`v -1.5` reads as minus and `1.5`) and a magnitude above `INT64_MAX` (`v -9223372036854775808` is read from the digits, never from the fused value, so `INT64_MIN`'s magnitude survives) | **Adopted** (standing go-ahead 2026-10-10): (a) |
| BJ-Q10 | **A foreign-key column assigned by an expression** | design, **[quiet-wrong]** | **(Revised by BJ-S0's review.)** `[source-read]` The first proposal resolved the parent per row inside the write scope, on the strength of AH-R1's self-referencing exception (`command_dispatcher.cpp:3576-3601`, AY-Q3). That exception has no working precedent: a self-referencing foreign key cannot be declared (`ASelfReferencingForeignKeyCannotBeDeclared`, `tests/foreign_key_test.cpp`), so the arm is unreachable. And the descent is a hang: the walk holds its child leaf exclusive across the per-row work, a parent descent takes parent leaves shared, and a parent `DELETE` holds a parent leaf exclusive while its reverse walk reads the child - the page-latch cycle of `docs/inflight/bugs/a-write-walks-subquery-reads-pages-under-its-exclusive-leaf-hold.md`, whose wait is an unbounded spin, so two reactors hang with no subquery involved. **So: a constant expression - no column reference, no `NOW()`, no subquery - is folded at compile and keeps the hoist; any other expression assigned to a foreign-key column is refused `NotImplemented` at its byte** until that bug's fix (b), a try-shared descent that refuses retryable instead of spinning, exists. A refusal is the truthful state; per-row resolution is its own order after the fix. Cells: a non-constant expression into an fk column is refused at compile; a constant one is checked against its folded value | **Adopted as revised** (standing go-ahead 2026-10-10); premise re-read at `080cd55`, the self-referencing arm is unreachable and the page-latch cycle bug is open |
| BJ-Q11 | **The measurement** (§5), at BJ's close | process | As written | **Adopted** (standing go-ahead 2026-10-10) |
| BJ-Q12 | **The duplicate-target defect ahead of BJ** (§1.1): refuse a column assigned twice in `CompileAssignments` now, as a fix of its own, or with BJ-S4 | sequencing, **[quiet-wrong] while open** | **Now, as its own fix.** It is a live fk bypass at `080cd55`, the fix is one compile-time check with a byte, and it changes no statement that means anything: a client that names a column twice gets a refusal instead of an unchecked parent | **Adopted** (standing go-ahead 2026-10-10): the fix lands first, as BJ-S1's opening commit; premise re-read at `080cd55`, the defect is real |

## 5. Measurement

Once, at BJ's close, in `build-release`, per `CLAUDE.md`'s Session
Workflow step 3. A = the commit BJ opens from, B = the commit that closes
it, each named by `git describe --tags`, results under `bench/v3.0.0/`,
`bench/README.md`'s rules.

- **The literal path, A against B**: `UPDATE ... SET v = <literal> WHERE
  id = ?` and a 10,000-row range `SET`, interleaved. The expectation is
  flat; a resolved cost is reported, not gated (W7).
- **The expression's own cost, on B**: `SET v = v + 1` against `SET v =
  <literal>`, point and range, and a two-assignment swap, so the
  evaluator's per-row price is stated.
- **The client's former round trip**: `SELECT` then `UPDATE ... SET
  <literal>` against one `SET v = v + 1`, one session, so the manual can
  say what BJ saves.

## 6. Row status

### BJ-S0 — written 2026-10-10

Written on `worktree-bj-expression-update` from `080cd55`. No file under
`src/`, `include/` or `tests/` moved, and no suite ran.

**Its review** (one independent pass against the tree at `080cd55`) found
20 items; applied:
- **A live defect**, filed: a column assigned twice checks its first fk
  value and writes its last (§1.1, BJ-Q12).
- **BJ-Q9 rewritten.** The first proposal decided the minus by the previous
  token's type, which breaks `x BETWEEN -5 AND -1`: `AND` is an identifier
  to the lexer.
- **BJ-Q6 rewritten.** The first proposal said the snapshot hides the
  statement's own writes; a view sees its own transaction's, so a per-row
  subquery over the target would read rows already written.
- **§0's lost update.** The client's two-statement pattern loses an update
  silently under `READ COMMITTED`; it was described as a refusal.
- **§1.5 cited the guard itself** (`EndWrite`, the stop arm, the resume),
  not a comment.
- **BJ-R3** restated as declared signatures, so BJ-Q4's `DATE + int` is not
  a contradiction; an integer literal scaling a decimal is scale 0; the
  bare literal's exception stated; the arithmetic rules tagged `[design]`.
- **§1.3**: the before-image is `row`, not `previous`, and evaluation runs
  before the no-refuse window.
- **BJ-Q10**: per-row resolution is AH-R1's self-referencing exception,
  with its refusal under contention stated.
- **BJ-S4's rig** narrowed to the shape whose answer is 2N; the
  equal-sort-keys index bug named for a counter column.
- **Reserved words** kept unreserved (§1.2); `NULL`'s `kNull` tag;
  citation offsets; the BA reference; the index row's item list; the
  describe at the close; CN-7's NULL difference, its AX sentence and §9's
  wording.

**Declined:** stating the rules once instead of in §0, §2 and §4 - the
house shape every order takes, as BG-S0's review also declined.

### BJ-S0 — the `critics-developer` review of `fdad71e4`, applied

On `worktree-bj-expression-update` at `fdad71e4`, a review of the committed
order against `080cd555` checked about 40 citations and found four line
drifts (corrected: `parser.cpp:2151-2175`, `command_dispatcher.cpp:7630-8028`,
`:7678-7686`, `:7887-7900`), a false premise (the clone is not shallow:
`git describe` reads `v2.7.0-723-g080cd555`, so the header and BJ-S8 no
longer say it must be deepened), and a Korean quotation in the marks file
(translated; every document is English). **Applied:**
- **BJ-Q10 revised, high.** Per-row parent resolution inside the walk is the
  page-latch cycle of `a-write-walks-subquery-reads-pages-under-its-exclusive-leaf-hold.md`,
  and the self-referencing arm it cited as precedent is unreachable. A
  non-constant expression into a foreign-key column is now refused
  `NotImplemented`; a constant one keeps the hoist.
- **BJ-Q6 and BJ-S7 revised, high.** A correlated subquery over another
  relation closes the same cycle, so every correlated subquery is refused;
  an uncorrelated one is evaluated once before the walk. The SET-position
  and WHERE-position visibility difference is stated.
- **BJ-S2's done condition**: a parsed family-1 expression is refused
  `NotImplemented` at execution until BJ-S4, since the hoist and the apply
  loop read `Assignment::val` directly.
- **BJ-R3's literal rule and BJ-R6's hash rule** say they are conditional on
  BJ-Q3 (c) and BJ-Q9 (a).
- **`CURRENT_DATE`** without parentheses: a column of that name wins
  (BJ-Q4). **BJ-S5** names `SET (a, b) = (x, y)`. **BJ-Q9** covers a signed
  decimal and a magnitude above `INT64_MAX`.
- **The bug entry** gains two costs (the reverse order's false refusal and
  the skipped `S` fence). **CN-7** drops a row from its "what remains" list,
  conditions the two-log-records sentence on O5, narrows R1's reading to what
  was ratified, marks its (BJ) insertions, fixes the stale AX sentence of
  §7, and notes O2's divergence from BJ-Q2.

**Rejected:** none; every finding was applied. The review's note that local
`main` lags `origin/main` by 32 commits is a fact about the checkout, not a
change to the order.

### BJ-S0 — the adoption of BJ-Q0..Q12

On the standing go-ahead of 2026-10-10, thirteen items adopted at once:
BJ-Q0 opens BJ. `[quiet-wrong]` items first: **BJ-Q3** (a)/(c), **BJ-Q6**
as revised, **BJ-Q10** as revised and **BJ-Q12** (the fix first), each with
its premise re-read at `080cd555` (the mark column says what was read).
**BJ-Q2 is adopted revised, (b) not (a)**: the one adoption that departs
from CLA's written proposal, because (a) is an implicit conversion by W3's
letter and cannot be taken back. The operator's reading of W3 would
reverse it. The rest follow the proposals as written.

### BJ-S1 — red first, and the census (built 2026-10-10)

On `worktree-bj-expression-update`, after `586ffa5f`. **Not measured; measured
at the milestone's close** (§5).

**The census**, `[source-read]` at the same tree:
- Every reader of `Assignment::val`: two, both in `UpdateInner` - the
  foreign-key hoist (`command_dispatcher.cpp`, the `fk_assignments` build)
  and the apply loop that overwrites the decoded row. `sim/` reads
  `Oracle::Assignment`, its own single-value type, never the parser's.
- Every production caller of `CompileAssignments`: one, `UpdateInner` (the unit test `step_compile_test.cpp` is the other); a parked
  statement re-enters `UpdateInner`, so a resume re-compiles and cannot skip
  a compile-time refusal.
- Every spec sentence BJ-S8 restates is listed in §1.6 and was re-read; none
  has moved since.

**Red cells and what they showed:**
- A duplicate SET target. Built with BJ-Q12's fix (`586ffa5f`, adopted
  under BJ-Q12): `CompileAssignmentsTest` and `ForeignKeyCheckTest` carry the
  cells, and the review reasoned the fk cell red against the unfixed tree
  (the hoist checks the first value, the write keeps the last). The cell was
  not run red; it was written with the fix in one step.
- `SET a = b, b = a` cannot be parsed before BJ-S2 and stays pending here.
- **BJ-R9's three paths**, `tests/bj_single_write_rig_test.cpp`, through a
  new seam `SetAfterUpdateRowAppliedForTest` that counts the calls per pk:
  a multi-leaf walk (1200 rows, each once), a walk parked midway and resumed
  after a `ROLLBACK` (rows before the held one written once, the rest once),
  the same with a `COMMIT` (always `TXN_CONFLICT` at the resumed row, no row twice), and a statement
  that wrote nothing before its park and then ran whole (each row once).
  **They are green, not red**: the guards hold on this branch at `bfdd8680`,
  as §1.5 said they would. That the cells read the counter was checked by
  mutation - calling the hook twice per row fails all four; that they catch
  a broken guard (a resume from the leftmost row) is by reasoning only. BJ-S4 makes them load-bearing; BA-S14's
  in-engine re-run is the path they do not cover until BA is on `main`.

### BJ-S2 — lexer, AST, parser, fingerprint (built 2026-10-10)

On `worktree-bj-expression-update`, after `bfdd8680`. **Not measured;
measured at the milestone's close** (§5).

- **Tokens.** `kPlus`, `kMinus`, `kSlash`, `kPercent`, `kConcat` (`||`). A
  `-` before a digit is still part of the signed literal and `--` still a
  comment, so every statement that lexed before lexes the same (BJ-Q9 (a)).
  `*` stays `kStar` and the parser decides what it is.
- **AST.** `Expr` (literal, column, unary, binary; immutable and shared, so
  `UpdateStmt` stays copyable) and `Assignment::expr`. **One bare literal
  keeps `Assignment::val` and a null `expr`**, so the per-row gate at the
  encode (BJ-Q3 (c)) and every reader of `val` are untouched; anything else,
  a column reference and a parenthesised literal included, is an `expr` with
  `val` unset.
- **Parser.** Precedence climbing in PostgreSQL's order (`||` loosest, then
  `+ -`, then `* / %`, then a unary sign), left associative; a signed
  literal in binary position is read as a minus and its magnitude, from the
  digits, so `INT64_MIN`'s magnitude survives; nesting is bounded at 64
  (`Unsupported`). Every family-2..5 form is refused `NotImplemented` at its
  byte and names the stage that builds it (BJ-S5, S6, S7); a column named
  `case` stays a column.
- **Execution is refused until BJ-S4.** `CompileAssignments` refuses an
  assignment holding an `expr` `NotImplemented` before any row is read,
  because the foreign-key hoist and the apply loop read `val` directly.
- **Fingerprint.** Five tags appended (15-19). **No stored `pattern_id`
  moved**: the golden corpus's existing lines are byte-identical in their
  `pattern_id`, `arg_hash` and `fetch_id` columns (the regenerated file
  differs from the old one only in two verdicts and 25 appended lines), so
  `kFingerprintVersion` does not move (BJ-R6). The two verdicts that moved,
  each by this stage's intent: `UPDATE t SET a = x` is `ok` at the parser
  (the compile refuses it) and `SET a = (SELECT ...)` is `NotImplemented`
  rather than `InvalidArgument`. `FunctionConjunctTest`'s pin of the old
  message for `SET k = NOW()` was updated to the new one.
- **Shape evidence** (`tests/parser_set_expression_test.cpp`): `v + 1` and
  `v + 2` share a `pattern_id` and differ in `arg_hash`; `v + 1`, `v - 1`,
  `v * 1`, `v / 1`, `v % 1`, `v || 1`, `w + 1`, `(v + 1)`, `-v` and
  `v + 1 + 1` are ten distinct patterns; `x = -7` and `x BETWEEN -5 AND -1`
  hash as they did.
- **Suite:** 3342/3342 at this stage's tree, `HeapSuspensionIsLifted` disabled
  as before.

### BJ-S2 — the `critics-developer` review of `6a4fb896`, applied

On `worktree-bj-expression-update` at `6a4fb896`, the review found, and the
tree now fixes:
- **A crash the stage made reachable (high).** The three binary levels are
  loops, so `1+1+1+...` leans entirely left and the nesting counter never saw
  it; a 2 MB statement parsed and then crashed the process freeing the tree.
  `Expr::height` is tracked and bounded at 256 (`Unsupported` at the
  operator's byte); the nesting bound of 64 stays for parentheses and signs.
  The cell parses a million `+` and 10,000 of each of `+`, `*`, `||`.
- **A pre-existing defect the grammar made reachable (high, quiet-wrong once
  BJ-S4 lands).** `Substitute` (`kwp_session.cpp`) pasted a bound negative
  value straight after a `-`, so `x -?` bound with -5 read `x --5`, a line
  comment that ate the rest of the statement. Fixed in the same stage with a
  space; the cell is red without it (`SET v = 100 -? WHERE id = ?` otherwise
  updates every row) and green with.
- **Wrong stage or code for four groups of not-yet-built forms**: `CAST`,
  `COALESCE`, `NULLIF`, `GREATEST`, `LEAST`, `IN`, `BETWEEN`, `NOT`, `::` and a
  `CASE` over a literal are BJ-S5's and are now named so; the `CASE` rule is
  "any token that does not end the value or continue an operator".
- **No cell for the compile refusal** that keeps the hoist and the apply loop
  from reading an empty `val`: `AnExpressionNeverWritesBeforeBjS4` asserts
  `NOT_IMPLEMENTED`, no `UPDATED`, and the row unchanged.
- **The depth refusal** was written twice; one helper.

**Rejected:** folding `MagnitudeOf` into `ParseValue` through a sign-stripped
token copy - it would save about fifteen lines and add a token constructor the
lexer owns, so the digit loop stays duplicated; removing
`NoStoredShapeMovedWithTheNewTokens` as a duplicate of two corpus lines - it
states the property in words and costs nothing. **Not done:** the select-list
window's `select_item_due_` after an operator, which matters only when BJ-Q8
opens the select list.

### BJ-S3 — typing at compile (built 2026-10-10)

On `worktree-bj-expression-update`, after `6a4fb896`. **Not measured;
measured at the milestone's close** (§5).

- **`exec::TypeSetExpression`** (`include/kds/exec/set_expr.hpp`,
  `src/exec/set_expr.cpp`): a parsed `Expr` resolved against the relation's
  schema into a `TypedSetExpr`, every column bound to its position, every
  literal coerced through `CoerceLiteralToColumn` and the new
  `CheckIntegerLiteralFits` (the integer gate `EncodeOneValue` already is,
  exposed rather than copied), every node carrying its type.
  `CompileAssignments` calls it for an expression assignment, so a mistyped
  one is refused by what is wrong with it - both types and the byte - whether
  or not a row matches; one that types is still refused `NotImplemented`
  (BJ-S4).
- **The rules as a table** in `docs/spec/types.md` §3.2b, marked as typing
  built and evaluation not.
- **BJ-Q2 (b), the exact type, is what the typer implements**: `int32 + int64`
  is refused naming both, and the tree's type must be the target's.
  An integer literal scaling a decimal under `*` is scale 0; decimal `/` is
  refused `NotImplemented` (it needs the narrowing BJ-S5/S6 build); date
  arithmetic `NotImplemented` (BJ-S6); timestamp arithmetic `Unsupported`.
- **Cells**: `tests/set_expr_typing_test.cpp`, 16 of them over a 13-column
  relation. **Suite 3360/3360** at this stage's tree with
  `HeapSuspensionIsLifted` disabled as before.
- **Stopped here.** BJ-S4 starts only after BA's code is on `main` (§3's
  order), and `worktree-ba-open-marks` is not merged at `080cd55`. BJ-S5..S8
  build on S4's evaluator, so they wait with it.

### BJ-S3 — the `critics-developer` review of `c3cf85fd`, applied

On `worktree-bj-expression-update`, after `c3cf85fd`, the review found, and
the tree now fixes:
- **A quiet-wrong write (high), older than BJ.** `EncodeOneValue`'s
  int8..int64 arm stored the literal the lexer wrapped
  (`SET i8 = 18446744073709551615` answered `UPDATED 1` and stored `-1`), and
  the typer's fit gate copied it. The arm refuses a wrapped literal
  `OutOfRange`, which covers an `INSERT`, a bare `SET` and the typer. The WHERE
  half of the same wrap stays in `an-integer-literal-past-int64-in-a-where-...md`.
- **Typing cost** was quadratic (a re-typed left subtree per level), not
  exponential as an earlier note said; the typer now types bottom-up with no
  hint and `Resolve` gives a context's type to the untyped literals in one
  walk. The `hint` parameter is gone.
- **The decimal-scalar rule was two rules** (`d * -2` typed, `d * - 2` did
  not; `d * NULL` and `d = 1.5 * 2` were refused). One `Settle` decides: under
  `*`/`/` against a decimal an integer literal or an expression of them is
  scale 0 and a NULL keeps the decimal's type.
- **A later assignment's refusal was hidden** behind the first expression's
  `NotImplemented`; `CompileAssignments` now judges every assignment and
  refuses `NotImplemented` last.
- **Refusal wording**: a literal's refusal named `column ''`; it now names the
  literal's kind and the target type. `NULL || NULL` into a number says the
  operator yields a string. `SetTypeName` spells the wide decimal
  `decimal(p,s)`, the only spelling a statement can write. Every typing
  refusal carries `(BJ-R3)`.
- **Spec text**: `types.md` §3.2b read "two decimals" for `*` (one `(p,s)`),
  "any arithmetic" for a date (binary), and a `varchar` length check that does
  not exist (only `char(N)` has one); all corrected, here and in BJ-R3.
- **Cells**: four new (the scalar rule over twelve spellings, the later
  assignment, the wording, the wrapped literal), 23 in the file.

**Rejected:** deleting the `Mismatch` helper into one message (it names a
different thing than `DoesNotFit`); folding constant expressions at compile so
`i8 = 200 - 100` types (the review lists it as by design, and BJ-S4's
evaluator is where a value is known); making `i64 = (NULL)` a compile
refusal (the NOT NULL verdict is the row's, BJ-R4). **Not done:** the
WHERE-side wrap, which is outside this order.

### BJ-S3 → BJ-S4 gate — BA's code lands on `main` (2026-10-10)

On the operator's word ("proceed with option 1", then, asked who merges,
"CLA merges and pushes") `worktree-ba-open-marks` (`2eea2720`, BA-S2..S4,
S7, S12..S15 and BA's close) was merged into `worktree-bj-expression-update`
at `836b9551` with no conflict, and pushed to `main` at `1a5176ae` after the
pre-push suite (3411 of 3411, `HeapSuspensionIsLifted` disabled). The merge
broke two of BJ-S1's cells, not the engine: a resumed `UPDATE` now commits
through BA-S7's parked-commit path, which the rig's held kicks never deliver,
so `bj_single_write_rig_test.cpp`'s waiter commits `relaxed`, as BA's own
`row_wait_wake_rig_test.cpp` does (`1a5176ae`).

### BJ-S4 — evaluation and the write (built 2026-10-10)

On `worktree-bj-expression-update`, after `1a5176ae`. **Not measured;
measured at the milestone's close** (§5).

- **`exec::EvaluateSetExpression`** (`src/exec/set_expr.cpp`): one `Evaluator`
  over a decoded row. Integer, uint64 and decimal values ride one 128-bit
  integer; every node's result is judged against **its own type** (an int8
  intermediate of 200 is `OutOfRange` though the final result would fit),
  with `__builtin_*_overflow` underneath so a decimal(38) pair cannot wrap
  the 128 bits; a decimal's bound is `10^p`. `/` truncates toward zero, `%`
  takes the dividend's sign, a zero divisor is `InvalidArgument` and is
  judged after NULL, so `NULL / 0` is NULL. A uint64 above `INT64_MAX`
  travels in `raw_int_text`, as `ValueAsUint64` reads it.
- **`CompileAssignments`** takes an optional `std::vector<CompiledAssignment>*`:
  the target's schema position (the row loop no longer finds a column by name
  per row), the typed tree, and for a foreign-key column the value its hoist
  resolves. The `NotImplemented (BJ-S4)` refusal is gone.
- **`UpdateInner`** evaluates every expression right after the row qualified
  and its conflict was decided - before the foreign-key check, before the
  no-refuse window, before any write (BJ-R2, BJ-R10) - into one per-statement
  `evaluated` vector, then writes them into the decoded row together. A failure
  names the failing operator's byte, the expression's first byte and the
  row's pk.
- **BJ-Q10, as adopted**: a constant expression into a foreign-key column is
  folded at compile and keeps the hoist; one that reads a column is refused
  `NotImplemented` at its byte (the refusal waits until every assignment has
  been judged). A constant whose fold fails hoists nothing, and the row that
  meets the failure raises it before any foreign-key check.
- **Cells**: `tests/set_expr_eval_test.cpp` (17: arithmetic, the swap, every
  type's overflow, uint64 above INT64_MAX, division by zero, NULL, NOT NULL,
  decimals, text and a spilled value, a failure in a later row, the foreign-key
  path); `tests/set_expr_two_core_rig_test.cpp` (two cores each add 1 to one
  row 300 times: the value is 600 and every retryable refusal is retried, none
  lost); the typing cells now run over a WHERE that matches nothing, and
  `ANegativeValueBoundAfterAMinusIsNotAComment` expects the statement to run
  and change nothing. **Suite 3429/3429** at `d13c79ea`, 3434/3434 after the review (below).

### BJ-S4 — the `critics-developer` review of `d13c79ea`, applied

On `worktree-bj-expression-update`, after `d13c79ea`, the review found, and
the tree now fixes:
- **A NOT NULL refusal named `(byte 0)`** (medium, BJ-S4's own): a computed
  value carries no position, so the encode's refusal printed none. The row
  loop stamps the expression's first byte on each evaluated value.
- **BJ-R10's wording.** A value that evaluates but does not fit its target -
  a NULL into a NOT NULL column, a `char(N)` result too long - is refused by
  the row's encode, after the no-refuse window opens and the assertion
  reserves, and the char refusal names no byte. It is safe (the failure takes
  the normal mid-statement path and the abort undoes the reservation); BJ-R10's
  "before anything of the row is written" is therefore **read as the
  evaluation's own failures**, and `types.md` §3.2b and the row loop's comment
  say the other two are the encode's verdicts.
- **Cuts**: one `values` vector (a bare literal's slot filled once, an
  expression's per row) in place of two sources and a lambda; `Int128Text`
  deleted for `std::to_string(uint64)`; the powers of ten a table, not a
  loop per node per row; `CompiledAssignment::expr` a `unique_ptr`.
- **Cells added**: the NOT NULL byte; `x * 2`, `x + x`, `-x - x` over the widest
  `decimal(38,0)` and `u * u` over the widest `uint64` (the inputs that reach
  the 128-bit overflow flag); a foreign-key constant whose fold fails
  (matching and non-matching WHERE) and a `NULL + 1` fold; the two-core
  counter now asserts the cores overlapped (the first to finish found the
  other had applied some); an autocommit failure in a later row undoes the
  earlier rows, and a failure inside `BEGIN` poisons the transaction.
- **Confirmed by reading and by a scratch probe**: the evaluator's arithmetic
  and value forms, the evaluation point, no other reader of `Assignment::val`,
  and BJ-R9 - BA-S14's in-engine re-run fires only when the trail has not
  grown, and an UPDATE's `NoteOverwrite` grows it before any write, so
  `SET v = v + 1` is never applied twice.

**Filed, not fixed (outside the stage):** `docs/inflight/bugs/a-negative-foreign-key-value-skips-the-parent-check.md` - a
negative integer in a foreign-key column skips the parent check on `INSERT`
and `UPDATE`, an older quiet wrong answer that a folded constant now reaches.

**Rejected:** returning `StatusOr<vector<CompiledAssignment>>` instead of the
out-parameter and storing the foreign key on the entry (a larger signature
change for the one nullptr caller, a unit test; the stage's refusals are not
waiting on it); a `constexpr` table for `kTable` (the first call builds it
once); BJ-S3's `d = 2 * 3` against `d = 2 + 3` (the product of integers is an
integer, the sum of integer literals takes its context, by design).

### BJ-S5 — families 2 and 3, and the row-value form (built 2026-10-10)

On `worktree-bj-expression-update`, after `1a251cea`. **Not measured;
measured at the milestone's close** (§5).

- **Grammar** (`src/parser/parser.cpp`): PostgreSQL's precedence, loosest
  first - `OR`, `AND`, `NOT`, `IS [NOT] NULL`, the six comparisons (which do
  not chain), `||`, `+ -`, `* / %`, a unary sign, `::`. `CASE` (a simple one
  is desugared to the searched form at parse), `COALESCE`, `NULLIF`,
  `GREATEST`, `LEAST` are one n-ary node kind (`Expr::Kind::kCall`); `CAST(x AS
  T)` and `x::T` are a unary node carrying a `TypeSpec`, whose parse is
  shared with `CREATE TABLE`'s column type. `SET (a, b) = (x, y)` (BJ-Q7) is
  `a = x, b = y`. **A new token `::`** (`kDoubleColon`, `ShapeTag` 20) lexed as
  `kError` before, so no stored hash moved: the golden corpus changed three
  verdicts (`NotImplemented` to `ok`) and gained nineteen lines.
- **Typing**: a comparison, `AND`/`OR`/`NOT`, `IS [NOT] NULL` yield `bool`; the
  operands of one comparison, one `CASE`'s results, `COALESCE`, `GREATEST` and
  `LEAST` are one exact type (the text family counts as one) and untyped
  literals take it - two untyped literals take int64 or varchar, a bare
  number with a point has no default and is refused; a string literal is also
  accepted where the encode accepts one (a date, a timestamp, a decimal). A
  `CAST` pair outside the table in `types.md` §3.2b, `float`, and `varchar(N)`
  are refused at compile; the CAST target's type names are listed in the
  typer (it takes no catalog).
- **Evaluation**: `AND`/`OR` left to right, the right operand skipped once the
  left decides, so a guarded division is safe; `CASE` evaluates only the
  chosen branch; `GREATEST`/`LEAST` skip NULLs; `CAST` rounds half away from
  zero when it narrows a scale (BJ-Q3 (a)) and refuses a result its target
  cannot hold (`OutOfRange`), a string that is not a number
  (`InvalidArgument`) and a `char(N)` too long.
- **Not built, by BJ-R1's own list**: `IN`, `BETWEEN`, `LIKE`, `EXISTS`, `IS
  TRUE`, `IS DISTINCT FROM`, a boolean literal (`TRUE`/`FALSE`) and `<>` (the
  engine spells inequality `!=`); each is refused at its byte. Decimal `/`
  moves to BJ-S6 with `ROUND`.
- **Cells**: `tests/set_expr_conditional_test.cpp` (15), nine new parser cells,
  the corpus lines. At this stage's tree the full suite had one failure,
  `IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence`, under
  `ctest -j8`; it passed 5 of 5 repeated alone (the load flake the notes
  name). The suite is re-run whole after the review, below.

### BJ-S5 — the `critics-developer` review of `ab37991c`, applied

On `worktree-bj-expression-update`, after `ab37991c`, the review found, and
the tree now fixes:
- **A wrong answer (high, BJ-Q3 (a)).** `Rescale`'s rounding test `2|r| >= divisor`
  overflowed 128 bits when 38 places are dropped, so `CAST(0.9 AS int64)` over a
  `decimal(38,38)` gave 0, not 1. It reads `|r| >= divisor - |r|` now; the cell
  covers +-0.9, 0.4999..., 0.5 and the text form.
- **A denial of service (high).** A simple `CASE` reuses its operand in every
  `WHEN`, so one nested in another's operand made typing and every row's
  evaluation exponential while the height and depth bounds saw a short tree:
  a 700-byte statement took 35 s to compile. `Expr::size` counts a shared
  child once per reference and the parser refuses past 4096 nodes
  (`Unsupported`, at the node's byte). The redesign the review also offers -
  the operand held once on the `CASE` node and evaluated once per row, which
  departs from the order's "desugared at parse" and would matter once BJ-S6
  adds non-trivial functions - is **left open**, not chosen.
- **Valid input refused**: `m -1::int64` (the fused-minus branch skipped the
  `::` level; `ParseCastSuffix` now serves both), `CAST(case AS int64)` (`AS`
  ends a value), and `NULL = 'a'` (a comparison of two untyped literals now
  settles both to the default of their merged kind).
- **Cuts**: the hand-written number parser inside `CAST` (it disagreed with the
  engine's: a leading space) is the engine's `ParseDecimalLiteralWide` now;
  `Unify`'s unused context, `ParseTypeSpec`'s bare-decimal switch (the CAST
  typer refuses it, with its byte), the `SameType` re-check in
  `BooleanBinary`; one `ComparisonOf`, one `NaryOf`, one `DepthScope`, one
  `MakeCast`.
- **Cells**: the +-0.9 narrowing, the nested-CASE bound, `m -1::int64` (parse and
  eval), a column named `case` before `AS`, `NULL = 'a'`, a bare `decimal` in a
  CAST, an equal char pair; two vacuous lines removed (`CASE END` now asserts
  the "at least one WHEN" refusal).

**Rejected or left:** `CAST('1.5' AS int64)` is 2 where PostgreSQL errors -
`types.md` §3.2b states the rounding as the engine's rule, so it stays;
`CAST(18446744073709551615 AS uint64)` refused because a bare integer literal
resolves to int64 first (the bare `SET u = 18446744073709551615` works) and
`-128::int8` accepted through the fused minus - both recorded for
`known-gaps.md` at BJ-S8; `DecimalTypeOfLiteral` counting leading zeros, an
unbounded `char(N)` in a CAST, and the byte named by `CASE END` / `<>` errors
(cosmetic, no wrong answer); `ColumnDef` holding a `TypeSpec` (touches every
CREATE TABLE reader for no behaviour).

### BJ-S6 — scalar functions, date arithmetic and decimal division (built 2026-10-10)

On `worktree-bj-expression-update`, after `022e5e46`. **Not measured;
measured at the milestone's close** (§5). **Suite 3472/3472** at this stage's
tree, `HeapSuspensionIsLifted` disabled as before.

- **Functions** (BJ-Q5, as adopted): `ABS SIGN CEIL FLOOR ROUND TRUNC MOD`, `UPPER
  LOWER LENGTH SUBSTRING TRIM LTRIM RTRIM POSITION(a IN b) REPLACE LPAD RPAD
  CONCAT`, and `NOW()`, `CURRENT_DATE`, `DATE(ts)` (BJ-Q4); anything else is
  refused `unknown function` at its byte, as AP-S4 refuses one. The parser takes
  any `name(...)` (`Expr::kFunction`; `MOD(a, b)` becomes `a % b`, `POSITION`
  its `IN` form); the typer owns the table (`Typer::Function`), the evaluator
  the semantics. Strings are bytes (`LENGTH` counts bytes, `UPPER` touches
  ASCII), because that is how `char(N)` and a `varchar`'s width are measured.
- **Date arithmetic** (BJ-Q4 (a)): `date + n`, `n + date`, `date - n` (a date,
  refused outside the supported range) and `date - date` (an `int32` of days);
  every other operator over a date is refused, and timestamp arithmetic stays
  `Unsupported`.
- **Decimal `/`** (BJ-Q3 (a)): `decimal(P, S)` with `S = min(s + 16, 38)` and the
  digits a quotient can need, exact then rounded half away from zero, never
  narrowed implicitly.
- **A scaled decimal** (`SetType::Kind::kScaled`): `ROUND/TRUNC/CEIL/FLOOR` over a
  decimal yield a fixed scale and take the precision of what they are assigned to
  or combined with, so `SET price = ROUND(price / 3, 2)` types for a
  `decimal(10, 2)` column. **This is a departure from BJ-Q2 (b)'s exact type,
  recorded as a CLA proposal under the standing go-ahead**: without it the
  explicit narrowing BJ-Q3 (a) names ("`ROUND(x, s)` or `CAST` is how to narrow
  it") could not produce the column's type for any precision but the argument's.
  `CAST` still names the whole type.
- **Purity** is not declared in `functions.hpp`. The order's §3 asked for it
  there; that catalog's entries are what a WHERE conjunct compiles, with a
  single argument type, and its own rule is that a field nothing reads is a
  promise nothing checks - an UPDATE has no determinism class until AQ. `NOW()`
  and `CURRENT_DATE` are the stable two (`StatementContext`, carried across a
  park); the rest are pure; `IsConstantSetExpression` treats the first two as
  not constant, so BJ-Q10 never folds them.
- **Cells**: `tests/set_expr_functions_test.cpp` (11), eight parser cells' worth of
  corpus lines (the golden corpus: one verdict moved, no stored hash moved), the
  typing and function-conjunct pins moved to what is now true.

### BJ-S6 — the `critics-developer` review of `1a04b918`, applied

On `worktree-bj-expression-update`, after `1a04b918`, the review found, and
the tree now fixes:
- **A quiet wrong answer (high): a quotient was rounded twice.** The division
  rounded to its own scale and `ROUND`/`TRUNC`/`CAST` rounded that again, so a
  value just under a tie was pushed over it (`ROUND(d / 200000000000000001, 4)`
  gave 0.0001 for 0.0000499...). A decimal `/` that is the argument of a
  rounding function, or of a `CAST` to a decimal or an integer, is divided
  **once** to the target's places with that function's rounding mode
  (`DivideAt`); `CEIL`/`FLOOR` over one keep the inexact remainder. Whether
  PostgreSQL's numeric division rounds twice was not checked; `types.md` states
  this engine's rule.
- **Decimals of scale 22 or more could not be divided (high).** `S = min(s+16, 38)`
  left no integer digits. **The quotient's type is now `S = max(s, min(s+16,
  38-I))`, `P = min(I+S, 38)` with `I = p-s+s2`: as many extra places as leave
  room.** That changes the formula `types.md` §3.2b carried; it is CLA's proposal,
  adopted on the operator's standing go-ahead of 2026-10-10
  (go-ahead-achieving-milestone), not the operator's mark. `decimal(10,2) / 2` is
  still `decimal(26,18)`; `decimal(38,2) / decimal(38,2)` has no room and stays at
  scale 2.
- **Scaled decimals (medium)**: `SIGN` of a decimal whose precision is its scale
  could never fit; two scaled decimals took the last one's precision, so
  `ROUND(w,2) = ROUND(x,2)` depended on the order; `ROUND(d52, 2)`,
  `ABS(ROUND(w, 2))` and a nested `ROUND` were refused where `ROUND(d53, 2)` was
  admitted. Every rounding function over a decimal is now scaled, `ABS` and the
  rest pass a scaled argument on, two of one scale keep the wider precision, and
  **in a comparison a scaled decimal takes the wider precision of the two sides**
  (review item B5: a data-dependent row failure for a comparison is worse than
  a compile refusal, and the result is a bool either way).
- **Smaller**: `ROUND(NULL, 2)` and `ABS(NULL)` are NULL of the context's type;
  `MOD(a)` and `MOD(a, 2, 3)` are refused as `MOD`'s arity, not as an unknown
  function; places over 38 are `InvalidArgument`, a negative count
  `NotImplemented`; a row error names the function (`the result of function abs
  ...`); the foreign-key refusal says "or the clock".
- **Cuts**: one `FloorEpochDay` (type_literals.hpp) for the three day floors
  (`DATE()` in the WHERE catalog included); `Rescale`'s unreachable branch; one
  `IsDateType`; ROUND/TRUNC's places argument is dropped from the typed tree once
  read; a function-name table the row errors share.
- **Cells**: the five brittle ones tightened (a tie that is a tie, TRUNC against
  ROUND, a negative tie, FLOOR against TRUNC), and new ones for the double
  rounding, the room for integer digits, SIGN over `decimal(2,2)`, the order of
  two scaled operands, a scaled decimal in a comparison, a NULL argument,
  `LENGTH('é') = 2`, a date below 1900-01-01 and the arity messages.

**Rejected or left:** unifying `ParseExprFunction` and `ParseExprCall`'s
argument loops (POSITION's `IN` and CAST's `AS` make them differ in the places
that matter); the review's L5, a column named `Current_Date` still reaching the
function through `CURRENT_DATE` (identifiers are folded, so they are one name);
a cell for `NOW()` carried across a parked `SET` (the WHERE one exists in
`function_conjunct_test.cpp`; the carried context is the same
`ParkedWrite::context`, read, not run) - listed for `known-gaps.md` at BJ-S8.

### BJ-S7 — the scalar subquery (built 2026-10-10)

On `worktree-bj-expression-update`, after `3292b20d`. **Not measured;
measured at the milestone's close** (§5).

- **`(SELECT ...)` in a SET value** (BJ-Q6, as revised): `Expr::Kind::kSubquery`
  carries the parsed statement and its text. The parser takes it as an
  outermost SELECT (an aggregate is allowed, which a nested one is not),
  so `SET v = v + (SELECT SUM(v) FROM t)` parses.
- **Run once, before the walk.** `TypeSetExpression` asks a
  `SubqueryResolver` (the dispatcher's `RunScalarSubquery`), which runs the text
  through `HandleSelect` with a one-column collecting `ResultSink` in the
  session's view (inside `BEGIN`, the transaction's own writes included) and
  returns the value and its column's type. The node becomes a **typed
  constant** of the tree - a literal of that type - so every later stage (the
  evaluator, the foreign-key fold) needs nothing new. No row is NULL; more than
  one row or column is `InvalidArgument`, **even when the WHERE matches
  nothing**, the stated difference from a per-row evaluation.
- **Correlation is the SELECT compiler's own classification**: the subquery is
  compiled as an `EXISTS` over the target relation, and a sub-chain it marks
  correlated is refused `NotImplemented` at the subquery's byte. `SELECT SUM(v)
  FROM t` inside `UPDATE t` is not correlated - its `v` is its own.
- **A park keeps its answer.** The resume compiles the statement again, which
  would run the subquery again and set the rows after the park from another state
  of the database than the rows before it. `Session::ParkedWrite` carries the
  answers; a mutant that ignores them is killed by `bj_single_write_rig_test.cpp`
  (rows 3..5 became 777).
- **Not built**: a subquery over a catalog view, the row form `SET (a, b) =
  (SELECT ...)`, and a correlated one - each refused by name.
- **Cells**: `tests/set_expr_subquery_test.cpp` (9: the pinned BJ-Q6 cell, another
  relation, aggregates, no row, more than one row, correlated by qualifier and
  by an outward name, types, the subquery's own errors, a foreign-key column, a
  transaction's own writes), a parser cell, the parked rig cell, and the corpus
  (two verdicts and three lines).

### BJ-S7 — the `critics-developer` review of `7c8b2cef`, applied

On `worktree-bj-expression-update`, after `7c8b2cef`, the review found, and
the tree now fixes:
- **A silent wrong value (high).** The collecting sink saved "the last row
  rendered", but a sorted statement renders rows as the top-N heap admits them
  and emits the survivors afterwards, so `(SELECT x FROM u ORDER BY x LIMIT 1
  OFFSET 1)` stored the third value, not the second. The row text is now the
  index of its value, so `Emit` takes exactly the one that was encoded for it.
- **Two reads of two states (medium).** Outside `BEGIN` the SELECT minted its own
  snapshot, later than the UPDATE's, so `SET a = (SELECT ...), b = (SELECT ...)`
  could read two versions of a row. `HandleSelect` takes an optional pinned
  snapshot, and the subquery reads at the UPDATE's own. Inside `BEGIN` it
  already did; the repeatable-read cell shows it (the subquery read the
  transaction's view, not another session's later commit). **The autocommit race
  itself has no cell** - it needs another core to commit between two subqueries of
  one statement - and is covered by reading.
- **Correlation (medium).** The text probe refused valid nested subqueries
  (it counted a grandchild's reference to the subquery's own relation as
  reaching the target) and skipped every aggregated or paginated correlated one
  (the probe did not parse), answering `InvalidArgument` where the order says
  `NotImplemented`. A subquery that **compiles on its own** names nothing outside
  it; one that does not is compiled as an `EXISTS` over the target, from the
  AST, and refused if that makes it correlated. Both are compiles; neither runs.
- **Error bytes (low)**: the SELECT's own offsets were relative to the
  subquery's text; the text is now padded to where it stood, so every byte is a
  byte of the UPDATE.
- **`;` inside the parentheses (low)** is refused: it ended the subquery early
  and the UPDATE went on.
- **Pinned in a cell, not changed (low)**: a subquery in a branch never taken
  still runs and its error fails the statement - the answer is taken before the
  walk whatever a row would do; `types.md` says so.
- **Cuts**: `Session local`, the `last_subquery_close_` side channel (the close
  byte is returned), the text probe.

**Rejected:** moving `SubqueryResult` to a header of its own so `session.hpp`
need not include `set_expr.hpp` (a header split for one struct); a
`const SubqueryResolver&` in place of the pointer (a signature churn through
`CompileAssignments`'s callers for no behaviour); `std::move` of the parked
answers (the parked write is read through a const reference and cleared two
lines later; the vector is a handful of values); the sink keeping only the
type oid and modifier (it keeps the field it was given).
