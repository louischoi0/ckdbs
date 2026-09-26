# KDS

**An OLTP database engine that keeps the invariants so the application does not have to.**

KDS narrows its surface to what online transaction processing needs — keyed reads and writes, short transactions, pk-chain joins, tight tail latency, durability the caller chooses — and spends the room that buys on two things most engines leave to the application: the rules the data must obey, and the physical shape the data should have. The first is the subject of the next section. The second is the physical optimizer.

---

## Invariants live in the engine

An application above a database keeps a list of things it must never get wrong: no duplicate ids, no group over its limit, no state change that skips a step, no retry that runs twice, no cache that disagrees with the row it caches. Each is checked in code, in every service, by every author. KDS is built on the position that these belong **in the engine, declared once in the schema**, so that whatever writes to the database — a service, a script, an agent — cannot violate them, and the code above stays thin.

That position matters more as more code is written by agents. Agent-written code is plentiful, cheap and not uniform. An invariant scattered across such code is a probability; an invariant enforced by the engine is a fact. The engine also gives an agent what it needs to work against a database: **every refusal names the byte position of the offending token** and says whether the statement was understood and declined (`Unsupported`) or simply wrong (`InvalidArgument`); **a statement's plan never changes with cardinality**, so its cost can be reasoned about; and **every statement shape the database has been asked is a catalog object** (`sys.patterns`), so what the workload actually does is a table, not a guess.

What the engine guarantees today, and what the application may forget:

