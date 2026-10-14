# Work order BV — Transition: a column's allowed state changes as a row-local constraint

Written 2026-10-14 on `worktree-cn4-transition-refresh` from `b0bc959c`
(`v3.1.0-31-gb0bc959c`). It follows the operator's words of 2026-10-14,
recorded in English:

- **W0:** *"First fix the note to match the present."* This was done as a
  re-read annotation (CN-4 §9). The note's body is unchanged, which is the
  form the operator chose over a rewrite (`docs/conceptnotes/README.md`).
- **W1:** *"Now mark that concept note [open] and write a work order that
  carries the work plan against the current code. Mark it v3.2.0."*
- **W2:** *"Q1: (a)."* This marks BV-Q1 (a) as written: oid 146,
  column oids from 147, root page 16, overflow at 17, superblock 24
  refusing 23.

**Written 2026-10-14; not opened for building.** W1 asks for a plan. It
does not give the word to build, and it does not mark §4. CN-4 §8 is
unanswered, so every proposal in §4 is CLA's and unmarked. **No stage
past BV-S0 starts until its gate in §3 is marked.** BV takes up
`docs/conceptnotes/cn4-transition-constraints.md` (CN-4). From this
commit, this order is the authority and the note is history
(`docs/conceptnotes/README.md`).

**Version.** The operator names this order's version **v3.2.0** (W1). The
annotated tag is a separate act and has not been taken. Until it is,
`git describe --tags` reads `v3.1.0-*`, and every measurement carries that
string.

BV was checked for a `BV-` token or a `workorder-bv` name against every
file under `instructions/`, `docs/` and `CLAUDE.md` at `b0bc959c`. No use
was found. The last letter assigned is BU.

**Claim tags:** `[source-read]` with `path:line` at `b0bc959c`; `[design]`
for CLA's reasoning about code not yet written; `[quiet-wrong]` marks an
item where a wrong choice turns a refusal into a wrong answer. Nothing is
`[measured]`.

## 0. What BV is

### 0.1 Subject

A **Transition** is declared on one column of one relation. It names:

- the states an `INSERT` may write (`ON INSERT`);
- the `old => new` edges an `UPDATE` may take (`ON UPDATE`);
- optionally, the states a row may be deleted from (`ON DELETE FROM`).

Each check reads one row and the catalog, and nothing else.

The path property follows by induction over committed versions
(CN-4 §2). Example: "`FINALIZED` only after `CHECKED`". The engine never
stores a row's history and never checks one.

```
CREATE TRANSITION order_status ON orders(status)
  ON INSERT ('NEW')
  ON UPDATE ('NEW' => 'CHECKED', 'CHECKED' => 'FINALIZED',
             'NEW' => 'CANCELLED', 'CHECKED' => 'CANCELLED')
  ON DELETE FROM ('NEW', 'CANCELLED');
DROP TRANSITION order_status;
SHOW TRANSITIONS;
```

### 0.2 Not in BV

These are CN-4 §7, unchanged:

- A trigger, or any action on a transition.
- A stored history or audit log.
- A state machine spanning columns or relations.
- Guarded edges (`WHEN ...`).
- Freezing a row in a terminal state.
- A cost claim before §5's measurement.

## 1. Survey at `b0bc959c`

### 1.1 The three check sites already exist, one per verb

`[source-read]`

- **`INSERT`.** `InsertOneRow` (`src/server/command_dispatcher.cpp:5658`)
  takes the row as `body`, the columns after the pk (`:5729`). It runs the
  foreign-key forward check (`:5746`) and the assertion admission
  (`:5755`) **before the id is allocated**, so a refused row burns
  nothing. Every `INSERT` reaches it:
  - textual and multi-row `VALUES`, through `InsertParsed` (`:5253`);
  - the KWP load stream, through `ExecuteInsert` (`:5232`), called from
    `src/server/kwp_load_server.cpp:513`.

  `SortedFillInner` (`:5432`) is the one other placement. It is
  heap-gated, so SUS-1 leaves it reachable only for a heap relation
  created before the suspension.
- **`UPDATE`.** In `UpdateInner` (`:7935`), each row is handled in this
  order:
  1. the borrow and the conflict check (`:8246`);
  2. the foreign-key verdict (`:8340`);
  3. `previous` is taken (`:8351`);
  4. the assertion delta check, under the no-refuse window (`:8359`).

  **A row is decoded whole only where a hook reads it** (`:8275`). A
  relation with no Cabin, index or assertion decodes only
  `set_read_mask`, and every other slot holds `kUndecoded`, which the
  evaluator refuses as `Corruption`.
