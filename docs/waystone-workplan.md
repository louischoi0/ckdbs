# Waystone — Workplan (Actionable Task List)

**Status:** Official work instructions, companion to `waystone-concept.md`. The specification is **confirmed as of 2026-07-28**; nothing below waits on a design decision. Every task is scoped to be startable immediately — where a neighboring subsystem (executor, physical optimizer) does not exist yet, the task builds against a fixed seam or fixture named in the task itself, and the later integration is its own task. Remaining `[OPEN]` items in the spec are isolated behind interfaces; no task requires deciding one.

Execution rules:
- Do tasks in numeric order unless the "needs" column says otherwise; T01–T08 are the spec-consistency gate and land before any T09+ code merges.
- Each task ships with its listed tests in the same change; `bash test.sh` green is part of "done".
- If a task turns out to touch an `[OPEN]` item after all — stop, flag, do not decide.

---

## Amendment 2026-07-30 — seek-first ordering

Invariant 8 was amended (spec §3.1): a pk point lookup may probe Waystone under a validate-and-fall-back contract. That changes what is on the critical path, because the goal became **row-seek latency** rather than relayout statistics. The numbered tasks below are unchanged in content; the order they are executed in is not.

Critical path, in order — `W00` (this amendment, done) → `T08` → `T09` → `T10` → `T04`/`T12` → `T13` → `T16` → **`W07`** → measure.

`W07` is new and has no `T` number because the old plan had nowhere to put it: wire the probe into `HandleSelect` for `WHERE id = <n>`, validating per §3.1 and falling back to `ChainVisit`. Its tests are spec §12-3 in its amended form.

Deferred, and explicitly not needed for the seek win: `T11` (fingerprint), `T14` (aggregator/heat), `T15` (observer ring), `T17` (backfill), `T20` (pattern groups). Heat exists to drive relayout, and there is no physical optimizer to drive. `T13`'s insert hook is what keeps coverage complete, so no backfill is needed for relations that were enabled at creation — `T17` is only for enabling a relation that already holds rows.

### Blocker found in W03 — headerless pages vs. the store's checksum `[RESOLVED 2026-07-30, option 1]`

Resolved by teaching the store which pages are headerless. `DevicePageStore::CreateNewHeaderless()` allocates one and records the fact in a **durable** second bitmap page — `PageType::kHeaderlessMap` at `kHeaderlessMapPageId` (2), same one-bit-per-page-id format as the free map. `StampIfHeadered()` is the single point where the stamp decision is made, and the read-miss path skips verification for a marked page.

Durability is not a nicety: the store never evicts, so a page comes off the device exactly once — on first touch after open — and that is precisely when a verify would reject it. It cannot be recomputed from the catalog either, since the store is opened before bootstrap has a catalog to ask. Write order is headerless map, then free map: the free map is what makes an id exist, so a crash between them leaves a headerless bit set for an unallocated id (harmless — `IsHeaderless()` also requires allocation), where the reverse would publish a Waystone page whose bit had not landed.

Accepted cost, stated as a test (`DamageToAHeaderlessPageIsSilentByDesign`): these pages carry no damage detection. Right for an advisory structure — a wrong entry must be *survivable*, which the probe's Keystone-id check guarantees, and survivable is stronger than detectable. Bit-rot becomes a probe miss and a fallback scan, never a wrong answer.

### Original blocker text, kept for the reasoning

Waystone entry and directory pages are **headerless by class** (spec §5/§6): 256 × 32 B and 2048 × 4 B tile 8 KiB exactly, and a header would cost a slot and break the shift/mask addressing the whole design rests on. `page_header.hpp` says as much.

`DevicePageStore` does not know that. `Flush()` and `FlushPages()` call `StampPageChecksum()` on **every** dirty frame, which writes a `uint32_t` at byte offset 4 — on a headerless entry page that is the upper half of entry 0's `pk`. The read path is symmetric: `ResidentBytes()` calls `VerifyPageChecksum()` on every miss, so a reloaded Waystone page would come back `Corruption`. Both halves of the round trip are broken, silently on write and loudly on read.

This does not affect `T09`/`T10` — their tests run on `InMemoryPageStore`, which neither stamps nor verifies — but it blocks `T13` and `W07`, the first tasks that put a Waystone page in the server's real store. **Decide before starting W05.** Sketch of the options, in preference order, none picked:

