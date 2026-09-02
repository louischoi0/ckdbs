# ASSERTION — Group-Level Declarative Constraints

Status: **ADOPTED (v1 scope), built and enforcing on every core**
Related documents: `docs/spec/cabin.md` (§12 is the Bound Cabin class split this spec requires), `docs/spec/wal.md`, `docs/spec/txn.md`, `docs/spec/foreign-keys.md`, `docs/spec/crosscore.md` (CC7's owner-builds exception). The U5 durability tier §5 cites is a design reference with no owning doc — v1 has no unique index (`docs/spec/index.md` IX11). ANALYZE's surface is `manual/sql/sql.md` §4.

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
- Concurrency is handled by a **reservation protocol executed on the
  relation's owner core** (AS4). Because group state is owned by exactly one
  core and mutated only inside its cooperative event loop, admission is
  atomic without latches. There is no waiting, no retry storm, and no
  deadlock; failure is immediate and deterministic.

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
| AS4 | Reservation protocol combined with owner-core group-key serialization. No latches, no waiting, no deadlock. |
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
| UPDATE, group columns changed | Yes, on the **destination** group only | Modeled as departure (no check) + arrival (checked). Both aggregate mutations are applied atomically on the owner core. |
| DELETE | No | Strictly decreasing; cannot violate an upper bound (AS11). |

The check runs inside the writing statement, the same way FK checks do —
no trigger machinery. Statement classes are unaffected; pattern
fingerprints are unaffected.

### 4.3 Timing and isolation

Checks execute at statement time against the group's current authoritative
aggregate (committed + reservations) on the owner core. This is intentionally
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

## 6. Concurrency: reservation on the owner core (AS4)

### 6.1 Ownership

All Bound Cabin state for a relation lives on that relation's **owner
core** and is mutated only within its cooperative event loop. No latches, no
atomic CAS loops, no cross-core sharing. v1 assertions are single-relation
(AS8), so the entire protocol is core-local. On a multi-core instance:

- **`CREATE ASSERTION` on a relation another core owns is built by that
  core.** Core 0 keeps §3.1's checks, the id and the `sys.assertions` row;
  the owner scans under its own view, allocates the chain from its own
  extent lease, logs `ASSERT_BUILD` and AS6a's base into its own stream, and
  adopts the directory at the end of its build. No page crosses a stream
  (`docs/spec/crosscore.md` CC7's owner-builds exception).
- **The enforcing core is the owning core, at every mount too.** Recovery's
  assertion resume runs per core and takes on only the relations that core
  owns; the owner's own checkpoint carries the group snapshots, so the base
  and the records folded onto it are one stream's.
- **A core that knows of an assertion it cannot enforce refuses the
  relation's writes.** Refusing is recoverable; admitting an unchecked write
  is not. The one such file is a cabin core 0 built for a relation another
  core owns (a file from before owner-built cabins): the owner refuses the
  relation's writes until the operator's repair, `DROP` then `CREATE`, which
  builds the cabin on the owner. The refusal is the peer write path's
  (`CheckWriteAffinity`'s peer branch); **on core 0** an unrecoverable
  assertion on a core-0-owned relation reports `enforcing=0` and admits
  writes.

`DROP ASSERTION` is core 0's statement and sends the owner one message to
forget the directory; a lost one leaves the owner over-enforcing until its
next mount, which is the fail-closed side of a message with no
acknowledgement.

### 6.2 Protocol

On a checked write (per §4.2), executed inline in the writing statement:

1. Compute the group key from the row; hash into the group directory.
2. **Admission check:** would `count + Δcount` / `sum + Δsum` exceed the
   bound? (Checked int64 arithmetic; overflow ⇒ statement error, AG3.)
   - If yes ⇒ fail the statement with `AssertionViolation`. Nothing was
     mutated; no cleanup needed.
3. **Reserve:** apply the delta to the group header and append a Bound Cabin
   entry with the `RESERVED` flag. Emit the WAL record (§7). Proceed with the
   heap/index writes of the statement.
4. **Commit:** clear `RESERVED` on the transaction's entries (piggybacked on
   commit processing; aggregate is already correct).
5. **Abort:** via the undo chain, remove the transaction's reserved entries
   and subtract their deltas from the group headers. Emit the compensating
   WAL records.

Properties:

- **No waiting / no deadlock.** Admission is a pure core-local computation;
  contenders are serialized by the event loop, never blocked.
- **Deterministic failure.** The loser of a race fails immediately with a
  truthful error; there is no retry storm and no livelock.
- **No false admissions.** Reservations are counted in the aggregate from the
  moment of admission.
- **Bounded false rejections.** A statement can be rejected due to a
  reservation of a transaction that later aborts. This is accepted and
  documented (identical in spirit to unique-index insertion behavior).

### 6.3 Interaction with MVCC

Reservations are orthogonal to tuple visibility: they constrain admission,
not reads. Undo integration (step 5) is mandatory for correctness. Row
locking (Keystone lock byte) is not used by this protocol.

---

## 7. Durability and recovery (AS6)

- Bound Cabin pages are headered, checksummed (S9), and cached through the
  standard per-core buffer pool (S7).
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
(`crosscore.md` P4), and the index backfill set the precedent. On a
cooperative core this means no write can interleave with the build, so
§8.1a's membership protocol is met trivially; it remains the correctness
story for a build that yields. A row written by a transaction still in
flight when the build reads it refuses the CREATE with `TxnConflict`,
retryably — counting it and losing the abort would overstate the group
forever, and skipping it and seeing the commit would understate it.

The steps below are three entry points, because on a multi-core instance
they do not all run on the same core: `PrepareAssertionDef` (validation and
the id) and the publish are core 0's, the catalog having one writer, and
**the build is the relation owner's** (§6.1). AS6a's base is logged at the
end of the *build* rather than after the publish, because the owner cannot
see core 0's row and must reply before it exists. What that costs is an
`ASSERT_SNAPSHOT` for an assertion whose publish then fails — a base for a
cabin no catalog row names, which no mount folds, since a mount folds only
what `ListAssertions` returns. The single-core path takes the same order.

1. Create-time validation (§3.1).
2. Full scan of the target relation on its owner core, inside the
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
then apply its delta, with nothing in between — and the owner core's
cooperative event loop provides that: both happen inside one
uninterruptible step, the same property AS4's admission protocol rests on
(§6.1). No new mechanism is introduced.

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
