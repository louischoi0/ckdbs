#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store.hpp"

// The var-heap: the out-of-line store for values too long to fit in a
// tuple's fixed-width tagged cell (docs/spec/heap-and-tuple.md section 3.4,
// docs/rules/rule-fixed-length-tuple.md section 5).
//
// ---- The design goal is to be boring -------------------------------------
//
// Fixing the tuple length removed the mobility problem from the heap. This
// file must not reintroduce it, which is what every rule below is for.
//
// **Values are immutable per version.** An append writes `{len, bytes}` and
// returns a pointer; nothing is ever rewritten and nothing is ever moved.
// There is deliberately no Update(). Four things fall out of that one rule,
// and they are the rationale rather than side effects:
//
//   - MVCC correctness is free. An old-version reader follows an old
//     pointer to bytes that *cannot* have changed, so there is nothing to
//     version and nothing to copy on write.
//   - Pointers need no epoch, no validation and no forwarding - unlike a
//     Waystone entry, which is advisory precisely because what it points at
//     can move.
//   - The class is relayout-exempt by construction. The physical optimizer
//     has no reason to touch a kVarHeap page, so it never will.
//   - Reclamation is not new machinery: it rides on purge. When a version
//     dies its values die with it.
//
// ---- The one operation that ends a value: Release() ----------------------
//
// This file said "no Update() and no **Free()**" until 2026-08-28, and
// nothing reclaimed. `PageRelease` is that gap closed, and it weakens no
// line above it - its contract is stated once, on its own declaration
// below.
//
// **Who may release, and when, is not this layer's decision.** A version's
// slots die when the version does: at rollback, or when the writer that
// superseded or deleted it falls below `ReadHorizon()`. This file only
// performs it, exactly as it performs an append without deciding what
// spills. `docs/spec/txn.md` §3.3 and §6 own the lifetime.
//
// A page whose every slot is a tombstone holds nothing reachable and is
// **reformatted in place** to become an append target again - which is why
// there is no free list here and no call into the free map: the chain stops
// growing without either.
//
// The accepted cost, recorded rather than hidden: churn-heavy string
// updates consume space until the release drains, which makes drain cadence
// a capacity input rather than a background detail. And a crash forgets the
// pending-release set, leaking those slots until a mount-time sweep exists.
//
// ---- Authoritative, not advisory -----------------------------------------
//
// kVarHeap is headered, checksummed and WAL-logged (`VARHEAP_APPEND`).
// Stated explicitly because everything added to this engine lately -
// waystone pages, the trail directory - was advisory, and the rules for
// those must not be pattern-matched onto this one. Losing a var-heap value
// loses a committed value, not a hint.
//
// ---- Page layout ---------------------------------------------------------
//
//   [ common page header ]  <- offset 0, 32 bytes (page_header.hpp)
//   [ VarHeapPageHeader   ]  <- kVarHeapHeaderOffset, 8 bytes
//   [ slot directory      ]  <- grows downward; one entry per value
//   [ ... free space ...  ]
//   [ value bytes         ]  <- grows upward from the tail reservation
//   [ next_page_id        ]  <- last sizeof(PageId) bytes, permanently reserved
//
// Deliberately the same shape as a heap page, for the same reason a btree
// leaf is one: a second slotted-page layout would be a second set of
// off-by-one bugs. What it is *not* is a heap page - there is no MVCC tuple
// header, no delete-mark and no slot retirement, because a value has no
// lifetime of its own. It lives and dies with the version pointing at it.
//
// ---- Chain --------------------------------------------------------------
//
// One chain per relation, rooted at `sys.tables.varheap_page_id`, grown by
// tail append exactly as heap_chain.cpp grows a heap: a full tail allocates
// a page, the value is written into it, and only then is the link
// published, because the link is what makes a page reachable. Per-relation
// rather than instance-wide so a future DROP TABLE reclaims one chain
// instead of sweeping a shared one.
//
// Unlike a heap chain there is no min_key and no ordering property: values
// are addressed only by the pointers in the tuples that own them, so a walk
// is never a search.
//
// ---- Concurrency ---------------------------------------------------------
//
// Free functions over a caller-supplied PageStore, holding no state. The
// caller holds whatever pin/latch the pages need, as with heap_chain.

