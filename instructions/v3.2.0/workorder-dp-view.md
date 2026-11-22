# Work order DP - VIEW: a named, stored SELECT that a statement's block absorbs by text before any plan is chosen, so a statement through a view runs the chain, the plan sources and the access ladder its expansion would run

Status: **OPENED 2026-11-22 on the operator's standing go-ahead (go-ahead-achieving-milestone; `raft-marks-2026-11-22.md` §5); DP-S0 done at `30c47abf`, DP-Q14..Q22 adopted (a) under that word.** Before: **WRITTEN 2026-11-22.** Written in the main checkout on
`main` at `71013396` (`v3.4.17-94-g71013396f`) on the operator's words of
2026-11-21 and 2026-11-22 (§0; `raft-marks-2026-11-22.md` §3). The letter
DP, the shape of §2, the rules, the items and the stages are CLA's
proposals. **DP-Q0..Q13 marked (a) by the operator on 2026-11-22**
(*"mark them as proposed"*, `raft-marks-2026-11-22.md` §4), after CLA's
account of each item's proposal and reason - **the operator's own mark,
not the standing go-ahead**, which is what DP-Q1 `[quiet-wrong]` and DP-Q6
`[plan-identity]` required. No stage is started; DP is not opened. Every
`[source-read]` claim below was read at `71013396`. No suite was run: the
order changes no build input.

**DP supersedes the conversation draft of 2026-11-21** (VW1-VW19, never
committed). Three of that draft's premises are false against the tree and
are corrected here, not carried: (1) "the join order is the view's written
order" - the managed-plan contract (CX) chooses a block's order, and a
view's text is only the written order's input (§1.4); (2) "the fingerprint
is taken over the expanded text" - the fingerprint is accumulated by the
parse of the statement as written (§1.3), so a view's name is a token of
its shape; (3) "the plan key carries the base relations only" - the key's
oid list is the block's relations, and a view that changes body under one
oid would let a captured order or an administrator's plan outlive the
shape it was made for (DP-R3, DP-Q6).

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct answer into a wrong one with no error; `[plan-identity]` where a
wrong choice lets a managed plan apply to a shape it was not made for -
no wrong row, but the engine changing a plan without the operator, which
CX-R1 forbids.

