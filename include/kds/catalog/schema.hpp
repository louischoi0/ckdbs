#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/catalog/rows.hpp"

namespace kds::exec {
// An expression or predicate Cabin's compiled key function
// (`exec/cabin_expr.hpp`), held by `TableAccess::ExprCabinRef`.
struct CompiledCabinExpr;
}  // namespace kds::exec

// In-memory schema, built from the sys.columns rows for a given rel_id,
// plus the per-relation access handles the cache hands out.

namespace kds::catalog {

struct Schema {
    // Every physical column in `pos` order, a dropped one included: the
    // vector index is the position every index, foreign key, Cabin,
    // Transition and assertion stores (BW-R6).
    std::vector<SysColumnRow> columns;

    // The relation's reserved 16-byte slots, from its `sys.tables` row
    // (BW-R2). Part of the schema because the row size is derived from the
    // schema alone (RowLayout below).
    std::uint16_t spare_slots = 0;

    // The live column named `name`, or nullptr; a dropped column has no name
    // a statement can reach (BW-R6).
    const SysColumnRow* FindColumn(std::string_view name) const noexcept;
};

// One reserved column slot's width (BW-R2, D-a): the narrowest tagged cell
// (`storage::CheckInlineCellWidth`'s floor), so `varchar(16)` fits one and
// so does every fixed-width type but a wide `char`.
inline constexpr std::uint32_t kColumnSlotWidth = 16;

// `CREATE TABLE ... SPARE n`'s ceiling (BW-R1): 64 keeps every slot of a
// relation of up to 64 declared columns inside `cabin_mask`/`index_mask`.
inline constexpr std::uint16_t kMaxSpareSlots = 64;

// True for the integer types a Keystone id can be declared as. Float,
// decimal, bool, char and varchar are not among them: none can represent a
// Keystone id, and a tuple key that cannot hold its own value is a table no row can
// be written to.
bool IsIntegerTypeVal(std::uint32_t type_val) noexcept;

// Rejects a schema that cannot carry a Keystone word: no columns at all,
// or a first column whose declared type is not an integer one. Every
// relation's first column is its system-generated tuple key
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

    // One past each column's last byte: `offsets[i] + ColumnWidth`, aligned
    // with `offsets`. A cell is exactly its column's width, never "up to the
    // next offset": with slots (BW-R2) the last declared column is followed
    // by the slot region and a slot column by vacant slots, and a char or
    // tagged cell sized by its span would take those bytes as its own.
    std::vector<std::uint32_t> cell_ends;

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

    // The null bits of the slots no column holds yet, `[begin, end)`
    // (BW-R3). Slots are taken lowest first and a dropped column keeps its
    // slot, so the vacant ones are always the suffix. `EncodeRow` sets these
    // bits in every row it builds, so a column a later `ADD` places in one
    // reads NULL in every row that predates it. **`[quiet-wrong]`**: without
    // them a vacant slot's zero bytes would read as a present 0.
    std::uint16_t vacant_slot_bit_begin = 0;
    std::uint16_t vacant_slot_bit_end = 0;

    // The columns a statement can see, the tuple key included: every column but a
    // dropped one (BW-R6). Positional `VALUES` count these.
    std::uint32_t live_columns = 0;

    // Whether any column is dropped - the codec's gate, as
    // `null_bitmap_bytes` is the bitmap's: a relation with no dropped column
    // asks no column whether it is. A dropped column's cell is written zero
    // and never interpreted - its bytes may be a NOT NULL varchar's zero
    // `kNull` tag, which a decode would otherwise refuse.
    bool has_dropped() const noexcept { return live_columns != offsets.size(); }

    // The slots a column holds, a dropped one's included (BW-R2).
    std::uint16_t slots_held = 0;

