# CN-11 — Concept Note: Templated Transaction — one call, one transaction, and a `CHECK` that aborts it

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §9.** Ordering
relative to CN-1 to CN-10 is not fixed; §6 names the notes it touches.
Author: CLA, 2026-09-30, against `2bb1f9d`
Origin: the operator's conversation of 2026-09-30. The operator asked how
commercial databases scope a stored procedure's or a trigger's
transaction; CLA answered from training knowledge (§1); the operator then
stated the concept (§0) and asked for this note. The operator's statement
is the one `[operator]` claim. The surface, the soundness rule of §4 and
every name are CLA's proposals (§9), none ratified.
Claim tags: `[operator]` for the operator's statement; `[source-read]`
with `path:line` at `2bb1f9d`; everything else `[design]`, including every
statement about VoltDB, Oracle, SQL Server, PostgreSQL, MySQL and Db2,
which comes from CLA's training knowledge and is to be re-checked against
each vendor's current documentation before a work order cites it.
Nothing is `[measured]`.
Relation to `docs/spec/protocol.md`: this note reserves no frame, bit or
code. A call is a statement and rides `C_PARSE`/`C_BIND`/`C_EXECUTE` as
they are (§3); the one status code it would need is O6.

---

## 0. The concept

`[operator]` *"나는 templated-transaction 이라는 개념을 생각중이야.
프로시져와 비슷한데, 한번의 호출이 하나의 트랜잭션에 대응된다는 개념이
있어. 그리고 트랜잭션 선언 어디든 CHECK를 통해 제약을 확인하는 기능을
제공하고, 만약 transaction이 커밋되기전에 CHECK가 실패하면 자동으로
롤백되는 개념이지."* — a templated transaction is like a procedure, but
one call is exactly one transaction; a `CHECK` may be written anywhere in
the declaration, and a `CHECK` that fails before the commit rolls the
transaction back automatically.

`[design]` Restated in this tree's terms, the concept has three parts,
and the note keeps them apart because each has its own cost:

1. **A declared, parameterised body of statements**, stored in the
   catalog and invoked by name.
2. **The call is the transaction.** No `BEGIN`, `COMMIT` or `ROLLBACK`
   is written or admitted inside it; the call begins it, the end of the
   body commits it, and any failure aborts it whole.
3. **`CHECK (predicate)` as a statement of the body**, evaluated where it
   stands, whose false (or unknown, O4) outcome aborts the call with a
   refusal naming the check.

## 1. What other systems ship

`[design]`, from CLA's training knowledge.

| system | is one call one transaction | commit/rollback inside | a failed condition |
|---|---|---|---|
| VoltDB (H-Store) | **yes** — a stored procedure is the unit of transaction, and the only one | not admitted | `voltAbort()` or an uncaught exception rolls the whole call back |
| Oracle PL/SQL | no commit boundary; an unhandled exception rolls back the call's own work to an implicit savepoint and leaves the caller's transaction open | admitted; `AUTONOMOUS_TRANSACTION` for an independent one | user code: `RAISE_APPLICATION_ERROR` |
| SQL Server T-SQL | **no** — under autocommit each statement commits; with `XACT_ABORT OFF` many errors abort one statement and continue | `BEGIN TRAN`/`COMMIT` with `@@TRANCOUNT` nesting | user code: `THROW` inside `TRY`/`CATCH` |
| PostgreSQL | **no** — a `PROCEDURE` called outside a transaction block may `COMMIT` mid-body, splitting the call; inside one, and always for a `FUNCTION`, it runs in its caller's transaction | `PROCEDURE` only, and only outside a transaction block | user code: `RAISE EXCEPTION`; PL/pgSQL has `ASSERT` |
| MySQL / InnoDB | **no** under `autocommit=1` | procedures yes; functions and triggers no | user code: `SIGNAL SQLSTATE '45000'` |
| Db2 | only inside a `BEGIN ATOMIC` compound statement | not inside `ATOMIC` | user code: `SIGNAL` |

