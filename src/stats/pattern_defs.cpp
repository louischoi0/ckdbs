#include "kds/stats/pattern_defs.hpp"

#include "kds/exec/wal_row_log.hpp"

#include <utility>

#include "kds/exec/row_codec.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/varheap.hpp"

namespace kds::stats {

namespace {

// Schema positions. Named rather than spelled as literals at each use: the
// relation's shape is fixed by Catalog::BootstrapPatternDefs() and these are
// the one place this file states its dependence on it.
inline constexpr std::size_t kColId = 0;
inline constexpr std::size_t kColPatternId = 1;
inline constexpr std::size_t kColParamCount = 2;
inline constexpr std::size_t kColName = 3;
inline constexpr std::size_t kColSourceText = 4;
inline constexpr std::size_t kColumnCount = 5;

// ASCII-only fold, matching the lexer's and the fingerprint's. Not
// std::tolower: it consults the locale, and which pattern a name resolves to
// must not depend on the locale the server booted in.
char FoldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (FoldAscii(a[i]) != FoldAscii(b[i])) return false;
    }
    return true;
}

StatusOr<const catalog::TableAccess*> OpenPatternDefs(catalog::Catalog& catalog) {
    auto access = catalog.InitTableAccess(catalog::kSysPatternDefsTable);
    if (!access.ok()) {
        return access.status().WithContext(
            "sys.pattern_defs is missing - the database was bootstrapped by an older build");
    }
    if (access.value()->schema.columns.size() != kColumnCount) {
        return Status::Corruption("sys.pattern_defs has the wrong column count");
    }
    return access.value();
}

// One row as the walk collected it: decoded, but with its spilled cells
// still unfetched. Kept as a struct because the pending spills index into
// `values`, so the two have to move together.
struct StagedRow {
    std::vector<parser::AstValue> values;
    std::vector<exec::PendingSpill> spills;
};

PatternDef ToPatternDef(const std::vector<parser::AstValue>& values) {
    PatternDef def{};
    // The pk arrives as a plain signed decode; ids are 40-bit, so it cannot
    // be negative and the cast is total.
    def.id = static_cast<std::uint64_t>(values[kColId].int_val);
    // pattern_id is a uint64 column, so reading it back is the codec's
    // question rather than this file's: below INT64_MAX the integer is
    // exact and the digit text is not written at all, above it only the
    // text is lossless (exec::ValueAsUint64). Doing the arithmetic here is
    // what made this the one reader that broke when the rule changed.
    auto pattern_id = exec::ValueAsUint64(values[kColPatternId]);
    def.pattern_id = pattern_id.ok() ? pattern_id.value() : 0;
    def.param_count = static_cast<std::uint32_t>(values[kColParamCount].int_val);
    def.name = values[kColName].str_val;
    def.source_text = values[kColSourceText].str_val;
    return def;
}

}  // namespace

StatusOr<std::vector<PatternDef>> ListPatternDefs(catalog::Catalog& catalog,
                                                  storage::PageStore& store) {
    auto access = OpenPatternDefs(catalog);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    // Staged first, resolved second. The decode inside the walk touches no
    // page but the one the walk is already holding; every var-heap fetch
    // happens below, after ChainVisit has returned and released its spans.
    // That ordering is I15's R1, and it is the whole reason this cannot
    // short-circuit on a match.
    std::vector<StagedRow> staged;
    Status walked = heap::ChainVisit(
        store, rel.desc_page_id, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            // A retired slot reads as NotFound and is not an error: it is
            // what DeletePatternDef leaves behind. ChainVisit deliberately
            // re-tests liveness through the callback rather than filtering
            // for it, so this skip belongs here.
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            if (tuple.value().deleted) return storage::VisitControl::kContinue;

            StagedRow row;
            row.values.resize(kColumnCount);
            if (Status s = exec::DecodeRowInto(rel.schema, rel.layout, tuple.value().payload,
                                                row.values, &row.spills);
                !s.ok()) {
                return s;
            }
            staged.push_back(std::move(row));
            return storage::VisitControl::kContinue;
        });
    if (!walked.ok()) return walked;

    std::vector<PatternDef> out;
    out.reserve(staged.size());
    for (StagedRow& row : staged) {
        if (Status s = exec::ResolveSpills(store, row.spills, row.values); !s.ok()) return s;
        out.push_back(ToPatternDef(row.values));
    }
    return out;
}

StatusOr<std::optional<PatternDef>> FindPatternDefByName(catalog::Catalog& catalog,
                                                         storage::PageStore& store,
                                                         std::string_view name) {
    auto defs = ListPatternDefs(catalog, store);
    if (!defs.ok()) return defs.status();
    for (PatternDef& def : defs.value()) {
        if (IEquals(def.name, name)) return std::optional<PatternDef>(std::move(def));
    }
    return std::optional<PatternDef>();
}

