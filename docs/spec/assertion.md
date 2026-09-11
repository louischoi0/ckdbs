# ASSERTION — Group-Level Declarative Constraints

Status: **ADOPTED (v1 scope), built and enforcing on every core**
Related documents: `docs/spec/cabin.md` (§12 is the Bound Cabin class split this spec requires), `docs/spec/wal.md`, `docs/spec/txn.md`, `docs/spec/foreign-keys.md`, `docs/spec/crosscore.md` (CC7, whose owner-builds exception assertions left at AT-S5d). The U5 durability tier §5 cites is a design reference with no owning doc — v1 has no unique index (`docs/spec/index.md` IX11). ANALYZE's surface is `manual/sql/sql.md` §4.

---

## 1. Positioning

`CREATE ASSERTION` was standardized in SQL-92 as a schema-level, declarative
constraint over arbitrary database state. For over thirty years no major DBMS
has shipped it. The two blocking problems are well understood:

1. **Re-evaluation cost.** A naive implementation re-evaluates the full
   assertion predicate on every write. General predicates admit no tractable
   incremental-checking analysis.
2. **Concurrency.** Two transactions may each observe a satisfying state and
   commit writes that jointly violate the predicate. Preventing this
   classically requires predicate locking or serializable isolation, which
   introduces waiting and deadlock — unacceptable for OLTP.

KDS resolves both problems by construction rather than by generality:

- The supported predicate class is **deliberately restricted** (AS1) to group
  cardinality and group sum upper bounds, for which exact incremental state is
  cheap to maintain.
- Incremental state lives in a **Bound Cabin** (AS5/AS6): a pinned,
  full-coverage, logged authority-class variant of the existing Cabin
  structure. Checks are O(1) against a per-group running aggregate.
- Concurrency is handled by a **reservation protocol over one registry for
  the instance** (AS4, §6.1). It ran on the relation's owner core, latch-free
  inside that core's event loop, until AT-S5 made a write run where its
  session is; since AT-S5d every core checks and reserves into the one
  registry, under its latch, and an admission **holds** what it admitted so
  two cores cannot both fit a group that has room for one. **The admission
  is atomic; its refusal need not be final** — since
  AO-S6e-c a rejection caused by a reservation whose transaction is still
  in flight **waits** for that decide instead of failing (§6.2), on the
  lock family's own channel, so it is in the wait-for graph and a cycle of
  two reservations is refused naming deadlock. No retry storm and no
  livelock; the failure, when it comes, is deterministic.

Everything outside the supported class is a truthful refusal, in line with
the engine-wide contract: fewer features, exactly specified, fast and
correct.

---

## 2. Decision Record

| ID | Decision |
|----|----------|
| AS1 | v1 predicate class: group cardinality (`COUNT(*)`) and group sum (`SUM(col)`) constraints over a single `GROUP BY` column list. General `NOT EXISTS` / subquery predicates: refused. |
| AS2 | KDS-restricted syntax (`CREATE ASSERTION ... ON rel GROUP BY (...) CHECK ...`), not the SQL-92 free-form `CHECK (search condition)`. The grammar itself encodes the supported class; create-time validation is maximized. |
| AS3 | Statement-time checking only (fail-fast). `DEFERRABLE` is reserved in the grammar and rejected as `Unsupported`. |
| AS4 | Reservation protocol combined with owner-core group-key serialization. **"No latches" is struck at AT-S5d**: the registry is the instance's and every core reserves into it, so its directory is serialized by a latch of its own (§6.1). **"No waiting, no deadlock" is struck** (`ar0-architecture-revision.md:416-419` struck AS4 with D8; AO-S6e-c built the wait): a false rejection waits for the reserver's decide, and two transactions each holding a reservation the other needs are a real cycle the detector ends. |
| AS5 | No separate counter store. The Bound Cabin is the single structure: entries plus a per-group running aggregate maintained in the group directory header. Checks are computed against the Cabin in real time on the write path. |
| AS6 | Bound Cabin is a **logged, headered authority class** (same durability tier as the var-heap (V3) and unique indexes (U5)). Prerequisite: the Cabin class split defined in §5. |
| AS6a | Where assertion replay starts: a **per-checkpoint snapshot of the group headers** (`{group_id, key, count, sum}`), folded forward with `ASSERT_*` records **from the last checkpoint** — never from the cabin's birth, which would make RTO a function of the assertion's lifetime and make WAL retention a correctness setting. Every entry carries its `group_id` (§5.1) so the header→entry linkage is rebuilt from the cabin's own pages instead of persisted. Narrows AS5's "not a separate store" to "not a separate authority". Full statement: §7. |
| AS6b | An aborted entry is distinguishable on the page: `flags` bit 3, `kEntryOrphaned`, set when its reservation aborts; the linkage scan skips it. Full statement: §7. |
| AS7 | `CREATE ASSERTION` performs a full scan of the target relation to build the Bound Cabin and initial aggregates. Any existing violation fails the CREATE and discards the build. `NOT VALID` is reserved grammar, `Unsupported`. |
| AS8 | v1 assertions target exactly one relation. Multi-relation assertions: `Unsupported`. |
| AS9 | A violation is a **statement error** that poisons like every other write failure (§4.4). New Status code `AssertionViolation`; the error carries the assertion name and the violating group key. |
| AS10 | Catalog: `sys.assertions` storing the full declaration `source_text` (same model as `sys.pattern_defs`). `DROP ASSERTION` supported. Dropping a relation referenced by an assertion is `RESTRICT`. ANALYZE reports per-statement assertion check counts and reservation failures. |
| AS11 | v1 supports **upper-bound constraints only**: comparison operators `<` and `<=`. Lower bounds (`>`, `>=`) are refused — they would require checking on DELETE and on decreasing UPDATE paths. **`=` is refused with them** (§3.1a): enforcing real equality needs the lower-bound half, so `=` costs exactly what `>=` costs. Consequently DELETE never requires an assertion check. |

