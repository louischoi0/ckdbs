# Workplan — the multi-page free map (`docs/spec/page.md` §5)

**Status: complete. FM1-FM11 built 2026-08-26.** An instance is bounded by the
2^31-page design ceiling, not by one bitmap page's 65,280 ids.

`docs/spec/page.md` §5 said free-map pages "sit at computable interval positions in
the id space" and did not say which positions. §2 recorded the two candidates
and their costs and picked neither; **the operator picked candidate A** — the
map pair at the head of the region it covers — along with D3(a), D4(a) and,
later the same day, D2(a), D5(a), D8(a) and D10(a).

**Decisions, and how each closed** (full statements in §7):

| | Question | Closed as |
|---|---|---|
| D1 | placement arithmetic | operator: candidate A |
| D2 | the headerless map at scale | operator: (a) — **after FM2 dissolved the premise**; see FM6 |
| D3 | may a run straddle a region | operator: (a), refuse |
| D4 | map pages in the pool | operator: (a), store-owned |
| D5 | peer coherence | operator: (a), refresh every resident region |
| D6 | `page.md` §4/§5 vs `superblock.hpp` | **still open** — a doc conflict this work did not need to resolve |
| D7 | mount validation scope | (a), and **forced rather than chosen**; see FM9 |
| D8 | what `allocated_pages()` means | operator: (a), maintained |
| D9 | does the map become logged | **deliberately open**; stays unlogged, RC04 repairs |
| D10 | the grant bitmaps' own ceiling | operator: (a), grow per region — a decision this work *created* |

Two of this document's own claims were **retracted by building it**, and both
are marked where they were made rather than quietly amended: §5's argument that
`IsHeaderless` could double the miss path (FM6 — FM2's eager loading removed
the cost), and §5's single-digit residency target (FM8 — resident map pages
track file size, not access pattern). §2's warning that candidate A would need
a third clause in `MayFault` also did not come true, because D4(a) keeps map
pages out of the pool; the two decisions cancelled.

Everything the survey states about the *pre-FM1* code was read on the
`multi-free-map` worktree at `d8be95a` and re-checked at `56b20d2` before the
series started; the FM rows in §6 name their own commits. §1's line numbers are
pre-FM2 and are kept as the record of what was changed, not as a map of what is
there now.

**Scope.** Raise the instance ceiling from one bitmap page to a computable
family of them. In scope: the addressing arithmetic, the store's map cache,
the ceilings that today read `kFreeMapBitsPerPage`, `ExtentAllocator` across a
page boundary, the headerless map's very different access pattern, mount
validation, and peer stores. Out of scope, explicitly, in §6a: reclamation.

---

## 1. The current path, end to end, for both bitmaps

`kFreeMapPageId` (1) and `kHeaderlessMapPageId` (2) are declared as fixed low
ids at `include/kds/storage/device_page_store.hpp:109` and `:115`, and share
one codec — `FormatFreeMapPage`, `ValidateFreeMapPage`, `FreeMapIsAllocated`,
`FreeMapAllocate`, `FreeMapFindFirstFree`, `FreeMapCountAllocated` in
`include/kds/storage/free_map.hpp` — distinguished on disk only by the common
header's type byte (`PageType::kFreeMap` vs `PageType::kHeaderlessMap`).
Coverage is one constant, `kFreeMapBitsPerPage = (8192 − 32) × 8 = 65,280` ids
(`free_map.hpp:30-32`) — 510 MiB of data file, and on `multi-free-map` at
`d8be95a` that is the **whole instance ceiling**, not a per-page one.

### 1.1 Load at mount

`DevicePageStore::Open` (`src/storage/device_page_store.cpp:24`) is the only
loader. It declares two stack `Page` buffers, decides freshness from
`device.page_capacity() <= kFreeMapPageId` (`:33`), reads exactly page 1
(`:35`) and exactly page 2 (`:57`), and validates each with
`ValidateFreeMapPage` (`:38`, `:64`). A fresh database formats both and marks
their own two bits (`:47-50`). The two buffers are copied into the store's
`free_map_page_` / `headerless_map_page_` members
(`device_page_store.hpp:621-622`) by the constructor at
`device_page_store.cpp:19-20`.

**They are `Page` members, not frames.** The maps are not in `frames_`, so
they are invisible to `DirtyPageIds()`, `DirtyPagesWithRecLsn()`, the CLOCK
sweep, `EvictClean` (`device_page_store.hpp:378`), the scan ring and
`PageRef`.

Note that **every core opens its own store over the same device**
(`src/server/core_runtime.cpp:54`), so an N-core mount holds N private copies
of both pages, and a peer's copy is frozen at its `Open()`.

### 1.2 Scan during `ExtentAllocator::Reserve`

`ExtentAllocator` holds `std::span<std::byte, kPageSize> free_map_` — **one
page, borrowed, for the allocator's lifetime**
(`include/kds/storage/extent_lease.hpp`; `Reserve` at
`src/storage/extent_lease.cpp:7`). The span comes from
`DevicePageStore::free_map_bytes()` at `src/server/expeditor.cpp:1175`, and
the allocator is a long-lived `std::optional` member
(`include/kds/server/expeditor.hpp:659`). The scan is
`FreeMapFindFirstFree(free_map_, candidate)` (`extent_lease.cpp:19`), the
run-length probe is `FreeMapIsAllocated(free_map_, start + run)` (`:33`), and
the refusal is `start + count > kFreeMapBitsPerPage` → `OutOfSpace` (`:25`).

Core 0 also allocates *per page* without going through `Reserve`:
`CreateNewUnpinned` calls `FreeMapFindFirstFree(free_map_bytes(),
next_new_page_id_)` at `device_page_store.cpp:449`. See §5 for why that
matters to the residency target.

### 1.3 Marking

Three sites set free-map bits: `CreateAtUnpinned`
(`device_page_store.cpp:413`), `Reserve`'s marking loop
(`extent_lease.cpp:40`), and `Open`'s bootstrap of the maps' own bits
(`device_page_store.cpp:48-49`, `:62`). The headerless bit is set in exactly
one place, `CreateNewHeaderlessUnpinned` (`:106`).

`FreeMapAllocate` **silently ignores** an index at or above
`kFreeMapBitsPerPage` (`free_map.cpp:38`) and `FreeMapIsAllocated` **returns
true** for one (`free_map.cpp:33`) — fail-closed by design, so a missed range
check cannot corrupt a neighbouring bit. Under a multi-page map those two
guards stop meaning "outside the id space" and start meaning "outside *this
page*", which is a different and far more common condition.

### 1.4 Dirty tracking

**One `bool maps_dirty_` for both pages** (`device_page_store.hpp:626`), with
the reason at the declaration: they are written together, in the same order,
at the same points, and a flag per map would create a state where one is on
disk and the other is not. It is set at `device_page_store.cpp:107`, `:414`,
and — unconditionally, on every call, whether or not the caller writes a bit —
by the public `free_map_bytes()` accessor at `device_page_store.hpp:332`.

### 1.5 Write-back

`FlushMaps` (`device_page_store.cpp:115`) is the only writer. It returns early
on a leased store (`:133`, the peer rule), stamps and writes the headerless
map (`:147`), then the free map (`:155`), then clears the flag (`:164`). It is
reached from `Flush()` (`:695`, after `WriteBack` of the data pages) and from
`FlushPages()` (`:781-782`, likewise after).

