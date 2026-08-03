#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/catalog/well_known.hpp"

// Fixed-layout catalog rows. Each is inserted and read as a single heap
// tuple payload (kds::heap::PageView::InsertTuple()/ReadTuple()), so no
// further serialization layer is needed.
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
    // after the highest tuple is deleted, and an id is the tuple's
    // identity, not a free slot to hand out twice. First id issued is 1,
    // keeping 0 free as "unset".
    std::uint64_t next_id;

    // Root of this relation's var-heap chain (storage/varheap.hpp), or
    // kInvalidPageId for a relation that has no spillable column.
    //
    // Allocated once, at CREATE TABLE, and never changed: the chain grows
    // by tail append through the pages' own links, so the *root* is fixed
    // by DDL. That is deliberate rather than incidental - a root that moved
    // would be a fact changing without DDL, which catalog_cache.hpp's rule
    // says may not be cached, and this one is cached on every TableAccess.
    PageId varheap_page_id;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kNamespaceOidOffset = 8;
    static constexpr std::size_t kNameOffset = 16;
    static constexpr std::size_t kDescPageIdOffset = kNameOffset + kCatalogNameMax;
    static constexpr std::size_t kClusteredTypeOffset = kDescPageIdOffset + sizeof(PageId);
    // uint64 rather than 8-byte-aligned: catalog rows are packed byte
    // streams read through memcpy, never overlaid on the buffer.
    static constexpr std::size_t kNextIdOffset = kClusteredTypeOffset + sizeof(std::uint8_t);
    static constexpr std::size_t kVarHeapPageIdOffset = kNextIdOffset + sizeof(std::uint64_t);
    static constexpr std::size_t kOnDiskSize = kVarHeapPageIdOffset + sizeof(PageId);

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

// ---- sys.patterns ----------------------------------------------------
//
// One row per observed query shape (docs/waystone-concpets.md section 4).
// `pattern_id` is the lookup key, not `oid`: callers arrive holding a
// fingerprint computed at parse (parser/fingerprint.hpp) and never an oid,
// which is the reverse of every other catalog relation here.
//
// The row carries three separable things, and it is worth naming them
// because they change at different rates: the pattern's *identity*
// (`pattern_id` + `fingerprint_version`), the *location* of its waystones
// (`waystone_root` + `dir_depth`), and its *heat* (`use_count`,
// `last_seen`). Only the first is stable; the second changes when the
// directory deepens, and the third on every execution.
//
// Field order below is by descending alignment so that the on-disk offsets
// and the struct's own offsets coincide, which is what lets every field
// carry an offsetof static_assert. SysTableRow gave that up past `next_id`
// and has to be read more carefully as a result; there was no reason to
// repeat it in a new row.
struct SysPatternRow {
    Oid oid;

    // The parse-time fingerprint of the statement's shape. Unique across
    // live rows, and the key every lookup arrives with.
    std::uint64_t pattern_id;

    // Truncated logical timestamp of the last execution observed.
    // Best-effort, like use_count: it feeds retention, and nothing may
    // depend on it being exact.
    std::uint64_t last_seen;

    // Which revision of the fingerprinting algorithm produced
    // `pattern_id` (parser/fingerprint.hpp kFingerprintVersion). A row
    // whose version is not the running build's is ignored - its hash names
    // a shape computed under different rules - and that is a miss, never
    // an error. 0 means unset, which a zeroed page decodes to and which is
    // never current.
    std::uint32_t fingerprint_version;

    // Root of this pattern's arg_hash directory (spec section 5).
    // **Meaningful only when dir_depth >= 1** - see the note there.
    // Written together with dir_depth and never separately: a root without
    // its depth is unwalkable, and a depth that disagrees with the root
    // sends every walk to the wrong leaf.
    PageId waystone_root;

    // Executions observed. Best-effort - events may be dropped under
    // pressure - and it exists to rank patterns for retention, not to
    // count anything anyone reports.
    std::uint32_t use_count;

    // The parser's execution-class tag for this shape (docs/parser.md I2).
    // **Deliberately a raw byte, not an enum.** The v1 class list is
    // `[PROPOSED]` and its ratification is an open decision in CLAUDE.md;
    // defining the enum here would be deciding it. The field exists now
    // because this is an on-disk format and adding one later is a format
    // break, and 0 means "unclassified" so a row written before the parser
    // can classify anything is honest rather than wrong.
    std::uint8_t stmt_class;

    // Levels the directory walk traverses, and **the authority on whether
    // a directory exists at all**: 0 means none, and any real directory
    // has depth >= 1.
    //
    // Depth rather than `waystone_root == kInvalidPageId` carries that
    // fact on purpose. A row read out of a zeroed or never-written page
    // decodes every field to 0, which would make its root look like page
    // 0 - a valid-looking PageId, and one that happens to be the
    // superblock. Keying "no directory" on the field whose zero value
    // already means it leaves no way to spell the state wrong. Writers
    // should still store kInvalidPageId when clearing a directory, but no
    // reader may depend on it; `HasWaystoneDirectory()` below is the only
    // test any of them should use.
    //
    // Persisted rather than derived, for the reason SysTableRow's deleted
    // equivalent recorded - a derived depth changes the instant the key
    // space crosses a coverage boundary, which is before the root is
    // relinked, and every walk in that window lands on the wrong leaf.
    std::uint8_t dir_depth;

