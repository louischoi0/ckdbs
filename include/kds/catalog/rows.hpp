#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/catalog/well_known.hpp"

// Fixed-layout catalog rows, ported from the legacy kernel engine's
// kds_sys_object_t/kds_sys_table_t/kds_sys_column_t/kds_sys_type_t/
// kds_sys_index_t (kds_catalog.h). Each is inserted/read as a single heap
// tuple payload (kds::heap::PageView::InsertTuple()/ReadTuple()) - no
// further serialization layer needed, same as the legacy design.
//
// Same field-wise-memcpy, no-reinterpret_cast, no-bitfields discipline as
// heap_page.hpp/keystone.hpp/superblock.hpp: each row has named offset
// constants pinned by offsetof static_asserts, and Encode()/Decode() copy
// one field at a time through those offsets rather than casting the
// buffer to the struct type. Unlike Keystone::Decode() (never fails, any
// bit pattern is valid), Row::Decode() here validates the input span is
// exactly kOnDiskSize bytes - the payload comes back from a heap tuple
// whose data_len is caller-supplied at read time, so a length mismatch is
// a real corruption signal worth catching rather than reading garbage.

namespace kds::catalog {

// ---- sys.objects ---------------------------------------------------------

struct SysObjectRow {
    Oid oid;
    Oid namespace_oid;
    Oid type_oid;
    Oid rel_id;
    Name name;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kNamespaceOidOffset = 8;
    static constexpr std::size_t kTypeOidOffset = 16;
    static constexpr std::size_t kRelIdOffset = 24;
    static constexpr std::size_t kNameOffset = 32;
    static constexpr std::size_t kOnDiskSize = kNameOffset + kCatalogNameMax;

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysObjectRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysObjectRow, oid) == SysObjectRow::kOidOffset);
static_assert(offsetof(SysObjectRow, namespace_oid) == SysObjectRow::kNamespaceOidOffset);
static_assert(offsetof(SysObjectRow, type_oid) == SysObjectRow::kTypeOidOffset);
static_assert(offsetof(SysObjectRow, rel_id) == SysObjectRow::kRelIdOffset);
static_assert(offsetof(SysObjectRow, name) == SysObjectRow::kNameOffset);

// ---- sys.tables -----------------------------------------------------------

// TODO(waystone, docs/waystone-concpets.md §7, T04/T12 in
// docs/waystone-workplan.md): this row will gain `waystone_enabled` (+
// coverage-complete state) and `waystone_dir_root: PageId`, defaulting to
// disabled / kInvalidPageId. Rule to enforce once that flag exists
// (docs/waystone-concpets.md §4.1, confirmed 2026-07-28): a table with
// waystone_enabled set must use system-generated, autoincrement pks -
// callers must not supply their own id/pk on insert into such a table.
// Waystone addresses entries directly by pk (entry_index = pk), so a
// caller-supplied, non-monotonic, or reused pk would break the
// directory's dense-growth assumption and can collide with an existing
// live entry. Enforce at the DDL/insert boundary (wherever pk assignment
// is owned), not inside Waystone itself - Waystone has no way to observe
// where a pk value came from.
struct SysTableRow {
    Oid oid;
    Oid namespace_oid;
    Name name;
    PageId desc_page_id;
    ClusteredType clustered_type;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kNamespaceOidOffset = 8;
    static constexpr std::size_t kNameOffset = 16;
    static constexpr std::size_t kDescPageIdOffset = kNameOffset + kCatalogNameMax;
    static constexpr std::size_t kClusteredTypeOffset = kDescPageIdOffset + sizeof(PageId);
    static constexpr std::size_t kOnDiskSize = kClusteredTypeOffset + sizeof(std::uint8_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysTableRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysTableRow, oid) == SysTableRow::kOidOffset);