Two lessons carry into §3. **The one engine that makes "one call, one
transaction" its model is the one built for short, known OLTP
transactions**, and it gets two things from it: one round trip per
transaction, and a transaction whose statements the engine knows before
the first one runs. **No surveyed engine has a declarative, enforcing
`CHECK` statement** in a procedure body: PL/pgSQL's `ASSERT` is the
nearest, and it is a debugging aid that `plpgsql.check_asserts` switches
off; every other engine spells it as control flow — test, then raise —
and a check written as control flow is invisible to the
engine: it cannot be listed, counted, named in a refusal or set to
observe (§6, CN-8).

## 2. What this tree has

`[source-read]` at `2bb1f9d` unless marked.

- **Failure atomicity is per transaction, and autocommit already aborts
  whole.** An `UPDATE` failing on row 7 of 10 inside `BEGIN` leaves rows
  1-6 written and the session in `failed-txn`; in autocommit the abort is
  automatic (`docs/spec/txn.md:912-918`). Savepoints and statement-level
  rollback are declared out of scope (`txn.md:980-981`). **A templated
  call is the autocommit path widened to N statements**: it needs no
  savepoint, because the only rollback it ever does is the whole one.
- **A commit that fails aborts** on both paths (`txn.md:920-932`), so a
  call whose commit fails is an ordinary abort, not a new state.
- **Two isolation levels and no `SERIALIZABLE`.** `READ COMMITTED`
  (default, a view per statement) and `REPEATABLE READ` (a view at
  `BEGIN`) (`txn.md:80-107`); `SERIALIZABLE` is out of scope because
  *nothing tracks which rows a reader read* (`txn.md:109-115`). This is
  the fact §4 is built on.
- **The lock family has a held `S` on a row**, and one reader already
  takes it: the foreign key's forward check takes `IS` on the parent
  relation and `S` on the parent row before it reads, held to the decide,
  and reads **under a check view minted after the grant**, at every
  isolation level (`docs/spec/foreign-keys.md:199-203`, `:235-238`). Its
  costs are written down: a
  parent `UPDATE` waits for open child writers, two child writers that
  then update their parent deadlock, and every `S` counts against
  `max_locks_per_txn` (`foreign-keys.md:264-290`); §8 carries them over.
- **Assertions are the engine's statement-time invariant**, fail-fast,
  with `DEFERRABLE` and `NOT VALID` reserved and refused `Unsupported`
  (`docs/spec/assertion.md:55`, `:61`, `:91`). An assertion guards every
  write to a relation; a template's `CHECK` guards one operation. §6 keeps
  the two apart.
- **`$name` lexes and is accepted by no production.** It was legal only
  in `CREATE PATTERN`'s body; since that grammar's withdrawal it is
  refused `Unsupported` by name and byte, and the token stays reserved
  for the extended protocol's named binds (`src/parser/parser.cpp:209-221`,
  `include/kds/parser/token.hpp:126`). It still fingerprints as a value
  (`src/parser/fingerprint.cpp:176`).
- **`CREATE PATTERN` was withdrawn for carrying two models of one
  thing** — a declared name beside the fingerprint that already
  identifies a pattern (`docs/spec/create-pattern-user-defined-patterns-v1.md:1-12`).
  §3 is written so that a template is not a second pattern model.
- **The wire already has a one-statement template.** `C_PARSE` computes
  the fingerprint at parse time; `C_BIND` binds typed parameters;
  pipelined frames after an `S_ERROR` are discarded to the next `C_SYNC`
  (`docs/spec/protocol.md:79-89`). A session holds at most 64 named
  statements (`include/kds/server/kwp_session.hpp:132`).
- **Scalar subqueries exist and are strict**: more than one row is
  `CardinalityViolation`, zero rows is `NULL` (`docs/spec/parser-v2.md:76`).
  A `CHECK` over `(SELECT balance FROM … WHERE id = …)` needs nothing new
  from the evaluator.
- **A new refusal earns a category**, never a fold into
  `InvalidArgument`: `FkViolation` and `AssertionViolation` are the
  precedent (`protocol.md:142-145`, `include/kds/base/status.hpp:87`,
  `:102`).
- **There is no `CHECK` constraint on a table, and no `CHECK (` anywhere
  in the grammar.** The assertion's `CHECK` is followed by `COUNT(*)` or
  `SUM(col)`, not a search condition (`include/kds/parser/ast.hpp:837-841`),
  and is **not reserved**: it is an identifier matched by text, as `GROUP`
  is (`src/parser/parser.cpp:972-973`, `:995-1000`).