    // Whether the relation's column set has changed since `CREATE TABLE` - a
    // column added or dropped. **`[quiet-wrong]`** (BW-Q9): on such a
    // relation a positional row carries every live column, the tuple key `NULL` to
    // have it issued, because after an `ADD` the short form's length is that
    // of a row written for the old columns with its key named, and admitting
    // it would shift every value one column over. A `DROP` leaves a case no
    // length catches (BW-Q11, `known-gaps.md`).
    bool column_set_changed() const noexcept { return has_dropped() || slots_held != 0; }

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
// four are its only reader and writers - explicit shift and mask, invariant
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

// The bit cleared, for a column re-encoded inside a copied row
// (`exec::EncodeAssigned`, BN-R3 of `workorder-bn-opt-read-update-path.md`);
// a column with kNoNullBit has none to clear.
inline void ClearNullBit(std::span<std::byte> payload, const RowLayout& layout,
                         std::size_t col) noexcept {
    const std::uint16_t bit = layout.null_bits[col];
    if (bit == kNoNullBit) return;
    payload[NullByteOf(layout, bit)] &= ~std::byte{static_cast<unsigned char>(1u << (bit % 8))};
}

// Every vacant slot's bit set (BW-R3): a slot no column holds is NULL in
// every row, so the column a later `ADD COLUMN` places there reads NULL in
// the rows that predate it. `EncodeRow` calls it on every row it builds;
// `EncodeAssigned` copies the bits with the before-image.
inline void SetVacantSlotBits(std::span<std::byte> payload, const RowLayout& layout) noexcept {
    for (std::uint16_t bit = layout.vacant_slot_bit_begin; bit < layout.vacant_slot_bit_end;
         ++bit) {
        payload[NullByteOf(layout, bit)] |= std::byte{static_cast<unsigned char>(1u << (bit % 8))};
    }
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

// One Transition on a relation, parsed from its `sys.transitions` row
// (`exec/transition.hpp` builds it, BV-S2). Every state is held as its
// **state key**: a string column's value as its bytes, an integer column's
// as its decimal spelling - one canonical spelling per value, so a check is
// a comparison of two strings whatever the column's type.
struct TransitionRef {
    std::uint64_t id = 0;
    std::string name;
    std::uint16_t column_no = 0;
    std::vector<std::string> initial;
    std::vector<std::pair<std::string, std::string>> edges;
    // Absent `ON DELETE FROM`: every delete is allowed (CN-4 T10).
    bool delete_declared = false;
    std::vector<std::string> deletable;
};

struct TableAccess {
    // Filled from the `sys.tables` row. Its one reader is
    // `Catalog::CheckRelationQualifier`, which asks whether a qualified
    // name's namespace is the relation's own.
    Oid namespace_oid;
    Oid oid;
    Schema schema;
    PageId desc_page_id;
    ClusteredType clustered_type;

    // Where a heap relation's chain last placed a tuple - heap::ChainInsert
    // reads it as the tail-search start and writes the landing page back,
    // which is what keeps an insert O(1) pages instead of a head-to-tail
    // walk per row (the BLK08 finding). **Advisory
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

    // **No `owner_core`** (AT-S9, D17): nothing reads a relation's core,
    // because every read and write runs where its session is and every page
    // is every core's to fault.

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
    // tuple key's Cabin is the clustered tree itself (spec section 2), so
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

    // The relation's **expression and predicate** Cabins (AQ-R3): a list
    // beside the column map, which keeps the column shape's path byte for
    // byte. Each carries its compiled key function `F`, filled by the
    // declaration loader from `sys.cabin_exprs` - `exec/`'s, which is why
    // `F` is a pointer to a type this layer only names. A relation with
    // none pays one `empty()` test on the write path.
    struct ExprCabinRef {
        std::uint64_t id = 0;
        std::uint8_t origin = kCabinOriginUnset;
        std::uint8_t shape = kCabinShapeUnset;
        std::uint64_t expr_id = 0;
        std::shared_ptr<const exec::CompiledCabinExpr> f;
    };
    std::vector<ExprCabinRef> expr_cabins;

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

    // ---- Transitions on this relation (BV-S2) ---------------------------
    //
    // At most one per column (CN-4 T6). A DDL fact by the foreign key's
    // test, cached here and dropped by the schema word. **The fence is the
    // memo** (BV-R4): `CREATE TRANSITION` writes its row under the relation
    // `X` and moves the word before release, so a writer's first relation
    // intention (`MemoIsCurrent`) re-runs a statement compiled against an
    // entry without it. Filled by the catalog's transition loader; a fill
    // that cannot read sys.transitions fails, as one that cannot read
    // sys.fkeys does - a constraint the write path cannot see does not run.
    std::vector<TransitionRef> transitions;

    // The Transition on `col_pos`, or nullptr.
    const TransitionRef* TransitionOn(std::uint16_t col_pos) const noexcept {
        for (const TransitionRef& t : transitions) {
            if (t.column_no == col_pos) return &t;
        }
        return nullptr;
    }

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
    // `Catalog::AdoptRoot()`, which writes the anchor and updates this field
    // **in place** (PW2-4), so the running statement's `const TableAccess*`
    // survives - the same one-field/one-owner license as the other in-place
    // updates (catalog_cache.hpp). What remains is that `root_page_id` read
    // before an index insert that grows a level is behind afterwards, which
    // is why `InsertInner` does its relink last and uses only plain ids
    // after it, and why the index write hook must do the same. `desc_page_id` has exactly the same property for a
    // clustered btree and takes the same treatment. The alternative - not
    // caching the root - costs a sys.indexes scan per statement, which is
    // what this whole struct exists to avoid.
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
    // slow. Bit 0 is always clear - `CreateIndex` refuses the tuple key,
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
// here, and neither may ever be added: they are heat, which moved on every
// execution until BL-S4's ruling 1 retired its writer (rows.hpp), and a
// fact that moves without DDL would be stale in a cache the moment it was
// taken, with no invalidation that could fix it. This is exactly why
// TableAccess carries no `next_id` (catalog_cache.hpp), and for exactly
// the same reason - a fact that moves without DDL is not a
// cacheable fact. A caller that wants heat reads the row from the page
// through Catalog::GetSysPatternRow().
//
// What is here divides in three. The identity - `oid`, `fetch_id`,
// `fingerprint_version`, `stmt_class` - is written once at registration
// and never changes. The location - `waystone_root`, `dir_depth` - changes
// only when the directory deepens, through the single writer
// Catalog::ClaimPatternWaystoneRoot(), which updates this entry in place so
// the cache stays coherent without a global invalidation. The lifecycle
// policy - `origin`, `flags` - has **no** writer since declared patterns
// were withdrawn on 2026-08-31: every registration passes kOriginAuto and
// nothing sets kPatternPinned. All three still pass this header's test:
// nothing here moves without an explicit DDL-shaped call.
struct PatternAccess {
    Oid oid = 0;
    std::uint64_t fetch_id = 0;
    std::uint32_t fingerprint_version = 0;
    PageId waystone_root = kInvalidPageId;
    std::uint8_t stmt_class = 0;
    std::uint8_t dir_depth = 0;

    // Who created the row (kOriginAuto / kOriginUser) and its policy bits
    // (kPatternPinned), both in rows.hpp. Cached because the trail recorder
    // reads origin on the statement path, and re-reading the catalog page
    // per execution to answer that would cost more than the recording does.
    //
    // Both read constant today - kOriginAuto and 0 - because withdrawing
    // declared patterns took away their only writer. They are cached rather
    // than dropped for the reason rows.hpp keeps them on disk: the fields
    // are the row's, not this feature's.
    std::uint8_t origin = kOriginAuto;
    std::uint16_t flags = 0;

    // Same rule as SysPatternRow's: depth is the authority, never the
    // root. Restated rather than shared because a caller holding a
    // PatternAccess has no row to pass to HasWaystoneDirectory().
    bool has_waystone_directory() const noexcept { return dir_depth >= 1; }
};

}  // namespace kds::catalog
