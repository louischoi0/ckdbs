#include "kds/catalog/rows.hpp"

#include <cstring>

namespace kds::catalog {

namespace {

Status CheckSize(std::span<const std::byte> bytes, std::size_t expected) {
    if (bytes.size() != expected) {
        return Status::Corruption("catalog row size mismatch");
    }
    return Status::OK();
}

}  // namespace

// ---- sys.objects -----------------------------------------------------

std::array<std::byte, SysObjectRow::kOnDiskSize> SysObjectRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kOidOffset, &oid, sizeof(oid));
    std::memcpy(base + kNamespaceOidOffset, &namespace_oid, sizeof(namespace_oid));
    std::memcpy(base + kTypeOidOffset, &type_oid, sizeof(type_oid));
    std::memcpy(base + kRelIdOffset, &rel_id, sizeof(rel_id));
    std::memcpy(base + kNameOffset, name.data(), kCatalogNameMax);
    return buf;
}

StatusOr<SysObjectRow> SysObjectRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysObjectRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.oid, base + kOidOffset, sizeof(row.oid));
    std::memcpy(&row.namespace_oid, base + kNamespaceOidOffset, sizeof(row.namespace_oid));
    std::memcpy(&row.type_oid, base + kTypeOidOffset, sizeof(row.type_oid));
    std::memcpy(&row.rel_id, base + kRelIdOffset, sizeof(row.rel_id));
    std::memcpy(row.name.data(), base + kNameOffset, kCatalogNameMax);
    return row;
}

// ---- sys.tables --------------------------------------------------------

std::array<std::byte, SysTableRow::kOnDiskSize> SysTableRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kOidOffset, &oid, sizeof(oid));
    std::memcpy(base + kNamespaceOidOffset, &namespace_oid, sizeof(namespace_oid));
    std::memcpy(base + kNameOffset, name.data(), kCatalogNameMax);
    std::memcpy(base + kDescPageIdOffset, &desc_page_id, sizeof(desc_page_id));
    auto ct = static_cast<std::uint8_t>(clustered_type);
    std::memcpy(base + kClusteredTypeOffset, &ct, sizeof(ct));
    return buf;
}

StatusOr<SysTableRow> SysTableRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysTableRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.oid, base + kOidOffset, sizeof(row.oid));
    std::memcpy(&row.namespace_oid, base + kNamespaceOidOffset, sizeof(row.namespace_oid));
    std::memcpy(row.name.data(), base + kNameOffset, kCatalogNameMax);
    std::memcpy(&row.desc_page_id, base + kDescPageIdOffset, sizeof(row.desc_page_id));
    std::uint8_t ct;
    std::memcpy(&ct, base + kClusteredTypeOffset, sizeof(ct));
    row.clustered_type = static_cast<ClusteredType>(ct);
    return row;
}

// ---- sys.columns -----------------------------------------------------

std::array<std::byte, SysColumnRow::kOnDiskSize> SysColumnRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kOidOffset, &oid, sizeof(oid));
    std::memcpy(base + kRelIdOffset, &rel_id, sizeof(rel_id));
    std::memcpy(base + kPosOffset, &pos, sizeof(pos));
    std::memcpy(base + kNameOffset, name.data(), kCatalogNameMax);
    std::memcpy(base + kTypeValOffset, &type_val, sizeof(type_val));
    std::memcpy(base + kLenOffset, &len, sizeof(len));
    auto nn = static_cast<std::uint8_t>(notnull ? 1 : 0);
    std::memcpy(base + kNotNullOffset, &nn, sizeof(nn));
    return buf;
}

StatusOr<SysColumnRow> SysColumnRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysColumnRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.oid, base + kOidOffset, sizeof(row.oid));
    std::memcpy(&row.rel_id, base + kRelIdOffset, sizeof(row.rel_id));
    std::memcpy(&row.pos, base + kPosOffset, sizeof(row.pos));
    std::memcpy(row.name.data(), base + kNameOffset, kCatalogNameMax);
    std::memcpy(&row.type_val, base + kTypeValOffset, sizeof(row.type_val));
    std::memcpy(&row.len, base + kLenOffset, sizeof(row.len));
    std::uint8_t nn;
    std::memcpy(&nn, base + kNotNullOffset, sizeof(nn));
    row.notnull = (nn != 0);
    return row;
}

// ---- sys.types --------------------------------------------------------

std::array<std::byte, SysTypeRow::kOnDiskSize> SysTypeRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kOidOffset, &oid, sizeof(oid));
    std::memcpy(base + kNameOffset, name.data(), kCatalogNameMax);
    std::memcpy(base + kTypeValOffset, &type_val, sizeof(type_val));
    std::memcpy(base + kLenOffset, &len, sizeof(len));
    return buf;
}

StatusOr<SysTypeRow> SysTypeRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysTypeRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.oid, base + kOidOffset, sizeof(row.oid));
    std::memcpy(row.name.data(), base + kNameOffset, kCatalogNameMax);
    std::memcpy(&row.type_val, base + kTypeValOffset, sizeof(row.type_val));
    std::memcpy(&row.len, base + kLenOffset, sizeof(row.len));
    return row;
}

// ---- sys.indexes -----------------------------------------------------

std::array<std::byte, SysIndexRow::kOnDiskSize> SysIndexRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kIndexOidOffset, &index_oid, sizeof(index_oid));
    std::memcpy(base + kTableOidOffset, &table_oid, sizeof(table_oid));
    std::memcpy(base + kColPosOffset, &col_pos, sizeof(col_pos));
    std::memcpy(base + kColTypeOffset, &col_type, sizeof(col_type));
    std::memcpy(base + kFlagsOffset, &flags, sizeof(flags));
    return buf;
}

StatusOr<SysIndexRow> SysIndexRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysIndexRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.index_oid, base + kIndexOidOffset, sizeof(row.index_oid));
    std::memcpy(&row.table_oid, base + kTableOidOffset, sizeof(row.table_oid));
    std::memcpy(&row.col_pos, base + kColPosOffset, sizeof(row.col_pos));
    std::memcpy(&row.col_type, base + kColTypeOffset, sizeof(row.col_type));
    std::memcpy(&row.flags, base + kFlagsOffset, sizeof(row.flags));
    return row;
}

}  // namespace kds::catalog