- **`DELETE`.** `DeleteInner` (`:9909`), in its mark lambda:
  1. completes the frame whole, ungated (`:10052`);
  2. takes the borrow and the conflict check (`:10063`–`:10067`);
  3. runs the assertion departure (`:10102`);

  all before the undo record and the delete-mark (`:10155`).

### 1.2 The memo is the fence; no registry is needed

`[source-read]` A write path reads its relation's constraints off
`TableAccess`, the statement's catalog memo. The index list does this
(`indexes`, `include/kds/catalog/schema.hpp:354`), and it is the precedent
that matters: `CREATE INDEX` adds to an existing relation. A foreign key
also rides the memo (`fkeys_out`, `:293`), but it is declared only at
`CREATE TABLE` and there is no `ALTER` (`:284`), so a child's `fkeys_out`
never changes under a writer and is no precedent for the fence.

A DDL takes the relation `X`, writes the catalog and moves the schema
version word before releasing. A writer's first relation intention checks
`Catalog::MemoIsCurrent` (`command_dispatcher.cpp:9181`, whose comment
names `CREATE INDEX` and `CREATE ASSERTION`); a stale memo re-runs the
statement before it writes anything.

The assertion asks a registry instead (`enforcer_->AnyOn`, `:8289`). The
reason is AT-S5d: an assertion carries derived state adopted under the
build's `X`. A Transition carries none, so it rides the memo, as the
index list does.

`[source-read]` AT-S5e recorded the memo check as **uncovered**: *"C2's
stale-memo check survived its mutant 0/1"*
(`instructions/v3.0.0/workorder-at-m3-uniformity.md:486`). A Transition
created in that window would be skipped without any refusal. BV owes that
cell (BV-R4).

### 1.3 A new catalog relation is a format event

`[source-read]`

- **Catalog root pages.** Roots are 4–14; 10 and 15 are retired and never
  reissued. The overflow range starts at 16
  (`include/kds/catalog/well_known.hpp:285`–`:367`). Its rule is *"It
  walks up by one with every new bootstrap relation, on that relation's
  superblock bump"* (`:365`).
- **Superblock.** The current version is 23 (`superblock.hpp:139`). The
  standing order of 2026-10-07 refuses older volumes with no migration.
- **Storage shape.** State values are `char(N)`, `varchar(N)` or integers,
  and edge lists are unbounded. That is the shape `sys.assertions` was
  made a row-codec relation for. It stores its declaration's text and
  re-parses it (`well_known.hpp:153`–`:170`; `assertion.md` AS10). Its
  readers live in `exec/` (`include/kds/exec/assertion_catalog.hpp`)
  because decoding needs the row codec.
- **Well-known oids.** Catalog relations sit in 100–132, with 115 and
  120–124 retired; 133 is retired; 140–145 is sys.assertions' column run.
  The rule is that a new oid goes *"past the highest rather than into a
  gap"* (`well_known.hpp:151`, `:277`). `kAllWellKnownOids` (`well_known.hpp:247`)
  makes the compiler read every one.

### 1.4 Surface

`[source-read]`

- `CREATE ASSERTION` is parsed by `Parser::ParseAssertion`
  (`src/parser/parser.cpp:1970`–`:2248`) and dispatched to
  `HandleAssertion` (`command_dispatcher.cpp:3106`). Its AST node is
  `AssertionStmt` (`include/kds/parser/ast.hpp:981`), one alternative of
  `Statement` (`:1038`).
- `SHOW ASSERTIONS` is dispatched at `command_dispatcher.cpp:1265`.
- `TRANSITION` appears nowhere in `src/`, `include/` or `tests/`.
- A grammar addition is additive shape and moves no `kFingerprintVersion`
  (`docs/spec/parser-v2.md:88`). `tests/parser_golden_test.cpp` and
  `tests/fingerprint_test.cpp` pin that.

### 1.5 The violation code

`[source-read]`

- `StatusCode::kAssertionViolation` is at `include/kds/base/status.hpp:103`.
- The wire categories are append-only. `kFkViolation` and
  `kAssertionViolation` took 16 and 17
  (`include/kds/wire/kwp.hpp:247`–`:248`).