StatusOr<std::optional<PatternDef>> FindPatternDefByPatternId(catalog::Catalog& catalog,
                                                              storage::PageStore& store,
                                                              std::uint64_t pattern_id) {
    auto defs = ListPatternDefs(catalog, store);
    if (!defs.ok()) return defs.status();
    for (PatternDef& def : defs.value()) {
        if (def.pattern_id == pattern_id) return std::optional<PatternDef>(std::move(def));
    }
    return std::optional<PatternDef>();
}

Status InsertPatternDef(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, std::uint64_t pattern_id, std::string_view name,
                        std::string_view source_text, std::uint32_t param_count) {
    // Refused here, naming the limit, rather than let EncodeRow report it as
    // an anonymous over-long varchar: the caller is a client who wrote a
    // statement, and the number that matters to them is how long a body may
    // be.
    if (source_text.size() > varheap::kMaxValueSize) {
        return Status::Unsupported("pattern declaration of " +
                                   std::to_string(source_text.size()) +
                                   " bytes exceeds the " +
                                   std::to_string(varheap::kMaxValueSize) +
                                   "-byte limit on a single stored value");
    }

    auto access = OpenPatternDefs(catalog);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    auto id = catalog.AllocateRowId(catalog::kSysPatternDefsTable);
    if (!id.ok()) return id.status();

    // The pk is not among these: it is carried by the Keystone word and
    // never also as a body column (invariant 11), so EncodeRow takes the
    // columns *after* it.
    std::vector<parser::AstValue> values(kColumnCount - 1);
    values[kColPatternId - 1].type = parser::ValueType::kInt;
    values[kColPatternId - 1].raw_int_text = std::to_string(pattern_id);
    values[kColParamCount - 1].type = parser::ValueType::kInt;
    values[kColParamCount - 1].int_val = static_cast<std::int64_t>(param_count);
    values[kColName - 1].type = parser::ValueType::kStr;
    values[kColName - 1].str_val = std::string(name);
    values[kColSourceText - 1].type = parser::ValueType::kStr;
    values[kColSourceText - 1].str_val = std::string(source_text);

    std::vector<exec::AppendedSpill> spills;
    exec::VarHeapSink sink;
    sink.store = &store;
    sink.root = rel.varheap_page_id;
    sink.owner_oid = rel.oid;
    sink.appended = &spills;

    auto payload = exec::EncodeRow(rel.schema, rel.layout, id.value(), values, sink);
    if (!payload.ok()) return payload.status();

    auto placed = heap::ChainInsert(store, rel.desc_page_id, id.value(), payload.value(),
                                    catalog::kBootstrapXid, rel.oid);
    if (!placed.ok()) return placed.status();
    // Logged since 2026-08-19, closing RV3's named remainder for this
    // relation: the spills and any grown page precede the row record
    // (exec/wal_row_log.hpp says why the order is load-bearing). A null
    // wal is the unlogged tests' path, exactly as everywhere else.
    return exec::LogChainInsert(wal, store, placed.value(), payload.value(),
                                catalog::kBootstrapXid, rel.oid, spills);
}

Status DeletePatternDef(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, std::uint64_t pattern_id) {
    auto access = OpenPatternDefs(catalog);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    bool found = false;
    // This walk *can* stop early, unlike the reader above, and for a reason
    // worth naming: `pattern_id` is a fixed-width column, so it is never
    // spilled and the match is decidable without leaving the page. The
    // spills list is passed only so a spilled `name`/`source_text` cell does
    // not turn the decode into an error; nothing here reads them.
    std::vector<parser::AstValue> values(kColumnCount);
    std::vector<exec::PendingSpill> spills;
    Status walked = heap::ChainVisit(
        store, rel.desc_page_id, storage::PageAccess::kWrite,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            // A retired slot reads as NotFound and is not an error: it is
            // what DeletePatternDef leaves behind. ChainVisit deliberately
            // re-tests liveness through the callback rather than filtering
            // for it, so this skip belongs here.
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            if (tuple.value().deleted) return storage::VisitControl::kContinue;

            if (Status s = exec::DecodeRowInto(rel.schema, rel.layout, tuple.value().payload,
                                                values, &spills);
                !s.ok()) {
                return s;
            }
            if (ToPatternDef(values).pattern_id != pattern_id) {
                return storage::VisitControl::kContinue;
            }

            // Retired, not delete-marked. Catalog reads have no snapshot to
            // filter a delete-mark against, so a marked row would still be
            // found by name and a DROP followed by a CREATE of the same name
            // would collide with a row nobody can see.
            if (Status s = page.RetireSlot(slot); !s.ok()) return s;
            if (Status s = exec::LogSlotRetire(wal, store, wal::kNoTxnId, page_id, slot);
                !s.ok()) {
                return s;
            }
            found = true;
            return storage::VisitControl::kStop;
        });
    if (!walked.ok()) return walked;
    if (!found) return Status::NotFound("no sys.pattern_defs row for this pattern_id");
    return Status::OK();
}

}  // namespace kds::stats