1. **Teach the store which pages are headerless.** A per-store set, populated when the page is created; excluded from stamping and verification. Cheapest, localized, and it keeps the tiling. The cost is that these pages then carry *no* damage detection, which is arguably right for an advisory structure — a wrong entry must be survivable (the probe's Keystone-id check), which is stronger than being detectable — but it interacts with the persistence-class `[OPEN]` below and should be decided with it.
2. **A separate store or file for Waystone pages.** Clean separation, no changes to `DevicePageStore`, but a second allocator, a second free map, and a second thing to open at boot.
3. **Give Waystone pages a header.** Rejected on its face: 8160/32 = 255 entries is not a power of two, so `pk & 0xFF` addressing dies and every lookup gains a division. It would invalidate the derivations in spec §5/§6.

Two `[OPEN]`s stay open and both are isolated behind seams, per the standing instruction:

- **Heap-page epoch storage** — `EpochProvider` seam. The default implementation reports a constant epoch for every page, which is *correct* today because nothing moves a tuple, and wrong the day relayout lands. `BumpFor(page)` exists unimplemented (`T19`).
- **Waystone page persistence class** — the hooks write through `PageStore`; whether the write is also WAL-logged is one call site in `HandleInsert`. Unlogged means a crash loses entries, probes miss, and reads fall back to the scan: slower, never wrong. Do not pick without the decision landing in spec §11.

## Gate: spec & repo consistency

**T01 — Amend Keystone layout in the design spec.**
File: `docs/heap-and-tuple.md` §4. Change layout to `id:40 | flags:8 | reserved:16`; delete `meta_handle` semantics; add "writers set 0, readers ignore; repurposing [OPEN]"; stamp `Amended 2026-07-28`.
Done when: section shows the new layout with the dated amendment note.

**T02 — Supersede the bounded metadata pool in the design spec.**
File: `docs/heap-and-tuple.md` §5. Add a banner: superseded by Waystone full coverage → link `docs/waystone-concept.md`. Keep the old text below the banner for history. Remove any normative claim of the 65,536 cap elsewhere in the file.
Done when: banner + link present; grep for `65,536|65536|meta_handle` finds only historical/banner text.

**T03 — Add the heap-page epoch to the design spec.**
File: `docs/heap-and-tuple.md` §3. Define the per-page epoch counter (bumped whenever tuples on the page move); storage location marked `[OPEN]` (header field vs core-local table), with the note that a header field would be on-disk format under `docs/rules.md` §5.
Done when: epoch defined once, `[OPEN]` marker present.

**T04 — Extend the catalog spec for Waystone.** — **done 2026-07-30, together with T12.**
Files: catalog documentation + `include/kds/catalog/well_known.hpp` comments. Relation entry gains `waystone_enabled` (with distinct *coverage-complete* state) and `waystone_dir_root: PageId`. Defaults: disabled, `kInvalidPageId`.
Done when: fields documented with defaults and DDL ownership noted.

Landed as **three** fields, not two: `WaystoneState` (`kDisabled`/`kBackfilling`/`kCovered`), `waystone_dir_root`, and `waystone_dir_depth`. The depth is the addition the spec did not name, and it has to be persisted rather than derived from `next_id` — a derived depth would change the instant the sequence crossed a coverage boundary, which is *before* `GrowDirectory()` relinks the root, and every lookup in that window would walk the wrong number of levels onto the wrong leaf. `Catalog::SetWaystoneDirectory()` writes all three as one unit and refuses an inconsistent triple, so every reader downstream may trust it without re-checking.

**Format break.** `SysTableRow::kOnDiskSize` grew by 6 bytes and `Decode()` requires an exact match, so **data files written before this change no longer open** — `ERR catalog row size mismatch` on any `sys.tables` read. Fresh project, no compatibility promise: discard old data files. Worth knowing that a *partially* migrated page is broken outright rather than partially — `AllocateRowId()` scans every row on the catalog page and fails on the first old one, so even a newly created table inside an old database cannot take an insert.

**Hazard handed to T13.** `SetWaystoneDirectory()` bumps the catalog version, which clears the `TableAccess` cache and dangles any `const TableAccess*` a statement is holding. `HandleInsert` holds one across its whole body, so directory growth — which happens mid-insert — must re-acquire it afterwards. Documented on the method; it is the one way to use this API wrong.

**T05 — Refresh `CLAUDE.md`.**
Remove obsolete opens (metadata pool eviction; eviction-vs-validation invalidation). Add the spec §11 opens. Fix the architecture summary lines (Keystone layout; metadata pool → Waystone full coverage; hint index unchanged).
Done when: CLAUDE.md open-decision list is byte-for-byte reconcilable with spec §11.

**T06 — Cross-reference in `docs/page-management.md`.**
Add one paragraph listing Waystone directory/entry pages as `PageStore` clients and future SpaceManager consumers.
Done when: paragraph present; no other edits.

**T07 — Repo doc-naming hygiene.**
Rename or redirect so every reference resolves: `KDS-DESIGN.md` ↔ `docs/heap-and-tuple.md`, `CPP-RULES.md` ↔ `docs/rules.md`; drop or stub `docs/overview.md` (duplicate of CLAUDE.md). Commit `docs/waystone-concept.md` + this file into `docs/`.
Done when: grep for the stale names returns nothing unresolved.

**T08 — Rewrite `waystone.hpp` to match the confirmed spec.**
File: `include/kds/stats/waystone.hpp`. Delete `AdmissionPolicy` and all 16-bit-handle addressing (`kMetaHandle*`, `MetaPageIndexOf(handle)`). Keep: `SingleKeyRef`, `TupleAccessEvent` (retire the `meta_handle` field into explicit padding), `AccessObserver`/`NullAccessObserver`. Add: entry constants with `page_epoch`, pk-direct addressing helpers (`pk >> 8`, `pk & 0xFF`), directory constants (`kDirFanout = 2048`, level-coverage constexprs with derivations), `Aggregator` API per spec §9 (Ingest/DecayTick/Probe/insert-delete hooks), decay policy behind an interface.
Done when: header compiles standalone; every constant carries its derivation comment; no reference to admission or handles remains.

## Core implementation

**T09 — Entry codec.**
Files: `src/stats/waystone_entry.cpp`, `tests/waystone_entry_test.cpp`. Implement `ReadMetaEntry`/`WriteMetaEntry` (field-wise memcpy, little-endian, offsets pinned by `static_assert` incl. `page_epoch` at 24). Tests: full-field round-trips, tiling exactness (256 × 32 = 8192), flag transitions.
Needs: T08.

**T10 — Page directory.**
Files: `src/stats/waystone_dir.cpp`, `tests/waystone_dir_test.cpp`. Multi-level walk (fanout 2048), lazy allocation via `PageStore::CreateNew`, depth growth by root relink, root handoff as a plain `PageId` in/out (catalog wiring is T12). Backed by `InMemoryPageStore`.
Tests: pk→leaf correctness across level boundaries (pk 524,287→524,288; 2³⁰ boundary), lazy-alloc sparseness (page count == touched ranges), depth growth preserves prior mappings.
Needs: T08–T09.

**T11 — Fingerprint layer (parser side).**
Files: `src/parser/fingerprint.cpp` + header, `tests/fingerprint_test.cpp`. Normalize the existing `Statement` variant (strip/parameterize constants), hash to `pattern_id`; `arg_hash` over bound values. Pure function of the AST — fully implementable today.
Tests: identical shapes with different constants → same `pattern_id`; different shapes → different; stability across runs (no address-based hashing).
Needs: nothing (parser exists). May run in parallel with T09–T10.

**T12 — Catalog flag & storage-form wiring.** — **done 2026-07-30**, see T04 above; `TableAccess` carries the triple, `tests/waystone_catalog_test.cpp` covers defaults, round trip, validation and cache invalidation.
Files: catalog module + `tests/catalog_test.cpp` additions. Add `waystone_enabled` (+coverage state) and `waystone_dir_root` to the relation record (on-disk rules: memcpy codec, static_asserts); relation-open path returns the storage form; DDL set/clear.
Tests: defaults on create; round-trip through catalog pages; open path reports the right form.
Needs: T04, T10.

**T13 — Coverage hooks (unit-level).** — **done 2026-07-30** as `waystone_hooks.{hpp,cpp}` + 15 tests. Two decisions worth carrying forward: the hooks take the directory root and depth as a `WaystoneRef` value and never touch the catalog (a hook that could grow the directory would bump the catalog version and dangle the `TableAccess*` its own caller is holding), and a pk past the current depth reports **`OutOfRange`**, distinct from `InvalidArgument`, because "grow and retry" is a normal event and a bad pk is a bug. Dispatcher wiring is *not* included — it is still behind the headerless-page/checksum decision above.
Files: `src/stats/waystone_hooks.cpp`, `tests/waystone_hooks_test.cpp`. `OnInsert(pk, page_id, slot, epoch)` initializes the entry (`kEntryLive`) through the directory; `OnDelete(pk)` clears. Single arithmetic entry write each — anything heavier is a spec violation. Executor integration is deliberately out of scope here (T18).
Tests: spec §12-4 hook half against HeapPage/InMemoryPageStore fixtures, incl. first-touch lazy page allocation.
Needs: T09–T10.

**T14 — Aggregator core.**
Files: `src/stats/waystone_aggregator.cpp`, `tests/waystone_aggregator_test.cpp`. `Ingest` (count/recency/location/epoch refresh), `DecayTick` behind a `DecayPolicy` interface (ship halving as the default *implementation*, interface keeps EWMA viable — this is not deciding the `[OPEN]`), `stats()` counters (ingested/dropped/invalidations) accurate from day one. Synthetic-event fixtures; no executor needed.
Tests: spec §12-5 counts/decay; monotone heat ordering within a tick.
Needs: T09–T10, T13.

**T15 — Observer & ring.**
Files: `src/stats/access_ring.cpp`, `tests/access_ring_test.cpp`. Core-local SPSC ring, preallocated, wait-free enqueue-or-drop with drop counting; `NullAccessObserver` as the disabled path. Producer side driven by a synthetic generator until T18.
Tests: spec §12-7 (saturation drops, never blocks, counts visible); steady-state no-allocation assertion.
Needs: T08. Parallel with T13–T14.

**T16 — Probe & epoch validation.** — **done 2026-07-30** as `waystone_probe.{hpp,cpp}` + 18 tests. `Probe()` folds all four ways to be unusable (no entry, no page, cleared `kEntryLive`, epoch mismatch) into one `trusted` flag, deliberately, so a caller cannot handle three and forget the fourth. `ProbeAndVerify()` owns the §3.1 rule-2 Keystone-id check and **fails closed** — a null id-reader is `InvalidArgument`, never a silently unverified trust. `Observe()` restores trust after a fallback lookup without touching heat and without resurrecting a delete-mark. `EpochProvider` keeps the epoch-storage `[OPEN]` open.
Files: extend aggregator + `tests/waystone_probe_test.cpp`. pk probe through the directory returning `{page_id, slot, use_count, page_epoch, trusted}`; epoch source behind an `EpochProvider` seam (stub now; real bump sites arrive with the physical optimizer, T19). Document the B+-tree-fallback obligation at the call site.
Tests: spec §12-5 — bump ⇒ untrusted, re-observe ⇒ trusted, counts survive.
Needs: T14.

**T17 — Backfill & toggle lifecycle.**
Files: `src/stats/waystone_backfill.cpp`, `tests/waystone_backfill_test.cpp`. Maintenance-group task scanning in key order through a `TupleScanSource` seam (stub over heap fixtures now; B+ tree plugs in later), restartable via a persisted low-water pk; coverage-complete transition; disable = wholesale drop + flag clear.
Tests: spec §12-6 full (enable→backfill→complete; restart mid-way; disable leaves results unchanged).
Needs: T12–T14.

**T18 — Executor integration.** *(startable as a stub today; completes with roadmap M1)*
Wire the observer into the executor's tuple-touch sites and the coverage hooks into the insert/delete paths; plumb `pattern_id`/`arg_hash` from the plan context (T11). Until M1 lands, keep the integration behind a fixture executor that replays scripted access traces — the trace format is this task's first deliverable and is what T14–T16 consume.
Tests: spec §12-2 (results identical with observer off / ring saturated / structure deleted) and §12-3 in its **amended** form (read-path *equivalence* across probe-on / probe-off / disabled / pages-deleted / stale-entry-pointing-at-a-different-pk — not the retired "zero page touches" assertion) — both become regression-mandatory for every later change.
Needs: T13–T15.

**T19 — Relayout epoch bump sites.** *(interface now, call sites later)*
Define `EpochProvider`'s mutating side (`BumpFor(page)`), invoked by page rebuild/relayout. Land the interface + tests against the stub now; the physical optimizer calls it when it exists.
Needs: T16.

**T20 — Pattern-correlated groups (hint-index seed).**
Reconstruct per-execution `SingleKeyRef` sequences keyed by `(pattern_id, arg_hash)` from `ordinal`s; bounded per-core table; expose as probe-side prefetch candidates only (per-template trust classification stays `[OPEN]` — surface candidates, don't act on them).
Needs: T11, T14–T16, T18 trace fixtures.

## Standing instructions

- Advisory-contract tests (T18's §12-2/§12-3) run in every CI pass from the moment they exist.
- No allocation on observer/ingest steady-state paths; all timing via the injected clock; all page access via `PageStore`.
- Update spec + this workplan together when an `[OPEN]` lands; move it into the spec body with the date.
