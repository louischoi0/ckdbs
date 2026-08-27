#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

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

    // The core that owns this relation (docs/inflight/in-progress/workplan-crosscore.md M1).
    //
    // **Ownership is a catalog fact and nothing else.** No code may derive
    // it from a page id, a hash, or the topology - that is workplan
    // guideline 4, and the reason is that page/extent hashing was rejected
    // outright: a btree descent and a heap-chain walk cannot cross cores
    // per hop, so ownership has to be per relation and it has to be
    // recorded.
    //
    // What this field does *not* need to say is where the relation's
    // auxiliaries live. Unique indexes, the Cabin, the Waystone pages and
    // the var-heap are write-coupled and therefore always co-located
    // (crosscore.md section 6) - and they are co-located structurally,
    // because they hang off this same row rather than carrying an owner of
    // their own. There is no way to spell a relation whose var-heap is
    // somewhere else.
    //
    // Assigned once, at CREATE, by catalog/core_placement.hpp; never
    // rebalanced (M3 observes skew and does not act on it), which is what
    // makes it cacheable on TableAccess - a fact that cannot change without
    // DDL.
    std::uint32_t owner_core;

    // Whether an id has ever landed here out of order (well_known.hpp's
    // KeyOrder, docs/spec/heap-and-tuple.md section 4.1).
    //
    // **Unlike every other field on this row it is not a DDL fact**, and it
    // is the one thing TableAccess caches that a plain INSERT can move: the
    // first below-the-mark id flips it and bumps the catalog version, so a
    // cached access whose flag reads stale is refreshed by the ordinary
    // invalidation. Reading it stale would cost a discarded sort, never a
    // wrong answer.
    //
    // It occupies the byte the `KeyMode` enum held until 2026-08-25, at the
    // same offset and the same width, with kAssigned's 0 and kExplicit's 1
    // carrying over as kAscending and kUnordered. `kOnDiskSize` therefore did
    // not move and no format bump came with the key mode's removal - a file
    // written before it mounts and means exactly what it meant.
    KeyOrder key_order;

    // The relation's anchor page (storage/anchor_page.hpp; PW2-1,
    // workplan-peer-writer.md §7a), or kInvalidPageId for a **system**
    // relation - a bootstrap table lives on a fixed catalog page whose
    // root never moves, so it carries no anchor and PW2-2 reads
    // kInvalidPageId as "desc_page_id is the root". Every user relation
    // gets one at CREATE TABLE, and the field never changes after: the
    // anchor is the page whose *contents* move so this row never has to.
    // The four bytes are a format-version event (superblock 14 -> 15;
    // Decode refuses any size but the exact one - the key-order byte's precedent).
    // Appended after `key_order` because every offset below is a fixed
    // on-disk position.
    PageId anchor_page_id;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kNamespaceOidOffset = 8;
    static constexpr std::size_t kNameOffset = 16;
    static constexpr std::size_t kDescPageIdOffset = kNameOffset + kCatalogNameMax;
    static constexpr std::size_t kClusteredTypeOffset = kDescPageIdOffset + sizeof(PageId);
    // uint64 rather than 8-byte-aligned: catalog rows are packed byte
    // streams read through memcpy, never overlaid on the buffer.
    static constexpr std::size_t kNextIdOffset = kClusteredTypeOffset + sizeof(std::uint8_t);
    static constexpr std::size_t kVarHeapPageIdOffset = kNextIdOffset + sizeof(std::uint64_t);
    static constexpr std::size_t kOwnerCoreOffset = kVarHeapPageIdOffset + sizeof(PageId);
    static constexpr std::size_t kKeyOrderOffset = kOwnerCoreOffset + sizeof(std::uint32_t);
    static constexpr std::size_t kAnchorPageIdOffset = kKeyOrderOffset + sizeof(std::uint8_t);
    static constexpr std::size_t kOnDiskSize = kAnchorPageIdOffset + sizeof(PageId);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysTableRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysTableRow, oid) == SysTableRow::kOidOffset);
