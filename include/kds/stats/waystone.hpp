#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/stats/instance_key.hpp"
#include "kds/storage/page_header.hpp"

// The waystone page: the recorded trail of one pattern instance
// (docs/spec/waystone-concpets.md §§1, 6).
//
// A *pattern* is the shape of a statement, reduced at parse time to
// `pattern_id`; a *pattern instance* is that shape with its arguments
// bound, `(pattern_id, arg_hash)`. Executing an instance touches some set
// of tuples, possibly across several relations. This page records their
// Keystones and where each one was last seen, in execution order.
//
// This header is format and contract only: the layouts, the arithmetic
// that bounds them, and the identity check a reader owes. The directory
// that finds a page, the executor seam that fills one, and the replay that
// reads one all live above it.
//
// ---- A trail is not an answer -------------------------------------------
//
// What is stored here says where a previous execution *found* rows. It
// never asserts that the set is complete, and nothing may read it as
// though it did. Invariant 9: Waystone is never authoritative.
//
// The consequence for every consumer, stated once: **a waystone may
// replace a lookup, never a search.** A step whose authoritative work is a
// keyed lookup - a pk equality, or a nested-loop join probing the next
// relation by pk - may be served from the trail, because completeness for
// that step follows from pk uniqueness rather than from the trail. A step
// that must search still searches, whatever the trail says.
//
// The failure modes are not symmetric, which is why the rule is a rule. A
// stale entry pointing at the wrong place is caught by the identity check
// in the replay contract. A trail missing a row inserted since it was
// recorded is wrong in a way no per-entry validation can detect, because
// there is no entry to validate: absence has no witness.
//
// ---- Headered, unlike the structure this replaces -----------------------
//
// These are ordinary `PageType::kWaystone` pages carrying the common page
// header. Nothing here is addressed by shift and mask - a trail is read
// sequentially, front to back - so the payload does not have to tile the
// page exactly, and in exchange the pages keep their CRC32C and their
// page_lsn. The 24 bytes of tail slack (below) are the whole price.
//
// Concurrency: pure functions over a caller-owned page buffer. The caller
// holds the pin and latch (rules.md §3).

namespace kds::stats {

// ---- Page header (spec §6) -----------------------------------------------
//
// Sits at the start of the page body, immediately after the common page
// header. Field order is by descending alignment, so the on-disk offsets
// and the struct's own offsets coincide and every field carries an
// offsetof static_assert.

struct WaystoneHeader {
    // Which pattern instance this page belongs to. **Self-identifying on
    // purpose**: the directory that leads here is keyed by a hash of
    // `arg_hash`, so a collision can hand a reader the wrong page. These
    // two fields are what turn that into a miss instead of a wrong trail
    // (see WaystonePageHolds below).
    std::uint64_t pattern_id;
    std::uint64_t arg_hash;

    // Truncated logical timestamp of the execution that recorded this
    // trail. Best-effort, for retention.
    std::uint64_t recorded_ts;

    // A trail longer than one page continues here; kInvalidPageId ends it.
    PageId next_page_id;

    // Replays served from this trail. Best-effort - events may be dropped
    // under pressure - and it exists to rank instances for eviction, not
    // to count anything anyone reports.
    std::uint32_t use_count;

    // Live entries on this page, in [0, kEntriesPerWaystonePage].
    std::uint16_t entry_count;

