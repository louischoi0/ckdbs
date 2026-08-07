#pragma once

#include <array>
#include <cstddef>
#include <iterator>

#include "kds/base/common.hpp"
#include "kds/catalog/oid.hpp"

// Well-known oids and fixed page ids. These values are persisted, so they
// are frozen: a number here may never change meaning, and a retired one is
// never reused.

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

// docs/spec-types.md TY1. Appended rather than inserted, like every oid
// above them: these are seeded into `sys.types` at bootstrap and a moved
// oid would change what an existing catalog row means.
inline constexpr Oid kTypeDate = 31;
inline constexpr Oid kTypeTimestamp = 32;

// The wide decimal (TY2's separate type, 2026-08-07). Appended like the
// two above; a data file bootstrapped before it lacks the row, which
// costs a "?type_val=13" rendering for a column such a file cannot
// contain anyway - the same purely-additive stance TY9 took.
inline constexpr Oid kTypeDecimalWide = 33;

inline constexpr Oid kSysTypesTable = 100;
inline constexpr Oid kSysObjectsTable = 110;
inline constexpr Oid kSysColumnsTable = 111;
inline constexpr Oid kSysTablesTable = 112;
inline constexpr Oid kSysIndexesTable = 113;

// sys.patterns (docs/waystone-concpets.md section 4): one row per observed
// query shape, keyed by the parse-time fingerprint. A pattern is a catalog
// object because it is the durable, inspectable statement of what this
// database is asked to do - and because the waystone directory for a
// pattern has to be rooted somewhere that survives a restart.
inline constexpr Oid kSysPatternsTable = 114;

// sys.pattern_defs (docs/spec-create-pattern-user-defined-patterns-v1.md
// section 4.2): the name and source text of a *declared* pattern, joined to
// sys.patterns by pattern_id. An auto-registered pattern has no row here and
// keeps printing as a bare hex id.
//
// It is a sibling relation rather than two more fields on SysPatternRow
// because that row is fixed-width and stays that way, and a pattern's source
// text is neither fixed-width nor small.
//
// **This is the first catalog relation stored in ordinary user tuple
// format** - a Keystone word, tagged cells, var-heap spill - where every
// other one is a fixed-offset typed row codec (catalog/rows.hpp). That is
// the deliberate consequence of storing text: the fixed-length rule already
// answers "where do arbitrary-length values go", and inventing a second
// answer for one catalog row would be inventing a second var-heap protocol.
// It costs one thing worth naming: its rows cannot be read from
// `catalog/`, because decoding them needs the row codec, which sits above
// the catalog. The readers live in stats/pattern_defs.hpp.
inline constexpr Oid kSysPatternDefsTable = 115;

// Fixed oids for sys.pattern_defs' four sys.columns rows, one per schema
// position. Fixed rather than from GenerateUserOid() for the reason
// kUserOidStart records below: that counter is in-memory and restarts every
// boot, and these rows are persisted.
inline constexpr Oid kSysPatternDefsColumnOidBase = 120;

// sys.access_stats (docs/heap-and-tuple.md §7): one row per access *shape*
// - `(kind, rel_id, column_mask)` - with how often it ran and when it last
// ran. A fixed-offset typed row like its neighbours, not a row-codec
// relation: everything in it is fixed-width, so none of sys.pattern_defs'
// reasons apply.
inline constexpr Oid kSysAccessStatsTable = 130;

// sys.cabins (docs/feat-cabin.md §10): one row per Cabin - a
// `(relation, non-pk column)` store authoritative for observed values.
// Fixed-offset typed rows like every catalog relation except
// sys.pattern_defs, since nothing in the row is variable-width.
//
// The Cabin is a catalog object for the reason sys.patterns is: its
// *existence* is DDL and has to survive a restart. Its observed set does
// not, and deliberately is not stored here - §9 makes a crash unobserve
// every Cabin, which is invariant-preserving by C1's own terms.
inline constexpr Oid kSysCabinsTable = 131;

// sys.fkeys (docs/impl-foreign-keys.md §1): one row per foreign key - a
// child relation's column that references a parent relation's Keystone id.
// Fixed-offset typed rows like every catalog relation except
// sys.pattern_defs, since nothing in the row is variable-width.
//
// **There is no parent-column field, and that is F1, not an omission.** A
// foreign key references the parent's engine pk and never a business key,
// so the parent side is fixed by the invariant that issues it: ids are
// issue-once (docs/keystoneid-invariant.md K1) and a pk cannot be updated
// (invariant 11), which is what buys ON UPDATE CASCADE never having to
// exist and a stored reference being able to dangle but never to
// mis-attribute.
inline constexpr Oid kSysFkeysTable = 132;

