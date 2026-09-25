# CN-6 — Concept Note: In-Engine Cache Relations — a typed, unlogged, TTL-bearing relation under the ordinary MVCC, read by `GET` inside SQL

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §8.** Ordering
relative to CN-1 to CN-5 is not fixed; AT (M3 Uniformity) is the active
milestone and this note does not touch its critical path.
Author: CLA, 2026-09-25, against `9db070b`
Origin: operator's request of 2026-09-25 to write up the cache-layer
integration as a note, after the operator ruled on five points in
conversation (§0). Those five are the only `[operator]` claims here; §8
holds what remains open.
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `9db070b`; everything else `[design]`. Nothing is
`[measured]`. §1's survey of other products is from CLA's training
knowledge, tagged `[design]` because it is not a source read of this tree,
and is to be re-checked against each vendor's current documentation
before a work order cites it.
Relation to `docs/spec/protocol.md`: this note adds nothing to KWP/1 and
reserves no frame, bit or code.

---

## 0. The concept, and the operator's five rulings

`[operator]` A cache layer combined with the relational engine, so that a
cache lookup can stand where a scalar stands in SQL:

```sql
SELECT * FROM users WHERE session_id = (GET user:1:session);
```

`[operator]` The cache is **inside the engine**, not an external store the
engine calls. On that footing the operator ruled, 2026-09-25:

| # | ruling |
|---|---|
| R1 | A cache is a relation under **the same MVCC as an ordinary btree relation; only the WAL is omitted** |
| R2 | TTL expiry is judged against **a time fixed when the snapshot is taken**, not the wall clock at each read |
| R3 | **No forced eviction.** The cache layer is a pool with a configured maximum size; nothing is thrown out to make room |
| R4 | **A committed `SET` may be lost after a crash.** This is the stated contract, not a gap |
| R5 | The value-typing problem is closed by making a cache **a typed relation**, declared with a schema |

`[design]` Everything below is what those five rulings imply on this tree,
and what they leave to decide.

## 1. What other systems ship

