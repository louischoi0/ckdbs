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

// The three Waystone fields (docs/waystone-concpets.md §7, landed
// 2026-07-30 as W04) are the relation's whole Waystone identity: whether
// it has one, where its directory starts, and how deep that directory is.
//
// §4.1's precondition - a Waystone-enabled relation must use
// system-generated autoincrement pks, because entry_index = pk and a
// caller-supplied or reused pk would alias an existing entry - needs no
// enforcement here: CLAUDE.md invariant 10 already requires that of
// *every* relation (widened 2026-07-29), and Catalog::AllocateRowId() is
// the only way an id is issued.
struct SysTableRow {
    Oid oid;
    Oid namespace_oid;
    Name name;
    PageId desc_page_id;
    ClusteredType clustered_type;
    // Next Keystone id this relation will issue. The relation's pk is
    // system-generated and autoincrement (heap-and-tuple.md section 4,
    // CLAUDE.md invariant 10), and the sequence has to be *persistent*
    // rather than derived: deriving it as max(id)+1 would reissue an id
    // after the highest tuple is deleted, and Waystone addresses tuples by
    // id directly, so a reissued id silently aliases a retired one. First
    // id issued is 1, keeping 0 free as "unset".
    std::uint64_t next_id;

    // ---- Waystone (docs/waystone-concpets.md §7) -------------------------
    //
    // Defaults are disabled / kInvalidPageId / 0, so a relation created
    // without asking for Waystone costs exactly what it did before.
    WaystoneState waystone_state;

    // Root of the relation's page directory (spec §6), kInvalidPageId when
    // disabled. Changes only when the directory deepens, which relinks a
    // new root over the old one.
    PageId waystone_dir_root;

    // Levels the directory walk traverses, 0 when disabled and otherwise
    // in 1..kMaxDirDepth. **Persisted rather than derived**: deriving it
    // from next_id would change the moment the sequence crossed a coverage
    // boundary, which is *before* GrowDirectory() relinks the root - and
    // every lookup in that window would walk the wrong number of levels
    // and land on the wrong leaf. The stored depth and the actual root are
    // one fact and are written together.
    std::uint8_t waystone_dir_depth;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kNamespaceOidOffset = 8;
    static constexpr std::size_t kNameOffset = 16;
    static constexpr std::size_t kDescPageIdOffset = kNameOffset + kCatalogNameMax;
    static constexpr std::size_t kClusteredTypeOffset = kDescPageIdOffset + sizeof(PageId);
    // uint64 rather than 8-byte-aligned: catalog rows are packed byte
    // streams read through memcpy, never overlaid on the buffer.
    static constexpr std::size_t kNextIdOffset = kClusteredTypeOffset + sizeof(std::uint8_t);
    static constexpr std::size_t kWaystoneStateOffset = kNextIdOffset + sizeof(std::uint64_t);
    static constexpr std::size_t kWaystoneDirRootOffset =
        kWaystoneStateOffset + sizeof(std::uint8_t);
    static constexpr std::size_t kWaystoneDirDepthOffset =
        kWaystoneDirRootOffset + sizeof(PageId);
    static constexpr std::size_t kOnDiskSize = kWaystoneDirDepthOffset + sizeof(std::uint8_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysTableRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysTableRow, oid) == SysTableRow::kOidOffset);
static_assert(offsetof(SysTableRow, namespace_oid) == SysTableRow::kNamespaceOidOffset);
static_assert(offsetof(SysTableRow, name) == SysTableRow::kNameOffset);
static_assert(offsetof(SysTableRow, desc_page_id) == SysTableRow::kDescPageIdOffset);
static_assert(offsetof(SysTableRow, clustered_type) == SysTableRow::kClusteredTypeOffset);

// The first id any relation issues. Zero stays reserved for "no id".
inline constexpr std::uint64_t kFirstRowId = 1;

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