static_assert(offsetof(SysTableRow, namespace_oid) == SysTableRow::kNamespaceOidOffset);
static_assert(offsetof(SysTableRow, name) == SysTableRow::kNameOffset);
static_assert(offsetof(SysTableRow, desc_page_id) == SysTableRow::kDescPageIdOffset);
static_assert(offsetof(SysTableRow, clustered_type) == SysTableRow::kClusteredTypeOffset);

// ---- sys.columns ----------------------------------------------------------

struct SysColumnRow {
    Oid oid;
    Oid rel_id;
    std::uint32_t pos;
    Name name;
    std::uint32_t type_val;
    std::uint32_t len;
    bool notnull;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kRelIdOffset = 8;
    static constexpr std::size_t kPosOffset = 16;
    static constexpr std::size_t kNameOffset = 20;
    static constexpr std::size_t kTypeValOffset = kNameOffset + kCatalogNameMax;
    static constexpr std::size_t kLenOffset = kTypeValOffset + sizeof(std::uint32_t);
    static constexpr std::size_t kNotNullOffset = kLenOffset + sizeof(std::uint32_t);
    static constexpr std::size_t kOnDiskSize = kNotNullOffset + sizeof(std::uint8_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysColumnRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysColumnRow, oid) == SysColumnRow::kOidOffset);
static_assert(offsetof(SysColumnRow, rel_id) == SysColumnRow::kRelIdOffset);
static_assert(offsetof(SysColumnRow, pos) == SysColumnRow::kPosOffset);
static_assert(offsetof(SysColumnRow, name) == SysColumnRow::kNameOffset);
static_assert(offsetof(SysColumnRow, type_val) == SysColumnRow::kTypeValOffset);
static_assert(offsetof(SysColumnRow, len) == SysColumnRow::kLenOffset);
static_assert(offsetof(SysColumnRow, notnull) == SysColumnRow::kNotNullOffset);

// ---- sys.types --------------------------------------------------------

struct SysTypeRow {
    Oid oid;
    Name name;
    std::uint32_t type_val;
    std::uint32_t len;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kNameOffset = 8;
    static constexpr std::size_t kTypeValOffset = kNameOffset + kCatalogNameMax;
    static constexpr std::size_t kLenOffset = kTypeValOffset + sizeof(std::uint32_t);
    static constexpr std::size_t kOnDiskSize = kLenOffset + sizeof(std::uint32_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysTypeRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysTypeRow, oid) == SysTypeRow::kOidOffset);
static_assert(offsetof(SysTypeRow, name) == SysTypeRow::kNameOffset);
static_assert(offsetof(SysTypeRow, type_val) == SysTypeRow::kTypeValOffset);
static_assert(offsetof(SysTypeRow, len) == SysTypeRow::kLenOffset);

// ---- sys.indexes -----------------------------------------------------

inline constexpr std::uint8_t kIndexFlagUnique = 0x1;  // the only mode supported today

struct SysIndexRow {
    Oid index_oid;
    Oid table_oid;
    std::uint32_t col_pos;
    std::uint32_t col_type;
    std::uint8_t flags;

    static constexpr std::size_t kIndexOidOffset = 0;
    static constexpr std::size_t kTableOidOffset = 8;
    static constexpr std::size_t kColPosOffset = 16;
    static constexpr std::size_t kColTypeOffset = 20;
    static constexpr std::size_t kFlagsOffset = 24;
    static constexpr std::size_t kOnDiskSize = kFlagsOffset + sizeof(std::uint8_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysIndexRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysIndexRow, index_oid) == SysIndexRow::kIndexOidOffset);
static_assert(offsetof(SysIndexRow, table_oid) == SysIndexRow::kTableOidOffset);
static_assert(offsetof(SysIndexRow, col_pos) == SysIndexRow::kColPosOffset);
static_assert(offsetof(SysIndexRow, col_type) == SysIndexRow::kColTypeOffset);
static_assert(offsetof(SysIndexRow, flags) == SysIndexRow::kFlagsOffset);

}  // namespace kds::catalog
