#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/catalog/rows.hpp"

// In-memory schema, built from the sys.columns rows for a given rel_id,
// plus the per-relation access handles the cache hands out.

namespace kds::catalog {

struct Schema {
    std::vector<SysColumnRow> columns;

    const SysColumnRow* FindColumn(std::string_view name) const noexcept;
};

// True for the integer types a Keystone id can be declared as. Float,
// decimal, bool, char and varchar are not among them: none can represent a
// 40-bit id, and a pk that cannot hold its own value is a table no row can
// be written to.
bool IsIntegerTypeVal(std::uint32_t type_val) noexcept;

// Rejects a schema that cannot carry a Keystone word: no columns at all,
// or a first column whose declared type is not an integer one. Every
// relation's first column is its system-generated primary key
// (heap-and-tuple.md section 4), so this is a property of the schema
// itself, checked at CREATE TABLE rather than at the first INSERT.
Status CheckKeystoneColumn(const Schema& schema);

// Refuses a column whose declared type this build cannot yet complete.
//
// Separate from CheckKeystoneColumn because it asks a different question:
// that one is about the *relation's* shape (invariant 11), this one about
// whether each column's type is finished. `RowLayout::ColumnWidth` answers
// "how wide", which a type can have before it has an encoding - and a
// column that is sized but not encodable is a column no INSERT can fill.
//
// Checked at CREATE TABLE only. A relation already on disk was accepted by
// the build that created it.
Status CheckDeclarableColumnTypes(const Schema& schema);

// A column with no null bit - every `NOT NULL` column, which is every
// column of a relation that declared nothing (null.md §2.2).
inline constexpr std::uint16_t kNoNullBit = 0xFFFF;

// ---- RowLayout -----------------------------------------------------------
//
// The per-relation row-size constant and its column offsets - invariant 13
// made computable (docs/spec/heap-and-tuple.md section 3.3,
// docs/rules/rule-fixed-length-tuple.md section 2).
//
// **Every tuple is fixed-length.** A relation's tuple layout is a sequence
// of fixed-size cells at offsets computable from the schema *alone*, so the
// row size is a property of the relation, never of the values in a row. A
// RowLayout is that property, computed once when the relation is opened and
// carried on its TableAccess.
//
// It lives here, beside CheckKeystoneColumn(), because it is derived from
// the schema and nothing else. The row codec consumes it; it does not own
// it. That is also what keeps a second, disagreeing copy of "how wide is
// this row" from being computed on an execute path.
//
// Two of Build()'s refusals are new, and both make the same argument: a
// relation whose rows can never be written is refused at CREATE TABLE
// rather than at the first INSERT.
//
//   - **float/decimal columns.** Under a fixed row size the layout has to
//     reserve a width for every column, and neither type has an on-disk
//     encoding yet - CLAUDE.md carries it as an open decision (the KWP
//     `DECIMAL` item, which extends to storage). Reserving a width is half
//     of picking the encoding, so this refuses instead. Before the
//     fixed-length rule they could be declared and simply never populated,
//     which cost nothing because a row's size did not depend on them.
//   - **A row wider than a heap page can hold**
//     (heap::kMaxTuplePayloadSize).
//
// Concurrency: a plain value, built on the catalog path under whatever
// discipline the caller already has, then read-only for the life of the
// TableAccess holding it.
struct RowLayout {
    // The schema constant. Every payload this relation's codec produces is
    // exactly this many bytes, and a stored tuple whose length disagrees is
    // Corruption, never interpreted (invariant 13's "checked redundancy").
    // Includes `null_bitmap_bytes`, which is 0 for an all-NOT NULL schema -
    // the property that makes this feature free for every relation that
    // never asks for it (null.md §2.2's no-migration argument).
    std::uint32_t row_size = 0;

    // Byte offset of each column within the payload, one per schema column
    // and positionally aligned with Schema::columns. offsets[0] is always 0:
    // the Keystone word leads every tuple. **Unchanged by the null bitmap**,
    // which is appended after the last column (null.md §2.1).
    std::vector<std::uint32_t> offsets;