namespace kds::varheap {

// ---- Page header ---------------------------------------------------------

inline constexpr std::size_t kVarHeapHeaderOffset = storage::kPageBodyOffset;

struct VarHeapPageHeaderFields {
    std::uint16_t flags;
    std::uint16_t nr_slots;
    std::uint16_t lower;  // offset just past the last slot entry
    std::uint16_t upper;  // offset of the start of the last value written
};

inline constexpr std::size_t kHeaderFlagsOffset = 0;
inline constexpr std::size_t kHeaderNrSlotsOffset = 2;
inline constexpr std::size_t kHeaderLowerOffset = 4;
inline constexpr std::size_t kHeaderUpperOffset = 6;
// 2+2+2+2 = 8, every field naturally aligned and no tail padding.
inline constexpr std::size_t kHeaderSize = 8;

static_assert(offsetof(VarHeapPageHeaderFields, flags) == kHeaderFlagsOffset);
static_assert(offsetof(VarHeapPageHeaderFields, nr_slots) == kHeaderNrSlotsOffset);
static_assert(offsetof(VarHeapPageHeaderFields, lower) == kHeaderLowerOffset);
static_assert(offsetof(VarHeapPageHeaderFields, upper) == kHeaderUpperOffset);
static_assert(sizeof(VarHeapPageHeaderFields) == kHeaderSize);

inline constexpr std::uint16_t kHeaderFlagInitialized = 0x1;

// ---- Slot directory ------------------------------------------------------
//
// Two fields, and no flags - including for the lifetime a slot gained in
// 2026-08-28, which needed no flags byte because a released slot is
// expressible in the field the format already had (see `offset` below). A
// value still has no lifetime of its own; it has its owner's.

struct VarHeapSlotFields {
    // Absolute page offset of the value bytes, or **0 for a released
    // slot**. Zero is an unambiguous tombstone rather than a chosen
    // sentinel: a live value's offset is never below the page header, so no
    // legal value can carry it. That is what let a lifetime arrive here
    // without a format change or a flags byte.
    std::uint16_t offset;
    // Value length in bytes. Kept across a release, so a tombstone still
    // says how much it held.
    std::uint16_t length;
};

// The offset value that marks a released slot. Named rather than spelled
// `0` at each site, because "the slot is dead" and "the offset happens to
// be zero" must not be two readings of one literal.
inline constexpr std::uint16_t kReleasedSlotOffset = 0;

inline constexpr std::size_t kSlotOffsetOffset = 0;
inline constexpr std::size_t kSlotLengthOffset = 2;
inline constexpr std::size_t kSlotOnDiskSize = 4;

static_assert(offsetof(VarHeapSlotFields, offset) == kSlotOffsetOffset);
static_assert(offsetof(VarHeapSlotFields, length) == kSlotLengthOffset);
static_assert(sizeof(VarHeapSlotFields) == kSlotOnDiskSize);

inline constexpr std::size_t kNextPageIdOffset = kPageSize - sizeof(PageId);

// The largest value a var-heap page can hold: everything between an empty
// page's lower and upper, less the one slot it needs.
//
// **This is also the spilled-value size cap, and that cap is an [OPEN]
// decision** (docs/rules/rule-fixed-length-tuple.md section 9: "uncapped blobs
// are not obviously an OLTP feature"). Nothing here decides it. A value
// larger than one page needs a multi-page representation, and rather than
// invent one to answer a question nobody has settled, an oversize value is
// refused with Unsupported. Every option stays open: a future cap can be
// lower (a policy check above this layer) or higher (a chained
// representation behind this same Append/Fetch pair).
inline constexpr std::size_t kMaxValueSize =
    kNextPageIdOffset - (kVarHeapHeaderOffset + kHeaderSize) - kSlotOnDiskSize;

static_assert(kMaxValueSize == 8144);

// ---- The pointer ---------------------------------------------------------
//
// What a kSpilled tagged cell carries: `page_id u32 | slot u16 | reserved
// u16`, packed into the u64 the cell has room for
// (storage/tagged_cell.hpp).
//
// Encoded with explicit shift/mask helpers, never a bitfield: this is an
// on-disk format and bitfield layout is implementation-defined
// (docs/rules/rules.md section 5, invariant 6).

struct VarHeapPtr {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;

