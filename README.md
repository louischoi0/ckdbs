# KDS

**A fast, reliable database system specialized for OLTP.**

KDS does not try to be everything a traditional RDBMS is. It deliberately narrows its feature surface to what online transaction processing actually needs — point lookups, short transactions, equi-join chains, tight tail latency, real durability — and delivers those extremely fast.

That shape is not domain-specific: it is what an e-commerce checkout, a banking ledger, a payment or insurance policy-administration system, an identity and user-management service, and an inventory or reservation system all have in common. Each is a system whose truth is *living data*, read and written by short, keyed transactions, thousands of times a second, where a wrong answer is worse than a slow one. KDS is built for exactly that class of workload, and for nothing else.

In exchange for that focus, KDS does two things most databases don't:

> **KDS treats physical data placement as a first-class optimization target.**
> Alongside the query optimizer every database has, KDS has a **physical optimizer** of equal rank. Runtime access statistics do not merely steer query plans — they decide *what structures exist and where bytes live*: which columns earn a lookup structure, when one has stopped paying for itself, and how the pages a workload touches become fewer, denser, and hotter in cache. The engine observes its own workload and reshapes storage under it. See [The Physical Optimizer](#the-physical-optimizer).

> **KDS indexes query patterns, not relations.**
> The index you did not create: the engine fingerprints every statement's shape at parse time, watches which pattern instances recur, and records **Waystone trails** — where each recurring `pattern(args)` actually found its rows, across every relation it touched. `CREATE INDEX` exists for the searches a trail may never replace; the recurring lookups teach the database how to serve them for free.

---

## Design Philosophy

- **OLTP-first, not general-purpose.** Primary-key point reads/writes, inner equi-join chains, predicate-position subqueries, pagination, short transactions. Every statement compiles to a **step chain** executed in written order — *the query is the plan*: execution shape is classified at parse time and dispatched without plan search. No CTEs, no window functions, no dialect-compatibility shims.
- **The engine observes, then reorganizes.** Executions feed a statistics layer. Time-decayed scores classify data and query shapes hot/warm/cold; the physical optimizer creates and retires lookup structures, and (behind named gates) clusters hot tuples and compacts dead ones. Statistics change *what exists* and *where bytes live*, not just how a query runs.
- **Advisory by construction.** Everything learned — trails, scores, placement hints, engine-created Cabins — is structurally advisory or value-granularly revocable: delete all of it and every query still returns the same rows, just slower. The B+ tree stays the sole unconditional authority. This is a hard invariant with its own test family, not a design preference.
- **Reliability is a product feature.** Write-ahead logging with per-transaction durability classes, page checksums, full-page-image torn-write recovery, and fault paths exercised by deterministic injection rather than assumed. What is *not* built is stated as plainly as what is — see [Status](#status).
- **Declarative group constraints.** SQL-92 `CREATE ASSERTION`, restricted to the class that can be checked incrementally — `COUNT(*)`/`SUM(col)` upper bounds per group — enforced lock-free at admission time on the relation's home core. A violating write is refused before it happens; the check is O(1) against a running aggregate, never a re-evaluation.
- **Mechanical sympathy everywhere.** Thread-per-core, shared-nothing: each core owns its data, its buffer pool, and its WAL stream. The buffer-cache hit path is a hash probe and an integer increment — no locks, no atomics. All I/O is asynchronous behind an injectable seam, so the entire engine runs under deterministic simulation.
- **Truthfulness beats convenience.** Every refusal carries the byte position of the offending token. `Unsupported` means "understood and declined"; `InvalidArgument` means "simply wrong". The engine never accepts a spelling and enforces something other than what was written.

---

## Concepts

KDS names its own structures because they are not the standard ones. Each has a stated purpose, a stated authority class, and a set of self-imposed constraints that its correctness argument depends on. The constraints are the interesting part: every one of them is a capability deliberately given up in exchange for a guarantee.

### The trust ladder

Four structures share the access path. What separates them is not speed — it is **how much they are allowed to be believed**.

| Structure | Authority | Write cost | Droppable | Failure of an entry means |
|---|---|---|---|---|
| Clustered pk B+ tree | Always authoritative | Per-write maintenance | Never | Corruption |
| Secondary index | Authoritative, full coverage | Per-write append | Wholesale (`DROP INDEX`) | Statement failure |
| **Cabin** | **Authoritative for observed values** | Probe + append, observed values only | Per value | Un-observe → scan |
| **Waystone trail** | **Never authoritative (advisory)** | Zero on the write path | Wholesale | A miss → authoritative path |

Every layer degrades independently, and correctness never depends on any of the bottom three. A location hint goes stale → pk descent. A value is evicted → scan. A trail is dropped → descent. This ladder is why the engine can learn aggressively: the blast radius of a wrong guess is performance, by construction.

### Keystone — the identity of a row

**Purpose.** Give every tuple one immutable, forever-unique name, so that any structure outside the tuple can refer to it by value instead of by address.

Every tuple's first column is a mandatory 64-bit **Keystone word**: `id:40 | flags:8 | reserved:16`. The 40-bit id is the primary key (≈1.1 × 10¹² ids per relation); the flags byte is the transaction/status byte (the lock-slot role `xmax` plays elsewhere); the 16 reserved bits are written 0 and ignored.

Self-imposed constraints:

- **The pk is unique for the life of the relation, and where it comes from is the `INSERT`'s choice** (`docs/spec/heap-and-tuple.md` §4.1, per-relation key mode built 2026-08-11 and removed 2026-08-25). Name a value in the pk's position and that value *is* the key; omit it and the engine issues one from `sys.tables.next_id`. Both work on every relation, row by row. The cursor is *persistent*, never derived as `max(id) + 1` — deriving it would reissue the identity of a deleted row — and it is a **high-water mark on what has been placed**, so an issued id always clears every key the caller has named.
- **A named key at or above the mark ascends; below it, only a btree relation takes one.** The two prove uniqueness differently, and that is the whole difference: the mark proves it without reading a page, the clustered btree's descent proves it by landing on the one leaf that may hold the key. So a semi-sorted heap chain — which has no descent — refuses a key below its mark, which is exactly the ascent its tail append, its page-wise ordering and its tail-page-only duplicate check all rest on.
- **The pk cannot be updated.** It is the tuple's identity, not a field of it; an `UPDATE` naming it is refused at compile time as `Unsupported`, with the byte position of the column. Naming a key at insert and changing one afterwards are unrelated permissions; only the first was granted.
- **The pk is stored once.** Carried only by the Keystone word, never also as a body column — two copies are how two copies come to disagree. A supplied id is written in the statement's first position and still lands only there.
- **Ids are unique, not gapless.** A failed insert burns one; an explicit relation may skip a range outright. Nothing depends on gaplessness.
- **The word is atomic; the encoding is manual.** Read and written as an atomic `uint64_t` (CAS), and encoded with explicit shift/mask helpers — compiler bitfields are forbidden in any persisted format, whose layout must be identical on every architecture.

What this buys is the **issue-once contract**: a stored id may *dangle* (its row aborted or purged), but it can never *mis-attribute* — no future tuple can inherit it. That single property is what lets Cabins and indexes store pks instead of addresses, which in turn is what makes the physical optimizer able to move pages without notifying anyone.

### The semi-sorted heap — ordering between pages, not within them

**Purpose.** Get range pruning without paying for a sort, and get lock-free readers without a coordination protocol.

Pages are 8192 bytes. Each heap page header carries an **immutable `min_key`** fixed at creation. Tuples inside a page are unordered (append at O(1)); pages along the chain are ordered by `min_key`. So a reader can skip whole pages by key range **without taking a lock** — the immutability is what makes the decision safe to make without one.

Self-imposed constraints: `min_key` is never mutated (re-partitioning means new pages, then retire the old); no tuple with `id < min_key` may ever be placed in a page, *including transiently during relayout*; chain growth is tail-append only, never a split; there is no free-space reuse — a delete-heavy relation grows monotonically until a purge pass exists. Walks are bounded at `kMaxChainPages` (2²⁰ pages, 8 GiB per relation), and exceeding it is `Corruption` rather than a hang.

A relation is stored either as this heap chain or as a **clustered B+ tree** on the pk, chosen at `CREATE TABLE` and by nothing else. The tree is what admits a caller-named key that sorts *below* keys already placed, because only a descent can place and prove one. A btree leaf *is* a heap page — same slots, same tuple format, same MVCC header — so the tree is a directory over the heap, not a second storage engine. The tree is the one place a page's tuples divide: an id sorting inside a full leaf moves the upper half out to a new leaf, the old leaf keeping its `min_key` and the new one taking the split key, so both invariants above survive the move.

### The fixed-length rule — tuples that never migrate

**Purpose.** Make a tuple's address stable for its entire life, so that recorded locations stay meaningful and relayout is a `memcpy` with exact arithmetic.

**Every tuple is fixed-length.** A relation's row size is a schema constant. Every variable-width value (`TEXT`, future blobs) occupies exactly one **tagged cell** of `kds.inline_cell_width` bytes (default 64), whatever it holds:

| Tag | Layout | Meaning |
|---|---|---|
| `kNull` | zeros | SQL NULL |
| `kInline` | `len u16` + bytes + padding | fits in the cell |
| `kSpilled` | `len u32` + `varheap_ptr u64` | bytes live in the var-heap |

The consequence is the point: **an UPDATE can never migrate a tuple.** In conventional engines a growing row is what breaks HOT chains and forces row migration; here the spill decision changes a cell's *tag*, never the tuple's size. The accepted costs are stated rather than hidden: padding is spent on short values, and variable-length management is *relocated* into the var-heap, not eliminated.

The **var-heap** is deliberately boring: values are appended, **immutable per version**, and never moved — so an old-version reader follows an old pointer to bytes that cannot have changed, MVCC correctness is free, and pointers need no epoch or forwarding. It is authoritative data: logged, headered, checksummed. Advisory rules do not apply to it. `kds.inline_cell_width` is pinned into the superblock at bootstrap and validated at every start; a disagreement refuses to boot, naming both values, because on-disk layout depends on it.

A tuple whose length disagrees with its relation's constant is `Corruption` — never interpreted.

### Step chains — the query is the plan

**Purpose.** Remove plan search from the statement path, and make a statement's cost predictable enough to be a *stable* number the optimizer can reason about.

Every statement compiles at parse time into an ordered list of steps — lookups, probes, ranges, scans, nested sub-chains for subqueries — executed in written order. The parser parameterizes literals as it lexes (so the pattern fingerprint comes out of the parse for free), tags the statement with an execution class, and binds catalog names to oids. The executor never re-analyzes shape and never resolves a name.

The self-imposed constraint is real and its cost is measured: **a `pattern_id` names a plan forever.** KDS chooses the same plan at every cardinality, so it cannot decline its own index on a small relation the way a cost-based planner does — and the benchmarks show exactly one cell where that loses (an 11% loss on a 200-row range). That trade is deliberate: a stable plan is what makes a recorded trail replayable and a workload's cost profile learnable.

### Waystone — the recorded trail of a repeated query

**Purpose.** Serve a recurring query from where the last execution *found* its rows, instead of searching for them again — across every relation the statement touched.

A *pattern* is a statement's shape, fingerprinted at parse time (`WHERE id = 42` and `WHERE id = ?` converge on one `pattern_id`). A *pattern instance* is that shape with arguments bound: `(pattern_id, arg_hash)`. From an instance's **second** execution on, KDS records its **trail**: the Keystones of every tuple that execution touched, tagged with the step of the chain that produced each one. Patterns are catalog objects in `sys.patterns` — a durable, inspectable statement of what this database is actually asked to do.

The trust model is one sentence:

> **A trail may replace a lookup. It may never replace a search.**

This is not a preference. A stale entry pointing at the wrong place is caught by the Keystone-id check; a stored *set* missing a row inserted since it was recorded is wrong in a way **no per-tuple validation can detect** — there is no tuple to validate. Absence has no witness. So a keyed step (pk equality, or a chain step probing the next relation by pk) may be served from a trail, because completeness for that step follows from pk uniqueness rather than from the trail. A step that must search still searches; negation (`NOT IN`, `NOT EXISTS`) is search-class by definition.

The per-entry replay contract, in order, all mandatory:

0. **Re-derive the probe key from the current outer row** and require it to equal the entry's pk — built as the lookup key itself, so it cannot be forgotten.
1. Read the tuple at the recorded `(page_id, slot)`; the Keystone id there must equal the entry's pk, and the page must belong to the entry's `rel_oid`.
2. The recorded **page epoch** must match the page's current epoch.
3. Apply MVCC visibility *exactly* as the authoritative path would — free by construction, because a validated location is handed to the same acceptance call a descent feeds.
4. On any miss, fall through to the authoritative path **for that step alone**.

Other constraints: a trail is **one page (253 entries), never continued** — a trail that would exceed it is not recorded *at all*, because a truncated trail is indistinguishable from a complete one and would serve a partial answer believing it whole. Only lookup-class steps are recorded. Storage and policy live entirely behind a one-method seam outside the executor, which is what keeps the advisory contract structurally enforceable rather than remembered.

What it buys: a three-relation pk join served from a trail is three direct page reads instead of three index descents — and a heap relation gets keyed acceleration for its observed patterns *without carrying a single secondary index*.

### Cabin — authority for the values that were asked for

**Purpose.** Make non-pk equality fast without paying an index's unconditional write cost on values nobody queries.

A Cabin is a per-`(relation, column)` store that tracks the tuples matching *observed* values of that column. Where a secondary index covers every tuple unconditionally, a Cabin covers exactly the values queries have touched — and **only there does it hold authority**:

> **Observed ⇒ complete (superset form).** For every observed value `v` and every active snapshot `S`, `v`'s entry set ⊇ { pk : tuple visible in `S` whose key column equals `v` }. A missing qualifying pk violates authority; a surplus entry never does — surplus is subtracted at read time.

Two corollaries carry the design. An observed value's **empty entry set is an authoritative "no rows"** — a negative answer no advisory structure can give. And **un-observing is always legal**: dropping a value's set returns queries for it to the scan path, a performance loss and never a correctness one. That is what keeps an *authoritative* structure evictable.

Self-imposed constraints:

- **Entries store the pk, never the location** (24 bytes: pk + an advisory page/epoch/slot hint). Authority lives in the id, which under the issue-once contract can dangle but never mis-attribute. This is what makes a Cabin **relocation-invariant** — the physical optimizer moves pages without ever touching one.
- **Every maintenance action is an append; removal is forbidden on the write path.** Not merely unnecessary — *incorrect*, because an older snapshot may still be entitled to match through the undo chain. INSERT appends; DELETE does nothing; an UPDATE `v→v′` appends to `v′` and leaves `v` alone. Pruning is lazy and background.
- **The read does the subtraction**: MVCC visibility, a re-check of the key equality, and seen-set dedup (a `v→v′→v` round trip duplicates an id under append-only maintenance).
- **The hint is verified through one shared verifier**, the same code Waystone replay uses — two verifiers is where the bugs would live. On failure: pk descent, then heal the hint in place.
- **Who may decide is declared per column at `CREATE TABLE`** and fixed for the relation's life: `NO CABIN` (never, by any route), `CABIN AUTO` (the engine may — this is the physical optimizer's jurisdiction), `CABIN` (created now, values observed on *first* selection rather than second, because a declaration is the evidence that waiting exists to gather). A policy on the pk column is **refused**, not ignored — the pk's Cabin is the clustered tree, so any of the three would be a statement about something that cannot exist.
- **Unlogged authoritative** — a new storage class. The completeness promise holds only while the write hook is live, so a restart declares every Cabin fully unobserved and traffic rebuilds it. This keeps the write hook entirely off the WAL path.

Cabin soundness depends on two engine properties and is invalid without either: statements for a relation run **to completion on its owning core** (so *scan + record + mark observed* is atomic against every other statement), and the **issue-once id contract** (so a stale reference cannot be corrupted by reuse).

At the limit, full observation of a column is permitted — at which point a Cabin has become a lazily built secondary index, paid for value by value, each increment individually evictable. **A traditional index is the limit case of a Cabin, not a competing feature.**

### Assertions and the Bound Cabin

**Purpose.** Ship SQL-92 `CREATE ASSERTION` — which no major DBMS enforces — by restricting it to the class that can be checked in O(1).

The two classic blockers are re-evaluation cost and concurrency. KDS removes both by construction rather than by generality: the predicate class is restricted to per-group `COUNT(*)`/`SUM(col)` upper bounds; the incremental state lives in a **Bound Cabin** — the same machinery as an Observational Cabin with the lifecycle contract inverted (eager full coverage, pinned pages, eviction *forbidden*, logged and crash-consistent, 32-byte entries carrying the row's aggregate inline); and concurrency is handled by a reservation protocol on the relation's **home core**, so admission is atomic without latches. No waiting, no retry storm, no deadlock — failure is immediate and deterministic.

An admission check reads **only the group header**: O(1), no entry iteration. The running aggregate is a field of that header rather than a second store, so there is exactly one number per group and no reconciliation problem. Everything outside the supported class is a truthful `Unsupported` with a byte position.

### Transactions, MVCC and the WAL

**Purpose.** Snapshot reads without a reader registry, and durability the caller chooses per transaction.

The per-tuple MVCC header is exactly 20 bytes — `trx_id:48 | undo_ptr | data_len | flags` — and **there is no `xmax`**: a version's death is the next version's birth, and walking the undo chain already names the overwriting transaction, so storing the boundary twice would be recording one fact twice. `DELETE` is a delete-mark plus the deleter's `trx_id`, with bytes left in place for older snapshots.

The WAL is per-core append-only streams: physiological redo plus undo-chain MVCC, full-page images against torn writes, fuzzy checkpoints, and three per-transaction durability classes — `strict` (ack after fsync), `group` (same durability point, batched), `relaxed` (a bounded loss window, for reconstructible data).

The constraint with the widest blast radius: **readers are deliberately not registered.** That buys a read path with no coordination at all, and it costs the engine a reader horizon — which is why nothing purges, why `SnapshotTooOld` is structurally unreachable, and why compaction is gated. The cost is paid knowingly and named wherever it bites.

### Thread-per-core

Each core owns its data, its buffer pool, and its WAL stream. Page latching is **core-local** — a latch serializes cooperative tasks on one core across suspension points; it is not a cross-core lock, and cross-core access goes through server-side forwarding rather than shared-memory locking. There is no canonical in-memory tuple and no identity cache to keep coherent: consistency comes from pin and latch discipline on the page. Tasks are C++20 stackless coroutines, run to completion, in scheduling groups (foreground / system / maintenance) throttled by SLO rather than preempted.

---

## The Physical Optimizer

The feature this project exists for, and the one most directly aimed at query performance. It is **one umbrella over two halves** that share a decay score and touch none of each other's structures: **Part I reshapes where bytes live**; **Part II decides which lookup structures exist.** Part II is complete and measured; Part I is deliberately shadow-only, and *that is a finding, not a hedge*.

### The shared substrate: the lazy-decay score and the page epoch

**The lazy-decay score (R1)** is the engine's one implementation of "how hot is this, now". State is two words — `score` and `last_bump` — and the decayed value at time *t* is `score · 2^(−(t − last_bump) / half_life)`. A touch decays-then-increments; a read decays only; **there is no background decay pass**, so ten thousand cold scores cost nothing for their coldness until something asks. Fixed-point arithmetic only, no floating point on any statement path; the clock is injected, so deterministic tests get exact halving, and with no clock the score degrades honestly to a raw count. Half-life is one instance-wide key (`decay_half_life`, default 600 s) — a second decay formula anywhere in the engine would be a defect.

**The page epoch (R4)** is how an advisory structure survives data moving underneath it. Every page header carries `relayout_epoch` (u64, in the slot the header comment had already nominated — so no format bump: every existing page reads 0). Waystone entries and Cabin hints record the epoch they observed; the shared verifier compares recorded against current, and a mismatch is a per-entry miss with the ordinary fall-through. **Relayout bumps one counter instead of synchronously rewriting every entry that pointed into the page** — that is what stops the advisory layer from turning into a second index that must be maintained.

The pairing rule is part of the decision: **no consumer may accept a location on epoch equality alone.** The epoch is a fast whole-page invalidation layered *over* the Keystone-id check, never a substitute for it.

### Part I — statistics-driven relayout (shadow-only, by design)

`sys.access_stats` records one row per access *shape* — `(kind, rel_id, column_mask)` — keyed by **columns, never values**, which is what bounds it by the schema rather than by the data. The kind split is what makes it worth having: a walk driven by an equality on an unindexed non-pk column is `kFilterScan`, not an undifferentiated `kScan`. The two cost the same and mean entirely different things — *"asked for everything"* versus *"asked for a few rows and had to read all of them to find out which"* — and since the index-kind split, a `kFilterScan` sitting beside an `kIndexProbe` on the same relation names two columns with different treatment. Collection costs +1–2% on a point lookup and is unmeasurable on anything slower.

`SHOW RELAYOUT` is the physical health report: per relation, each access shape with its raw count and its decayed weight, chain length, delete-mark density, and one line per **candidate plan** with predicted benefit (pages-not-touched per execution × decayed shape frequency) and its verdict:

| Plan kind | What it would do | Blocked on |
|---|---|---|
| `compact` | Rewrite the chain dropping delete-marked tuples past the reader horizon | Gate 1 |
| `cluster` | Co-locate a hot set on fewer pages | Gate 2 |
| `defrag` | Rewrite a chain onto contiguous page ids for sequential I/O | Gate 3 |

The planner is **pure and pull-only** — it reads statistics and the catalog, runs when `SHOW RELAYOUT` asks, has no background task and no cadence. Measured idle cost is exactly zero at the noise floor; the report itself prices at ~60 µs + 24 ns/slot.

**The three gates — why nothing moves yet.** The design audited every candidate move against the engine as it stands, and each is blocked by a named, owned constraint:

1. **Reader horizon.** Dropping a delete-marked tuple requires knowing no snapshot can still need it — and readers are deliberately unregistered. The same fact that makes purge impossible makes compaction impossible. A mover that *guesses* a horizon is a correctness bug wearing an optimization's clothes.
2. **Ordered-between compatibility.** Clustering an arbitrary hot id set onto one page satisfies the `min_key` invariant while silently breaking the between-pages ordering that range pruning reads — turning pruning into row loss. Three legal forms exist; choosing one is an open decision the shadow report exists to inform.
3. **Cross-relation page reuse.** Trail validation checks `rel_oid` and the Keystone id at a recorded slot — but ids are issued *per relation*, so a page freed from relation A and reallocated to relation B could hold a colliding id at that slot. Until that is detectable, retired pages are **quarantined** rather than freed: the leak is the honest price, bounded by how much relayout runs.

So v1 applies the engine's own promotion-gate philosophy **to the optimizer itself**: observe, classify, plan, report — enact only when a gate opens, with the shadow report as the evidence that opening it pays. The mover is nonetheless specified structurally, so building it later is filling in a form rather than reopening the design. Its rules, normative for any mover ever written: move a live tuple's bytes **verbatim** (a move is not a version — no undo record, no visibility change); create new pages and retire whole sources, **never** edit a `min_key` in place; hold the `min_key` invariant at every *intermediate* state; bump the epoch on every source and destination before any statement can observe the new layout; log a full-page image of everything it touches (an unlogged relayout is forbidden even while recovery does not exist — a log that names slots a relayout silently moved is a log that lies); run as a maintenance task on the relation's home core; refuse to run while any open transaction's undo trail names addresses in the relation. It may not touch a var-heap page, a catalog page, an undo page, or any trail, Cabin, or index structure — **its entire maintenance surface is the epoch**, which is precisely why the first mover targets heap relations.

`physical_optimizer` takes `off | shadow`, default `shadow`. **`on` is refused at startup, naming the open gates** — a config written for the future fails loudly today instead of silently under-delivering.

### Part II — the Cabin controller (complete)

The half that *is* enacting. A per-core background controller that decides which Observational Cabins should exist, based on nothing but what the workload did. It operates exclusively on advisory-class structures, which is what licenses it to act autonomously: a wrong decision costs performance only — stale hints heal on read, dangling entries are discarded, and a dropped Cabin merely returns the system to its baseline.

**Three input signals, no more.** (S1) fingerprint execution frequency under the shared decay score; (S2) observed pages scanned per execution, from the executor's per-statement counters; (S3) Cabin quality — hint hit/failure counters and coverage misses. Buffer-pool statistics are excluded as relation-granular, too coarse for a per-column, per-value decision.

**A closed action vocabulary.** `CREATE` (build a Cabin for a column), `EXTEND` (widen an existing one's value coverage), `HEAL` (batch re-validate location hints), `DROP` (retire a cold or unhealable one). `REBUILD` is excluded as `DROP` + `CREATE`. **Bound Cabins are permanently outside its jurisdiction** — they belong to assertions, and the controller enumerates only Cabins carrying its own `auto` origin tag.

**Everything in the common currency of page accesses**, fixed-point, no floats:

```
Benefit  B(c) = Σᵢ fᵢ × max(0, P_scan,i − P_cabin)
Cost     C(c) = P_rel / T_amort  +  h_fail × f_lookup × k_heal
```

— decayed frequency × pages saved, against amortized build cost plus healing upkeep. The rules are asymmetric on purpose:

| Action | Condition |
|---|---|
| CREATE | `B > 3 × C` sustained for 3 consecutive snapshots, and the budget admits |
| EXTEND | coverage-miss share > 20%, and the missed share's marginal benefit alone clears the create bar |
| HEAL | hint-failure rate > 10% while the Cabin still pays for itself |
| DECAYING | `B < 0.5 × C` |
| DROP | that condition persists for the cooldown, or a HEAL failed to recover quality |
| recover | `B > 3 × C` again |

The wide gap `θ_drop ≪ 1 ≪ θ_create`, plus the confirm count and cooldown, is the **anti-thrash mechanism**: a Cabin is created only on strong sustained evidence and retired only on strong sustained absence of it. Hysteresis here is load-bearing, not tuning sugar — a raw cost model oscillates.

Two operating decisions worth stating because they were made *from measurement*, in a three-business-day workload with hot value sets rotating nightly:

- **The amortization window is 64 half-lives**, ratified after the day-scenario showed a one-half-life window makes the lifecycle a *nightly rebuild loop* by the model's own arithmetic. The window is one belief read by both sides: raising it lowers the admission bar by the same factor, and the page budget — not the bar — is what bounds the population.
- **The cooldown is its own parameter** (128 half-lives), decoupled once it became clear one number was answering two questions: how long a build is believed to pay for itself, and how much silence proves death. The honest limit is documented rather than tuned away: **a dead Cabin and an overnight-quiet one emit the same signal** — no lookups — so any cooldown below a workload's quiet period retires *live* Cabins, not dead ones.

**Safety properties, all structural.** Decide is a **pure function** from an immutable snapshot to an action set (no I/O, no clock reads, no live-counter access) and Execute is the only effectful phase — which is what makes seed-driven replay through checked-in golden traces a real determinism proof. Every build scan goes through the **scan ring**, mandatory, so the controller can never displace the foreground working set it is trying to serve. Every state transition is a single home-core step. The budget is a solved ranking problem, not a growth valve: an over-budget CREATE is admitted only in *exchange* for dropping the lowest-net-benefit active Cabin, and only if it beats it by a factor. `SET kds.cabin_optimizer = off` is a runtime kill switch that halts new decisions and in-flight builds and **touches nothing that exists** — there is no destructive path on disable.

**Observability is the deliverable, not a nicety.** `SHOW CABIN_OPTIMIZER` reports every managed Cabin with its state, net-benefit score, hint hit rate, coverage, pages, and last action *with the scores that produced it*; `ANALYZE` marks a probe served by an optimizer-managed Cabin. The controller's whole claim is auditable after the fact.

**Measured** (the run is no longer written up in `bench/`; the driver is
`tools/cabin_optimizer_benchmark.py`):

| Question | Answer |
|---|---|
| What does it cost a workload it can do nothing for? | Nothing measurable — the on/off delta sits inside same-configuration noise; the tick is 2–3 µs CPU, under one part per million of a core at the default cadence |
| What is a self-created Cabin worth? | **10.9× client p50, 19× server CPU** at 10,000 rows — created **1.4 s** after switch-on, in exactly the 3 confirm snapshots configured, reply verified byte-identical to the walked one |
| Does it match a human who knew the answer? | Three-business-day workload, day-1 TPS: **608 off, 1,680 controller (2.8×), 1,724 hand-declared** — parity with an operator who declared the right five columns in advance |

Known limits, stated: managed state and the decision log are **memory-resident** — a restart forgets what was managed, and re-observation rebuilds it. The decay score's fixed-point range underflows to zero after ~16 half-lives, so across most of a long cooldown a DROP is a *timeout rather than a judgement*; closing that is an open decision on the score's dynamic range, with consumers beyond this controller. And the key is **off by default**: with `cabin_optimizer = off`, a column declared `CABIN AUTO` behaves exactly as an undeclared one.

---

## Architecture

```
                     clients  ·  KWP binary frames / newline text
                                    │
                        ┌───────────▼───────────┐
                        │   TCP server / KWP    │  frames · sessions · txn control
                        └───────────┬───────────┘
                        ┌───────────▼───────────┐   pattern fingerprint, at parse time
                        │        Parser         │───────────────┐
                        └───────────┬───────────┘               ▼
                        ┌───────────▼───────────┐       ┌────────────────┐
                        │  Executor (step VM)   │◀─────▶│ Waystone trails│  advisory
                        └──┬──────────────┬─────┘ record│ + sys.patterns │
                           │              │      replay └───────┬────────┘
                           │        ┌─────▼──────┐              │
                           │        │   Cabin    │  authoritative for observed values
                           │        └─────┬──────┘              │
                        ┌──▼──────────────▼──────┐              │
                        │ B+ tree (pk) + indexes │◀─ validates ─┘
                        └──────────┬─────────────┘
                                   │              ┌──────────────────────────┐
                                   │   relayout ◀─┤  Physical optimizer      │
                                   ▼    (gated)   │   I · relayout planner   │
                        ┌──────────────────────┐  │  II · Cabin controller ──┼─► create
                        │   Semi-sorted heap   │  └──────────────────────────┘   extend
                        │  min_key pages ·     │   reads sys.access_stats and    heal
                        │  Keystone tuples     │   decayed shape frequencies     drop
                        └──────────┬───────────┘
              ┌────────────────────┼────────────────────┐
   ┌──────────▼──────────┐ ┌───────▼──────────┐ ┌───────▼───────────┐
   │ Buffer pool (1/core)│ │ WAL (1/core)     │ │  Space manager    │
   │ clock · bg writer   │ │ D1/D2/D3 classes │ │  extents · freemap│
   └──────────┬──────────┘ └───────┬──────────┘ └───────┬───────────┘
              └────────────────────┼────────────────────┘
                        ┌──────────▼───────────┐
                        │ Single growable file │  + per-core WAL segment streams
                        └──────────────────────┘
```

## Components

| Component | What it does |
|---|---|
| **KWP wire protocol** | Custom length-prefixed binary protocol: version/capability handshake, extended PARSE/BIND/EXECUTE, chunked result streaming with explicit flow control, per-transaction durability selection, structured retryable errors |
| **Parser** | Small OLTP grammar — joins and predicate-position subqueries included. Parameterizes literals during the parse (pattern fingerprints come out for free), tags each statement with an execution class, binds catalog names to oids at parse time — the executor never re-analyzes shape or resolves names |
| **Executor (step VM)** | Every statement is a **step chain** — lookups, probes, scans, nested sub-chains — run in written order. Replay-eligible steps consult the instance's trail first (validated per entry); the rest run authoritatively. Records trails from an instance's second execution |
| **Waystone** | The trail store: per pattern instance `(pattern_id, arg_hash)`, the recorded Keystones of the rows it touched, with last-seen locations and step tags. Reached through `sys.patterns` and a per-pattern directory. Strictly advisory — droppable wholesale without changing any result |
| **Cabin** | Per-`(relation, column)` value store, authoritative **for observed values only**: entries hold pks (relocation-invariant) plus an advisory location hint. Append-only maintenance, read-time verification, per-value eviction. Declared per column as `NO CABIN` / `CABIN AUTO` / `CABIN` |
| **Semi-sorted heap** | 8 KiB pages with an immutable per-page key lower bound (`min_key`): pages are unordered inside, ordered between — range pruning without full sorting. Each tuple carries a 64-bit **Keystone** word (40-bit id · flags/lock byte · reserved) as its identity |
| **B+ tree** | The authoritative pk → location index. One tree core, thin facades; append-optimized for monotonic engine-issued ids (rightmost fast path, asymmetric splits), and dividing a full leaf at its median key when a caller names one below the relation's high-water mark. Core-local — no latching protocols at all |
| **Secondary indexes** | Multi-column and covering, on clustered relations, for the searches a trail may never replace. Formally "a Cabin that observed everything" — which is why its correctness argument was already proved. Index entries are logged before the heap write they describe |
| **Assertions** | `CREATE ASSERTION` over `COUNT(*)`/`SUM(col)` group upper bounds, enforced at admission on the home core against an O(1) running aggregate held in a pinned, logged **Bound Cabin** |
| **Physical optimizer** | Two halves. **I — relayout** *(shadow-only)*: the decay score, the page epoch live at every validation site, and `SHOW RELAYOUT` reporting every candidate plan with its predicted benefit and the gate blocking it. **II — the Cabin controller** *(complete; `cabin_optimizer`, off by default)*: a per-core background controller that creates, extends, heals and drops advisory Cabins from workload statistics, under a page budget, a kill switch, and a pure fixed-point decision core — with `SHOW CABIN_OPTIMIZER` exposing every decision and the scores behind it |
| **Buffer pool** | One per core over core-owned pages. RAII pinned-page handles, clock eviction, background writer, WAL-ordering gate enforced in code. Hit path: zero locks, zero atomics, zero allocation |
| **WAL** | Per-core append-only streams. Physiological redo + undo-chain MVCC (writer trx-id + undo pointer; no xmax). Durability classes per transaction: `strict` / `group` / `relaxed`. Fuzzy checkpoints, full-page images, point-in-time-recovery-ready archives |
| **Storage** | One growable data file, pure arithmetic page addressing (`offset = page_id × 8 KiB`), extent-based crash-safe growth, bitmap free-space management, CRC32C page checksums. mmap deliberately rejected — explicit async I/O only |
| **Scheduler** | Cooperative reactor pinned per core: C++20 stackless coroutines, run to completion, scheduling groups (foreground / system / maintenance) with SLO-based throttling instead of preemption |
| **Deterministic testing** | Clock, randomness, and all I/O are injected. The whole engine runs single-threaded under a simulated scheduler with crash and torn-write injection — durability claims are tested, not asserted |

## Invariants — the self-imposed constraints

Never violated, never "temporarily" bypassed. Each is a capability given up for a guarantee; the owning spec is `docs/spec/heap-and-tuple.md` §8.

| # | Invariant | Bought |
|---|---|---|
| 1 | 8192-byte pages; `uint32_t` page ids; `0xFFFFFFFF` invalid | Arithmetic addressing, no indirection |
| 2 | A page's `min_key` is immutable after creation | Lock-free range pruning |
| 3 | No tuple with `id < min_key(page)` in that page, ever — including by relayout, including transiently | The pruning decision is always sound |
| 4 | Tuples within a page are unordered | O(1) append |
| 5 | The Keystone column is exactly `id:40 \| flags:8 \| reserved:16` | One word names a row |
| 6 | The Keystone word is atomic `uint64_t`; persisted formats use shift/mask, **never** compiler bitfields | No torn fields; portable on-disk layout |
| 7 | Ids outside the tuple header are zero-extended `uint64_t` | One id representation everywhere |
| 8 | Waystone is advisory: deleting it wholesale may cost performance, never a result | Learning is risk-free |
| 9 | Waystone is never authoritative — it chooses *where to look*, never *what is visible* | Absence needs no witness |
| 10 | No canonical in-memory tuple; consistency is page pin + latch discipline | No coherence cache to keep |
| 11 | Every pk is a unique 40-bit id, carried only by the Keystone word, **never updatable**; the `INSERT` names it or omits it, per row; a named key below the high-water mark is btree-only | Issue-once identity |
| 12 | The MVCC header is exactly 20 bytes and there is **no `xmax`** | One fact stored once |
| 13 | Every tuple is fixed-length; a disagreeing length is `Corruption`, never interpreted | Tuples never migrate |
| 14 | Var-heap values are immutable per version; `kVarHeap` pages are never relocated | MVCC correctness for free |

## Specifications

The design is specification-first: every subsystem has a spec carrying its decisions, its open questions, and its required tests. `docs/spec/heap-and-tuple.md` is authoritative — where it and another document disagree, it wins.

| Subsystem | Spec |
|---|---|
| Row storage, heap, Keystone, invariants *(authoritative)* | `docs/spec/heap-and-tuple.md`, `docs/rules/rule-fixed-length-tuple.md`, `docs/spec/page.md` |
| Pattern-keyed access trails | `docs/spec/waystone-concpets.md`, `docs/spec/create-pattern-user-defined-patterns-v1.md` |
| Value-observed authoritative store | `docs/spec/cabin.md` |
| Physical optimizer (relayout + Cabin controller) | `docs/spec/physical-optimizer.md` |
| Transactions & MVCC | `docs/spec/txn.md` |
| Logging and durability | `docs/spec/wal.md` |
| Query language, step chains, joins, subqueries | `docs/spec/parser-v2.md` |
| Aggregation | `docs/spec/aggregate.md` |
| Secondary indexes | `docs/spec/index.md` |
| Group-level assertions | `docs/spec/assertion.md` |
| Foreign keys | `docs/spec/foreign-keys.md` |
| Types (DATE, TIMESTAMP, DECIMAL, DECIMAL128) | `docs/spec/types.md` |
| Buffer-pool eviction | `docs/spec/eviction.md` |
| Cross-core execution & scheduling | `docs/spec/crosscore.md`, `docs/spec/sched.md` |
| Wire protocol | `docs/spec/protocol.md`, `docs/inflight/in-progress/protocol-wp.md` |
| DDL (`ALTER TABLE`, `DROP TABLE`, bulk insert) | `docs/spec/alter.md`, `docs/spec/drop-table.md`, `docs/spec/bulkinsert.md` |
| Id issue-once contract | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md` |
| C++ rules | `docs/rules/rules.md` |
| **What is missing, and what a restart loses** | **`docs/inflight/known-gaps.md`** |

## Glossary

The stone metaphor is deliberate — a *keystone* holds the structure up, a *waystone* guides the traveler without being the road, and a *cabin* is a place someone has actually been.

| Term | Meaning |
|---|---|
| **Keystone** | The 64-bit identity word every tuple carries: 40-bit id · 8-bit flags/lock byte · 16 reserved bits. Everything that names a tuple names it by its Keystone id |
| **Waystone** | The advisory store of trails, reached per pattern instance through `sys.patterns`. Droppable wholesale without changing any result |
| **Trail** | The recorded path of one pattern instance — the Keystones a previous execution touched, in execution order, with step tags and last-seen locations. A trail may replace a lookup, never a search |
| **Cabin** | A per-`(relation, column)` store, authoritative for the values queries have actually observed; entries hold pks, not addresses. A **Bound Cabin** is the pinned, fully covering, logged variant that backs an assertion |
| **Pattern / pattern instance** | A pattern is a statement's *shape*, fingerprinted at parse time as `pattern_id` (literals parameterized, so inline values and bind parameters converge). An instance is that shape with arguments bound: `(pattern_id, arg_hash)` |
| **Step chain** | The compiled form of every statement: an ordered list of steps executed in written order. "The query is the plan" |
| **Semi-sorted heap / `min_key`** | The heap layout: pages are unordered inside but ordered between, via an immutable per-page key lower bound |
| **Epoch** | A per-page counter bumped whenever its tuples move. Recorded by trail entries and Cabin hints; a mismatch means the location is no longer trusted and the authoritative path runs |
| **Decay score** | The engine's one time-decay implementation: exponential half-life, computed lazily from `{score, last_bump}`, never swept. Consumed by hot/cold classification, trail retention, and the Cabin controller |
| **Advisory** | The invariant class every learned structure belongs to: deleting it may cost performance but can never change a query result. Enforced by a dedicated test family, not by convention |
| **Shadow mode** | The relayout planner's evaluation mode: plans are produced and their predicted benefit reported, but nothing moves. The promotion gate between observing an optimization and enacting it |
| **Durability class** | Per-transaction WAL acknowledgment semantics: `strict` (ack after fsync), `group` (same durability point, batched), `relaxed` (bounded loss window, for reconstructible data) |
| **KWP** | The KDS Wire Protocol: length-prefixed binary frames, version/capability handshake, extended PARSE/BIND/EXECUTE, chunked streaming, per-transaction durability selection |

## Roadmap

The learned layer grows in deliberate steps:

1. **Advisory acceleration** — trails skip descents for recurring patterns; Cabins serve non-pk equality for observed values. *Built.*
2. **Self-managing structures** *(current work)* — the Cabin controller decides which Cabins exist, from measured cost and decayed demand, under a budget and a kill switch. *Built; the relayout half stays shadow-only behind its three named gates.*
3. **Bounded set caching** — with a commit-time change stamp per relation, "nothing changed under this result" becomes provable, and search-class steps join the party *(open design)*.
4. **Hands-off operation** — everything needed to run KDS is exposed as data and levers, not intuition: the workload is inspectable (`sys.patterns`, `SHOW ACCESS`, `SHOW RELAYOUT`, `SHOW CABIN_OPTIMIZER`), every optimization can be evaluated before it acts, and every action is a flag or a threshold with a promotion metric to verify it. The control loop closes without anyone in it; who — or what — sits in the operator seat is deliberately left open.

One property makes the last step sane rather than reckless, and it is structural: everything in the autonomous loop is advisory or value-granularly revocable by invariant, so the worst mistake any operator — scripted, automated, or human — can make through these surfaces costs performance, never correctness.

## What KDS is not

No CTEs or derived tables, no window functions, no cross-dialect SQL compatibility, no attempt to be a data warehouse. `GROUP BY` with `COUNT`/`SUM`/`MIN`/`MAX`/`AVG` is built; `HAVING` and sorted aggregate output are not. Secondary indexes exist (`CREATE INDEX`, multi-column and covering, on clustered relations) for the searches a trail may never replace — an index answers with a *set*, a trail only ever replaces a *lookup*. If your workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data, KDS is built for exactly that.

---

## Benchmarks

Every number below comes from a results file under [`bench/`](bench/), each recording the commit it was measured at, the environment, the binary's provenance, and its own caveats. Nothing is quoted from memory or averaged across runs that used different code. Where a comparison flatters KDS, the file says why; where it does not, that is stated with the same prominence. **Read [Caveats](#caveats--what-these-numbers-do-not-show) before quoting anything.**

### Environment and method

One method, kept by every results file in `bench/`:

| | |
|---|---|
| Host | AWS EC2, AMD EPYC 7571, 2 vCPU, 7 GiB RAM |
| Storage | NVMe-backed EBS volume (ext4/xfs) — never tmpfs, so every fsync is real |
| KDS | `build-release` (`-O3 -DNDEBUG`), newline text protocol, defaults unless stated |
| PostgreSQL | **17.10, packaged build, untuned defaults** (`shared_buffers = 128MB`, `synchronous_commit = on`) |
| Client | Python 3.9, single connection, one request at a time, same harness on both sides |
| Statements | Inline-literal text, parsed per execution **on both sides** — no prepared statements anywhere, because KDS has no PARSE/BIND path yet and giving PostgreSQL one would flatter it |
| Verification | Where a workload has a correct answer, `--verify` compares the two engines' replies row for row before any number is kept |

PostgreSQL is left at stock defaults on purpose: a baseline tuned by hand is not a baseline — and "stock" cuts both ways, which the per-file caveats price. Both engines share the box with the Python client, sequentially, never concurrently. Client-visible latencies carry a ~90–210 µs Python socket floor on both sides.

### Summary against PostgreSQL 17

One line per area. "Verdict" is the results file's own conclusion, not a re-interpretation.

| Area | Workload | KDS | PostgreSQL 17 | Verdict | Source |
|---|---|---|---|---|---|
| pk point access | `WHERE id = <n>`, 50k rows, server p50 | **12 µs** (Waystone) | 74 µs (btree pk) | ~6× faster, both flat in row count | `results-waystone-vs-pg.md` |
| pk-chain join | 2-relation join by pk, client p50 | ~143 µs | ~296–313 µs | ~2.1× faster at every tested size | `results-scenario1-vs-pg.md` |
| `EXISTS` subquery | uncorrelated, hoisted | ~132 µs | ~239 µs | ~1.8× faster, flat | `results-scenario1-vs-pg.md` |
| pk `BETWEEN` range | 200 rows returned | ~388 µs | ~2,254 µs | ~5.8× faster | `results-scenario1-vs-pg.md` |
| Non-pk filter scan | day-slice, 252 → 10,080 rows | 158 → 1,418 µs | 224 → 1,293 µs | **crossover**: KDS wins small, PG wins large | `results-scenario1-vs-pg.md` |
| Non-pk equality matrix | 15 shape×size cells, p50 ratio | 1.14×–1.99× faster | — | KDS ahead in every cell, walk or index | `results-scenario3-library.md` |
| OLTP txn mix, 1 conn | group commit vs `synchronous_commit=on` | 200.8 TPS | 212.9 TPS | within 6%; PG's tail is tighter | `results-latency-matrix.md` |
| OLTP txn mix, 4 conns | same | 213.7 TPS | **491.5 TPS** | **PG 2.3× ahead** — its group commit amortizes across connections, KDS's does not yet | `results-latency-matrix.md` |
| Relaxed durability | D3 loss window vs PG sync=on | 1,610 / 2,875 TPS (1/4 conn) | 212.9 / 491.5 | not the same guarantee — stated, not hidden | `results-latency-matrix.md` |
| Secondary index read | selective equality, 10k rows | 9.7× vs own walk | PG picks the same plan shape | index pays where selectivity does | `results-index.md` |
| Index build | `CREATE INDEX`, 10k rows | 8.68 ms | 4.06 ms | **PG ~2× faster** at scale | `results-index.md` |
| Index INSERT overhead | one index, `relaxed` | +0.6–2.1% | +0.8–1.2% | comparable, both small | `results-index.md` |
| Aggregation scaling | 1 → thousands of groups | +46% | +454% (HashAggregate) | fold cost tracks group count better | `docs/inflight/in-progress/workplan-aggregate-perf.md` |
| Aggregation, no index-only scan | `COUNT(*)` over indexed col | costs a full resolve | PG Index Only Scan ~10% cheaper | honest structural gap, gated on a visibility witness | `results-index.md` §7 |
| Bulk INSERT, durable | 1,000-row `VALUES`, rows/s | **210,165** | 81,400 (`sync=on`) | 2.6× at the widest batch, 1.07× at batch 1 | `results-bulk-insert.md` |
| Whole-transaction booking | 4 reads, ~6.6 writes, FK-checked | **3,663 µs** | 4,072 µs | KDS ahead 10%; the *decomposition* is the finding | `results-scenario2-freight.md` |
| **Physical optimizer** | 3 business days, day-1 TPS | **1,680** (controller) vs 608 (off) | twin in `results/cabinopt-days-pg.json` | **2.8×**, matching hand-declared (1,724) | `results-cabin-optimizer-days.md` |
| Multi-core | 4 isolated relations, cores=1 vs 2 | 1.05× | — | parity, as designed — core 0 serves everything until the pipeline lands | `results-multicore.md` |

### What the comparison actually says

Four findings generalize; everything else is a cell in a table.

**1. KDS's per-statement fixed cost is small and PostgreSQL's is larger; KDS's per-row cost is slightly higher.** So which engine wins is decided by row count, and the crossover sits *inside* the measured range — the day-slice shape fits to KDS ≈ 35.2 µs fixed + 128.1 ns/row (+1.8% fit error). This is why every KDS win above is on a keyed or small-result shape, and the one loss is a 10,080-row filter scan. Joins and ranges stay flat because they are pk-keyed: written order is the plan, each step probes by pk, and reply size is constant by construction.

**2. Where a recorded location replaces a descent, the win is large and expected.** pk point access is ~6× PostgreSQL's btree, server-side, flat in row count — narrowly, because a validated recorded location replacing a descent is precisely and only what Waystone is for.

**3. Under concurrency, PostgreSQL is ahead, and the reason is structural.** At four connections PG more than doubles (491.5 TPS) and KDS does not (213.7): PG's group commit amortizes one fsync across concurrent committers, and KDS's equivalent — real, but serving a cooperative single statement stream — has nobody to batch with until the cross-core pipeline lands. The freight decomposition says the same thing from the other side: KDS spends **half a booking waiting on one fsync** where PostgreSQL spends 28%, while PostgreSQL's write *statements* cost 1.8× more.

**4. The engine's own structures pay in proportion to re-probing, and the benchmarks price both directions.** Waystone: 22–33× on repeated point access against a heap relation, and a 3–6% *loss* on a btree one. Cabin: 90.6% hit rate → 14.3% faster than the walk at 200 rows; 9.1% hit rate → 172.3% *slower* at 10,000, because a uniform draw stops re-probing as the value space grows. The hit rate is a property of the workload, not of the structure — which is exactly the judgement the Cabin controller was built to make, and what its 2.8× on a rotating-hot-set workload measures.

**And one measurement that invalidates numbers rather than producing them:** the same business-stress mix runs at **1,730.6 TPS on tmpfs and 166.8 TPS on real xfs** — a 10× swing from the filesystem alone. Every results file names its block device and refuses tmpfs because of it. Any benchmark where fsync is free describes a different engine.

### Caveats — what these numbers do not show

Stated once, applying to everything above:

1. **Single-connection bias.** Almost every number is one connection, one request at a time. That flatters KDS's fsync ladder (a batch of one is a batch) and hides PostgreSQL's concurrency strengths — which is why the four-connection row, where PG wins 2.3×, is quoted with the same prominence as the 6× pk result.
2. **PostgreSQL is untuned.** Stock 17.10, 128 MB `shared_buffers`. The per-file caveats say where that costs it (e.g. large scans).
3. **Durability is not symmetric.** PostgreSQL fsyncs *and can replay its WAL after a crash*. KDS logs every data mutation at the same acknowledgment points but **recovery is not implemented** — nothing reads the log back. A durability number bought without recovery is cheaper; keep that in mind wherever the two engines tie.
4. **No prepared statements on either side**, no connection pooling, no parallel query on the PG side.
5. **The client floor.** ~90–210 µs of Python socket cost rides on every client-visible latency on both sides; sub-10 µs client-side differences are noise.
6. **Shared 2-vCPU host.** Engines ran sequentially, never concurrently, and each file records load averages and compiler activity.

### The provenance discipline

Every results file carries the same evidence block, which is what makes these numbers quotable at all: **the commit measured and the binary's provenance** (mtime against HEAD, diff by diff where a binary predates HEAD, dirty files listed with why none is on the measured path); **the device, named and refused when wrong**; **machine state** (load averages before and after every cell, compiler checks, and in the newest files a hard load gate whose discarded attempts are listed rather than silently retried); **a noise floor from inside the run** (interleaved A/B repeats, so "X% faster" reads against the spread the same harness produced the same day); **verification before measurement** (`--verify` row-for-row, plan assertions through `ANALYZE`, enforcement stamps — a run that cannot prove it measured the right thing is aborted, and aborts are recorded as findings); and **supersession recorded, never overwritten** — a results file is history, and history does not get edited.

### Reproducing

Each comparison has a driver pair under [`tools/`](tools/) sharing one harness, so both sides measure identically; [`bench/docs/README.md`](bench/docs/) records exact invocations per results file.

```bash
tools/pg_setup.sh init                      # scratch PostgreSQL 17 cluster, port 15433
build-release/kds_server --config kds.conf  # KDS on 15432 — Release build, never ./build

tools/benchmark.py                # the four-phase client path       (+ pg_benchmark.py)
tools/scenario1_backtest.py       # joins/ranges ladder              (+ pg_scenario1_backtest.py)
tools/scenario2_freight.py        # FK-checked bookings              (+ pg_scenario2_freight.py)
tools/scenario3_library.py        # non-pk equality matrix           (+ pg_scenario3_library.py)
tools/index_benchmark.py          # secondary indexes                (+ pg_index_benchmark.py)
tools/bulk_insert_benchmark.py    # batch ladder                     (+ pg_bulk_insert_benchmark.py)
tools/cabin_optimizer_benchmark.py# the physical optimizer's controller (+ pg_ twin)
tools/multicore_benchmark.py      # the 4-relation isolation baseline (no PG twin)
```

Ground rules, learned the hard way: measure `build-release` only (`./build` is Debug and has reported the wrong *sign* twice); put data files on a real filesystem, never `/tmp`; run the engines sequentially on this class of box; and re-measure a premise before building on it.

### Full results index

`bench/` keeps a results document for each whole-workload scenario and for
nothing else. The narrower measurements — the id allocator, the fold, the
index's write cost, the durability ladder, the statistics feed and the rest —
were each written up once and have been removed; their drivers are still in
`tools/`, documented in `bench/docs/README.md`, and re-running one is how to
get the number back.

| File | What it measures |
|---|---|
| [`bench/results-scenario1-vs-pg.md`](bench/results-scenario1-vs-pg.md) | Joins, subqueries, ranges at three sizes — the fixed-cost/per-row fit and the crossover |
| [`bench/results-scenario2-freight.md`](bench/results-scenario2-freight.md) | Where a whole transaction spends its time — the fsync's share, the options matrix, lost updates under READ COMMITTED, and PostgreSQL beside it |
| [`bench/results-scenario3-library.md`](bench/results-scenario3-library.md) | What a non-pk equality costs: index vs Cabin vs walk vs PostgreSQL, and the join that does not use the index |

---

## Status

Under active development, specification-first: every subsystem has a spec with explicit open decisions and required tests in [`docs/`](docs/) — start with `heap-and-tuple.md` (authoritative) and `rules.md`.

**Read [`docs/inflight/known-gaps.md`](docs/inflight/known-gaps.md) before relying on anything.** It is the engine-wide list of what is missing and what a restart loses, kept deliberately blunt. The headline entries: **WAL recovery is not implemented** (the log is written and never read back); nothing purges anywhere, because readers are deliberately unregistered; cross-core execution has just begun — a rotated relation's single-step `SELECT` executes on its owning core, and everything else is still served by core 0 until the rest of the step pipeline lands; Cabin entry sets, the assertion registry, and the Cabin controller's managed state are memory-resident and rebuilt by traffic after a restart; and there is no auth, no TLS, loopback only.

[`manual/`](manual/) is the user-facing surface, verified against code rather than against specs.

```bash
./build.sh        # build
./test.sh         # run the full deterministic test suite
tools/ckdbs_cli.py --port 15432   # talk to a running instance
```