    std::uint16_t flags;    // reserved; no bit is assigned yet
    std::uint32_t reserved; // 0
};

inline constexpr std::size_t kWaystoneHeaderPatternIdOffset = 0;
inline constexpr std::size_t kWaystoneHeaderArgHashOffset = 8;
inline constexpr std::size_t kWaystoneHeaderRecordedTsOffset = 16;
inline constexpr std::size_t kWaystoneHeaderNextPageOffset = 24;
inline constexpr std::size_t kWaystoneHeaderUseCountOffset = 28;
inline constexpr std::size_t kWaystoneHeaderEntryCountOffset = 32;
inline constexpr std::size_t kWaystoneHeaderFlagsOffset = 34;
inline constexpr std::size_t kWaystoneHeaderReservedOffset = 36;
// 8+8+8+4+4+2+2+4 = 40, every field naturally aligned, no tail padding.
inline constexpr std::size_t kWaystoneHeaderSize = 40;

static_assert(offsetof(WaystoneHeader, pattern_id) == kWaystoneHeaderPatternIdOffset);
static_assert(offsetof(WaystoneHeader, arg_hash) == kWaystoneHeaderArgHashOffset);
static_assert(offsetof(WaystoneHeader, recorded_ts) == kWaystoneHeaderRecordedTsOffset);
static_assert(offsetof(WaystoneHeader, next_page_id) == kWaystoneHeaderNextPageOffset);
static_assert(offsetof(WaystoneHeader, use_count) == kWaystoneHeaderUseCountOffset);
static_assert(offsetof(WaystoneHeader, entry_count) == kWaystoneHeaderEntryCountOffset);
static_assert(offsetof(WaystoneHeader, flags) == kWaystoneHeaderFlagsOffset);
static_assert(offsetof(WaystoneHeader, reserved) == kWaystoneHeaderReservedOffset);
static_assert(sizeof(WaystoneHeader) == kWaystoneHeaderSize);

// ---- Entry (spec §6) -----------------------------------------------------
//
// One Keystone the recorded execution touched, and where it was found.

struct WaystoneEntry {
    // The tuple's Keystone id, zero-extended (invariant 7: upper 24 bits
    // are 0). Never 5-byte-packed.
    std::uint64_t pk;

    // **The field that makes a cross-relation trail possible.** One page
    // holds the Keystones of every relation the execution touched, so an
    // entry has to say which one it came from; the per-relation structure
    // this replaces had no need for it and no room for it.
    //
    // A full 64-bit catalog::Oid, not a truncated 32-bit one. Oids are
    // uint64 at the source, and a narrowed copy that happens to fit today
    // is an aliasing bug waiting for the day it does not. Typed as
    // uint64_t rather than catalog::Oid so this layer does not depend on
    // the catalog for a typedef.
    std::uint64_t rel_oid;

    // Where the tuple was last seen. Advisory - always validated, never
    // trusted (see the replay contract in the file header).
    PageId page_id;

    // The heap page's epoch when the location was recorded. A location is
    // trustworthy only while this still matches the page's current epoch;
    // relayout bumps one counter instead of rewriting every entry that
    // pointed into the page.
    std::uint32_t page_epoch;

    std::uint16_t slot;

    std::uint16_t flags;  // kWaystoneEntryValid | reserved

    // Which step of the pattern produced this entry - the join position,
    // numbered from 0 in written order ("the query is the plan",
    // docs/parser.md I12).
    //
    // Without it a cross-table trail is an unordered bag of tuples from
    // three relations, which is describable but not replayable. 16 bits:
    // this counts relations in a join chain, not rows.
    std::uint16_t step_id;

