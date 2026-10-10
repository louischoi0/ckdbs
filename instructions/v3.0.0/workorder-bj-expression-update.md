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
  entry deleted with the fix, as `docs/inflight/bugs/README.md` asks;
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
  by zero is refused; `||` yields a string whose length must fit the
  target's `N`, or the row is refused - nothing is truncated.
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
- Every caller of `CompileAssignments`: one, `UpdateInner`; a parked
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
  the same with a `COMMIT` (refused or not, no row twice), and a statement
  that wrote nothing before its park and then ran whole (each row once).
  **They are green, not red**: the guards hold on `main`, which §1.5 said
  they would. That they can fail was checked by mutation - calling the hook
  twice per row fails all four. BJ-S4 makes them load-bearing; BA-S14's
  in-engine re-run is the path they do not cover until BA is on `main`.