Related: `docs/spec/parser-v2.md` §2 (J2: table-position nesting is not a
step), I5 (resolution once, before execution), I9, I11a (`FOR UPDATE`),
I12 (the plan sources, CX-S2's rewrite inventory and class (b)), I19 (the
query layer), item 11 of §9 (the fingerprint during the parse);
`docs/spec/namespace.md` NS5, NS6; `docs/spec/ddl-transactional.md`;
`docs/spec/drop-table.md` DT2, DT3; `docs/spec/alter.md`;
`docs/spec/sys-views.md`; `docs/spec/waystone-concpets.md` §3, §4d;
`instructions/v3.2.0/workorder-cx-managed-plans.md` (CX-R1, CX-R2, CX-Q4,
CX-Q9, CX-Q21); `workorder-di-shape-identity.md` (DI-R2, the binding);
`workorder-dn-heap-rowid.md` (DN-R5: the only word reserved by the
operator's word).

## 0. What DP is

`[operator]` 2026-11-21: *"I want to apply the view feature to ckdbs in
the way that is optimized for it."*

`[operator]` 2026-11-21, correcting CLA's first draft: *"The contract that
the query is the plan has been retired. In its place, a little relaxed:
the database cannot change a plan freely, and the plan itself is a managed
object."*

`[operator]` 2026-11-22: *"Bring the kdbs repo up to date and write the
view milestone work order again to fit the recent state of the work."*

DP builds `CREATE VIEW`, `DROP VIEW` and reading a view in a `SELECT`'s
`FROM`. Its one idea, CLA's: **a view is text the block absorbs before
any plan source is asked** - its relations, its conjuncts and its
projection spliced into the referencing block by the catalog's DDL state
alone, so a statement through a view compiles, is planned, is captured,
takes an administrator's plan, is advised, replays a trail and is
`ANALYZE`d exactly as its expansion written out by hand would be, under
its own shape identity. A view whose body cannot be absorbed into one
block without changing an answer is answered by the query layer as a
derived table (I19) - or refused (DP-Q2) - and is never planned.

What DP is not: a materialized view (§6), an updatable view (§6, DP-Q9),
a security object (§6).

## 1. The tree as read `[source-read]`

### 1.1 There is no user view

- No `VIEW` keyword: the lexer's keywords are `JOIN`, `ON`, `AS`, `IN`,
  `EXISTS`, `NOT`, `BETWEEN` and the refused outer-join words
  (`include/kds/parser/token.hpp:37-54`), and a keyword hashes as an
  identifier does (`token.hpp:48-49`).
- "View" in the tree means two other things: a transaction's read view
  (`txn::ReadView`, `include/kds/txn/visibility.hpp:190`) and a `sys`
  catalog view (`include/kds/exec/catalog_view.hpp:52`,
  `docs/spec/sys-views.md`). DP's object is neither, and every new
  identifier is named so (DP-R9).
- `sys.objects` types an object by a well-known type oid; the last two are
  `kTypeDroppedTable = 34` and `kTypeDroppedNamespace = 35`
  (`include/kds/catalog/well_known.hpp:112`, `:125`).
- The superblock is 39 (`include/kds/server/superblock.hpp:274`, DN).

### 1.2 How a name becomes a relation

- `Catalog::ResolveRelation` resolves `qualifier.name` or `name` in the
  session's namespace, one namespace asked, `sys` refused because its
  relations are catalog views that never come here
  (`include/kds/catalog/catalog.hpp:510-533`). Under it,
  `FindTableOidByName` scans `sys.objects` for `(namespace, name)` and is
  "the gate": a name that does not resolve is a relation that cannot be
  touched, and DT3's filter by the reader's view is applied there
  (`catalog.hpp:555-574`).
- Resolution happens once, in the step compiler, before execution; a
  compiled chain holds no identifier (I5, `docs/spec/parser-v2.md` §3).
- A table-position subquery is not a step: a derived table's result would
  have to become a relation with a schema, probed by something other than
  a tuple key, "which is the entire shape of the execution model"
  (`parser-v2.md:82-84`, J2). The query layer answers derived tables over
  rows it holds, never correlated, never carrying a `WHERE` term into
  itself (`parser-v2.md:242`; `src/exec/query_layer.cpp:332`, `:412-413`).

### 1.3 Shape identity

- The fingerprint is accumulated during the parse, not by a second lex
  (`parser-v2.md:278`, item 11). So a statement's `pattern_id` and
  `fetch_id` are functions of the text **as written**; a view's name is a
  token of the shape and its body is not.
- `kFingerprintVersion` moves only when an already-stored key would
  change meaning; an additive shape that changes no existing hash does
  not move it (`include/kds/parser/fingerprint.hpp:162-174`).
- A statement's identity beside its `fetch_id` is its **binding**
  (`include/kds/stats/statement_binding.hpp:69-95`): the namespace its
  unqualified names meant and the distinct relations it declared, in first
  declaration order, which must be "a function of the text"
  (`statement_binding.hpp:40-55`); at most `kMaxRelations` = 16, past which
  `over` is set and the binding is not known (DI-R2).
- Trails are keyed by `fetch_id` under the binding (DI-S3;
  `waystone-concpets.md` §3, §4d).

### 1.4 The plan contract DP must not bend

- A block's order comes from, first to last: an enabled administrator's
  plan (`CREATE PLAN`, CX-S1b), the shape's captured order under
  `join_order = fixed`, the search, the written order (`parser-v2.md:141-149`).
- The captured and administrator's plans share one key: `fetch_id`, the
  block's relation count and every relation's oid in written order
  (`include/kds/exec/captured_plans.hpp:70-83`), at most
  `kMaxPlannedRelations` = 16 (`include/kds/exec/step_chain.hpp:993`).
  The captured table is bounded and never evicted: "evicting one and
  capturing it again would be the engine changing a plan on its own"
  (`captured_plans.hpp:58-60`, CX-Q21 (a)).
- CX-S2's rewrite inventory: every compile-time item is decided by the
  statement's text, the catalog's DDL state and the order, and none reads
  a statistic, the memo, a plan or `join_order`; structurally, the
  compile-time items are in `CompileBound`, which is handed no
  `PlanOptions` (`parser-v2.md:167`). **Class (b) - a rewrite that
  reschedules reads by the engine's choice - has no member and is never
  automatic**: no subquery unnested, no inner side hoisted, no conjunct
  dropped, no `DISTINCT` or `LIMIT` pushed below a join (`parser-v2.md:183`).
- A block with a subquery predicate is never reordered and takes no
  `CREATE PLAN` (`parser-v2.md:183`, CX-S7); a joined `SELECT *` is
  refused by the parser (`parser-v2.md:148`).
- `FOR UPDATE` is one relation's, no join, no derived table
  (`parser-v2.md:135`, I11a).

### 1.5 Where a declaration's text lives

- Four **declaration relations** - `sys.assertions`, `sys.transitions`,
  `sys.cabin_exprs`, `sys.plans` - share one row shape: the declaration's
  text plus four fixed fields, the parsed form recovered by re-parsing
  `source_text`; deletion is physical; **a declaration commits at its
  statement, never with a session's transaction**
  (`include/kds/exec/declaration_relation.hpp:18-50`).
- A relation's DDL is transactional and logged, rolled back at mount when
  its transaction lost (`docs/spec/ddl-transactional.md`); a drop is
  RESTRICT against its dependents (`docs/spec/drop-table.md` DT3), and a
  dropped oid is never reissued (DT2).

## 2. The design `[design]`

None is built or measured.

**The object.** A view is a `sys.objects` row of a new type
`kTypeView` (36) in a namespace, so its name is unique against tables
under NS6 and is reached only through `ResolveRelation`'s gate, plus a
row in a new `sys.views` holding its text, its column names and its
dependency list (DP-Q3, DP-Q4). `ResolveRelation` answers the oid;
the compiler asks the oid's type, as it asks a relation's storage form
today.

**Absorbable form** (DP-Q1). A body is absorbable when it is one block
the native grammar reads: `FROM` one relation or an inner equi-join chain
(views among them), a `WHERE` of conjuncts and boolean trees the native
grammar reads, a select list of plain columns with optional aliases, no
`*` over a join. Anything else - an aggregate or `GROUP BY`, `DISTINCT`,
`ORDER BY`, `LIMIT`/`OFFSET`, a set operation, a derived table, a
function in the select list - is **not absorbable**: moving the outer
block's conjuncts across it would change an answer (§2's table, below),
and doing it anyway is class (b).