    std::uint16_t reserved;  // 0
};

inline constexpr std::size_t kWaystoneEntryPkOffset = 0;
inline constexpr std::size_t kWaystoneEntryRelOidOffset = 8;
inline constexpr std::size_t kWaystoneEntryPageIdOffset = 16;
inline constexpr std::size_t kWaystoneEntryPageEpochOffset = 20;
inline constexpr std::size_t kWaystoneEntrySlotOffset = 24;
inline constexpr std::size_t kWaystoneEntryFlagsOffset = 26;
inline constexpr std::size_t kWaystoneEntryStepIdOffset = 28;
inline constexpr std::size_t kWaystoneEntryReservedOffset = 30;
// 8+8+4+4+2+2+2+2 = 32, every field naturally aligned, no tail padding.
inline constexpr std::size_t kWaystoneEntrySize = 32;

static_assert(offsetof(WaystoneEntry, pk) == kWaystoneEntryPkOffset);
static_assert(offsetof(WaystoneEntry, rel_oid) == kWaystoneEntryRelOidOffset);
static_assert(offsetof(WaystoneEntry, page_id) == kWaystoneEntryPageIdOffset);
static_assert(offsetof(WaystoneEntry, page_epoch) == kWaystoneEntryPageEpochOffset);
static_assert(offsetof(WaystoneEntry, slot) == kWaystoneEntrySlotOffset);
static_assert(offsetof(WaystoneEntry, flags) == kWaystoneEntryFlagsOffset);
static_assert(offsetof(WaystoneEntry, step_id) == kWaystoneEntryStepIdOffset);
static_assert(offsetof(WaystoneEntry, reserved) == kWaystoneEntryReservedOffset);
static_assert(sizeof(WaystoneEntry) == kWaystoneEntrySize);

// Set when the entry describes a tuple. A never-written entry reads back
// all-zero, which decodes to pk 0 and page_id 0 - both plausible-looking
// values - so a reader tests this flag, never `pk != 0`.
inline constexpr std::uint16_t kWaystoneEntryValid = 1u << 0;

// ---- How many fit --------------------------------------------------------
//
// Derived, not chosen: the page body is what the common header leaves
// (8192 - 32 = 8160), the waystone header takes 40 of it, and each entry
// is 32.
//
//     (8160 - 40) / 32 = 8120 / 32 = 253 entries, 24 bytes of tail slack
//
// **Not a power of two, and that is now allowed.** The structure this
// replaces addressed entries as `pk >> 8` / `pk & 0xFF`, which forced an
// exact power-of-two tiling and left no room for a page header - which in
// turn cost those pages their checksum. Nothing here is addressed by
// arithmetic: a trail is walked front to back, so 253 is as good as 256
// and the page keeps its damage detection.
inline constexpr std::size_t kWaystoneBodyOffset =
    storage::kPageBodyOffset + kWaystoneHeaderSize;
inline constexpr std::size_t kEntriesPerWaystonePage =
    (storage::kPageBodySize - kWaystoneHeaderSize) / kWaystoneEntrySize;

static_assert(kEntriesPerWaystonePage == 253);
static_assert(kWaystoneBodyOffset + kEntriesPerWaystonePage * kWaystoneEntrySize <= kPageSize);

// ---- Codec ---------------------------------------------------------------
//
// Field-wise memcpy through the named offsets above (rules.md §§2, 5):
// never a reinterpret_cast onto page bytes, never a compiler bitfield.

// Formats a brand-new waystone page for `key`: zeroes the page, writes the
// common header for PageType::kWaystone, and writes a waystone header with
// no entries and no continuation.
//
// The instance arrives as one value rather than two `uint64_t`s for the
// reason instance_key.hpp gives: swapping them used to compile, and the
// resulting bug is a silent permanent trail miss.
void FormatWaystonePage(std::span<std::byte, kPageSize> page, const InstanceKey& key,
                        std::uint64_t recorded_ts);

// Decodes the waystone header verbatim, with no validation - a pure
// decode, like storage::ReadPageHeader. A caller inspecting a page that
// may not be a waystone at all uses WaystonePageHolds() first.
WaystoneHeader ReadWaystoneHeader(std::span<const std::byte, kPageSize> page);

// Fails with InvalidArgument for an `entry_count` past
// kEntriesPerWaystonePage, which is a caller bug rather than damage: a
// count the page cannot hold would send every reader off the end of the
// body.
Status WriteWaystoneHeader(std::span<std::byte, kPageSize> page, const WaystoneHeader& header);

// Reads/writes the entry at `index`. Both fail with OutOfRange at or past
// kEntriesPerWaystonePage.
//
// ReadWaystoneEntry does no validation beyond the bound - a garbage entry
// is indistinguishable from a plausible stale one, which is exactly why
// the replay contract makes the caller check the Keystone id and rel_oid
// of the tuple actually found rather than trust anything returned here.
StatusOr<WaystoneEntry> ReadWaystoneEntry(std::span<const std::byte, kPageSize> page,
                                          std::size_t index);
Status WriteWaystoneEntry(std::span<std::byte, kPageSize> page, std::size_t index,
                          const WaystoneEntry& entry);

// Whether `page` is a formatted waystone page recording exactly `key`.
//
// **Every reason to answer false is folded into one bool**, deliberately:
// an unformatted page, a page of another type, a format version this build
// cannot parse, and a page belonging to a different instance. A caller
// must do the same thing in all four - treat it as a miss and fall through
// to the authoritative path - and four distinct returns would invite
// handling three of them.
//
// The last of the four is the load-bearing one. The directory is keyed by
// a hash, so a collision leads a reader to a real, valid, wrong trail;
// this check is what makes that a miss rather than someone else's rows.
bool WaystonePageHolds(std::span<const std::byte, kPageSize> page,
                       const InstanceKey& key) noexcept;

}  // namespace kds::stats
