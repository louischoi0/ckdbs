#include <string>
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
    std::memcpy(base + kNextIdOffset, &next_id, sizeof(next_id));
    std::memcpy(base + kVarHeapPageIdOffset, &varheap_page_id, sizeof(varheap_page_id));
    std::memcpy(base + kOwnerCoreOffset, &owner_core, sizeof(owner_core));
    auto ko = static_cast<std::uint8_t>(key_order);
    std::memcpy(base + kKeyOrderOffset, &ko, sizeof(ko));
    std::memcpy(base + kAnchorPageIdOffset, &anchor_page_id, sizeof(anchor_page_id));
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
    std::memcpy(&row.next_id, base + kNextIdOffset, sizeof(row.next_id));
    std::memcpy(&row.varheap_page_id, base + kVarHeapPageIdOffset, sizeof(row.varheap_page_id));
    std::memcpy(&row.owner_core, base + kOwnerCoreOffset, sizeof(row.owner_core));
    std::uint8_t ko;
    std::memcpy(&ko, base + kKeyOrderOffset, sizeof(ko));
    row.key_order = static_cast<KeyOrder>(ko);
    std::memcpy(&row.anchor_page_id, base + kAnchorPageIdOffset, sizeof(row.anchor_page_id));
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
    std::memcpy(base + kCabinPolicyOffset, &cabin_policy, sizeof(cabin_policy));
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
    std::memcpy(&row.cabin_policy, base + kCabinPolicyOffset, sizeof(row.cabin_policy));
    // Pure, like its neighbours. `cabin_policy == kCabinPolicyUnset` is a
    // legitimate stored value ("nothing was said"), and turning it into the
    // effective policy is the reader's job through EffectiveCabinPolicy() -
    // a decode that substituted would erase the distinction the two values
    // exist to keep.
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
    std::memcpy(base + kRootPageIdOffset, &root_page_id, sizeof(root_page_id));
    std::memcpy(base + kKeyWidthOffset, &key_width, sizeof(key_width));
    std::memcpy(base + kEntryWidthOffset, &entry_width, sizeof(entry_width));
    std::memcpy(base + kNameOffset, name.data(), kCatalogNameMax);
    std::memcpy(base + kNkeysOffset, &nkeys, sizeof(nkeys));
    std::memcpy(base + kNcoveredOffset, &ncovered, sizeof(ncovered));
    std::memcpy(base + kFlagsOffset, &flags, sizeof(flags));
    std::memcpy(base + kReserved0Offset, &reserved0, sizeof(reserved0));
    // Element by element rather than one block copy of the array: the
    // on-disk stride is sizeof(uint16_t) by declaration, and a block copy
    // would silently adopt whatever stride the compiler chose for the
    // std::array (rules.md #2).
    for (std::size_t i = 0; i < kMaxIndexKeyColumns; ++i) {
        std::memcpy(base + kKeyColsOffset + i * sizeof(std::uint16_t), &key_cols[i],
                    sizeof(std::uint16_t));
    }
    for (std::size_t i = 0; i < kMaxIndexCoveredColumns; ++i) {
        std::memcpy(base + kCoveredColsOffset + i * sizeof(std::uint16_t), &covered_cols[i],
                    sizeof(std::uint16_t));
    }
    return buf;
}

StatusOr<SysIndexRow> SysIndexRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysIndexRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.index_oid, base + kIndexOidOffset, sizeof(row.index_oid));
    std::memcpy(&row.table_oid, base + kTableOidOffset, sizeof(row.table_oid));
    std::memcpy(&row.root_page_id, base + kRootPageIdOffset, sizeof(row.root_page_id));
    std::memcpy(&row.key_width, base + kKeyWidthOffset, sizeof(row.key_width));
    std::memcpy(&row.entry_width, base + kEntryWidthOffset, sizeof(row.entry_width));
    std::memcpy(row.name.data(), base + kNameOffset, kCatalogNameMax);
    std::memcpy(&row.nkeys, base + kNkeysOffset, sizeof(row.nkeys));
    std::memcpy(&row.ncovered, base + kNcoveredOffset, sizeof(row.ncovered));
    std::memcpy(&row.flags, base + kFlagsOffset, sizeof(row.flags));
    std::memcpy(&row.reserved0, base + kReserved0Offset, sizeof(row.reserved0));
    for (std::size_t i = 0; i < kMaxIndexKeyColumns; ++i) {
        std::memcpy(&row.key_cols[i], base + kKeyColsOffset + i * sizeof(std::uint16_t),
                    sizeof(std::uint16_t));
    }
    for (std::size_t i = 0; i < kMaxIndexCoveredColumns; ++i) {
        std::memcpy(&row.covered_cols[i], base + kCoveredColsOffset + i * sizeof(std::uint16_t),
                    sizeof(std::uint16_t));
    }

    // A count past its array would let every later reader index out of
    // bounds off one corrupt byte. Checked here, at the one door these rows
    // come through, rather than at each of them.
    if (row.nkeys == 0 || row.nkeys > kMaxIndexKeyColumns ||
        row.ncovered > kMaxIndexCoveredColumns) {
        return Status::Corruption("sys.indexes row claims " + std::to_string(row.nkeys) +
                                  " key and " + std::to_string(row.ncovered) +
                                  " covered columns, outside the declared limits");
    }
    return row;
}

