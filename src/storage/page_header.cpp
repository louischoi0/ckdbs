#include "kds/storage/page_header.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "kds/storage/crc32c.hpp"
#include "kds/storage/page_bytes.hpp"

namespace kds::storage {

std::uint8_t MaxSupportedFormatVersion(PageType type) noexcept {
    switch (type) {
        case PageType::kHeap:
        case PageType::kBtreeInternal:
        case PageType::kBtreeLeaf:
        case PageType::kUndo:
        case PageType::kCatalog:
        case PageType::kSuperBlock:
        case PageType::kFreeMap:
        case PageType::kHeaderlessMap:
        case PageType::kWaystone:
        case PageType::kVarHeap:
        case PageType::kIndexInternal:
        case PageType::kIndexLeaf:
        // Missing until 2026-08-24, and not cosmetically: falling through to
        // the 0 below made `FormatPage` stamp format_version 0 on every
        // Bound Cabin page, which `ValidatePageHeader` then reads as
        // Corruption ("version == 0 || version > supported", with supported
        // also 0). Nothing on the live path asks - `BoundCabinPage::Open`
        // checks the type byte itself - so no page was ever rejected, but
        // the first caller that did validate one would have called every
        // Bound Cabin page in the database corrupt.
        case PageType::kCabinBound:
        case PageType::kAnchor:
            return 1;
        case PageType::kInvalid:
            break;
    }
    // Unformatted pages have no layout to version.
    return 0;
}

PageHeaderFields ReadPageHeader(std::span<const std::byte, kPageSize> page) {
    PageHeaderFields fields;
    fields.page_type = LoadField<std::uint8_t>(page, kPageTypeOffset);
    fields.format_version = LoadField<std::uint8_t>(page, kFormatVersionOffset);
    fields.flags = LoadField<std::uint16_t>(page, kPageFlagsOffset);
    fields.checksum = LoadField<std::uint32_t>(page, kPageChecksumOffset);
    fields.page_lsn = LoadField<std::uint64_t>(page, kPageLsnOffset);
    fields.relayout_epoch = LoadField<std::uint64_t>(page, kPageRelayoutEpochOffset);
    fields.owner_oid = LoadField<std::uint64_t>(page, kPageOwnerOidOffset);
    return fields;
}

void WritePageHeader(std::span<std::byte, kPageSize> page, const PageHeaderFields& fields) {
    StoreField<std::uint8_t>(page, kPageTypeOffset, fields.page_type);
    StoreField<std::uint8_t>(page, kFormatVersionOffset, fields.format_version);
    StoreField<std::uint16_t>(page, kPageFlagsOffset, fields.flags);
    StoreField<std::uint32_t>(page, kPageChecksumOffset, fields.checksum);
    StoreField<std::uint64_t>(page, kPageLsnOffset, fields.page_lsn);
    StoreField<std::uint64_t>(page, kPageRelayoutEpochOffset, fields.relayout_epoch);
    StoreField<std::uint64_t>(page, kPageOwnerOidOffset, fields.owner_oid);
}

void FormatPage(std::span<std::byte, kPageSize> page, PageType type, std::uint16_t flags,
                std::uint64_t owner_oid) {
    std::memset(page.data(), 0, page.size());

    PageHeaderFields fields{};
    fields.page_type = static_cast<std::uint8_t>(type);
    fields.format_version = MaxSupportedFormatVersion(type);
    fields.flags = flags;
    fields.checksum = 0;  // stamped at flush, never here
    fields.page_lsn = kNoPageLsn;
    fields.relayout_epoch = 0;
    fields.owner_oid = owner_oid;
    WritePageHeader(page, fields);
}

Status ValidatePageHeader(std::span<const std::byte, kPageSize> page) {
    const std::uint8_t raw_type = LoadField<std::uint8_t>(page, kPageTypeOffset);
    if (raw_type == static_cast<std::uint8_t>(PageType::kInvalid)) {
        return Status::Corruption("page header: unformatted page (page_type 0)");
    }
    if (raw_type > kMaxAssignedPageType) {
        return Status::Corruption("page header: unknown page_type " + std::to_string(raw_type));
    }

    const auto type = static_cast<PageType>(raw_type);
    const std::uint8_t version = LoadField<std::uint8_t>(page, kFormatVersionOffset);
    const std::uint8_t supported = MaxSupportedFormatVersion(type);
    if (version == 0 || version > supported) {
        return Status::Corruption("page header: format_version " + std::to_string(version) +
                                  " for page_type " + std::to_string(raw_type) +
                                  " is not supported by this build (max " +
                                  std::to_string(supported) + ")");
    }
    return Status::OK();
}

Status ValidatePageHeader(std::span<const std::byte, kPageSize> page, PageType expected) {
    Status status = ValidatePageHeader(page);
    if (!status.ok()) {
        return status;
    }
    const std::uint8_t raw_type = LoadField<std::uint8_t>(page, kPageTypeOffset);
    if (raw_type != static_cast<std::uint8_t>(expected)) {
        return Status::Corruption(
            "page header: expected page_type " +
            std::to_string(static_cast<std::uint8_t>(expected)) + ", found " +
            std::to_string(raw_type));
    }
    return Status::OK();
}

std::uint8_t RawPageType(std::span<const std::byte, kPageSize> page) {
    return LoadField<std::uint8_t>(page, kPageTypeOffset);
}

std::uint64_t GetPageLsn(std::span<const std::byte, kPageSize> page) {
    return LoadField<std::uint64_t>(page, kPageLsnOffset);
}

void SetPageLsn(std::span<std::byte, kPageSize> page, std::uint64_t lsn) {
    StoreField<std::uint64_t>(page, kPageLsnOffset, lsn);
}

std::uint32_t GetStoredChecksum(std::span<const std::byte, kPageSize> page) {
    return LoadField<std::uint32_t>(page, kPageChecksumOffset);
}

std::uint64_t GetOwnerOid(std::span<const std::byte, kPageSize> page) {
    return LoadField<std::uint64_t>(page, kPageOwnerOidOffset);
}

std::uint16_t GetPageStreamStamp(std::span<const std::byte, kPageSize> page) {
    return LoadField<std::uint16_t>(page, kPageFlagsOffset);
}

void SetPageStreamStamp(std::span<std::byte, kPageSize> page, std::uint16_t stamp) {
    StoreField<std::uint16_t>(page, kPageFlagsOffset, stamp);
}

std::uint64_t GetRelayoutEpoch(std::span<const std::byte, kPageSize> page) {
    return LoadField<std::uint64_t>(page, kPageRelayoutEpochOffset);
}

void SetRelayoutEpoch(std::span<std::byte, kPageSize> page, std::uint64_t epoch) {
    StoreField<std::uint64_t>(page, kPageRelayoutEpochOffset, epoch);
}

void BumpRelayoutEpoch(std::span<std::byte, kPageSize> page) {
    SetRelayoutEpoch(page, GetRelayoutEpoch(page) + 1);
}

std::uint32_t ComputePageChecksum(std::span<const std::byte, kPageSize> page) {
    // The stored checksum cannot cover itself, so it is treated as zero.
    // Done as three CRC segments rather than by copying the page or
    // temporarily zeroing the field: the verify path must not mutate a
    // page it may not even own exclusively.
    static constexpr std::array<std::byte, sizeof(std::uint32_t)> kZeroField{};

    std::uint32_t crc = Crc32cExtend(kCrc32cInit, page.first(kPageChecksumOffset));
    crc = Crc32cExtend(crc, kZeroField);
    crc = Crc32cExtend(crc, page.subspan(kPageChecksumOffset + sizeof(std::uint32_t)));
    return crc;
}

void StampPageChecksum(std::span<std::byte, kPageSize> page) {
    StoreField<std::uint32_t>(page, kPageChecksumOffset, ComputePageChecksum(page));
}

Status VerifyPageChecksum(std::span<const std::byte, kPageSize> page) {
    const std::uint32_t stored = GetStoredChecksum(page);
    const std::uint32_t computed = ComputePageChecksum(page);
    if (stored != computed) {
        return Status::Corruption("page checksum mismatch: stored " + std::to_string(stored) +
                                  ", computed " + std::to_string(computed));
    }
    return Status::OK();
}

}  // namespace kds::storage
