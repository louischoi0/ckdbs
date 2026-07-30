#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// The common 32-byte page header that sits at offset 0 of every *headered*
// page - heap, B+ tree, undo, catalog, superblock, free map (docs/page.md
// sections 1 and 2, CONFIRMED layout). Type-specific content begins at
// kPageBodyOffset.
//
// This module is the single owner of that layout: type-specific codecs
// compose it and must not re-implement it (page.md section 2). It also
// owns the checksum discipline, because the checksum's scope - "the whole
// 8 KiB with the checksum field zeroed" - is a property of this layout,
// not of any page type.
//
// Waystone entry and directory pages are headerless by class and never
// pass through here (page.md section 1). That is a property of their
// layout - exact power-of-two tilings that a header would break - and not
// of invariant 8, which as of 2026-07-30 permits read-path probes
// (docs/waystone-concpets.md section 3.1). Headerless also means no
// page_lsn, which is why their persistence class is a live [OPEN].
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
    std::uint64_t reserved0;      // 0; leading candidate is the heap relayout epoch
    std::uint64_t reserved1;      // 0
};

inline constexpr std::size_t kPageTypeOffset = 0;
inline constexpr std::size_t kFormatVersionOffset = 1;
inline constexpr std::size_t kPageFlagsOffset = 2;
inline constexpr std::size_t kPageChecksumOffset = 4;
inline constexpr std::size_t kPageLsnOffset = 8;
inline constexpr std::size_t kPageReserved0Offset = 16;
inline constexpr std::size_t kPageReserved1Offset = 24;

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
static_assert(offsetof(PageHeaderFields, reserved0) == kPageReserved0Offset);
static_assert(offsetof(PageHeaderFields, reserved1) == kPageReserved1Offset);
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
// page_lsn 0, both reserved words 0, and checksum 0. The body is left
// zeroed for the type-specific codec to initialize.
void FormatPage(std::span<std::byte, kPageSize> page, PageType type, std::uint16_t flags = 0);

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