`[design]` (see the header on this section's tag). The question the
operator asked in conversation — *is there a precedent for fusing a cache
layer into the RDB, and is it worth doing?* — is answered here rather than
in a separate file.

| system | shape | what it has of R1–R5 | what it lacks |
|---|---|---|---|
| SQL Server 2014+ In-Memory OLTP (Hekaton), `DURABILITY = SCHEMA_ONLY` | memory-optimized table whose schema persists and whose rows do not | **the closest precedent**: multi-version, non-logged rows, typed schema, empty after restart, marketed explicitly for session state and staging | no TTL; a separate engine and compiler inside the product, not the ordinary table path |
| PostgreSQL `UNLOGGED` tables | ordinary heap table with WAL skipped | ordinary MVCC, typed, truncated at crash recovery | no TTL, no size bound, still replicates nothing |
| MySQL `MEMORY` engine | table in memory | typed, gone at restart | table-level locks, no MVCC — a reader can see a writer's half |
| Oracle TimesTen / IMDB Cache | a second database in front of Oracle | typed SQL cache with write-through | a separate product and a separate transaction domain; the two-store problem this note removes |
| Redis, Memcached, Dragonfly | external key-value store | TTL, size bound, eviction | no transactions with the RDB, untyped bytes, dual-write inconsistency is the application's |
| Cassandra, DynamoDB | TTL on a cell / item | TTL as a first-class attribute | TTL judged at query time, not snapshot time; no cross-item MVCC |
| ReadySet, ProxySQL / Heimdall query caches | a cache of query results in front of the RDB | transparent to SQL | caches results, not application-chosen keys; staleness is by invalidation, not TTL |

Three lessons carry into §3. Hekaton's `SCHEMA_ONLY` shows the combination
"typed + MVCC + not logged + lost at restart" is a shipped, named
durability class, so R1 and R4 together are not a novelty. No surveyed
system judges TTL against a snapshot; every one that has TTL judges it at
query time, and none of those has snapshot isolation to protect — R2 is
the one genuinely new composite here, and it is what makes a `GET`
repeatable inside a transaction. And every external cache in the table
exists to be shared by many application servers; an in-engine cache is
shared by every client of the instance, which is the same population, so
the reason to put the cache outside the database does not survive putting
the database in front of it.

`[design]` **Is it worth it.** The value is not speed. It is that one
transaction can write `users` and the session key together, and one
statement can read both under one snapshot, so the two failure modes of
an external cache — a stale key pointing at a row the reader cannot see,
and a dual write that half-fails — cannot occur. The cost is that memory
held by an old version is released only when the oldest snapshot moves
past it (§3), which an external cache never pays. For a financial OLTP
client whose "cache" is session and idempotency state, the first is worth
the second; for a general web cache of bulk read-mostly data it is not,
and this note does not claim that use.

## 2. What this tree has

`[source-read]` unless marked.

- **Every data mutation is logged.** `docs/spec/wal.md:5` states it as the
  status line, and the one unlogged page class is Waystone, whose
  *advisory contract makes an empty structure correct* (`wal.md:39`). So
  R1's "WAL omitted" has one precedent in the tree, and that precedent
  carries exactly R4's argument: the structure is correct when empty.
  Catalog writes are unlogged too, "the pre-RV3 engine exactly"
  (`include/kds/catalog/catalog.hpp:182`), which matters for §3's DDL.
- **The read view carries no clock.** `ReadView` is a POD of
  `snapshot_lsn`, `own_trx_id`, a visibility pointer and two flags
  (`include/kds/txn/read_view.hpp:69-105`); its predicate `Visible()`
  answers by trx id, floor and commit LSN alone (`read_view.hpp:106-118`).
  R2 needs a time fixed at the mint, and nothing in the view holds one
  today.
- **Purge is bounded by the oldest live snapshot.** The manager publishes
  its oldest live `snapshot_lsn` per core *and a purge consults the
  minimum over cores* (`read_view.hpp:53-54`). This is the mechanism R3's
  pool and R2's expiry both hang from: a superseded or expired cache
  version is reclaimable only when that minimum passes it.
- **A scalar subquery in predicate position already exists.**
  `PredicateKind::kCompareSubquery` is `col op (SELECT ...)`, with more
  than one row a runtime `CardinalityViolation`
  (`include/kds/parser/ast.hpp:199-201`); `kInSubquery`, `kNotInSubquery`,
  `kExists`, `kNotExists` sit beside it (`ast.hpp:202-205`). An
  uncorrelated sub-chain's answer *is the same for every outer row* and it
  is hoisted ahead of the step that uses it (`src/exec/step_vm.cpp:2930`,
  `src/exec/plan_printer.cpp:243`, `src/exec/step_compiler.cpp:1661`).
  `GET k` is, at the executor, an uncorrelated `kCompareSubquery` over a
  one-row relation; §3 builds on that and adds no new sub-chain shape.
- **Types are a closed, confirmed list** with compile-time literal
  coercion (`docs/spec/types.md:26-160`); a typed relation is the ordinary
  case and an untyped one does not exist. R5 is therefore not an addition
  to the type system but a refusal to add a bytes-only relation.
- **Memory has one budget.** `buffer_pool_frames` is an undivided instance
  total, `0 = unbounded`, and the sweep runs inline on the fault path when
  residency passes it (`docs/spec/page.md:166`, `:195`;
  `kds.conf.sample:85`). R3's pool is a second budget, or a reserved
  slice of this one (O4).
- **DDL surface today**: `CREATE TABLE`, `CREATE NAMESPACE`, `CREATE
  INDEX`, `CREATE ASSERTION`, `CREATE CABIN` (`manual/sql/sql.md:19`), with
  `BTREE` as the storage word (`sql.md:39`). Heap relations are suspended
  (SUS-1), so a cache relation is a btree relation or nothing.

## 3. The mechanism

`[design]`

- **The relation.** A cache is a btree relation with a declared schema:
  one key column (the btree key), value columns of ordinary types, and a
  system column `expire_at` the user does not name. It lives in the
  catalog like any relation, so DDL, `DROP`, namespaces and the catalog
  views apply unchanged. Its pages are a page class of their own so that
  `page.md`'s flush gate never writes them and recovery never reads them
  (Waystone's position, `wal.md:39`, with a header this time — the class
  still needs `page_lsn`-free visibility bookkeeping, O5).
- **Visibility = MVCC ∧ not expired.** A version is visible to a view iff
  `ReadView::Visible(trx_id)` holds **and** `expire_at > view.mint_time`,
  where `mint_time` is a wall-clock stamp taken once at the mint (R2).
  Expiry never changes an answer inside a transaction; two `GET`s of the
  same key in one transaction agree. Expiry is not a delete: no
  transaction writes it, no undo record describes it. The version simply
  stops being visible to views minted after `expire_at`, and purge
  reclaims it once the oldest live snapshot's `mint_time` passes it — the
  same watermark rule as a superseded version (`read_view.hpp:53-54`).
- **`SET` is a write.** `SET cache[key] = (v1, v2) [TTL n]` is an upsert
  under the ordinary write path: a new version, an undo chain, a lock on
  the tuple, commit or abort with the transaction. What is omitted is the
  WAL append and the flush gate. A `SET` therefore rolls back with its
  transaction, is invisible until commit, and is lost at crash (R4). TTL
  defaults per relation and may be given per `SET`.
- **`GET` is a read.** `GET cache[key]` in scalar position compiles to
  the existing uncorrelated `kCompareSubquery` over the cache relation,
  hoisted once per statement (`step_vm.cpp:2930`), so the outer predicate
  binds a runtime value and takes the index path it would take for a
  literal. A miss is `NULL`; `col = NULL` yields no rows, as it does
  today. A read-through form is O7, not this note.
- **The pool (R3).** Each cache relation, or the cache layer as a whole
  (O4), has a byte ceiling. A `SET` that would allocate past it is
  **refused as a resource refusal**, retryable, naming the ceiling; no
  key is evicted to admit it. Because reclamation waits on the oldest
  snapshot, a long transaction can hold the pool at its ceiling while
  every `SET` is refused — a consequence stated, not hidden (§7).
- **Recovery.** At mount the cache pages are discarded and every cache
  relation is empty; the catalog rows describing the relations survive
  because the catalog is its own (unlogged, checkpointed) structure
  (`catalog.hpp:182`). `SHOW META` reports the cache as reset. No redo,
  no undo, no repair.
- **Surface, as a sketch.**

```sql
CREATE CACHE session_cache (key varchar, value int64) TTL 3600 SIZE 256mb;
SET session_cache['user:1:session'] = 4711;
SELECT * FROM users WHERE session_id = GET session_cache['user:1:session'];
DROP CACHE session_cache;
```

  The key is a relation-qualified subscript rather than a bare
  `user:1:session` token, so the parser sees an expression and a
  parameter can stand in it; O1 fixes the spelling.

## 4. Placement against existing structures

`[design]`

- **The read view gains one field**: `mint_time`. It stays a POD; the
  stamp is taken beside `snapshot_lsn` at the mint. No cache relation
  present ⇒ the field is stamped and never read, which is the zero
  single-core overhead rule's shape. Whether it is stamped unconditionally
  or only when a cache relation exists in the catalog is O3.
- **The page class list** (`page.md` §1) gains a headered, unlogged,
  never-flushed class. The flush gate's `durable_lsn() ≥ page_lsn` test
  is bypassed by class, not by a null WAL, so the `wal` null = unlogged
  test path (`include/kds/exec/index_ddl.hpp:88`) is not what carries
  this.
- **Purge** learns one more predicate: an expired version is reclaimable
  by the same watermark that reclaims a superseded one. No new pass.
- **The buffer pool.** A cache page is resident by construction — there
  is nowhere to evict it to. So it is either outside `buffer_pool_frames`
  with its own ceiling (O4a) or a pinned resident class inside it (O4b),
  the Bound Cabin's position (`docs/spec/assertion.md` §5).
- **CN-5** (Commit Outcome): a cache relation is the natural home for
  neither the token map nor its burn log — both must survive a crash,
  which R4 forbids. The two are orthogonal.
- **CN-3** (Supersede): a cache tuple is never superseded and never
  chained; the cache relation is excluded from CN-3's surface.

## 5. Durability

`[design]` R4 in full: a `SET` acknowledged in a `strict` or `group`
transaction is durable **with respect to concurrent readers** — it is
visible to every view minted after its commit — and **not durable across
a crash**. The transaction it rode in is durable; the cache half of it is
not. A client that needs the cache half back after a mount repopulates
it. This asymmetry is what §1's `SCHEMA_ONLY` precedent also ships, and
it is stated in the relation's own DDL by the word `CACHE` rather than by
a durability class on the transaction. `relaxed` changes nothing here.

## 6. What this note does not license

- Any grammar token, page class, catalog relation, config key or
  `SHOW META` line.
- Read-through (`GET ... OR LOAD`), write-behind, or any automatic fill.
- Eviction of any kind. R3 forbids it; a future note that wants LRU
  reopens R3.
- Replication of cache relations, or any statement about a replica's
  `GET`. There is no replica today.
- Cross-instance sharing. A cache relation is this instance's.
- A claim about cost: version-chain growth under overwrite-heavy keys,
  the purge's extra predicate, and the stamp at the mint have not been
  measured, and no number is set here (Guideline 2).

## 7. Known consequences

`[design]`

- **Overwrite-heavy keys build chains.** Every `SET` on a hot key is a
  version behind the previous one until the oldest snapshot passes it.
  This is the price of R1 and the thing to measure first if the note is
  taken up (§8, O9).
- **A long transaction can freeze the pool.** With R3 there is no relief
  valve: when reclamation waits on one old snapshot, the ceiling is
  reached and stays reached. AN-R14's transaction-life ceiling is not
  enforced today (`docs/inflight/known-gaps.md`); this note makes that
  gap a cache-availability gap as well.
- **A `GET` in a write statement is fine; a `GET` in a WAL record is
  not.** `UPDATE ... WHERE x = GET c[k]` logs the rows it changed, never
  the `GET`, so replay is unaffected. Nothing here needs the resolved
  value in the log.
- **Expiry can precede commit.** A transaction that `SET`s with a short
  TTL and commits after `expire_at` has committed a version no later view
  will ever see. Correct, and worth a manual line.

## 8. Operator decisions this note leaves open

Each carries CLA's proposal, none ratified.

| # | question | CLA's proposal |
|---|---|---|
| O1 | Surface spelling | **`GET cache[key]` / `SET cache[key] = (...)`** with the relation named, rather than a bare `GET user:1:session` — the key is then an expression and takes a parameter |
| O2 | DDL word | **`CREATE CACHE`**, a relation kind of its own, rather than `CREATE TABLE ... CACHE`: the word carries R4 where a table option would hide it |
| O3 | When `mint_time` is stamped | **Unconditionally at every mint.** One clock read per view; a catalog check to avoid it costs about the same and adds a branch |
| O4 | Where the pool lives | **(a) Outside `buffer_pool_frames`, its own ceiling per cache relation**, summed as a cache-layer total in `SHOW META`. (b), a pinned resident class, would let cache growth starve data pages of frames |
| O5 | Page class | **Headered but `page_lsn`-less**: keeps the checksum and the per-frame latch word, drops the flush gate by class |
| O6 | Pool-full behaviour | **Retryable resource refusal on the `SET`**, naming the relation and ceiling; never an abort of the enclosing transaction by the engine |
| O7 | Read-through | **Deferred.** `GET ... OR LOAD (subquery) TTL n` makes a `SELECT` write; its interaction with read-only views is its own note |
| O8 | Key type | **`varchar` only in v1**, as the btree key; int keys are a later widening |
| O9 | Measurement gate | **Before any stage opens**: an AS-E cell of `SET`-heavy overwrite on a handful of keys under one long-lived reader, reporting chain depth and pool residency, against a no-cache baseline |
| O10 | TTL granularity | **Seconds**, integer, per relation with a per-`SET` override; sub-second TTL has no consumer |
| O11 | `DROP CACHE` and `TRUNCATE` | **Both immediate and unlogged**, like the catalog write they are; a `TRUNCATE` is the one sanctioned way to free the pool early |