**Which refusal code** (`include/kds/base/status.hpp`): **`NotImplemented`**
for AS11's lower bounds and `=` ("v1 excludes", not "cannot"), `MIN`/`MAX`/`AVG`
bounds, `COUNT(<column>)`, `COUNT(DISTINCT ...)`, and a declaration longer than
one var-heap value can hold. **`Unsupported`** for AS3's and AS7's timing
clauses — an assertion is checked at statement time, always, so `DEFERRABLE`
and `NOT VALID` name a mechanism this design does not have rather than one it
has not built — and for `SUM` over `uint64`, at AG3 parity.

---

## 3. Syntax

```sql
CREATE ASSERTION <name>
  ON <relation>
  GROUP BY ( <column> [, <column> ...] )
  CHECK COUNT(*)      <op> <int_literal>
      | SUM(<column>) <op> <int_literal> ;      -- <op> is < or <=

DROP ASSERTION <name> ;
```

- `<op>` ∈ { `<`, `<=` } (AS11). `>`, `>=` and `=` parse but return
  `NotImplemented`.
- `DEFERRABLE` / `NOT DEFERRABLE` and `NOT VALID` are reserved tokens: they
  parse and return `Unsupported` (AS3, AS7).
- Assertion names live in the same namespace as other schema objects and must
  be unique.

### 3.1 Create-time validation (maximized)

`CREATE ASSERTION` fails immediately (before any scan) when:

- the target relation does not exist;
- any `GROUP BY` column does not exist in the relation;
- the `SUM` column does not exist or is not `int64` (v1 restriction; checked
  arithmetic per AG3 — an overflow during aggregate maintenance is a
  statement error, never silent wraparound);
- the comparison operator is outside the v1 set;
- the bound literal is not a non-negative integer literal (v1: literals only,
  no expressions, consistent with TY3 conservatism);
- a duplicate assertion name exists;
- semantically degenerate forms: `CHECK COUNT(*) <= 0` and `CHECK COUNT(*) <
  1` can never admit a row and are rejected at create time. A group exists
  only because it holds at least one row, so any ceiling below 1 declares a
  relation that may never be written to again. The same argument deliberately
  does **not** extend to `SUM`, whose column may hold negative values, so no
  non-negative bound is provably unsatisfiable.

### 3.1a Why `=` is refused (AS11)

Accepting `=` as meaning `aggregate <= N` would have the engine enforce
something other than what the operator wrote: a client reading
`CHECK COUNT(*) = 5` would reasonably expect a group of three rows to be a
violation. A constraint that quietly means less than it says is worse than
one that is refused, because the refusal is visible at `CREATE` and the
reinterpretation is visible nowhere.

Enforcing the operator as written is not a cheaper option either. True
equality implies a **lower** bound, which is checked on DELETE and on every
decreasing UPDATE — exactly the write-path expansion AS11 exists to exclude.
So `=` costs what `>=` costs, and is refused beside it. Both remaining
operators map onto a ceiling exactly, reinterpreting nothing:

```
CHECK COUNT(*) <= 5   ->  count <= 5
CHECK COUNT(*) <  5   ->  count <= 4
```

### 3.2 Example