// ---- sys.patterns ----------------------------------------------------

std::array<std::byte, SysPatternRow::kOnDiskSize> SysPatternRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kOidOffset, &oid, sizeof(oid));
    std::memcpy(base + kPatternIdOffset, &pattern_id, sizeof(pattern_id));
    std::memcpy(base + kLastSeenOffset, &last_seen, sizeof(last_seen));
    std::memcpy(base + kFingerprintVersionOffset, &fingerprint_version,
                sizeof(fingerprint_version));
    std::memcpy(base + kWaystoneRootOffset, &waystone_root, sizeof(waystone_root));
    std::memcpy(base + kUseCountOffset, &use_count, sizeof(use_count));
    std::memcpy(base + kStmtClassOffset, &stmt_class, sizeof(stmt_class));
    std::memcpy(base + kDirDepthOffset, &dir_depth, sizeof(dir_depth));
    std::memcpy(base + kFlagsOffset, &flags, sizeof(flags));
    std::memcpy(base + kOriginOffset, &origin, sizeof(origin));
    return buf;
}

StatusOr<SysPatternRow> SysPatternRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysPatternRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.oid, base + kOidOffset, sizeof(row.oid));
    std::memcpy(&row.pattern_id, base + kPatternIdOffset, sizeof(row.pattern_id));
    std::memcpy(&row.last_seen, base + kLastSeenOffset, sizeof(row.last_seen));
    std::memcpy(&row.fingerprint_version, base + kFingerprintVersionOffset,
                sizeof(row.fingerprint_version));
    std::memcpy(&row.waystone_root, base + kWaystoneRootOffset, sizeof(row.waystone_root));
    std::memcpy(&row.use_count, base + kUseCountOffset, sizeof(row.use_count));
    std::memcpy(&row.stmt_class, base + kStmtClassOffset, sizeof(row.stmt_class));
    std::memcpy(&row.dir_depth, base + kDirDepthOffset, sizeof(row.dir_depth));
    std::memcpy(&row.flags, base + kFlagsOffset, sizeof(row.flags));
    std::memcpy(&row.origin, base + kOriginOffset, sizeof(row.origin));
    // Deliberately no validation of the decoded values - not of
    // fingerprint_version, not of the root/depth pair. This is a pure
    // decode, like every Decode() above it: the size check is the only
    // corruption signal it owns, and whether a version is current or a
    // root/depth pair is coherent are questions for the layer that acts on
    // them (P04), which is also the only layer that can do anything about
    // the answer.
    return row;
}

// ---- sys.access_stats -------------------------------------------------

std::array<std::byte, SysAccessStatRow::kOnDiskSize> SysAccessStatRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kRelIdOffset, &rel_id, sizeof(rel_id));
    std::memcpy(base + kColumnMaskOffset, &column_mask, sizeof(column_mask));
    std::memcpy(base + kUseCountOffset, &use_count, sizeof(use_count));
    std::memcpy(base + kLastSeenOffset, &last_seen, sizeof(last_seen));
    std::memcpy(base + kKindOffset, &kind, sizeof(kind));
    return buf;
}