static_assert(offsetof(SysTableRow, namespace_oid) == SysTableRow::kNamespaceOidOffset);
static_assert(offsetof(SysTableRow, name) == SysTableRow::kNameOffset);
static_assert(offsetof(SysTableRow, desc_page_id) == SysTableRow::kDescPageIdOffset);
static_assert(offsetof(SysTableRow, clustered_type) == SysTableRow::kClusteredTypeOffset);
// `key_order` gets no offsetof assert, for the same reason `next_id`,
// `varheap_page_id` and `owner_core` have none: everything from `next_id`
// on sits behind the compiler's alignment padding, so its in-memory offset
// and its on-disk offset are different numbers by design. Encode/Decode
// address the buffer through the constants above, field by field, which is
// what makes that difference harmless - this file's discipline, stated at
// the top.

// The first id any relation issues. Zero stays reserved for "no id".
inline constexpr std::uint64_t kFirstRowId = 1;

// ---- sys.columns ----------------------------------------------------------

struct SysColumnRow {
    Oid oid;
    Oid rel_id;
    std::uint32_t pos;
    Name name;
    std::uint32_t type_val;

    // What `len` means depends on the type, and only two types read it.
    //
    //   `char`     the declared width, which is what it has always meant.
    //   `decimal`  the packed **(precision, scale)** pair - see
    //              `PackDecimalLen` below.
    //   anything else
    //              the type's width, stored and **never consulted**:
    //              `RowLayout::ColumnWidth` derives every other width from
    //              `type_val` alone.
    //
    // The decimal reading is an **overload of an existing field, chosen
    // over widening this row** (docs/spec/types.md TY9, workplan TY02).
    // Widening it is a data-file format change: `SysColumnRow` has no spare
    // byte, so `(p, s)` would have cost a superblock version bump and every
    // pre-existing data file would stop mounting - which is what the last
    // four bootstrap-relation additions each cost. `len` was already dead
    // weight for a decimal column, so the pair rides in it for free.
    //
    // The price, stated so nobody has to find it: `len` is no longer
    // readable as "a width" without knowing the type. Two display paths
    // consulted it that way - `sys.columns` and `DESCRIBE` - and both now
    // render the type instead.
    std::uint32_t len;

    // Defaulted true, which is D1's ratified semantics (`workplan-null.md`):
    // a column is NOT NULL unless declared otherwise - so a hand-built row
    // that never thinks about nullness means what every column meant before
    // the feature existed, instead of silently growing a bitmap. A member
    // default changes no layout and no offset; the static_asserts below
    // still hold.
    bool notnull = true;

    // Whether this column may carry a Cabin, and on whose initiative
    // (docs/spec/cabin.md). One of the kCabinPolicy* values below.
    //
    // **A schema property, not a Cabin's property.** A Cabin can be created
    // and dropped; the policy says what is *permitted* for this column and
    // outlives any particular Cabin on it - which is why it belongs here and
    // not on the `sys.cabins` row.
    std::uint8_t cabin_policy;

