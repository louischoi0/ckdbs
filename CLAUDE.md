# CLAUDE.md — KDS Storage Engine

Working guide for AI development agents. `docs/heap-and-tuple.md` is the authoritative design spec; this file summarizes it and adds working rules. When the two conflict, the spec wins — flag the conflict instead of guessing.

## What This Project Is

KDS is a **userspace OLTP-specialized database storage engine written in C++**. Its purpose is fast and *correct* OLTP performance through two engine-native mechanisms:

1. **Engine-driven physical page optimization** — runtime statistics reorganize where tuples physically live, not just how the optimizer plans.
2. **Waystone** — a record of where a previous execution of a query pattern found its rows, so a repeated pattern can look directly instead of searching.

Target domain: financial systems. Feature scope is deliberately narrow; do not add general-purpose DBMS features unasked.

**KDS is userspace software.** Do not introduce kernel-module code, kernel headers, or kernel-only APIs.

## Core Architecture

See `docs/heap-and-tuple.md` for the full detail; this is the map.

- **Pages:** 8 KB. Page ids are `uint32_t`; 16 TB capacity = 2^31 pages; `0xFFFFFFFF` is invalid. Never signed.
- **Semi-sorted heap:** every heap page header holds an **immutable `min_key`** plus a mutable **epoch counter** (bumped when tuples on the page move; storage location `[OPEN]`). A tuple with `id < min_key` can never be placed in that page. Tuples *within* a page are unordered. `min_key` immutability is what lets readers prune by key range without locks. **A relation is a chain of heap pages** linked through `next_page_id` and rooted at `sys.tables.desc_page_id` (`include/kds/storage/heap/heap_chain.hpp`): inserts go to the tail, a full tail grows the chain by one page whose `min_key` is the id that caused the growth, and nothing is split or moved — so each page's ids sit entirely below the next page's `min_key`. The **heap page split policy is open**, and there is no free-space reuse on earlier pages until page compaction exists.
- **Clustered B+ tree:** the alternative storage form, chosen per relation at `CREATE TABLE` (`clustered_type`). A **leaf is a heap page**, so the row codec, `PageView` reads and overwrites, and `HEAP_INSERT` redo all work on either. On a btree relation a pk point lookup descends and the descent is **authoritative** — a miss means the row does not exist. A heap relation has no pk index and scans the chain.
- **Keystone column:** every tuple's mandatory first column is one 64-bit word: `id:40 | flags:8 | reserved:16`. The flags byte is the transaction byte (Oracle lock-byte style); tuple liveness lives in the slot directory. The reserved 16 bits are written 0 and ignored; repurposing is `[OPEN]`.
- **Waystone:** `(pattern_id, arg_hash)` — `patternX(a, b)` — maps to the Keystones a previous execution of that pattern instance found, **across relations, in one page** (`docs/waystone-concpets.md`). Patterns are catalog objects in `sys.patterns`. The trust model is forced by invariant 9, not chosen: **a waystone may replace a *lookup*, never a *search*.** A stored set trusted as complete would be authoritative, and a set missing a row inserted since it was recorded is wrong in a way no per-tuple validation can detect, because absence has no witness. So a pk-equality step or a nested-loop pk probe may be served from a trail; a non-pk predicate, range, or scan still searches, and the trail may only prefetch for it. Every replayed entry is validated — `rel_oid` plus the Keystone id at the recorded `(page_id, slot)`, then the page epoch, then MVCC visibility exactly as the authoritative path would apply it — and any miss falls through for that step alone. **Status: the fingerprint (`include/kds/parser/fingerprint.hpp`), `sys.patterns`, the waystone page format (`include/kds/stats/waystone.hpp`) and the `arg_hash` directory (`include/kds/stats/waystone_dir.hpp`) exist; nothing records or replays a trail.** The directory is a hash directory, so deepening it preserves only the mappings whose new top digit is zero — growth is a cache flush, safe by invariant 8 and self-healing on the next execution (`docs/waystone-concpets.md` §5).
- **Page-latch consistency:** there is no enforced single canonical in-memory tuple. Consistency is kept per-page: a frame is pinned and latched (shared for reads, exclusive for structural mutation) for the duration of an access, and tuple bytes are read and written directly within it. Latching is core-local (thread-per-core/shared-nothing, `docs/rules.md` §3) — it serializes cooperative tasks on the owning core, it is not a cross-core lock. Executors may hold private working copies; these are ephemeral projections, not competing canonical copies.
- **WAL:** physiological redo log, one stream per core, LSN = stream-local byte offset (`docs/wal.md`). Durability classes D1 strict / D2 group / D3 relaxed are a per-transaction property. WAL-before-data is store-enforced: a frame is not flushed until `durable_lsn ≥ page_lsn` (`DevicePageStore::SetWalGate()`, applied in `Flush`/`Sync`/`FlushPages`). Superblock anchors hold a `kMaxWalCores`-slot table indexed by `core_id` carrying `{checkpoint_lsn, redo_start_lsn, durable_lsn, segment_no}`; the checkpointer publishes into it only after `CHECKPOINT_END` is durable, and an all-zero entry means "replay from the start of the stream". **INSERT is the one logged statement**: `TXN_BEGIN` → (`FULL_PAGE_IMAGE` of the old tail + `PAGE_INIT` of the new one, only on chain growth) → `HEAP_INSERT` → `TXN_COMMIT`, with the class set by the `durability` config key (default `group`). The FPI is there because growth mutates the *predecessor's* `next_page_id` and no record type describes a link edit; it costs about +50% log volume on small rows, and a `HEAP_CHAIN_LINK` record type would retire it. Measured on an EBS gp3 volume, single connection: 802 inserts/s strict, 798 group (a batch of one is a batch), 6,332 relaxed, against 7,673 unlogged. **`CREATE TABLE`, `UPDATE` and all catalog rows are unlogged, and recovery is not implemented** — nothing reads the log back, so a restart is protected only by `PageStore::Sync()` at `SYNC` or clean shutdown.
- **MVCC:** tuple headers carry `trx_id` (48-bit writer) / `undo_ptr` / `data_len` / flags — 20 bytes, **no `xmax`**: the undo chain encodes validity intervals, so the boundary is never stored twice. DELETE is a delete-mark — slot flag `DELETED` plus the deleter's `trx_id` — distinct from physical slot retirement (`DEAD`).
- **Transactions (`docs/txn.md`) — specified, not built.** There is no `TransactionManager` and no `Session` in the tree; every row the dispatcher writes is stamped `kBootstrapXid`. What the spec settles: exactly two isolation levels — `READ COMMITTED` (default; read view per statement) and `REPEATABLE READ` (read view per transaction). `SERIALIZABLE` is out of scope, not open. Undo is InnoDB-shaped: headered `PageType::kUndo` pages, a 24-byte page header, 28-byte unpadded records, `undo_ptr = (page_id << 16) | offset` with 0 meaning "no predecessor". **INSERT writes no undo record** — `undo_ptr == 0` plus an invisible writer already means "no visible version" — so insert cost is unchanged and rollback of an insert uses an in-memory trail. **`trx_id == 1` (`kBootstrapXid`) is visible to every read view, permanently**, which is what makes every catalog row readable; `next_trx_id` starts at 2 and never reissues it. **Write conflicts are first-updater-wins** — no lock manager, no waiting, no deadlock detection; the loser gets `StatusCode::kTxnConflict`, mapping to the wire contract `TxnConflict`/`retryable=1`. **Version identity is per logical tuple**, forced by the pk being unupdatable, `OverwriteTuple` keeping `(page_id, slot)`, and undo versions having no address. **Known gap the spec accepts (`docs/txn.md` §8): MVCC is designed to ship before recovery, so an uncommitted row surviving a crash would read as *committed* on the next boot** — its `trx_id` is below the new high-water mark and in no live set. Closing it requires a persisted commit watermark, i.e. recovery.
- **Catalog cache** (`include/kds/catalog/catalog_cache.hpp`): catalog reads are served from a core-local in-memory cache instead of re-scanning a catalog page per statement — `name → oid`, `oid → TableAccess`, `pattern_id → PatternAccess`, the `sys.types` snapshot, and the table list. One question decides what may live there: *can the fact change without DDL?* If yes, it is not cacheable. Three rules follow. (1) **Sequences and heat are never cached** — `GetSysTableRow()`/`AllocateRowId()` and `GetSysPatternRow()` always read the page, which is what lets a statement hold a cached `TableAccess` across its own insert. (2) **Absences are never cached**, which is also why registering a pattern invalidates nothing and is safe on the statement path. (3) **One invalidation choke point**, `Catalog::BumpVersion()`, which also advances `Catalog::catalog_version()` — the counter `docs/parser.md` I5 stamps bound statements with. The single exception is `SetPatternWaystoneRoot()`, which updates its cached entry in place: the fact belongs to one pattern and is read by nothing else, so a global drop would dangle every other held pointer for nothing. Coherency is **instance-scoped**: two `Catalog`s over one `PageStore` do not see each other's DDL, which is sound only while catalog mutation stays append-only and dies with `DROP`/`ALTER TABLE`.
- **Wire protocol (KWP/1)** (`docs/protocol.md`, `docs/protocol-wp.md`): length-prefixed binary frames, a version/capability handshake, and an extended PARSE/BIND/EXECUTE model over server-side statement and portal handles. All-binary little-endian; results stream as row batches with portal suspension; durability class is a per-transaction protocol field; cross-core access is server-side forwarding, so clients are topology-unaware. **Status: only the frame codec exists** (`include/kds/wire/kwp.hpp`, `src/wire/frame_codec.cpp`), and nothing calls it — the server speaks the newline text protocol (`docs/client-manual.md`), which KWP/1 demotes to an off-by-default loopback debug surface.
- **Parser** (`docs/parser.md`, tasks in `docs/parser-workplan.md`): runs at KWP `C_PARSE` only, never per execution. Three properties define it: (1) **literals are parameterized at parse time** — the AST holds literal-table slot indices, never values, so `pattern_id` falls out of the parse itself and inline-literal and bind-parameter forms converge to one fingerprint; (2) **every statement carries an execution-class tag** that the executor `switch`es on — there is no plan search; (3) **join order is written order** — "the query is the plan", a documented client contract, never silently reordered. Also: flat index-based AST in a per-session arena (zero alloc), `string_view` tokens over the KWP frame (zero copy), catalog binding at PARSE with DDL version-stamp invalidation, and 40-bit pk range enforcement at the front door. **Status: the parser in the tree is a recursive-descent implementation with an owning AST, a copying lexer and no catalog binding.** The one blueprint property that exists is the fingerprint, built as a separate pass over the same lexer for Waystone.