// Starting point for user-created object oids. **KNOWN GAP:** this counter
// is in-memory only and resets on every process restart, so two objects
// created in different runs can share an oid. Persisting it means adding a
// field to kds::server::SuperBlock, a layout change other code depends on.
// This is why sys.patterns rows take their oid from a persistent sequence
// instead (Catalog::RegisterPattern).
inline constexpr Oid kUserOidStart = 4000;

// Fixed page ids for the bootstrap catalog heap pages. Reserved: a
// PageStore's CreateAt is called with these exact values during
// Catalog::Bootstrap(), never handed out by a general-purpose allocator.
// All sit below kds::server::kFirstUserPageId (128), which is where the
// reserved range ends.
inline constexpr PageId kCatalogPageTypes = 4;
inline constexpr PageId kCatalogPageColumns = 5;
inline constexpr PageId kCatalogPageObjects = 6;
inline constexpr PageId kCatalogPageTables = 7;
inline constexpr PageId kCatalogPageIndexes = 8;
inline constexpr PageId kCatalogPagePatterns = 9;

// Root heap page of sys.pattern_defs. Fixed like the six above, even though
// the relation is an ordinary row-codec one: it has to be findable at
// bootstrap without a catalog read, which is the same reason the others are.
// Its *var-heap* root is not fixed - that one is allocated by CreateNew()
// and recorded in sys.tables, where it is DDL-immutable and therefore
// cacheable (rows.hpp's note on varheap_page_id).
inline constexpr PageId kCatalogPagePatternDefs = 10;
inline constexpr PageId kCatalogPageAccessStats = 11;
inline constexpr PageId kCatalogPageCabins = 12;
inline constexpr PageId kCatalogPageFkeys = 13;

// Every catalog relation's **root** page, in id order.
//
// One list, because two places now need "all of them at once" and a
// hand-written second copy is how a page added later gets left out of one
// of them: `Catalog::Bootstrap()` creates them, and multicore flushes them
// before telling peers to re-read (docs/workplan-crosscore.md P6). A page
// missing from the flush would leave a peer permanently unable to see the
// relation it describes.
//
// **These are roots, not the whole relation.** A catalog relation is a
// chain of heap pages linked through `next_page_id`, exactly as a user
// relation is; the id here is where the chain starts. Callers that need
// every page use the overflow range below as well.
inline constexpr PageId kAllCatalogPages[] = {
    kCatalogPageTypes,       kCatalogPageColumns,     kCatalogPageObjects,
    kCatalogPageTables,      kCatalogPageIndexes,     kCatalogPagePatterns,
    kCatalogPagePatternDefs, kCatalogPageAccessStats, kCatalogPageCabins,
    kCatalogPageFkeys,
};

// ---- Where a catalog chain grows into ------------------------------------
//
// A catalog relation that outgrows its root page takes its next page from
// **this reserved range**, never from the free map's general supply, and
// that is a correctness requirement rather than tidiness.
//
// Two rules depend on a catalog page's id being low. A peer core may fault
// any page below `system_page_limit_` read-only and may fault nothing else
// it was not leased (`DevicePageStore::MayFault`, workplan-crosscore.md P6)
// - so a catalog page allocated from the general supply, at an id above
// that limit, would be one a peer could not read, and a peer that cannot
// read the catalog cannot resolve a relation at all. And the flush that
// precedes `kCatalogInvalidate` has to name every catalog page; a bounded
// range can be named, an arbitrary set of general-supply ids cannot without
// storing it somewhere durable.
//
// The range is what is left of the reserved low pages after the roots: ids
// 14 up to the first user page. That is ~114 pages, and a page holds ~68
// `sys.columns` rows, so the whole instance's ceiling moves from ~68
// columns to ~7,800. It is a ceiling and not "unbounded" - saying so is the
// point, because the previous ceiling was also unstated until something hit
// it.
inline constexpr PageId kCatalogOverflowFirst = 14;

// One past the last id a catalog chain may take. Must equal
// kds::server::kFirstUserPageId; the static_assert lives in catalog.cpp,
// which is free to include both headers.
inline constexpr PageId kCatalogOverflowLimit = 128;