    // The null-bit index of each column, positionally aligned with
    // Schema::columns; kNoNullBit for a NOT NULL column. Bit i lives in
    // bitmap byte i/8 at bit i%8 from the least significant end - explicit
    // shift/mask per invariant 6, in the two helpers below and nowhere
    // else. Derived in Build() from the schema alone, so no execute path
    // ever computes a second, disagreeing notion of "which bit is mine".
    std::vector<std::uint16_t> null_bits;

    // ceil(nullable columns / 8); 0 when none is nullable. The bitmap sits
    // at `row_size - null_bitmap_bytes`, zero-filled meaning all-present -
    // which is exactly what a row written before this feature means.
    std::uint32_t null_bitmap_bytes = 0;

    // The instance-pinned kds.inline_cell_width this layout was built for.
    // Carried so the codec never has to be told twice.
    std::uint32_t inline_cell_width = 0;

    // Computes the layout for `schema` under `inline_cell_width`.
    //
    // Fails with InvalidArgument for whatever CheckKeystoneColumn() rejects
    // or an out-of-range width, and with Unsupported for a float/decimal
    // column or a row wider than a heap page (see above).
    static StatusOr<RowLayout> Build(const Schema& schema, std::uint32_t inline_cell_width);

    // Width of one column's cell under `inline_cell_width`. Fails with
    // Unsupported for float/decimal. Exposed because the row codec checks
    // against it per column, which is what keeps a codec change from
    // silently disagreeing with the layout it is writing into.
    static StatusOr<std::uint32_t> ColumnWidth(const SysColumnRow& col,
                                                std::uint32_t inline_cell_width);
};

// The bitmap is the sole authority on nullness (null.md §3), and these
// two are its only readers and writer - explicit shift and mask, invariant
// 6, with the byte's address computed in exactly one place. `payload` is
// the whole tuple payload of exactly `layout.row_size` bytes; a column with
// kNoNullBit is never NULL by construction, and callers of SetNullBit
// branch on `null_bits[col] != kNoNullBit` before calling - the same field
// the helper reads, so writer and layout cannot disagree.
inline std::size_t NullByteOf(const RowLayout& layout, std::uint16_t bit) noexcept {
    return layout.row_size - layout.null_bitmap_bytes + bit / 8;
}

inline bool NullBitIsSet(std::span<const std::byte> payload, const RowLayout& layout,
                         std::size_t col) noexcept {
    const std::uint16_t bit = layout.null_bits[col];
    if (bit == kNoNullBit) return false;
    return (std::to_integer<std::uint8_t>(payload[NullByteOf(layout, bit)]) >> (bit % 8)) & 1u;
}

inline void SetNullBit(std::span<std::byte> payload, const RowLayout& layout,
                       std::size_t col) noexcept {
    const std::uint16_t bit = layout.null_bits[col];
    payload[NullByteOf(layout, bit)] |= std::byte{static_cast<unsigned char>(1u << (bit % 8))};
}

// True if any column of `schema` can produce a spilled value - i.e. any
// column occupies a tagged cell. Decides whether CREATE TABLE allocates the
// relation a var-heap chain at all, so a relation of plain integers costs
// no var-heap page.
bool SchemaCanSpill(const Schema& schema) noexcept;

// One end of a foreign key, as the relation at the *other* end holds it
// (docs/spec/foreign-keys.md §1). Which end `rel_oid` names depends on
// which list it is in - the parent in `fkeys_out`, the child in `fkeys_in`
// - because a relation reading its own list already knows which side it is
// on, and a field saying so again is a field that can disagree.
struct ForeignKeyRef {
    std::uint64_t fk_id = 0;
    Oid rel_oid = 0;
    std::uint16_t column_no = 0;
    std::uint16_t flags = 0;
};

struct TableAccess {
    Oid namespace_oid;
    Oid oid;
    Schema schema;
    PageId desc_page_id;
    ClusteredType clustered_type;

    // Where a heap relation's chain last placed a tuple - heap::ChainInsert
    // reads it as the tail-search start and writes the landing page back,
    // which is what keeps an insert O(1) pages instead of a head-to-tail
    // walk per row (bench/results-bulk-insert.md's finding). **Advisory
    // and self-healing**: the chain grows only at the tail and a page
    // never leaves it, so a stale value is behind, never wrong, and a
    // damaged one costs one retried walk. Mutable on a cached, const-
    // borrowed entry deliberately - the cache is core-local and statements
    // run to completion, the same license the in-place root updates rely
    // on - and it dies with the entry on BumpVersion, so it never
    // survives DDL. kInvalidPageId for a btree relation, which descends.
    mutable PageId heap_tail_hint = kInvalidPageId;