**Absorption** (one compile-time item, class (a)). Inside
`CompileBound`, before equality propagation and before any plan source:

1. the view's binding in `FROM` is replaced by its body's relations, in
   the body's written order, at the view's position; each takes the
   binding name `<view binding>.<inner binding>` (DP-Q7);
2. each reference to a view column is rewritten to the body column it
   names;
3. the body's `WHERE` and `ON` conjuncts are appended to the block's
   conjunct list after the outer block's, marked `from_view` for
   `ANALYZE`;
4. the outer select list is read through the column map.

Nested views absorb inside out, to a depth bound (DP-Q8). The result is an
ordinary block: equality propagation, the access ladder, the inner build,
key sets, Waystone, Cabin and every plan source apply as written, with no
code of DP's after step 4.

**Why each non-absorbable form is not absorbed:**

| body holds | an outer `WHERE x = 1` moved inside would | so |
|---|---|---|
| `GROUP BY`, an aggregate | filter rows before the fold, not groups after it | not absorbable |
| `DISTINCT` | change which duplicates exist | not absorbable |
| `ORDER BY` + `LIMIT` | take the first n of the filtered rows, not filter the first n | not absorbable |
| a set operation | need distribution over each arm - a rewrite of its own | not absorbable |

**Shape identity** (DP-Q5, DP-Q6). The `fetch_id` is the written text's,
so `SELECT * FROM v WHERE id = ?` and its expansion written by hand are
two shapes with two trails and two plans - which is what the operator
manages: the statement the application sends. The binding declares, in a
fixed order that is a function of the text, the view's oid and then its
body's relations (`statement_binding.hpp:40-55`'s rule kept). The plan
key carries the block's relations - the base relations after absorption -
and **the view's oid as well**, so a captured order or an administrator's
plan names the view it was made through.