static_assert(kCatalogOverflowFirst > kCatalogPageFkeys,
              "the overflow range must start past every fixed root");

// Every page a catalog relation can occupy: the roots, then the whole
// overflow range.
//
// The two callers - the flush that precedes `kCatalogInvalidate` and a
// peer's eviction of its stale copy - both need "all of them", and both
// tolerate an id that is not resident (`FlushPages` skips a page it has no
// frame for; `EvictClean` erases nothing). So the range is named in full
// rather than tracked: an exact set would have to be discovered by walking
// every chain, and a walk that happens on the invalidation path is a walk
// that can fail there.
inline constexpr std::size_t kCatalogPageSpanSize =
    std::size(kAllCatalogPages) + (kCatalogOverflowLimit - kCatalogOverflowFirst);

inline constexpr std::array<PageId, kCatalogPageSpanSize> MakeCatalogPageSpan() {
    std::array<PageId, kCatalogPageSpanSize> out{};
    std::size_t at = 0;
    for (const PageId id : kAllCatalogPages) out[at++] = id;
    for (PageId id = kCatalogOverflowFirst; id < kCatalogOverflowLimit; ++id) out[at++] = id;
    return out;
}

inline constexpr std::array<PageId, kCatalogPageSpanSize> kEveryCatalogPage =
    MakeCatalogPageSpan();

// Transaction id stamped on every bootstrap-time tuple - mirrors
// PostgreSQL's FrozenTransactionId: bootstrap rows are inserted before a
// transaction manager exists, so they need a fixed, always-visible xmin
// rather than one from a real transaction.
inline constexpr std::uint64_t kBootstrapXid = 1;

// The first id a real transaction may be issued (docs/txn.md section 4.2).
// 2, so kBootstrapXid is never reissued - which is what makes it safe for
// every read view to trust that id unconditionally and permanently.
inline constexpr std::uint64_t kFirstUserTrxId = 2;
static_assert(kFirstUserTrxId > kBootstrapXid);

// sys.columns/sys.types `type_val` tags for the scalar types Bootstrap()
// registers. Placeholder values (no external format they must match) but
// named rather than left as magic numbers now that row_codec.cpp switches
// on them to decide on-disk encoding. kTypeValFloat is the one declared
// type with no encoding anywhere - storage refuses it at CREATE TABLE and
// the wire refuses it by fallthrough; kTypeValDecimal gained its storage
// encoding at TY04 and its wire encoding on 2026-08-07 (protocol.md §6:
// unscaled int64 LE, scale in the row description's type_mod).
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

// ---- docs/spec-types.md TY1 / TY9 --------------------------------------
//
// **Purely additive**, and that is the whole migration story: no existing
// `type_val` changes meaning, so no existing relation does either.
//
// All three are fixed-width and compare as signed integers, which is the
// selection criterion TY2 and TY4 applied rather than a coincidence - it
// is what lets every ordered structure in the engine take them unmodified:
// a btree's clustering, a `kRange`'s bounds, MIN/MAX's int arm, and the
// fold's first-seen group key encoding.
//
//   kTypeValDate       int32 LE, days since 1970-01-01
//   kTypeValTimestamp  int64 LE, microseconds since the epoch, **UTC**
//   kTypeValDecimal    int64 LE, the unscaled value (reserved since
//                      bootstrap, given an encoding here)
inline constexpr std::uint32_t kTypeValDate = 11;
inline constexpr std::uint32_t kTypeValTimestamp = 12;

// The wide decimal (docs/spec-types.md TY2's "future separate type",
// built 2026-08-07): `decimal(p, s)` with `19 <= p <= 38`, stored as an
// **int128 unscaled value in 16 LE bytes** - a different schema constant
// coexisting with the 8-byte type, never a widening of it. Selected by
// the declared precision at CREATE TABLE (the one DDL site), or declared
// directly as `decimal128(p, s)`; `(p, s)` packs into `SysColumnRow::len`
// exactly as the narrow type's does, and 38 fits the precision byte with
// room to spare. 38 is the cap because 10^38 - 1 < 2^127.
inline constexpr std::uint32_t kTypeValDecimalWide = 13;

enum class ClusteredType : std::uint8_t {
    kHeap = 0,
    kBtree = 1,
};

}  // namespace kds::catalog
