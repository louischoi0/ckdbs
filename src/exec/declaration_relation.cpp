#include "kds/exec/declaration_relation.hpp"

#include <utility>

#include "kds/exec/row_codec.hpp"
#include "kds/exec/wal_row_log.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/wal/manager.hpp"

namespace kds::exec {

namespace {

// Schema positions, fixed by `Catalog::DeclarationSchema` - the one place
// this file states its dependence on it.
inline constexpr std::size_t kColId = 0;
inline constexpr std::size_t kColTargetOid = 1;
inline constexpr std::size_t kColAux = 2;
inline constexpr std::size_t kColFlags = 3;
inline constexpr std::size_t kColName = 4;
inline constexpr std::size_t kColSourceText = 5;
inline constexpr std::size_t kColumnCount = 6;

// ASCII-only fold, matching the lexer's and the fingerprint's. Not
// std::tolower: it consults the locale, and which declaration a name
// resolves to must not depend on the locale the server booted in.
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

StatusOr<const catalog::TableAccess*> OpenDeclarations(catalog::Catalog& catalog,
                                                       catalog::Oid rel_oid) {
    auto access = catalog.InitTableAccess(rel_oid);
    if (!access.ok()) {
        return access.status().WithContext(
            "declaration relation oid " + std::to_string(rel_oid) +
            " is missing - the database was bootstrapped by an older build");
    }
    if (access.value()->schema.columns.size() != kColumnCount) {
        return Status::Corruption("declaration relation oid " + std::to_string(rel_oid) +
                                  " has the wrong column count");
    }
    return access.value();
}

// One row as the walk collected it: decoded, but with its spilled cells
// still unfetched. The pending spills index into `values`, so the two move
// together.
struct StagedRow {
    std::vector<parser::AstValue> values;
    std::vector<PendingSpill> spills;
};

DeclarationRow ToRow(const std::vector<parser::AstValue>& values) {
    DeclarationRow row{};
    // The tuple key arrives as a plain signed decode; ids fit int64, so it cannot
    // be negative and the cast is total.
    row.id = static_cast<std::uint64_t>(values[kColId].int_val);
    row.target_oid = static_cast<catalog::Oid>(values[kColTargetOid].int_val);
    row.aux = values[kColAux].int_val;
    row.flags = static_cast<std::uint32_t>(values[kColFlags].int_val);
    row.name = values[kColName].str_val;
    row.source_text = values[kColSourceText].str_val;
    return row;
}

}  // namespace

StatusOr<std::vector<DeclarationRow>> ScanDeclarations(catalog::Catalog& catalog,
                                                       storage::PageStore& store,
                                                       catalog::Oid rel_oid,
                                                       bool resolve_spills) {
    auto access = OpenDeclarations(catalog, rel_oid);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    // Staged first, resolved second: the decode inside the walk touches no
    // page but the one the walk is already holding, and every var-heap
    // fetch happens after ChainVisit has returned and released its spans.
    std::vector<StagedRow> staged;
    Status walked = heap::ChainVisit(
        store, rel.desc_page_id, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            // A retired slot reads as NotFound and is not an error: it is
            // what DeleteDeclaration leaves behind. ChainVisit re-tests
            // liveness through the callback rather than filtering for it.
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            if (tuple.value().deleted) return storage::VisitControl::kContinue;

            StagedRow row;
            row.values.resize(kColumnCount);
            if (Status s = DecodeRowInto(rel.schema, rel.layout, tuple.value().payload,
                                          row.values, &row.spills);
                !s.ok()) {
                return s;
            }
            staged.push_back(std::move(row));
            return storage::VisitControl::kContinue;
        });
    if (!walked.ok()) return walked;

    std::vector<DeclarationRow> out;
    out.reserve(staged.size());
    for (StagedRow& row : staged) {
        if (resolve_spills) {
            if (Status s = ResolveSpills(store, row.spills, row.values); !s.ok()) return s;
        }
        out.push_back(ToRow(row.values));
    }
    return out;
}

StatusOr<std::optional<DeclarationRow>> FindDeclarationByName(catalog::Catalog& catalog,
                                                              storage::PageStore& store,
                                                              catalog::Oid rel_oid,
                                                              std::string_view name) {
    auto rows = ScanDeclarations(catalog, store, rel_oid);
    if (!rows.ok()) return rows.status();
    for (DeclarationRow& row : rows.value()) {
        if (IEquals(row.name, name)) return std::optional<DeclarationRow>(std::move(row));
    }
    return std::optional<DeclarationRow>();
}

StatusOr<std::vector<DeclarationRow>> DeclarationsOnRelation(catalog::Catalog& catalog,
                                                             storage::PageStore& store,
                                                             catalog::Oid rel_oid,
                                                             catalog::Oid target_oid) {
    auto rows = ScanDeclarations(catalog, store, rel_oid);
    if (!rows.ok()) return rows.status();
    std::vector<DeclarationRow> out;
    for (DeclarationRow& row : rows.value()) {
        if (row.target_oid == target_oid) out.push_back(std::move(row));
    }
    return out;
}