StatusOr<SysAccessStatRow> SysAccessStatRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysAccessStatRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.rel_id, base + kRelIdOffset, sizeof(row.rel_id));
    std::memcpy(&row.column_mask, base + kColumnMaskOffset, sizeof(row.column_mask));
    std::memcpy(&row.use_count, base + kUseCountOffset, sizeof(row.use_count));
    std::memcpy(&row.last_seen, base + kLastSeenOffset, sizeof(row.last_seen));
    std::memcpy(&row.kind, base + kKindOffset, sizeof(row.kind));
    // A pure decode, like every other one here: the size check is the only
    // corruption signal it owns. Whether `kind` names a real access kind is
    // a question for the layer that acts on it.
    return row;
}

std::array<std::byte, SysCabinRow::kOnDiskSize> SysCabinRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kCabinIdOffset, &cabin_id, sizeof(cabin_id));
    std::memcpy(base + kRelOidOffset, &rel_oid, sizeof(rel_oid));
    std::memcpy(base + kObservedCtOffset, &observed_ct, sizeof(observed_ct));
    std::memcpy(base + kColumnNoOffset, &column_no, sizeof(column_no));
    std::memcpy(base + kOriginOffset, &origin, sizeof(origin));
    std::memcpy(base + kStatusOffset, &status, sizeof(status));
    return buf;
}

StatusOr<SysCabinRow> SysCabinRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysCabinRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.cabin_id, base + kCabinIdOffset, sizeof(row.cabin_id));
    std::memcpy(&row.rel_oid, base + kRelOidOffset, sizeof(row.rel_oid));
    std::memcpy(&row.observed_ct, base + kObservedCtOffset, sizeof(row.observed_ct));
    std::memcpy(&row.column_no, base + kColumnNoOffset, sizeof(row.column_no));
    std::memcpy(&row.origin, base + kOriginOffset, sizeof(row.origin));
    std::memcpy(&row.status, base + kStatusOffset, sizeof(row.status));
    // Pure, like its neighbours: whether `origin` and `status` name real
    // values is the acting layer's question, and `column_no == 0` - a row
    // claiming a Cabin on the pk - is CreateCabin()'s to refuse, not this
    // function's to reinterpret.
    return row;
}

std::array<std::byte, SysFkeyRow::kOnDiskSize> SysFkeyRow::Encode() const {
    std::array<std::byte, kOnDiskSize> buf{};
    std::byte* base = buf.data();
    std::memcpy(base + kFkIdOffset, &fk_id, sizeof(fk_id));
    std::memcpy(base + kChildRelOidOffset, &child_rel_oid, sizeof(child_rel_oid));
    std::memcpy(base + kParentRelOidOffset, &parent_rel_oid, sizeof(parent_rel_oid));
    std::memcpy(base + kChildColumnNoOffset, &child_column_no, sizeof(child_column_no));
    std::memcpy(base + kFlagsOffset, &flags, sizeof(flags));
    return buf;
}

StatusOr<SysFkeyRow> SysFkeyRow::Decode(std::span<const std::byte> bytes) {
    if (Status s = CheckSize(bytes, kOnDiskSize); !s.ok()) return s;

    SysFkeyRow row{};
    const std::byte* base = bytes.data();
    std::memcpy(&row.fk_id, base + kFkIdOffset, sizeof(row.fk_id));
    std::memcpy(&row.child_rel_oid, base + kChildRelOidOffset, sizeof(row.child_rel_oid));
    std::memcpy(&row.parent_rel_oid, base + kParentRelOidOffset, sizeof(row.parent_rel_oid));
    std::memcpy(&row.child_column_no, base + kChildColumnNoOffset, sizeof(row.child_column_no));
    std::memcpy(&row.flags, base + kFlagsOffset, sizeof(row.flags));
    // Pure, like its neighbours: a `child_column_no == 0` - a foreign key
    // claiming the child's own pk - is CreateForeignKey()'s to refuse, and
    // whether the two oids name live relations is a question this function
    // has no catalog to ask.
    return row;
}

std::string ColumnTypeText(const SysColumnRow& col, std::string_view base_name) {
    if (col.type_val == kTypeValDecimal || col.type_val == kTypeValDecimalWide) {
        return std::string(base_name) + "(" + std::to_string(DecimalPrecisionOf(col.len)) + "," +
               std::to_string(DecimalScaleOf(col.len)) + ")";
    }
    // `char` is the other type whose declared width is part of its name.
    if (col.type_val == kTypeValChar) {
        return std::string(base_name) + "(" + std::to_string(col.len) + ")";
    }
    // Every other type's width comes from its type_val, so `len` says
    // nothing a reader wants and the bare name is the whole truth.
    return std::string(base_name);
}

}  // namespace kds::catalog
