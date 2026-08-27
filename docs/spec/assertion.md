# ASSERTION — Group-Level Declarative Constraints

Status: **ADOPTED (v1 scope)**
Related documents: `docs/spec/cabin.md` (§12 is the Bound Cabin class split this spec required, landed at AST01), `docs/spec/wal.md`, `docs/spec/txn.md`, `docs/spec/foreign-keys.md`.

*(Filenames corrected 2026-08-10. The list was written against names that were never in this repository: `cabin.md`, `fk.md`, and two documents that do not exist at all — `unique-index.md`, whose U5 durability tier §5 cites is a design reference with no owning doc, since v1 has no unique index (`docs/spec/index.md` IX11), and `analyze.md`, since **nothing owns ANALYZE**; its surface is `manual/sql/sql.md` §4.)*

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
  relation's home core** (AS4). Because group state is owned by exactly one
  core and mutated only inside its cooperative event loop, admission is
  atomic without latches. There is no waiting, no retry storm, and no
  deadlock; failure is immediate and deterministic.

Everything outside the supported class is a truthful `Unsupported` error, in
line with the engine-wide contract: fewer features, exactly specified, fast
and correct.

---

## 2. Decision Record

| ID | Decision |
|----|----------|
| AS1 | v1 predicate class: group cardinality (`COUNT(*)`) and group sum (`SUM(col)`) constraints over a single `GROUP BY` column list. General `NOT EXISTS` / subquery predicates: `Unsupported`. |
| AS2 | KDS-restricted syntax (`CREATE ASSERTION ... ON rel GROUP BY (...) CHECK ...`), not the SQL-92 free-form `CHECK (search condition)`. The grammar itself encodes the supported class; create-time validation is maximized. |
| AS3 | Statement-time checking only (fail-fast). `DEFERRABLE` is reserved in the grammar and rejected as `Unsupported`. |
| AS4 | Reservation protocol combined with home-core group-key serialization. No latches, no waiting, no deadlock. |
| AS5 | No separate counter store. The Bound Cabin is the single structure: entries plus a per-group running aggregate maintained in the group directory header. Checks are computed against the Cabin in real time on the write path. |
| AS6 | Bound Cabin is a **logged, headered authority class** (same durability tier as the var-heap (V3) and unique indexes (U5)). Prerequisite: the Cabin class split defined in §5. |
| AS6a | **Decided 2026-08-11.** Where assertion replay starts: a **per-checkpoint snapshot of the group headers** (`{group_id, key, count, sum}`), folded forward with `ASSERT_*` records **from the last checkpoint** — never from the cabin's birth, which would make RTO a function of the assertion's lifetime and make WAL retention a correctness setting. Every entry carries its `group_id` (§5.1) so the header→entry linkage is rebuilt from the cabin's own pages instead of persisted. Narrows AS5's "not a separate store" to "not a separate authority". Full statement and costs: §7. Owned by `docs/workplan-wal-recovery.md` RC07. |
| AS7 | `CREATE ASSERTION` performs a full scan of the target relation to build the Bound Cabin and initial aggregates. Any existing violation fails the CREATE and discards the build. `NOT VALID` is reserved grammar, `Unsupported` in v1. |
| AS8 | v1 assertions target exactly one relation. Multi-relation assertions: `Unsupported` (blocked on cross-core write / 2PC, which is itself reserved). |
| AS9 | A violation is a **statement error** (transaction survives), consistent with AG3 overflow semantics. New Status code `AssertionViolation`; the error carries the assertion name and the violating group key. |
| AS10 | Catalog: `sys.assertions` storing the full declaration `source_text` (same model as `sys.pattern_defs`). `DROP ASSERTION` supported. Dropping a relation referenced by an assertion is `RESTRICT`. ANALYZE reports per-statement assertion check counts and reservation failures. |
| AS11 | **Revised 2026-08-08.** v1 supports **upper-bound constraints only**: comparison operators `<` and `<=`. Lower bounds (`>`, `>=`) are `Unsupported` — they would require checking on DELETE and on decreasing UPDATE paths. **`=` is `Unsupported` with them**, having briefly been accepted as meaning `aggregate <= N`: that reinterpreted what the operator wrote, and enforcing real equality needs the lower-bound half anyway, so `=` costs exactly what `>=` costs. Consequently DELETE never requires an assertion check in v1. |

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