## 3. The mechanism

`[design]`

- **Declaration.** `CREATE TEMPLATE name (param type, …) AS body END`;
  `DROP TEMPLATE name`. The body is a **straight line**: DML statements,
  `SELECT`s and `CHECK`s, in order. No branch, no loop, no variable
  assigned from a row (O5). The source text is stored in one catalog
  relation (`sys.templates`, name and oid only; O9) and parsed at first
  use, the way every statement is.
- **Invocation.** `CALL name(arg, …)` is **one statement**. Over KWP it
  is `C_PARSE`d once and `C_BIND`/`C_EXECUTE`d per call with typed
  parameters — no frame, no capability bit. Over the debug text port the
  arguments are literals.
- **Parameters** are named in the body. The sigil is O3: `$name` is the
  token a named parameter already lexes as, but it is reserved for a
  *client's* named binds on the extended protocol (D4), a different role
  from a parameter inside stored text.
- **Identity, and what is not a second model.** A template has a name
  because a caller must name it. Its **statements are still identified by
  their own fingerprints**: each body statement is an ordinary statement
  to Waystone, to `SHOW ACCESS` and to the Cabin controller, and a
  template adds no `sys.patterns` row, no `origin`, no recording policy.
  This is the line `CREATE PATTERN` crossed and this note does not.
- **`CHECK (predicate) [AS check_name]`.** Evaluated at its position, by
  the `WHERE` evaluator, under a **check view minted after its holds are
  granted** (§4) with the call's own writes visible — never the
  statement's or the transaction's view. True continues; false aborts the call and returns the
  refusal (O6) naming the template, the check (its name, else its
  ordinal) and its byte position in the stored source. **There is no
  "deferred" form, because in a straight-line body it collapses into the
  positional one**: a check that must hold at commit is the last
  statement of the body, and nothing but the commit follows it.
- **Transaction boundary.** The call begins a transaction at the session's
  isolation and durability, runs the body, and commits. `BEGIN`, `COMMIT`,
  `ROLLBACK`, `SET`, and every DDL are refused inside a body at `CREATE`,
  `Unsupported` with the byte. A `CALL` inside an explicit transaction is
  O2. A `CALL` inside a template body is refused, for the same reason.
- **The reply.** Per-statement row counts in the completion, and the rows
  of at most one `SELECT` — the body's last (O7).

The sketch:

```sql
CREATE TEMPLATE transfer (src BIGINT, dst BIGINT, amt DECIMAL(18,2)) AS
  UPDATE accounts SET balance = balance - $amt WHERE id = $src;
  CHECK ((SELECT balance FROM accounts WHERE id = $src) >= 0) AS no_overdraft;
  UPDATE accounts SET balance = balance + $amt WHERE id = $dst;
  INSERT INTO transfers (src, dst, amt) VALUES ($src, $dst, $amt);
END;

CALL transfer(41, 7, 100.00);
-- refused: template transfer, check no_overdraft failed (byte n); rolled back
```

## 4. What a passing `CHECK` guarantees

`[design]` This is the part of the concept that can be quietly wrong, and
the reason it gets its own section.

A `CHECK` reads a snapshot. A snapshot read is true of the snapshot, not
of the commit: between the check and the commit another transaction can
change a row the check read and did not write, and both commit. With no
rule, `CHECK` passes, the call commits, and the predicate is false of the
committed state — a write-skew, which this engine does not prevent in
general because it has no `SERIALIZABLE` (§2). A `CHECK` that is
sometimes false after it passed is worse than no `CHECK`: it is a
guarantee the reader believes and the engine does not give.

**The soundness rule (proposal): every key a `CHECK` reads is held by
the call until its decide, and the `CHECK` reads under a view minted after
the last hold is granted.**

- **The view.** A hold taken after the view was minted protects a version
  that may already be stale: under `REPEATABLE READ` a row frozen and
  committed between the call's `BEGIN` and the `S` grant reads as active
  and the check passes. The foreign key's forward check has the same
  problem and the same answer — a `check_view` minted at the check, at
  every isolation level (`[source-read]` `txn.md:719-725`,
  `foreign-keys.md:199-203`). A `CHECK` takes it the same way.
