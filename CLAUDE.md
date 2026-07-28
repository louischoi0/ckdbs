# CLAUDE.md — KDS Storage Engine

Guidance for AI development agents working on this repository. `docs/heap-and-tuple.md` (referred to elsewhere by its historical name `KDS-DESIGN.md`) is the authoritative design spec; this file summarizes it and adds working rules. When the two conflict, `docs/heap-and-tuple.md` wins — flag the conflict instead of guessing. Waystone (`docs/waystone-concpets.md`, `docs/waystone-workplan.md`) amends and supersedes the metadata-pool parts of that spec as of 2026-07-28; where this file and the older spec sections disagree with Waystone, Waystone wins.

## What This Project Is

KDS is a **userspace OLTP-specialized database storage engine, written in C++ as a fresh project**. Its purpose is fast and *correct* OLTP performance through two engine-native mechanisms:

1. **Engine-driven physical page optimization** — runtime statistics reorganize where tuples physically live, not just how the optimizer plans.
2. **Waystone + hint index** — a full-coverage, pk-addressed per-relation access-tracking structure (Waystone, superseding the earlier bounded metadata pool) and an automatic hint index accelerate repeated access patterns.

Target domain: financial systems. Feature scope is deliberately narrow; do not add general-purpose DBMS features unasked.

**Kernel integration is abandoned.** Earlier KDS iterations were a Linux kernel module; that direction is dead. Do not introduce kernel-module code, kernel headers, or kernel-only APIs. The old C codebase may be consulted as a design reference only.

## Core Architecture (summary — see `docs/heap-and-tuple.md` for full detail)

- **Pages:** 8 KB. Page IDs are unsigned 32-bit (`uint32_t`); 16 TB capacity = 2^31 pages; `0xFFFFFFFF` = invalid. Never signed.
- **Semi-sorted heap:** every heap page header holds an **immutable `min_key`** plus a mutable **epoch counter** (bumped on relayout/page rebuild; new 2026-07-28, storage location still `[OPEN]` — see `docs/heap-and-tuple.md` §3.1a). A tuple with `id < min_key` can never be placed in that page — including by relayout. Tuples *within* a page are unordered. `min_key` immutability lets readers prune by key range without locks.
- **Keystone column:** every tuple's mandatory first column is one 64-bit word (the "Keystone word"): `id:40 | flags:8 | reserved:16` (amended 2026-07-28; field was `meta_handle:16`, now retired — write 0, ignore on read; repurposing is `[OPEN]`). Flags byte is the transaction byte (Oracle lock-byte style); tuple liveness status lives in the slot directory instead.
- **Waystone:** per-relation, full-coverage access-tracking structure that supersedes the old bounded metadata pool (confirmed 2026-07-28, `docs/waystone-concpets.md`). Every tuple of a `waystone_enabled` relation gets a 32-byte entry addressed directly by `id` (`page_id = id >> 8`, `slot = id & 0xFF`) through a per-relation page directory — no eviction, no handle. Serves relayout and statistics **only** — never the read path. Entry existence is guaranteed 100%; recorded location is advisory, validated against the heap page's epoch counter. **Waystone-enabled relations require system-generated, autoincrement `id`s — callers must not supply their own pk on insert**, since pk-direct addressing depends on a dense, monotonically issued id sequence (see `docs/heap-and-tuple.md` §4).
- **Read path:** the B+ tree over the heap is authoritative. The **hint index** (`(query_pattern_id, args)` → tuple locations across tables) is advisory: consumers validate the target (PK identity + MVCC visibility) and fall back to the B+ tree on mismatch. Hint indexes are ultimately auto-generated from query fingerprints; zero user administration.
- **Page-latch consistency (confirmed 2026-07-28, supersedes the former single-copy rule):** there is no enforced single canonical in-memory tuple. Consistency is kept per-page: a page frame is pinned and latched (shared for reads, exclusive for structural mutation) for the duration of an access, and tuple bytes are read/written directly within that frame. Latching is core-local (thread-per-core/shared-nothing, `docs/rules.md` §3) — it serializes cooperative tasks on the owning core, it is not a cross-core lock. Executors may still hold private working copies of tuple data for processing; these are ephemeral projections, not competing canonical copies.
- **MVCC:** tuple headers carry `xmin` / `xmax` / `undo_ptr`; slot directory entries carry their own flags (`DEAD`, etc.).
- **Wire protocol (KWP/1):** confirmed 2026-07-28, `docs/protocol.md` + `docs/protocol-wp.md`. Replaces the current newline text protocol (kept only as an off-by-default loopback debug surface) with length-prefixed binary frames, a version/capability handshake, and an extended PARSE/BIND/EXECUTE statement model over server-side statement/portal handles. All-binary little-endian encoding; results stream as row batches with portal suspension; durability class is a per-transaction protocol field; cross-core access is server-side forwarding (clients are topology-unaware).

## Hard Invariants — never violate, never "temporarily" bypass