- `<op>` ∈ { `<`, `<=` } (AS11 as revised). `>`, `>=` and `=` parse but
  return `Unsupported`.
- `DEFERRABLE` / `NOT DEFERRABLE` and `NOT VALID` are reserved tokens: they
  parse and return `Unsupported` (AS3, AS7).
- Assertion names live in the same namespace as other schema objects and must
  be unique.

### 3.1 Create-time validation (maximized, per CREATE PATTERN precedent)

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

### 3.1a Why `=` is refused (AS11, revised 2026-08-08)

`=` was originally admitted as an upper-bound-style constraint: the enforced
invariant would have been `aggregate <= N`, and the operator was accepted "for
syntax familiarity and documented as such".

**That is withdrawn.** Documenting the reinterpretation does not make it
honest: the engine would enforce something other than what the operator wrote,
and a client reading `CHECK COUNT(*) = 5` would reasonably expect a group of
three rows to be a violation. A constraint that quietly means less than it
says is worse than one that is refused, because the refusal is visible at
`CREATE` and the reinterpretation is visible nowhere.

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
| UPDATE, group columns changed | Yes, on the **destination** group only | Modeled as departure (no check) + arrival (checked). Both aggregate mutations are applied atomically on the home core. |
| DELETE | No | Strictly decreasing; cannot violate an upper bound (AS11). |

The check is compiled into the statement's step chain (same mechanism as FK
checks — no trigger machinery). Statement classes are unaffected; pattern
fingerprints are unaffected.

### 4.3 Timing and isolation

Checks execute at statement time against the group's current authoritative
aggregate (committed + reservations) on the home core. This is intentionally
**stricter than snapshot visibility**: a statement may be rejected because of
a concurrent uncommitted reservation. This is the correct trade for an
upper-bound admission constraint — it can produce false rejections only in
races where at most one of the contenders could have succeeded anyway, and it
can never produce a false admission.

### 4.4 Error semantics (AS9)

A violation aborts the **statement only**. The transaction remains open and
usable. Status: `AssertionViolation`, message including the assertion name
and the rendered group key, e.g.:

> **[RESOLVED at AST07 (2026-08-09), operator-decided]: the violation
> poisons, like every other write failure.** AS9's "the transaction remains
> open and usable" is amended: inside an explicit transaction the session
> enters failed-txn and the client must ROLLBACK — uniform with
> `FK_VIOLATION` and per-transaction failure atomicity (docs/spec/txn.md §6),
> and the only honest option once a multi-row UPDATE can violate on row 3
> of 10 with rows 1-2 already written; "open and usable" would need
> statement-level rollback the engine does not have. In autocommit the
> statement *is* its transaction and unwinds fully, reservations included —
> which is the sense in which a violation is a statement error. A refusal
> itself still mutates nothing (§6.2 step 2).

```
AssertionViolation: assertion "user_product_purchase_limit"
  group (user_id=41, product_id=7): COUNT(*) would exceed bound 5
```

---

## 5. Bound Cabin (Cabin class split)

This section is normative for the required revision of `docs/spec/cabin.md`,
which landed at AST01 as that document's §12.

The Cabin structure splits into two classes with a shared page format and
shared lookup machinery but different lifecycle contracts:

| Property | Observational Cabin (existing) | **Bound Cabin (new)** |
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
| flags | 8 bit | includes `RESERVED` for in-flight entries (§6) and, since AS6b, `ORPHANED` for an entry whose reservation aborted (§7) |
| reserved | 16 bit | alignment / future |
| location hint: page id / epoch / slot | 64 bit | advisory; shares Waystone validation rules; on hint failure fall back to pk descent and heal in place |
| aggregate value | 64 bit | the row's `SUM` column value, inline (int64). For COUNT-only assertions this field is written as 1. |
| `group_id` | 32 bit | **AS6a.** Which group of this cabin the entry belongs to. Authoritative, not advisory: it is what lets recovery rebuild the header→entry linkage by scanning the cabin's own pages. |
| padding | — | to 32 B |

Exact bit packing is an implementation detail of AST04; the normative facts
are: fixed 32 B, pk authoritative, hint advisory, value inline, `group_id`
authoritative.

**Built 2026-08-12** (`docs/workplan-wal-recovery.md` RC07 parts 1-2): the
field exists on the entry, on `AssertEntryPayload`, and on the in-memory group
header, and all three writing sites stamp it. The replay fold **adopts** the
id a record carries rather than assigning one, because a fold starting from a
checkpoint meets groups in record order and an id assigned in that order would
drift from the ids the entries already on the pages carry — misattributing them
at the next recovery. What is not built is where the snapshot comes from: RC07
parts 3-4.