    bool operator==(const VarHeapPtr&) const = default;
};

inline constexpr int kVarHeapPtrPageIdShift = 32;
inline constexpr int kVarHeapPtrSlotShift = 16;

constexpr std::uint64_t EncodePtr(VarHeapPtr ptr) noexcept {
    return (static_cast<std::uint64_t>(ptr.page_id) << kVarHeapPtrPageIdShift) |
           (static_cast<std::uint64_t>(ptr.slot) << kVarHeapPtrSlotShift);
}

constexpr VarHeapPtr DecodePtr(std::uint64_t word) noexcept {
    return VarHeapPtr{static_cast<PageId>(word >> kVarHeapPtrPageIdShift),
                      static_cast<std::uint16_t>((word >> kVarHeapPtrSlotShift) & 0xFFFF)};
}

static_assert(DecodePtr(EncodePtr(VarHeapPtr{0x01020304u, 0x0506})) ==
              VarHeapPtr{0x01020304u, 0x0506});

// ---- Page operations -----------------------------------------------------

// Formats `page` as a brand-new, empty var-heap page. `owner_oid`
// (page.md §2a) is the owning relation's oid — a chain is per-relation, so
// every page of it carries the same owner; 0 only from replay of a
// pre-§2a record.
Status FormatPage(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid = 0);

// Appends `value` to this page alone, returning its slot. Fails with
// OutOfSpace if the page has no room - which is the signal the chain uses
// to grow, exactly as PageView::InsertTuple's is.
StatusOr<std::uint16_t> PageAppend(std::span<std::byte, kPageSize> page,
                                    std::span<const std::byte> value);

// Writes a value at a **named** slot, for redo
// (docs/workplan-wal-recovery.md RC03). The counterpart of
// PageView::RedoWriteTuple and txn::UndoPageWriteAt, and it exists for the
// reason VARHEAP_APPEND records its slot at all: the tuple cell that points
// here already carries `(page_id, slot)`, so a value that came back at a
// different slot would be a value silently swapped for another.
//
// `slot == nr_slots` appends exactly as PageAppend does; `slot < nr_slots`
// is a re-application and must find the same length already there, which is
// what makes replaying a record twice a no-op (values are immutable per
// version - invariant 14 - so identical bytes are the only legal
// re-application). A slot past the end is Corruption: slots are dense.
Status PageWriteAt(std::span<std::byte, kPageSize> page, std::uint16_t slot,
                   std::span<const std::byte> value);

// Reads the value at `slot`. The returned span points into `page` itself,
// so it is valid only while those bytes stay alive and untouched - callers
// that keep the value must copy it. Fails with Corruption for a slot that
// is out of range or whose extent does not lie inside the page.
StatusOr<std::span<const std::byte>> PageRead(std::span<const std::byte, kPageSize> page,
                                               std::uint16_t slot);

// Tombstones `slot`: its value is dead and its bytes are no longer
// reachable.
//
// **Writes a slot, never a value.** The value's bytes stay exactly where
// they are, so invariant 14 holds to the letter - a released value is
// *dead*, not moved and not rewritten.
//
// **Idempotent.** Releasing a released slot writes the same zero, which is
// what makes it legal both as a rollback compensation replayed after a
// crash (`recovery_undo.hpp` - no CLR) and as a redo application.
//
// **Frees no space.** Slots are dense and values are packed from the tail,
// so one dead value in a live page is a hole nothing can use. Space comes
// back a whole page at a time, when every slot is a tombstone and the page
// is reformatted - which is what `PageLiveSlots` is for.
//
// Fails with Corruption for a page that is not a kVarHeap page, and for a
// slot past this page's directory - both meaning "this record is not this
// page's". Never for a slot already released.
Status PageRelease(std::span<std::byte, kPageSize> page, std::uint16_t slot);

// How many of the page's slots still hold a value. `0` on a page whose
// `PageSlotCount` is non-zero means every value it ever held is dead, and
// the page can be reformatted in place and re-used.
std::uint16_t PageLiveSlots(std::span<const std::byte, kPageSize> page);

std::uint16_t PageSlotCount(std::span<const std::byte, kPageSize> page);
std::uint16_t PageFreeSpace(std::span<const std::byte, kPageSize> page);
PageId PageNextPageId(std::span<const std::byte, kPageSize> page);

// ---- Chain operations ----------------------------------------------------

// Creates a relation's var-heap root page and returns its id. Called once,
// at CREATE TABLE, for a relation whose schema can spill - so the root is
// fixed by DDL and stays a cacheable fact (catalog_cache.hpp's rule).
// `owner_oid` is that relation's oid (page.md §2a) — deliberately not
// defaulted: every chain has a relation, so every caller can say which.
StatusOr<PageId> CreateChain(storage::PageStore& store, std::uint64_t owner_oid);

// What an append changed besides writing the value, so a logging caller can
// describe all of it.
//
// **Why this is a struct and not just a pointer.** A `VARHEAP_APPEND` record
// says "these bytes, at this slot, on this page". It does not say the page was
// *created*, and it does not say the previous tail's next-page link now points
// at it - and both of those happen here, in memory, through the store's plain
// allocation path. A caller that logged only the append left recovery with a
// record naming a page nothing creates (redo refuses the mount) or a value
// page that exists and is unreachable (silent loss). The heap and btree paths
// already report their structural changes for exactly this reason
// (`insert_placement.hpp`, and `command_dispatcher.cpp`'s loop over them);
// this is the var-heap's, and its absence was `docs/inflight/known-gaps.md`'s var-heap
// entry.
struct ChainAppendResult {
    VarHeapPtr ptr;