## Hard Invariants — never violate, never "temporarily" bypass

Numbered to match `docs/heap-and-tuple.md` §8.

1. 8192-byte pages; `uint32_t` page ids; `0xFFFFFFFF` reserved as invalid.
2. `min_key` is immutable after page creation.
3. No tuple with `id < min_key(page)` in that page, ever — including by relayout.
4. Tuples within a heap page are unordered.
5. The Keystone column is exactly `id:40 | flags:8 | reserved:16`.
6. The Keystone word is read and written as an atomic `uint64_t` (CAS for updates) and its fields never tear; on-disk encodings use **explicit shift/mask helpers only — compiler bitfields are forbidden** for any persisted format, since their layout is implementation-defined and KDS must be architecture-portable.
7. Ids stored outside the tuple header (B+ tree keys, `min_key`, Waystone entries) are zero-extended `uint64_t`; the upper 24 bits are always 0.
8. Waystone is advisory: deleting it wholesale may cost performance but must never change a query result. This is the invariant that matters.
9. Waystone is never **authoritative**. A reader may consult it for *where to look*, provided it (a) treats a missing or stale entry as a miss, (b) checks the Keystone id of the tuple actually at the reported `(page_id, slot)` and treats a mismatch as a miss too, (c) applies MVCC visibility exactly as the authoritative path would, and (d) falls through to that path — a btree descent on a btree relation, a chain scan on a heap one — on any of those. It chooses where to look, never what is visible.
10. No single canonical in-memory tuple is enforced; consistency comes from page pin and latch discipline (shared latch for reads, exclusive for structural mutation).
11. **Every** relation requires system-generated, autoincrement `id` values — a caller-supplied pk on insert is a defect, not a feature. The sequence is persistent (`sys.tables.next_id`, issued by `Catalog::AllocateRowId()`), never derived as `max(id)+1`, which would hand a new tuple the identity of a deleted one. Ids are unique and monotonic, not gapless. The pk is carried only by the Keystone word — never also as a body column — and cannot be updated: it is the tuple's identity, not a field of it. A relation's first column must be declared with an integer type (`catalog::CheckKeystoneColumn`), checked at `CREATE TABLE`.
12. The tuple MVCC header is exactly `trx_id:48 (zero-extended to 64) | undo_ptr | data_len | flags` = 20 bytes. There is no `xmax`.

