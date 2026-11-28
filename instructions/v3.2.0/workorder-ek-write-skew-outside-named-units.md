# Work order EK - the write skew that named units do not close: what `REPEATABLE READ` still admits, and whether the answer is a declaration, a lock or a level

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`service-txn-orders` (branch `worktree-service-txn-orders`) from
`72580aa04` (`v3.5.1-6-g72580aa04`) on the operator's word of 2026-11-28
(`raft-marks-2026-11-28.md` §6, §7). **Exploratory, and it re-reads two
operator rulings** (AR0 D1 and D8, §1.1): EK-S0 is a census with one
demonstration cell, and no candidate of §2 is built before the operator's
word on its verdict. No stage is started. Every `[source-read]` claim with
a `path:line` was read by CLA at `72580aa04` in the session that wrote
this order, and re-read by the `critics-developer` pass of the same day
(§7).

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/txn.md` §1, §5, §9; `docs/spec/assertion.md` §4, §6,
§10; `docs/spec/foreign-keys.md` §2a-§2c; `docs/spec/index.md` §10.2;
`docs/spec/parser-v2.md` I11a;
`instructions/v3.0.0/ar0-architecture-revision.md` (D1, D8);
`instructions/v3.0.0/raft-marks-2026-09-28.md` §4, §5;
`instructions/v3.2.0/workorder-cp-lock-model-exploration.md` (CP);
`tests/bs/history.hpp`; EI, EJ and EL, written the same day.

## 0. What EK is

`[operator]` The operator's premise of 2026-11-28 and the word that asked
for this order are EI's §0. C3 is the survey's third constraint: **no
`SERIALIZABLE`; `REPEATABLE READ` is a snapshot with first-updater-wins,
so a write skew is admitted.**

`[source-read]` **"Build `SERIALIZABLE`" is not what the tree leaves
open.** The level is "out of scope and is not `[OPEN]`"
(`docs/spec/txn.md:213-219`), `txn.md` §9 repeats it
(`docs/spec/txn.md:1436-1440`), and the operator ruled the matter twice on
2026-09-28 (§1.1): two levels, and write skew "closed by named units, not
gap locks". EK does not assume either ruling moved.

`[design]` What the premise does add is a kind of writer the rulings were
not written against: services that each check a condition by reading and
then write a different row, in code the database's owner does not review.
**EK asks which of those shapes the named units leave open today, shows
one of them on the engine, and puts before the operator the three kinds
of answer** - widen what can be declared, add a lock a statement can ask
for, or add a level - with what each costs. Its null hypothesis is the
standing ruling: no level.

## 1. The tree as read `[source-read]`

### 1.1 The rulings EK re-reads

- **D1**: AR0 proposed "(b) SI + explicit gap/predicate locks on
  assertion and FK paths" over "(c) full serializable (SSI or S2PL)",
  because "(c) taxes every read for a guarantee only two paths need"
  (`instructions/v3.0.0/ar0-architecture-revision.md:82-84`). The
  operator's word: "this project only propose 2 levels (RR, RC)", as typed
  (`instructions/v3.0.0/raft-marks-2026-09-28.md:60-68`). **The mark's
  "Does not address" row**: D1's option (b) itself, which CLA read as
  standing with its premise narrowed to the two levels (`:67`).
- **D8**: "write skew is closed by named units, not gap locks - the
  assertion group via registre reservation, the FK parent row via D9(a)'s
  held S. GROUP BY is the declaration"
  (`instructions/v3.0.0/raft-marks-2026-09-28.md:70-78`; as typed but for
  the apostrophe, a backtick in the source). The same mark records that
  AR0-M3's item 3, "a gap needs a structure to name it in", falls away
  with gap locks (`:76`).

### 1.2 What closes a write skew today

- **An assertion's group**, for one shape only: "`COUNT(*)` and
  `SUM(int64 col)` upper bounds only", one relation, no `WHERE`-scoped
  assertion (`docs/spec/assertion.md:912-921`). A `DELETE` and a
  decreasing `UPDATE` are not checked, because they cannot violate an
  upper bound; a move between groups is an arrival, checked at its
  destination (`docs/spec/assertion.md:158-166`). The check is "stricter
  than snapshot visibility" and "can never produce a false admission",
  counting committed rows, reservations and holds
  (`docs/spec/assertion.md:172-181`).
- **A foreign key's parent row**, by the child writer's `S` held to its
  decide (`CLAUDE.md`, Foreign keys; `docs/spec/foreign-keys.md:234-240`).
- **A unique index's key**, by the `kIndexKey` unit
  (`docs/spec/txn.md:830-834`).
- **A row or a tuple key window a statement names**, by `SELECT ... FOR
  UPDATE`: `U` on each row, "or, where the `WHERE` bounds the tuple key,
  the window declared in `U` before the walk", and "rows are claimed that
  are not returned: every key of a declared window"
  (`docs/spec/txn.md:1107-1111`, `:1145-1146`). So an absent key inside a
  tuple key window **is** claimed, against inserts as well; over any other
  predicate only the rows returned are. Every other strength is refused by
  name (`docs/spec/parser-v2.md:135`), and a `U` excludes other claimers,
  so two readers of one row serialize where they need only exclude its
  writer.
- **A key window a write declares**, the same unit asked by an `UPDATE`
  or a `DELETE` (`docs/spec/txn.md:967-972`). No plain read declares
  anything finer than its relation's `IS`, for the statement and not the
  transaction (`docs/spec/txn.md:1169-1186`).

### 1.3 What the suite checks

- The history checker infers Adya's graph over list-append histories and
  refuses G0, G1a-c, G-single and G-nonadjacent under `REPEATABLE READ`;
  "G2 with adjacent rw edges - write skew - both levels allow (no
  SERIALIZABLE), so it is never reported" (`tests/bs/history.hpp:28-36`).
  **No cell counts how often it happens, or pins a shape of it.**

## 2. What is left open, and the candidates `[design]`

**The shapes named units do not close** - each to be confirmed or struck
by S0, none run yet:

1. **A lower bound.** "At least one row of the group stays in state X":
   two transactions each read two such rows and each remove a different
   one. The assertion surface has no lower bound, and a departure is not
   checked (§1.2). This is the textbook write skew. **One recipe closes
   part of it today**: where the population is fixed, an upper bound on
   the complementary group ("at most n - 1 rows in state not-X") is
   checked at the move's destination. Its limit is that one literal bound
   applies to every group of the `GROUP BY`. S0 runs it.
2. **A condition over two relations**, or over a filtered group: outside
   one relation's `GROUP BY` (§1.2).
3. **A check on absence over a predicate that is not a tuple key
   window**: "no row matches, so insert one", where no unique index names
   the condition. Inside a tuple key window `FOR UPDATE` already claims
   the absent keys (§1.2).
4. **Read-then-write on rows the writer does not update**, where the
   client did not ask `FOR UPDATE`, or asked and paid exclusion among
   readers it did not need.

**The candidates**, none designed past what S0 needs to judge it:

- **K0 - nothing built.** The manual gains a section naming the four
  shapes and the recipe for each (`FOR UPDATE` on a row or a key window
  both writers read; a row that materializes the condition; an assertion
  where the bound is an upper one, the complementary group included). The
  standing ruling as it is.
- **K1 - widen the declaration.** A lower bound on an assertion's group:
  a departure reserves against the floor as an arrival reserves against
  the ceiling. It is D8's own mechanism turned around, closes shape 1 for
  every writer whether or not its author thought of it, and leaves 2 to 4.
- **K2 - a share-strength locking read.** `FOR SHARE`: the unit `FOR
  UPDATE` takes - a row, or a tuple key window - asked in `S`, the mode a
  foreign key's check already takes on a parent row, held to the decide.
  Closes shape 4 without excluding readers from each other. It adds a
  strength, not a unit: shape 3 stays as open as it is today.
- **K3 - a level.** `SERIALIZABLE` by tracking what each transaction read
  and refusing, at commit, the transaction that closes a dangerous
  structure (SSI), or by holding K2's locks for every read (S2PL), which
  still names no gap outside a tuple key window. It is D1's option (c),
  which the operator did not take; SSI is the only candidate that closes
  all four shapes without the writer's cooperation, and the only one that
  taxes every read.

## 3. Rules

- **EK-R1. No spelling before its guarantee** `[quiet-wrong]`: `ISOLATION
  LEVEL SERIALIZABLE` stays refused by name until a build forbids G2 in
  the history checker and the sim's corpus passes under it. A level
  accepted and enforced as snapshot isolation is the wrong answer this
  order exists to prevent.
- **EK-R2. A plain read keeps its contract**: under `READ COMMITTED` and
  `REPEATABLE READ` a `SELECT` without a locking clause is never refused
  and never waits for a borrow (`txn.md` §5), whatever is built.
- **EK-R3. A new claim says what it covers** `[quiet-wrong]`: a locking
  clause EK adds is documented, and tested, as claiming the rows returned
  and every key of a declared tuple key window and nothing else - what
  `FOR UPDATE` claims today. No spelling EK adds implies a claim on rows
  that do not exist outside such a window.
- **EK-R4. A lock a read asks follows the mode rule of `txn.md` §5**: a
  claim that must exclude a row's deletion or re-keying is not placed
  under a mode that admits one.
- **EK-R5. An assertion never admits falsely, in either direction**
  `[quiet-wrong]`: an upper bound counts committed rows and every reserved
  arrival, as now (`assertion.md` §4.3). A lower bound, if built, counts
  committed rows **less every reserved departure, and no uncommitted
  arrival**: an arrival counted toward a floor admits a departure that
  becomes a violation when the arrival rolls back.
- **EK-R6. D1 and D8 stand until the operator re-marks them in words**:
  S0's verdict is a proposal, and a green suite moves neither.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EK-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EK-Q0 | Is EK opened, and how far? | (a) S0 only; (b) S0 and one proof of concept named by EK-Q3; (c) not opened: K0's manual section written and the order abandoned | (a) |
| EK-Q1 | Does EK re-open D1's level | (a) no: K3 is laid out in S0 with its cost and built by no stage of this order; a level is a new order on the operator's word; (b) yes, K3 is a candidate S1 may build | (a) |
| EK-Q2 | The history checker and G2 | (a) it **counts** G2 with adjacent rw edges per level and reports the count, failing nothing, so the sim shows how often the corpus's shapes skew; (b) left as it is | (a) |
| EK-Q3 | The first proof of concept, if one is opened | (a) K1, the lower bound: it is the engine's stated purpose - a declared invariant enforced on every write - applied to the textbook shape; (b) K2 | (a) |
| EK-Q4 | `FOR SHARE` (K2) against `parser-v2.md` I11a's one clause | (a) not opened by EK unless S0 shows shape 4 costing a workload its readers' exclusion, measured; (b) opened | (a) |
| EK-Q5 | The manual's section (K0) | (a) written at EK's close whatever else is built: the shapes S0 confirms are true of the engine today; (b) only if nothing is built | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EK-S0** | The census: each shape of §2 written as two transactions over a named schema, and for each, on the two-core rig, a cell that runs it at `REPEATABLE READ` and at `READ COMMITTED` and records whether both commit - shape 1 first, as the demonstration, with and without the complementary-group recipe; the units of §1.2 read against each shape with a `path:line`; the checker's G2 count (EK-Q2) over the sim's corpus; K1 to K3 each laid out with the rule it must keep and the structure it needs | the four shapes confirmed or struck, `[measured]`; a verdict among K0 to K3 as a proposal; the operator's word |
| **EK-S1** | (gated on EK-Q0 and EK-Q3) One proof of concept, in the engine, behind its own refusal cells: S0's demonstration cell for its shape turns from "both commit" to "one refused or waiting", and for K1 a cell in which an arrival that rolls back does not leave a departure admitted (EK-R5) | the suite, the sim corpus, the contract suites; what the proof did not carry |
| **EK-S2** | The close: `txn.md` §1 and §9 state what was decided; `assertion.md` or `parser-v2.md` where a surface moved; the manual's section (EK-Q5); `known-gaps.md`; the overhead measurement if S1 built anything | the operator's word on landing |

## 6. What EK does not do

- No `SERIALIZABLE` spelling, and no third level, without EK-Q1 (b).
- No gap lock on a secondary index, and no predicate lock over a
  non-key condition: D8's mark is "no gap locks".
- No deferred constraint and no trigger.
- No change to first-updater-wins, to `READ COMMITTED`'s re-run rules or
  to what a plain read borrows.
- No `SKIP LOCKED` or `NOWAIT` (EJ-Q4 names the first as outside EJ too).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `service-txn-orders` from `72580aa04`, on the operator's word. No stage
is opened and no code is written. §2's four shapes are `[design]`: each
follows from §1.2's reading and none has been run on the engine.

**The opening review** (`critics-developer`, the same day, at `5d8dd1ddf`)
found the first writing wrong in three places, each corrected above: it
said `FOR UPDATE` claims no absent row, where a tuple key window's absent
keys are claimed (`txn.md:1107-1111`, `:1145-1146`), so shape 3 is
narrowed and a fifth candidate, a declared read range, is folded into K2
as already built but for its strength; EK-R5 said a lower bound is checked
"as the upper one is", which would admit falsely; and a sentence of
AR0-M3's was attributed to D8's mark. It added shape 1's partial recipe.
Rejected: writing D8's quotation with the source's stray backtick, which
would break the line's Markdown; the difference is stated beside the
quotation instead.
