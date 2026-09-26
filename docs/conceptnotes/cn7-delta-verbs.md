# CN-7 — Concept Note: Delta Verbs — `INC` / `DEC` as SQL, on every numeric column, with the value returned

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §8.** Ordering
relative to CN-1 to CN-6 and CN-8 is not fixed. Sagas, compensation
registration and cross-instance coordination are **out of this note**
(§6); so is the concurrent-hold ledger (escrow), which §6 places under a
later note of the physical optimizer's.
Author: CLA, 2026-09-26, against `2b41d31`
Origin: the operator's proposal of 2026-09-26, in conversation, of a
commutative SQL surface — `INC`/`DEC` supported at the SQL level — and
three rulings on it the same day (§0). The rest is CLA's elaboration and
the open items in §8.
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `2b41d31`; everything else `[design]`. One
`[measured]` claim, AT-S13's, cited by file.
Relation to `docs/spec/protocol.md`: this note reserves no frame, bit or
code. `RETURNING` needs a row-bearing reply on a write (§3), which is a
change to the reply *sequence*, not a new frame; O6 names it for that
spec to decide.

---

## 0. The concept, and the operator's three rulings

`[operator]` A SQL syntax with commutativity: operations such as `INC`
and `DEC` supported at the SQL level, so that a write states *a change*
rather than *a value*.

`[operator]` Rulings, 2026-09-26:

| # | ruling |
|---|---|
| R1 | **No implicit conversion.** An `UPDATE ... SET` is never turned into an `INC`/`DEC` by the engine; the verb is written or it is not there |
| R2 | **Universal.** The verbs apply to every column of a suitable type, with **no column declaration** (no `ESCROW`, no `RESERVABLE`) |
| R3 | **`RETURNING` at admission.** The statement returns the value the write produced |

`[design]` R2 and R3 together fix the execution model. A concurrent-hold
ledger (escrow) is what would let two transactions `INC` one row at
once, and its price is that the value is not known at admission — which
R3 excludes — and that the column carries a structure — which R2
excludes. So under these rulings an `INC` is **a delta write under the
ordinary tuple lock**: it takes the borrow every write takes, reads the
current version under it, applies the delta, writes the new version and
returns it. §1 says what that still buys; §7's first row says what it
does not.

## 1. Why a delta verb, when `SET` exists

`[source-read]` **Today a delta cannot be written in one statement.**
`UPDATE`'s grammar is `SET <col> = <val>` (`manual/sql/sql.md:536`) and
an `Assignment` carries an `AstValue` — a literal or a parameter, not an
expression (`include/kds/parser/ast.hpp:694-696`). `SET balance =
balance - 100` does not parse. The only way to apply a delta is the
client's: `SELECT`, compute, `UPDATE SET` the value. Under the default
READ COMMITTED that pair is the lost-update pattern — the second client
computes from a value the first has already changed — and the engine's
only defence is the write conflict it refuses retryably when the row's
header names a writer the snapshot cannot see (`sql.md:982`;
`docs/spec/txn.md:586-592`). The client then retries the whole pair.

`[design]` So `INC` is not sugar over an existing expression. It is the
engine's **first server-side read-modify-write**, and what it buys
follows from that:

| gain | why |
|---|---|
| **One round trip, no client-side arithmetic.** | The read happens under the lock, after any wait, so the delta is applied to the value that is actually there. The `SELECT`+`UPDATE` pair, and its retry loop, go |
| **The conflict becomes a wait, where the engine already waits.** | A writer meeting an undecided holder on its own core waits for the decide (AO-S3, `txn.md:589-590`); after the wait `INC` applies to the new version and needs no re-issue. A `SET` after the same wait would need the client to recompute |
| **The log carries a delta.** | A record that says *+100* replays in any order and composes; a record that says *=900* does not (§4) |
| **The inverse is defined.** | `DEC n` undoes `INC n` without a before-image. This is what makes a later compensation note safe by construction rather than by comparison (§6) |
| **The intent is in the statement.** | The engine, the optimizer and a reading agent know the write is commutative. If a ledger tier ever exists (§6), the statement does not change; only its execution does |
| **A row bound is one predicate away.** | `DEC balance BY 100 WHERE id = ? AND balance >= 100`: the guard is evaluated under the same lock as the write, and a violation is `UPDATED 0` — no declaration, no new refusal code |

`[design]` **What it does not buy, stated once.** Against the tuple lock
an `INC` is exactly an `UPDATE`: same borrow, same wait, same
serialisation. It is not faster than `SET` and the manual must say so.
The hot-row shape AT-S13 measured is unchanged by this note (§7).

## 2. What this tree has

`[source-read]` unless marked.