    // Root of this relation's var-heap chain, or kInvalidPageId when the
    // schema has nothing that could spill. Cacheable for the reason
    // rows.hpp gives: it is fixed at CREATE TABLE and the chain grows
    // through the pages' own links, never by moving the root.
    PageId varheap_page_id = kInvalidPageId;

    // The core that owns this relation (docs/inflight/in-progress/workplan-crosscore.md M1),
    // from sys.tables. Cacheable by this struct's own admission test:
    // ownership is assigned at CREATE and never rebalanced (M3 observes
    // skew and deliberately does not act on it), so it cannot change
    // without DDL.
    //
    // This is what the statement planner reads to pick crosscore.md §2's
    // fast path over the pipeline. Defaulting to 0 is not a placeholder -
    // it is the correct answer on a single-core instance and the system
    // core's id everywhere else.
    std::uint32_t owner_core = 0;

    // Whether an id has ever landed on this relation out of order
    // (well_known.hpp's KeyOrder, docs/spec/heap-and-tuple.md section 4.1), from
    // sys.tables.
    //
    // **The one cached field here that is not a DDL fact**, so it is the one
    // that needed an argument rather than the admission test. It moves at
    // most once in a relation's life - kAscending to kUnordered, never back -
    // and `AdmitExplicitRowId` publishes that single flip through
    // `CatalogCache::MarkKeysUnordered` - an **in-place** update locally,
    // because the flip happens inside a running INSERT that is holding a
    // pointer into this cache - **plus** a version bump and a peer
    // notification, because unlike the index root and the desc page this
    // field is read by a core that does not own the relation. catalog.cpp's
    // note at the flip carries the whole argument.
    //
    // Stale here is a **wrong answer**, not a lost optimization: a cache that
    // says kAscending on an unordered relation lets `ORDER BY <pk>` be
    // discarded, and the walk then emits one page out of key order. That is
    // the reason for the bump, and it is why nothing here defaults the other
    // way as a safety margin - a default that lied in the safe direction on
    // every relation would cost every relation a sort.
    KeyOrder key_order = KeyOrder::kAscending;

    // The relation's anchor page - rows.hpp owns what it is and the
    // system-relation sentinel. Cacheable for varheap_page_id's reason
    // exactly: fixed at CREATE TABLE, and the page's *contents* move so
    // this fact never does. PW2-2 is what starts reading it.
    PageId anchor_page_id = kInvalidPageId;

    // The relation's fixed row size and column offsets (row_layout.hpp),
    // computed once when the entry is filled. It belongs here for the same
    // reason the schema does and by the same test the rest of this struct
    // passes: it is a function of the schema and the instance-pinned
    // inline_cell_width, neither of which can change without DDL.
    RowLayout layout;

    // ---- Cabins on this relation (docs/spec/cabin.md) -------------------
    //
    // Which columns carry a Cabin, and which Cabin each is. Both are DDL
    // facts - `CREATE CABIN` and `DROP CABIN` bump the catalog version -
    // so they pass this struct's admission test, and they are here rather
    // than probed per step because the compiler asks "is this column
    // cabined" once per equality per compile and the answer is one bit.
    //
    // It is deliberately a **positive** fact. Asking the catalog per step
    // would mean caching an *absence* for the common case, which
    // catalog_cache.hpp forbids outright; a mask says which columns do
    // have one and says nothing about the rest.
    //
    // `cabin_mask` holds a bit per `col_pos`. Bit 0 is always clear: the
    // pk's Cabin is the clustered tree itself (spec section 2), so
    // CreateCabin() refuses column 0. A relation wider than 64 columns
    // folds its high columns into no bit - which loses the *acceleration*
    // for those columns and can never lose a row, since an unnoticed
    // Cabin simply means the step compiles to the scan it would have
    // compiled to anyway. Stated here rather than left to be discovered.
    std::uint64_t cabin_mask = 0;