1. 8192-byte pages; `uint32_t` page IDs; `0xFFFFFFFF` reserved invalid.
2. `min_key` is immutable after page creation.
3. No tuple with `id < min_key(page)` in that page, ever.
4. Keystone column encoding is exactly `id:40 | flags:8 | reserved:16` (amended 2026-07-28; field was `meta_handle:16`); the word is read/written as an atomic `uint64_t` (CAS for updates); fields never tear.
5. On-disk encodings use **explicit shift/mask helpers only — compiler bitfields are forbidden** for any persisted format (layout is implementation-defined; KDS must be architecture-portable).
6. Ids stored outside the tuple header (B+ tree keys, `min_key`, hint entries, Waystone back-refs) are zero-extended `uint64_t`; upper 24 bits are always 0.
7. Waystone and hint index are advisory: deleting either wholesale may cost performance but must never change query results.
8. Waystone pages are never on the normal read path.
9. **(Revised 2026-07-28)** No single canonical in-memory tuple is enforced; consistency comes from page pin + latch discipline instead (shared latch for reads, exclusive latch for structural mutation).
10. **(New 2026-07-28)** A relation with `waystone_enabled` set requires system-generated, autoincrement `id` values; a caller-supplied pk on insert into such a relation is a defect, not a feature.

## C++ Working Rules

- Fresh codebase: do not copy kernel-style C wholesale; port ideas, write idiomatic modern C++.
- Prefer a disciplined subset: RAII for every resource (page pins, latches, file descriptors); no raw `new`/`delete` in engine logic; `std::atomic<uint64_t>` for the Keystone word.
- On-disk structs: `static_assert` sizes and offsets; fixed-width integer types only (`uint32_t`, `uint64_t`); explicit serialization, no `#pragma pack` reliance for format definition.
- Every size/offset constant gets a named `constexpr` with the derivation in a comment (e.g., entries-per-page chosen as a power of two for shift/mask addressing).
- Concurrency: document the lock/atomic protocol at the top of each subsystem file, as the previous codebase did. State what each lock protects and the acquisition order.
- Tests accompany every subsystem: page encode/decode round-trips, invariant checks (min_key rule, Keystone-word tearing), and crash-consistency tests for WAL-touching code.

## Open Decisions — DO NOT assume or silently pick

*(2026-07-28: the former "metadata pool eviction policy" and "eviction-vs-validation invalidation mechanism" items are removed — full-coverage Waystone has no admission/eviction to decide. Replaced by the Waystone-specific opens below, from `docs/waystone-concpets.md` §11.)*

- C++ standard/toolchain pin, build system, and test framework (propose, don't decide).
- Persistence class of Waystone pages (WAL-logged vs unlogged; on unlogged loss, rebuild = backfill).
- Heap page split policy / how new `min_key` boundaries are chosen.
- Per-page epoch counter storage (header field vs core-local table) and epoch width/wraparound.
- Repurposing of the freed 16 Keystone `reserved` bits.
- Id-reuse / low-range reclamation policy for high-churn Waystone-enabled relations.
- Waystone decay function and cadence (halving vs EWMA; tick period).
- Waystone ring sampling policy under pressure (drop-newest vs drop-oldest vs probabilistic).
- Waystone directory depth growth protocol details (root relink ordering vs concurrent probes).
- Hint index admission/eviction policy; per-template safety classification (trusted for unique lookups vs prefetch-only).
- MVCC version identity semantics (identity per version vs per logical tuple) — independent of the retired single-copy rule.
- Buffer-pool page-frame reclamation policy (pin refcount vs epoch-based eviction) under the page-latch consistency model.
- I/O backend abstraction (plain O_DIRECT vs io_uring vs pluggable).
- KWP/1 TLS activation phase and mode (direct vs upgrade); SCRAM parameters (`docs/protocol.md` §14).
- KWP/1 `kMaxFrame`, default row-batch size target, session/portal idle-timeout defaults.
- KWP/1 `DECIMAL` wire encoding (depends on the not-yet-ported type system); additional wire types.
- KWP/1 compression capability; credit-based flow-control capability; topology/smart-routing extension.
- KWP/1 auth→authorization model (roles/permissions) — the protocol only reserves the handshake stage today.

When work touches an open decision, stop and ask, or implement behind an interface that keeps every listed option viable.

## Documents

- `docs/heap-and-tuple.md` — authoritative design specification (confirmed decisions + invariants); historically referred to as `KDS-DESIGN.md`.
- `docs/rules.md` — normative C++ coding rules; historically referred to as `CPP-RULES.md`.
- `docs/sched.md` — scheduling/reactor specification.
- `docs/waystone-concpets.md` / `docs/waystone-workplan.md` — Waystone spec (supersedes the metadata-pool sections of `docs/heap-and-tuple.md`, confirmed 2026-07-28) and its task breakdown.
- `docs/protocol.md` / `docs/protocol-wp.md` — KWP/1 wire protocol spec (confirmed 2026-07-28, supersedes the newline text protocol) and its task breakdown (`P01`-`P17`).
- `docs/overview.md` — retired duplicate of this file; see that file for the pointer back here.
- `CLAUDE.md` (this file) — agent working guide. Update both this file and the design spec when a decision lands; move items from Open Decisions into the architecture sections with the date.