    static constexpr std::size_t kOidOffset = 0;
    static constexpr std::size_t kRelIdOffset = 8;
    static constexpr std::size_t kPosOffset = 16;
    static constexpr std::size_t kNameOffset = 20;
    static constexpr std::size_t kTypeValOffset = kNameOffset + kCatalogNameMax;
    static constexpr std::size_t kLenOffset = kTypeValOffset + sizeof(std::uint32_t);
    static constexpr std::size_t kNotNullOffset = kLenOffset + sizeof(std::uint32_t);
    static constexpr std::size_t kCabinPolicyOffset = kNotNullOffset + sizeof(std::uint8_t);
    static constexpr std::size_t kOnDiskSize = kCabinPolicyOffset + sizeof(std::uint8_t);

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
static_assert(offsetof(SysColumnRow, cabin_policy) == SysColumnRow::kCabinPolicyOffset);

// ---- decimal(p, s) packed into `len` (docs/spec/types.md TY9) ----------
//
// Explicit shift and mask, never a compiler bitfield: invariant 6 forbids
// one for any persisted format, because their layout is
// implementation-defined and this engine must be architecture-portable.
//
// Precision in the high byte, scale in the low one. Both are bounded well
// under 255 by TY2 (1 <= p <= 18, 0 <= s <= p), so sixteen of `len`'s
// thirty-two bits are used and the rest stay zero and available.
inline constexpr std::uint32_t PackDecimalLen(std::uint8_t precision,
                                              std::uint8_t scale) noexcept {
    return (static_cast<std::uint32_t>(precision) << 8) | static_cast<std::uint32_t>(scale);
}

inline constexpr std::uint8_t DecimalPrecisionOf(std::uint32_t len) noexcept {
    return static_cast<std::uint8_t>((len >> 8) & 0xFF);
}

inline constexpr std::uint8_t DecimalScaleOf(std::uint32_t len) noexcept {
    return static_cast<std::uint8_t>(len & 0xFF);
}

// How a column's declared type reads back to a client: `int64`, `varchar`,
// `char(8)`, `decimal(10,2)`, `date`. `base_name` is the `sys.types` name
// for the column's `type_val`, which the caller has already resolved.
//
// Here rather than at each display site because `len`'s meaning is this
// header's to know, and two callers rendering it differently is how a
// `DESCRIBE` and a `sys.columns` come to disagree about one column.
std::string ColumnTypeText(const SysColumnRow& col, std::string_view base_name);

// ---- Per-column cabin policy (docs/spec/cabin.md §8) -------------------
//
// Declared at `CREATE TABLE`, per column, and fixed for the relation's life
// (there is no `ALTER TABLE`). It answers one question: **who is allowed to
// decide that this column carries a Cabin?**
//
// The three answers are deliberately not "on/off". A Cabin is a standing
// cost - a directory probe on every write to the relation - paid against a
// benefit that depends on the workload, so the useful axis is *who judges*:
// the operator, the engine, or nobody.
inline constexpr std::uint8_t kCabinPolicyUnset = 0;

// **Disabled.** No Cabin on this column, ever, by any route: `CREATE CABIN`
// is refused and auto-creation will never consider it. For a column an
// operator knows is never filtered by equality, or one whose write rate
// makes the hook a bad trade at any hit rate.
inline constexpr std::uint8_t kCabinPolicyDisabled = 1;

// **Auto.** The engine may create a Cabin on this column when its own
// signals say the column has earned one - Waystone's per-instance
// `use_count` and the recording scan's measured cardinality, the promotion
// pipeline of §7: `cold → trail (advisory, free) → Cabin (authoritative,
// earns its write hook)`.
//
// **Not implemented.** No code creates a Cabin on this policy, and the value
// exists so that the decision has a name and a stored representation before
// the pipeline that consumes it. A column declared auto today behaves
// exactly as an undeclared one: no Cabin until someone writes
// `CREATE CABIN`. The threshold itself is `[OPEN]` and belongs to the
// retention/policy spec alongside tracking levels.
inline constexpr std::uint8_t kCabinPolicyAuto = 2;

// **Enabled.** A Cabin is created on this column at `CREATE TABLE`, and its
// values are observed **on first selection** rather than on second.
//
// The n=1 half mirrors the rule `CREATE PATTERN` already settled
// (`TrailRecorder`: n=1 for a declared pattern, n=2 for an auto-observed
// one) and rests on the same argument: a declaration *is* the evidence that
// waiting exists to gather. An operator who wrote `CABIN` on the column has
// already said it is probed by value, and making them prove it with traffic
// asks a question that was answered.
inline constexpr std::uint8_t kCabinPolicyEnabled = 3;

// The policy a column carries when nothing said otherwise - including every
// row written before this field existed, which decodes to 0.
//
// `kCabinPolicyUnset` is treated as `kCabinPolicyAuto` by every reader, and
// the mapping is here so no caller invents its own. The two are stored
// distinctly because they are different statements - "nothing was said" and
// "the engine may decide" - and the day auto-creation exists, an operator
// will want to know which of the two a column carries.
constexpr std::uint8_t EffectiveCabinPolicy(std::uint8_t stored) noexcept {
    return stored == kCabinPolicyUnset ? kCabinPolicyAuto : stored;
}

// Whether a Cabin may be created on a column carrying this policy, by an
// operator asking for one. The single test, so `CREATE CABIN` and any
// future auto-creation cannot come to disagree about what disabled means.
constexpr bool CabinPolicyPermitsCreation(std::uint8_t stored) noexcept {
    return EffectiveCabinPolicy(stored) != kCabinPolicyDisabled;
}

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
//
// One row per secondary index (docs/spec/index.md §12, workplan IX03).
//
// The row this replaces described a **single-column** index: one `col_pos`,
// no root page, no name. Nothing ever wrote it - `HasUnindexedEqualityFilter`
// was its only reader and every answer was "unindexed" - so widening it costs
// no migration of live data. What it does cost is a superblock version bump,
// for a reason worth stating exactly because it is *not* the reason the four
// previous bumps had: no pre-existing file misparses anything here, since no
// pre-existing file has an index row at all. The bump protects the other
// direction - an **older binary opening a newer file**, which would find rows
// of a size its `Decode` rejects and fail on every SELECT compile rather than
// refusing to mount. That refusal is what a version is for.

// A **cap refuses, never truncates** (spec §11): an index declared with more
// columns than fit is rejected at CREATE INDEX, because a truncated index
// marked complete is a wrong answer with a right answer's shape. Both are
// `[PROPOSED]`, chosen to keep the row inside one catalog slot; nothing may
// depend on either number, only on the rule.
inline constexpr std::size_t kMaxIndexKeyColumns = 4;
inline constexpr std::size_t kMaxIndexCoveredColumns = 8;

// Defined and never written (spec IX11). v1 is a read accelerator that
// cannot fail a write for a reason of its own, so `UNIQUE` is refused at
// declaration; the bit exists so turning it on later is a code change and
// not a format event, exactly as `UndoRecordType::kInsert` does.
inline constexpr std::uint8_t kIndexFlagUnique = 0x1;

struct SysIndexRow {
    // From AllocateRowId(kSysIndexesTable) - this relation's own persistent
    // sequence, like a cabin_id or an fk_id, and not GenerateUserOid(),
    // which counts objects rather than rows of this relation.
    Oid index_oid;
    Oid table_oid;