## C++ Working Rules

- Fresh codebase: do not copy kernel-style C wholesale; port ideas, write idiomatic modern C++.
- Prefer a disciplined subset: RAII for every resource (page pins, latches, file descriptors); no raw `new`/`delete` in engine logic; `std::atomic<uint64_t>` for the Keystone word.
- On-disk structs: `static_assert` sizes and offsets; fixed-width integer types only (`uint32_t`, `uint64_t`); explicit serialization, no `#pragma pack` reliance for format definition.
- Every size/offset constant gets a named `constexpr` with the derivation in a comment (e.g., entries-per-page chosen as a power of two for shift/mask addressing).
- Concurrency: document the lock/atomic protocol at the top of each subsystem file, as the previous codebase did. State what each lock protects and the acquisition order.
- Tests accompany every subsystem: page encode/decode round-trips, invariant checks (min_key rule, Keystone-word tearing), and crash-consistency tests for WAL-touching code.

## Open Decisions — DO NOT assume or silently pick

**Storage**
- Heap page split policy — how a full page's contents are divided and the new `min_key` chosen. Chain growth by tail append deliberately avoids deciding it. Free-space reuse on non-tail pages and page compaction are open with it, and both need reader registration first.
- Per-page epoch counter storage (header field vs core-local table), width, and wraparound.
- Repurposing of the 16 reserved Keystone bits.
- Id-reuse / low-range reclamation for high-churn relations.
- Buffer-pool page-frame reclamation policy (pin refcount vs epoch-based eviction) under the page-latch model.
- I/O backend abstraction (plain `O_DIRECT` vs `io_uring` vs pluggable).
- Whether to relax invariant 3 (`min_key`) or invariant 11 (system-generated ids). Waystone no longer needs a dense, monotonically issued pk sequence, so arbitrary pk values are *possible* — but `min_key` exists for lock-free range pruning and `next_id` for tuple identity, and neither was ever about Waystone. Permitted by the design, not performed by it.