**`FlushMaps` does not go through `WriteBack`**, so the maps are not
run-coalesced, not gated by `AwaitWalGate`, and never in a checkpoint dirty
table — consistent with the map being unlogged (`src/server/expeditor.cpp:1163`
says so in as many words; `RecordType::kAlloc`/`kFree` exist at
`include/kds/wal/record.hpp:58`/`:64` with a payload codec at
`include/kds/wal/payload.hpp:333`, and `grep` finds no emitter anywhere in
`src/`).

### 1.6 Which paths assume "exactly one page"

Assume **one page** — must change:

| Site | The assumption |
|---|---|
| `device_page_store.hpp:621-622` | two `Page` members, one each |
| `device_page_store.hpp:626` | one `maps_dirty_` bool for both |
| `device_page_store.hpp:332`, `:598`, `:601` | `*_bytes()` returns *the* page |
| `device_page_store.cpp:33` | freshness = capacity vs. page 1 |
| `device_page_store.cpp:35`, `:57` | mount reads exactly ids 1 and 2 |
| `device_page_store.cpp:115-165` | `FlushMaps` writes exactly two device pages |
| `device_page_store.cpp:173` | `IsAllocated`: `id >= kFreeMapBitsPerPage` → **false** — the hard ceiling that makes `Get()` answer `NotFound` |
| `device_page_store.cpp:187` | `allocated_pages()` counts one page |
| `device_page_store.cpp:403-406` | `CreateAt` → `OutOfRange` past coverage |
| `device_page_store.cpp:449` | `CreateNew` searches one page from `next_new_page_id_` |
| `device_page_store.cpp:479-483` | `RaiseAllocationFloor` → `OutOfRange` past coverage |
| `extent_lease.hpp` (`free_map_`) | the allocator borrows one page for its lifetime |
| `extent_lease.cpp:19`, `:25`, `:33`, `:40` | scan, ceiling, probe and mark all within it |
| `free_map.cpp:33`, `:38`, `:45`, `:51`, `:56` | `kFreeMapBitsPerPage` read as the size of the id space |

Already "the page covering this id", or indifferent — need not change:

- `free_map.cpp`'s bit arithmetic itself. `ByteOffset`/`BitMask` are relative
  to `kPageBodyOffset` and take a within-page index, so they become correct
  the moment a caller passes `id % kFreeMapBitsPerPage`.
- `IsHeaderless` (`device_page_store.cpp:84`) is *shape*-correct — it asks two
  maps and ANDs them — but both lookups are single-page; see §5.
- `MayFault` (`:357`) and `MayWrite` (`:383`) are id-range predicates that
  already work per id; whether they still answer *correctly* depends on §2's
  placement.
- `IsPinnedClass` (`:848`) is per id and per resident frame, and its
  page-kind half extends to `kFreeMap`/`kHeaderlessMap` with no structural
  change.
- The WAL envelope, every catalog row, every page-format link — see §4.

---

## 2. Placement arithmetic — candidates and costs. **Candidate A, chosen 2026-08-26.**

`docs/spec/page.md` §5 fixes only that locating a bitmap is arithmetic and not a
lookup. Two candidates, with what each actually costs. This section recommended
neither; **the operator chose candidate A on 2026-08-26**, and it is FM1's four
function bodies. Candidate B is kept below unchanged, because the reason A was
chosen is B's cost and that reasoning is worth keeping legible.

### Candidate A — the map pair at the head of the range it covers — **CHOSEN**

Region `N` is ids `[N × 65,280, (N+1) × 65,280)`; its free map sits at
`N × 65,280 + 1` and its headerless map at `N × 65,280 + 2`.

- **It reproduces today's ids exactly for region 0** — `0 × 65,280 + 1 = 1`
  and `+ 2 = 2`, which is `kFreeMapPageId` and `kHeaderlessMapPageId` on
  `multi-free-map` at `d8be95a`, with the superblock still at 0 and the
  catalog's fixed pages 4-14 plus its overflow range 15-127
  (`include/kds/catalog/well_known.hpp:254-331`) all inside region 0. A
  database that fits in one region is byte-identical, so **no superblock
  version bump and no migration**.
- **Dirty tracking:** per map page. The dirty map pages are scattered across
  the file at 510 MiB intervals, so `WriteBack`'s ascending-run coalescing
  (`device_page_store.cpp:568`, `kWritebackRunPages = 8`) can never fold two
  of them together — one seek per dirty map page per flush. At §5's
  single-digit residency that is a handful of extra writes, not a class of
  cost.
- **Mount validation:** how many regions exist is `page_capacity() / 65,280`,
  arithmetic on a number `Open` already has. Validating all of them is
  O(regions) *scattered* reads; validating region 0 only is one read, exactly
  as today — that is decision **D7**.
- **Locality:** a data page's map is always inside its own 510 MiB region, so
  the allocator's read and the pages it then writes are near each other, and a
  future per-region sweep touches one map page.
- **Adjacency:** the two bitmaps stay adjacent, permanently, by construction.
- **The costs, named.** Map pages sit *above* `system_page_limit_`
  (`kFirstUserPageId = 128`, `include/kds/server/superblock.hpp:42`) for every
  region above 0, so `MayFault` (`device_page_store.cpp:357`) refuses a peer
  the very map pages it would need to answer `IsAllocated` for a CC7-granted
  page — the check has to learn the placement arithmetic as a third clause.
  The same holds for `IsPinnedClass`'s id-range half (`:848`): a scattered map
  page is above `first_evictable_page_id_` and is caught only by the kind
  half, which requires the frame to be resident already. And region 0's map
  lives at `+1`/`+2` rather than at the region head, which is a deliberate
  off-by-one that exists solely to leave page 0 to the superblock — it must be
  written once, in one function, and tested, or it will be re-derived wrongly.

### Candidate B — a reserved region extending today's sub-128 system range — *not chosen*

Every map page contiguous at the head of the file: free maps for regions
`0..M` at ids `F..F+M`, headerless maps likewise, either as a second block or
interleaved.

- **Dirty tracking:** per map page, and the dirty ones are contiguous low ids,
  so `WriteBack`'s run coalescing folds them into one or two `WritePageRun`s.
  Strictly better write behaviour than A.
- **Mount validation:** the same `page_capacity()`-derived count, but the
  reads are one sequential run — a single `ReadPageRun` covers every map page
  in existence. The cheapest possible answer to D7's "validate everything at
  the door", which is the stance RV3 already took for catalog pages.
- **Locality:** the worst of the two — the map for a page at 15 TiB sits at
  the file head. With extent leases the map is touched once per 64 pages
  (`kDefaultExtentPages`, `include/kds/storage/page_device.hpp:44`), so the
  cost is a cold read per lease refill rather than per allocation.
- **Adjacency:** a design choice, not a consequence. Two blocks lose it;
  interleaving (`free, headerless, free, headerless, …`) keeps it and keeps
  the arithmetic a single multiply-add.