    // The index tree's root (storage/index/index_tree.hpp). Allocated
    // eagerly at CREATE INDEX and **never moved by growth** - a root split
    // publishes a new root here through this row, which is why the id may
    // live on a cached TableAccess at all (catalog_cache.hpp's rule).
    PageId root_page_id;

    // The index's schema constants, in the sense RowLayout::row_size is a
    // relation's: `key_width` from exec::IndexKeyWidth over the key columns,
    // `entry_width` the whole leaf entry. Stored rather than recomputed so
    // every page the tree touches can be **checked** against them, and a
    // page that disagrees is Corruption rather than a reinterpretation.
    std::uint16_t key_width;
    std::uint16_t entry_width;

    Name name;

    std::uint8_t nkeys;
    std::uint8_t ncovered;
    std::uint8_t flags;
    std::uint8_t reserved0;

    // Positions into the relation's schema, in **declared index order** -
    // which is the order the key encoding concatenates them in, so it is
    // part of the format and not a presentation detail. Entries past
    // `nkeys`/`ncovered` are written 0 and never read.
    std::array<std::uint16_t, kMaxIndexKeyColumns> key_cols;
    std::array<std::uint16_t, kMaxIndexCoveredColumns> covered_cols;

    static constexpr std::size_t kIndexOidOffset = 0;
    static constexpr std::size_t kTableOidOffset = 8;
    static constexpr std::size_t kRootPageIdOffset = 16;
    static constexpr std::size_t kKeyWidthOffset = 20;
    static constexpr std::size_t kEntryWidthOffset = 22;
    static constexpr std::size_t kNameOffset = 24;
    static constexpr std::size_t kNkeysOffset = kNameOffset + kCatalogNameMax;      // 88
    static constexpr std::size_t kNcoveredOffset = kNkeysOffset + 1;                // 89
    static constexpr std::size_t kFlagsOffset = kNcoveredOffset + 1;                // 90
    static constexpr std::size_t kReserved0Offset = kFlagsOffset + 1;               // 91
    static constexpr std::size_t kKeyColsOffset = kReserved0Offset + 1;             // 92
    static constexpr std::size_t kCoveredColsOffset =
        kKeyColsOffset + kMaxIndexKeyColumns * sizeof(std::uint16_t);               // 100
    static constexpr std::size_t kOnDiskSize =
        kCoveredColsOffset + kMaxIndexCoveredColumns * sizeof(std::uint16_t);       // 116

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysIndexRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysIndexRow, index_oid) == SysIndexRow::kIndexOidOffset);
static_assert(offsetof(SysIndexRow, table_oid) == SysIndexRow::kTableOidOffset);
static_assert(offsetof(SysIndexRow, root_page_id) == SysIndexRow::kRootPageIdOffset);
static_assert(offsetof(SysIndexRow, key_width) == SysIndexRow::kKeyWidthOffset);
static_assert(offsetof(SysIndexRow, entry_width) == SysIndexRow::kEntryWidthOffset);
static_assert(offsetof(SysIndexRow, name) == SysIndexRow::kNameOffset);
static_assert(offsetof(SysIndexRow, nkeys) == SysIndexRow::kNkeysOffset);
static_assert(offsetof(SysIndexRow, ncovered) == SysIndexRow::kNcoveredOffset);
static_assert(offsetof(SysIndexRow, flags) == SysIndexRow::kFlagsOffset);
static_assert(offsetof(SysIndexRow, reserved0) == SysIndexRow::kReserved0Offset);
static_assert(offsetof(SysIndexRow, key_cols) == SysIndexRow::kKeyColsOffset);
static_assert(offsetof(SysIndexRow, covered_cols) == SysIndexRow::kCoveredColsOffset);