- **`UPDATE` is a `SET` list of values and a `WHERE`** (`sql.md:536`,
  `ast.hpp:709-713`). The pk cannot be assigned, refused `Unsupported`
  with the column's byte at compile (`sql.md:538-546`, K2). The same
  refusal covers `INC` on the pk (§3).
- **No `RETURNING` exists**; the write reply is `UPDATED <n>`
  (`sql.md:548`). A write that returns rows needs the row-description and
  row-batch frames a `SELECT` sends, on a write's reply (O6).
- **The write conflict and the wait.** The row-level wait's predicate is
  the core's in-flight set, so a holder on another core reads as settled
  and the writer is refused retryable (`txn.md:589-592`;
  `docs/inflight/known-gaps.md`, Locks). AX (instance in-flight
  publication) is the order that makes the wait instance-wide.
- **Arithmetic is checked.** `SUM` uses overflow-checked int64, int128
  for `DECIMAL128`, and an overflow is a statement error (AG3,
  `docs/spec/aggregate.md:19`). `INC`/`DEC` take the same rule and the
  same error (§3).
- **The numeric types.** `int64`; `DECIMAL(p,s)` as scaled int64 for
  `p ≤ 18`; `DECIMAL(p,s)` / `DECIMAL128(p,s)` as int128 for `19 ≤ p ≤ 38`
  (`docs/spec/types.md:15`, `:41`). A delta on a decimal column is a
  same-scale addition (§3).
- **The record types.** Every data mutation is one of
  `kHeapInsert`/`kHeapOverwrite`/`kHeapDeleteMark`/`kSlotRetire` per row
  (`docs/spec/wal.md:152`). An `INC` today would log a `kHeapOverwrite`
  with the new image; §4 proposes a delta record beside it.
- **The measured shape.** `[measured]` AT-S13, `cores = 8` spread: 3.9%,
  5.7% and 5.9% of hot-row updates refused, zero when the sessions are
  pinned to one core; the file attributes an unquantified part of the
  refusals to the per-core wait predicate above rather than to contention
  itself (`bench/v3.0.0/results-at-s13-prices-v2.7.0-391-gf6f2073.md:136-151`).

## 3. The mechanism

`[design]`

- **Grammar.**

  ```
  UPDATE <table>
    { INC | DEC } <col> BY <val> [, { INC | DEC } <col> BY <val> ...]
    [WHERE <predicate>]
    [RETURNING <col> [, <col> ...]]
  ```

  `<val>` is a literal or a parameter, like `SET`'s (§2). A statement is
  either a `SET` list or an `INC`/`DEC` list; mixing the two is refused
  `Unsupported` with the byte of the second verb (O3). `DEC col BY v` is
  `INC col BY -v` at the executor and keeps its own spelling for the
  reader's sake.
- **Admissible columns (R2).** Any column of `int64`, `DECIMAL(p,s)` or
  `DECIMAL128(p,s)`. The pk is refused as it is for `SET` (K2, same
  message, same byte). Any other type is `Unsupported` naming the type
  and the byte. Nothing in the catalog marks a column; the check is the
  compile-time type check every assignment already gets.
- **Execution.** Compile to the `UPDATE` chain with a per-row delta step
  in place of the value copy. Per row, under the tuple borrow the
  ordinary write takes: read the current version's column; add the delta
  with checked arithmetic — int64 with overflow, int128 for the wide
  decimal, same-scale for decimals, an overflow the statement error AG3
  names; write the new version through the ordinary heap/index/Cabin
  hooks; if `RETURNING` names columns, emit the new version's values as
  a row. A `NULL` column is left `NULL` and counted neither as updated
  nor as an error (O4).
- **`RETURNING` (R3).** The reply becomes a row description, the rows,
  then `UPDATED <n>`; the value is the one written, read under the lock,
  exact. It is a value at the statement's snapshot plus this write: not a
  promise about what the row holds after commit, which is every
  `RETURNING`'s meaning everywhere. Without `RETURNING` the reply is
  unchanged.
- **Bounds.** None declared. A guard is written in the `WHERE` and
  evaluated per row under the same borrow as the write; a row that fails
  it is not written and not counted. `UPDATED 0` on a point write is the
  bound's signal.
- **Assertions.** A delta on a column that is an assertion's `SUM` column
  is the ordinary checked path with `Δsum` = the delta
  (`docs/spec/assertion.md` §4.2: only a positive delta is checked). No
  new path.
- **Cabins and indexes.** The column changed is a value change like any
  `SET`'s: the append-only hooks run unchanged.

## 4. The log record

`[design]` A `kHeapDelta` record beside `kHeapOverwrite`: page, slot,
column, the signed delta at the column's width, plus the same header
every row record carries. Redo applies the delta to the page's current
value; undo applies its negation. Two properties follow. **Replay is
order-free** across deltas to one row, which the overwrite record is
not. **The record is smaller** than an image of the row when the row is
wide. Both are consequences, not the reason: the reason is that the log
should say what the statement said (§1).

