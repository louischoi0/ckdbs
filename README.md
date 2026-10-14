# KDS

**An OLTP database engine that keeps the invariants so the application does not have to.**

KDS narrows its surface to what online transaction processing needs — keyed reads and writes, short transactions, tuple-key-chain joins, tight tail latency, durability the caller chooses — and spends the room that buys on two things most engines leave to the application: the rules the data must obey, and the physical shape the data should have. The first is the subject of the next section. The second is the physical optimizer.

---

## Invariants live in the engine

An application above a database keeps a list of things it must never get wrong: no duplicate ids, no group over its limit, no state change that skips a step, no retry that runs twice, no cache that disagrees with the row it caches. Each is checked in code, in every service, by every author. KDS is built on the position that these belong **in the engine, declared once in the schema**, so that whatever writes to the database — a service, a script, an agent — cannot violate them, and the code above stays thin.

That position matters more as more code is written by agents. Agent-written code is plentiful, cheap and not uniform. An invariant scattered across such code is a probability; an invariant enforced by the engine is a fact. The engine also gives an agent what it needs to work against a database: **every refusal names the byte position of the offending token** and says whether the statement was understood and declined (`Unsupported` when the architecture cannot admit it, `NotImplemented` when nobody built it) or simply wrong (`InvalidArgument`); **a statement's plan never changes with cardinality**, so its cost can be reasoned about; and **every statement shape the database has been asked is a catalog object** (`sys.patterns`), so what the workload actually does is a table, not a guess.

What the engine guarantees today, and what the application may forget:

| Guaranteed | The application no longer | Status |
|---|---|---|
| **Issue-once identity.** A tuple key is a 48-bit id bound to at most one committed row for the life of the relation (only `PURGE` frees one); a stored id can dangle but never names another committed row | derives keys, checks for reuse, or guards against a stale reference pointing at the wrong row | built |
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