    // One per schema column, positionally aligned with `schema.columns`;
    // `id == 0` - never a real cabin_id - for a column with no Cabin.
    // Parallel to the mask rather than folded into it because the compiler
    // needs only the bit, while the executor needs the id (to find the
    // entry sets) and the origin (to know whether the Cabin was *declared*,
    // which is what decides n=1 versus n=2 - the same rule a declared
    // pattern already gets from `PatternAccess::origin`).
    struct CabinRef {
        std::uint64_t id = 0;
        std::uint8_t origin = kCabinOriginUnset;
    };
    std::vector<CabinRef> cabin_ids;

    // The Cabin on `col_pos`, or a zeroed ref. One accessor, so no caller
    // re-derives the relationship between the mask and the vector.
    CabinRef CabinOn(std::uint16_t col_pos) const noexcept {
        return col_pos < cabin_ids.size() ? cabin_ids[col_pos] : CabinRef{};
    }

    // ---- Foreign keys at both ends (docs/spec/foreign-keys.md §1) -------
    //
    // `fkeys_out` is this relation as the **child**: each entry names the
    // parent it references and the local column holding the reference. The
    // forward check (§2) reads it when a row is inserted or an fk column is
    // updated.
    //
    // `fkeys_in` is this relation as the **parent**: each entry names a
    // child that references it and that child's column. The reverse check
    // (§3) reads it when a row is deleted.
    //
    // Both are DDL facts by the same test the rest of this struct passes -
    // a foreign key is declared at CREATE TABLE and there is no ALTER - so
    // they are cached here and dropped by the same BumpVersion() that drops
    // everything else. Note the direction that makes the bump necessary
    // rather than tidy: creating a *child* changes the **parent's**
    // `fkeys_in`, so an FK-creating DDL stales an entry for a relation it
    // does not name.
    //
    // Neither is consulted per tuple. A write path reads the list once and
    // then works from what it compiled, exactly as §1 requires.
    std::vector<ForeignKeyRef> fkeys_out;
    std::vector<ForeignKeyRef> fkeys_in;

    // The foreign key on `col_pos`, or nullptr. Linear over a list whose
    // length is the number of references a relation declares - single
    // digits - so no index is built for it.
    const ForeignKeyRef* ForeignKeyOn(std::uint16_t col_pos) const noexcept {
        for (const ForeignKeyRef& fk : fkeys_out) {
            if (fk.column_no == col_pos) return &fk;
        }
        return nullptr;
    }

    // ---- Secondary indexes on this relation (docs/spec/index.md) --------
    //
    // One entry per index, everything a compiler or a write hook needs to
    // reach the tree without going back to sys.indexes.
    //
    // **This is the one field on this struct that can move without DDL, and
    // the exception is deliberate rather than overlooked.** A root split
    // during an ordinary INSERT republishes `root_page_id` through
    // `Catalog::UpdateIndexRoot()`, which bumps the catalog version and so
    // drops this entry - meaning a caller **holding a `const TableAccess*`
    // across an index insert that grows a level is holding a dangling
    // pointer**. That is not a new hazard: `desc_page_id` has exactly the
    // same property for a clustered btree, and `InsertInner` handles it by
    // doing the relink last and using only plain ids afterwards, with a
    // comment saying so. The index write hook (IX06) must do the same. The
    // alternative - not caching the root - costs a sys.indexes scan per
    // statement, which is what this whole struct exists to avoid.
    struct IndexRef {
        Oid index_oid = 0;
        PageId root_page_id = kInvalidPageId;
        // The index's schema constants: `key_width` is what
        // exec::EncodeIndexKey produces, `entry_width` the whole leaf entry.
        std::uint16_t key_width = 0;
        std::uint16_t entry_width = 0;
        std::uint8_t nkeys = 0;
        std::uint8_t ncovered = 0;
        // In **declared index order**, which is the order the key encoding
        // concatenates them in - part of the format, not a presentation
        // choice.
        std::array<std::uint16_t, kMaxIndexKeyColumns> key_cols{};
        std::array<std::uint16_t, kMaxIndexCoveredColumns> covered_cols{};

        std::span<const std::uint16_t> keys() const noexcept {
            return std::span<const std::uint16_t>(key_cols.data(), nkeys);
        }
        std::span<const std::uint16_t> covered() const noexcept {
            return std::span<const std::uint16_t>(covered_cols.data(), ncovered);
        }
        // The only column an equality can enter this index by.
        std::uint16_t leading_column() const noexcept { return key_cols[0]; }
    };