```sql
CREATE ASSERTION user_product_purchase_limit
  ON purchases
  GROUP BY (user_id, product_id)
  CHECK COUNT(*) <= 5;
```

---

## 4. Semantics

### 4.1 Enforced invariant

For every group `g` (a distinct tuple of values over the `GROUP BY` columns)
in the target relation, the aggregate of **committed and reserved** rows in
`g` never exceeds the declared bound. Reserved rows are those written by
in-flight statements that have passed admission (§6).

### 4.2 Checked write paths (v1, upper-bound only)

| Write | Check required | Notes |
|-------|----------------|-------|
| INSERT | Yes | Increases COUNT by 1 / SUM by the inserted value. |
| UPDATE, group columns unchanged, SUM column unchanged | No | Aggregate is invariant. |
| UPDATE, SUM column changed (group unchanged) | Only if the delta is positive | Negative delta cannot violate an upper bound. |
| UPDATE, group columns changed | Yes, on the **destination** group only | Modeled as departure (no check) + arrival (checked). The arrival is held from its check until its record, so no other core's admission sees the group between the two mutations with room it does not have (§6.2). |
| DELETE | No | Strictly decreasing; cannot violate an upper bound (AS11). |

The check runs inside the writing statement, the same way FK checks do —
no trigger machinery. Statement classes are unaffected; pattern
fingerprints are unaffected.

### 4.3 Timing and isolation

Checks execute at statement time against the group's current authoritative
aggregate (committed + reservations + holds) in the instance's registry. This is intentionally
**stricter than snapshot visibility**: a statement may be rejected because of
a concurrent uncommitted reservation. This is the correct trade for an
upper-bound admission constraint — it can produce false rejections only in
races where at most one of the contenders could have succeeded anyway, and it
can never produce a false admission.

### 4.4 Error semantics (AS9)

A violation is a statement error with Status `AssertionViolation`, message
including the assertion name and the rendered group key:

```
AssertionViolation: assertion "user_product_purchase_limit"
  group (user_id=41, product_id=7): COUNT(*) would exceed bound 5
```

**The violation poisons, like every other write failure.** Inside an
explicit transaction the session enters failed-txn and the client must
`ROLLBACK` — uniform with `FK_VIOLATION` and per-transaction failure
atomicity (`docs/spec/txn.md` §6), and the only honest option once a
multi-row UPDATE can violate on row 3 of 10 with rows 1-2 already written;
"open and usable" would need statement-level rollback the engine does not
have. In autocommit the statement *is* its transaction and unwinds fully,
reservations included — which is the sense in which a violation is a
statement error. A refusal itself mutates nothing (§6.2 step 2).

---

## 5. Bound Cabin (Cabin class split)

This section is normative for `docs/spec/cabin.md` §12.

The Cabin structure splits into two classes with a shared page format and
shared lookup machinery but different lifecycle contracts:

| Property | Observational Cabin | **Bound Cabin** |
|---|---|---|
| Population | Lazy — observed values only | Eager — full coverage of the group-column combination, built at CREATE |
| Eviction | Allowed | **Forbidden (pinned)** |
| Coverage contract | Partial by design | 100% of live rows of the target relation |
| Durability | Non-authoritative; entries discardable; dangling entries dropped on read | **Logged, headered authority class** (V3/U5 tier); WAL-before-data; crash-consistent |
| Role | Advisory acceleration (hints) | Authoritative constraint substrate |
| Entry size | 24 B | **32 B** (adds inline aggregate value) |

### 5.1 Entry layout (32 B, fixed)

| Field | Width | Notes |
|---|---|---|
| pk | 40 bit | Keystone id, authoritative (K1 invariants: never reused, never changed) |
| flags | 8 bit | includes `RESERVED` for in-flight entries (§6) and `ORPHANED` (bit 3, `kEntryOrphaned`) for an entry whose reservation aborted (AS6b, §7) |
| reserved | 16 bit | alignment |
| location hint: page id / epoch / slot | 64 bit | advisory; shares Waystone validation rules; on hint failure fall back to pk descent and heal in place |
| aggregate value | 64 bit | the row's `SUM` column value, inline (int64). For COUNT-only assertions this field is written as 1. |
| `group_id` | 32 bit | **AS6a.** Which group of this cabin the entry belongs to. Authoritative, not advisory: it is what lets recovery rebuild the header→entry linkage by scanning the cabin's own pages. |
| padding | — | to 32 B |