// ---- sys.patterns ----------------------------------------------------
//
// One row per observed query shape (docs/spec/waystone-concpets.md section 4).
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
    // (docs/spec/create-pattern-user-defined-patterns-v1.md section 4.1).
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

// ---- sys.access_stats -------------------------------------------------
//
// How often each **access shape** ran, and when it last ran
// (`docs/spec/heap-and-tuple.md` §7). One row per
// `(kind, rel_id, column_mask)`: the physical optimizer's input, and the
// first thing in this engine to collect the "access statistics" §7 has
// described since it was written.
//
// **Keyed by columns, never by values.** `WHERE flag = 1` and
// `WHERE flag = 2` are one row here. That is what makes the relation
// bounded - by the schema rather than by the data - so it needs no
// eviction policy and no directory. The *values* already identify
// something: a Waystone pattern instance, through `arg_hash`. Two layers,
// two questions: "which columns does this workload search on" belongs
// here, "which arguments repeat" belongs there.
//
// Field order by descending alignment, so the on-disk offsets and the
// struct's coincide and every field carries an offsetof assert.
struct SysAccessStatRow {
    Oid rel_id;

    // Bit per `col_pos` the access was keyed or filtered on: bit 0 (the
    // pk) for a lookup, probe or range; the filtered columns for a filter
    // scan; 0 for a bare scan.
    //
    // A relation wider than 64 columns folds its high columns into no bit,
    // which makes the shape *coarser* rather than wrong - two accesses
    // differing only past column 63 merge into one row. Stated here rather
    // than left to be discovered.
    std::uint64_t column_mask;