Status InsertDeclaration(catalog::Catalog& catalog, storage::PageStore& store,
                         wal::WalManager* wal, catalog::Oid rel_oid, std::string_view kind,
                         const DeclarationRow& row) {
    // Refused here, naming the limit, rather than letting EncodeRow report
    // it as an anonymous over-long varchar: the number that matters to the
    // client is how long a declaration may be.
    if (row.source_text.size() > varheap::kMaxValueSize) {
        return Status::NotImplemented(std::string(kind) + " declaration of " +
                                      std::to_string(row.source_text.size()) +
                                      " bytes exceeds the " +
                                      std::to_string(varheap::kMaxValueSize) +
                                      "-byte limit on a single stored value");
    }

    auto access = OpenDeclarations(catalog, rel_oid);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    // The duplicate-name check, made here because this is the door every
    // caller comes through, and **under the relation's root page held
    // exclusive through the insert** (AT-S17), so no other core publishes
    // the name in between. Declaration names are instance-wide; a DDL's
    // relation `X` fences its own relation only. What nests under the hold
    // is this relation's own var-heap chain, so the order is root, then
    // var-heap, and never the other way.
    auto names = store.Get(rel.desc_page_id);
    if (!names.ok()) return names.status();
    auto existing = FindDeclarationByName(catalog, store, rel_oid, row.name);
    if (!existing.ok()) return existing.status();
    if (existing.value().has_value()) {
        return Status::AlreadyExists(std::string(kind) + " \"" + row.name +
                                     "\" already exists");
    }

    // The tuple key is carried by the Keystone word and never also as a body
    // column (invariant 11), so EncodeRow takes the columns *after* it.
    std::vector<parser::AstValue> values(kColumnCount - 1);
    auto set_int = [&](std::size_t col, std::int64_t v) {
        values[col - 1].type = parser::ValueType::kInt;
        values[col - 1].int_val = v;
    };
    auto set_str = [&](std::size_t col, std::string_view v) {
        values[col - 1].type = parser::ValueType::kStr;
        values[col - 1].str_val = std::string(v);
    };
    set_int(kColTargetOid, static_cast<std::int64_t>(row.target_oid));
    set_int(kColAux, row.aux);
    set_int(kColFlags, static_cast<std::int64_t>(row.flags));
    set_str(kColName, row.name);
    set_str(kColSourceText, row.source_text);

    std::vector<AppendedSpill> spills;
    VarHeapSink sink;
    sink.store = &store;
    sink.root = rel.varheap_page_id;
    sink.owner_oid = rel.oid;
    sink.appended = &spills;

    auto payload = EncodeRow(rel.schema, rel.layout, row.id, values, sink);
    if (!payload.ok()) return payload.status();

    auto placed = heap::ChainInsert(store, rel.desc_page_id, row.id, payload.value(),
                                    catalog::kBootstrapXid, rel.oid);
    if (!placed.ok()) return placed.status();
    // Logged: an acknowledged declaration must survive a crash by redo like
    // any other catalog write (exec/wal_row_log.hpp has the order).
    return LogChainInsert(wal, store, placed.value(), payload.value(), catalog::kBootstrapXid,
                          rel.oid, spills);
}

StatusOr<DeclarationRow> DeleteDeclaration(catalog::Catalog& catalog, storage::PageStore& store,
                                           wal::WalManager* wal, catalog::Oid rel_oid,
                                           std::string_view kind, std::string_view name) {
    auto access = OpenDeclarations(catalog, rel_oid);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    // This walk **cannot** stop early on its match: it matches on `name`,
    // which may be spilled, and resolving a spill under a live page span is
    // what I15's R1 forbids. So the row is found by a full scan first and
    // the retiring walk matches on the tuple key, which is always on the page.
    auto found = FindDeclarationByName(catalog, store, rel_oid, name);
    if (!found.ok()) return found.status();
    if (!found.value().has_value()) {
        return Status::NotFound("no " + std::string(kind) + " named \"" + std::string(name) +
                                "\"");
    }
    const std::uint64_t target_id = found.value()->id;

    bool retired = false;
    Status walked = heap::ChainVisit(
        store, rel.desc_page_id, storage::PageAccess::kWrite,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            if (tuple.value().deleted) return storage::VisitControl::kContinue;

            auto id = KeystoneIdOfPayload(tuple.value().payload);
            if (!id.ok()) return id.status();
            if (id.value() != target_id) return storage::VisitControl::kContinue;
            if (Status s = page.RetireSlot(slot); !s.ok()) return s;
            if (Status s = LogSlotRetire(wal, store, wal::kNoTxnId, page_id, slot); !s.ok()) {
                return s;
            }
            retired = true;
            return storage::VisitControl::kStop;
        });
    if (!walked.ok()) return walked;
    if (!retired) {
        return Status::NotFound("no " + std::string(kind) + " named \"" + std::string(name) +
                                "\"");
    }
    return std::move(*found.value());
}

}  // namespace kds::exec
