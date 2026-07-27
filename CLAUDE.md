# CLAUDE.md — KDS Storage Engine

Guidance for AI development agents working on this repository. `KDS-DESIGN.md` is the authoritative design spec; this file summarizes it and adds working rules. When the two conflict, `KDS-DESIGN.md` wins — flag the conflict instead of guessing.

## What This Project Is

KDS is a **userspace OLTP-specialized database storage engine, written in C++ as a fresh project**. Its purpose is fast and *correct* OLTP performance through two engine-native mechanisms:

1. **Engine-driven physical page optimization** — runtime statistics reorganize where tuples physically live, not just how the optimizer plans.
2. **Metadata caching** — a bounded per-relation metadata pool and an automatic hint index accelerate repeated access patterns.

Target domain: financial systems. Feature scope is deliberately narrow; do not add general-purpose DBMS features unasked.

**Kernel integration is abandoned.** Earlier KDS iterations were a Linux kernel module; that direction is dead. Do not introduce kernel-module code, kernel headers, or kernel-only APIs. The old C codebase may be consulted as a design reference only.

## Core Architecture (summary — see KDS-DESIGN.md for full detail)

- **Pages:** 8 KB. Page IDs are unsigned 32-bit (`uint32_t`); 16 TB capacity = 2^31 pages; `0xFFFFFFFF` = invalid. Never signed.
- **Semi-sorted heap:** every heap page header holds an **immutable `min_key`**. A tuple with `id < min_key` can never be placed in that page — including by relayout. Tuples *within* a page are unordered. `min_key` immutability lets readers prune by key range without locks.
- **Keystone column:** every tuple's mandatory first column is one 64-bit word (the "Keystone word"): `id:40 | flags:8 | meta_handle:16`. Flags byte is the transaction byte (Oracle lock-byte style); tuple liveness status lives in the slot directory instead.
- **Per-relation metadata pool:** up to 65,536 fixed-length entries (addressed by the 16-bit `meta_handle`), stored in ordinary 8 KB pages, O(1) arithmetic addressing, eviction-managed. Serves relayout and statistics **only** — never the read path.
- **Read path:** the B+ tree over the heap is authoritative. The **hint index** (`(query_pattern_id, args)` → tuple locations across tables) is advisory: consumers validate the target (PK identity + MVCC visibility) and fall back to the B+ tree on mismatch. Hint indexes are ultimately auto-generated from query fingerprints; zero user administration.
- **Single-copy rule:** an identical tuple exists at most once in program memory, converged through a hash-table lookup.
- **MVCC:** tuple headers carry `xmin` / `xmax` / `undo_ptr`; slot directory entries carry their own flags (`DEAD`, etc.).

## Hard Invariants — never violate, never "temporarily" bypass

1. 8192-byte pages; `uint32_t` page IDs; `0xFFFFFFFF` reserved invalid.
2. `min_key` is immutable after page creation.
3. No tuple with `id < min_key(page)` in that page, ever.
4. Keystone column encoding is exactly `id:40 | flags:8 | meta_handle:16`; the word is read/written as an atomic `uint64_t` (CAS for updates); fields never tear.
5. On-disk encodings use **explicit shift/mask helpers only — compiler bitfields are forbidden** for any persisted format (layout is implementation-defined; KDS must be architecture-portable).
6. Ids stored outside the tuple header (B+ tree keys, `min_key`, hint entries, metadata back-refs) are zero-extended `uint64_t`; upper 24 bits are always 0.
7. Metadata pool and hint index are advisory: deleting either wholesale may cost performance but must never change query results.
8. Metadata pages are never on the normal read path.
9. At most one in-memory copy of an identical tuple.

## C++ Working Rules

- Fresh codebase: do not copy kernel-style C wholesale; port ideas, write idiomatic modern C++.
- Prefer a disciplined subset: RAII for every resource (page pins, latches, file descriptors); no raw `new`/`delete` in engine logic; `std::atomic<uint64_t>` for the super-column word.
- On-disk structs: `static_assert` sizes and offsets; fixed-width integer types only (`uint32_t`, `uint64_t`); explicit serialization, no `#pragma pack` reliance for format definition.
- Every size/offset constant gets a named `constexpr` with the derivation in a comment (e.g., entries-per-page chosen as a power of two for shift/mask addressing).
- Concurrency: document the lock/atomic protocol at the top of each subsystem file, as the previous codebase did. State what each lock protects and the acquisition order.
- Tests accompany every subsystem: page encode/decode round-trips, invariant checks (min_key rule, super-column tearing), and crash-consistency tests for WAL-touching code.

## Open Decisions — DO NOT assume or silently pick

- C++ standard/toolchain pin, build system, and test framework (propose, don't decide).
- Metadata pool eviction policy and eviction-vs-validation invalidation mechanism.
- Persistence class of metadata pages (WAL-logged vs unlogged).
- Heap page split policy / how new `min_key` boundaries are chosen.
- Hint index admission/eviction policy; per-template safety classification (trusted for unique lookups vs prefetch-only).
- MVCC identity semantics of the single-copy rule and memory reclamation (refcount vs epoch).
- I/O backend abstraction (plain O_DIRECT vs io_uring vs pluggable).

When work touches an open decision, stop and ask, or implement behind an interface that keeps every listed option viable.

## Documents

- `KDS-DESIGN.md` — authoritative design specification (confirmed decisions + invariants).
- `CLAUDE.md` (this file) — agent working guide. Update both when a decision lands; move items from Open Decisions into the architecture sections with the date.
