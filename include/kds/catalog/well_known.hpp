#pragma once

#include <cctype>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"

// Well-known oids and fixed page ids. These values are persisted, so they
// are frozen: a number here may never change meaning, and a retired one is
// never reused.

namespace kds::catalog {

// Core 0: the core the instance's mount runs on and its few placements go
// to - the delete-mark purge's sweep, the checkpoint cadence's phase 0.
// **A placement, never an authority**: every core writes the superblock,
// the free map and the catalog (AT-S5), and no relation is placed on any
// core (AT-S9).
inline constexpr std::uint32_t kSystemCore = 0;

inline constexpr Oid kNamespaceSys = 0;
inline constexpr Oid kNamespacePublic = 1;

// Whether `namespace_oid` names the catalog's own namespace - the test
// behind every "'x' is a system relation and cannot be ..." refusal.
//
// **Spelled `== kNamespaceSys`, not `!= kNamespacePublic`**: a user
// relation in namespace `orders` is not a system relation, and the other
// spelling would refuse to rename, drop or alter it while calling it one
// (AF-P3, `instructions/v2.8.0/ratification-af-namespace.md`).
//
// **A gate wants the opposite failure and must not call this**: a
// permission question keeps `!= kNamespacePublic` at its own site - a
// different question that shared an answer while there were only two
// namespaces.
constexpr bool IsSystemNamespace(Oid namespace_oid) noexcept {
    return namespace_oid == kNamespaceSys;
}

// The **SQL spellings** of the two namespaces that exist before any DDL
// does, and they are not the names their `sys.objects` rows carry
// ("namespaceSys" / "namespacePublic", written by `InitWellKnownObjects`).
// Two names for one thing, which this codebase otherwise forbids - so the
// split is stated rather than left to be discovered:
//
// - `sys` is a qualifier in the grammar: `exec::kCatalogSchema` is "sys",
//   and `sys.tables` resolves to a *view* rather than to the bootstrap
//   relation of that name (`command_dispatcher.cpp`'s schema-qualified
//   arm), so a user namespace called `sys` would be ambiguous.
// - `public` is the default namespace and needs no qualifier; a user
//   namespace called `public` would be a *third* thing, and
//   `public.customer` would name it rather than the relation an
//   unqualified `customer` resolves to.
//
// So both are refused at `CREATE NAMESPACE` and both resolve in
// `FindNamespaceOidByName`. The registry names are left alone: they are
// what a `SELECT ... FROM sys.objects` shows.
inline constexpr std::string_view kNamespaceSysName = "sys";
inline constexpr std::string_view kNamespacePublicName = "public";

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

// docs/spec/types.md TY1. Appended rather than inserted, like every oid
// above them: these are seeded into `sys.types` at bootstrap and a moved
// oid would change what an existing catalog row means.
inline constexpr Oid kTypeDate = 31;
inline constexpr Oid kTypeTimestamp = 32;

// The wide decimal (TY2's separate type, 2026-08-07). Appended like the
// two above; a data file bootstrapped before it lacks the row, which
// costs a "?type_val=13" rendering for a column such a file cannot
// contain anyway - the same purely-additive stance TY9 took.
inline constexpr Oid kTypeDecimalWide = 33;

// A dropped relation's sys.objects row is *retyped* to this, never
// retired (docs/spec/drop-table.md DT2): GenerateUserOid() recovers its
// floor from the highest oid on the catalog pages - the rows are the
// counter - so the row must stay to keep the dead oid from being
// reissued, while every name lookup filters on kTypeTable and so frees
// the name immediately. A value, not a format change: type_oid was
// always data.
//
// Appended at 34 (2026-08-13) after briefly sharing 22 with
// `kTypeOperator`; a dropped-table row at 22 in an older file is still not
// `kTypeTable`, so it keeps freeing the name and counting the oid.
inline constexpr Oid kTypeDroppedTable = 34;

// A dropped **namespace**'s sys.objects row is retyped to this, for the
// identical reason the value above exists (AF-T1,
// `instructions/v2.8.0/ratification-af-namespace.md`): the row is
// `GenerateUserOid()`'s floor evidence, so it may never be retired, while
// every namespace lookup filters on `kTypeNamespace` and so frees the name
// at once.
//
// **A distinct value rather than reusing `kTypeDroppedTable`**, appended
// past it per the rule above. Sharing one oid would put two concepts under
// one identity, and the namespace sweep and the relation sweep would each
// match the other's tombstones.
inline constexpr Oid kTypeDroppedNamespace = 35;

inline constexpr Oid kSysTypesTable = 100;
inline constexpr Oid kSysObjectsTable = 110;
inline constexpr Oid kSysColumnsTable = 111;
inline constexpr Oid kSysTablesTable = 112;
inline constexpr Oid kSysIndexesTable = 113;

// sys.patterns (docs/spec/waystone-concpets.md section 4): one row per observed
// query shape, keyed by the parse-time fingerprint. A pattern is a catalog
// object because it is the durable, inspectable statement of what this
// database is asked to do - and because the waystone directory for a
// pattern has to be rooted somewhere that survives a restart.
inline constexpr Oid kSysPatternsTable = 114;

// **115, and 120 through 124, are retired.** They were `sys.pattern_defs`
// - the name and source text of a *declared* pattern - and its five
// `sys.columns` rows, until the operator withdrew user-declared patterns on
// 2026-08-31 (`docs/spec/create-pattern-user-defined-patterns-v1.md`, marked
// withdrawn and kept as the design record). Bootstrap no longer creates the
// relation and no build reads those rows.
//
// **They are never reused**: an oid names exactly one object for the life
// of a database, and a pre-withdrawal file opened by a later build would
// otherwise resolve a new relation's oid to rows that describe a pattern
// definition. A relation added later goes past the highest value here.

// sys.assertions (docs/spec/assertion.md §8.2, workplan AST03): one row per
// declared assertion - a group-level upper-bound constraint over one
// relation.
//
// **One of the two catalog relations stored in ordinary user tuple format**
// (sys.transitions, below, is the other), and the second built that way: it stores the declaration's `source_text`
// verbatim (AS10), and the fixed-length rule already answers where an
// arbitrary-length value goes. `sys.pattern_defs` was the first and is
// retired above, which leaves this relation carrying the argument alone.
// That choice is what lets the `GROUP BY` list have no cap at all - the
// columns are recovered by re-parsing the stored text, so a longer list
// costs text and not a widened row - and it is why there is no sibling
// relation for them.
//
// Its readers therefore cannot live in `catalog/` either: decoding needs
// `exec::DecodeRowInto`, and `exec/` depends on `catalog/`. They are
// `include/kds/exec/assertion_catalog.hpp`.
inline constexpr Oid kSysAssertionsTable = 116;

// Fixed oids for sys.assertions' six sys.columns rows, one per schema
// position. Fixed rather than from GenerateUserOid() because these rows are
// written *during* bootstrap: the oid sequence recovers its position by
// reading sys.objects and sys.columns, and asking it for an oid while those
// are the pages being built is a question with no answer yet.
inline constexpr Oid kSysAssertionsColumnOidBase = 140;

// sys.access_stats (docs/spec/heap-and-tuple.md §7): one row per access *shape*
// - `(kind, rel_id, column_mask)` - with how often it ran and when it last
// ran. A fixed-offset typed row like its neighbours, not a row-codec
// relation: everything in it is fixed-width, so none of sys.assertions'
// reasons apply.
inline constexpr Oid kSysAccessStatsTable = 130;

// sys.cabins (docs/spec/cabin.md §10): one row per Cabin - a
// `(relation, non-tuple-key column)` store authoritative for observed values.
// Fixed-offset typed rows like every catalog relation except
// sys.assertions, since nothing in the row is variable-width.
//
// The Cabin is a catalog object for the reason sys.patterns is: its
// *existence* is DDL and has to survive a restart. Its observed set does
// not, and deliberately is not stored here - §9 makes a crash unobserve
// every Cabin, which is invariant-preserving by C1's own terms.
inline constexpr Oid kSysCabinsTable = 131;

// sys.fkeys (docs/spec/foreign-keys.md §1): one row per foreign key - a
// child relation's column that references a parent relation's Keystone id.
// Fixed-offset typed rows like every catalog relation except
// sys.assertions, since nothing in the row is variable-width.
//
// **There is no parent-column field, and that is F1, not an omission.** A
// foreign key references the parent's engine tuple key and never a business key,
// so the parent side is fixed by the invariant that issues it: ids are
// issue-once (docs/rules/keystoneid-invariant.md K1) and a tuple key cannot be updated
// (invariant 11), which is what buys ON UPDATE CASCADE never having to
// exist and a stored reference being able to dangle, naming a different
// committed row only after a `PURGE` freed the parent's key - which needs
// the parent deleted, which RESTRICT refused while any child named it.
inline constexpr Oid kSysFkeysTable = 132;

// Oid 133 was `sys.ranges`' until the split relation was retired
// (2026-09-30). Unused and not reissued: nothing below kUserOidStart is
// generated.

// sys.transitions (docs/spec/transition.md): one row per Transition - the
// allowed state changes of one column. The second row-codec catalog
// relation, for sys.assertions' reason: a declaration's state values and
// edge list are neither fixed-width nor bounded, so the row stores the
// declaration's text and the reader re-parses it (BV-Q1 (a),
// `instructions/v3.2.0/workorder-bv-transition-constraints.md`). Past the
// highest well-known oid, as the note at 114 requires - 134..139 were never
// issued, but a gap is not where a relation goes.
inline constexpr Oid kSysTransitionsTable = 146;

// Fixed oids for sys.transitions' six sys.columns rows, 147..152, for
// kSysAssertionsColumnOidBase's reason: written during bootstrap, when the
// oid sequence cannot answer yet.
inline constexpr Oid kSysTransitionsColumnOidBase = 147;

// sys.cabin_exprs (docs/spec/cabin.md §10, AQ-R1): the canonical text of an
// expression or predicate Cabin's key function `F`, one row per such Cabin,
// keyed by its `cabin_id` in the `aux` column. The third declaration
// relation, for sys.assertions' reason: a predicate's literals are neither
// fixed-width nor bounded, so `sys.cabins` keeps its fixed row and the text
// lives here. A column Cabin has no row.
inline constexpr Oid kSysCabinExprsTable = 153;

// Fixed oids for sys.cabin_exprs' six sys.columns rows, 154..159.
inline constexpr Oid kSysCabinExprsColumnOidBase = 154;

// A Cabin's shape (`SysCabinRow::shape`, AR1-4: column, expression and
// predicate are shapes of one key function), with **0 reserved for "unset"**
// so a zeroed row reads as no Cabin of any shape. Here rather than in
// rows.hpp because the parser's `CabinStmt` carries one too.
inline constexpr std::uint8_t kCabinShapeUnset = 0;
inline constexpr std::uint8_t kCabinShapeColumn = 1;
inline constexpr std::uint8_t kCabinShapeExpression = 2;
inline constexpr std::uint8_t kCabinShapePredicate = 3;

// The **floor** for user-created object oids, not a counter.
//
// `Catalog::GenerateUserOid()` starts here on an empty database and, on a
// populated one, resumes past the highest oid `sys.objects` and `sys.columns`
// already carry - so an oid names one object for the life of the database.
//
// The position is *recovered*, not persisted (since 2026-08-08,
// docs/rules/keystoneid-k0-findings.md §6), so no format carries it.
// sys.patterns rows take their oid from a persistent per-relation sequence
// instead.
//
// The number itself may not move: it is the boundary below which every oid
// is a bootstrap oid, and lowering it would collide with them.
inline constexpr Oid kUserOidStart = 4000;

// ---- Every well-known oid, once, so a duplicate cannot compile ----------
//
// A list puts every oid side by side and a `static_assert` makes the
// compiler read it (two constants once shared 22 for three days,
// 2026-08-10..13).
//
// Namespaces, types and relations share one oid space - `register_namespace`
// and `register_type` both write into `sys.objects` - so they are checked
// together rather than per group. `kSysAssertionsColumnOidBase` names the
// start of a small run (six columns from 140); the base is listed and the
// run is clear of every other value here. **The retired 115 and 120-124 are
// absent**: this list cannot police a gap; the comment at 114 does.
//
// **`kTypeInt64` is deliberately absent**: it is an alias for `kTypeInt`, not
// a distinct oid, so listing it would fail this check on a fact that is
// intended. An alias belongs beside the thing it aliases, never here.
inline constexpr Oid kAllWellKnownOids[] = {
    kNamespaceSys,        kNamespacePublic,      kTypeInt,
    kTypeVarchar,         kTypeSchema,           kTypeBool,
    kTypeBytes,           kTypeNamespace,        kTypeAttribute,
    kTypeColumn,          kTypePage,             kTypeTable,
    kTypeOperator,        kTypeIndex,            kTypeChar,
    kTypeInt8,            kTypeInt16,            kTypeInt32,
    kTypeFloat,           kTypeDecimal,          kTypeUint64,
    kTypeDate,            kTypeTimestamp,        kTypeDecimalWide,
    kTypeDroppedTable,    kTypeDroppedNamespace, kSysTypesTable,        kSysObjectsTable,
    kSysColumnsTable,     kSysTablesTable,       kSysIndexesTable,
    kSysPatternsTable,    kSysAssertionsTable,   kSysAccessStatsTable,
    kSysCabinsTable,      kSysFkeysTable,        kSysAssertionsColumnOidBase,
    kSysTransitionsTable, kSysTransitionsColumnOidBase, kSysCabinExprsTable,
    kSysCabinExprsColumnOidBase, kUserOidStart,
};

// O(n^2) over ~39 values is ~750 compile-time comparisons - a sort would be
// faster and would need a mutable copy to be constexpr in, which is more
// machinery than the saving is worth at this size.
constexpr bool WellKnownOidsAreDistinct() {
    for (std::size_t i = 0; i < std::size(kAllWellKnownOids); ++i) {
        for (std::size_t j = i + 1; j < std::size(kAllWellKnownOids); ++j) {
            if (kAllWellKnownOids[i] == kAllWellKnownOids[j]) return false;
        }
    }
    return true;
}

static_assert(WellKnownOidsAreDistinct(),
              "two well-known oids share a value; an oid names exactly one object for the "
              "life of a database, so add the new one past the highest rather than into a "
              "gap, and list it above");

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

// **Page 10 is reserved and unused**: sys.pattern_defs' root until
// 2026-08-31, never written by this build, and never handed to another
// relation for the reason the retired oids are never reused.
//
// The rule `kCatalogPageAssertions` carries: a catalog relation's root
// page stays in the reserved range because bootstrap must find it without
// a catalog read, while its *var-heap* root is allocated by CreateNew()
// and recorded in sys.tables, where it is DDL-immutable and therefore
// cacheable (rows.hpp's note on varheap_page_id). That split is intended,
// not incidental - operator ratification 2026-08-31, crosscore.md
// CC12/CR1. One frame table serves every core, so no core needs a right to
// read a page another loaded.
inline constexpr PageId kCatalogPagePatternDefs = 10;
// sys.access_stats is pinned here, inside the reserved range. Every core
// writes it under its root page latch (AT-S7); the per-core copy CC12/CR3
// left open was declined (`core_runtime.hpp`).
inline constexpr PageId kCatalogPageAccessStats = 11;
inline constexpr PageId kCatalogPageCabins = 12;
inline constexpr PageId kCatalogPageFkeys = 13;

// Root heap page of sys.assertions. Fixed like the ones above, and - as
// a row-codec catalog relation (with sys.transitions) - its *var-heap* root is not:
// that one is allocated by CreateNew() and recorded in sys.tables, where it
// is DDL-immutable and therefore cacheable. Intended
// rather than incidental, and binding on any catalog relation that gains a
// var-heap: crosscore.md CC12/CR1 (operator ratification 2026-08-31).
//
// A peer reads this var-heap the way it reads anything else: the frame
// table is the instance's, so a page's id decides nothing about who may
// fault it. CR3 (a grown catalog page managed outside the reserved range,
// on the ordinary relation rules) is what governs.
inline constexpr PageId kCatalogPageAssertions = 14;

// Root heap page of sys.cabin_exprs (superblock 26, AQ-S1). Page 15 was
// `sys.ranges`' root until the split relation was retired (2026-09-30) and
// nothing created it since, so the new relation takes it. Its var-heap
// root is allocated, as sys.assertions' is.
inline constexpr PageId kCatalogPageCabinExprs = 15;

// Root heap page of sys.transitions (superblock 24), the highest fixed
// root. Ids 17..127 are unused, left for a future bootstrap root. Its
// var-heap root is allocated, as sys.assertions' is.
inline constexpr PageId kCatalogPageTransitions = 16;

// Every catalog relation's **root** page, in id order, with the relation
// it heads.
//
// One list, because several places need "all of them at once" and a
// hand-written second copy is how a page added later gets left out of one
// of them: `Catalog::Bootstrap()` creates them, `RetireDeleteMarks` walks
// them, and `InsertRow` stamps a chain's growth with the oid beside its
// root.
//
// **These are roots, not the whole relation.** A catalog relation is a
// chain of heap pages linked through `next_page_id`, exactly as a user
// relation is; the id here is where the chain starts, and every page past
// it comes from the free map's general supply (`crosscore.md` CC12/CR3),
// so the only way to name a relation's pages is to walk its chain.
// `kCatalogPagePatternDefs` is deliberately **not** in this list: nothing
// creates that page any more, so walking it would name a page this build
// never writes.
//
// **The oid is the owner a page the chain grows onto is stamped with**
// (page.md §2a), as `heap::ChainInsert` stamps a user relation's: the page
// says whose it is, so a walk that must not stray into another relation's
// page - the `sys.access_stats` discard - can test a link by what it
// reaches, and reclaim, which frees only a page a dropped user relation
// owns, passes it by. The roots themselves keep the owner bootstrap gave
// them: 0 for the fixed core, the relation's oid for the three row-codec
// relations.
struct CatalogRoot {
    PageId page;
    Oid oid;
};

inline constexpr CatalogRoot kCatalogRoots[] = {
    {kCatalogPageTypes, kSysTypesTable},
    {kCatalogPageColumns, kSysColumnsTable},
    {kCatalogPageObjects, kSysObjectsTable},
    {kCatalogPageTables, kSysTablesTable},
    {kCatalogPageIndexes, kSysIndexesTable},
    {kCatalogPagePatterns, kSysPatternsTable},
    {kCatalogPageAccessStats, kSysAccessStatsTable},
    {kCatalogPageCabins, kSysCabinsTable},
    {kCatalogPageFkeys, kSysFkeysTable},
    {kCatalogPageAssertions, kSysAssertionsTable},
    {kCatalogPageCabinExprs, kSysCabinExprsTable},
    {kCatalogPageTransitions, kSysTransitionsTable},
};

// The relation whose chain starts at `root`; 0, unattributed, for a page
// that heads none.
constexpr Oid CatalogRelationAtRoot(PageId root) {
    for (const CatalogRoot& entry : kCatalogRoots) {
        if (entry.page == root) return entry.oid;
    }
    return 0;
}

// Distinct roots, every one below `first_user_page`, in one pass over the
// list that already exists, for `WellKnownOidsAreDistinct()`'s reason:
// nothing else puts the root ids side by side, and joining `kCatalogRoots`
// is the whole job for a new bootstrap relation. The bound is a parameter
// because it is `server::kFirstUserPageId`, which this header does not
// include; the static_assert lives in catalog.cpp, which is free to
// include both. Only the roots are fixed: bootstrap must find a root
// without a catalog read, so it is created at its id (`CreateAt`), below
// the first id `CreateNew` hands out.
constexpr bool CatalogRootsAreDistinctAndFixed(PageId first_user_page) {
    for (std::size_t i = 0; i < std::size(kCatalogRoots); ++i) {
        if (kCatalogRoots[i].page >= first_user_page) return false;
        for (std::size_t j = i + 1; j < std::size(kCatalogRoots); ++j) {
            if (kCatalogRoots[i].page == kCatalogRoots[j].page) return false;
        }
    }
    return true;
}

// Transaction id stamped on every bootstrap-time tuple - mirrors
// PostgreSQL's FrozenTransactionId: bootstrap rows are inserted before a
// transaction manager exists, so they need a fixed, always-visible xmin
// rather than one from a real transaction.
inline constexpr std::uint64_t kBootstrapXid = 1;

// The first id a real transaction may be issued (docs/spec/txn.md section 4.2).
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

// ---- docs/spec/types.md TY1 / TY9 --------------------------------------
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

// The wide decimal (docs/spec/types.md TY2's "future separate type",
// built 2026-08-07): `decimal(p, s)` with `19 <= p <= 38`, stored as an
// **int128 unscaled value in 16 LE bytes** - a different schema constant
// coexisting with the 8-byte type, never a widening of it. Selected by
// the declared precision at CREATE TABLE (the one DDL site), or declared
// directly as `decimal128(p, s)`; `(p, s)` packs into `SysColumnRow::len`
// exactly as the narrow type's does, and 38 fits the precision byte with
// room to spare. 38 is the cap because 10^38 - 1 < 2^127.
inline constexpr std::uint32_t kTypeValDecimalWide = 13;

// `catalog::TypeModOf` (rows.hpp) spells these two numbers literally,
// because rows.hpp is included *by* this header and cannot name them. The
// two spellings are pinned together here so a renumber - which would be a
// format change anyway - cannot leave one of them behind.
static_assert(kTypeValDecimal == 7 && kTypeValDecimalWide == 13,
              "catalog::TypeModOf spells these values literally; keep them in step");

// How a relation's rows are stored, on its `sys.tables` row. Every user
// relation is `kBtree` (BZ-R1 of
// `instructions/v3.2.0/workorder-bz-heap-hidden-key.md`); `kCatalogChain` is
// the system relations' own, a chain of heap pages the catalog walks
// (`heap_chain.hpp`), and the on-disk 0 a pre-BZ user heap also carried.
enum class ClusteredType : std::uint8_t {
    kCatalogChain = 0,
    kBtree = 1,
};

}  // namespace kds::catalog