- **OLTP-first, not general-purpose.** Tuple-key point reads and writes, inner equi-join chains, predicate-position subqueries, pagination, short transactions. Every statement compiles to a **step chain** executed in written order — *the query is the plan*: execution shape is classified at parse time and dispatched without plan search. No CTEs, no window functions, no dialect-compatibility shims.
- **The engine observes, then reorganizes.** Executions feed a statistics layer. Time-decayed scores classify data and query shapes hot/warm/cold; the physical optimizer creates and retires lookup structures. Statistics change *what exists*, not just how a query runs.
- **Advisory by construction.** Everything learned — trails, scores, placement hints, engine-created Cabins — is structurally advisory or value-granularly revocable: delete all of it and every query still returns the same rows, just slower. The B+ tree stays the sole unconditional authority. This is a hard invariant with its own test family, not a design preference.
- **Reliability is a product feature.** One write-ahead log per instance with per-transaction durability classes, page checksums, full-page-image torn-write recovery, recovery at mount, and fault paths exercised by deterministic injection rather than assumed. What is *not* built is stated as plainly as what is — see [Status](#status).
- **Thread-per-core over shared memory.** One cooperative reactor per core; every statement runs to completion on the core its session is on. Pages, the buffer pool and the log are shared; what serializes access is a **borrow model** — transaction-scoped, waitable *locks* and critical-section-scoped *latches* that are never held across a suspension point. The only cross-core primitive is a reactor kick. All I/O goes through an injectable device seam, so the entire engine runs under deterministic simulation.

---

## Concepts

KDS names its own structures because they are not the standard ones. Each gives up a capability in exchange for a guarantee; the owning spec carries the full contract.

### The trust ladder

Four structures share the access path, separated by **how much they are allowed to be believed**:

| Structure | Authority | Write cost | Droppable | Failure of an entry means |
|---|---|---|---|---|
| Clustered tuple-key B+ tree | Always authoritative | Per-write maintenance | Never | Corruption |
| Secondary index | Authoritative, full coverage | Per-write append | Wholesale (`DROP INDEX`) | Statement failure |
| **Cabin** | **Authoritative for observed values** | Probe + append, observed values only | Per value | Un-observe → scan |
| **Waystone trail** | **Never authoritative (advisory)** | Zero on the write path | Wholesale | A miss → authoritative path |

Correctness never depends on the bottom three, so a wrong guess costs performance only — which is why the engine can learn aggressively.

### Keystone — the identity of a row

Every tuple's first column is a 64-bit **Keystone word**, `id:48 | flags:8 | reserved:8`: the 48-bit id is the **tuple identity key** (the *tuple key*), the flags byte the status byte. It exists so that any structure outside the tuple can name a row by value instead of by address.

- **The tuple key is unique for the life of the relation, and where it comes from is the `INSERT`'s choice.** Name a value in the tuple key's position and that value *is* the key; omit it and the engine issues one from `sys.tables.next_id`, a **high-water mark on what has been placed** — persistent, never derived as `max(id) + 1`, because deriving it would reissue the identity of a deleted row.
- **On a btree a named key may be any key not already bound, in any order.** A btree places it where it sorts — after `id = 100`, `id = 99` is placed — and refuses it because of its value for exactly two reasons: a duplicate (`AlreadyExists`, proved by the descent that lands on the one leaf that may hold it) or a key outside `[1, 2^48 - 1]` (`OutOfRange`). A key whose insert rolled back is free again; a key whose row was committed stays bound, even after `DELETE`, until a `PURGE` retires it. An issued id that meets a concurrently named key is burned and drawn again. (A heap relation, which can no longer be created, still refuses a named key below its mark, `OutOfRange`.)
- **The tuple key cannot be updated** (refused at compile, `Unsupported`), is stored only in the Keystone word, and is unique but not gapless. The word is an atomic `uint64_t` encoded with shift/mask helpers; compiler bitfields are forbidden in persisted formats.

This is the **issue-once contract**: a stored id may *dangle*, but a committed id never names another row, so Cabins and indexes can store tuple keys instead of addresses, and every reader re-checks the row it finds.

### Pages and relations — btree first

Pages are 8192 bytes. Every page header carries an **immutable `min_key`** fixed at creation, and no tuple with `id < min_key` may ever be placed in a page — including transiently during any relayout. Tuples inside a heap page are unordered (append at O(1)); a btree leaf is sorted by placement, each row taking the slot its key sorts to; pages are ordered by `min_key`.

A relation is stored as a **clustered B+ tree** on the tuple key. A btree leaf *is* a heap page — same slots, same tuple format, same MVCC header — so the tree is a directory over pages, not a second storage engine. The tree is what admits a caller-named key that sorts *below* keys already placed, because only a descent can place and prove one: the row takes the slot its key sorts to, so every leaf stays in key order whatever order keys arrive in. An id sorting inside a full leaf moves the upper part out to a new leaf, both invariants above survive the move, and the split is logged as one record that replays whole or not at all.

The **semi-sorted heap chain** (the same pages without a directory) is suspended since 2026-09-05 (SUS-1): existing heap relations mount and serve, creating one is refused, and the storage default is `BTREE`, so the btree track matures first ([`workorder-as-sus1-heap-suspended.md`](instructions/v3.0.0/workorder-as-sus1-heap-suspended.md)).

### The fixed-length rule — tuples that never migrate

**Every tuple is fixed-length**: row size is a schema constant, and each variable-width value occupies one **tagged cell** of `kds.inline_cell_width` bytes (default 64; a `varchar(N)` column uses its own width `N`) — `kNull`, `kInline` (fits in the cell), or `kSpilled` (a pointer into the var-heap). So **an UPDATE never migrates a tuple** and a recorded location stays meaningful; the cost is padding on short values. The **var-heap** is append-only, **immutable per version** and never relocated, so an old-version reader's pointer always reaches unchanged bytes; it is logged and checksummed. The cell width is pinned in the superblock, and a tuple whose length disagrees with its relation's constant is `Corruption`.

### Step chains — the query is the plan

Every statement compiles at parse time into an ordered list of steps (lookups, probes, ranges, scans, nested sub-chains), executed in written order; literals are parameterized during the lex, so the pattern fingerprint comes free, and names bind to oids at parse time. **A `pattern_id` names a plan forever**: the cost is that KDS cannot decline its own index on a small relation; the gain is that a recorded trail is replayable and a workload's cost profile learnable.

### Waystone — the recorded trail of a repeated query

A *pattern* is a statement's shape, fingerprinted at parse time and kept in `sys.patterns`; an instance is that shape with arguments bound. From an instance's second execution on, KDS records its **trail** — the Keystone of every tuple it touched, tagged by step — so a recurring query can look where the last one found its rows instead of searching.

> **A trail may replace a lookup. It may never replace a search.**

A stale entry is caught by the Keystone-id check, but a missing row has no witness, so only keyed steps — whose completeness follows from tuple-key uniqueness — are served from a trail; negation is always search-class. Each entry is verified (key, Keystone id, relation, page epoch, MVCC visibility) and any miss falls through for that step alone. A trail is one page (253 entries) and never truncated: one that would not fit is not recorded. Contract: `docs/spec/waystone-concpets.md`.

### Cabin — authority for the values that were asked for

A per-`(relation, column)` store covering only the values queries have *observed*, built so equality on a column other than the tuple key is fast without an index's unconditional write cost:

> **Observed ⇒ complete (superset form).** For every observed value and active snapshot, its entry set contains every visible matching tuple key; surplus is subtracted at read time.

An observed value's empty set is therefore an authoritative "no rows", and un-observing is always legal — it costs performance, never correctness. Entries hold tuple keys plus an advisory location hint, maintenance is append-only, the read subtracts (visibility, key re-check, dedup), policy is declared per column (`NO CABIN` / `CABIN AUTO` / `CABIN`), and a Cabin is unlogged: a restart unobserves everything and traffic rebuilds it. **A traditional index is the limit case of a Cabin**, fully observed. Contract: `docs/spec/cabin.md`.

### Assertions and the Bound Cabin

SQL-92 `CREATE ASSERTION`, which no major DBMS enforces, is shipped by restricting it to per-group `COUNT(*)` / `SUM(col)` upper bounds, held in a **Bound Cabin** (eager full coverage, pinned, logged, never evicted) so admission reads one group header in O(1). Enforced on every core; anything outside the class is `Unsupported` with a byte position.

### Transactions, MVCC and the WAL

The per-tuple MVCC header is exactly 20 bytes — `trx_id:48 | undo_ptr | data_len | flags` — and **there is no `xmax`**: a version's death is the next version's birth, named by the undo chain. `DELETE` is a delete-mark plus the deleter's `trx_id`.

A read view is a **snapshot LSN** answered from an instance-wide floor and a window of unresolved writers. Readers whose view outlives a suspension take a `ReaderLease`, and purge recycles only versions below the oldest live snapshot. An explicit transaction is rolled back at `txn_lifetime_ceiling_ms` (60 s).

The WAL is **one stream per instance**: physiological redo plus undo-chain MVCC, full-page images against torn writes, fuzzy checkpoints, and three durability classes — `strict` (ack after the commit record is synced), `group` (same point, batched), `relaxed` (a bounded loss window). **Recovery runs at mount** — analysis, redo, high-water repair, undo — so losers are rolled back before anything is served.

### Thread-per-core

One cooperative reactor per core; tasks are C++20 stackless coroutines, run to completion, in scheduling groups (foreground / system / maintenance) sharing the core by static share weights rather than preemption. **Every statement runs on the core its session is on** — no relation or range is owned by a core, and there is no placement to get wrong. The buffer pool is one frame table for the instance, so a page one core faulted is read by every other rather than faulted again. Access is serialized by the borrow model: a *lock* is transaction-scoped and waitable by parking; a *latch* is critical-section-scoped and never held across a park. Cross-core signalling is a reactor kick over `eventfd`, and nothing else. There is no canonical in-memory tuple and no identity cache to keep coherent: consistency comes from pin and latch discipline on the page.

---

## The Physical Optimizer

Beside the query optimizer, runtime access statistics decide *what structures exist*. Two halves share one decay score and touch none of each other's structures. **Part II, the Cabin controller, is built. Part I, relayout, is shadow-only and gated** — what is enacting comes first. Contract: `docs/spec/physical-optimizer.md`.

**Shared substrate.** The **lazy-decay score** (`score · 2^(−Δt / half_life)`, computed on touch, never swept, fixed-point, `decay_half_life` default 600 s) is the engine's one notion of "how hot, now". The **page epoch** (`relayout_epoch` in every page header) lets an advisory structure survive data moving under it: a mismatch is a per-entry miss, layered over the Keystone-id check, never a substitute for it.

**Part II — the Cabin controller (built).** It decides which advisory Cabins exist from three signals — decayed fingerprint frequency, pages scanned per execution, Cabin quality — with a closed vocabulary (`CREATE`, `EXTEND`, `HEAL`, `DROP`) priced in page accesses. Wide hysteresis between create and drop thresholds, a confirm count and a cooldown prevent thrash. Decide is a pure function of an immutable snapshot (replayed against golden traces), builds go through the scan ring, the page budget admits a new Cabin only by evicting the lowest-benefit one, and `SET kds.cabin_optimizer = off` stops it without touching what exists. `SHOW CABIN_OPTIMIZER` reports every decision with the scores behind it. The key is off by default; managed state is memory-resident. Its v2-era measurement (a self-created Cabin at parity with hand-declared ones) is history of the engine AR0 replaced, not a v3 number.

**Part I — relayout (shadow-only).** `sys.access_stats` records one row per access shape, keyed by columns, never values; `SHOW RELAYOUT` reports each relation's shapes and candidate plans (`compact`, `cluster`, `defrag`) with the gate blocking each. The planner is pull-only and builds plans for heap relations only, so since SUS-1 it proposes nothing for a new relation. **No mover exists**; `physical_optimizer = on` is refused at startup, naming the open gates.

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
                        │ B+ tree (key) + indexes│◀─ validates ─┘
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

| Component (not covered under Concepts) | What it does |
|---|---|
| **KWP wire protocol** | Length-prefixed binary protocol: version/capability handshake with an authentication stage, extended PARSE/BIND/EXECUTE over server-side statement and portal handles, chunked result streaming with explicit flow control, per-transaction durability selection, a structured error registry with retryable classes |
| **Parser** | Small OLTP grammar — joins and predicate-position subqueries included. Parameterizes literals during the parse (pattern fingerprints come out for free), tags each statement with an execution class, binds catalog names to oids at parse time |
| **B+ tree** | The authoritative tuple key → location index, the storage form of every new relation. Append-optimized for monotonic engine-issued ids (rightmost fast path, asymmetric splits); a named key is placed where it sorts, and a full leaf takes an append split when the key is above all its keys, otherwise divides - the rightmost at the insertion point, any other at its median |
| **Secondary indexes** | Multi-column and covering, for the searches a trail may never replace. Formally "a Cabin that observed everything". An entry is `key || tuple key`, never a location, and is logged under the index tree's hold |
| **Foreign keys** | Declared at `CREATE TABLE`, checked on write; see `docs/spec/foreign-keys.md` and the open entry in `known-gaps.md` |
| **Buffer pool** | One pool for the instance, bounded: `buffer_pool_frames` is required and a hard ceiling. Frames are slots reclaimed by a CLOCK hand in bounded batches, inline and on a background tick; a scan faults cold so it does not displace the working set; a full pool refuses a statement rather than grow. RAII pinned-page handles, WAL-ordering gate enforced in code |
| **Storage** | One growable data file, arithmetic page addressing (`offset = page_id × 8 KiB`), extent-based crash-safe growth, bitmap free-space management, CRC32C page checksums. mmap deliberately rejected — explicit I/O through the device seam only |
| **Deterministic testing** | Clock, randomness, and all I/O are injected. The whole engine runs single-threaded under a simulated scheduler with crash and torn-write injection — durability claims are tested, not asserted |

## Invariants — the self-imposed constraints

Never violated, never "temporarily" bypassed. Each is a capability given up for a guarantee; the owning statement is `CLAUDE.md`'s hard-invariant list and `docs/spec/heap-and-tuple.md` §8.

| # | Invariant | Bought |
|---|---|---|
| 1 | 8192-byte pages; `uint32_t` page ids; `0xFFFFFFFF` invalid | Arithmetic addressing, no indirection |
| 2 | A page's `min_key` is immutable after creation, until the page is freed | Lock-free range pruning |
| 3 | No tuple with `id < min_key(page)` in that page, ever — including by relayout, including transiently | The pruning decision is always sound |
| 4 | Tuples within a heap page are unordered; a btree leaf is sorted by placement | O(1) append; `ORDER BY <tuple key>` for free |
| 5 | The Keystone column is exactly `id:48 \| flags:8 \| reserved:8` | One word names a row |
| 6 | The Keystone word is atomic `uint64_t`; persisted formats use shift/mask, **never** compiler bitfields | No torn fields; portable on-disk layout |
| 7 | Ids outside the tuple header are zero-extended `uint64_t` | One id representation everywhere |
| 8 | Waystone is advisory: deleting it wholesale may cost performance, never a result | Learning is risk-free |
| 9 | Waystone is never authoritative — it chooses *where to look*, never *what is visible* | Absence needs no witness |
| 10 | No canonical in-memory tuple; consistency is page pin + latch discipline | No coherence cache to keep |
| 11 | Every tuple key is a unique 48-bit id carried only by the Keystone word, **never updatable**, never rebound except by `PURGE` (see Keystone) | Issue-once identity |
| 12 | The MVCC header is exactly 20 bytes and there is **no `xmax`** | One fact stored once |
| 13 | Every tuple is fixed-length; a disagreeing length is `Corruption`, never interpreted | Tuples never migrate |
| 14 | Var-heap values are immutable per version; `kVarHeap` pages are never relocated | MVCC correctness for free |

## Specifications

The design is specification-first: every subsystem has a spec carrying its decisions, its open questions, and its required tests. `docs/spec/heap-and-tuple.md` is authoritative for row storage — where it and another document disagree, it wins. `CLAUDE.md` carries the invariants and the version discipline.

| Subsystem | Spec |
|---|---|
| Row storage, pages, Keystone, invariants *(authoritative)* | `docs/spec/heap-and-tuple.md`, `docs/rules/rule-fixed-length-tuple.md`, `docs/spec/page.md` |
| Pattern-keyed access trails | `docs/spec/waystone-concpets.md` (`create-pattern-user-defined-patterns-v1.md` is withdrawn) |
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
| Scheduling; cross-core execution | `docs/spec/sched.md`, `docs/spec/crosscore.md` |
| Wire protocol | `docs/spec/protocol.md` |
| Backup and restore | `docs/spec/backup.md` |
| DDL (`ALTER TABLE`, `DROP TABLE`, bulk insert, transactional DDL) | `docs/spec/alter.md`, `docs/spec/drop-table.md`, `docs/spec/bulkinsert.md`, `docs/spec/ddl-transactional.md` |
| Id issue-once contract | `docs/rules/keystoneid-invariant.md`, `docs/rules/keystoneid-k0-findings.md` |
| C++ rules | `docs/rules/rules.md` |
| Concepts not yet decided to build | `docs/conceptnotes/` |
| Active work orders and ratifications | `instructions/v3.0.0/` |
| **What is missing, and what a restart loses** | **`docs/inflight/known-gaps.md`** |

## Terminology

**`pk` is a retired word** (operator, 2026-10-14). The 48-bit id a tuple's Keystone word carries is called the **tuple key** or the **tuple identity key** - two names for one thing - and never the *pk* or *primary key*: it is a row's identity - unique for the life of the relation, never updated, rebound only by `PURGE` - not a business key. New text in this repository - specs, work orders, the manual, code comments and messages - uses those names. A SQL or code token that already spells `pk` is an identifier and is not renamed by this rule; an older document that says *pk* means the tuple key.

## Glossary

The stone metaphor is deliberate — a *keystone* holds the structure up, a *waystone* guides the traveler without being the road, and a *cabin* is a place someone has actually been.

| Term | Meaning |
|---|---|
| **Keystone** | The 64-bit identity word every tuple carries: 48-bit id · 8-bit flags · 8 reserved bits. Everything that names a tuple names it by its Keystone id |
| **Tuple key / tuple identity key** | The 48-bit id in the Keystone word: a relation's unique, never-updated row identity. The two names mean the same thing; KDS does not call it a *pk* (see *Terminology*) |
| **Waystone** | The advisory store of trails, reached per pattern instance through `sys.patterns`. Droppable wholesale without changing any result |
| **Trail** | The recorded path of one pattern instance — the Keystones a previous execution touched, in execution order, with step tags and last-seen locations. A trail may replace a lookup, never a search |
| **Cabin** | A per-`(relation, column)` store, authoritative for the values queries have actually observed; entries hold tuple keys, not addresses. A **Bound Cabin** is the pinned, fully covering, logged variant that backs an assertion |
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

1. **Advisory acceleration** (trails, Cabins) and **self-managing structures** (the Cabin controller) — *built*; relayout stays shadow-only behind its gates and SUS-1.
2. **M3 Uniformity** — *closed 2026-09-28 at AT-S20*: ownership, placement and the cross-core pipeline retired; every statement runs on its session's core over shared memory (`instructions/v3.0.0/workorder-at-m3-uniformity.md`).
3. **Invariants in the engine** — the concept notes in `docs/conceptnotes/` (CN-1 to CN-6), in whatever order the operator rules; each waits on a measurement gate or an operator decision recorded in the note.
4. **Hands-off operation** — the workload inspectable (`sys.patterns`, `SHOW ACCESS`, `SHOW RELAYOUT`, `SHOW CABIN_OPTIMIZER`), every optimization evaluable before it acts, every action a flag or threshold. Who sits in the operator seat is left open; it is safe because everything in the loop is advisory or revocable by invariant, so the worst mistake costs performance, never correctness.

## What KDS is not

No CTEs or derived tables, no window functions, no cross-dialect SQL compatibility, no attempt to be a data warehouse. `GROUP BY` with `COUNT`/`SUM`/`MIN`/`MAX`/`AVG` is built; `HAVING` and sorted aggregate output are not. New relations are btree-only while SUS-1 stands. If your workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data with rules the data must obey, KDS is built for exactly that.

---

## Measurement

Every number comes from a results file under [`bench/`](bench/) that names its `git describe --tags` version, environment and caveats; [`bench/README.md`](bench/README.md) holds the rules a run is invalid without. In short: a release build rebuilt at the measured commit (Debug has reported the wrong sign), a block device and never tmpfs for the data file, a copy of the binary, the host's load recorded per cell, and a chosen port. `bench/v3.0.0/` is a fresh series; the v2-era files describe the engine AR0 replaced, and no v3 number is set beside them.

---

## Status

Under active development, specification-first, in v3.0.0. Start with `CLAUDE.md`, then `docs/spec/heap-and-tuple.md`.

**Read [`docs/inflight/known-gaps.md`](docs/inflight/known-gaps.md) before relying on anything** — the engine-wide list of what is missing and what a restart loses. Among its entries: the transaction lifetime ceiling bounds explicit transactions only, so a long autocommit statement still holds its view; a Cabin builds rarely on a busy instance, because its banking gate declines while anything is unresolved anywhere; and Part I of the physical optimizer is unreachable for every relation created since SUS-1. Cabin entry sets and the Cabin controller's managed state are memory-resident and rebuilt by traffic after a restart.

[`manual/`](manual/) is the user-facing surface, verified against code rather than against specs.

```bash
scripts/build.sh                   # build
scripts/test.sh                    # configure, build and run the full test suite
tools/ckdbs_cli.py --port 15432    # talk to a running instance
```
