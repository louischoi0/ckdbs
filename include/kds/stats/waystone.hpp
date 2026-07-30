#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/keystone.hpp"

// Waystone: a per-relation, full-coverage, pk-addressed record of where
// each tuple was last seen and how hot it is (docs/waystone-concpets.md;
// supersedes the bounded metadata pool of heap-and-tuple.md section 5).
//
// This header is format and contract only - entry layout, the arithmetic
// that turns a pk into an address, the directory's shape, and the seams
// consumers talk to. The directory walk, the hooks and the probe live in
// their own translation units above it.
//
// ---- The two facts, with different guarantees (spec section 3) ----------
//
//   Existence   guaranteed 100%. Every tuple of an enabled relation has an
//               entry, written on the insert path as one O(1) arithmetic
//               store. This is why a probe can distinguish "no such row"
//               from "row exists, location unknown".
//   Location    advisory, never authoritative. Trusted only while the
//               entry's page_epoch still matches the heap page's, and only
//               after the Keystone id at the target has been checked.
//
// Heat (use_count/last_ts) is best-effort throughout: events may be
// dropped under pressure and nothing downstream may depend on a count
// being exact.
//
// ---- Read-path probes are permitted (amended 2026-07-30) ----------------
//
// Invariant 8 used to read "Waystone pages are never on the normal read
// path". Spec section 3.1 replaced it: a pk point lookup MAY probe, and
// what it owes in return is spelled out in ProbeResult below. The
// invariant that did not change, and the one that actually protects
// correctness, is that Waystone is never *authoritative* - deleting it
// wholesale may cost performance and must never change a result.
//
// A note that will expire: in today's engine no tuple ever moves. Tail
// append never splits a page, UPDATE overwrites in place, DELETE is a
// delete-mark, and neither relayout nor compaction exists. So a recorded
// location is exact in practice and the epoch check is trivially true.
// That is a property of what is unimplemented, not a guarantee. Every
// consumer here validates anyway, because the day relayout lands is the
// day the ones that did not become silently wrong.
//
// ---- What is deliberately not here yet ----------------------------------
//
// The event/observer/aggregator half of spec section 9 - TupleAccessEvent,
// AccessObserver, the SPSC ring, Ingest/DecayTick - is deferred with tasks
// T14/T15/T20 (see the 2026-07-30 amendment in waystone-workplan.md). It
// exists to feed relayout, and there is no physical optimizer to feed.
// Declaring the API before writing it would be a guess at the shape of a
// consumer nobody has built. Heat fields are present in the entry format
// because the format is on-disk and must not change later; nothing writes
// them yet.
//
// Concurrency: core-local, owned by the relation's owning core, no
// internal synchronization (rules.md section 3). Every page comes from
// storage::PageStore.