`group_id` occupies the first 4 bytes of what AST04 shipped as padding and
wrote as a literal zero, so the 32 B width is unchanged. **An id and not a
group-key hash**, and the difference is correctness rather than taste:
`HashGroupKey` is a mixing function whose collisions are expected and are
resolved by confirming the stored key (§5.2), and an entry carries no key —
so an entry holding only a hash could not be attributed between two colliding
groups. An id makes attribution exact and removes the collision question from
the recovery path.

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
Entries exist for violation diagnostics, repair/verification (re-summation),
and future extension; they are not on the check hot path.

The running aggregate is not a separate **authority** (AS5): it is a field of
the Cabin group header, recovered by WAL replay and verifiable against the
entry list. AS6a narrows AS5's original wording — the directory does acquire a
durable form, a per-checkpoint snapshot — but the narrowing is only of the
word "store": the entries remain the authority, the snapshot is a derived
cache, and `VerifyAgainstEntries` is what proves one against the other.

### 5.3 One Bound Cabin per assertion

v1 binds exactly one Bound Cabin instance to each assertion. Sharing a Bound
Cabin between assertions with identical group-column lists is a possible
later optimization, out of scope for v1.

---

## 6. Concurrency: reservation on the home core (AS4)

### 6.1 Ownership

All Bound Cabin state for a relation lives on that relation's **home core**
and is mutated only within its cooperative event loop. No latches, no atomic
CAS loops, no cross-core sharing. v1 assertions are single-relation (AS8), so
the entire protocol is core-local.

**Made true across cores 2026-08-26** (PW1c-6c,
`docs/inflight/in-progress/workplan-peer-writer.md` §7d). This paragraph
said "home core" while the *implementation* built every Bound Cabin on core
0, and on a multi-core instance the two are different cores. What that cost
was measured before it was fixed
(`bench/v2.2.0/results-shipping-part-a-v2.2.0-11-g925f483.md` Finding 2): a
write to an assertion-covered relation a peer owned was **admitted and not
checked** — a second row landed in a group under `CHECK COUNT(*) <= 1` — and
it could not have been checked, because appending an entry to core 0's pages
is a write the owner is refused. Three consequences, now enforced:

- **`CREATE ASSERTION` on a relation another core owns is built by that
  core.** Core 0 keeps §3.1's checks, the id and the `sys.assertions` row;
  the owner scans under its own view, allocates the chain from its own
  extent lease, logs `ASSERT_BUILD` and AS6a's base into its own stream, and
  adopts the directory at the end of its build. No page crosses a stream, so
  PL's handoff is not invoked (`docs/spec/crosscore.md` CC7's owner-builds
  exception).
- **The enforcing core is the owning core, at every mount too.** RC07's
  resume runs per core and takes on only the relations that core owns; the
  owner's own checkpoint carries the group snapshots (PW3), so the base and
  the records folded onto it are one stream's.
- **A peer that knows of an assertion it cannot enforce refuses the
  relation's writes.** That is the pre-PW1c-6c file — a cabin core 0 built
  for a peer's relation — and the fail-closed answer to every other way a
  directory can fail to come back: refusing is recoverable, admitting an
  unchecked write is not. The operator's repair is `DROP` then `CREATE`,
  which builds the cabin on the owner. **On core 0 the stance is unchanged**
  and the refusal does not apply: the gate that carries it is the peer write
  path's (`CheckWriteAffinity`'s peer branch), and an unrecoverable assertion
  on a core-0-owned relation still reports `enforcing=0` and admits writes,
  as it has since RC07. Widening it there would turn a constraint that cannot
  be rebuilt into a relation that cannot be written, on the single-core
  configuration too, which is a product decision this task did not take.

`DROP ASSERTION` stays core 0's statement and sends the owner one message to
forget the directory; a lost one leaves the owner over-enforcing until its
next mount, which is the fail-closed side of a message with no
acknowledgement.

### 6.2 Protocol

On a checked write (per §4.2), executed inline in the step chain:

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
not reads. Undo integration (step 5) is mandatory for correctness and is an
explicit workplan item (AST07). Row locking (Keystone lock byte) is not used
by this protocol.

---

## 7. Durability and recovery (AS6)

