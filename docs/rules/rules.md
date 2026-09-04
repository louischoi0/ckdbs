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

- The engine is **thread-per-core**, and **core-local state is the default** (AR0-2). Most engine state has exactly one owning core and only that thread touches it; a subsystem that departs from this says so in its own spec, names what is shared and names what serializes it.
- **Shared-nothing is no longer the rule it was.** "Shared mutable state across cores is forbidden" was the rule until AR0 M0; it is now "shared mutable state across cores is *declared*". A structure that is shared and undeclared is a defect, and so is a lock in a subsystem whose spec does not carry one — the change widened what may be shared, not who may decide it. **The declaration lives in the owning spec, and this list is an index of them rather than the authority**:

  | shared | serialized by | declared in |
  |---|---|---|
  | The **WAL** — one stream per instance | the stream latch, `wal/stream.hpp`'s stated order | `docs/spec/wal.md` §3 |
  | The reactor **wake flag** and the `Waker` | atomics; a missed wake costs latency, never liveness | `docs/spec/sched.md` §7 |
  | The **data-file device**'s capacity, grown by core 0 and read from every store | core 0 alone writes; readers see a monotone value | `docs/spec/crosscore.md` CC11, `docs/spec/page.md` §6 |
  | The **lock table** — the lock family's partitioned table, one for the instance (**M2; the structure exists at AO-S1, no core constructs one until AO-S3**) | a `base/latch.hpp` latch per partition, null at `cores = 1`; a striped `S`/`X`-fence counter per relation is atomic and read without the latch; taken with no page latch, no WAL latch and no window latch held, one partition at a time, released before any park — **stated, not enforced: the census that arms the order is AO-S3's**, when callers exist to violate it | `include/kds/txn/lock_table.hpp`; `instructions/v3.0.0/workorder-ao-m2-lock-family.md` AO-R2 until AO-S8 moves it to `docs/spec/txn.md` §5 |
  | A **page frame's latch word** — one `uint32` per resident frame, armed only at `cores > 1` (AR0 M1, AM-S1) | CAS on the word (`storage/page_latch.hpp`): shared readers, one exclusive owner core, re-entrant for that core, never upgraded; **outer** to the WAL stream latch (a task appends holding its page latches; no WAL path asks for a page latch while holding the stream latch — recovery's redo is the one place `wal/` touches a frame, and it holds no stream latch); never nested with the window latch; never held across a park; held across a durability wait only on the fault path, which the writer thread's taking no page latch keeps sound; **page against page unordered through M1** (one core owns its pool, so two holders never wait on each other) and AM-S2's to state for the shared pool | `docs/spec/page.md` §6; `device_page_store.hpp`'s "The page latch" section |
  | The **visibility window** — the instance read view's commit-LSN window, one for the instance (AN-S1) | the window latch, a `base/latch.hpp` `Latch` (`txn/instance_visibility.hpp`): taken with the WAL stream latch released and never under it, holding no other latch, never across a suspension point | `instructions/v3.0.0/workorder-an-read-view.md` AN-R9 until AN-S4 moves it to `docs/spec/txn.md` §4.1 |

  Adding a row is a spec change first and a code change second — the lock table's row above is that order made visible: it was argued in its work order and declared here before a line of it existed. The first three rows were not a small number for an engine that called itself shared-nothing a week ago, and the point of writing them down is that every later one is argued for rather than noticed later.
- Cross-core *work* still moves only over the explicit message/queue interfaces (`docs/spec/sched.md` §5). Sharing a structure is never a licence to run another core's task on this thread.
- Locks stay a last-resort mechanism, and the justification comment is now the mechanism that keeps the declared list honest: any lock requires a comment in the subsystem header stating what it protects and its acquisition order, and the owning spec must say the same thing. Two locks held at once need a stated order between them; the WAL's three — the stream latch, the log device's segment-table lock taken under it, and the writer's wait mutex, taken only with the latch released — were the whole list until AR0 M1's page latch joined it (outer to the stream latch, never meeting the other two; `device_page_store.hpp` states the order) and AN's visibility window latch (taken with the stream latch released; `txn/instance_visibility.hpp`), and `wal/stream.hpp` and `wal/writer.hpp` state the WAL's order.
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
- **A catalog relation whose content is *only* a statistic is written unlogged.** A trail invariant 8 already prices as performance and never a result is neither redone nor undone, and a mount that finds it damaged **discards** it rather than repairing it or refusing to start. This is the **sole exception** to the rule that catalog writes are WAL-logged as ordinary record types and replayed (`docs/spec/ddl-transactional.md` §7). `sys.access_stats` is the only relation that qualifies today; adding a second requires showing it meets the same test, not that it resembles this one — and the test is the content's *class*, not its cost: a relation a reader may act on is not a statistic, however cheap it is to rebuild.

## 6. General C++ Rules

- Idiomatic modern C++ throughout. KDS is userspace software; kernel-module code, kernel headers, and kernel-only APIs have no place in it.
- RAII for every resource: page pins, latches, file descriptors. No raw `new` / `delete` in engine logic.
- Each subsystem file begins with a comment documenting its concurrency protocol: what is core-local, what crosses cores, and any boundary locks with their ordering.
- No kernel headers, kernel-module code, or kernel-only APIs anywhere in the tree.

## 7. Undecided Rule Areas — do not invent policy

The decisions for these areas are unrecorded here: the C++ standard pin (C++20 minimum), toolchain versions, build system and test framework; allocator policy (arena/pool design, global allocator choice, the allocation-failure handling boundary); STL usage scope in hot paths and third-party dependency policy; the language-feature whitelist (virtual dispatch in hot paths, template complexity limits); release-build invariant-checking tiers and the fail-fast policy on corruption; and the platform pin (x86-64 Linux only vs portable) with its consequences for intrinsics and endianness rules. When code touches one, ask, or hide the choice behind an interface that keeps all options open.

One feature in that list is decided: **C++20 stackless coroutines are the task representation** (`docs/spec/sched.md` §3, `include/kds/sched/coro.hpp`). They are permitted for *suspendable* work — a statement, a cross-core request, a lease — and not on the per-tuple path, because a frame is a heap allocation. A coroutine promise needs `unhandled_exception()`; ours does nothing, since §1's no-exceptions rule still holds and a throw should fail at its own site.