The normative facts are: fixed 32 B, pk authoritative, hint advisory, value
inline, `group_id` authoritative. `group_id` is carried by the entry, by
`AssertEntryPayload`, and by the in-memory group header, and all three
writing sites stamp it. The replay fold **adopts** the id a record carries
rather than assigning one, because a fold starting from a checkpoint meets
groups in record order and an id assigned in that order would drift from
the ids the entries already on the pages carry — misattributing them at the
next recovery.

**An id and not a group-key hash**, and the difference is correctness
rather than taste: `HashGroupKey` is a mixing function whose collisions are
expected and are resolved by confirming the stored key (§5.2), and an entry
carries no key — so an entry holding only a hash could not be attributed
between two colliding groups. An id makes attribution exact and removes the
collision question from the recovery path.

Ids are **dense per cabin**, assigned at group creation, never reused while
the cabin lives; `DROP ASSERTION` releases the whole space with the cabin
(§8.3).

### 5.2 Group directory and running aggregate

The Bound Cabin group directory maps `group_key_hash → group header`. Each
group header maintains:

- `count` (int64) — committed + reserved cardinality;
- `sum` (int64, checked) — committed + reserved sum (SUM assertions);
- entry-list linkage into headered Bound Cabin pages.

Admission checks read only the group header: **O(1)**, no entry iteration.
Entries exist for violation diagnostics and repair/verification
(re-summation); they are not on the check hot path.

The running aggregate is not a separate **authority** (AS5): it is a field of
the Cabin group header, recovered by WAL replay and verifiable against the
entry list. AS6a narrows AS5's original wording — the directory does acquire a
durable form, a per-checkpoint snapshot — but the narrowing is only of the
word "store": the entries remain the authority, the snapshot is a derived
cache, and `VerifyAgainstEntries` is what proves one against the other.

### 5.3 One Bound Cabin per assertion

Exactly one Bound Cabin instance is bound to each assertion; two assertions
with identical group-column lists have two.

---

## 6. Concurrency: one registry, reserved into from every core (AS4)

### 6.1 Ownership

**All Bound Cabin state lives in one registry for the instance** (AT-S5d,
`workorder-at-m3-uniformity.md` AT-R15): every assertion's group directory,
the admissions' holds and each transaction's pending reservations.
`Expeditor` owns it and hands it to every core, `LockTable`'s idiom; a
dispatcher built without one builds its own, which is a fixture's shape and
a one-core instance's.

**It was the owner core's until then**, mutated only within that core's
cooperative event loop - no latches, no sharing - and that was sound while
every write to a relation ran on its owner. AT-S5 made a write run where
its session is, and a write on a core whose registry held no directory for
the relation was admitted unchecked (D1). The core-local alternative - route
an asserted relation's writes back to its owner - would restore exactly the
routing AT-S5 removed, and AT-R15 ruled the shared registry instead.

**What serializes it** (`rules.md` §3's row; `exec/assertion_check.hpp`):

- the **directory latch**, over every map, every directory, the holds and
  the pending reservations. It **never spans page work**; it **does** span
  the WAL append that describes a header change, and that pairing is the
  durability argument - a checkpoint's `ASSERT_SNAPSHOT` is taken and
  appended under the same latch (§7), so no `ASSERT_*` record can land
  between the headers a snapshot carries and its own LSN, and the snapshot
  stays equal to the fold of every record before it;
- one **chain latch** per assertion, across its cabin chain's appends,
  which are page work: the chain's tail and growth are one writer's state.

Both are null at `cores = 1`. The order is a relation's page latch (an
`UPDATE` or `DELETE` reserves from inside its walk), the chain latch, a
cabin page's latch, the directory latch, the WAL stream latch; it is
stated and not checked. Commit and abort take the directory latch, release
it, and only then touch a page.

**The wait AO-S6e-c added rides the lock family**, not this registry: a
refused admission may **wait** for the transaction whose reservation or
hold refused it, and the wait is an edge in the instance's wait-for graph,
because two transactions can each hold a reservation the other's admission
needs and a cycle of that shape is real. AR2 D8's `S`/`X` fence over the
group's slice is not built. It was declined because the check and the
reserve ran inline with nothing between them on one core; they are two
calls with page work between them now, and what closes that interleaving
is the admission's **hold** under the directory latch (§6.2), not a lock
unit.

On a multi-core instance:

- **`CREATE ASSERTION` is built where its session is** and adopts into the
  instance's registry. It was built by the relation's owner from PW1c-6c,
  shipped to it and adopted into its registry; the ship and its three ring
  kinds went at AT-S5d. **The build is not fenced against a writer on
  another core** (§8.1), which is
  `docs/inflight/bugs/create-assertion-build-is-not-fenced-against-writers.md`.