    // The page this append created, or kInvalidPageId when the tail had room.
    // Needs a PAGE_INIT with page_type kVarHeap before the append record -
    // which `wal::ApplyPageInit` already formats, so nothing new is needed on
    // the replay side.
    PageId created_page_id = kInvalidPageId;

    // The old tail, whose next-page link now points at `created_page_id`.
    // kInvalidPageId whenever nothing was linked. No record type describes a
    // link edit, so the caller logs a full page image - the same answer the
    // heap path gives for the same question.
    PageId linked_page_id = kInvalidPageId;

    bool grew() const noexcept { return created_page_id != kInvalidPageId; }
};

// Appends `value` to the chain rooted at `root`, growing it by one page if
// the tail is full, and returns the pointer to write into the tuple's cell
// plus whatever growth it had to do (above).
//
// Fails with Unsupported if `value` is larger than kMaxValueSize (see
// there - the cap is an open decision this layer refuses to make), and
// with whatever the store reports otherwise.
StatusOr<ChainAppendResult> ChainAppend(storage::PageStore& store, PageId root,
                                        std::span<const std::byte> value,
                                        std::uint64_t owner_oid);

// Resolves `ptr`. Fetches exactly one page - the pointer names it directly,
// so this is never a walk.
//
// The returned span points into a page frame owned by `store`. It is valid
// only until the caller touches the store again; every caller in the engine
// copies it immediately, which is also what keeps the nested-access rule
// (docs/spec/parser-v2.md I15 R1) satisfiable on the decode path.
// `pin` keeps the value's page resident: the span points into its frame,
// and dies with the pin, not with the call. Passing a fresh PageRef per
// call is the whole contract.
StatusOr<std::span<const std::byte>> Fetch(storage::PageStore& store, VarHeapPtr ptr,
                                           storage::PageRef& pin);

// Pages in the chain. For tests and inspection, not a hot path.
StatusOr<std::uint32_t> ChainLength(storage::PageStore& store, PageId root);

}  // namespace kds::varheap