- A Transition's refusal would take the next category.

### 1.6 `PURGE` breaks CN-4 §2's induction

`[source-read]` BH-S3 (`26766699`) made this possible:

1. `DELETE` from a deletable state;
2. `PURGE`;
3. a named `INSERT` of the same key in an initial state.

The row's history restarts under its old identity
(`docs/spec/heap-and-tuple.md:286`, §4.1c; `src/exec/purge_key.cpp:14`).

- **Scope.** `PURGE` runs on btree relations only, autocommit, admin
  role. Since SUS-1, every new relation is a btree.
- **Nothing remembers a purged key.** The slot is retired keyless, so a
  later `INSERT` cannot tell a purged key from a never-placed one. Any
  answer that refuses the re-insert therefore needs new durable state,
  which BV-R1 rules out.

### 1.7 Surrounding facts BV must keep

`[source-read]`

- **`DROP TABLE`.** It RESTRICTs on foreign keys and assertions, and asks
  again under `X` (`docs/spec/drop-table.md`, `command_dispatcher.cpp:2889`).
- **Recovery.** Catalog writes are logged and replayed, and DDL losers are
  rolled back at mount (`docs/spec/ddl-transactional.md`). A Transition
  adds no record kind.
- **Statement-level rollback.** There is none: a refusal on row 3 of 10
  fails the transaction (`docs/spec/assertion.md:193`).
- **First-updater-wins** (`docs/spec/txn.md` §5). The value a check reads
  as "old" is the head.
- **`SET` expressions** (BJ, `docs/spec/types.md` §3.2b). A `SET` value
  can be an expression, evaluated per row against the before-image. The
  check compares the computed value with the replaced one.
- **BU-S8, planned.** It will add `INSERT` with a column list,
  `INSERT ... SELECT`, expressions in `VALUES`, `ON CONFLICT DO NOTHING`,
  `UPDATE ... FROM`, `DELETE ... USING` and `RETURNING`
  (`instructions/v3.0.0/workorder-bu-basic-query-surface.md:168`). Each is
  a new write path BV-R2 binds.

## 2. Rulings

### BV-R1 — Row-local and stateless

A Transition is its catalog row and nothing else:

- no registry, no derived structure, no WAL record kind, no cross-core
  message;
- a check reads the row being written and `TableAccess`;
- recovery has nothing to rebuild.

A design that needs more than that has left CN-4's concept and goes back
to the operator.

### BV-R2 — Every write path checks, or refuses the write `[quiet-wrong]`

A path that writes the state column, and cannot run the check, refuses the
statement on a relation carrying a Transition. It never skips the check
(CN-4 T8). This binds every path added after BV, BU-S8's first among them.
BV pins it with a test per path, and the test is written so a path added
without the check fails it.

### BV-R3 — The check refuses before anything is written

**Placement:**

- **`INSERT`:** beside the foreign-key forward check, before the id is
  allocated.
- **`UPDATE` and `DELETE`:** after the conflict check, and before the
  assertion's reservation, the undo record and the no-refuse window
  (BE-Q11).

**Why "after the conflict check":** the old value it reads is the head the
borrow holds (§1.7).

**Refusal:** the row's refusal, and it fails the transaction (§1.7).

### BV-R4 — The fence is the relation `X` and the memo check, and BV proves the memo check `[quiet-wrong]`

`CREATE TRANSITION` and `DROP TRANSITION` take the relation `X`, as the
index and assertion builds do (AT-S5e). They write the catalog and move
the word before release.

BV adds the cell AT-S5e did not have:

1. a writer held between its task boundary and its first relation
   intention;
2. a `CREATE TRANSITION` that commits in between;
3. the writer's statement must re-run and meet the check.

The mutant that removes `MemoIsCurrent`'s refusal must fail that cell on
repeated runs, with a barrier forcing the interleaving.

### BV-R5 — Nothing new is reserved

`TRANSITION` hashes as an identifier. `kFingerprintVersion` does not move,
and the golden corpus is the proof, not the assumption.

## 3. Stages