    // **Sorted by `index_oid`**, which is creation order. That is not
    // tidiness: spec §9 breaks a tie between two equally-usable indexes by
    // lowest oid, and a plan that depended on scan order would compile the
    // same statement differently as rows moved on the catalog page - which
    // is exactly what a recorded `pattern_id` must not do.
    std::vector<IndexRef> indexes;

    // A bit per **leading** key column, on the same argument `cabin_mask`
    // makes: the compiler asks "could an equality on this column enter an
    // index" once per equality per compile, and the answer is one bit.
    //
    // Leading only, and the distinction is the one `Catalog::
    // FindIndexOnColumn` makes for the same reason: an index on `(a, b)`
    // cannot serve an equality on `b`, so a bit for `b` would stop the
    // compiler calling that step a filter scan while leaving it exactly as
    // slow. Bit 0 is always clear - `CreateIndex` refuses the primary key,
    // whose index is the clustered tree. A relation wider than 64 columns
    // folds its high columns into no bit, which loses the acceleration and
    // never a row.
    std::uint64_t index_mask = 0;

    // The index an equality on `col_pos` may enter, or nullptr. The
    // lowest-oid one when several qualify, matching §9's tie-break.
    //
    // Linear over a list whose length is the number of indexes a relation
    // declares - single digits - so nothing is built for it. A caller that
    // needs the *longest matching prefix* rather than any match walks
    // `indexes` itself; this answers the one-column question.
    const IndexRef* IndexOn(std::uint16_t col_pos) const noexcept {
        for (const IndexRef& ix : indexes) {
            if (ix.nkeys > 0 && ix.leading_column() == col_pos) return &ix;
        }
        return nullptr;
    }
};

// A pattern as the cache holds it (docs/spec/waystone-concpets.md section 4):
// everything about a `sys.patterns` row that DDL alone can change.
//
// **The omissions are the point.** `use_count` and `last_seen` are not
// here, and neither may ever be added: they change on every execution,
// which is not DDL, so a cached copy of them would be stale the moment it
// was taken and there is no invalidation that could fix it. This is
// exactly why TableAccess carries no `next_id` (catalog_cache.hpp), and
// for exactly the same reason - a fact that moves without DDL is not a
// cacheable fact. A caller that wants heat reads the row from the page
// through Catalog::GetSysPatternRow().
//
// What is here divides in three. The identity - `oid`, `pattern_id`,
// `fingerprint_version`, `stmt_class` - is written once at registration
// and never changes. The location - `waystone_root`, `dir_depth` - changes
// only when the directory deepens, through the single writer
// Catalog::SetPatternWaystoneRoot(), which updates this entry in place so
// the cache stays coherent without a global invalidation. The lifecycle
// policy - `origin`, `flags` - changes only when an operator declares or
// adopts a pattern, through the one other in-place writer
// Catalog::SetPatternOrigin(). All three pass this header's test: nothing
// here moves without an explicit DDL-shaped call.
struct PatternAccess {
    Oid oid = 0;
    std::uint64_t pattern_id = 0;
    std::uint32_t fingerprint_version = 0;
    PageId waystone_root = kInvalidPageId;
    std::uint8_t stmt_class = 0;
    std::uint8_t dir_depth = 0;

    // Who created the row (kOriginAuto / kOriginUser) and its policy bits
    // (kPatternPinned), both in rows.hpp. Cached because the trail recorder
    // reads origin on the statement path to decide whether this pattern
    // records from its first execution or waits for n=2, and re-reading the
    // catalog page per execution to answer that would cost more than the
    // recording does.
    std::uint8_t origin = kOriginAuto;
    std::uint16_t flags = 0;

    bool pinned() const noexcept { return (flags & kPatternPinned) != 0; }
    bool user_declared() const noexcept { return origin == kOriginUser; }

    // Same rule as SysPatternRow's: depth is the authority, never the
    // root. Restated rather than shared because a caller holding a
    // PatternAccess has no row to pass to HasWaystoneDirectory().
    bool has_waystone_directory() const noexcept { return dir_depth >= 1; }
};

}  // namespace kds::catalog