- Bound Cabin pages are headered, checksummed (S9), and cached through the
  standard per-core buffer pool (S7).
- WAL record types (extends `wal.md`):
  - `ASSERT_RESERVE` — entry append + group delta (statement time);
  - `ASSERT_COMMIT` — reserved→committed flag transition (batched per txn);
  - `ASSERT_ROLLBACK` — compensating removal + negative delta (abort path);
  - `ASSERT_BUILD` — bulk records emitted by the CREATE-time builder;
  - `ASSERT_DROP` — teardown.
- Ordering: WAL-before-data, consistent with the existing contract.
- Recovery: replay restores group headers and entries exactly; in-flight
  (uncommitted) reservations at crash are rolled back by normal transaction
  recovery via `ASSERT_ROLLBACK` compensation. The constraint is enforceable
  immediately at restart — **no rebuild scan, no enforcement gap**. "No
  rebuild scan" means no re-scan of the *relation*, which is what AS7's
  CREATE-time build costs; AS6a's linkage rebuild reads the cabin's own
  pages, whose size is the assertion's entry count and not the table's.

  > **AS6a — where assertion replay starts. Decided 2026-08-11; closes the
  > gap found at AST05 (2026-08-09).** Owned by
  > `docs/workplan-wal-recovery.md` RC07.
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
  > **Why not the other option.** Starting replay at each cabin's
  > `ASSERT_BUILD` makes RTO a function of the assertion's lifetime, but the
  > disqualifier is not speed — it makes correctness depend on the WAL never
  > recycling the segment holding that record. `wal.md` §13 lists retention
  > as ordinary operational configuration, and a retention setting that
  > silently becomes a correctness setting is the wrong coupling to ship.
  >
  > **Why the snapshot is headers-only, and why the entry had to change.**
  > A header's entry-list is not O(groups): `BoundCabin::Apply` and
  > `ApplyDeparture` append one `(page_id, index)` pair per checked write
  > and only ever remove one on abort, so the linkage is O(all writes,
  > forever). It cannot simply be dropped from the snapshot either —
  > `Unapply` answers `NotFound` when the pair is absent, so a reservation
  > made before a checkpoint and rolled back after it would fail the mount.
  > Persisting the linkage would mean writing O(all entries) at every
  > checkpoint; carrying `group_id` on the entry is what makes it
  > reconstructible instead, and reduces the snapshot to the group count.
  >
  > **What this costs, and why now.** Two persisted formats move: the entry
  > gains `group_id` in bytes AST04 already writes as zero, and
  > `AssertEntryPayload` gains the same field so replay never re-derives an
  > id. Both are free **today** — every entry's padding is zero on every
  > page in existence, and no WAL stream has ever been read back, which is
  > the same argument that let RC03's `UNDO_WRITE` correction move without a
  > format-version event. Once assertions ship, each becomes one.
  >
  > **Unchanged:** the write amplification budgeted below, the admission
  > check, and its O(1) read.

  > **AS6b — an aborted entry is distinguishable on the page. Decided and
  > built 2026-08-12**; closes the half of the recovered-linkage defect that
  > `DedupeEntryLinkage` could not.
  >
  > **The defect.** AS6a rebuilds the header→entry linkage by scanning the
  > cabin's own pages. `AssertionEnforcer::AbortTxn` removes an aborted
  > reservation's entry from the group's list but leaves the bytes on the page
  > by design — the orphaned slot is the recorded leak that rides on purge —
  > and the scan could not tell those bytes from a live entry's. Any cabin
  > whose history includes an abort **before the last checkpoint** therefore
  > recovered with an entry list the live directory had dropped. The abort's
  > `ASSERT_ROLLBACK` is outside the fold's range in exactly that case, so no
  > amount of folding could have repaired it.
  >
  > **What was and was not wrong.** The aggregate was correct either way —
  > it is the snapshot plus the folded deltas, never a re-sum — so admission
  > answered right and the constraint enforced correctly. What broke was
  > §5.2's proof: `VerifyAgainstEntries` reported `Corruption` for a directory
  > that was right, which disables the one check that would catch a real
  > divergence precisely after a restart, when it is most worth running.
  >
  > **The rule.** `flags` bit 3, `kEntryOrphaned`, is set on the entry when
  > its reservation aborts, by the live path and by `ASSERT_ROLLBACK` replay
  > alike, and the linkage scan skips a marked entry. Nothing shrinks and no
  > width moves: AST04 shipped three flags, so bit 3 reads 0 on every entry
  > written before this and "0" means "not aborted", which is true of them.
  >
  > **Why not the other two options.** Letting the fold own linkage and
  > stopping the walk from attaching costs AS6a's own `Unapply` ordering note
  > — a reservation made before a checkpoint and rolled back after it would
  > have no entry to remove, and the mount would fail. Narrowing §5.2 to
  > "holds on a live cabin only" keeps every byte as it is and gives up the
  > proof at the one moment it earns its keep.
  >
  > **The chosen option meets that same ordering note, and the fold is where
  > it is answered.** A mark is durable as soon as the checkpoint that
  > flushed its page completes, so the *other* order — reserved before the
  > checkpoint, rolled back **after** it, page on disk before the crash — has
  > the walk skipping an entry whose `ASSERT_ROLLBACK` is still inside the
  > fold's range. The compensation must happen anyway (the base snapshot was
  > taken while the reservation was live and counts its delta), so
  > `ReplayRollback` **restores a linkage the walk deliberately did not**
  > before calling `Unapply`. `Unapply`'s missing-pair `NotFound` stays a name
  > check for the live abort path, where the pair comes from the transaction's
  > own reservation list; a rebuild is entitled not to have restored it, and
  > treating that as an error failed the whole recovery pass — an assertion
  > left unenforcing after an ordinary crash, which is exactly what RC07
  > exists to prevent.
  > `AssertionRecoverTest.AnAbortAfterTheCheckpointStillCompensatesWithTheMarkOnDisk`
  > pins it and was verified to fail without the restore.
  >
  > **What it costs — measured 2026-08-13, and the first version of this
  > paragraph was wrong.** It claimed commit "was already paying exactly this
  > to clear `kEntryReserved` (§6.2 step 4), so the two halves of the protocol
  > now cost the same". They do not, and the reason is a batching asymmetry
  > that has nothing to do with the flag: `CommitTxn` groups its pending
  > reservations by `(assertion, page)` and pays one page fetch, one `Open`,
  > one WAL record and one `StampPageLsn` per *group*, while `AbortTxn` walks
  > reservations one at a time and pays all four per *reservation*. Since
  > `BoundCabinChainWriter::Append` always appends at the tail, a transaction's
  > K entries share a page whatever their `GROUP BY` values — so the two costs
  > coincide only at K=1, and diverge as 1/K thereafter.
  >
  > `bench/results-assertion-abort.md` at `2199780`: per-reservation protocol
  > cost is flat for abort and a 1/K curve for commit (K=1, 0.200 µs against
  > 0.500; K=16, 0.350 against 0.106), so aborting a 16-reservation transaction
  > costs 5.6 µs against commit's 1.7. **The asymmetry predates AS6b** — the
  > base binary already paid an `Unapply` and a WAL `Append` per reservation —
  > and this flag widened it rather than created it. AS6b's own increment is
  > **0.056 µs per reservation**, first clearing the noise floor at K=8 and
  > honestly indistinguishable from zero below that.
  >
  > Closing the gap is **not** blocked by the page write, which is already one
  > named method: it is blocked by `ASSERT_ROLLBACK` carrying one group key per
  > record where `ASSERT_COMMIT` takes a repeated-index list. Batching abort
  > therefore means moving a WAL payload — a `docs/spec/wal.md` §4.1 decision, and
  > cheapest taken while the segment format version has just moved to 2.