- **The costs, named.** The sub-128 range is **fully spoken for**:
  `kCatalogOverflowFirst = 15` through `kCatalogOverflowLimit = 128`
  (`well_known.hpp:326`, `:331`) is catalog overflow, and 128 is
  `kFirstUserPageId`. A reserved map region therefore has to move
  `kFirstUserPageId`, which is a **superblock version bump** — the block at
  `include/kds/server/superblock.hpp:52-160` records six precedents, and the
  12→13 entry is exactly this shape, where page 14 collided with the overflow
  range — and there is no in-place path for an existing file. Worse, the
  region has to be **sized in advance**: 2^31 ids need 32,897 free-map pages,
  so a fixed region sized for the design ceiling reserves 65,794 ids (514 MiB
  of *id space*, sparse on disk) ahead of the first user page. A growable
  region reintroduces exactly the placement question it was meant to answer,
  one level up.

### What both share

Either way the map's own bits are self-marking (under A, a map page's bit
lives in the page it is; under B, in the first map page, which is always
resident), and either way `page_capacity()` — not a stored count — is what
says how many regions exist. Neither candidate needs a new superblock field:
the superblock deliberately holds no allocation state
(`include/kds/server/superblock.hpp:16-20`), which **contradicts
`docs/spec/page.md` §4's "the superblock at page 0 anchors … free-map root,
high-water" and §5's "High-water mark and free-map root live in the
superblock".** That conflict is decision **D6**; it is flagged here rather
than guessed, per CLAUDE.md.

---

## 3. Is faulting map pages through the buffer pool circular?

Today `free_map_page_` and `headerless_map_page_` are `Page` members outside
`frames_` (`device_page_store.hpp:621-622`). Checked in three parts.

**Reading a map page: no cycle.** `ResidentBytes`
(`device_page_store.cpp:211`) allocates a frame and reads the device; it never
consults the free map. The free-map check lives one level up, in
`GetUnpinned` (`:495`) and `GetForReadUnpinned` (`:502`), which a map fault
would simply not call. The one genuine loop candidate is inside
`ResidentBytes` itself: the miss path asks `IsHeaderless(page_id)` at `:266`
before verifying the checksum, and `IsHeaderless` (`:84`) reads the headerless
map — which under a multi-page map is itself a page that might have to be
faulted, whose fault asks `IsHeaderless` again. **It terminates, and by
arithmetic rather than by luck:** a map page's class is known from its id
under either §2 candidate, so `IsHeaderless` short-circuits to `false` for any
map page without reading a map. That short-circuit is a required part of the
work, not an optimization — without it the recursion is real.

**Writing a map page back: no cycle, but a real ordering hazard.**
`StampIfHeadered` (`:88`, called per page from `WriteBack`'s stamping loop at
`:603`) asks the same question and takes the same short-circuit. What does
*not* survive is the ordering rule: `FlushMaps` (`:115`) exists precisely to
write the maps **after** the data pages they describe — the comment above the
two writes says the reverse order would publish an allocated headerless page
whose headerless bit had not landed — and `WriteBack` (`:568`) sorts strictly
ascending. Map pages at low ids (candidate B, or region 0 under A) would be
written **first**. So map pages may become ordinary pool frames only if the
flush path keeps excluding them from the ascending sweep and writes them in a
second, later pass. That is decision **D4**, and it is why "just put them in
the pool" is not free.