| Guaranteed | The application no longer | Status |
|---|---|---|
| **Issue-once identity.** A pk is a 40-bit id that is unique for the life of the relation; a deleted row's id is never reissued, and a stored id can dangle but never mis-attribute | derives keys, checks for reuse, or guards against a stale reference pointing at the wrong row | built |
| **Group bounds.** SQL-92 `CREATE ASSERTION` over per-group `COUNT(*)` / `SUM(col)` upper bounds, checked in O(1) at admission; a violating write is refused before it happens | counts, sums and compares before every write, and reconciles when two writers race | built |
| **Referential integrity.** Foreign keys, checked on write | joins to confirm a parent exists before inserting a child | built ([gap](docs/inflight/known-gaps.md#foreign-keys)) |
| **Advisory learning that cannot change an answer.** Every structure the engine learns — trails, hints, engine-created Cabins — may cost performance if wrong, never a result | verifies the database's own optimizations | built, with its own test family |
| **Chosen durability.** `strict` / `group` / `relaxed` per transaction, with the loss window stated | wonders what an acknowledgment meant | built |
| **A repeatable plan.** A `pattern_id` names one plan forever | tunes around a planner that changes its mind | built |
| **Transition constraints.** A column's legal state changes — initial states, `old => new` edges, deletable states — declared with the column | checks `if (old == X && new == Y)` in every writer | concept, [CN-4](docs/conceptnotes/cn4-transition-constraints.md) |
| **At-most-once execution.** A client token claimed on arrival and carried by the commit record, answering both "did it commit?" and "do not run this twice" | builds idempotency tables and outcome lookups | concept, [CN-5](docs/conceptnotes/cn5-commit-outcome-and-idempotency.md) |
| **A cache that cannot disagree with the data.** A typed, unlogged, TTL-bearing relation under the ordinary MVCC, read by `GET` inside SQL, written in the same transaction as the rows it caches | keeps a second store consistent with the first | concept, [CN-6](docs/conceptnotes/cn6-in-engine-cache-relations.md) |
| **Identity succession.** Retiring a tuple and chaining a successor to it, so an old id resolves to the tuple that replaced it | joins across "the same customer, re-registered" | concept, [CN-3](docs/conceptnotes/cn3-supersede-and-chain-id-succession.md) |
| **Machine-operable tuning.** The optimizer's levers exposed as data and settings an external agent can drive safely | employs a DBA, or asks an agent to guess at one | concept, [CN-1](docs/conceptnotes/cn1-agent-operable-optimization-surface.md) |

Rows marked *concept* are recorded in [`docs/conceptnotes/`](docs/conceptnotes/) with their reasoning, their boundaries and the decisions they leave open. A concept note opens no stage and licenses no code; it is listed here because it is where the engine is pointed, not because it exists.

---

## Design philosophy

- **OLTP-first, not general-purpose.** Primary-key point reads and writes, inner equi-join chains, predicate-position subqueries, pagination, short transactions. Every statement compiles to a **step chain** executed in written order — *the query is the plan*: execution shape is classified at parse time and dispatched without plan search. No CTEs, no window functions, no dialect-compatibility shims.
- **The engine observes, then reorganizes.** Executions feed a statistics layer. Time-decayed scores classify data and query shapes hot/warm/cold; the physical optimizer creates and retires lookup structures. Statistics change *what exists*, not just how a query runs.
- **Advisory by construction.** Everything learned — trails, scores, placement hints, engine-created Cabins — is structurally advisory or value-granularly revocable: delete all of it and every query still returns the same rows, just slower. The B+ tree stays the sole unconditional authority. This is a hard invariant with its own test family, not a design preference.
- **Reliability is a product feature.** One write-ahead log per instance with per-transaction durability classes, page checksums, full-page-image torn-write recovery, recovery at mount, and fault paths exercised by deterministic injection rather than assumed. What is *not* built is stated as plainly as what is — see [Status](#status).
- **Thread-per-core over shared memory.** One cooperative reactor per core; every statement runs to completion on the core its session is on. Pages, the buffer pool and the log are shared; what serializes access is a **borrow model** — transaction-scoped, waitable *locks* and critical-section-scoped *latches* that are never held across a suspension point. The only cross-core primitive is a reactor kick. All I/O is asynchronous behind an injectable seam, so the entire engine runs under deterministic simulation.
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

Every tuple's first column is a mandatory 64-bit **Keystone word**: `id:40 | flags:8 | reserved:16`. The 40-bit id is the primary key (≈1.1 × 10¹² ids per relation); the flags byte is the transaction/status byte; the 16 reserved bits are written 0 and ignored.

Self-imposed constraints:

- **The pk is unique for the life of the relation, and where it comes from is the `INSERT`'s choice.** Name a value in the pk's position and that value *is* the key; omit it and the engine issues one from `sys.tables.next_id`, a **high-water mark on what has been placed** — persistent, never derived as `max(id) + 1`, because deriving it would reissue the identity of a deleted row. An issued id always clears every key the caller has named.
- **A named key below the mark is btree-only**, proved by the descent that lands on the one leaf that may hold it; the mark proves a key at or above it without reading a page.
- **The pk cannot be updated.** It is the tuple's identity, not a field of it; an `UPDATE` naming it is refused at compile time as `Unsupported`, with the byte position of the column.
- **The pk is stored once**, carried only by the Keystone word, never also as a body column.
- **Ids are unique, not gapless.** A failed insert burns one. Nothing depends on gaplessness.
- **The word is atomic; the encoding is manual.** Read and written as an atomic `uint64_t`, encoded with explicit shift/mask helpers — compiler bitfields are forbidden in any persisted format.

What this buys is the **issue-once contract**: a stored id may *dangle* (its row aborted or purged), but it can never *mis-attribute* — no future tuple can inherit it. That single property is what lets Cabins and indexes store pks instead of addresses.

### Pages and relations — btree first

Pages are 8192 bytes. Every page header carries an **immutable `min_key`** fixed at creation, and no tuple with `id < min_key` may ever be placed in a page — including transiently during any relayout. Tuples inside a page are unordered (append at O(1)); pages are ordered by `min_key`.

A relation is stored as a **clustered B+ tree** on the pk. A btree leaf *is* a heap page — same slots, same tuple format, same MVCC header — so the tree is a directory over pages, not a second storage engine. The tree is what admits a caller-named key that sorts *below* keys already placed, because only a descent can place and prove one; an id sorting inside a full leaf moves the upper half out to a new leaf, and both invariants above survive the move.

The **semi-sorted heap chain** — the same pages without a directory, tail-append only — exists and still mounts and serves for relations created before 2026-09-05, but **creating one is refused** (`SUS-1`): the storage clause defaults to `BTREE` and `HEAP` is declined by name, so the btree track can mature first. The suspension's resume condition is in `instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`.

### The fixed-length rule — tuples that never migrate

**Purpose.** Make a tuple's address stable for its entire life, so that recorded locations stay meaningful.

**Every tuple is fixed-length.** A relation's row size is a schema constant. Every variable-width value occupies exactly one **tagged cell** of `kds.inline_cell_width` bytes (default 64), whatever it holds:

| Tag | Layout | Meaning |
|---|---|---|
| `kNull` | zeros | SQL NULL |
| `kInline` | `len u16` + bytes + padding | fits in the cell |
| `kSpilled` | `len u32` + `varheap_ptr u64` | bytes live in the var-heap |

The consequence is the point: **an UPDATE can never migrate a tuple.** The spill decision changes a cell's *tag*, never the tuple's size. The accepted costs are stated rather than hidden: padding is spent on short values, and variable-length management is *relocated* into the var-heap, not eliminated.

The **var-heap** is deliberately boring: values are appended, **immutable per version**, and never moved — so an old-version reader follows an old pointer to bytes that cannot have changed. It is authoritative data: logged, headered, checksummed. `kds.inline_cell_width` is pinned into the superblock at bootstrap and validated at every start; a disagreement refuses to boot, naming both values.

A tuple whose length disagrees with its relation's constant is `Corruption` — never interpreted.

### Step chains — the query is the plan

**Purpose.** Remove plan search from the statement path, and make a statement's cost predictable enough to be a *stable* number the optimizer — or an agent — can reason about.

Every statement compiles at parse time into an ordered list of steps — lookups, probes, ranges, scans, nested sub-chains for subqueries — executed in written order. The parser parameterizes literals as it lexes (so the pattern fingerprint comes out of the parse for free), tags the statement with an execution class, and binds catalog names to oids. The executor never re-analyzes shape and never resolves a name.

The self-imposed constraint is real and its cost is measured: **a `pattern_id` names a plan forever.** KDS chooses the same plan at every cardinality, so it cannot decline its own index on a small relation the way a cost-based planner does. That trade is deliberate: a stable plan is what makes a recorded trail replayable and a workload's cost profile learnable.

### Waystone — the recorded trail of a repeated query

**Purpose.** Serve a recurring query from where the last execution *found* its rows, instead of searching for them again — across every relation the statement touched.

A *pattern* is a statement's shape, fingerprinted at parse time (`WHERE id = 42` and `WHERE id = ?` converge on one `pattern_id`). A *pattern instance* is that shape with arguments bound: `(pattern_id, arg_hash)`. From an instance's **second** execution on, KDS records its **trail**: the Keystones of every tuple that execution touched, tagged with the step of the chain that produced each one. Patterns are catalog objects in `sys.patterns`.

The trust model is one sentence:

> **A trail may replace a lookup. It may never replace a search.**

A stale entry pointing at the wrong place is caught by the Keystone-id check; a stored *set* missing a row inserted since it was recorded is wrong in a way **no per-tuple validation can detect** — absence has no witness. So a keyed step may be served from a trail, because completeness for that step follows from pk uniqueness rather than from the trail. A step that must search still searches; negation (`NOT IN`, `NOT EXISTS`) is search-class by definition.

The per-entry replay contract, in order, all mandatory: re-derive the probe key from the current outer row and require it to equal the entry's pk; read the tuple at the recorded `(page_id, slot)` and require the Keystone id there to equal the entry's pk and the page to belong to the entry's `rel_oid`; require the recorded **page epoch** to match the page's current epoch; apply MVCC visibility exactly as the authoritative path would; on any miss, fall through to the authoritative path for that step alone.

A trail is **one page (253 entries), never continued** — a trail that would exceed it is not recorded *at all*, because a truncated trail is indistinguishable from a complete one. Storage and policy live entirely behind a one-method seam outside the executor.

### Cabin — authority for the values that were asked for

**Purpose.** Make non-pk equality fast without paying an index's unconditional write cost on values nobody queries.

A Cabin is a per-`(relation, column)` store that tracks the tuples matching *observed* values of that column. Where a secondary index covers every tuple unconditionally, a Cabin covers exactly the values queries have touched — and **only there does it hold authority**:

> **Observed ⇒ complete (superset form).** For every observed value `v` and every active snapshot `S`, `v`'s entry set ⊇ { pk : tuple visible in `S` whose key column equals `v` }. A missing qualifying pk violates authority; a surplus entry never does — surplus is subtracted at read time.

Two corollaries carry the design. An observed value's **empty entry set is an authoritative "no rows"** — a negative answer no advisory structure can give. And **un-observing is always legal**: dropping a value's set returns queries for it to the scan path, a performance loss and never a correctness one. That is what keeps an *authoritative* structure evictable.

Self-imposed constraints: entries store the pk plus an advisory location hint, never the location as authority; every maintenance action is an append and removal is forbidden on the write path, because an older snapshot may still be entitled to match through the undo chain; the read does the subtraction (visibility, key re-check, seen-set dedup); the hint is verified through the one verifier Waystone replay also uses; who may decide is declared per column at `CREATE TABLE` as `NO CABIN` / `CABIN AUTO` / `CABIN`, and a policy on the pk column is refused; and a Cabin is **unlogged authoritative** — a restart declares every Cabin fully unobserved and traffic rebuilds it, which keeps the write hook off the WAL path.

At the limit, full observation of a column is permitted — at which point a Cabin has become a lazily built secondary index, paid for value by value. **A traditional index is the limit case of a Cabin, not a competing feature.**

### Assertions and the Bound Cabin

**Purpose.** Ship SQL-92 `CREATE ASSERTION` — which no major DBMS enforces — by restricting it to the class that can be checked in O(1).

The two classic blockers are re-evaluation cost and concurrency. KDS removes both by construction rather than by generality: the predicate class is restricted to per-group `COUNT(*)`/`SUM(col)` upper bounds; the incremental state lives in a **Bound Cabin** — the same machinery as an Observational Cabin with the lifecycle contract inverted (eager full coverage, pinned pages, eviction *forbidden*, logged and crash-consistent, the row's aggregate carried inline); and an admission check reads **only the group header**, so it is O(1) with no entry iteration. Enforcing on every core. Everything outside the supported class is a truthful `Unsupported` with a byte position.

### Transactions, MVCC and the WAL

**Purpose.** Snapshot reads over shared memory, and durability the caller chooses per transaction.

The per-tuple MVCC header is exactly 20 bytes — `trx_id:48 | undo_ptr | data_len | flags` — and **there is no `xmax`**: a version's death is the next version's birth, and walking the undo chain already names the overwriting transaction. `DELETE` is a delete-mark plus the deleter's `trx_id`, with bytes left in place for older snapshots.

A read view is a **snapshot LSN**: every commit whose record sits at or below it was published when the view was minted, whatever its transaction id. Visibility is answered from an instance-wide floor and a window of unresolved writers; the view itself stays a plain value that nothing tracks. **Readers are registered** — a holder whose view can outlive a suspension point takes a `ReaderLease`, the manager publishes the oldest live snapshot per core, and purge consults the minimum. That horizon is what lets the undo purge recycle versions no view can reach.

The WAL is **one stream per instance**: core 0 owns it, every peer attaches, and every `fdatasync` is issued once over one file. Physiological redo plus undo-chain MVCC, full-page images against torn writes, fuzzy checkpoints, and three per-transaction durability classes — `strict` (ack after the commit record is device-synced), `group` (same durability point, batched), `relaxed` (a bounded loss window, for reconstructible data). **Recovery runs at mount**: analysis, redo, the high-water repair and an injected undo phase, so losers are rolled back; a stream with losers recovered without undo refuses the mount rather than publishing uncommitted writes.

### Thread-per-core

One cooperative reactor per core; tasks are C++20 stackless coroutines, run to completion, in scheduling groups (foreground / system / maintenance) throttled by SLO rather than preempted. **Every statement runs on the core its session is on** — no relation or range is owned by a core, and there is no placement to get wrong. The buffer pool is one frame table for the instance, so a page one core faulted is read by every other rather than faulted again. Access is serialized by the borrow model: a *lock* is transaction-scoped and waitable by parking; a *latch* is critical-section-scoped and never held across a park. Cross-core signalling is a reactor kick over `eventfd`, and nothing else. There is no canonical in-memory tuple and no identity cache to keep coherent: consistency comes from pin and latch discipline on the page.

---

## The Physical Optimizer

Alongside the query optimizer every database has, KDS has a **physical optimizer** of equal rank: runtime access statistics decide *what structures exist* — which columns earn a lookup structure and when one has stopped paying for itself. It is one umbrella over two halves that share a decay score and touch none of each other's structures. **Part II, the Cabin controller, is built and measured. Part I, relayout, is shadow-only and gated, and since SUS-1 proposes nothing for a relation created after the suspension.** The order is deliberate: what is enacting comes first.

### The shared substrate: the lazy-decay score and the page epoch

**The lazy-decay score (R1)** is the engine's one implementation of "how hot is this, now". State is two words — `score` and `last_bump` — and the decayed value at time *t* is `score · 2^(−(t − last_bump) / half_life)`. A touch decays-then-increments; a read decays only; **there is no background decay pass**. Fixed-point arithmetic only; the clock is injected. Half-life is one instance-wide key (`decay_half_life`, default 600 s) — a second decay formula anywhere in the engine would be a defect.

**The page epoch (R4)** is how an advisory structure survives data moving underneath it. Every page header carries `relayout_epoch`; Waystone entries and Cabin hints record the epoch they observed; the shared verifier compares recorded against current, and a mismatch is a per-entry miss with the ordinary fall-through. **No consumer may accept a location on epoch equality alone** — the epoch is a fast whole-page invalidation layered *over* the Keystone-id check, never a substitute for it.

### Part II — the Cabin controller (built)

A background controller that decides which Observational Cabins should exist, based on nothing but what the workload did. It operates exclusively on advisory-class structures, which is what licenses it to act autonomously: a wrong decision costs performance only.

**Three input signals, no more.** (S1) fingerprint execution frequency under the shared decay score; (S2) observed pages scanned per execution; (S3) Cabin quality — hint hit/failure counters and coverage misses.

**A closed action vocabulary.** `CREATE`, `EXTEND`, `HEAL`, `DROP`. Bound Cabins are permanently outside its jurisdiction; the controller enumerates only Cabins carrying its own `auto` origin tag.

**Everything in the common currency of page accesses**, fixed-point, no floats:

```
Benefit  B(c) = Σᵢ fᵢ × max(0, P_scan,i − P_cabin)
Cost     C(c) = P_rel / T_amort  +  h_fail × f_lookup × k_heal
```

| Action | Condition |
|---|---|
| CREATE | `B > 3 × C` sustained for 3 consecutive snapshots, and the budget admits |
| EXTEND | coverage-miss share > 20%, and the missed share's marginal benefit alone clears the create bar |
| HEAL | hint-failure rate > 10% while the Cabin still pays for itself |
| DECAYING | `B < 0.5 × C` |
| DROP | that condition persists for the cooldown, or a HEAL failed to recover quality |

The wide gap `θ_drop ≪ 1 ≪ θ_create`, plus the confirm count and cooldown, is the **anti-thrash mechanism**; hysteresis here is load-bearing. The amortization window (64 half-lives) and the cooldown (128 half-lives) were set *from measurement* on a three-business-day workload with hot value sets rotating nightly, and the honest limit is documented rather than tuned away: a dead Cabin and an overnight-quiet one emit the same signal.

**Safety properties, all structural.** Decide is a **pure function** from an immutable snapshot to an action set, and Execute is the only effectful phase — which makes seed-driven replay through checked-in golden traces a real determinism proof. Every build scan goes through the scan ring, so the controller can never displace the foreground working set. The budget is a solved ranking problem: an over-budget CREATE is admitted only in *exchange* for dropping the lowest-net-benefit active Cabin. `SET kds.cabin_optimizer = off` is a runtime kill switch that halts new decisions and in-flight builds and **touches nothing that exists**.

**Observability is the deliverable.** `SHOW CABIN_OPTIMIZER` reports every managed Cabin with its state, net-benefit score, hint hit rate, coverage, pages, and last action *with the scores that produced it*; `ANALYZE` marks a probe served by an optimizer-managed Cabin.

**Measured** (`tools/cabin_optimizer_benchmark.py`): idle cost sits inside same-configuration noise — the tick is 2–3 µs CPU at the default cadence; a self-created Cabin is worth **10.9× client p50, 19× server CPU** at 10,000 rows, created 1.4 s after switch-on with the reply verified byte-identical to the walked one; on the three-business-day workload, day-1 TPS is **608 with the controller off, 1,680 with it on, 1,724 with the five right columns hand-declared** — parity with an operator who knew the answer in advance.

Known limits, stated: managed state and the decision log are memory-resident and rebuilt by re-observation after a restart; the decay score's fixed-point range underflows after ~16 half-lives, so across most of a long cooldown a DROP is a timeout rather than a judgement; the key is **off by default**.

### Part I — statistics-driven relayout (shadow-only, gated)

`sys.access_stats` records one row per access *shape* — `(kind, rel_id, column_mask)` — keyed by columns, never values. `SHOW RELAYOUT` is the physical health report: per relation, each access shape with its raw count and decayed weight, chain length, delete-mark density, and one line per **candidate plan** (`compact`, `cluster`, `defrag`) with predicted benefit and the gate blocking it. The planner is **pure and pull-only** — it runs when `SHOW RELAYOUT` asks, has no background task, and its measured idle cost is zero at the noise floor.

**No mover exists.** Each candidate move is blocked by a named constraint recorded in `docs/spec/physical-optimizer.md` §6, and the planner builds plans for heap relations only — so for every relation created since SUS-1 it proposes nothing at all. The mover is nonetheless specified structurally (§4, the legal-move table), so building it later is filling in a form rather than reopening the design. `physical_optimizer` takes `off | shadow`; **`on` is refused at startup, naming the open gates** — a config written for the future fails loudly today instead of silently under-delivering.

---

## Architecture

```
                     clients  ·  KWP/1 binary frames
                                    │
                        ┌───────────▼───────────┐
                        │   TCP server / KWP    │  handshake · sessions · txn control
                        └───────────┬───────────┘
                        ┌───────────▼───────────┐   pattern fingerprint, at parse time
                        │        Parser         │───────────────┐
                        └───────────┬───────────┘               ▼
                        ┌───────────▼───────────┐       ┌────────────────┐
                        │  Executor (step VM)   │◀─────▶│ Waystone trails│  advisory
                        │  on the session core  │ record│ + sys.patterns │
                        └──┬──────────────┬─────┘ replay└───────┬────────┘
                           │        ┌─────▼──────┐              │
                           │        │   Cabin    │  authoritative for observed values
                           │        └─────┬──────┘              │
                        ┌──▼──────────────▼──────┐              │
                        │ B+ tree (pk) + indexes │◀─ validates ─┘
                        └──────────┬─────────────┘
                                   │              ┌──────────────────────────┐
                                   │              │  Physical optimizer      │
                                   ▼              │   II · Cabin controller ─┼─► create
                        ┌──────────────────────┐  │   I  · relayout planner  │   extend
                        │  Pages: min_key,     │  │       (shadow, gated)    │   heal
                        │  Keystone tuples,    │  └──────────────────────────┘   drop
                        │  20-byte MVCC header │   reads sys.access_stats and
                        └──────────┬───────────┘   decayed shape frequencies
              ┌────────────────────┼────────────────────┐
   ┌──────────▼──────────┐ ┌───────▼──────────┐ ┌───────▼───────────┐
   │ Buffer pool (shared)│ │ WAL (1/instance) │ │  Space manager    │
   │ one frame table     │ │ D1/D2/D3 classes │ │  extents · freemap│
   └──────────┬──────────┘ └───────┬──────────┘ └───────┬───────────┘
              └────────────────────┼────────────────────┘
                        ┌──────────▼───────────┐
                        │ Single growable file │  + one WAL segment stream
                        └──────────────────────┘
```

## Components

| Component | What it does |
|---|---|
| **KWP wire protocol** | Length-prefixed binary protocol: version/capability handshake with an authentication stage, extended PARSE/BIND/EXECUTE over server-side statement and portal handles, chunked result streaming with explicit flow control, per-transaction durability selection, a structured error registry with retryable classes |
| **Parser** | Small OLTP grammar — joins and predicate-position subqueries included. Parameterizes literals during the parse (pattern fingerprints come out for free), tags each statement with an execution class, binds catalog names to oids at parse time |
| **Executor (step VM)** | Every statement is a **step chain** — lookups, probes, scans, nested sub-chains — run in written order on the session's core. Replay-eligible steps consult the trail first (validated per entry); the rest run authoritatively |
| **Waystone** | The trail store: per pattern instance `(pattern_id, arg_hash)`, the recorded Keystones of the rows it touched, with last-seen locations and step tags. Strictly advisory — droppable wholesale without changing any result |
| **Cabin** | Per-`(relation, column)` value store, authoritative **for observed values only**: entries hold pks plus an advisory location hint. Append-only maintenance, read-time verification, per-value eviction. Declared per column as `NO CABIN` / `CABIN AUTO` / `CABIN` |
| **B+ tree** | The authoritative pk → location index, the storage form of every new relation. Append-optimized for monotonic engine-issued ids (rightmost fast path, asymmetric splits), dividing a full leaf at its median when a caller names a key below the high-water mark |
| **Secondary indexes** | Multi-column and covering, for the searches a trail may never replace. Formally "a Cabin that observed everything". Index entries are logged before the row write they describe |
| **Assertions** | `CREATE ASSERTION` over `COUNT(*)`/`SUM(col)` group upper bounds, enforced at admission on every core against an O(1) running aggregate held in a pinned, logged **Bound Cabin** |
| **Foreign keys** | Declared at `CREATE TABLE`, checked on write; see `docs/spec/foreign-keys.md` and the open entry in `known-gaps.md` |
| **Physical optimizer** | **II — the Cabin controller** *(built; `cabin_optimizer`, off by default)*: creates, extends, heals and drops advisory Cabins from workload statistics under a page budget, a kill switch and a pure fixed-point decision core, with `SHOW CABIN_OPTIMIZER` exposing every decision. **I — relayout** *(shadow-only, heap-gated)*: the decay score, the page epoch at every validation site, and `SHOW RELAYOUT` |
| **Buffer pool** | One frame table for the instance. RAII pinned-page handles, an inline sweep on the fault path past `buffer_pool_frames`, WAL-ordering gate enforced in code |
| **WAL** | One append-only stream per instance. Physiological redo + undo-chain MVCC (writer trx-id + undo pointer; no xmax). Durability classes per transaction: `strict` / `group` / `relaxed`. Fuzzy checkpoints, full-page images, recovery at mount with redo and undo |
| **Transactions** | Snapshot-LSN read views over shared memory; registered readers with a purge horizon; the lock family (relation, range, slice, tuple) for transaction-scoped waits; latches for critical sections |
| **Storage** | One growable data file, arithmetic page addressing (`offset = page_id × 8 KiB`), extent-based crash-safe growth, bitmap free-space management, CRC32C page checksums. mmap deliberately rejected — explicit async I/O only |
| **Scheduler** | Cooperative reactor pinned per core: C++20 stackless coroutines, run to completion, scheduling groups (foreground / system / maintenance) with SLO-based throttling instead of preemption; `eventfd` kick as the sole cross-core primitive |
| **Deterministic testing** | Clock, randomness, and all I/O are injected. The whole engine runs single-threaded under a simulated scheduler with crash and torn-write injection — durability claims are tested, not asserted |

## Invariants — the self-imposed constraints

Never violated, never "temporarily" bypassed. Each is a capability given up for a guarantee; the owning statement is `CLAUDE.md`'s hard-invariant list and `docs/spec/heap-and-tuple.md` §8.

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
| 11 | Every pk is a unique 40-bit id, carried only by the Keystone word, **never updatable**; the `INSERT` names it or omits it, per row; `sys.tables.next_id` is a high-water mark on what has been placed, and a named key below it is btree-only | Issue-once identity |
| 12 | The MVCC header is exactly 20 bytes and there is **no `xmax`** | One fact stored once |
| 13 | Every tuple is fixed-length; a disagreeing length is `Corruption`, never interpreted | Tuples never migrate |
| 14 | Var-heap values are immutable per version; `kVarHeap` pages are never relocated | MVCC correctness for free |

## Specifications

The design is specification-first: every subsystem has a spec carrying its decisions, its open questions, and its required tests. `docs/spec/heap-and-tuple.md` is authoritative for row storage — where it and another document disagree, it wins. `CLAUDE.md` carries the invariants and the version discipline.

| Subsystem | Spec |
|---|---|
| Row storage, pages, Keystone, invariants *(authoritative)* | `docs/spec/heap-and-tuple.md`, `docs/rules/rule-fixed-length-tuple.md`, `docs/spec/page.md` |
| Pattern-keyed access trails | `docs/spec/waystone-concpets.md`, `docs/spec/create-pattern-user-defined-patterns-v1.md` |
| Value-observed authoritative store | `docs/spec/cabin.md` |
| Physical optimizer (relayout + Cabin controller) | `docs/spec/physical-optimizer.md` |
| Transactions & MVCC | `docs/spec/txn.md` |
| Logging, durability and recovery | `docs/spec/wal.md` |
| Query language, step chains, joins, subqueries | `docs/spec/parser-v2.md` |
| Aggregation | `docs/spec/aggregate.md` |
| Secondary indexes | `docs/spec/index.md` |
| Group-level assertions | `docs/spec/assertion.md` |
| Foreign keys | `docs/spec/foreign-keys.md` |
| Types (DATE, TIMESTAMP, DECIMAL, DECIMAL128, `char(N)`, `varchar(N)`) | `docs/spec/types.md` |
| Buffer-pool eviction | `docs/spec/eviction.md` |
| Scheduling; cross-core execution (largely retired at AT-S9) | `docs/spec/sched.md`, `docs/spec/crosscore.md` |
| Wire protocol | `docs/spec/protocol.md` |
| DDL (`ALTER TABLE`, `DROP TABLE`, bulk insert, transactional DDL) | `docs/spec/alter.md`, `docs/spec/drop-table.md`, `docs/spec/bulkinsert.md`, `docs/spec/ddl-transactional.md` |
| Id issue-once contract | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md` |
| C++ rules | `docs/rules/rules.md` |
| Concepts not yet decided to build | `docs/conceptnotes/` |
| Active work orders and ratifications | `instructions/v3.0.0/` |
| **What is missing, and what a restart loses** | **`docs/inflight/known-gaps.md`** |

## Glossary

The stone metaphor is deliberate — a *keystone* holds the structure up, a *waystone* guides the traveler without being the road, and a *cabin* is a place someone has actually been.

| Term | Meaning |
|---|---|
| **Keystone** | The 64-bit identity word every tuple carries: 40-bit id · 8-bit flags · 16 reserved bits. Everything that names a tuple names it by its Keystone id |
| **Waystone** | The advisory store of trails, reached per pattern instance through `sys.patterns`. Droppable wholesale without changing any result |
| **Trail** | The recorded path of one pattern instance — the Keystones a previous execution touched, in execution order, with step tags and last-seen locations. A trail may replace a lookup, never a search |
| **Cabin** | A per-`(relation, column)` store, authoritative for the values queries have actually observed; entries hold pks, not addresses. A **Bound Cabin** is the pinned, fully covering, logged variant that backs an assertion |
| **Pattern / pattern instance** | A pattern is a statement's *shape*, fingerprinted at parse time as `pattern_id`. An instance is that shape with arguments bound: `(pattern_id, arg_hash)` |
| **Step chain** | The compiled form of every statement: an ordered list of steps executed in written order. "The query is the plan" |
| **`min_key`** | The immutable per-page key lower bound: pages are unordered inside, ordered between |
| **Epoch** | A per-page counter bumped whenever its tuples move. Recorded by trail entries and Cabin hints; a mismatch means the location is no longer trusted and the authoritative path runs |
| **Decay score** | The engine's one time-decay implementation: exponential half-life, computed lazily from `{score, last_bump}`, never swept |
| **Advisory** | The invariant class every learned structure belongs to: deleting it may cost performance but can never change a query result. Enforced by a dedicated test family |
| **Borrow** | A scoped right to a page or tuple: a *lock* (transaction-scoped, waitable by parking) or a *latch* (critical-section-scoped, never held across a park) |
| **Read view** | A snapshot LSN plus the viewing transaction's id; visibility is answered from the instance's commit floor and window, never from a copied list |
| **Durability class** | Per-transaction WAL acknowledgment semantics: `strict`, `group`, `relaxed` |
| **Concept note (CN)** | A `docs/conceptnotes/` file recording an idea, its reasoning, its boundaries and its open decisions before anything is built — it opens no stage and licenses no code |
| **KWP** | The KDS Wire Protocol: length-prefixed binary frames, handshake with authentication, extended PARSE/BIND/EXECUTE, chunked streaming, per-transaction durability selection |

## Roadmap

1. **Advisory acceleration** — trails skip descents for recurring patterns; Cabins serve non-pk equality for observed values. *Built.*
2. **Self-managing structures** — the Cabin controller decides which Cabins exist, from measured cost and decayed demand, under a budget and a kill switch. *Built; the relayout half stays shadow-only behind its gates and SUS-1.*
3. **M3 Uniformity** *(active, `workorder-at-m3-uniformity.md`)* — one execution model on every core: ownership, placement, the cross-core pipeline and the ring transport retired; every statement on its session's core over shared memory. Stages S0–S9 landed; S10–S13 (ring-kind cleanup, transport deletion, prose sweep, measurement) open.
4. **Invariants in the engine** — the concept notes in `docs/conceptnotes/`, in whatever order the operator rules: CN-1 (agent-operable optimization surface), CN-2 (loose foreign keys and tuple completeness), CN-3 (supersede and chain), CN-4 (transition constraints), CN-5 (commit outcome and idempotency), CN-6 (in-engine cache relations). Each waits on a measurement gate or an operator decision recorded in the note itself.
5. **Hands-off operation** — everything needed to run KDS exposed as data and levers: the workload inspectable (`sys.patterns`, `SHOW ACCESS`, `SHOW RELAYOUT`, `SHOW CABIN_OPTIMIZER`), every optimization evaluable before it acts, every action a flag or a threshold with a promotion metric. Who — or what — sits in the operator seat is deliberately left open.

One property makes the last step sane rather than reckless, and it is structural: everything in the autonomous loop is advisory or value-granularly revocable by invariant, so the worst mistake any operator — scripted, automated, or human — can make through these surfaces costs performance, never correctness.

## What KDS is not

No CTEs or derived tables, no window functions, no cross-dialect SQL compatibility, no attempt to be a data warehouse. `GROUP BY` with `COUNT`/`SUM`/`MIN`/`MAX`/`AVG` is built; `HAVING` and sorted aggregate output are not. New relations are btree-only while SUS-1 stands. If your workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data with rules the data must obey, KDS is built for exactly that.

---

## Measurement

Every number in this file comes from a results file under [`bench/`](bench/), each recording the commit it was measured at (`git describe --tags`), the environment, the binary's provenance, and its own caveats. Nothing is quoted from memory or averaged across runs that used different code.

The rules, learned the hard way and kept by every results file: **`build-release` only** (`./build` is Debug and has reported the wrong *sign* twice); **a real filesystem, never tmpfs** — the same transaction mix has run 10× faster on tmpfs than on xfs, so any benchmark where fsync is free describes a different engine; **a noise floor from inside the run**, from interleaved A/B repeats, so "X% faster" reads against the spread the same harness produced the same day; **verification before measurement** — a run that cannot prove it measured the right thing is aborted, and aborts are recorded as findings; **no constant set without a measured or explicitly provisional value**; and **supersession recorded, never overwritten** — a results file is history.

v3.0.0's performance work runs in a single measurement epoch (AS-E): no performance claim is made about the shared-memory engine until that epoch's cells have run. The v2-era files remain in `bench/` as history of an engine AR0 replaced; their numbers are not this engine's.

Drivers live under [`tools/`](tools/); `bench/docs/README.md` records exact invocations per results file.

---

## Status

Under active development, specification-first, in v3.0.0. Every subsystem has a spec with explicit open decisions and required tests in [`docs/`](docs/) — start with `CLAUDE.md`, then `heap-and-tuple.md`.

**Read [`docs/inflight/known-gaps.md`](docs/inflight/known-gaps.md) before relying on anything.** It is the engine-wide list of what is missing and what a restart loses, kept deliberately blunt and opened fresh for v3.0.0. Among its entries: the buffer pool's exhaustion protocol is not built — `buffer_pool_frames` is a soft target and an undersized pool grows rather than refusing; no transaction has a lifetime ceiling, so one open transaction pins the purge horizon for the instance; a foreign key's forward check holds nothing, so a parent can be deleted under a child's check; the Cabin controller's CREATE decision still sees one core; and Part I of the physical optimizer is unreachable for every relation created since SUS-1. Cabin entry sets, the assertion registry, and the Cabin controller's managed state are memory-resident and rebuilt by traffic after a restart.

[`manual/`](manual/) is the user-facing surface, verified against code rather than against specs.

```bash
./build.sh        # build
./test.sh         # run the full deterministic test suite
tools/ckdbs_cli.py --port 15432   # talk to a running instance
```