- Verification: an offline/maintenance check may re-sum entries against group
  headers (hooks into the integrity sweep of the testing harness, S-1).

Write amplification budget: one 32 B entry + one small group-delta WAL record
per checked write. Documented as an accepted product cost of enabling an
assertion on a relation.

---

## 8. Lifecycle and catalog (AS7, AS10)

### 8.1 CREATE

> **[AMENDED at AST06 (2026-08-09).]** The build runs **synchronously
> inside the CREATE statement**, not in a background scheduling group: the
> engine has no suspendable statement path (crosscore.md P4), and the index
> backfill set the precedent. On a cooperative core this means no write can
> interleave with the build, so §8.1a's membership protocol is met
> trivially; it remains the decided correctness story for when the build
> learns to yield. A row written by a transaction still in flight when the
> build reads it refuses the CREATE with `TxnConflict`, retryably —
> counting it and losing the abort would overstate the group forever, and
> skipping it and seeing the commit would understate it.

> **[AMENDED at PW1c-6c (2026-08-26).]** The three steps below are three
> entry points, because on a multi-core instance they do not all run on the
> same core: `PrepareAssertionDef` (validation and the id) and the publish
> are core 0's, the catalog having one writer, and **the build is the
> relation owner's** (§6.1). One consequence is visible in the order: AS6a's
> base is logged at the end of the *build* rather than after the publish,
> because the owner cannot see core 0's row and must reply before it exists.
> What that costs is an `ASSERT_SNAPSHOT` for an assertion whose publish then
> fails — a base for a cabin no catalog row names, which no mount folds,
> since a mount folds only what `ListAssertions` returns. The single-core
> path takes the same order rather than keeping its own.