- **Core 0's mount resumes the registry, for every relation**, before any
  peer is built; a peer handed it resumes nothing. **Only core 0's
  checkpoints snapshot it** (§7): two cores' runs of the same assertions in
  the one stream could land back to back, and recovery would meet the
  second run's first group while the first run's base was still open. A
  mount's scan starts at or below core 0's own `CHECKPOINT_BEGIN`, so its
  snapshot is always in range.
- **An assertion the registry knows of and cannot enforce refuses the
  relation's writes, on every core.** Refusing is recoverable; admitting an
  unchecked write is not. What reaches "cannot enforce" is a revive that
  failed and a checkpoint whose snapshots do not cover the base.
- **The file that made a cabin unenforceable is not one any more** (AW-S1b):
  a cabin page is a *user* page and every core writes those, so a chain
  core 0 built for a relation another core owned is appended to like any
  other. The operator's repair - `DROP` then `CREATE` - is no longer
  required for it.

`DROP ASSERTION` evicts from the instance's registry. It sent the owner a
fire-and-forget message to forget the directory until AT-S5d.

### 6.2 Protocol

On a checked write (per §4.2), executed inline in the writing statement:

1. Compute the group key from the row; hash into the group directory.
2. **Admission check:** would `count + Δcount` / `sum + Δsum`, with every
   held contribution to the group counted, exceed the bound? (Checked int64
   arithmetic; overflow ⇒ statement error, AG3.)
   - If yes ⇒ fail the statement with `AssertionViolation`. Nothing was
     mutated or held; no cleanup needed.
   - If no ⇒ the contribution is **held** (AT-S5d, §6.1): it counts in every
     later admission, and the statement gives it back on any exit that does
     not reach step 3.
3. **Reserve:** append a Bound Cabin entry with the `RESERVED` flag, then -
   one step under the registry's latch - emit the WAL record (§7), apply the
   delta to the group header and release the hold. Proceed with the
   heap/index writes of the statement.
4. **Commit:** clear `RESERVED` on the transaction's entries (piggybacked on
   commit processing; aggregate is already correct).
5. **Abort:** via the undo chain, remove the transaction's reserved entries
   and subtract their deltas from the group headers. Emit the compensating
   WAL records.

Properties. **Three of these were rewritten at AO-S6e-c** (census row 11,
`instructions/v3.0.0/workorder-ao-m2-lock-family.md`), which turned the
last one from an accepted outcome into a wait; what each said before is
kept beside what it says now, because a property list is cited and a
silent replacement would leave the citations pointing at the wrong claim.