    // Executions observed. Saturating, never wrapping: a count that rolled
    // over would make the hottest access in the database look like the
    // coldest, which is the one reading an optimizer must never be handed.
    std::uint64_t use_count;

    // Truncated logical timestamp of the last execution. Best-effort, in
    // the same sense sys.patterns' is: it ranks shapes, and nothing
    // reports it.
    std::uint64_t last_seen;

    // The `exec::AccessKind`, mapped explicitly by
    // `exec::StoredAccessKind()` - never a raw cast. The enum's first
    // value is 0 and so is a zeroed page's, which is exactly the collision
    // `stmt_class` already had to be taught to avoid.
    std::uint8_t kind;

    static constexpr std::size_t kRelIdOffset = 0;
    static constexpr std::size_t kColumnMaskOffset = 8;
    static constexpr std::size_t kUseCountOffset = 16;
    static constexpr std::size_t kLastSeenOffset = 24;
    static constexpr std::size_t kKindOffset = 32;
    static constexpr std::size_t kOnDiskSize = kKindOffset + sizeof(std::uint8_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysAccessStatRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysAccessStatRow, rel_id) == SysAccessStatRow::kRelIdOffset);
static_assert(offsetof(SysAccessStatRow, column_mask) == SysAccessStatRow::kColumnMaskOffset);
static_assert(offsetof(SysAccessStatRow, use_count) == SysAccessStatRow::kUseCountOffset);
static_assert(offsetof(SysAccessStatRow, last_seen) == SysAccessStatRow::kLastSeenOffset);
static_assert(offsetof(SysAccessStatRow, kind) == SysAccessStatRow::kKindOffset);
static_assert(SysAccessStatRow::kOnDiskSize == 33);

// The stored `kind` a zeroed or never-written row decodes to, and therefore
// the one value that never names a real access kind.
inline constexpr std::uint8_t kAccessKindUnset = 0;

// Distinct access shapes recorded before new ones stop being admitted.
//
// `[PROPOSED]`. The population is (kind x relation x column combination),
// which in a real schema is dozens - a relation is searched on a handful of
// columns, not on the power set. The cap exists because "in a real schema"
// is an assumption, and an unbounded catalog relation on the statement path
// is the kind of assumption that fails quietly.
inline constexpr std::size_t kMaxAccessShapes = 4096;

// The class value a row carries when nothing has classified the statement
// - which is every row until the parser gains its class tags. Named so no
// call site writes a bare 0 and leaves the next reader guessing whether it
// meant "unclassified" or "the first class in some list".
inline constexpr std::uint8_t kStmtClassUnclassified = 0;

// ---- sys.cabins -------------------------------------------------------
//
// One row per Cabin: a per-`(relation, non-pk column)` store that is
// **authoritative for the values queries have observed** and for nothing
// else (`docs/spec/cabin.md` §10, C1).
//
// What is *in* this row is the whole of what survives a restart, and that
// is deliberate. §9 makes Cabin pages unlogged-authoritative: the
// completeness promise holds only while the write hook is live, so a crash
// declares every Cabin fully unobserved and traffic rebuilds it. So the
// catalog stores the Cabin's **existence** - which is DDL, and durable -
// and never its observed set, which is neither.
//
// `observed_ct` is the one field that describes runtime state, and it is
// therefore **best-effort**, in the same sense sys.patterns' `use_count`
// is: nothing reads it back to make a decision, and a stale value costs an
// inaccurate `SHOW CABINS` line and never an answer. v1 does not write it
// at all - the runtime store owns the live count - and it is present so
// that persisting the sets (phase 2) does not need a row format change,
// which by the rule above is a format-version event.
//
// Field order by descending alignment, so the on-disk offsets and the
// struct's coincide and every field carries an offsetof assert.
struct SysCabinRow {
    // From AllocateRowId(kSysCabinsTable) - the persistent sequence in
    // sys.cabins' own sys.tables row, for the reason RegisterPattern()
    // records: GenerateUserOid() numbers *objects*, not this relation's rows, and
    // this row is persisted.
    std::uint64_t cabin_id;

