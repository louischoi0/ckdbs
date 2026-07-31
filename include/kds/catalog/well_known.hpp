#pragma once

#include "kds/base/common.hpp"
#include "kds/catalog/oid.hpp"

// Well-known oids / fixed page ids, ported from the legacy kernel engine's
// kds_catalog.h constants of the same names (values kept identical where
// there is no reason to renumber, so anyone cross-referencing the legacy
// design doc isn't tripped up).

namespace kds::catalog {

inline constexpr Oid kNamespaceSys = 0;
inline constexpr Oid kNamespacePublic = 1;

inline constexpr Oid kTypeInt = 12;
inline constexpr Oid kTypeVarchar = 13;
inline constexpr Oid kTypeSchema = 14;
inline constexpr Oid kTypeBool = 15;
inline constexpr Oid kTypeBytes = 16;
inline constexpr Oid kTypeNamespace = 17;
inline constexpr Oid kTypeAttribute = 18;
inline constexpr Oid kTypeColumn = 19;
inline constexpr Oid kTypePage = 20;
inline constexpr Oid kTypeTable = 21;
inline constexpr Oid kTypeOperator = 22;
inline constexpr Oid kTypeIndex = 23;
inline constexpr Oid kTypeChar = 24;

inline constexpr Oid kTypeInt8 = 25;
inline constexpr Oid kTypeInt16 = 26;
inline constexpr Oid kTypeInt32 = 27;
inline constexpr Oid kTypeFloat = 28;
inline constexpr Oid kTypeDecimal = 29;
inline constexpr Oid kTypeUint64 = 30;
inline constexpr Oid kTypeInt64 = kTypeInt;

inline constexpr Oid kSysTypesTable = 100;
inline constexpr Oid kSysObjectsTable = 110;
inline constexpr Oid kSysColumnsTable = 111;
inline constexpr Oid kSysTablesTable = 112;
inline constexpr Oid kSysIndexesTable = 113;

// Starting point for user-created object oids. KNOWN GAP (same as the
// legacy engine): this counter is in-memory only and resets on every
// process restart - persisting it would mean adding a field to
// kds::server::SuperBlock, deliberately not done here since that's a
// layout change to a struct other code already depends on.
inline constexpr Oid kUserOidStart = 4000;

// Fixed page ids for the bootstrap catalog heap pages. Reserved: a
// PageStore's CreateAt is called with these exact values during
// Catalog::Bootstrap(), never handed out by a general-purpose allocator.
// All sit below kds::server::kFirstUserPageId (128), same convention the
// legacy engine used (KDS_SYS_RESERVED_PAGES).
inline constexpr PageId kCatalogPageTypes = 4;
inline constexpr PageId kCatalogPageColumns = 5;
inline constexpr PageId kCatalogPageObjects = 6;
inline constexpr PageId kCatalogPageTables = 7;
inline constexpr PageId kCatalogPageIndexes = 8;

// Transaction id stamped on every bootstrap-time tuple - mirrors
// PostgreSQL's FrozenTransactionId: bootstrap rows are inserted before a
// transaction manager exists, so they need a fixed, always-visible xmin
// rather than one from a real transaction.
inline constexpr std::uint64_t kBootstrapXid = 1;

// sys.columns/sys.types `type_val` tags for the scalar types Bootstrap()
// registers. Placeholder values (no external format they must match) but
// named rather than left as magic numbers now that row_codec.cpp switches
// on them to decide on-disk encoding - see that file's comment for which
// of these are actually encodable today (kTypeValFloat/kTypeValDecimal
// are declared but not yet given an on-disk encoding, a currently open
// decision - see CLAUDE.md's KWP `DECIMAL` wire encoding open item, which
// extends to storage until a type registry exists).
inline constexpr std::uint32_t kTypeValInt8 = 1;
inline constexpr std::uint32_t kTypeValInt16 = 2;
inline constexpr std::uint32_t kTypeValInt32 = 3;
inline constexpr std::uint32_t kTypeValInt64 = 4;
inline constexpr std::uint32_t kTypeValUint64 = 5;
inline constexpr std::uint32_t kTypeValFloat = 6;
inline constexpr std::uint32_t kTypeValDecimal = 7;
inline constexpr std::uint32_t kTypeValBool = 8;
inline constexpr std::uint32_t kTypeValVarchar = 9;
inline constexpr std::uint32_t kTypeValChar = 10;

enum class ClusteredType : std::uint8_t {
    kHeap = 0,
    kBtree = 1,
};

}  // namespace kds::catalog