**A view's body never changes under its oid** (DP-R3). `CREATE OR
REPLACE VIEW` is not built (DP-Q6); a new body is `DROP VIEW` and
`CREATE VIEW`, which issues a new oid (DT2), so every key, trail and
statistics row made through the old body names an oid that no longer
resolves and is retired by the rules that already retire a dropped
relation's (`captured_plans.hpp`'s mount, DI's binding).

**Dependencies.** At `CREATE VIEW` the body is resolved once; the oids
and column ids it read are its dependency list. A `DROP TABLE`, `DROP
COLUMN`, `RENAME COLUMN`, `RENAME TABLE` or `DROP VIEW` of a dependency is
refused RESTRICT, naming the view (DT3's rule extended; DP-Q4). An
administrator's plan made through a view RESTRICTs its `DROP VIEW`
(CX-Q4 (a)'s rule for a pinned index, extended; DP-Q6).

**What a statement through a non-absorbable view gets** (DP-Q2 (a)): the
query layer, the view as a derived table, nothing pushed into it -
CX-S2's class (b) stands. Its whole body is folded whatever the outer
filter is; that cost is a documented product limit, said by `CREATE VIEW`
as a warning and by `sys.views.absorbable = false`.

## 3. Rules

Binding once DP opens.

- **DP-R1 - one answer** `[quiet-wrong]`. A statement through a view
  returns what the same statement with the view's body written as a
  derived table returns under SQL's semantics, but for emission order
  without `ORDER BY`, which is no contract. Absorption is checked against
  that answer by a differential cell in the sim (DP-S3).
- **DP-R2 - absorption is class (a)**. It reads the statement's text and
  the catalog's DDL state and nothing else: no statistic, no plan, no
  `join_order`, no load. It runs in `CompileBound`, which has no
  `PlanOptions` (`parser-v2.md:167`), and is added to CX-S2's inventory
  table as its own row. A non-absorbable body is never partly absorbed.
- **DP-R3 - a view's body is fixed for its oid's life** `[plan-identity]`.
  No statement changes the text, the column map or the dependency list
  behind a view oid; a new body is a new oid (DT2).
- **DP-R4 - a plan names the view it was made through**
  `[plan-identity]`. A captured order and an administrator's plan whose
  statement read a view carry the view's oid in their key, so neither
  applies to the hand-written expansion, nor to another view with the
  same body, nor survives the view's drop.
- **DP-R5 - the binding is a function of the text** (DI-R2 kept). A view
  declares its oid, then its body's relations in written order, depth
  first; a statement that crosses 16 relations so is `over`, as any
  other.
- **DP-R6 - nothing is pushed into a non-absorbable view**. CX-S2's class
  (b) gains no member.
- **DP-R7 - no new reserved word**. `VIEW` and `MATERIALIZED` are matched
  by text at statement position, as `FOR`/`UPDATE` are (I11a); `view`
  stays a column name; `kFingerprintVersion` does not move, the golden
  corpus the witness (`fingerprint.hpp:162-174`).
- **DP-R8 - a format event is said**: `kTypeView` and `sys.views` are a
  superblock bump to 40, and the mount refuses an older volume by name as
  every earlier format change did.
- **DP-R9 - one quantity, one name**: the object is a *view* in SQL and
  in `sys.views`; in code it is `UserView`/`view_def`, never bare
  `View` or `view`, which already name a `txn::ReadView` and a
  `CatalogView`.
- **DP-R10 - a statement over no view pays nothing**: no standard cell
  moves outside its floor; the absorption step is one type check on each
  resolved `FROM` oid.
- **DP-R11 - no constant without a measurement**: the depth bound (DP-Q8)
  is `[provisional]` until DP-S4 measures a compile at that depth.

## 4. Items for the operator (DP-Q)

(a) is CLA's proposal in each. **DP-Q0..Q13 are marked (a) by the
operator, 2026-11-22** (`raft-marks-2026-11-22.md` §4). **DP-Q14..Q22 were
raised at DP-S0 and are adopted (a) on the operator's standing go-ahead of
2026-11-22 (go-ahead-achieving-milestone)** - CLA's proposals, not the
operator's per-item mark (`raft-marks-2026-11-22.md` §5); DP-Q16
`[quiet-wrong]` and DP-Q17 `[plan-identity]` among them, each premise
re-read against the tree at `30c47abf` before its adoption (§7, DP-S0).

| | item | (a) | other |
|---|---|---|---|
| DP-Q0 | The order's standing. | **a build order**: the premise (absorption costs nothing outside a view, §2) is checked at DP-S0 by census, not by a spike | (b) exploratory, DP-S0 measuring an absorbed statement against its hand-written expansion before anything is built |
| DP-Q1 `[quiet-wrong]` | What is absorbable. | **§2's absorbable form**: one native block, inner equi-joins, native conjuncts and boolean trees, plain columns with aliases; a subquery predicate in the body admitted (the block is then never reordered, `parser-v2.md:183`) | (b) narrower: no subquery predicate, no boolean tree; (c) wider: a body the native grammar reads with a function in its select list, the function evaluated at the outer projection |
| DP-Q2 | A non-absorbable body. | **the query layer, the view as a derived table, nothing pushed in** (DP-R6); `CREATE VIEW` warns and `sys.views.absorbable` says so | (b) refused at `CREATE VIEW` `NotImplemented`, every view absorbable by construction - one performance character for every view |
| DP-Q3 | Where a view is stored. | **a `sys.objects` row of type `kTypeView` (36)** for the name, so NS6's uniqueness and DT3's visibility filter hold unchanged, **plus `sys.views`**: `source_text`, the column names in order, `absorbable`, the dependency list (oid and column id pairs) | (b) `sys.views` alone, its own name table beside `sys.objects`: two name spaces a table and a view could collide across |
| DP-Q4 | A view's DDL and its transaction. | **transactional, as a relation's** (`ddl-transactional.md`): `CREATE VIEW` and `DROP VIEW` take the name under its root page (CT7), are logged, and roll back with their transaction; a dependency's drop or rename is RESTRICT, naming the view | (b) a declaration relation's rule, committed at its statement (`declaration_relation.hpp:18-50`): simpler, but a `CREATE VIEW` inside a transaction is then durable before its `COMMIT` |
| DP-Q5 | A view statement's `fetch_id`. | **the written text's** - the fingerprint as the parse accumulates it (`parser-v2.md:278`); a statement through a view and its hand-written expansion are two shapes | (b) the expansion's, by hashing the body's tokens in place of the view's name: one shape for both, at the cost of a second hash pass and a view name the statement statistics no longer show |
| DP-Q6 `[plan-identity]` | A new body for a view. | **`DROP VIEW` then `CREATE VIEW` only, a new oid** (DP-R3); `CREATE OR REPLACE VIEW` refused `NotImplemented`; **an enabled administrator's plan made through a view RESTRICTs its `DROP VIEW`**, naming the plan | (b) `CREATE OR REPLACE` keeping the oid, every captured order and administrator's plan that names it disabled and counted; (c) as (a), but the drop is allowed and the plan disabled and counted |
| DP-Q7 | How a plan names a view's inner relation. | **`<view binding>.<inner binding>`**, depth-first for a nested view (`v.w.o`), in `ORDER (...)`, `ACCESS`, `BUILD`, `sys.plans.variants` and `ANALYZE` alike | (b) the inner binding alone, refused where two views' bindings collide |
| DP-Q8 | Nesting. | **allowed to depth 8 `[provisional]`** (DP-R11); a cycle is impossible by construction, since a body resolves only views that exist and no body changes (DP-R3) | (b) no view over a view |
| DP-Q9 | Writes through a view. | **refused `NotImplemented`** - `INSERT`, `UPDATE`, `DELETE`, `PURGE` and `MERGE`-like forms naming a view; reserved for a later order (§6) | (b) an absorbable single-relation view whose projection holds the tuple key written through as its relation, `WITH CHECK OPTION` refused |
| DP-Q10 | `FOR UPDATE` through a view. | **admitted where the absorbed block is one relation** - the I11a check asked after absorption; refused at the byte of `FOR` otherwise | (b) refused for every view |
| DP-Q11 | `SELECT *` in a body. | **a body over one relation may write `*`; it is expanded at `CREATE VIEW` to the columns then live and stored so** - an `ADD COLUMN` later does not change the view, so its shape and its plan key do not move | (b) `*` refused in a body |
| DP-Q12 | `SELECT *` from a view over a join. | **admitted**: the view's columns are its own named list, so the statement reads no join's `*`; the native parser's refusal (`parser-v2.md:148`) is of a `*` written over a written join, which this is not | (b) refused, as a joined `*` is |
| DP-Q13 | Who may create a view. | **`admin`, as every DDL is** (`docs/spec/protocol.md:142`: ranks, not grant sets, no per-relation `GRANT`); a read through a view is a `SELECT`, `readonly`'s. With no per-relation grant there is nothing a view could hide, so DP gives it no access meaning | (b) `readwrite` may create a view: the first DDL below `admin`, a change to the rank table |
| DP-Q14 | How `sys.views` stores its rows (raised at DP-S0). | **fixed-width rows, one relation, written by the catalog's transactional typed-row writer** - a header row, the text in 192-byte chunks, one row a column name, one row a dependency - placed under the DDL's transaction (`InsertRow`, `catalog.cpp:475`), delete-marked by the drop as `DropTable`'s sweep marks a relation's rows (`catalog.cpp:2470-2520`), read through `ScanAll`'s view filter (DT3); a body of at most 16,384 bytes | (b) a transactional var-length writer for a declaration-shaped relation: none exists - `InsertDeclaration` writes at `kBootstrapXid` and `ScanDeclarations` filters by no view (`declaration_relation.cpp:82-239`) - so it is a writer and a reader to build for one relation |
| DP-Q15 | How a view's columns are named (raised at DP-S0). | **`CREATE VIEW v [(c1, ...)] AS <select>`**: the list names the columns; without it a column is named by its item - a column's name, or a query-layer item's alias; two columns of one name are refused `InvalidArgument`, naming the list as the remedy. **The native select list carries no alias** (`ColumnName`, `ast.hpp:147`), so a body that writes `AS` in its select list is not a native block and is non-absorbable: §2's "plain columns with optional aliases" is read as plain columns and the view's column list (a revision of DP-Q1's wording, below) | (b) the native grammar taught select-list aliases: a grammar change to every native SELECT, for a name a column list already gives |
| DP-Q16 `[quiet-wrong]` | What a view's column dependencies are (raised at DP-S0). | **a dependency on every column of a relation the body reads whose name the body's text names** (every column for a `*` body); `DROP COLUMN` or `RENAME COLUMN` of one is RESTRICT; **and `ADD COLUMN`, or `RENAME COLUMN` to a name the body's text names, on a relation the body reads, is RESTRICT too** - a new column of that name could re-point a bare name in the stored body (a correlated subquery's name resolved outward today would resolve to the new inner column, `step_compiler.cpp:651-662`), which changes a view's answer with no statement naming the change | (b) relation-level: every column change of a relation a view reads refused |
| DP-Q17 `[plan-identity]` | The plan key's shape with a view (raised at DP-S0). | **`n` and `oids[]` stay the block's base relations**, so every order and pin is measured by them as today (`captured_plans.hpp:70-93`, `admin_plans.cpp:106-120`), **and the key gains a view list**: every view oid the block absorbed, in declaration order, at most 16, compared and hashed with the rest; `sys.plans`' text carries it as `views=`, absent for none, so no stored row changes meaning; a block absorbing more views has no plan | (b) the view oids folded into the key's `fetch_id`: one field, but `PlanRowCanMatch`'s fetch-id comparison (`plan_catalog.cpp:582`) and every display of the key would read a number no parse produces |
| DP-Q18 | DP-R10's fast path (raised at DP-S0). | **a catalog word "a view exists or has existed since mount"**, set at mount from `sys.objects` and by every `CREATE VIEW`, never cleared: while it is clear a compile asks nothing; once set, each `FROM` name of every block is asked as a view before it binds, measured at DP-S4 | (b) always asked: one name lookup a relation for every statement on every instance |
| DP-Q19 | `ANALYZE`'s mark on a view's conjuncts (raised at DP-S0; §2's step 3). | **not built**: an absorbed relation's step is named `<table> AS <view>.<inner>` (DP-Q7), which says where it came from; the conjuncts carry no mark, since no reader consumes one and a new chain field would enter the configurations the contract suites compare | (b) a `from_view` flag on each lowered conjunct, printed by `ANALYZE` |
| DP-Q20 | A non-absorbable body at `CREATE VIEW` (raised at DP-S0). | **compiled by the query layer without running** - a describe entry beside `RunQuery` - so its relations resolve, its columns are named and typed, and a wrong body is refused at its `CREATE`, not at its first read | (b) parsed only, refused at its first read |
| DP-Q21 | Where a body's unqualified names resolve (raised at DP-S0). | **in the view's own namespace**, whatever the reading session's `USE` (DP-R3: the body is fixed for its oid's life) | (b) in the reader's namespace: one view text meaning other relations under another `USE` |
| DP-Q22 | `DROP VIEW` against a running reader (raised at DP-S0). | **no lock of its own**: a reader declares the view's oid (DP-R5) and holds `IS` on every base relation it absorbed, so a drop changes nothing a running statement reads; `DROP VIEW` waits for nothing | (b) `DROP VIEW` takes the view oid's `X` under the 1 s fault net, as `DROP TABLE` takes a relation's |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DP-S0 - census** | No engine code. **Read**: every site that takes a resolved `FROM` oid as a table (the compiler, the query layer's describe, the binding's `Declare`, `HandleSelect`'s catalog-view branch, `ANALYZE`, `WAYSTONE ON`, `CREATE PLAN`, `ADVISE PLAN`, `SHOW`/`DESCRIBE`); every DDL that must ask a view's dependency list (DROP, RENAME, DROP COLUMN, RENAME COLUMN, DROP NAMESPACE); where in `CompileBound` absorption stands relative to propagation and the boolean-tree flattening; the golden corpus's coverage of a `CREATE`/`DROP` word at statement position. | the census, DP-Q0..Q13 restated against it |
| **DP-S1 - the object** | `kTypeView`, `sys.views`, superblock 40 (DP-R8); `CREATE VIEW` and `DROP VIEW` under DP-Q3, Q4, Q11, Q13; the dependency RESTRICTs; `SHOW VIEWS`, `DESCRIBE` of a view, `sys.views`. A view is created and dropped; nothing reads one. | review, the full suite |
| **DP-S2 - absorption** | The compile-time item (§2, DP-R2) and its inventory row in `parser-v2.md` I12; the binding (DP-R5); the plan key with the view's oid (DP-R4) and DP-Q6's RESTRICT; DP-Q7's names; DP-Q10. A statement through an absorbable view runs natively. | review, the full suite |
| **DP-S3 - the rest** | DP-Q2 for a non-absorbable body; DP-Q8's nesting; DP-Q9's refusals; the sim's differential cell (DP-R1): random absorbable bodies, each statement answered through the view and as a derived table by the query layer, compared. | review, the full suite, `scripts/sim.sh` |
| **DP-S4 - the close** | `build-release`: the standard cells (DP-R10); a point read, a two-relation key join and a 1,000-row range, each through a view and written by hand - the compile and the run - and a compile at DP-Q8's depth (DP-R11). | closed |

## 6. What DP does not do

- **No materialized view.** A view whose rows are stored and maintained
  on write is its own order. CLA notes for it, `[design]`: a single-
  relation `COUNT`/`SUM ... GROUP BY` view is what a Bound Cabin's group
  header already maintains for an assertion, transactionally and with
  undo (`docs/spec/assertion.md`), so such an order would start from that
  structure, not from a refresh.
- **No write through a view** (DP-Q9 (a)).
- **No predicate pushed into a non-absorbable view** (DP-R6).
- **No `CREATE OR REPLACE VIEW`** (DP-Q6 (a)).
- **No security semantics**: no definer's rights, no security barrier.
- **No view-level plan**: a plan attaches to a statement's shape as today;
  "every statement through this view orders its inner relations so" would
  be a new key kind, with no consumer named.
- **No outer join**, in a body or around a view (I9).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-22

Written at `71013396` on the operator's words of §0. The 2026-11-21
conversation draft (VW1-VW19) is superseded; its three corrected premises
are in this order's header. Nothing is built.

### DP-Q0..Q13 marked, 2026-11-22

CLA gave the operator each item's proposal with its reason: DP-Q6 (a new
body is a new oid, because a body changed under one oid keeps the plan key
and lets a captured order or an administrator's plan run over a shape it
was not made for; an enabled administrator's plan RESTRICTs `DROP VIEW`,
CX-Q4 (a)'s rule), DP-Q1 (the absorbable form is exactly the bodies an
appended outer conjunct cannot change, a subquery predicate kept in its
block so its `up` levels do not move), DP-Q2 (refusing a non-absorbable
body buys no speed, since the same derived table written by hand takes the
same query-layer path; the cost stated - no `ANALYZE`, no plan, the whole
body folded), DP-Q4 (a view's name is in `sys.objects` with the tables',
so a view committed at its statement could outlive a `ROLLBACK` of the
table it reads; the cost stated - `sys.views` needs a transactional row
write, not `declaration_relation.hpp`'s `kBootstrapXid` writer), DP-Q13
(every DDL is `admin`'s and there is no per-relation grant), and the rest
in one line each. The operator marked all fourteen (a).

### DP-S0 - the census, 2026-11-22

Opened on the operator's standing go-ahead of 2026-11-22
(go-ahead-achieving-milestone, `raft-marks-2026-11-22.md` §5), in worktree
`sync-stale-records` on branch `worktree-dp-view` at `30c47abf`. Every
`[source-read]` line below was read at `30c47abf`; the full suite ran 0
failed of 5107 there (Debug, before any DP change). No engine code.

**Every site that takes a resolved `FROM` oid as a table.**

- The native bind is one loop: `CompileBlock`'s step 1
  (`src/exec/step_compiler.cpp:2575-2591`) resolves every relation of the
  `FROM`, every `JOIN` and every subquery block, declares it
  (`PositionSink::Position`) and reads its schema. `exec::Compile`
  (`:2290`) is the one entry above it, so absorption at `Compile`'s head
  reaches every native caller: `SelectBody` (`command_dispatcher.cpp:10753`),
  an `UPDATE`'s scalar subquery (`:11546`), `ON CONFLICT ... DO UPDATE`'s
  subqueries (`:7947`), the query layer's `DescribeRelation` (`:11675`),
  `WAYSTONE ON`/`OFF` (`:14898`, `:14927`), `CREATE PLAN` (`:15193`) and
  `ADVISE PLAN` (`:15475`).
- Read off the compiled chain as though one written relation were one
  step, and so to be fixed for an absorbed block: `SELECT *`'s rendering
  from `steps[0]`'s schema (`:10974-10983`), `CREATE PLAN`'s and
  `ADVISE PLAN`'s bindings and key oids by written position (`:15180`,
  `:15206-15210`, `:15463-15487`, `:15538`), and `PlanRowCanMatch`'s
  written-name check (`src/exec/plan_catalog.cpp:570-600`, which answers
  "nothing established" when the counts differ, `:575`).
- `FOR UPDATE` compiles no chain: `RunLockingRead` resolves the one
  relation and lowers its `WHERE` and select list by `CompileWhere` and
  `CompileSelectList` (`:13101-13136`), so DP-Q10 is served there by the
  absorbed statement, not by `Compile`.
- The query layer resolves a relation through `LayerHost::DescribeRelation`
  and `WalkRelation` (`src/exec/query_layer.cpp:662-703`, `:1284-1306`); a
  derived table is `FromItem::derived` (`ast.hpp:977-981`), but **the
  runner reads every block's `where_spans` against one statement text**
  (`query_layer.cpp:884-899`), so a view's block parsed from its own text
  must carry its own text into the runner, or a pushed conjunct would be
  read from the wrong bytes.
- Writes and DDL resolve their target through `Catalog::ResolveRelation`,
  whose gate matches `kTypeTable` only (`catalog.cpp:1601`, `:2213-2271`):
  `INSERT` (`:7919`), `UPDATE` (`:12347`), `DELETE` (`:16024`),
  `RETURNING` (`:11776`), `PURGE` (`:15597`, `:15826`), the KWP load
  (`kwp_load_server.cpp:391`), and every DDL target (`CREATE INDEX`,
  Cabin, `ALTER`, `DROP TABLE`, a foreign key's parent, Transition,
  Assertion). **DP-Q9's refusal is therefore one arm in the gate's miss
  path** - a name that is a view answers `NotImplemented` naming it -
  reaching every one of them at once.

**Every DDL that must ask a view's dependency list.** `DROP TABLE`'s
`restricted` lambda (`command_dispatcher.cpp:5137-5219`, asked before and
under the relation's `X`); `ALTER ... RENAME TO` and `RENAME COLUMN`
(`HandleAlter`, `:4653-4792`, beside AL4's assertion RESTRICT at `:4724`);
`DROP COLUMN` (`DropColumnRestriction`, `:4794`); `ADD COLUMN` (DP-Q16);
`DROP NAMESPACE` (`Catalog::CheckNamespaceEmpty`, `catalog.cpp:1743`, under
page 6); `DROP VIEW` of a view another view reads. The name checks -
`CheckNameFree` (`catalog.cpp:1675-1704`), `FindTableOidByName`,
`ExistingRelationReply` (`command_dispatcher.cpp:9546`) - test
`kTypeTable` and so must count a view as a live name and its tombstone as a
pending drop (NS6, CT7). `HighestIssuedUserOid` scans every `sys.objects`
row (`catalog.cpp:900`), so a dropped view keeps a tombstone row and its oid
is never reissued (DT2, DP-R3).

**Where absorption stands in the compile.** Before `CompileBlock`'s bind
loop and so before everything in `CompileBound`: the ON and WHERE lowering
(`step_compiler.cpp:3089-3140`), equality propagation (`:3143`), the
boolean-tree conjuncts (`:3131`), placement, and every plan source
(`:2614-2842`). An inner join's ON is one flat conjunct list with the WHERE
(`:3082-3087`), so a body's ON conjuncts may be appended wherever its
relations land. **One resolver**: `ResolveColumn` (`:677-760`) - qualified
by binding, unqualified exactly one match at the innermost level that has
one - is extended rather than paralleled, so an absorbed view's columns are
resolved by the rule every other column is.

**The golden corpus.** `tests/testdata/parser_corpus.txt` (752 lines,
`tests/parser_golden_test.cpp:185-196`) holds `CREATE`/`DROP` lines only
with the hash `-` (not a patternable head), and no `VIEW` line: a new
`CREATE VIEW` branch moves no hash and flips no verdict, so
`kFingerprintVersion` stays 2 (DP-R7).

**DP-Q0..Q13 restated against the census.** All stand as marked. Two read
differently against the tree and are written down here, not re-opened:

- **DP-Q1 `[quiet-wrong]`, premise re-read**: "an appended outer conjunct
  cannot change the body's answer" holds for exactly the native block -
  inner joins, conjuncts, plain columns - because an inner join's ON and
  WHERE are one conjunct list (`step_compiler.cpp:3082-3087`). **The
  wording "plain columns with optional aliases" is revised**: the native
  select list has no alias (`ast.hpp:147`), so a view's column names come
  from its column list (DP-Q15) and a body writing a select-list `AS` is a
  query-layer body, non-absorbable. The revision narrows what is absorbed
  and changes no answer: a body moved out of the absorbable set is answered
  by the query layer as a derived table, which DP-R1 names as the answer.
  Adopted so revised on the standing go-ahead.
- **DP-Q4**: no transactional writer for a text-bearing system relation
  exists (`declaration_relation.cpp:158-239`), so DP-Q4's "transactional, as
  a relation's" is met by DP-Q14's fixed-width rows, written under the
  DDL's transaction by the typed-row writer `sys.fkeys` uses.

**DP-Q14..Q22 raised and adopted (a)** on the standing go-ahead (§4).
DP-Q16 `[quiet-wrong]`'s premise, re-read: an unqualified name resolves at
the innermost level that has a match (`step_compiler.cpp:651-662`), so a
column added to a body's relation can capture a name a body subquery
resolved outward - hence the `ADD COLUMN` arm. DP-Q17 `[plan-identity]`'s
premise, re-read: `n` is both the key's width and the count every order and
pin is measured by (`captured_plans.hpp:93`, `admin_plans.hpp:167-174`), so
the view oids go beside it, not into it.
