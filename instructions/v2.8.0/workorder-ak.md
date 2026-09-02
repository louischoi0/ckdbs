# Work order AK — the auxiliary refusal matrix, and taking every refusal down that consistency lets go

Drafted 2026-09-02 by CLA on `worktree-resolving-refusal-window` at
`464b6a0` (`v2.7.0-113-g464b6a0`). **Every `path:line` below is
`464b6a0` unless a row says otherwise.** The series letter follows AJ.

## AK-1 — The direction, verbatim

> review refusal matrix for aux and I want to remove all refusal keeping
> data consistency and no breaks rule, functionality, concept.
> Investigate and plan the stages and go ahead for it.

Given as a self-paced loop: an iteration of one to two hours, a commit
and a `--no-verify` push at every finished unit of twenty to thirty
minutes. **This is operator input, not a CLA proposal.**

## AK-2 — Where this sits against AE

`ratification-ae.md` is the governing direction for `instructions/v2.8.0/`
and this order is filed *under* it, not against it. Two of AE's sentences
bound what "all" can mean here, and the operator's three conditions —
consistency, no broken rule, no broken concept — are what make them
binding rather than CLA's caution:

- **AE-2's seam.** A refusal that fires on a *split* relation is
  range-shaped and AE-5.1 says it stays: *"Every
  `RefuseAuxiliaryOnSplitRelation` arm and `RangeEligible` gate is
  strengthened by AE, not loosened."* Taking one down means building the
  auxiliary under a split, which is exactly the concept AE-3.4 withdrew
  (SA/SB's range-scoped Cabin claim, per-owner index builds, fan-out to
  child-*range* owners). That is a broken concept by AE's own definition,
  so the range-shaped rows below are **kept**, and AK-R0 asks the operator
  to confirm that reading rather than have CLA assume it.
- **AE-6's list.** A refusal that fires because two relations sit on two
  cores, or because a feature was never built, is *this version's work*:
  the five auxiliaries, complete, correct on the one-range shape, free to
  cross owners. Those rows are what this order takes down, and AE-6
  already names most of them (UNIQUE, heap parents, the cross-owner FK's
  remainder).

**The test every stage passes before its refusal goes** (AE-5.3's form,
applied to a lift): `cores = 1` byte-identical, the cross-owner suites
green, and a cell that *reaches the removed refusal's site and gets the
right answer* — because the one trade this engine never makes is a
refusal for a quiet wrong answer, and a refusal removed without a cell
behind it is that trade made on faith.

## AK-3 — The matrix