namespace kds::stats {

// ---- Entry format (spec section 5) ---------------------------------------
//
// 256 bits per tuple. Deliberately a power of two: it makes the in-page
// address a shift and a mask rather than a multiply, and it tiles an 8 KiB
// page exactly with nothing left over to account for.

inline constexpr std::size_t kEntrySize = 32;

inline constexpr std::size_t kEntryPkOffset = 0;
inline constexpr std::size_t kEntryPageIdOffset = 8;
inline constexpr std::size_t kEntrySlotOffset = 12;
inline constexpr std::size_t kEntryFlagsOffset = 14;
inline constexpr std::size_t kEntryUseCountOffset = 16;
inline constexpr std::size_t kEntryLastTsOffset = 20;
inline constexpr std::size_t kEntryPageEpochOffset = 24;
inline constexpr std::size_t kEntryReservedOffset = 28;
// 8+4+2+2+4+4+4+4 = 32, every field naturally aligned, no tail padding.
inline constexpr std::size_t kEntryUsedSize = 32;
static_assert(kEntryUsedSize == kEntrySize);

// Entries per page: 8192 / 32 = 256. Exact - no partial entry, no
// per-page slack to reason about.
inline constexpr std::size_t kEntriesPerPage = kPageSize / kEntrySize;
static_assert(kEntriesPerPage == 256);
static_assert(kEntriesPerPage * kEntrySize == kPageSize, "entries must tile the page exactly");

// Entry flags. Only one is assigned; the field is 16 bits so the rest can
// be without a format change.
inline constexpr std::uint16_t kEntryLive = 1u << 0;

// Mirror struct for the codec (rules.md sections 2 and 5): field-wise
// memcpy through the named offsets above, never a reinterpret_cast onto
// page bytes, never a compiler bitfield.
struct WaystoneEntry {
    std::uint64_t pk;           // zero-extended Keystone id; upper 24 bits 0
    PageId page_id;             // last observed heap page; kInvalidPageId if never observed
    std::uint16_t slot;         // last observed slot within that page
    std::uint16_t flags;        // kEntryLive | reserved
    std::uint32_t use_count;    // decayed access counter; best-effort
    std::uint32_t last_ts;      // truncated logical timestamp of last access
    std::uint32_t page_epoch;   // heap-page epoch at observation (spec section 3)
    std::uint32_t reserved;     // 0; future pattern link / argument affinity
};

static_assert(offsetof(WaystoneEntry, pk) == kEntryPkOffset);
static_assert(offsetof(WaystoneEntry, page_id) == kEntryPageIdOffset);
static_assert(offsetof(WaystoneEntry, slot) == kEntrySlotOffset);
static_assert(offsetof(WaystoneEntry, flags) == kEntryFlagsOffset);
static_assert(offsetof(WaystoneEntry, use_count) == kEntryUseCountOffset);
static_assert(offsetof(WaystoneEntry, last_ts) == kEntryLastTsOffset);
static_assert(offsetof(WaystoneEntry, page_epoch) == kEntryPageEpochOffset);
static_assert(offsetof(WaystoneEntry, reserved) == kEntryReservedOffset);
static_assert(sizeof(WaystoneEntry) == kEntrySize);

// Reads/writes the entry at `slot` of an entry page. Both fail with
// OutOfRange for a slot at or past kEntriesPerPage; WriteEntry also fails
// with InvalidArgument for a pk above kMaxPk, which is a caller bug rather
// than damage (ids are range-checked at the front door, invariant 6).
//
// ReadEntry does no validation beyond the slot bound - it is a pure
// decode, like ReadPageHeader. A garbage entry is not distinguishable from
// a plausible stale one anyway, which is precisely why the probe contract
// (spec section 3.1) makes the caller check the Keystone id at the target
// instead of trusting anything this returns.
//
// An all-zero entry decodes to pk 0, page_id 0, flags 0 - which is why
// kEntryLive matters: a never-written entry is indistinguishable from a
// zeroed one, and only the flag says whether it means anything. Callers
// must test kEntryLive, not pk != 0.
StatusOr<WaystoneEntry> ReadEntry(std::span<const std::byte, kPageSize> page, std::size_t slot);
Status WriteEntry(std::span<std::byte, kPageSize> page, std::size_t slot,
                  const WaystoneEntry& entry);

// ---- pk-direct addressing (spec section 4) -------------------------------
//
// The pk *is* the address. There is no handle, no hash, and no lookup
// table - which is the whole reason full coverage was chosen over a
// bounded pool: with every tuple present, arithmetic suffices.
//
//   logical entry page = pk >> 8      (kEntriesPerPage = 2^8 per page)
//   slot within it     = pk & 0xFF
//
// "Logical" because that index is not a PageId: it is the leaf number the
// directory below resolves to a real page.

// log2(kEntriesPerPage) = log2(256) = 8.
inline constexpr int kEntrySlotBits = 8;
inline constexpr std::uint64_t kEntrySlotMask = (std::uint64_t{1} << kEntrySlotBits) - 1;
static_assert((std::uint64_t{1} << kEntrySlotBits) == kEntriesPerPage);

constexpr std::uint64_t LogicalEntryPageOf(std::uint64_t pk) noexcept {
    return pk >> kEntrySlotBits;
}
constexpr std::size_t EntrySlotOf(std::uint64_t pk) noexcept {
    return static_cast<std::size_t>(pk & kEntrySlotMask);
}

// Widest pk the addressing has to cover: the Keystone id is 40 bits
// (keystone.hpp), and ids outside that range are rejected at the front
// door (CLAUDE.md invariant 6), so nothing here has to handle them.
inline constexpr std::uint64_t kMaxPk = (std::uint64_t{1} << kKeystoneIdBits) - 1;

// ---- Page directory (spec section 6) -------------------------------------
//
// Waystone pages cannot be physically contiguous - inserts arrive in id
// order but pages are allocated wherever the store has room - so
// continuity is logical, through the inode-block-map pattern: interior
// pages holding child PageIds, walked by the base-2048 digits of the pk.
//
// Fanout: 8192 / 4 = 2048 PageIds per directory page. Power of two again,
// so a walk is shifts and masks.
inline constexpr std::size_t kDirFanout = kPageSize / sizeof(PageId);
static_assert(kDirFanout == 2048);
static_assert(kDirFanout * sizeof(PageId) == kPageSize, "child ids must tile the page exactly");

// log2(2048) = 11.
inline constexpr int kDirFanoutBits = 11;
static_assert((std::size_t{1} << kDirFanoutBits) == kDirFanout);
inline constexpr std::uint64_t kDirIndexMask = (std::uint64_t{1} << kDirFanoutBits) - 1;

// Tuples covered by a directory of each depth, derived rather than
// tabulated: depth d addresses 2048^d leaf pages of 256 entries each, so
// coverage = 2^(11d + 8).
constexpr std::uint64_t DirCoverageAtDepth(int depth) noexcept {
    return std::uint64_t{1} << (kDirFanoutBits * depth + kEntrySlotBits);
}
static_assert(DirCoverageAtDepth(1) == 524288);            // 2^19, spec section 6
static_assert(DirCoverageAtDepth(2) == 1073741824);        // 2^30, ~1.07e9
static_assert(DirCoverageAtDepth(3) == (std::uint64_t{1} << 41));

// Depth 3 covers 2^41 > the 2^40 pk space, so the directory never needs a
// fourth level and a walk is at most 3 interior hops plus the leaf.
inline constexpr int kMaxDirDepth = 3;
static_assert(DirCoverageAtDepth(kMaxDirDepth) > kMaxPk);
static_assert(DirCoverageAtDepth(kMaxDirDepth - 1) <= kMaxPk,
              "kMaxDirDepth must be the smallest depth that covers the pk space");

// Unpopulated ranges hold this at every level; a walk that meets it stops
// and reports a miss rather than allocating (spec section 6, lazy
// allocation). Sparse id spaces cost only what they touch.
inline constexpr PageId kEmptyDirSlot = kInvalidPageId;

// ---- Epoch seam (spec sections 3 and 11) ---------------------------------
//
// Where a heap page's epoch counter lives - a page-header field or a
// core-local table keyed by page_id - is an [OPEN] decision, as are its
// width and wraparound handling. Nothing here may assume either, so the
// epoch arrives through this seam and the decision stays open.
//
// The default implementation reports a constant for every page. That is
// *correct* today for the reason in this file's header - nothing moves a
// tuple - and it is exactly what stops being correct when relayout lands,
// which is what BumpFor exists to mark.
class EpochProvider {
public:
    virtual ~EpochProvider() = default;