**Creating a new map page: no cycle, and this is the clearest case.** Its id
is arithmetic under either candidate — `N × 65,280 + 1`, or `F + N` — so
creating one never asks the map where to put it. `CreateAtUnpinned` (`:392`)
takes a chosen id, checks `IsAllocated`, and marks the bit; for the first page
of a new region under candidate A the bit it marks is *in the page being
created*, which is self-referential and terminating (format, then set bit 1 of
the page's own body). Under candidate B the bit is in map page 0, which is
always resident.

**Conclusion.** There is no real cycle in any of the three — only an apparent
one, and it is apparent only because `IsHeaderless` sits on the fault path.
Two things must be true for that to hold: a map page's class is decided by
arithmetic and never by a map lookup, and the flush path keeps writing maps
after data. Both are cheap; neither is automatic.

---

## 4. Does every on-disk structure that stores a page id survive 2^31?

**Yes. Every persisted page-id field found on `multi-free-map` at `d8be95a`
stores a full 32-bit `PageId`, and none reserves a high bit for flags.** The
sweep, by structure:

| Structure | Field | Width |
|---|---|---|
| B+ tree internal node | `leftmost_child` (`btree_page.hpp:68`), entry `child` (`:98`, at offset 8 of a 12-byte entry) | `PageId`, 32 bits |
| Heap page | tail `next_page_id` reservation | `sizeof(PageId)` = 4 (`heap/heap_page.hpp:166`) |
| Var-heap page | tail `next_page_id` | `sizeof(PageId)` (`varheap.hpp:57`) |
| Var-heap cell pointer | `page_id << 32 \| slot << 16` in the u64 a `kSpilled` tagged cell carries | 32 bits at shift 32 (`varheap.hpp:159-176`) |
| Undo chain pointer | `undo_ptr = u64(page_id) << 16 \| offset` | 32 bits at shift 16 inside a u64 — headroom to 2^48 (`txn/undo_page.hpp:271-282`) |
| Undo page / record | `prev_page_id`, `target_page_id` | `PageId` (`undo_page.hpp:87`, `:147`) |
| Secondary index | `right_sibling`, `leftmost_child`, entry child | `PageId` (`index/index_page.hpp:134`, `:170`, `:118`) |
| Waystone trail page | `next_page_id` @ 24; entry `page_id` @ 16 of a 32-byte entry | `PageId` (`stats/waystone.hpp:79`, `:137`) |
| Waystone directory | 2048 child ids tiling 8192 bytes exactly | `sizeof(PageId)` = 4, asserted (`stats/waystone_dir.hpp:87-89`) |
| Bound Cabin entry | `page_id` in the 32-byte entry | `PageId` (`cabin_bound_page.hpp:138`) |
| Catalog rows | `SysTableRow.desc_page_id` / `.varheap_page_id`, `SysIndexRow.root_page_id`, `SysPatternRow.waystone_root` | `PageId`, with offsets derived through `sizeof(PageId)` (`catalog/rows.hpp:62`, `:81`, `:384`, `:485`) |
| Superblock | **holds no page id at all** — no free-map root, no high-water, no page counter, by the design note at `server/superblock.hpp:16-20` | n/a |
| WAL record envelope | `page_id` @ 28 | `PageId` (`wal/record.hpp:199`, `:209`) |

The file-offset arithmetic is already 64-bit: `PageOffset` casts to
`std::uint64_t` before multiplying (`src/storage/file_page_device.cpp:37`),
`CheckPageRunRange` computes `first + nr_pages` in `std::uint64_t`
(`src/storage/page_device.cpp`), and the 2^31 ceiling is enforced at the
device layer rather than merely documented — `kMaxPageCount = 1u << 31` with
`static_assert(kMaxFileBytes == 16 TiB)` at
`include/kds/base/common.hpp:22-25`, checked at `file_page_device.cpp:75`,
`:170`, `:186` and `memory_page_device.cpp:42`, `:56`, `:64`.

**Three things that are *not* width bugs but sit near the boundary**, recorded
so nobody re-derives them:

1. `Extent::end()` is `first + count` in `PageId`
   (`storage/extent_lease.hpp`), safe only because `Reserve` refuses a run
   reaching the coverage limit (`extent_lease.cpp:25`). When that limit
   becomes 2^31 the guard must move with it, or `end()` wraps at the top of
   the id space.
2. `allocated_pages()` returns `std::uint32_t`
   (`device_page_store.cpp:186`). 2^31 fits, but only just, and the number is
   printed at mount and shutdown (`src/server/main.cpp:334`, `:349`) and by
   `SHOW META` (`src/server/expeditor.cpp:882`, `:1568`) — see **D8**.
3. `kEmptyDirSlot = kInvalidPageId` (`waystone_dir.hpp:107`) and every other
   `kInvalidPageId` sentinel stay unambiguous only because the ceiling is 2^31
   and the sentinel is `0xFFFFFFFF`, which `common.hpp:26`'s `static_assert`
   already pins.

So what blocks 16 TiB today is **not** a stored width. It is one constant,
`kFreeMapBitsPerPage`, and the ~15 sites in §1.6 that read it as though it
were the size of the id space. The real ceiling on `multi-free-map` at
`d8be95a` is **65,280 pages = 510 MiB**, and that number is not recorded in
`docs/inflight/known-gaps.md` — a finding this plan does not act on (§9).

---

## 5. Residency: the single-digit steady state, and why the two maps differ

**Taken as given**, and confirmed at `extent_lease.hpp`'s header note and
`device_page_store.cpp:428-446`: the 22 synchronous allocation sites go
through `LeasedIdSource::Next()`, which touches no map at all, so a peer core
allocates without reading a bitmap and only core 0 carves extents.

**One correction to the premise, because it changes the count and not the
conclusion.** Core 0 has no lease — `src/server/expeditor.cpp:1177` grants
leases to cores `1..N-1` only — so on core 0 `CreateNewUnpinned` still scans
the free map per page, at `device_page_store.cpp:449`, from
`next_new_page_id_`. That hint advances monotonically (`:464`), so core 0's
per-page allocation is likewise confined to the one map page covering its
hint. A single-core deployment is entirely this path. The allocation-side
working set is therefore **two hint-local pages, not one** —
`ExtentAllocator::next_` and `DevicePageStore::next_new_page_id_` — plus their
headerless twins if D2 keeps any. Single-digit still holds, comfortably.

**The headerless map does not follow a hint, and that is the residency
problem.** `IsHeaderless` sits on three paths that are not allocation:

- **every page fault** — `ResidentBytes` asks it before verifying the checksum
  (`device_page_store.cpp:266`);
- **every page write-back** — `StampIfHeadered` asks it per page in
  `WriteBack`'s stamping loop (`:603`, through `:88`);
- **every dirty page in a flush batch** — `AwaitWalGate` asks it to decide
  whether the bytes at the `page_lsn` offset are a real LSN (`:549`).

Those follow the *statement's* working set, which is scattered across the id
space by construction. At the design ceiling the headerless map is 32,897
pages; a per-read bitmap lookup over that is, in the worst case, a second
fault for every first fault — the opposite of a single-digit resident set, and
it would show up as a doubling of the miss path rather than as a rounding
error.

That is why **D2 is a required decision and not a detail**: at one page the
headerless map is free, and the *only* reason it is free is that it is one
page. What makes the options viable at all: the headerless class is Waystone
entry and directory pages and nothing else (`docs/spec/page.md` §1), they are
allocated through ordinary `CreateNewHeaderless` calls, and a database with no
Waystone directory has zero of them.

---

## 6. Task series

Each task ends with a `critics-developer` review per CLAUDE.md's session
workflow. D1 was settled 2026-08-26, so the series has started.

- **FM1 — the addressing layer. Built 2026-08-26.** The placement section of
  `include/kds/storage/free_map.hpp` owns `FreeMapRegionOf`,
  `FreeMapPageIdFor`, `HeaderlessMapPageIdFor`, `FreeMapBitIndexOf` and
  `IsMapPageId` as `constexpr` functions, with D1's candidate A expressed in
  those bodies and nowhere else. Three departures from the row as planned,
  each for a reason: the functions live in `free_map.hpp` rather than a new
  header, because the region size *is* `kFreeMapBitsPerPage` and a header
  holding five functions that all read one constant defined next door is a
  file boundary that buys nothing; `BitIndexWithin` is named
  `FreeMapBitIndexOf`, because `kds::storage` has many bit indices and an
  unqualified name at namespace scope does not say which; and
  `FreeMapRegionOf` is exposed rather than kept private, because FM9's
  region count and FM5's growth point both need it and a second derivation of
  `id / kFreeMapBitsPerPage` elsewhere is exactly what this task exists to
  prevent. Tests (`tests/free_map_test.cpp`, `FreeMapPlacementTest`): the
  `(map page, bit)` pair reconstructs the id, so the mapping is exactly
  one-to-one; both map pages of a region fall at bits 1 and 2 of that region's
  own free map; `IsMapPageId` agrees with the two constructors on every probe
  id; region 0 yields 1 and 2. Two `static_assert`s pin that nothing at the top
  of the id space produces an id at or above `kMaxPageCount`, and two more, at
  the declarations in `device_page_store.hpp`, bind `kFreeMapPageId` and
  `kHeaderlessMapPageId` to the arithmetic. No behaviour change — nothing calls
  the new functions yet, and the existing single-page paths are untouched.
- **FM2 — the store's map cache. Built 2026-08-26.** The two `Page` members
  and the single `maps_dirty_` are gone; `map_regions_` is a
  `std::map<region, MapRegion>`, ordered so a flush writes regions in
  ascending id order, and `FlushMaps` writes each dirty region's headerless
  map before its free map, after the data pages. Two departures from the row,
  both argued rather than assumed:
  - **The dirty flag is per region, not per page.** The reason the old code
    gave for one flag over two was never "there are two pages" — it was that
    the pair is written together, in the same order, at the same points, and a
    flag per page creates a state where one is on disk and the other is not.
    That argument holds unchanged *within* a region and says nothing *across*
    regions, so the pair stayed the unit and the region became the key.
  - **`Open` loads every region the device holds, not region 0 alone.** This
    is D7(a)'s shape, and the row asked for D7(b)'s. It is forced, and by a
    signature rather than a preference: `IsAllocated` and `IsHeaderless` are
    `const noexcept` and sit on the fault path, the write-back path and the
    WAL gate, so neither can read a device or report a failure. Lazy loading
    therefore has to answer from a `mutable` cache with a swallowed error —
    the exact failure shape RV3 converted into a refusal. Loading eagerly
    makes "absent from the cache" mean "does not exist", which is what lets
    those two predicates answer from an empty page and stay `const noexcept`.
    **This narrows D7 rather than settling it**: what remains open there is
    whether validation may be deferred, not whether loading may be.
  For a database inside one region the mount reads exactly the two pages it
  always did — the loop body does not run — and region 0's ids are still 1 and
  2. On byte-identity, see §8's amended first item: it is asserted
  structurally, not by differencing two builds.
- **FM3 — raise the ceilings. Built 2026-08-26.** `IsAllocated`, `CreateAt`
  and `RaiseAllocationFloor` compare against `kMaxPageCount`; `CreateNew`'s
  search crosses regions and creates the next one when it runs off the end;
  `allocated_pages()` sums the resident regions, which is still the instance
  total because every region that exists is resident. `CreateAt`'s two named
  map ids became `IsMapPageId`, so every region's bitmaps are unplaceable and
  not just region 0's. The `free_map.cpp` guards kept their fail-closed shape
  and are now unreachable by construction: every caller passes
  `FreeMapBitIndexOf(id)`, which is in range by definition, so the guard is a
  backstop against a caller that forgot rather than a condition anything
  handles. **Three tests pinned the old ceiling as the instance ceiling and
  were rewritten** — and there were three, not the two §6's FM11 row named:
  `tests/wal_high_water_test.cpp`'s
  `APageIdBeyondTheFreeMapsCoverageRefusesTheMount` was not on that list. It
  is the `RaiseHighWater` repair refusing a log that names a page this build
  could not have written, and what "could not have written" means moved with
  the ceiling.
- **FM4 — `ExtentAllocator` across a boundary. Built 2026-08-26.** It holds
  no page: the store-backed form asks `FreeMapBytesForRegion(region)` per
  call, and the bare-bytes form keeps a `std::byte*` because a fixed-extent
  `std::span` has no empty state. `Reserve` walks region by region; **D3(a) is
  built as the region-advance** — a run that would straddle abandons the tail
  of its region and restarts in the next — and a `count` above
  `kFreeMapBitsPerPage` is refused up front, since under D3(a) a region is the
  longest possible reservation. `Extent::end()`'s safety moved with the
  ceiling (§4's first named boundary case): the guard is now `kMaxPageCount`,
  which matters because the top region is partial.
  **One behaviour change worth naming**: constructing a store-backed allocator
  no longer marks the map dirty, because it no longer takes the span and
  taking the span is what marked it. The guarantee that motivated that side
  effect — a reservation always dirties the region it wrote — is unchanged,
  but a test that leaned on *construction* to dirty the map is now testing
  nothing. That is the PW3b review's C1 in reverse, and it is why the
  constructor's comment says so at the declaration.
- **FM5 — growing the map. Built 2026-08-26.** Growth has no separate call:
  `EnsureRegionResident` loads a region if the device holds one and formats a
  fresh one if it does not, and both `Reserve` and `CreateNew` reach it simply
  by walking into a region. A created region formats both bitmaps, marks its
  own two ids in its own free map — self-referential and terminating, which is
  the property FM1's arithmetic exists to give — and grows the device's
  capacity to cover them. Crash-safety is the existing rule: `FlushMaps`
  writes maps after data, so a lost map write loses an allocation record and
  RC04's `RaiseAllocationFloor` repairs it, exactly as at one page (D9(a),
  unchanged).
  **A hazard found and closed while building it, which the survey did not
  reach**: a *peer* arrives here through `CreateNewHeaderlessUnpinned` once
  its lease lies above region 0. Reading the region from the device would be
  an unsynchronised read of a page core 0 owns and is writing without a latch
  — the hazard `RefreshFreeMapFromDevice` handles for region 0 and which does
  not generalise. Refusing instead is worse: the bit the peer wants to set is
  what stops `StampIfHeadered` writing a checksum over a headerless page's
  payload, and that bit matters **in memory** even though `FlushMaps` has
  always dropped a leased store's map writes. So a peer gets a private, empty,
  never-dirty region — its region-0 copy's semantics, generalised. Recording a
  peer's headerless pages durably is FM7's, under D5.
- **FM6 — the headerless map at scale. Built 2026-08-26**, as D2(a) — but
  **the decision's premise had to be corrected before it could be answered**,
  and the correction is the substance of this row. §5 argued `IsHeaderless`
  could cost a second fault per fault, because the headerless map would be
  partly resident and the paths it sits on follow a scattered working set.
  FM2 loads every region that exists at mount, so **every headerless bitmap is
  already resident and the lookup is a `std::map::find` plus a bit test** — no
  I/O, no fault, no second miss. The cost §5 named does not exist, and FM6 is
  therefore not the thing §5 asked for.
  What remains is *resident memory*: 16 KiB per 510 MiB of file, about 526 MiB
  at the 16 TiB ceiling, half of it headerless bitmaps. D2(a) attacks that
  half. Two mechanisms, both cheap:
  - **A region's headerless bitmap is not built until the region holds a
    headerless page**, and **its id is not marked allocated until then
    either**. A database with no Waystone directory therefore holds one
    bitmap page per region instead of two and reads one per region at mount.
    `allocated_pages()` on a fresh database is 1 where it was 2, which is the
    honest number: nothing is at that id.

    That second clause is a **correction the simulation harness forced**, and
    it is the more interesting half. FM6 first shipped with the id *reserved*
    at region creation and only the bytes deferred — which reads as the safe
    choice, since a reserved id cannot be handed out. It is not: an id the
    free map calls allocated whose page was never written is precisely the
    signature of a torn creation, and `sim/`'s integrity sweep reads every
    allocated page. It failed on seed 4, in three modes, with
    *"page 2: allocated page unreadable ... never written (all zero)"*. The
    fix is to claim the id in `EnsureHeaderlessMap`, as the page is placed,
    and to let both allocation paths skip a bitmap id by **arithmetic**
    (`IsMapPageId`) rather than by finding a bit set. That is strictly more
    robust than the reservation it replaced: allocation correctness no longer
    depends on a bit having been written at the right moment, and the same
    `IsMapPageId` already guarded `CreateAt`.
  - **A whole-instance `any_headerless_` flag** short-circuits `IsHeaderless`
    to `false` with *no lookup at all* — the fast path D2(a) actually named.
    Seeded at mount from what loaded, moved by the one writer that can change
    it. `src/stats/waystone_dir.cpp` is the engine's only caller of
    `CreateNewHeaderless`, which is what makes "one writer" true rather than
    hopeful.
  A database written before the headerless map existed has nothing at that id
  and reads as `kInvalid`, which is now simply "no headerless pages here" —
  the legacy special case stopped being special.
- **FM7 — peer stores.** What a non-zero core loads at `Open`, what it may
  fault, and whether its cached map pages can go stale in a way `EvictClean`
  does not cover (**D5**). Under candidate A — chosen — this includes
  `MayFault`'s third clause. **It also includes the two grant bitmaps, which
  the survey did not count; see §9's second finding.** `fault_rights_` and
  `write_rights_` (`device_page_store.hpp:732-733`) are single-page bitmaps of
  the same shape indexed by *absolute* page id, so their coverage is the same
  65,280 and D1's placement does not reach them at all — they are per-store
  side tables, not pages in the id space. The moment FM3 lets a data page exist
  above region 0, a peer cannot be granted rights over it: `GrantWritePages`
  drops the id at `device_page_store.cpp:462`'s explicit `if`, `GrantFaultPages`
  loses it to `FreeMapAllocate`'s silent no-op, and the failure then surfaces
  one layer away, as `MayFault` refusing a page the grant appeared to cover.
  **Built 2026-08-26 as D10(a)**: `rights_regions_` is a
  `std::map<region, RightsRegion>` mirroring `map_regions_`, each half built
  only when something is granted into that region, and both `HasFaultRight`
  and `HasWriteRight` lost their id ceiling. `GrantFaultPages` no longer
  clamps an extent — an extent is an id range and nothing confines it to one
  region, so the loop creates each region's bitmap as it reaches it — and
  `TryClaimByStamp`'s ceiling moved to `kMaxPageCount`. Never persisted, so
  there was no format question and no migration, which is what made growing
  them the cheap answer.
  **D5(a) built the same day**: `RefreshFreeMapFromDevice` refreshes *every*
  resident region rather than region 0 alone. The reason it had to is
  structural — `Open` runs before the lease is installed, so a peer loads
  every region the device holds and every one of them goes stale from that
  moment; refreshing only the first left the rest frozen at mount. The
  scratch-validate-union discipline is per region and unchanged, and a region
  the peer created privately after its lease was installed is skipped, since
  the device holds no such page.
  **One cost D1 named did not arise.** §2's candidate A warned that `MayFault`
  would need a third clause, because a map page above `kFirstUserPageId` is
  one a peer must fault. It does not: D4(a) keeps map pages store-owned and
  out of `frames_`, so no map page is ever faulted and `MayFault` is never
  asked about one. The two decisions cancelled.
- **FM8 — residency and the pinned class. Built 2026-08-26.**
  `IsPinnedClass` gained both halves: the kind half now names `kFreeMap` and
  `kHeaderlessMap` beside `kCabinBound`, and an *arithmetic* half above it —
  `IsMapPageId` answers without a resident frame to read a header off, which
  the id-range half could not do for a map page scattered above
  `first_evictable_page_id_`. Under D4(a) this is a guard against a future
  that pools map pages rather than a live case, and it is written as
  arithmetic precisely so that future cannot get it wrong.
  **§5's single-digit target is retracted, not met.** It assumed a hint-local
  working set over a partly-resident map. FM2 loads every region that exists,
  so resident map pages are `regions + regions-with-a-headerless-bitmap` — a
  number that tracks file size, not access pattern. FM10 reports it rather
  than checking it against a target that no longer describes the design.
- **FM9 — mount validation. Built 2026-08-26, as D7(a) — and D7 turned out
  to be narrower than it read.** Loading every region at mount is *forced* by
  a signature, not chosen: `IsAllocated` and `IsHeaderless` are
  `const noexcept` and sit on the fault path, the write-back path and the WAL
  gate, so neither can read a device or report a failure, and D7(b)'s
  load-on-first-touch would need a `mutable` cache with a swallowed error —
  the exact failure shape RV3 converted into a refusal. Since a region must be
  loaded to be trusted, and loading validates, D7(a) follows. `fresh`
  detection stayed region 0's business, and a torn map page refuses the mount
  with `Corruption` (`ATornMapPageRefusesTheMountRatherThanServingIt`).
  What this leaves genuinely open is *cost*, not correctness: mount reads one
  page per region, which is 32,896 scattered reads for a full 16 TiB file.
  Nothing in the engine can produce such a file yet, and when something can,
  the fix is a batched `ReadPageRun` over the map ids rather than a change of
  policy.
- **FM10 — observability. Built 2026-08-26.** `SHOW META` prints
  `map_regions`, `map_pages_resident`, `map_coverage_ids` and
  `headerless_pages`, unconditionally — a one-region database's `1` is the
  answer that says the multi-page map costs nothing here, and
  `map_pages_resident` below twice `map_regions` is FM6's saving made visible
  rather than assumed. Reported through a `PageStore::MapResidency` virtual
  defaulted to a map of nothing, so the dispatcher stays blind to which
  concrete store it holds, which is the seam every other store-shaped
  reporting path uses.
  **D8(a) built with it**: `allocated_pages()` is maintained, not swept —
  seeded at mount (which already reads every region, so the seed is free) and
  moved by each site that sets a free-map bit. One of those sites is not in
  the store: `ExtentAllocator::Reserve` marks through the raw span, so it
  reports its run through `NoteAllocated`, and it may add rather than re-scan
  because the probe next door proved every bit of that run clear.
  `RefreshFreeMapFromDevice` is the one path that changes bits without being
  able to report how many, so it recounts.
- **FM11 — the ceiling tests, moved. Built 2026-08-26.** Two rounds, because
  the ceiling moved twice in one day. FM3's round rewrote the three tests that
  *failed* when the instance ceiling became `kMaxPageCount` — and there were
  three, not the two this row originally named: `tests/wal_high_water_test.cpp`
  was missed. FM6's round rewrote two more that failed when the headerless
  bitmap stopped being built unconditionally, and one of them,
  `FlushWritesIdSortedWithTheFreeMapLast`, came out **stronger**: it now pins
  the no-headerless case (free map last, nothing else) *and* the case that
  actually exercises the ordering rule (create a headerless page, then assert
  headerless-then-free-map). `tests/extent_lease_test.cpp`'s coverage test is
  a per-page-coverage test now, with a new instance-ceiling test beside it,
  and `tests/free_map_test.cpp`'s out-of-range cases were already per-page and
  needed no change.
  A third round followed FM6's correction: eight tests asserted the old
  `allocated_pages()` totals, which fell by one per region when the
  headerless id stopped being reserved. Three of those are pre-existing
  (`FreshDeviceHasOnlyTheTwoMapsAllocated`, now
  `...HasOnlyTheFreeMapAllocated`, and the two reopen tests) and five are this
  series'.
  **Not done, and named rather than implied**: the `sim/` run over a device
  spanning *two regions*. Note what did happen, though — `sim/` ran on region
  0 alone and still caught FM6's defect, because the flaw was in what a
  region's creation marks rather than in crossing between regions. Reaching
  region 1 needs the harness to place pages above 65,280, which is a
  `SimPlan` change and not a test edit. §8 carries it with the other owed
  run.

### 6a. Reclamation is out of scope and is **not** a prerequisite

Nothing in this plan frees a page, and nothing in it needs anything to.
Reclamation is blocked elsewhere and for its own reasons —
`docs/spec/physical-optimizer.md` §6 gate 3 (a reallocated page breaks trail
validation), `docs/inflight/known-gaps.md`'s record that `DROP TABLE` orphans pages by
decision, and `extent_lease.hpp`'s "nothing frees an extent" — and none of
those is made easier or harder by how many bitmap pages exist. `FreeMapFree()`
does not exist and this plan does not add it.

**The one thing that does couple them, stated so it is not discovered later:**
with nothing freeing pages, a larger map raises the ceiling on **leaked** pages
exactly as much as on live ones. Today a runaway `DROP TABLE`/rebuild loop
stops at 510 MiB — badly, with `OutOfSpace`, but it stops. After this work the
same loop consumes up to 16 TiB before anything refuses. That is not an
argument against the work; it is an observation that the **instance ceiling is
currently doing duty as a leak bound**, and that removing it makes
reclamation's absence a bigger number rather than a new problem.

---

## 7. Named decisions — do not assume

### D1. Placement arithmetic — **SETTLED 2026-08-26: candidate A**

§2 states the two candidates and their costs. The operator chose **candidate
A**: region `N` is the ids `[N × 65,280, (N+1) × 65,280)`, its free map at
`N × 65,280 + 1` and its headerless map at `+ 2`.

What that buys, and what it obliges, both already priced in §2: region 0 yields
1 and 2, so a database inside one region is byte-identical — **no superblock
version bump, no migration** — and the obligations are `MayFault`'s third
clause (a peer must be allowed to fault a map page above `kFirstUserPageId`),
`IsPinnedClass`'s id-range half no longer catching a map page, and the `+1`/`+2`
off-by-one existing in exactly one place. FM1 discharged the last of those; the
first two are FM7 and FM8.

The arithmetic lives in `include/kds/storage/free_map.hpp`'s placement section
and nowhere else, and `kFreeMapPageId` / `kHeaderlessMapPageId` are
`static_assert`ed against `FreeMapPageIdFor(0)` / `HeaderlessMapPageIdFor(0)` at
their declarations, so the two fixed ids stopped being independent facts.

### D2. How the headerless map answers per read, once it is not one page — **SETTLED 2026-08-26: (a), after the premise was corrected**

**Read FM6 before the options below.** The question as posed rests on §5's
claim that `IsHeaderless` could cost a second fault per fault, and FM2
dissolved that claim by loading every region at mount — the lookup never
faults. What survived is a memory figure, and (a) is what the operator chose
against it: a region's headerless bitmap is built only where something is
headerless, and a whole-instance flag answers `false` with no lookup when
nothing is. (c) became moot with the premise. (b) and (d) stay available and
unbuilt.

§5 states the problem: `IsHeaderless` sits on the fault, write-back and
WAL-gate paths, and those follow a scattered working set rather than a hint.

- **(a) A whole-instance fast path.** Carry "this database has zero headerless
  pages" as a cheap durable fact and answer `false` with no lookup. Correct by
  construction for every database with no Waystone directory, which is the
  common case; buys nothing for one that has any. Needs the fact to be durable
  and maintained by exactly one writer, `CreateNewHeaderlessUnpinned`.
- **(b) Reserve an id region for headerless pages**, so the answer is
  arithmetic and no bitmap is read at all. Retires the headerless map
  entirely — the strongest option and the most invasive, because it makes page
  class a property of *where a page is*, which is a new rule for the id space
  and interacts directly with D1.
- **(c) Pin the covering pages of the Waystone allocation region.** Headerless
  pages come from ordinary allocation, so they cluster in whatever extent was
  current; pin those few headerless-map pages resident. Cheapest to build, and
  it degrades quietly rather than loudly when the clustering assumption stops
  holding — which is a cost, not a feature.
- **(d) A second bit in the free map** — two bits per page id, "allocated" and
  "headerless" — so one map page answers both questions and there is one
  family of map pages instead of two. Halves per-page coverage to 32,640 ids
  and doubles the page count, but the resident set becomes the *same* pages
  the allocator already holds, which is the only option that makes the
  headerless answer free rather than cheap. It is a format change to a page
  class with exactly one reader and one writer.

(b) and (d) are mutually exclusive, and both change what §2's "the two bitmaps
stay adjacent" even means.

### D3. May an extent straddle a map-page boundary? — **SETTLED 2026-08-26: (a), refuse**

`Reserve` marks `count` contiguous ids (`extent_lease.cpp:40`). Across a
boundary that is two pages dirtied by one reservation. The operator chose
**(a)**: a run that would cross advances the hint to the next region, so a
reservation stays one map page, one dirty flag and one crash window, at a cost
of at most 63 ids per 65,280 — under 0.1%. FM4 builds it; the wasted ids are
never reclaimed, per §6a, so they are a permanent 0.1% and are named as such.

- **(a) Refuse to straddle** — *chosen* — advance the hint to the next region when a run
  would cross. Keeps a reservation to one page and one dirty flag; wastes up
  to `count − 1` ids per boundary (at `kDefaultExtentPages = 64`, at most 63
  ids per 65,280 — under 0.1%).
- **(b) Allow straddling** — the allocator holds two map pages for the
  duration of a marking loop. No waste; the marking loop stops being
  single-page, and the crash window now spans two pages that must both land.

### D4. Do map pages become buffer-pool frames, or stay store-owned? — **SETTLED 2026-08-26: (a), store-owned**

§3 shows neither is circular. The choice is real anyway. The operator chose
**(a)**: a small keyed cache of store-owned map pages, each with its own dirty
flag. `FlushMaps`'s write-maps-after-data ordering therefore stays trivially
true, map pages stay out of the checkpoint dirty table and away from `PageRef`,
and the cost accepted is a second caching mechanism inside the store. FM2 builds
it. Note the consequence for D9: with map pages outside `frames_` they are also
outside `DirtyPagesWithRecLsn()`, so logging the map (D9(b)) would want this
revisited rather than merely extended.

- **(a) Stay store-owned members** — *chosen*, as today, in a small keyed cache with its
  own eviction (or none). Keeps `FlushMaps`'s ordering guarantee trivially,
  keeps maps out of the checkpoint dirty table, and keeps `PageRef` out of the
  picture. Costs a second, parallel caching mechanism inside the store.
- **(b) Ordinary frames**, pinned-class per FM8. One caching mechanism, and
  map pages become visible to `DirtyPagesWithRecLsn()` — which matters only if
  D9 makes them logged. Costs the flush-ordering fix §3 names: `WriteBack`
  sorts ascending, so map pages must be excluded from the ordinary sweep and
  written in a second pass, or a low-id map page is published ahead of the
  data it describes.

### D5. Peer coherence for cached map pages — **SETTLED 2026-08-26: (a)**

Built as "refresh every resident region", which is (a) minus its faulting
half: a peer needs no on-demand fault because `Open` runs before the lease is
installed and loads every region the device holds. (a)'s stated requirement —
a third clause in `MayFault` — did not arise either, because D4(a) keeps map
pages out of the pool entirely. See FM7.

A peer's map copy is frozen at its `Open()` (`core_runtime.cpp:54`), and that
is sound today because `IsAllocated` short-circuits on `lease_->Owns()`
(`device_page_store.cpp:182`) and because a peer's stale view is a retryable
not-found by `crosscore.md` §5. Multi-page multiplies the objects that can be
stale, and `EvictClean` — the invalidation route core 0 uses for catalog pages
(`device_page_store.hpp:378`) — has no map equivalent.

- **(a) Nothing changes**: a peer loads region 0 at `Open`, faults further map
  pages read-only on demand, and staleness stays a retryable not-found.
  Requires that the *fault* be permitted, which under candidate A is
  `MayFault`'s missing third clause.
- **(b) A peer never reads a map page at all** — `IsAllocated` answers from
  the lease and the CC7 grants alone on a leased store, and a question outside
  both is not-found. Removes the coherence question rather than answering it;
  changes what a peer can say about a page core 0 allocated after the peer
  started.

### D6. `docs/spec/page.md` §4/§5 vs. `superblock.hpp` — which is right? **Still open.**

Untouched by FM1-FM11, and deliberately: neither placement candidate needed a
stored root, so the whole series ran without resolving the contradiction. It is
a documentation defect rather than a design one — `page.md` describes a field
the superblock does not have and never had — and it outlives this workplan.

§4 says the superblock anchors the "free-map root, high-water"; §5 repeats it.
`include/kds/server/superblock.hpp:16-20` says the opposite, in as many words
and with a reason ("two records of the same fact is one record too many"), and
the code has no such field. Options: **(a)** amend `page.md` §4/§5 to match
the code, since neither §2 candidate needs a stored root; **(b)** add the
fields, which is a superblock version bump and reopens whether placement is
arithmetic at all. Flagged rather than guessed, per CLAUDE.md.

### D7. Mount validation scope — **SETTLED 2026-08-26: (a), and forced rather than chosen**

The choice below reads as a policy question and is not one. `IsAllocated` and
`IsHeaderless` are `const noexcept` and sit on the fault path, the write-back
path and the WAL gate; neither can read a device or report a failure. (b)'s
load-on-first-touch therefore needs a `mutable` cache with a swallowed error —
the failure shape RV3 deliberately converted into a refusal — so a region must
be loaded to be trusted, and loading is validating. What stays open is the
*cost*: 32,896 scattered reads at a full 16 TiB, wanting a batched
`ReadPageRun` before anything can produce such a file. See FM9.

Today `Open` validates both map pages at the door
(`device_page_store.cpp:38`, `:64`). With N regions: **(a)** validate every
map page at mount — O(regions) reads, one sequential run under candidate B and
scattered under A, and a torn map refuses the mount as RV3 taught; **(b)**
validate region 0 at mount and the rest on first touch — constant mount cost,
but a torn map page surfaces mid-statement, which is the failure shape RV3
deliberately converted into a refusal.

### D8. What `allocated_pages()` means once it must sweep — **SETTLED 2026-08-26: (a), maintained**

It is `FreeMapCountAllocated` over one page (`device_page_store.cpp:186`),
`std::uint32_t`, printed at mount, at shutdown and by `SHOW META`
(`src/server/main.cpp:334`, `:349`; `src/server/expeditor.cpp:882`, `:1568`).
Over 32,897 map pages it is either a sweep of every one in existence or a
maintained counter. **(a)** a maintained running count, durable somewhere,
with one writer; **(b)** count only the resident map pages and rename it so
the number does not read as the instance total; **(c)** drop it from the mount
and shutdown lines and keep it as a diagnostic that says it is expensive.

### D10. What happens to the two grant bitmaps when the map's ceiling lifts? — **SETTLED 2026-08-26: (a), grow per region**

§9's second finding: `fault_rights_` and `write_rights_` are single-page
bitmaps over absolute page ids, so FM3 would leave a peer unable to hold rights
over any page above region 0. **(a)** grow them as FM2 grows the map — a keyed
set of resident right-pages per store, 16 KiB becoming 16 KiB per touched
region, and since they are never persisted there is no format question;
**(b)** replace them with a representation that does not tile the id space —
the extent vector and sorted page vector that preceded them, which PW1c-7's
review replaced precisely because re-delivery repeats grants and both checks
sit on the frame-load path, so this reopens a settled question rather than
answering a new one; **(c)** bound them by ownership instead — a peer's rights
are its lease's ranges plus its exact grants, which is D5(b)'s shape one level
up and retires the bitmaps rather than sizing them. Nothing here is decided,
and FM7 does not start until it is.

### D9. Does the free map become logged as part of this?

`RecordType::kAlloc`/`kFree` are assigned (`include/kds/wal/record.hpp:58`,
`:64`) with a payload codec (`include/kds/wal/payload.hpp:333`) and **no
emitter anywhere in `src/`**; `docs/spec/page.md` §5 nonetheless describes the free
map as "a headered, logged page class replayed like any other", and
`src/server/expeditor.cpp:1163` records the truth — it is unlogged, which is
why RC04's `RaiseAllocationFloor` exists to repair it. Multi-page changes the
stakes: today losing the map write loses the whole instance's allocation
record, and afterwards it loses one region's. **(a)** stay unlogged and keep
RC04's repair, extended to find the right region; **(b)** emit ALLOC/FREE and
let redo replay map pages, which retires the repair and makes D4(b)
load-bearing. Out of scope for the series above either way — recorded because
FM5 and FM9 both want the answer, and because `page.md` §5 currently asserts
one the code does not implement.

---

## 8. What must be measured, and where

Nothing is measured in this document — no code changed, and this worktree has
no `build-release`. Two numbers gate the work itself, both in `build-release`
with interleaved A/B per CLAUDE.md:

1. **FM2's byte-identity claim — asserted structurally, not measured.**
   Stated plainly because the difference matters: what was built and tested is
   that a one-region database writes exactly the two map pages it always did,
   at ids 1 and 2, never grows the file past region 0, and reports the same
   `allocated_pages()`; plus the whole 2,639-test suite over unchanged on-disk
   formats. What was *not* done is producing a data file from the pre-FM2
   build and differencing the bytes, which is what "byte-identical" says. The
   structural assertions plus an unchanged codec are strong evidence and are
   not the same claim. The differencing run is still owed.
2. ~~**FM6's fault-path cost.**~~ **Withdrawn — the cost it would measure does
   not exist.** §5 argued `IsHeaderless` could double the miss path because
   the headerless map would be partly resident; FM2 loads every region at
   mount, so the lookup never faults. What replaced it is a memory figure, not
   a time one, and it is arithmetic rather than measured: 16 KiB per 510 MiB
   of file, halved by FM6 for any database with no Waystone directory, and
   reported per instance by `SHOW META`'s `map_pages_resident`.

3. **The `sim/` two-region run (FM11).** Owed. Reaching region 1 needs the
   harness to place pages above 65,280, which is a `SimPlan` change rather
   than a test edit.

## 9. Two findings this plan surfaced — now recorded

The 65,280-page / 510 MiB instance ceiling is enforced in four places on
`multi-free-map` at `d8be95a` (`device_page_store.cpp:173`, `:403`, `:479`;
`extent_lease.cpp:25`) and pinned by three test files, but it was **not in
`docs/inflight/known-gaps.md`** — which is where CLAUDE.md says engine-wide gaps live,
and which discussed free-map reuse being gated without ever saying how small
the map is. Added there 2026-08-23, under "Storage and key modes", including
the §6a coupling: the ceiling is currently the engine's only bound on leaked
space, so lifting it raises the ceiling on orphaned pages as much as on live
ones.

**Second: there are four single-page bitmaps, not two, and the other two are
not in the id space.** Read on `worktree-workplan-multi-free-map` at `56b20d2`.
`fault_rights_` and `write_rights_` (`device_page_store.hpp:732-733`) share the
free map's codec and its `kFreeMapBitsPerPage` coverage, but they are per-store
side tables addressed by absolute page id, deliberately never persisted. The
consequence is that lifting the *map's* ceiling does not lift theirs: after FM3
a peer core's rights would stop at 510 MiB whatever the map says, and the
refusal appears at `MayFault` rather than at the grant — the failure shape
CLAUDE.md's truthfulness rule exists to prevent. The declaration comment at
`:370-372` already says ids beyond coverage hold no bit and calls it "the
store's existing ceiling", so this is a documented consequence that becomes a
defect only when the ceiling it refers to moves. It is FM7's work and D10's
decision, recorded here because a reader of §1.6's table would not find it:
those two bitmaps appear in neither of that section's two lists.
