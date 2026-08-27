#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// The common 32-byte page header that sits at offset 0 of every *headered*
// page - heap, B+ tree, undo, catalog, superblock, free map (docs/spec/page.md
// sections 1 and 2, CONFIRMED layout). Type-specific content begins at
// kPageBodyOffset.
//
// This module is the single owner of that layout: type-specific codecs
// compose it and must not re-implement it (page.md section 2). It also
// owns the checksum discipline, because the checksum's scope - "the whole
// 8 KiB with the checksum field zeroed" - is a property of this layout,
// not of any page type.
//
// A headerless page class never passes through here (page.md section 1) -
// a property of its layout, an exact power-of-two tiling a header would
// break. Headerless also means no page_lsn, so such a class cannot be
// WAL-logged as it stands. No such class exists today.
//
// Encoding rules per rules.md sections 2 and 5: a mirror struct with
// offsetof static_asserts, field-wise memcpy through named offsets, fixed
// width little-endian, no bitfields, no reinterpret_cast onto page bytes.
//
// Concurrency: pure functions over a caller-owned page buffer. The caller
// holds whatever pin/latch the page needs (shared for the reads here,
// exclusive for the writes); this file takes none.

namespace kds::storage {

// ---- On-disk field layout ----------------------------------------------

struct PageHeaderFields {
    std::uint8_t page_type;       // kds::PageType, frozen append-only enum
    std::uint8_t format_version;  // per-type layout version; bumps are format events
    std::uint16_t flags;          // per-type meaning; 0 unless the type specifies otherwise
    std::uint32_t checksum;       // CRC32C over the full page with this field zeroed
    std::uint64_t page_lsn;       // LSN of the last WAL record applied; 0 = never logged
    std::uint64_t relayout_epoch; // bumped when tuples on the page move; 0 = never relayouted
    std::uint64_t owner_oid;      // catalog oid of the owning object; 0 = unattributed (page.md §2a)
};

inline constexpr std::size_t kPageTypeOffset = 0;
inline constexpr std::size_t kFormatVersionOffset = 1;
inline constexpr std::size_t kPageFlagsOffset = 2;
inline constexpr std::size_t kPageChecksumOffset = 4;
inline constexpr std::size_t kPageLsnOffset = 8;
// Offset 16 is the word that was `reserved0`, whose comment nominated
// exactly this use — which is why the field's arrival is not a format
// event: every page ever written already carries 0 here, and 0 reads as
// "never relayouted" (docs/spec/physical-optimizer.md R4).
inline constexpr std::size_t kPageRelayoutEpochOffset = 16;
// Offset 24 was `reserved1`, consumed by `owner_oid` the same way offset 16
// was consumed above (page.md §2a): every page ever written already carries
// 0 here, and 0 reads as "unattributed" — so the arrival is not a format
// event and no format_version moves.
inline constexpr std::size_t kPageOwnerOidOffset = 24;

// 1+1+2+4+8+8+8 = 32, with no interior or tail padding at these offsets
// (every field is naturally aligned and the total is a multiple of 8), so
// sizeof() is safe to assert directly.
inline constexpr std::size_t kPageHeaderSize = 32;

// Where type-specific content starts, and how much of it fits.
inline constexpr std::size_t kPageBodyOffset = kPageHeaderSize;
inline constexpr std::size_t kPageBodySize = kPageSize - kPageHeaderSize;  // 8160

static_assert(offsetof(PageHeaderFields, page_type) == kPageTypeOffset);
static_assert(offsetof(PageHeaderFields, format_version) == kFormatVersionOffset);
static_assert(offsetof(PageHeaderFields, flags) == kPageFlagsOffset);
static_assert(offsetof(PageHeaderFields, checksum) == kPageChecksumOffset);
static_assert(offsetof(PageHeaderFields, page_lsn) == kPageLsnOffset);
static_assert(offsetof(PageHeaderFields, relayout_epoch) == kPageRelayoutEpochOffset);
static_assert(offsetof(PageHeaderFields, owner_oid) == kPageOwnerOidOffset);
static_assert(sizeof(PageHeaderFields) == kPageHeaderSize);

// A page that has never been written back reads as page_lsn 0 (wal.md
// section 9). Spelled out so the meaning of the zero page is explicit.
inline constexpr std::uint64_t kNoPageLsn = 0;

// ---- Format versions ----------------------------------------------------

// Highest format_version this build can parse for `type`. A page carrying
// a higher version was written by a newer build and is refused rather than
// misparsed (page.md section 18-1, "version-bump gate"). Bump the entry
// for a type in the same change that alters its on-disk layout.
std::uint8_t MaxSupportedFormatVersion(PageType type) noexcept;

// ---- Codec --------------------------------------------------------------

// Decodes the header bytes verbatim, with no validation - use
// ValidatePageHeader() for that. Never fails, so callers inspecting a
// possibly-garbage page (recovery, tooling) can look at the raw fields.
PageHeaderFields ReadPageHeader(std::span<const std::byte, kPageSize> page);

// Writes all header fields, leaving the rest of the page untouched. Does
// not recompute the checksum: `fields.checksum` is written as given, and
// the authoritative way to set it is StampPageChecksum() at flush time.
void WritePageHeader(std::span<std::byte, kPageSize> page, const PageHeaderFields& fields);

// Formats a brand-new headered page: zeroes all kPageSize bytes, then
// writes a header for `type` at this build's current format version, with
// page_lsn 0, relayout_epoch 0, checksum 0, and `owner_oid` as given. The
// body is left zeroed for the type-specific codec to initialize.
//
// `owner_oid` (page.md §2a): the catalog oid of the object whose structure
// this page is — the relation for heap, var-heap and clustered-tree pages,
// the index for secondary-index pages. 0 = unattributed, the value for
// system classes (undo, catalog, superblock, freemap, cabin) and the only
// value pre-§2a pages can carry. **This is the sole writer**: the field is
// immutable until the page is re-initialized (the min_key discipline), so
// there is deliberately no SetOwnerOid.
void FormatPage(std::span<std::byte, kPageSize> page, PageType type, std::uint16_t flags = 0,
                std::uint64_t owner_oid = 0);

// Rejects a page whose header this build must not interpret: an
// unformatted page (page_type 0, which is also what a sparse never-written
// page reads back as), a page_type beyond kMaxAssignedPageType, or a
// format_version newer than MaxSupportedFormatVersion(). Passing
// `expected` additionally requires the type to match. Does not check the
// checksum - that is VerifyPageChecksum(), which the load path runs first.
Status ValidatePageHeader(std::span<const std::byte, kPageSize> page);
Status ValidatePageHeader(std::span<const std::byte, kPageSize> page, PageType expected);

// Field accessors, for the common cases that do not want a whole struct.
std::uint8_t RawPageType(std::span<const std::byte, kPageSize> page);
std::uint64_t GetPageLsn(std::span<const std::byte, kPageSize> page);
void SetPageLsn(std::span<std::byte, kPageSize> page, std::uint64_t lsn);
std::uint32_t GetStoredChecksum(std::span<const std::byte, kPageSize> page);
std::uint64_t GetOwnerOid(std::span<const std::byte, kPageSize> page);

// ---- The PL-C stream stamp (page-lsn-cross-stream.md §9 rule 4) ----
//
// The 16-bit `flags` word at offset 2 carries `core_id + 1` of the WAL
// stream that last wrote the page; **0 means never stamped**, which is
// what every pre-PW1c-3 page reads - the owner_oid no-backfill precedent.
// It exists because `page_lsn` is a byte offset into one stream's file:
// after a PL-B handoff the number is meaningless to the other stream, and
// the stamp is what lets redo tell "mine" from "incomparable" (rule 5).
// Explicit load/store like every persisted field - invariant 6, no
// bitfields. Like SetPageLsn, mutation does not restamp the checksum.
std::uint16_t GetPageStreamStamp(std::span<const std::byte, kPageSize> page);
void SetPageStreamStamp(std::span<std::byte, kPageSize> page, std::uint16_t stamp);

// The one home of the `core_id + 1` convention (invariant 6's discipline:
// a persisted encoding is spelled once). 0 stays "never stamped".
constexpr std::uint16_t StreamStampFor(std::uint32_t core_id) noexcept {
    return static_cast<std::uint16_t>(core_id + 1);
}
// Foreign = stamped, and by some other stream. Never true of an unstamped
// page, whose page_lsn is meaningful as-is.
constexpr bool StampIsForeign(std::uint16_t stamp, std::uint32_t core_id) noexcept {
    return stamp != 0 && stamp != StreamStampFor(core_id);
}

// ---- Relayout epoch (docs/spec/physical-optimizer.md R4) ----------------
//
// Bumped only by the physical optimizer's mover when tuples on the page
// move; INSERT, UPDATE and DELETE never bump it, because the fixed-length
// rule makes them address-stable — that stability is what keeps every
// recorded location valid today. **Nothing calls Bump yet**: the mover
// does not exist (physical-optimizer.md §6's gates), and the API is
// here ahead of it — the eviction sweep's precedent — so the consumers'
// comparisons (Waystone replay rule 2, Cabin entry verification, workplan
// PX04) are real code now rather than promises.
//
// The pairing rule, stated where a consumer will read it: **no consumer
// may accept a location on epoch equality alone.** The epoch is a fast
// whole-page invalidation layered over the Keystone-id check (K1), never
// a substitute for it — two equal epochs prove nothing about one slot.
//
// Like SetPageLsn, mutation does not restamp the checksum: the field is
// inside the checksummed span, and StampPageChecksum() at flush time is
// the one place that seals it.
std::uint64_t GetRelayoutEpoch(std::span<const std::byte, kPageSize> page);
void SetRelayoutEpoch(std::span<std::byte, kPageSize> page, std::uint64_t epoch);
void BumpRelayoutEpoch(std::span<std::byte, kPageSize> page);  // +1, under the caller's exclusive access

// ---- Checksums ----------------------------------------------------------

// CRC32C over the full page with the 4 checksum bytes treated as zero -
// computed without mutating or copying the page, by running the CRC over
// [0, kPageChecksumOffset), then four zero bytes, then the remainder.
std::uint32_t ComputePageChecksum(std::span<const std::byte, kPageSize> page);

// Computes and stores the checksum. This is the last thing that touches a
// page before it is written out (page.md section 8): every other mutation
// must already have happened.
void StampPageChecksum(std::span<std::byte, kPageSize> page);

// Recomputes and compares. Runs on the miss path only - never on a buffer
// hit (page.md section 10). Fails with Corruption on mismatch; during
// recovery such a page is healed from a WAL full-page image, so the
// checksum only has to detect.
Status VerifyPageChecksum(std::span<const std::byte, kPageSize> page);

}  // namespace kds::storage