**Waystone** (`docs/waystone-concpets.md` §9)
- Retention and eviction per pattern. The catalog bounds patterns; nothing yet bounds *instances* per pattern.
- Recording policy: every execution, sampled, or after *n* sightings.
- Persistence class of waystone pages (WAL-logged vs unlogged).
- `arg_hash` collision handling beyond the header check.
- Decay function and cadence; ring sampling policy under pressure.
- Whether invariant 9 is ever amended to permit trusting a cached result set as complete — which needs a per-relation change stamp bumped at *commit*, not at write.

**Transactions and WAL** (`docs/txn.md` §9, `docs/wal.md` §15)
- Undo retention policy and `SnapshotTooOld` surfacing (error class, retryability). Structurally unreachable today: nothing purges, because readers are deliberately not registered.
- 48-bit `trx_id` wraparound and epoch handling. Exhaustion is `OutOfRange`, never wrapped.
- Cross-core transaction commit protocol; recovery under a changed core count.

**Protocol** (`docs/protocol.md`)
- TLS activation phase and mode (direct vs upgrade); SCRAM parameters.
- `kMaxFrame`, default row-batch size target, session and portal idle-timeout defaults.
- `DECIMAL` wire encoding (depends on the not-yet-ported type system); additional wire types.
- Compression capability; credit-based flow control; topology/smart-routing extension.
- The authorization model (roles, permissions). The protocol only reserves the handshake stage.

**Parser** (`docs/parser.md`)
- Aggregates (`COUNT`/`SUM`, `GROUP BY`) — exclude from the grammar entirely, or reserve the keywords and reject with `Unsupported`. Do not implement either path.
- Ratification of the `[PROPOSED]` statement-class list; per-statement literal-table size cap; whether `kUnclassified` is permitted in production builds or gated.

**Project**
- C++ standard/toolchain pin, build system, and test framework (propose, don't decide).

When work touches an open decision, stop and ask, or implement behind an interface that keeps every listed option viable.

## Documents

- `docs/heap-and-tuple.md` — **authoritative design specification**: pages, the semi-sorted heap, the Keystone column, page-latch consistency, and the numbered invariants.
- `docs/rules.md` — normative C++ coding rules.
- `docs/sched.md` — the reactor and scheduling groups.
- `docs/page.md` — page allocation, the buffer pool, file layout, and the I/O path.
- `docs/waystone-concpets.md` / `docs/waystone-workplan.md` — Waystone: the pattern-keyed access trail, and its task breakdown (`P01`-`P17`, six phases).
- `docs/txn.md` — transactions and MVCC: isolation levels, undo page layout, the visibility predicate, first-updater-wins, rollback, and the ship-before-recovery gap.
- `docs/wal.md` — the write-ahead log: record set, durability classes, checkpointing, and the recovery that is not yet implemented.
- `docs/parser.md` / `docs/parser-workplan.md` — the parser blueprint and its 4-phase task breakdown (`PR01`-`PR24`).
- `docs/protocol.md` / `docs/protocol-wp.md` — the KWP/1 wire protocol and its task breakdown. Note both this workplan and Waystone's use `P01`-`P17`; cite the file, not the bare number.
- `docs/client-manual.md` — the newline text protocol the server speaks today, and the config keys.
- `docs/observability.md` — **a proposal, nothing implemented**: per-request tracing and inspection. A development surface, distinct from `docs/wal.md` §13's operator metrics.
- `kds.conf.sample` — commented template for the server config file (`--config`). Precedence: defaults → file → flags. An unknown key is a startup error.
- `CLAUDE.md` (this file) — the agent working guide. When a decision lands, update this file and the spec that owns it, and move the item out of Open Decisions.