    Oid rel_oid;

    // Live values, best-effort. See the note above.
    std::uint64_t observed_ct;

    // The key column's schema position. **Never 0**: the pk's Cabin is the
    // clustered tree itself (§2), so a 0 here is a row no writer produces.
    std::uint16_t column_no;

    // kCabinOriginAuto / kCabinOriginUser. Mirrors sys.patterns' origin
    // axis, and for the same reason: an operator-declared Cabin and one a
    // future promotion pipeline created have different lifecycle rights.
    std::uint8_t origin;

    // kCabinStatusActive / kCabinStatusBuilding / kCabinStatusDemoted.
    std::uint8_t status;

    static constexpr std::size_t kCabinIdOffset = 0;
    static constexpr std::size_t kRelOidOffset = 8;
    static constexpr std::size_t kObservedCtOffset = 16;
    static constexpr std::size_t kColumnNoOffset = 24;
    static constexpr std::size_t kOriginOffset = 26;
    static constexpr std::size_t kStatusOffset = 27;
    static constexpr std::size_t kOnDiskSize = kStatusOffset + sizeof(std::uint8_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysCabinRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysCabinRow, cabin_id) == SysCabinRow::kCabinIdOffset);
static_assert(offsetof(SysCabinRow, rel_oid) == SysCabinRow::kRelOidOffset);
static_assert(offsetof(SysCabinRow, observed_ct) == SysCabinRow::kObservedCtOffset);
static_assert(offsetof(SysCabinRow, column_no) == SysCabinRow::kColumnNoOffset);
static_assert(offsetof(SysCabinRow, origin) == SysCabinRow::kOriginOffset);
static_assert(offsetof(SysCabinRow, status) == SysCabinRow::kStatusOffset);
static_assert(SysCabinRow::kOnDiskSize == 28);

// `origin` and `status`, with **0 reserved for "unset"** in both.
//
// The same collision `StoredStatementClass` and `StoredAccessKind` were
// each taught to avoid: a zeroed or never-written row decodes to 0, so a
// 0 that also named a real value would make every absent row read as a
// real one. Here that would be an active user-declared Cabin conjured out
// of empty page bytes.
inline constexpr std::uint8_t kCabinOriginUnset = 0;
inline constexpr std::uint8_t kCabinOriginAuto = 1;
inline constexpr std::uint8_t kCabinOriginUser = 2;

inline constexpr std::uint8_t kCabinStatusUnset = 0;
inline constexpr std::uint8_t kCabinStatusActive = 1;
// Declared but not yet serving: the catalog row exists and the read path
// must not consult it. No writer produces it in v1 - a Cabin is active the
// moment it is created, because creating one observes nothing (§4's miss
// path is what fills it) - and it is named so that a phase-2 background
// build has a status to occupy rather than inventing one.
inline constexpr std::uint8_t kCabinStatusBuilding = 2;
// Kept in the catalog, not served: the §8 budget's answer to a write-hot
// Cabin whose sets churn faster than they pay. Un-observing is always
// legal (§1), so demotion is a performance decision and never a
// correctness one - which is what lets it be a status rather than a drop.
inline constexpr std::uint8_t kCabinStatusDemoted = 3;

// Whether the read path may serve from this Cabin at all. One test, here,
// so a status value added later cannot mean "servable" in one caller and
// "not" in another.
constexpr bool IsCabinServing(const SysCabinRow& row) noexcept {
    return row.status == kCabinStatusActive;
}