    // Policy bits. Today only kPatternPinned, which says what retention may
    // do to this pattern's waystones
    // (docs/spec-create-pattern-user-defined-patterns-v1.md section 4.1).
    std::uint16_t flags;

    // Who created this row: kOriginAuto or kOriginUser.
    //
    // **Separate from `flags` on purpose.** Origin is provenance and
    // pinning is policy, and they move independently: an operator may pin
    // an auto-registered pattern without re-declaring it, and a declared
    // pattern may be created unpinned. Folding pinning into origin would
    // make both of those unspellable.
    //
    // The field order below is not cosmetic. `flags` (u16) sits before
    // `origin` (u8) because `dir_depth` ends the row at 37: a u16 next pads
    // the struct to 38 and the u8 after it lands at 40, so both keep the
    // offsetof static_assert this file's layout rule depends on. Reversing
    // them puts the u16 at struct offset 40 against an on-disk 39 and the
    // assert stops holding.
    std::uint8_t origin;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kPatternIdOffset = 8;
    static constexpr std::size_t kLastSeenOffset = 16;
    static constexpr std::size_t kFingerprintVersionOffset = 24;
    static constexpr std::size_t kWaystoneRootOffset = 28;
    static constexpr std::size_t kUseCountOffset = 32;
    static constexpr std::size_t kStmtClassOffset = 36;
    static constexpr std::size_t kDirDepthOffset = 37;
    static constexpr std::size_t kFlagsOffset = 38;
    static constexpr std::size_t kOriginOffset = 40;
    static constexpr std::size_t kOnDiskSize = kOriginOffset + sizeof(std::uint8_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysPatternRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysPatternRow, oid) == SysPatternRow::kOidOffset);
static_assert(offsetof(SysPatternRow, pattern_id) == SysPatternRow::kPatternIdOffset);
static_assert(offsetof(SysPatternRow, last_seen) == SysPatternRow::kLastSeenOffset);
static_assert(offsetof(SysPatternRow, fingerprint_version) ==
              SysPatternRow::kFingerprintVersionOffset);
static_assert(offsetof(SysPatternRow, waystone_root) == SysPatternRow::kWaystoneRootOffset);
static_assert(offsetof(SysPatternRow, use_count) == SysPatternRow::kUseCountOffset);
static_assert(offsetof(SysPatternRow, stmt_class) == SysPatternRow::kStmtClassOffset);
static_assert(offsetof(SysPatternRow, dir_depth) == SysPatternRow::kDirDepthOffset);
static_assert(offsetof(SysPatternRow, flags) == SysPatternRow::kFlagsOffset);
static_assert(offsetof(SysPatternRow, origin) == SysPatternRow::kOriginOffset);
static_assert(SysPatternRow::kOnDiskSize == 41);

// Who wrote a sys.patterns row. Persisted, so the numbers are frozen.
//
// The distinction is lifecycle policy, never the trust model: a replayed
// entry is validated identically whatever its origin, for the same reason
// the engine keeps one evaluator and one step-kind table. What origin
// decides is recording probation (a declaration *is* the evidence n=2 waits
// for, so a user pattern records from its first execution) and what a
// fingerprint version bump does to the row - an auto row holds only a hash
// and retires, a user row re-fingerprints from its stored source text.
inline constexpr std::uint8_t kOriginAuto = 0;
inline constexpr std::uint8_t kOriginUser = 1;

// `flags` bit 0: retention may not evict this pattern's waystones.
//
// A policy promise, not a correctness requirement - invariant 8 still holds
// for a pinned pattern, so a manual purge remains legal and costs
// performance rather than answers.
inline constexpr std::uint16_t kPatternPinned = 0x1;

// The class value a row carries when nothing has classified the statement
// - which is every row until the parser gains its class tags. Named so no
// call site writes a bare 0 and leaves the next reader guessing whether it
// meant "unclassified" or "the first class in some list".
inline constexpr std::uint8_t kStmtClassUnclassified = 0;

// Deepest directory a pattern's waystones can need, and therefore the
// largest value `dir_depth` may hold.
//
// Derived, not chosen: the directory is walked by digits of a 64-bit
// `arg_hash` at a fanout of 2048 = 2^11 per level (spec section 5), so
// ceil(64 / 11) = 6 levels address the whole key space. A seventh could
// not be reached by any key. P07 owns the walk and may need *fewer* levels
// than this; it cannot need more.
inline constexpr std::uint8_t kMaxPatternDirDepth = 6;
static_assert(kMaxPatternDirDepth * 11 >= 64, "the depth bound must cover a 64-bit arg_hash");

// Whether this pattern has a waystone directory to walk. The one test for
// it: `dir_depth`, never the root, for the reason recorded at that field.
// A zeroed row answers false, which is the whole point.
constexpr bool HasWaystoneDirectory(const SysPatternRow& row) noexcept {
    return row.dir_depth >= 1;
}

}  // namespace kds::catalog