- **Keys the call wrote** are already held — each `X` is held to the
  decide. In the sketch, `no_overdraft` reads `accounts[$src]`, which the
  first `UPDATE` wrote: sound with no new mechanism.
- **A pk point the call did not write** takes `IS` on the relation and
  `S` on the key before the check view is minted, held to the decide —
  the forward check's pair. **The `S` holds the key's absence too**: the
  tuple unit is keyed by pk, not by a physical row
  (`[source-read]` `include/kds/txn/lock_table.hpp:361-363`), and both
  insert paths borrow the key's `X` before placing the row — a named key
  inside `AdmitExplicitRowId`, an issued one right after `AllocateRowId`
  (`src/server/command_dispatcher.cpp:5030-5058`). So a check that a row
  does *not* exist stays true too.
- **A pk window the call did not write** (`id BETWEEN $a AND $b`) can be
  held by an `S` **range** unit: the lock family has `kRange`, AR2 defines
  a fence as `S` or `X`, and an insert's `X` meets a fence on its key
  (`[source-read]` `lock_table.hpp:275-280`, `:369-376`; `txn.md:621-623`).
  No read tracking is needed because the window is declared by the body,
  not discovered by the read. Whether v1 builds it is O10; if not, it is
  refused `NotImplemented`, since nothing in the architecture forbids it.
- **Every key a `CHECK` reads must be a literal or a parameter** — the
  foreign key's enumerability rule (`foreign-keys.md:251-254`). A key read
  out of another row cannot be held before the read that finds it.
- **Anything else is refused at `CREATE TEMPLATE`, `Unsupported` with the
  byte**: a read through a secondary index, a non-pk predicate, a join.
  No lock unit covers them, and adding one changes AR2's units. An
  aggregate over rows is already refused by the grammar — a subquery
  holding an aggregate is AG8 (`[source-read]` `parser-v2.md:79`) — and
  an aggregate invariant over a relation is the assertion's job, which
  enforces it on every write rather than on one operation.

Rejected alternatives, stated so they are not rediscovered:

- *Re-evaluate every `CHECK` at commit.* Without holds, the re-evaluation
  has the same race one step later.
- *Run a template at `REPEATABLE READ`.* A single view makes the checks
  mutually consistent, not true at commit.
- *Admit unheld range checks with a warning in the manual.* A check that
  is sound only sometimes is the belief failure CN-8 §7 names.

The rule is decided at `CREATE`, by reading the body, which is possible
only because the body is a straight line whose `CHECK`s name their keys
by literals and parameters.

## 5. Transaction semantics

`[design]`

- **Abort is total and automatic.** A failed `CHECK`, a statement's
  refusal (`FkViolation`, `AssertionViolation`, `CardinalityViolation`, …)
  and a failed commit each abort the call whole through `txn.md` §6's
  `Abort`. The session is **not** left in `failed-txn`, because no
  transaction outlives the call (O2 decides the one case where one does).
- **Retry.** A `TxnConflict` (including a deadlock) is retryable and the
  call is the unit of retry: everything the retry needs is the name and
  the arguments. Whether the server retries a call itself is O8; the
  client can in any case, and CN-5's token makes a resend after a lost
  reply safe.
- **Envelope.** A call is a bounded transaction by construction — the
  10 s shape `txn.md` §1 serves, and the unit CN-10's admission queue
  would admit at its boundary.

## 6. Relation to other notes and structures

`[design]`

- **Assertions.** A global invariant on a relation, checked on every
  write, is an assertion. A condition of one operation, checked on that
  operation, is a template's `CHECK`. §4 refuses the `CHECK` that tries
  to be the first.
- **CN-4 (transitions).** A row-local `old => new` rule on every write;
  a template can enforce the same edge for its own writes only. The
  transition constraint is the stronger statement and stays CN-4's.
- **CN-5 (idempotency).** One call, one transaction, one token: a token
  on `C_EXECUTE` of a `CALL` covers the whole operation.