Whether the record is worth its type — an overwrite with the new image
is correct and exists — is O5. Recovery's high-water repair and the
full-page-image rules apply unchanged; a delta record is a physiological
record like the rest.

## 5. Transaction semantics

`[design]` Nothing new. An `INC` is a write: it takes the borrow, is
visible to no other view until commit, rolls back with its transaction
by the undo chain, poisons the transaction on any of its own errors
(overflow, refusal) like every write, and obeys the durability class the
transaction chose. Its interaction with a concurrent writer is the
ordinary one — wait on the same core, refusal across cores until AX —
and this note changes neither.

## 6. What this note does not license

- **A concurrent-hold ledger (escrow).** Two `INC`s on one row proceeding
  at once, the `[low, high]` interval, declared `MIN`/`MAX`, and a
  `RETURNING` that yields an interval. R2 rules out a declared column and
  R3 rules out an interval; so if the ledger comes it comes **without a
  declaration, opened where contention is observed** — the shape of an
  Observational Cabin, decided by the physical optimizer, under a note of
  CN-1's family, after AX has landed and AT-S13's driver has been re-run
  without the wait gap. This note's one obligation to that future is
  §1's last-but-one row: **the statement and its inverse mean the same
  under a ledger as under the lock.**
- Sagas, compensation callbacks, at-most-once tokens (CN-5) and any
  cross-instance step. The inverse is defined here; who calls it is not.
- Any commutative operation other than addition: set insertion,
  max-merge, string append, and the rest of the CRDT catalogue.
- `SET col = <expression>`. Expressions in `SET` are a parser question
  this note does not open; R1 says they would not become `INC`.
- A performance claim. The lock path is the `UPDATE` path; nothing is
  faster and nothing is measured (§8, O7).

## 7. Known consequences

`[design]`

- **Hot-row concurrency is unchanged.** The 4–6% AT-S13 refuses under
  spread, and whatever remains of it once AX turns the cross-core refusal
  into a wait, is this note's inheritance, not its cure. The cure is the
  ledger §6 defers.
- **`INC` is not faster than `SET`.** Same borrow, same wait, one extra
  addition. The manual states it in the verb's own section.
- **A `RETURNING` on a multi-row write is a result set of that size.**
  A `WHERE`-less `INC ... RETURNING` on a large relation streams the
  relation. The row-batch flow control the protocol already has bounds
  it; the manual warns.
- **`NULL` arithmetic has to be decided** (O4). SQL's `NULL + 100` is
  `NULL`, which makes an `INC` on a `NULL` counter a silent no-op —
  correct by the standard and surprising to a counter's author.
- **Two log spellings of one write.** If O5 takes the delta record, an
  `INC` and a `SET` to the same value leave different records and the
  same page. Tooling that reads the log sees the difference; nothing
  else does.

## 8. Operator decisions this note leaves open

Each carries CLA's proposal, none ratified. R1–R3 are ratified (§0) and
are not repeated here.

| # | question | CLA's proposal |
|---|---|---|
| O1 | Verb spelling | **`INC col BY v` / `DEC col BY v`** inside `UPDATE`, so the statement class, the `WHERE` production and the pk refusal are `UPDATE`'s. Not a new top-level statement |
| O2 | Admissible types | **`int64`, `DECIMAL`, `DECIMAL128`**; everything else `Unsupported` with the type and the byte. No implicit widening: the delta takes the column's type |
| O3 | Mixing `SET` and `INC` in one statement | **Refused in v1**, `Unsupported` at the second verb's byte. Two statements in one transaction cost nothing this cannot |
| O4 | `INC` on a `NULL` value | **Leave `NULL`, do not count the row as updated**, and say so in the manual. The alternative — treat `NULL` as zero — is a silent coercion the type system elsewhere refuses |
| O5 | Log record | **A `kHeapDelta` type** (§4). The overwrite record is correct and would do; the delta record is the one that says what the statement said, and is smaller on wide rows |
| O6 | `RETURNING` on a write's reply | **Row description + row batches + `UPDATED <n>`**, the `SELECT` frames on a write, no new frame. `protocol.md` owns the sequence and says whether a client that did not ask for rows can receive them |
| O7 | Measurement gate | **None before the stage.** The path is `UPDATE`'s with one addition. The stage's row states "not measured"; the first AS-E cell that includes it confirms `INC` ≈ `SET` and reports the `RETURNING` reply's cost |
| O8 | Target rows | **Point and set alike**, as `UPDATE` is. Restricting to the pk point would be a limit the lock model does not require |
| O9 | The ledger tier's home | **Out of CN-7** (§6): a physical-optimizer note, opened only after AX lands and AT-S13's C3 driver is re-run to price hot-row contention without the wait gap |