Every refusal on the five auxiliaries' paths — assertion, Cabin, foreign
key, secondary index, UNIQUE — as the tree stands. Columns: where it
fires, what shape (AE-2's axis), when a client meets it, and the verdict.
"Shipped configuration" means `range_size_ids` off (spreading off, the
default since 2026-08-31) and relations placed by namespace, so `cores ≥ 2`
puts relations on peer cores as a matter of course (AF).

### AK-3a — Range-shaped: kept under AE-5.1

Reachable only with `range_size_ids` set. AE-3.2 makes one range per
relation the design assumption; AE-5.1 keeps every one of these.

| # | refusal | site | verdict |
|---|---|---|---|
| R1 | `CREATE INDEX` on a relation of two or more ranges | `src/catalog/catalog.cpp:3332` (`RefuseAuxiliaryOnSplitRelation`, index arm) | **keep** — AE-5.1 |
| R2 | An FK naming a split relation on either side | `catalog.cpp:3162`, `:3165` (child and parent arms) | **keep** — AE-5.1 |
| R3 | `CREATE ASSERTION` on a split relation | `src/exec/assertion_catalog.cpp:208`, `:461` | **keep** — AE-5.1 |
| R4 | The reverse FK check on a split child this core cannot serve whole | `src/exec/fk_check.cpp:205` | **keep** — AE-5.2 names it as a guard proven by breaking it |
| R5 | `RangeEligible`'s four arms (`kBtree`, `kSpill`, `kForeignKey`, `kAssertion`) | `src/exec/range_eligible.cpp` | not an auxiliary refusal — they refuse the *split*; listed so the matrix is complete. **keep** |

The Cabin's range arm is already gone (SB, 2026-09-01), and it went by
scoping the authority claim rather than by relaxing the decline; that is
the precedent this order follows for every lift below.

### AK-3b — Owner-shaped: this version's work

Reachable in the shipped configuration at `cores ≥ 2`, the moment a
namespace places a relation on a peer core.

| # | refusal | site | why it exists | what removal is | verdict |
|---|---|---|---|---|---|
| O1 | **A relation with any Observational Cabin takes no writes on its owner** when the owner is a peer | `src/server/command_dispatcher.cpp:6302` (the funding gate's Cabin arm, keyed on `access.cabin_ids`) | The arm's own text names a *Bound* question ("whether a Bound Cabin's entry pages follow the grant"), but its predicate is the *Observational* class. The real hazard: **core 0 holds the engine's only `stats::CabinStore`** (`expeditor.cpp:1712`; `core_runtime.cpp:283` and the three `remote_step_service.cpp` stages pass `/*cabins=*/nullptr`), so a peer's write would run no `NoteWrite` and core 0's set would silently become a subset. Except core 0's set never serves a peer-owned relation either — its reads ship to the owner, which has no store — so today `CREATE CABIN` on a namespace-placed relation **serves nothing and blocks every write to it** | **A Cabin store per core.** The owner observes, appends and serves — the relation's one owner (AE-3.2), so the `cabin.md` §4b claim *(observed value × the ranges its core owns)* covers the whole relation by construction. The arm goes; so does the `funded_shape` tail at `:6318`, which has no arm left to guard. `FkProbeServer` on a peer gains F6's lookup as a side effect (`expeditor.cpp:1714` names the consequence). What stays core 0's is the **optimizer** (`cabin_optimizer_exec.cpp`, fed by core 0's signals) — a peer-owned relation's `CABIN AUTO` is a follow-on named in the stage, not a silent gap | **remove** — AK-S2 |
| O2 | A peer refuses writes to a relation under an assertion it knows and cannot enforce | `command_dispatcher.cpp:6309` (`CannotEnforce`) | A file written before PW1c-6c (2026-08-26) has the Bound Cabin core 0 built; the owner cannot append to core 0's pages. Fail-closed on a measured defect (Finding 2, `bench/v2.2.0/results-shipping-part-a-*`). Remedy today: `DROP` + `CREATE` | The owner **rebuilds at mount**: a registry that finds a core-0-built directory for a relation it owns runs the same owner-side build `CREATE ASSERTION` runs (`assertion_build_service`), adopts, and core 0's chain orphans as a dropped assertion's does | **remove, last** — AK-S10; reachability is a pre-2026-08-26 file with an assertion on a peer-owned relation |
| O3 | The funding gate's tail ("this relation's shape is not funded") | `command_dispatcher.cpp:6318` | The whitelist's tail, so a future secondary structure refuses rather than slips through | Unreachable once O1 and O2 go; removed with O1, and the whitelist's *comment* keeps the rule | **remove with O1** |
| O4 | `CREATE INDEX` on a peer-owned relation **inside an explicit transaction** | `command_dispatcher.cpp:2827` (`BeginForeignIndexBuild`) | The owner refuses the relation's writes retryably from the build request until `done` (`index_build_service.hpp`, "The refusal window"), because a row written meanwhile would be in nobody's index — the owner's catalog cannot see the uncommitted `sys.indexes` row (DT9 is core-local). Inside a transaction that window lasts until the client's `COMMIT` | **Maintain before publish.** The owner starts appending to the pending tree for every write *before* the backfill begins and keeps doing so until `done`; duplicates are IX1's superset and a missed row is impossible. `done(committed)` hands maintenance to the catalog, `done(rolled back)` drops the tree. The window closes for the autocommit arm too — `IndexBuildPending` stops being met | **remove** — AK-S4 |
| O5 | `DROP INDEX` on a peer-owned relation inside an explicit transaction | `command_dispatcher.cpp:~2845` | The mark broadcasts at once; the owner's DT9 cannot see the deleter in flight, so it stops maintaining before `COMMIT`, and a `ROLLBACK` restores an index missing the meanwhile-writes | The owner **maintains a delete-marked index until told it resolved**: core 0 sends `drop_pending(index_oid)` at the mark and `done(committed | rolled back)` at resolve, the `done` leg the CREATE path already has | **remove** — AK-S5 |
| O6 | `CREATE ASSERTION` on a peer-owned relation inside an explicit transaction | `command_dispatcher.cpp:3632` (`BeginForeignAssertionBuild`) | "Inside a transaction that row waits on the client's COMMIT." But assertions are **non-transactional DDL** (`ddl-transactional.md` §5, §5f): the local arm takes the statement inside a transaction and publishes at once, and §5f's own words are that the foreign arm is "isolated differently, and deliberately" — it refuses no writes | The foreign arm publishes as the local arm does: the park is the session's, the row is written the way the local arm writes it, and a `ROLLBACK` leaves it exactly as it leaves a locally-declared assertion. A divergence closed, not a design | **remove** — AK-S1 |
| O7 | A dispatcher with no index-build / assertion-build client | `command_dispatcher.cpp:2797`, `:3520` | A fixture without a ring; at `cores = 1` no relation is peer-owned | Not a product configuration | **keep** as the guard it is |
| O8 | `DELETE` of a parent with cross-owner children **by anything but `pk = k`** | `command_dispatcher.cpp:4616` (AJ-R2 (i)'s remainder; AJ-T6 "not sequenced") | The rows a `WHERE` selects are the walk's answer, and the walk cannot park; the fan-out needs the pks before it | **The collect pass** (AJ-T6): a first read-only walk under the statement's view collects the pks, the fan-out runs as the pk shape's does, the delete walk answers each row from the held verdicts and refuses `TxnConflict` (retryable) any row it meets that the collect pass did not — a row that became visible between the passes — rather than deleting it unchecked | **remove** — AK-S3; the operator's "all" is the benchmark AJ-T6 was waiting for |
| O9 | The reverse fan-out with no reactor or no statement text to resume | `command_dispatcher.cpp:4340` | The same class as O7 | — | **keep** as the guard it is |
| O10 | `CheckNoChildReferences`' owner arm | `src/exec/fk_check.cpp:196` | The backstop behind O8 and AJ-T3; after both it is reached only by a caller bug (`:4754`'s rule) | — | **keep** as a guard, retitled when O8 lands |
| O11 | A caller-supplied pk on a peer core | `command_dispatcher.cpp:7542` | Not an auxiliary — admitting one writes the relation's `sys.tables` row, core 0's page | Named because it is the largest write refusal a namespace-placed relation meets | **out of scope** unless the operator names it |

### AK-3c — Feature-shaped: reachable at `cores = 1`

| # | refusal | site | why it exists | what removal is | verdict |
|---|---|---|---|---|---|
| F1 | **`UNIQUE` index** (IX11) | `catalog.cpp:3320` | "v1 is a read accelerator that cannot fail a write for a reason of its own" (`index.md` §11). AE-6 names it as this version's subject, a single-relation problem at one range | Uniqueness enforced at the write: on `INSERT`/`UPDATE` of a key column the index is probed for the key, every entry resolved to its row and put through **check visibility** (`foreign-keys.md` §4's mode, not a second one) — a visible committed match is `kUniqueViolation` (terminal), an in-flight writer's match is `TxnConflict` (retryable, F3's one-code rule), a superseded entry is the superset the probe already subtracts. `CREATE UNIQUE INDEX` on a populated relation refuses on the first duplicate the backfill meets. The error reaches KWP as its own registry entry | **remove** — AK-S8; AK-R4 rules the code |
| F2 | **A heap parent** in a foreign key | `src/catalog/foreign_key.cpp:23` | `LocateByPk` answers `kScan` for a heap relation (`command_dispatcher.cpp:8291`), so every child write would walk the parent; "refusing keeps a constraint's cost a descent" | Admit, with the forward check as the `min_key`-bounded chain walk — the cost class `foreign-keys.md` §3 **already accepts** for the reverse check ("a full child walk per deleted parent until a Cabin covers the fk column") — and a `WARN` at `CREATE TABLE` naming the cost, AF-T4's precedent. IB1-5's batch resolution later bounds it. Reverses IB-R5's fence | **remove** — AK-S6, gated on AK-R2 |
| F3 | **An index on a heap relation** (IX3) | `catalog.cpp:3358` | A heap relation resolves an entry's pk by a chain scan | Work order IB, IB1-IB5 (`docs/inflight/in-progress/workplan-ib.md` §3 says where IB1 picks up); AE-6 keeps IB as a subject | **remove** — AK-S9, by IB's own plan |
| F4 | **A nullable index key or covered column** | `catalog.cpp:3394`, `:3414` | `null.md` D2, ratified 2026-08-20: every row has exactly one entry of a key that always exists; an entry has no NULL encoding | Key: a row whose key is NULL gets **no entry** — a probe `k = v` never needs it (three-valued: `NULL = v` is unknown, never true), `IS NULL` stays a scan, and IX1's superset rule holds as stated (the index is a superset of the rows *carrying that key*). Covered: a per-entry null bitmap sized to the covered *nullable* columns, present only on an index that declares one (a `sys.indexes` flag), so every existing index stays byte-identical — `null.md`'s own tail-bitmap shape. Reverses D2 | **remove** — AK-S7, gated on AK-R3 |
| F5 | Assertion `SUM` over a `uint64` column | `assertion_catalog.cpp:411`, `Unsupported` | The group header's accumulator is a checked `int64`; half of `uint64`'s range does not fit | Architecture, not absence — the two-code rule's `Unsupported` | **keep** by concept |
| F6 | An assertion declaration longer than `kMaxValueSize` | `assertion_catalog.cpp:189`, `:475` | A single stored value's ceiling | A limit, not a shape | **keep** |
| F7 | `REFERENCES parent(col)`; an FK on column 0 or on a non-integer column; a pk in an index key; `=` in an assertion (AS11) | `foreign_key.cpp:12`, `:17`; `catalog.cpp:3378`; `assertion.md` §3.1a | Each is `InvalidArgument` — "simply wrong" under the concept (F1 fixes the referenced column, the Keystone word is not a field, the clustered tree is the pk's index) | — | **keep** by concept |
| F8 | `CASCADE` / `SET NULL` | v1 non-goals | AE-6: "stay out as v1 non-goals unless the operator says otherwise" | — | **keep** unless the operator names them |

**Struck from the matrix, because they are not refusals against this
tree:** `kFkNullable` — a nullable FK column is admitted, and a NULL
passes the forward check vacuously and blocks no reverse check
(`foreign-keys.md` §2 "Semantics", `null.md` §4); and
`CheckForeignKeyColocation` — converted to a recommendation at AH-T4
and refusing nothing (`foreign_key.cpp:31`).

## AK-4 — Rulings

**AK-R0 `[DECISION]` — The range-shaped rows (AK-3a) stay.** CLA's
reading of the direction against AE-5.1 and the operator's "no breaks
concept". If the operator's "all" includes them, that is a reversal of
AE-5.1 and AE-3.4 and needs its own ratification, because it re-opens
SA's whole programme.

**AK-R1 — Accepted by default: the stage order of AK-5 and the mechanism
each row names.** Cheapest divergence first (O6), then the refusal a
namespace makes ordinary (O1), then the FK remainder (O8), then the two
DDL windows (O4, O5), then the three feature rows that need a ruling, then
the legacy-file repair (O2).

**AK-R2 `[DECISION]` — Heap parents (F2).** Admitting one reverses
IB-R5's fence (`instructions/v2.7.2/index.md`, "F1's heap-parent FK
refusal stays"). CLA's proposal: admit with the `WARN`, on the strength of
§3's reverse-check precedent; IB bounds the cost later. **CLA proceeds on
this at AK-S6 unless told otherwise**, because the direction's "all" is
the word and the reversal changes no format and invalidates no row.

**AK-R3 `[DECISION]` — Nullable index columns (F4).** Reverses `null.md`
D2, a ratified decision. CLA's proposal is the row's: no entry for a NULL
key, a flag-gated null bitmap for covered columns. **CLA proceeds at
AK-S7 unless told otherwise**, same reasoning.

**AK-R4 — Accepted by default: `UNIQUE`'s violation is its own terminal
code**, `kUniqueViolation`, wire-spelled and registered like
`kFkViolation`; the busy verdict stays `TxnConflict`, one code wide, per
FK-M2's argument in `status.hpp`.

## AK-5 — Stages

Each stage is one or more commit units; a unit ends with the suite green
(Debug, `scripts/test.sh`'s `ctest`) or with "not run" said plainly, a
`critics-developer` review per the Session Workflow, and a `--no-verify`
push, which the direction authorises and the reply names as skipped.

| stage | row | what lands | size |
|---|---|---|---|
| AK-S0 | — | This document; `index.md`'s row | done at the commit that carries it |
| AK-S1 | O6 | The foreign `CREATE ASSERTION` arm publishes inside a transaction as the local arm does. Cell: the statement inside `BEGIN`, on a namespace-placed relation, enforcing before `COMMIT` and after `ROLLBACK` | S |
| AK-S2 | O1, O3 | A `CabinStore` per core: `CoreRuntime` builds one, the three fan-in stages take the core's, the funding gate's Cabin arm and tail go. Cells: a namespace-placed relation with a Cabin takes writes on its owner; the owner's store observes a shipped read and serves the next one (`ANALYZE`'s `cabin_hits`); `SHOW CABIN` from a peer session; `cores = 1` byte-identical. Follow-on named: the optimizer's signals for a peer-owned relation | M-L |
| AK-S3 | O8 | AJ-T6, the collect pass. Cells: `DELETE … WHERE <non-pk>` on a cross-owner parent, zero / one / many rows, a child on a second owner, a row that appears between the passes → `TXN_CONFLICT retryable=1`, never deleted unchecked | M |
| AK-S4 | O4 | Maintain-before-publish on the owner's index build. Cells: a write during the backfill lands in the index; `CREATE INDEX` inside `BEGIN` on a peer-owned relation, then `COMMIT` and then `ROLLBACK`; `IndexBuildPending` no longer met | M-L |
| AK-S5 | O5 | The pending-drop maintenance on the owner. Cells: `DROP INDEX` inside `BEGIN`, a write meanwhile, `ROLLBACK` → the index has the row | M |
| AK-S6 | F2 | Heap parents admitted (AK-R2). Cells: the forward check against a heap parent, present / absent / in-flight-deleted; the `WARN` | S-M |
| AK-S7 | F4 | Nullable index keys and covered columns (AK-R3). Cells: the contract suite's byte-identity on every existing index; a NULL key row absent from the probe and present in the scan; a covered NULL resolving as NULL | M |
| AK-S8 | F1 | `UNIQUE` (AK-R4). Cells: violation, busy-then-retry, rollback of the conflicting writer, `CREATE UNIQUE INDEX` on a relation with a duplicate, KWP's code | L |
| AK-S9 | F3 | IB1-IB5 | L, by `workplan-ib.md` |
| AK-S10 | O2 | The owner's rebuild at mount | M |

Every stage's refusal message, until its stage lands, keeps naming the
remedy a client can act on today (AF-T4's rule: the last clause is the
actionable one).

## Row status (CLA, appended as rows land)

| row | status |
|---|---|
| AK-S0 | **Written 2026-09-02** on `worktree-resolving-refusal-window` at `464b6a0`; the matrix is source-read at that commit |
| AK-S1 | **Built 2026-09-02** on `worktree-resolving-refusal-window`. `BeginForeignAssertionBuild`'s explicit-transaction refusal is gone and its `session` parameter with it (`HandleAssertion` loses the same parameter — nothing in either arm reads a session, which is the whole finding: a non-transactional DDL has nothing to ask one). The refusal's stated ground — "the row waits on the client's COMMIT" — was never true: `exec::InsertAssertion` takes no transaction. Cell: `AForeignAssertionBuildsInsideAnExplicitTransactionLikeTheLocalArm` — parks inside `BEGIN`, `built_by_core=1`, the owner enforces **before** `COMMIT` (a violating shipped write answers `ASSERTION_VIOLATION`), the session's transaction survives the park, and after `ROLLBACK` the transaction's own row is gone and the assertion is listed and still enforcing. `ddl-transactional.md` §5f amended. **Review** (`critics-developer`): sound, verified at four points (`kBootstrapXid` in, no view filter out, no undo hook, `SyncAll` for durability; the park reads no session state; `CloseClient` defers teardown under an in-flight statement). Two consequences it named, both taken as text rather than code: a transaction that already wrote the relation meets the build's in-flight refusal and cannot succeed by retrying (§5f says so now; the terminal-izing alternative was declined as a second refusal site for one sentence's worth of fact), and the target is resolved unfiltered on both arms — pre-existing, three other ways in, now a `known-gaps.md` entry naming the index arm's `ViewFor(session)` as the fix and a later stage as its home. Five cuts applied: the comment collapsed to four lines, the header's duplicate sentence dropped, the test's preamble cut to what it pins, §5f's spliced paragraph moved so the sentence it broke reads again, and the file's line 11 no longer says §5f refuses. Two test findings applied: the `ROLLBACK` cell now has a row to undo (a core-0-owned relation's), and the transaction's *own* enrolled write to the target is exercised — the one path the lift newly reaches |
| AK-S2 | **Built 2026-09-02** on `worktree-resolving-refusal-window`. A `stats::CabinStore` per core: `CoreRuntime::Config` carries `cabins`/`cabin_limits` (copied from `Expeditor::Config` through one `CabinLimitsOf()`), `CoreRuntime` builds the store ahead of every borrower and hands it to its dispatcher, its `RemoteStepServer` (new ctor parameter, the three stage sites) and its `FkProbeServer`; core 0's step server takes core 0's. `CheckWriteAffinity` loses the Cabin arm and the "not funded" tail — `CannotEnforce` is the one arm left. **What the survey found under the arm**: its message asked a *Bound* question of the *Observational* class, and its real ground — no store to append to — was also why core 0's set never served a peer-owned relation, so `CREATE CABIN` on a namespace-placed relation served nothing and blocked every write to it. Cells: `ACabinedPeerRelationTakesWritesAndItsOwnerServesTheCabin` (admitted; the owner observes, serves, appends after observation, serves both rows; `ANALYZE`'s `cabin_hits`; `SHOW CABINS` truthful on both cores) and `AShippedRefusalCrossesTheRingByteIdenticalAndTerminal` (A5's property on the caller-supplied-pk refusal, which stays — O11). `FundPeerForRelation` now grants the var-heap head, which the rights probe asks for. **Review** (`critics-developer`): data path sound — no owner-side write bypasses `NoteCabinWrite`, no other core can both observe and serve, every store access is on its reactor, §6a is judged by the owner's own manager, `cores = 1` untouched. Five findings applied: `SHOW CABINS` on a peer printed zeros for Cabins it does not hold (now the dash arm naming the owner, pinned by the cell); `SHOW CABIN_OPTIMIZER`'s "(cabins = off)" reason was false on a peer; the store was declared below `remote_steps_`, its borrower (moved above it); `CabinScopeCovers`' comment called itself dead code (it is the live rule now); `expeditor.cpp`'s grant-handler comment still said one store is every store, and `range_alloc.cpp`'s argument over-claimed the scope rule — only "no range opens under v2.8.0" carries it, because the owner declines on `access.ranges`, which its cache learns from the broadcast. Seven cuts applied (the gate comment to six lines, the six-site argument to one home plus citations, `CabinLimitsOf()`, `CountOccurrences`, the FK cell's stale block comment, the split workplan's stale premise). Left as text: dropped sets stay in the owner's store until restart (`known-gaps.md`); a peer-owned relation earns no `CABIN AUTO` (the optimizer's signals are core 0's); a Cabin on a column past 64 is pre-existing write-side asymmetry. Suite: see the commit |
| AK-S3..S10 | not started |
