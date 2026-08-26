# KDS C++ Coding Rules

Normative coding rules for the KDS storage engine. Rules only: design rationale and architecture live in `docs/spec/heap-and-tuple.md`, and the agent working guide is `CLAUDE.md`. Every rule here is binding; the final section lists rule areas that are **not yet decided** — do not invent policy for them.

---

## 1. Error Handling

- `throw` is **forbidden** everywhere in the engine. Exceptions are disabled at build level once the toolchain is pinned.
- Every fallible function returns the KDS explicit error/status type (RocksDB-`Status`-style). `void`-returning functions must be infallible by construction.
- Constructors must not fail. Fallible construction goes through static factory functions returning `status_or<T>` (or equivalent).
- Error values must be checked or explicitly discarded; silently dropping a status is a defect.

## 2. Page Buffer Access

- All access to on-disk page bytes goes through **field-wise `memcpy` helpers** (Option A).
- `reinterpret_cast` of struct types onto page buffers is **forbidden without exception** — it is undefined behavior and is not acceptable even as a temporary measure.
- For each on-disk record, define a mirror struct; derive field offsets with `offsetof` and pin them with `static_assert`; expose typed `get_*` / `set_*` helpers that `memcpy` through those offsets.
- Do not bypass the helpers for "performance": small fixed-size `memcpy` compiles to plain loads/stores.

## 3. Threading

- The engine is **thread-per-core, shared-nothing**. Every piece of engine state has exactly one owning core; only the owning thread touches it.
- Cross-core communication uses explicit message/queue interfaces. Shared mutable state across cores is forbidden.
- Locks are a last-resort mechanism at partition boundaries. Any lock requires a justification comment in the subsystem header stating what it protects and its acquisition order.
- The tuple super-column word is manipulated only via `std::atomic<uint64_t>` operations (CAS for updates); fields within the word must never tear.

## 4. Deterministic Testability

- Deterministic simulation is a first-class constraint. All of the following must go through injectable interfaces: file/disk I/O, wall-clock and monotonic time, randomness, and cross-core messaging.
- Direct syscalls, `std::chrono` reads inside engine logic, and ad-hoc thread creation are forbidden outside the platform layer.
- The entire engine must be runnable single-threaded under a simulated scheduler with fault injection (I/O errors, torn writes, message reordering).
- Every subsystem ships tests: encode/decode round-trips, invariant checks (`min_key` rule, super-column tearing), and crash-consistency tests for anything touching the WAL.

## 5. On-Disk Format Rules

- Fixed-width integer types only (`uint32_t`, `uint64_t`, ...) in any persisted structure.
- **Compiler bitfields are forbidden** in on-disk formats. Packed fields (e.g., the Keystone column `id:40 | flags:8 | reserved:16`; see `docs/spec/heap-and-tuple.md` §4) are encoded/decoded with explicit shift/mask `constexpr` helpers.
- Every persisted struct has `static_assert`s for its total size and each field offset.
- Page IDs are unsigned 32-bit; `0xFFFFFFFF` is the invalid sentinel; page IDs never appear in signed types.
- Tuple ids stored outside the Keystone column (B+ tree keys, `min_key`, hint entries, metadata back-references) are zero-extended `uint64_t`; upper 24 bits are always 0.
- Every size/offset constant is a named `constexpr` with its derivation in a comment (e.g., entries-per-page is a power of two for shift/mask addressing).
- **Tuples are fixed-length** (`docs/spec/heap-and-tuple.md` §3.3, invariant 13). A relation's row size is a schema constant and cell offsets are computed from the schema, never scanned for: no code path may emit a tuple of a different size, and the row codec `static_assert`s or checks the constant rather than trusting a caller. A variable-width value occupies one tagged cell of `kds.inline_cell_width` bytes — tag byte first, never a sentinel value — and spilling to the var-heap changes the cell's *tag*, never the tuple's size.
- A `length` or `data_len` field that duplicates a schema constant is **checked redundancy**: compare it, report `Corruption` on disagreement, and never compute from it.

## 6. General C++ Rules

- Idiomatic modern C++ throughout. KDS is userspace software; kernel-module code, kernel headers, and kernel-only APIs have no place in it.
- RAII for every resource: page pins, latches, file descriptors. No raw `new` / `delete` in engine logic.
- Each subsystem file begins with a comment documenting its concurrency protocol: what is core-local, what crosses cores, and any boundary locks with their ordering.
- No kernel headers, kernel-module code, or kernel-only APIs anywhere in the tree.

## 7. Undecided Rule Areas — do not invent policy

- C++ standard pin (C++20 minimum), toolchain versions, build system, test framework.
- Allocator policy: arena/pool design, global allocator choice, allocation-failure handling boundary.
- STL usage scope in hot paths and third-party dependency policy.
- Language feature whitelist: virtual dispatch in hot paths, template complexity limits. **Coroutines are decided** (2026-08-05): C++20 stackless coroutines are the task representation (`docs/spec/sched.md` §3, `include/kds/sched/coro.hpp`). They are permitted for *suspendable* work — a statement, a cross-core request, a lease — and not on the per-tuple path, because a frame is a heap allocation. A coroutine promise needs `unhandled_exception()`; ours does nothing, since §-level no-exceptions still holds and a throw should fail at its own site.
- Release-build invariant checking tiers and fail-fast policy on corruption.
- Platform pin (x86-64 Linux only vs portable) and its consequences for intrinsics/endianness rules.

When code touches an undecided area, ask, or hide the choice behind an interface that keeps all options open.