- **No waiting inside the check; a wait after it.** Admission is a
  computation under the registry's directory latch, and contenders on two
  cores are serialized by that latch (AT-S5d; they were serialized by one
  core's event loop until then). **An admitted row's contribution is
  held** from its admission until its reservation's record is appended:
  a hold counts in every later admission to the group and in no snapshot,
  and a statement that fails, or parks and re-runs, between the two gives
  it back. An `UPDATE` holds its arrival's full contribution, whatever
  was checked, because its departure is applied first and a group lowered
  by it with nothing held would read low to another core in between. What
  changed at AO-S6e-c is what happens *after* a refusal:
  where the aggregate that refused includes a reservation of a transaction
  still in flight, the statement waits for that decide rather than
  failing. It was *"never blocked"*.
- **Deterministic failure, no retry storm, no livelock.** The half that
  survives. It was also *"the loser of a race fails **immediately**"*, and
  that is now false by construction: the loser of a race against an
  undecided reservation waits for it. The failure it eventually gets is
  still deterministic and still truthful.
- **No false admissions.** Unchanged. Reservations are counted in the
  aggregate from the moment of admission - as a hold until their record is
  appended, since AT-S5d. A negative contribution is held as zero, so a
  hold never reads as room another core's admission could take.
- **A false rejection is a wait, not an outcome — under four conditions,
  and all four are ordinary.** A statement refused because of a reservation
  belonging to a transaction that later aborts waits for the decide and is
  admitted when the reservation goes; when the reservation **commits**, the
  refusal was true and is delivered then. It was *"bounded false rejections
  … accepted and documented (identical in spirit to unique-index insertion
  behavior)"*, and the analogy went with it — a unique index has nothing to
  wait for.

  The conditions, stated here because this is the bullet that gets cited:
  the statement must be on a path that can **park** (the synchronous
  `Dispatch` and the KWP load path's `ExecuteInsert` still refuse at once);
  it must have **written nothing yet**, so a bulk `INSERT` past its first
  row and an `UPDATE` past its first written row are refused rather than
  waited — which is the common case for a bulk statement, not an edge; an
  `UPDATE` that has already **reserved** for an earlier assertion on the
  same relation is refused too, because its reservations are invisible to
  the re-runnability test and a re-run would count them twice; and the wait
  ends at the lock family's fault net like any other.

The wait is the lock family's, not a mechanism of this protocol's own: it
is recorded where every other wait in M2 is, so it is bounded by the same
fault net, refused where it would close a cycle in the same wait-for
graph, and offered only on a path that can park — the synchronous
`Dispatch` still answers the violation at once, which is what
`AssertionEnforceTest`'s own cell pins. The contended resource is a
**group** and not a row, so a refusal that names a waiter names the
assertion and never a key, on both arms: an `INSERT`'s id is not issued at
admission time (the admission runs first so that a refused row burns
nothing), and an `UPDATE`'s row is the one *being written* rather than the
one being waited for — the holder of a group reservation may never have
touched it.

**And reaching the fault net means something different here.** For a row it
is a defect report (AO-R8): a wait ends at the holder's decide, so the
clock ending it means a deadlock went undetected or a holder is stuck. A
group is held for a transaction's length and every writer touching one
account serialises on it, so the net is reached by ordinary contention. The
refusal says so rather than sending an operator after a stuck holder.
Whether a group wait should carry a shorter bound of its own is open —
AO-0 item 22, deferred to AO-S7 because it interacts with AO-R8's
one-net-per-statement rule.

### 6.3 Interaction with MVCC

Reservations are orthogonal to tuple visibility: they constrain admission,
not reads. Undo integration (step 5) is mandatory for correctness. Row
locking (Keystone lock byte) is not used by this protocol — and since AO-R3
the byte is not used by anything: the lock family holds no persisted bit,
the tuple `X` being the header's `trx_id` stamp. What this protocol does
use since AO-S6e-c is the family's **wait**, and §6.1 and §6.2 say where.

---

## 7. Durability and recovery (AS6)

- Bound Cabin pages are headered, checksummed (S9), and cached through the
  standard buffer pool, which is the instance's one frame table since AM-S2 step 3 (`page.md` S7).
- WAL record types (extends `wal.md`):
  - `ASSERT_RESERVE` — entry append + group delta (statement time);
  - `ASSERT_COMMIT` — reserved→committed flag transition (batched per txn);
  - `ASSERT_ROLLBACK` — compensating removal + negative delta (abort path);
  - `ASSERT_BUILD` — bulk records emitted by the CREATE-time builder;
  - `ASSERT_SNAPSHOT` — AS6a's per-checkpoint group-header base;
  - `ASSERT_DROP` — teardown.
- Ordering: WAL-before-data, consistent with the existing contract.
- Recovery: replay restores group headers and entries exactly; in-flight
  (uncommitted) reservations at crash are rolled back by normal transaction
  recovery via `ASSERT_ROLLBACK` compensation. The constraint is enforceable
  immediately at restart — **no rebuild scan, no enforcement gap**. "No
  rebuild scan" means no re-scan of the *relation*, which is what AS7's
  CREATE-time build costs; AS6a's linkage rebuild reads the cabin's own
  pages, whose size is the assertion's entry count and not the table's.

  > **AS6a — where assertion replay starts.**
  >
  > **The rule.** A Bound Cabin's group directory is made durable by a
  > **per-checkpoint snapshot of its group headers** —
  > `{group_id, key, count, sum}`, O(groups) — and assertion replay folds
  > `ASSERT_*` records **from the last checkpoint forward**, never from the
  > cabin's birth. Every entry carries the `group_id` of its group (§5.1),
  > so the header→entry linkage is rebuilt by scanning the cabin's own
  > pages rather than persisted.
  >
  > **A snapshot is a base only if it equals the fold of every `ASSERT_*`
  > record before it.** One core's event loop gave that for free; with every
  > core reserving into one registry (§6.1, AT-S5d) it is the registry's
  > directory latch, held across both a header change and the record that
  > describes it, and across a snapshot and the records that carry it. A
  > hold is in no snapshot: it becomes header when its record is appended.
  >
  > **Recovery order.** Ordinary redo restores the entry pages → the
  > snapshot is loaded → the cabin's pages are scanned and bucketed by
  > `group_id`, rebuilding the linkage → `ASSERT_*` records are folded from
  > the checkpoint forward. Bounded by the cabin's own pages: not by the
  > relation, and not by the log.
  >
  > **Why not from `ASSERT_BUILD`.** Starting replay at each cabin's build
  > record makes RTO a function of the assertion's lifetime, but the
  > disqualifier is not speed — it makes correctness depend on the WAL never
  > recycling the segment holding that record. `wal.md` §13 lists retention
  > as ordinary operational configuration, and a retention setting that
  > silently becomes a correctness setting is the wrong coupling.
  >
  > **Why the snapshot is headers-only, and why the entry carries the id.**
  > A header's entry-list is not O(groups): `BoundCabin::Apply` and
  > `ApplyDeparture` append one `(page_id, index)` pair per checked write
  > and only ever remove one on abort, so the linkage is O(all writes,
  > forever). It cannot simply be dropped from the snapshot either —
  > `Unapply` answers `NotFound` when the pair is absent, so a reservation
  > made before a checkpoint and rolled back after it would fail the mount.
  > Carrying `group_id` on the entry and on `AssertEntryPayload` is what
  > makes the linkage reconstructible instead, and reduces the snapshot to
  > the group count.
  >
  > **Unchanged:** the write amplification budgeted below, the admission
  > check, and its O(1) read.

  > **AS6b — an aborted entry is distinguishable on the page.**
  >
  > `AssertionEnforcer::AbortTxn` removes an aborted reservation's entry
  > from the group's list but leaves the bytes on the page by design — the
  > orphaned slot is the recorded leak that rides on purge. AS6a's linkage
  > scan must not attach those bytes as a live entry, and an abort *before*
  > the last checkpoint has its `ASSERT_ROLLBACK` outside the fold's range,
  > so folding cannot repair it. (The aggregate is right either way — it is
  > the snapshot plus the folded deltas, never a re-sum; what a mis-attached
  > entry breaks is §5.2's proof, `VerifyAgainstEntries`, which would report
  > `Corruption` for a directory that is right.)
  >
  > **The rule.** `flags` bit 3, `kEntryOrphaned`, is set on the entry when
  > its reservation aborts, by the live path and by `ASSERT_ROLLBACK` replay
  > alike, and the linkage scan skips a marked entry. Bit 3 reads 0 on every
  > entry written before the flag existed and "0" means "not aborted", which
  > is true of them.
  >
  > **The fold answers the other order.** A mark is durable as soon as the
  > checkpoint that flushed its page completes, so *reserved before the
  > checkpoint, rolled back after it, page on disk before the crash* has
  > the walk skipping an entry whose `ASSERT_ROLLBACK` is still inside the
  > fold's range. The compensation must happen anyway (the base snapshot was
  > taken while the reservation was live and counts its delta), so
  > `ReplayRollback` **restores a linkage the walk deliberately did not**
  > before calling `Unapply`. `Unapply`'s missing-pair `NotFound` stays a
  > name check for the live abort path, where the pair comes from the
  > transaction's own reservation list; a rebuild is entitled not to have
  > restored it.
  >
  > **Cost asymmetry, stated.** `CommitTxn` groups its pending reservations
  > by `(assertion, page)` and pays one page fetch, one `Open`, one WAL
  > record and one `StampPageLsn` per *group*; `AbortTxn` walks reservations
  > one at a time and pays all four per *reservation*, because
  > `ASSERT_ROLLBACK` carries one group key per record where `ASSERT_COMMIT`
  > takes a repeated-index list. Since `BoundCabinChainWriter::Append`
  > always appends at the tail, a transaction's K entries share a page
  > whatever their `GROUP BY` values, so the two costs coincide only at K=1.

- Verification: `VerifyAgainstEntries` re-sums entries against group headers.

Write amplification budget: one 32 B entry + one small group-delta WAL record
per checked write. Documented as an accepted product cost of enabling an
assertion on a relation.

---

## 8. Lifecycle and catalog (AS7, AS10)

### 8.1 CREATE

The build runs **synchronously inside the CREATE statement**, not in a
background scheduling group: the engine has no suspendable statement path
(`crosscore.md` P4), and the index backfill set the precedent. On one
cooperative core this means no write can interleave with the build, so
§8.1a's membership protocol is met trivially. **Not on more than one core,
since AT-S5**: a write runs where its session is, and a write on another
core that is admitted before the build's directory is adopted, and places
its row where the build's scan has already passed, is in neither the
cabin nor the scan. Nothing fences it - the build takes no relation lock -
and §8.1a's membership protocol, which would, is not built.
`docs/inflight/bugs/create-assertion-build-is-not-fenced-against-writers.md`
carries it, and `workorder-at-m3-uniformity.md` AT-0 item 13 its shape. A row written by a transaction still in
flight when the build reads it refuses the CREATE with `TxnConflict`,
retryably — counting it and losing the abort would overstate the group
forever, and skipping it and seeing the commit would understate it.

The steps below are three entry points - `PrepareAssertionDef`
(validation and the id), the build, the publish - and since AT-S5d all
three run where the session is (§6.1); the build was the relation owner's
until then. AS6a's base is logged at the end of the *build* rather than
after the publish, the order the owner's build needed because it could
not see core 0's row and had to reply before it existed. What that costs is
an `ASSERT_SNAPSHOT` for an assertion whose publish then fails — a base for
a cabin no catalog row names, which no mount folds, since a mount folds
only what `ListAssertions` returns.

1. Create-time validation (§3.1).
2. Full scan of the target relation where the session is, inside the
   statement.
3. Build Bound Cabin entries and group aggregates; emit `ASSERT_BUILD` WAL.
4. If any group violates the bound ⇒ CREATE fails with `AssertionViolation`
   naming the first violating group; the partial build is discarded.
5. At success the Bound Cabin exactly reflects all admitted rows, and
   enforcement begins atomically at cutover (§8.1a).

### 8.1a Cutover: the membership-check protocol

**A row counts as incorporated if and only if its pk is present in the Bound
Cabin.** Not inferred from pk ordering, not from scan position, not from a
watermark. The Cabin is the sole source of truth about its own contents.

A pk watermark ("already scanned" ⇔ `pk <= watermark`) is not used: it would
rest on Keystone pk issuance being monotonic, which
`docs/rules/keystoneid-invariant.md` K3 ("No density promise") withholds
precisely so that no correctness argument may be built on it.

Membership removes the external assumption rather than repairing it.
Correctness reduces to **check-then-apply atomicity** — classify the row,
then apply its delta, with nothing in between. One cooperative core's event
loop provided it, because both happened inside one uninterruptible step;
the registry's directory latch is what would provide it across cores
(§6.1), and the protocol is not built (§8.1).

*What follows from it.*

- **Builder scan order is correctness-irrelevant.** Plain page order is
  enough; the builder needs no ordering guarantee from the storage layer and
  imposes none.
- **Build-time write deltas apply at commit time**, which keeps undo
  integration out of the build phase entirely — the abort path during a build
  is the ordinary one, not a second protocol.
- **Membership lookups cost something, but only while building.** For a
  `COUNT` assertion the per-group entry set is bounded by the assertion's own
  bound, so the lookup is small by construction; for a `SUM` assertion it is
  not bounded.
- **Publish is the single commit point.** Final validation, the
  `sys.assertions` row and plan-cache invalidation happen in one step, so no
  crash timing can leave an assertion partially enforced: either it is
  published and enforcing, or it does not exist.

### 8.2 Catalog

`sys.assertions` (fixed-page bootstrap, same pattern as `sys.patterns`):

| Column | Type | Notes |
|---|---|---|
| assertion_id | u32 | engine-issued |
| name | varchar | unique |
| target_oid | u32 | RESTRICT on relation drop |
| source_text | varchar | full declaration verbatim (sys.pattern_defs model; single row, no params table) |
| cabin_root | page id | Bound Cabin anchor |
| flags | u32 | reserved (deferrable/not-valid bits) |

### 8.3 DROP

`DROP ASSERTION` removes the catalog row, tears down the Bound Cabin
(`ASSERT_DROP`), and unpins its pages. `DROP TABLE` on a relation with
assertions fails with `Restrict`.

---

## 9. Observability (AS10)

ANALYZE gains an `Assertion` line per checked statement:

```
Assertion  checks=1  reserved=1  violations=0  group_dir_probes=1
```

Production counters (per assertion, in the stats system): admission checks,
violations, reservations rolled back by abort, hint-heal events. Dev-mode
profiling hooks follow the established dev/production split.

---

## 10. Product constraints and non-goals (v1)

Documented, truthful limits — all violations of these produce a refusal
(§2's code rule) or a create-time error, never silent degradation:

- Predicates: `COUNT(*)` and `SUM(int64 col)` upper bounds only (AS1, AS11).
- Single relation per assertion (AS8).
- Statement-time enforcement only; no deferred mode (AS3).
- Bounds are non-negative integer literals.
- No `HAVING`-style filtered groups, no `WHERE`-scoped (partial) assertions.
- `AVG`, `MIN`, `MAX` bounds: refused (MIN/MAX are not incrementally
  maintainable under deletion without extra structure).
- uint64 SUM: `Unsupported` (AG3 parity).
- Assertions on relations with Waystone/pattern features remain fully
  compatible: Bound Cabin is an independent instance and does not alter
  Observational Cabin, trail, or pattern behavior.