- **CN-7 (delta verbs).** `INC`/`DEC` inside a template body is the
  natural pairing; `CHECK` after `DEC` is the overdraft rule without a
  read-modify-write. Neither note depends on the other — but §4's "keys
  the call wrote are held" is true of `DEC` only while `DEC` takes the
  tuple `X`; CN-7's later ledger would break it.
- **CN-8 (enforcement mode).** A named `CHECK` is a declared invariant
  and can take `ENFORCE | OBSERVE` like any other; §7 leaves it out.

## 7. What this note does not license

- Any keyword, catalog relation, status code, frame or config key.
  `CALL`, `TEMPLATE` and `END` need reserve nothing: the tree matches
  `CHECK` and `GROUP` as identifiers by text (§2), and these would be
  matched the same way.
- An enforcement mode on a named `CHECK`; that is CN-8's generalisation.
- Control flow, variables, cursors, exception handlers, or a procedural
  language. The straight line is what makes §4 decidable.
- A trigger. A template runs when called, never on another statement's
  write.
- Statement-level rollback or savepoints (`txn.md:980-981` stands).
- Declared-ahead locking or static lock ordering from the body's
  footprint. The footprint is knowable; using it is a later note.
- A claim about cost. The round trips saved and the held `S`'s price
  have not been measured, and the overhead measurement is per milestone.

## 8. Known consequences

`[design]`

- **`S` holds cost what they cost the foreign key**: an `UPDATE` of a row
  another open call's `CHECK` read waits for that call, and two calls that
  each check a row the other then writes deadlock and one is refused.
  §4's soundness is bought with exactly this, and the manual must say so.
- **Some checks cannot be written.** "No open order of this customer is
  past due" reads a non-pk predicate and is refused at `CREATE`. That is
  §4 working, not a gap.
- **`NULL` is a decision, not a detail.** SQL's table `CHECK` passes on
  unknown; `WHERE` rejects it. A template `CHECK` that passes on unknown
  lets `(SELECT … WHERE id = $missing) >= 0` through for a row that does
  not exist (O4).
- **A template names relations and columns by text.** A `RENAME` or
  `DROP` under it breaks the next call at bind, or is refused RESTRICT
  (O9).

## 9. Operator decisions this note leaves open

Each carries CLA's proposal, none ratified.

| # | question | CLA's proposal |
|---|---|---|
| O1 | Name and spelling | **`CREATE TEMPLATE … AS … END` / `CALL`**; "procedure" suggests the control flow and in-body commit §3 refuses |
| O2 | `CALL` inside `BEGIN` | **Refuse, `Unsupported`**: admitting it breaks "one call, one transaction", and running the body as part of the outer transaction is the per-transaction failure atomicity §5 exists to avoid |
| O3 | Parameter sigil | **`$name`**, the token a named parameter already lexes as (§2). The cost: if D4's client named binds are wired, a `CREATE TEMPLATE` sent through `C_PARSE` has `$` names that are not that statement's binds, and the parser must tell the two apart by production |
| O4 | `CHECK` on unknown | **Fail**: a check that could not be established was not established; `WHERE`'s three-valued rule, not table `CHECK`'s |
| O5 | Values from one statement into the next (`SELECT … INTO`) | **Not in v1.** It keeps the body a straight line but widens what §4 must read at `CREATE`; a consumer first |
| O6 | The refusal | **A new category, `CheckViolation`**, `retryable = 0`, naming template, check and byte, on `protocol.md` §11's rule; not `AssertionViolation`, which names a different declaration |
| O7 | The reply | **Row counts per statement, plus the rows of the body's last `SELECT` if it is one** |
| O8 | Server-side retry on `TxnConflict` | **No**; the client retries the call, with CN-5's token where it has one |
| O9 | Catalog and dependencies | **`sys.templates` with the source text; `DROP`/`RENAME` of a relation a template names refused RESTRICT**, as fkeys and assertions are |
| O10 | §4's soundness rule | **Ratify as written**: keys enumerable; own writes already held; pk points `IS` + `S`; the check view minted after the last grant; pk windows by an `S` range unit, **refused `NotImplemented` in v1** until a consumer names a window check; everything else refused at `CREATE`. This is the decision the note cannot proceed without |