| Stage | Builds | Gate |
|---|---|---|
| **BV-S0** | This order; CN-4 marked `[open]` | written |
| **BV-S1** | Catalog and format: `sys.transitions` (BV-Q1); the superblock bump, refusing the previous version; the bootstrap row and oids; the `exec/` reader; `TableAccess` carrying the relation's Transition. No SQL reaches it | BV-Q1 marked |
| **BV-S2** | Surface and enforcement, together, so no Transition can be declared before every write path checks it: `CREATE`/`DROP`/`SHOW TRANSITIONS`; CN-4 §4's create-time refusals; the build scan under `X`; the three check sites (BV-R3), including the `UPDATE` read mask (§1.1); `TRANSITION_VIOLATION` (BV-Q3); the `DROP TABLE` arm (T7); BV-R2's per-path tests; BV-R4's cell | BV-Q2, BV-Q3, CN-4 T1–T8 and T10 marked |
| **BV-S3** | The `PURGE` answer (BV-Q4) and the CN-3 answer (BV-Q5); the sim harness gains a Transition op and an oracle that walks the edge graph over committed versions, crashed and reconciled | BV-Q4, BV-Q5 marked |
| **BV-S4** | The close: `docs/spec/transition.md` as the owning spec, the manual, the `CLAUDE.md` milestone row, CN-4's status, overhead measured once (§5) | — |

Each stage runs the Session Workflow:

- a `critics-developer` review of the stage's commit;
- the full suite;
- a row in §6.

A stage lands with "overhead not measured; measured at the milestone's
close".

## 4. Items for the operator

| # | Question | CLA's proposal | Mark |
|---|---|---|---|
| BV-Q0 | Open BV whole, or stage by stage | Stage by stage, each on its gate's mark. W1 asked for the plan, not the build | — |
| BV-Q1 | Where a Transition is stored | (a) A new row-codec catalog relation `sys.transitions` (oid 146, past the highest well-known oid as `well_known.hpp:151` requires, with a fixed run of column oids from 147 like `kSysAssertionsColumnOidBase`, because its `sys.columns` rows are written during bootstrap; root page 16, overflow moving to 17), storing the declaration's text and re-parsed into `TableAccess`; the superblock goes to 24 and refuses 23, under the standing order. (b) Rows of a new kind in `sys.assertions`: one relation for two concepts, a second meaning for one name. **(a)** || **(a), marked by the operator 2026-10-14** (W2) |
| BV-Q2 | The grammar | §0.1's shape. `ON DELETE FROM` lists states; `=>` is one new token. A `DROP TRANSITION` inside an explicit transaction follows `DROP ASSERTION`'s rule | — |
| BV-Q3 | The violation code | A new `StatusCode::kTransitionViolation`, wire category 18, token `TRANSITION_VIOLATION`, non-retryable; the message names the Transition, the row id and both values (CN-4 §4) | — |
| CN-4 T1–T8, T10 | The semantics | CN-4 §8's proposals, unchanged. Not restated here so the two cannot drift. Each is marked here, beside its id, when the operator marks it | — |
| BV-Q4 (CN-4 T11) | `PURGE` restarts a row's history (§1.6) | (a) **`PURGE` is refused `Unsupported`, at the relation's byte, on a relation carrying a Transition.** A key on it stays bound for life, and the refusal names the Transition. (b) Accept the restart: the graph speaks for one tenure of a key, and the spec says so. (c) Refuse the re-insert: needs a durable record of purged keys, against BV-R1. **(a)**, because (b) makes the guarantee false for exactly the rows an administrator touched, and says so only in a document | — |
| BV-Q5 (CN-4 T9) | A CN-3 successor's state | CN-3 is unbuilt (`CHAIN` appears nowhere in `src/parser/`). Answer now in the strict form: **a successor is an `INSERT` and meets the initial set.** CN-3's order may ask to relax it, against this order's tests | — |

## 5. Measurement

Once, at BV-S4, over BV's whole code change: `ck-tester`'s interleaved A/B
in `build-release`, against `b0bc959c`. The budget is about one hour,
building included.

| Cell | Workload | Expected |
|---|---|---|
| 1 | `INSERT` and `UPDATE` on a relation with no Transition | No change. The check is one empty-list test |
| 2 | The same workload with a Transition on a `varchar` state column | The per-row cost of the check, stated, not judged |

## 6. Row status

### BV-S0 — written 2026-10-14

This document, on `worktree-cn4-transition-refresh` from `b0bc959c`. CN-4
is marked `[open]` and cites this order. No code, no spec edit, no test.
The suite was not executed and no pass is claimed. Overhead not measured.