// ---- sys.fkeys --------------------------------------------------------
//
// One row per foreign key: a column of a child relation whose value is a
// **parent relation's Keystone id** (`docs/spec/foreign-keys.md` §1, F1).
//
// The row is what a foreign key *is*; the checks it drives are FK-M2 and
// FK-M3 and read nothing else. Two fields that a reader expects and will
// not find, both by decision rather than by omission:
//
//   - **no parent column.** F1 fixes the parent side to the Keystone id
//     for every foreign key there can be. That is what makes ON UPDATE
//     CASCADE unnecessary rather than deferred - the referenced key is
//     immutable (invariant 11) - and what makes a stale reference able to
//     dangle but never to name a different row (K1's issue-once).
//   - **no action.** v1 is RESTRICT / NO ACTION only (F2), so an action
//     field would have exactly one legal value. CASCADE and SET NULL need
//     the budget-interaction design F2 defers, and adding the field then is
//     a format-version event like any other - the same trade `SysCabinRow`
//     made in the other direction with `observed_ct`, and here the field
//     that would be speculative is the one left out.
//
// Field order by descending alignment, so the on-disk offsets and the
// struct's coincide and every field carries an offsetof assert.
struct SysFkeyRow {
    // From AllocateRowId(kSysFkeysTable) - the persistent sequence in
    // sys.fkeys' own sys.tables row, for the reason SysCabinRow records:
    // GenerateUserOid() numbers objects rather than this relation's rows, and this row
    // is persisted.
    std::uint64_t fk_id;

    // The relation holding the reference, and the one referenced. Both are
    // needed on the row because both directions are read: the forward check
    // (child INSERT/UPDATE) starts from the child, the reverse check
    // (parent DELETE) from the parent.
    Oid child_rel_oid;
    Oid parent_rel_oid;

    // The referencing column's schema position. **Never 0**: column 0 is
    // the Keystone pk, which is the child's own identity and not a field of
    // it, so a 0 here is a row no writer produces.
    std::uint16_t child_column_no;

    // kFkNullable and whatever joins it. A never-written row decodes to 0,
    // which reads as "not nullable" - the reading under which the check
    // always runs. That direction is deliberate: a zeroed row must not be
    // able to switch a constraint off.
    std::uint16_t flags;

    static constexpr std::size_t kFkIdOffset = 0;
    static constexpr std::size_t kChildRelOidOffset = 8;
    static constexpr std::size_t kParentRelOidOffset = 16;
    static constexpr std::size_t kChildColumnNoOffset = 24;
    static constexpr std::size_t kFlagsOffset = 26;
    static constexpr std::size_t kOnDiskSize = kFlagsOffset + sizeof(std::uint16_t);

    std::array<std::byte, kOnDiskSize> Encode() const;
    static StatusOr<SysFkeyRow> Decode(std::span<const std::byte> bytes);
};

static_assert(offsetof(SysFkeyRow, fk_id) == SysFkeyRow::kFkIdOffset);
static_assert(offsetof(SysFkeyRow, child_rel_oid) == SysFkeyRow::kChildRelOidOffset);
static_assert(offsetof(SysFkeyRow, parent_rel_oid) == SysFkeyRow::kParentRelOidOffset);
static_assert(offsetof(SysFkeyRow, child_column_no) == SysFkeyRow::kChildColumnNoOffset);
static_assert(offsetof(SysFkeyRow, flags) == SysFkeyRow::kFlagsOffset);
static_assert(SysFkeyRow::kOnDiskSize == 28);

// MATCH SIMPLE: a NULL fk value skips the check (§2).
//
// Written by `Catalog::CreateForeignKey` from the child column's declared
// nullability - the one writer, at the one moment the fact is known.
// Enforcement never consults it: a NULL value skips the forward probe by
// being a NULL, a NULL child cell matches no parent pk on the reverse walk,
// and a NOT NULL column refuses the NULL in the row codec before any row
// lands. The bit records the declaration so `SHOW` prints it without a
// second catalog read, and a stored 0 keeps its one reading: the check runs.
inline constexpr std::uint16_t kFkNullable = 1u << 0;

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