    // Current epoch of `page_id`. A probe trusts a recorded location iff
    // the entry's page_epoch equals this.
    virtual std::uint32_t EpochOf(PageId page_id) const noexcept = 0;

    // Called by page rebuild/relayout after tuples on the page have
    // moved, invalidating every recorded location on it at once. This is
    // the whole invalidation mechanism: no page sweep, no double-write,
    // one counter.
    virtual void BumpFor(PageId page_id) noexcept = 0;
};

// Every page at epoch 0, forever. The engine's behaviour until a physical
// optimizer exists to bump anything (T19).
class StaticEpochProvider final : public EpochProvider {
public:
    std::uint32_t EpochOf(PageId) const noexcept override { return 0; }

    // Deliberately does nothing rather than asserting: a caller wiring up
    // relayout against this provider should get an unaccelerated engine,
    // not a crash. It is a stub, and the [OPEN] it stands in for is the
    // thing that has to land before it is replaced.
    void BumpFor(PageId) noexcept override {}
};

// ---- Probe result (spec section 3.1) -------------------------------------
//
// What a pk probe hands back, and what the caller still owes. `trusted`
// folds together every reason the location may not be usable - no entry,
// kEntryLive clear, page_id kInvalidPageId, epoch mismatch - because a
// caller must treat all of them identically, and giving each its own
// return would invite handling three and forgetting the fourth.
//
// **`trusted == true` is not permission to skip validation.** The caller
// must still read the tuple at (page_id, slot), compare its Keystone id
// against the pk it asked for, and fall through to the authoritative path
// on a mismatch. An entry can name a page and slot that now hold a
// different tuple entirely - that is a stale entry, not corruption, and
// the id check is the only thing standing between it and a wrong answer.
struct ProbeResult {
    bool trusted = false;      // location may be tried, subject to the id check above
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    std::uint32_t use_count = 0;  // best-effort heat; 0 when untracked
};

}  // namespace kds::stats