1. Create-time validation (§3.1).
2. Full scan of the target relation on its home core (background-group
   scheduled; cooperative yielding per the scheduler contract).
3. Build Bound Cabin entries and group aggregates; emit `ASSERT_BUILD` WAL.
4. If any group violates the bound ⇒ CREATE fails with `AssertionViolation`
   naming the first violating group; the partial build is discarded.
5. Writes to the relation admitted during the build are handled by the
   builder's cutover protocol. The normative requirement is unchanged — at
   success the Bound Cabin exactly reflects all admitted rows, and enforcement
   begins atomically at cutover — and the scheme is now decided (§8.1a).

### 8.1a Cutover: the membership-check protocol (decided 2026-08-08)

**A row counts as incorporated if and only if its pk is present in the Bound
Cabin.** Not inferred from pk ordering, not from scan position, not from a
watermark. The Cabin is the sole source of truth about its own contents.

*What this replaces.* The earlier sketch was a pk watermark: the builder
advances a high-water mark as it scans, and a concurrent write decides
"already scanned" by testing `pk <= watermark`. That predicate is invalid
here. It needs Keystone pk issuance to be monotonic in a way the engine
refuses to promise — `keystoneid-invariant.md` K3 is titled "No density
promise" and states that ordering is *provided but not promised*, precisely so
that no correctness argument may be built on it. A cutover resting on it would
be a fifth subsystem depending on a property K3 exists to withhold.

*Why membership is stronger than a fix.* It removes the external assumption
rather than repairing it. Correctness reduces to **check-then-apply
atomicity** — classify the row, then apply its delta, with nothing in between
— and the home core's cooperative event loop provides that for free: both
happen inside one uninterruptible step, which is the same property AS4's
admission protocol already rests on (§6.1). No new mechanism is introduced.

*What follows from it.*

- **Builder scan order becomes correctness-irrelevant.** Plain page order is
  enough; the builder needs no ordering guarantee from the storage layer and
  imposes none.
- **Build-time write deltas apply at commit time**, which keeps undo
  integration out of the build phase entirely — the abort path during a build
  is the ordinary one, not a second protocol.
- **Membership lookups cost something, but only while building.** For a
  `COUNT` assertion the per-group entry set is bounded by the assertion's own
  bound, so the lookup is small by construction. For a `SUM` assertion it is
  not bounded, and a build-scoped temporary pk hash set is the obvious
  remedy — **reserved as a measured optimization, not a v1 default.** Do not
  build it before there is a measurement that asks for it.
- **Publish stays the single commit point.** Final validation, the
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
| flags | u32 | reserved (deferrable/not-valid future bits) |

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

Documented, truthful limits — all violations of these produce `Unsupported`
or a create-time error, never silent degradation:

- Predicates: `COUNT(*)` and `SUM(int64 col)` upper bounds only (AS1, AS11).
- Single relation per assertion (AS8). Multi-relation assertions are blocked
  on cross-core write / 2PC and are explicitly reserved.
- Statement-time enforcement only; no deferred mode (AS3).
- Bounds are non-negative integer literals.
- No `HAVING`-style filtered groups, no `WHERE`-scoped (partial) assertions
  in v1 (grammar-level future extension; interacts with the abandoned
  FilteredIndex direction of Cabin v1 and must be re-decided deliberately).
- `AVG`, `MIN`, `MAX` bounds: out of scope (MIN/MAX are not incrementally
  maintainable under deletion without extra structure).
- uint64 SUM: `Unsupported` (AG3 parity).
- Assertions on relations with Waystone/pattern features remain fully
  compatible: Bound Cabin is an independent instance and does not alter
  Observational Cabin, trail, or pattern behavior.
