#include "kds/exec/assertion_catalog.hpp"

#include "kds/exec/wal_row_log.hpp"

#include <variant>

#include "kds/parser/parser.hpp"

#include <array>
#include <utility>

#include "kds/exec/assertion_build.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"

namespace kds::exec {

namespace {

// Schema positions. Named rather than spelled as literals at each use: the
// relation's shape is fixed by Catalog::BootstrapAssertions() and these are
// the one place this file states its dependence on it.
inline constexpr std::size_t kColId = 0;
inline constexpr std::size_t kColTargetOid = 1;
inline constexpr std::size_t kColCabinRoot = 2;
inline constexpr std::size_t kColFlags = 3;
inline constexpr std::size_t kColName = 4;
inline constexpr std::size_t kColSourceText = 5;
inline constexpr std::size_t kColumnCount = 6;

// ASCII-only fold, matching the lexer's and the fingerprint's. Not
// std::tolower: it consults the locale, and which assertion a name resolves
// to must not depend on the locale the server booted in.
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

StatusOr<const catalog::TableAccess*> OpenAssertions(catalog::Catalog& catalog) {
    auto access = catalog.InitTableAccess(catalog::kSysAssertionsTable);
    if (!access.ok()) {
        return access.status().WithContext(
            "sys.assertions is missing - the database was bootstrapped by an older build");
    }
    if (access.value()->schema.columns.size() != kColumnCount) {
        return Status::Corruption("sys.assertions has the wrong column count");
    }
    return access.value();
}

// One row as the walk collected it: decoded, but with its spilled cells still
// unfetched. Kept as a struct because the pending spills index into `values`,
// so the two have to move together.
struct StagedRow {
    std::vector<parser::AstValue> values;
    std::vector<PendingSpill> spills;
};

AssertionDef ToAssertionDef(const std::vector<parser::AstValue>& values) {
    AssertionDef def{};
    // The pk arrives as a plain signed decode; ids are 40-bit, so it cannot
    // be negative and the cast is total.
    def.id = static_cast<std::uint64_t>(values[kColId].int_val);
    def.target_oid = static_cast<catalog::Oid>(values[kColTargetOid].int_val);
    def.cabin_root = static_cast<PageId>(values[kColCabinRoot].int_val);
    def.flags = static_cast<std::uint32_t>(values[kColFlags].int_val);
    def.name = values[kColName].str_val;
    def.source_text = values[kColSourceText].str_val;
    return def;
}

// The one walk every reader here shares. Staged first, resolved second: the
// decode inside the walk touches no page but the one the walk is already
// holding, and every var-heap fetch happens after ChainVisit has returned and
// released its spans. That ordering is I15's R1, and it is the whole reason
// none of these can short-circuit on a match - the name to match on may still
// be an unresolved pointer while the walk runs.
// `resolve_spills = false` returns the fixed columns alone - id, target oid,
// cabin root - and leaves `name` and `source_text` empty. The fixed columns
// are inline in the heap tuple, so that form **touches no var-heap page**,
// which is the difference between a reader a peer core can run and one it
// cannot: a catalog relation's heap pages are all below `kFirstUserPageId`
// by construction (well_known.hpp), and its var-heap is not
// (`ListAssertionTargets` says what depends on that).
StatusOr<std::vector<AssertionDef>> ScanAssertions(catalog::Catalog& catalog,
                                                   storage::PageStore& store,
                                                   bool resolve_spills = true) {
    auto access = OpenAssertions(catalog);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    std::vector<StagedRow> staged;
    Status walked = heap::ChainVisit(
        store, rel.desc_page_id, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            // A retired slot reads as NotFound and is not an error: it is
            // what DeleteAssertion leaves behind. ChainVisit deliberately
            // re-tests liveness through the callback rather than filtering for
            // it, so this skip belongs here.
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

    std::vector<AssertionDef> out;
    out.reserve(staged.size());
    for (StagedRow& row : staged) {
        if (resolve_spills) {
            if (Status s = ResolveSpills(store, row.spills, row.values); !s.ok()) return s;
        }
        out.push_back(ToAssertionDef(row.values));
    }
    return out;
}

}  // namespace

StatusOr<std::vector<AssertionDef>> ListAssertions(catalog::Catalog& catalog,
                                                   storage::PageStore& store) {
    return ScanAssertions(catalog, store);
}

StatusOr<std::vector<AssertionDef>> ListAssertionTargets(catalog::Catalog& catalog,
                                                         storage::PageStore& store) {
    return ScanAssertions(catalog, store, /*resolve_spills=*/false);
}

StatusOr<std::vector<PageId>> AssertionSpillPages(catalog::Catalog& catalog,
                                                  storage::PageStore& store) {
    auto access = OpenAssertions(catalog);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    std::vector<PageId> pages;
    Status walked = heap::ChainVisit(
        store, rel.desc_page_id, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            if (tuple.value().deleted) return storage::VisitControl::kContinue;

            std::vector<parser::AstValue> values(kColumnCount);
            std::vector<PendingSpill> spills;
            if (Status s = DecodeRowInto(rel.schema, rel.layout, tuple.value().payload, values,
                                          &spills);
                !s.ok()) {
                return s;
            }
            // The ids the row *names*, never followed: this runs where the
            // fetch is not yet permitted, which is the whole point.
            for (const PendingSpill& spill : spills) {
                if (spill.ptr.page_id == kInvalidPageId) continue;
                if (std::find(pages.begin(), pages.end(), spill.ptr.page_id) == pages.end()) {
                    pages.push_back(spill.ptr.page_id);
                }
            }
            return storage::VisitControl::kContinue;
        });
    if (!walked.ok()) return walked;
    return pages;
}

StatusOr<std::optional<AssertionDef>> FindAssertionByName(catalog::Catalog& catalog,
                                                          storage::PageStore& store,
                                                          std::string_view name) {
    auto defs = ScanAssertions(catalog, store);
    if (!defs.ok()) return defs.status();
    for (AssertionDef& def : defs.value()) {
        if (IEquals(def.name, name)) return std::optional<AssertionDef>(std::move(def));
    }
    return std::optional<AssertionDef>();
}

StatusOr<std::vector<AssertionDef>> AssertionsOnRelation(catalog::Catalog& catalog,
                                                         storage::PageStore& store,
                                                         catalog::Oid target_oid) {
    auto defs = ScanAssertions(catalog, store);
    if (!defs.ok()) return defs.status();

    std::vector<AssertionDef> out;
    for (AssertionDef& def : defs.value()) {
        if (def.target_oid == target_oid) out.push_back(std::move(def));
    }
    return out;
}

Status InsertAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                       wal::WalManager* wal, std::uint64_t id,
                       catalog::Oid target_oid, std::string_view name,
                       std::string_view source_text, PageId cabin_root) {
    // Refused here, naming the limit, rather than letting EncodeRow report it
    // as an anonymous over-long varchar: the caller is a client who wrote a
    // statement, and the number that matters to them is how long a
    // declaration may be.
    if (source_text.size() > varheap::kMaxValueSize) {
        return Status::Unsupported("assertion declaration of " +
                                   std::to_string(source_text.size()) + " bytes exceeds the " +
                                   std::to_string(varheap::kMaxValueSize) +
                                   "-byte limit on a single stored value");
    }

    // §3.1's duplicate-name check. Made here rather than at the parser
    // because it is the catalog's question and this is the door every caller
    // comes through. `CreateAssertion` also checks it *before* the build,
    // which buys failing before the scan rather than after; this one is the
    // guard that cannot be reached past.
    auto existing = FindAssertionByName(catalog, store, name);
    if (!existing.ok()) return existing.status();
    if (existing.value().has_value()) {
        return Status::AlreadyExists("assertion \"" + std::string(name) + "\" already exists");
    }

    auto access = OpenAssertions(catalog);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    // The pk is not among these: it is carried by the Keystone word and never
    // also as a body column (invariant 11), so EncodeRow takes the columns
    // *after* it.
    std::vector<parser::AstValue> values(kColumnCount - 1);
    values[kColTargetOid - 1].type = parser::ValueType::kInt;
    values[kColTargetOid - 1].int_val = static_cast<std::int64_t>(target_oid);
    // The built chain's root (AST06), or kInvalidPageId from a caller that
    // built nothing. Written explicitly rather than left at a default so a
    // reader can tell "no structure" from "decoded a zero".
    values[kColCabinRoot - 1].type = parser::ValueType::kInt;
    values[kColCabinRoot - 1].int_val = static_cast<std::int64_t>(cabin_root);
    values[kColFlags - 1].type = parser::ValueType::kInt;
    values[kColFlags - 1].int_val = 0;
    values[kColName - 1].type = parser::ValueType::kStr;
    values[kColName - 1].str_val = std::string(name);
    values[kColSourceText - 1].type = parser::ValueType::kStr;
    values[kColSourceText - 1].str_val = std::string(source_text);

    std::vector<AppendedSpill> spills;
    VarHeapSink sink;
    sink.store = &store;
    sink.root = rel.varheap_page_id;
    sink.owner_oid = rel.oid;
    sink.appended = &spills;

    auto payload = EncodeRow(rel.schema, rel.layout, id, values, sink);
    if (!payload.ok()) return payload.status();

    auto placed = heap::ChainInsert(store, rel.desc_page_id, id, payload.value(),
                                    catalog::kBootstrapXid, rel.oid);
    if (!placed.ok()) return placed.status();
    // Logged since 2026-08-19, closing RV3's loudest remainder: this row
    // is what RC07 rebuilds the *enforcing* registry from at mount, so an
    // acknowledged declaration must survive a crash by redo like any
    // other catalog write (exec/wal_row_log.hpp; the order note is there).
    return LogChainInsert(wal, store, placed.value(), payload.value(),
                          catalog::kBootstrapXid, rel.oid, spills);
}

Status DeleteAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                       wal::WalManager* wal, std::string_view name) {
    auto access = OpenAssertions(catalog);
    if (!access.ok()) return access.status();
    const catalog::TableAccess& rel = *access.value();

    // Unlike DeletePatternDef, this walk **cannot** stop early on its match:
    // it matches on `name`, which is a varchar and may therefore be spilled,
    // and resolving a spill under a live page span is exactly what I15's R1
    // forbids. So the id is found by a full scan first and the retiring walk
    // matches on the pk, which is fixed-width and always on the page.
    auto found = FindAssertionByName(catalog, store, name);
    if (!found.ok()) return found.status();
    if (!found.value().has_value()) {
        return Status::NotFound("no assertion named \"" + std::string(name) + "\"");
    }
    const std::uint64_t target_id = found.value()->id;

    // The spills list is passed only so a spilled `name`/`source_text` cell
    // does not turn the decode into an error; nothing here reads them, and
    // `id` is int64 and therefore never spilled.
    std::vector<parser::AstValue> values(kColumnCount);
    std::vector<PendingSpill> spills;
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

            spills.clear();
            if (Status s = DecodeRowInto(rel.schema, rel.layout, tuple.value().payload, values,
                                          &spills);
                !s.ok()) {
                return s;
            }
            if (static_cast<std::uint64_t>(values[kColId].int_val) != target_id) {
                return storage::VisitControl::kContinue;
            }

            // Retired, not delete-marked. Catalog reads have no snapshot to
            // filter a delete-mark against, so a marked row would still be
            // found by name and a DROP followed by a CREATE of the same name
            // would collide with a row nobody can see.
            if (Status s = page.RetireSlot(slot); !s.ok()) return s;
            if (Status s = LogSlotRetire(wal, store, wal::kNoTxnId, page_id, slot); !s.ok()) {
                return s;
            }
            retired = true;
            return storage::VisitControl::kStop;
        });
    if (!walked.ok()) return walked;
    if (!retired) return Status::NotFound("no assertion named \"" + std::string(name) + "\"");
    return Status::OK();
}

namespace {

// Exact-match resolution, like `Schema::FindColumn` everywhere else: the
// catalog stores a column's name as it was declared, and case folding here
// alone would make `CREATE ASSERTION` accept a spelling no other statement
// does. (The *assertion's own* name is matched case-insensitively, which is a
// different question - that is an object name, like a pattern's.)
StatusOr<std::uint16_t> ResolveColumn(const catalog::TableAccess& access,
                                       const parser::IndexColumnRef& col,
                                       const std::string& table_name) {
    for (std::size_t i = 0; i < access.schema.columns.size(); ++i) {
        if (catalog::NameView(access.schema.columns[i].name) == col.name) {
            return static_cast<std::uint16_t>(i);
        }
    }
    return Status::NotFound("relation '" + table_name + "' has no column '" + col.name +
                            "' (byte " + std::to_string(col.byte_offset) + ")");
}

}  // namespace

namespace {

// The teardown marker (§7's "WAL'd teardown"): a stream that saw this
// assertion's records learns they are void. Replay needs nothing more,
// because its skip rule already drops records for an id the catalog cannot
// resolve - the marker makes the stream self-describing rather than
// dependent on that. `cabin_root` is a diagnostic; replay does not touch it.
Status EmitAssertDrop(wal::WalManager* wal, std::uint64_t assertion_id, PageId cabin_root) {
    if (wal == nullptr) return Status::OK();
    std::array<std::byte, wal::kAssertDropPayloadSize> payload{};
    auto used = wal::EncodeAssertDrop(payload, wal::AssertDropPayload{assertion_id});
    if (!used.ok()) return used.status();
    auto rec = wal->Append(
        wal::RecordSpec{wal::RecordType::kAssertDrop, wal::kNoTxnId, cabin_root}, payload);
    return rec.ok() ? Status::OK() : rec.status();
}

}  // namespace

namespace {

// The schema positions every path needs, resolved against **one core's**
// view of the relation. Factored out because `PrepareAssertionDef` and
// `BuildAssertionCabin` both need them and, on the cross-core path, run on
// different cores against different catalog views - so this is one
// implementation resolving twice, never one resolution shared across a
// wire.
struct AssertionColumns {
    std::vector<std::uint16_t> group_cols;
    std::uint16_t sum_col = 0;  // read for a SUM assertion only
};

StatusOr<AssertionColumns> ResolveAssertionColumns(const catalog::TableAccess& access,
                                                   const parser::AssertionStmt& stmt) {
    AssertionColumns out;

    // Every GROUP BY column must exist. The positions are kept for the
    // builder and still not *stored*, because §8.2 keeps `source_text` as the
    // canon and the columns are recovered by re-parsing it. The refusal is
    // §3.1's whole "maximized validation" point - a declaration naming a
    // column that is not there could never be enforced.
    out.group_cols.reserve(stmt.group_columns.size());
    for (const parser::IndexColumnRef& col : stmt.group_columns) {
        auto pos = ResolveColumn(access, col, stmt.table_name);
        if (!pos.ok()) return pos.status();
        out.group_cols.push_back(pos.value());
    }

    if (stmt.func == parser::AggFunc::kSum) {
        auto pos = ResolveColumn(access, stmt.sum_column, stmt.table_name);
        if (!pos.ok()) return pos.status();
        out.sum_col = pos.value();

        const catalog::SysColumnRow& col = access.schema.columns[pos.value()];
        if (col.type_val == catalog::kTypeValUint64) {
            // Named separately because §10 names it separately, and for AG3's
            // reason: half of a uint64's range does not fit the int64
            // accumulator a group header keeps, so a sum of them is not a
            // number this engine can maintain.
            return Status::Unsupported(
                "SUM over a uint64 column is not supported (byte " +
                std::to_string(stmt.sum_column.byte_offset) +
                "); a group's running aggregate is a checked int64 "
                "(docs/spec/assertion.md §10)");
        }
        if (col.type_val != catalog::kTypeValInt64) {
            return Status::InvalidArgument(
                "an assertion's SUM column must be int64; '" + stmt.sum_column.name +
                "' is type_val=" + std::to_string(col.type_val) + " (byte " +
                std::to_string(stmt.sum_column.byte_offset) +
                "); v1 restricts the aggregate to a checked int64 "
                "(docs/spec/assertion.md §3.1)");
        }
    }
    return out;
}

// The relation, or §3.1's refusal with the byte the parser recorded.
StatusOr<catalog::Oid> AssertionTargetOid(catalog::Catalog& catalog,
                                          const parser::AssertionStmt& stmt) {
    auto oid = catalog.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return Status::NotFound("no relation named '" + stmt.table_name + "' (byte " +
                                std::to_string(stmt.table_byte_offset) + ")");
    }
    return oid.value();
}

}  // namespace

StatusOr<AssertionPrepared> PrepareAssertionDef(catalog::Catalog& catalog,
                                                storage::PageStore& store,
                                                const parser::AssertionStmt& stmt) {
    auto oid = AssertionTargetOid(catalog, stmt);
    if (!oid.ok()) return oid.status();
    auto access = catalog.InitTableAccess(oid.value());
    if (!access.ok()) return access.status();

    auto columns = ResolveAssertionColumns(*access.value(), stmt);
    if (!columns.ok()) return columns.status();

    // The cheap refusals the build must not run ahead of: a taken name and
    // an over-long declaration would both fail the publish, and finding out
    // after a full scan is the scan wasted. `InsertAssertion` keeps both
    // guards - these buy the position of the failure, `index_ddl.cpp`'s
    // "one implementation, two callers" note.
    if (stmt.source_text.size() > varheap::kMaxValueSize) {
        return Status::Unsupported("assertion declaration of " +
                                   std::to_string(stmt.source_text.size()) +
                                   " bytes exceeds the " +
                                   std::to_string(varheap::kMaxValueSize) +
                                   "-byte limit on a single stored value");
    }
    auto existing = FindAssertionByName(catalog, store, stmt.name);
    if (!existing.ok()) return existing.status();
    if (existing.value().has_value()) {
        return Status::AlreadyExists("assertion \"" + stmt.name + "\" already exists");
    }

    // The id, before the build: ASSERT_BUILD records carry it. A failed
    // build burns it, which K3 makes free.
    auto id = catalog.AllocateRowId(catalog::kSysAssertionsTable);
    if (!id.ok()) return id.status();

    AssertionPrepared prepared;
    prepared.target_oid = oid.value();
    prepared.assertion_id = id.value();
    return prepared;
}

StatusOr<AssertionCabinBuild> BuildAssertionCabin(catalog::Catalog& catalog,
                                                  storage::PageStore& store,
                                                  const parser::AssertionStmt& stmt,
                                                  std::uint64_t assertion_id,
                                                  const txn::ReadView& check_view,
                                                  wal::WalManager* wal) {
    auto oid = AssertionTargetOid(catalog, stmt);
    if (!oid.ok()) return oid.status();
    auto access = catalog.InitTableAccess(oid.value());
    if (!access.ok()) return access.status();
    auto columns = ResolveAssertionColumns(*access.value(), stmt);
    if (!columns.ok()) return columns.status();

    // ---- Built before it is published (§8.1, index_ddl.cpp's shape) ------
    auto build = BuildBoundCabin(store, *access.value(), stmt, assertion_id,
                                 columns.value().group_cols, columns.value().sum_col,
                                 check_view, wal);
    if (!build.ok()) {
        // The discard marker, then the caller's error. Pages and id leak -
        // the backfill's precedent; nothing reclaims a page in this engine.
        if (Status s = EmitAssertDrop(wal, assertion_id, kInvalidPageId); !s.ok()) return s;
        return build.status();
    }

    AssertionCabinBuild out;
    out.cabin_root = build.value().cabin_root;
    out.rows_incorporated = build.value().rows_incorporated;
    out.group_count = build.value().cabin.group_count();

    // The live half, resolved here where the statement, the schema and the
    // build are all in hand: the write hook must never re-scan a catalog or
    // re-parse a declaration per statement.
    LiveAssertion& live = out.live;
    live.assertion_id = assertion_id;
    live.target_oid = oid.value();
    live.name = stmt.name;
    live.aggregate = stmt.func == parser::AggFunc::kSum ? BoundAggregate::kSum
                                                        : BoundAggregate::kCount;
    live.group_cols = columns.value().group_cols;
    live.sum_col = columns.value().sum_col;
    live.sum_col_name = stmt.sum_column.name;
    for (std::size_t i = 0; i < live.group_cols.size(); ++i) {
        live.group_col_names.push_back(stmt.group_columns[i].name);
        live.group_type_vals.push_back(
            access.value()->schema.columns[live.group_cols[i]].type_val);
    }
    live.chain = build.value().chain;
    live.cabin = std::move(build.value().cabin);

    // **The new cabin's base, at its build** (AS6a, RC07). Without it an
    // assertion created after the last checkpoint has no base in any range: the
    // mount cannot recover it, so it stays out of the registry, so the completion
    // checkpoint - which snapshots the registry - cannot give it one either, and
    // `enforcing=0` is *permanent* until DROP + CREATE. With a long
    // `checkpoint_interval_ms` that is every new assertion.
    //
    // **Into the stream of the core that built it**, which is the core that
    // will append to the cabin - so the base and the records folded onto it
    // are one stream's, whichever core that is (PW1c-6c). Logged before the
    // publish rather than after it: the owner cannot see core 0's row, so
    // the cross-core order is forced, and the local path keeps the same one
    // rather than differing by path (assertion_catalog.hpp says what that
    // costs).
    if (wal != nullptr) {
        wal::AssertionCabinSnapshot base;
        base.assertion_id = assertion_id;
        for (const BoundCabin::GroupSnapshot& group : live.cabin.SnapshotGroups()) {
            wal::AssertionSnapshotGroup entry;
            entry.group_id = group.group_id;
            entry.count = group.count;
            entry.sum = group.sum;
            entry.key = group.key;
            base.groups.push_back(std::move(entry));
        }
        if (Status s = wal::LogAssertionSnapshot(*wal, base); !s.ok()) {
            return s.WithContext("publishing assertion \"" + stmt.name + "\"'s group snapshot");
        }
    }
    return out;
}

StatusOr<AssertionDdlResult> CreateAssertion(catalog::Catalog& catalog,
                                             storage::PageStore& store,
                                             const parser::AssertionStmt& stmt,
                                             const txn::ReadView& check_view,
                                             wal::WalManager* wal) {
    // **The order is validate -> build -> publish** (§8.1), and the three
    // steps are the three entry points a cross-core create splits across
    // (PW1c-6c): this is the single-core arm, where all three are here.
    auto prepared = PrepareAssertionDef(catalog, store, stmt);
    if (!prepared.ok()) return prepared.status();

    auto build = BuildAssertionCabin(catalog, store, stmt, prepared.value().assertion_id,
                                     check_view, wal);
    if (!build.ok()) return build.status();

    // ---- The publish: the single commit point (§8.1a) ---------------------
    //
    // The row carrying the root, and deliberately **no version bump**:
    // `Catalog::BumpVersion` is private on purpose, called only by catalog
    // mutators from which something cached is derived, and nothing cached is
    // derived from a `sys.assertions` row - no CatalogCache entry, and no
    // compiled plan consults assertions. The task that changes the second
    // fact (AST07, whose check steps make a plan a function of the
    // assertion set) is the task that must make this publish invalidate
    // plans, and it owns choosing the door.
    if (Status s = InsertAssertion(catalog, store, wal, prepared.value().assertion_id,
                                   prepared.value().target_oid, stmt.name, stmt.source_text,
                                   build.value().cabin_root);
        !s.ok()) {
        if (Status drop = EmitAssertDrop(wal, prepared.value().assertion_id,
                                         build.value().cabin_root);
            !drop.ok()) {
            return drop;
        }
        return s;
    }

    AssertionDdlResult result;
    result.assertion_id = prepared.value().assertion_id;
    result.target_oid = prepared.value().target_oid;
    result.cabin_root = build.value().cabin_root;
    result.rows_incorporated = build.value().rows_incorporated;
    result.group_count = build.value().group_count;
    result.live.emplace(std::move(build.value().live));
    return result;
}

StatusOr<LiveAssertion> ReviveAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                                       const AssertionDef& def) {
    if (def.cabin_root == kInvalidPageId) {
        return Status::InvalidArgument("assertion \"" + def.name +
                                      "\" carries no Bound Cabin root, so it was never built");
    }

    // §8.2's canon, re-parsed. The declaration is stored verbatim exactly so
    // this is possible: the group columns are positions nothing persists, and
    // re-deriving them from the text is what keeps the GROUP BY list uncapped.
    parser::Parser declaration(def.source_text);
    auto parsed = declaration.Parse();
    if (!parsed.ok()) {
        return parsed.status().WithContext("reviving assertion \"" + def.name +
                                          "\": its stored declaration no longer parses");
    }
    const auto* stmt_ptr = std::get_if<parser::AssertionStmt>(&parsed.value());
    if (stmt_ptr == nullptr) {
        // The row's source text is not a CREATE ASSERTION at all, which the
        // publish path cannot produce - so this is a corrupted row rather than
        // a declaration to interpret.
        return Status::Corruption("assertion \"" + def.name +
                                 "\" stores a declaration that is not a CREATE ASSERTION");
    }
    const parser::AssertionStmt& stmt_value = *stmt_ptr;

    auto access = catalog.InitTableAccess(def.target_oid);
    if (!access.ok()) {
        return access.status().WithContext("reviving assertion \"" + def.name + "\"");
    }

    // Resolved against the schema as it is **now**, not as it was: a column an
    // ALTER renamed makes the declaration unenforceable, and that has to be a
    // reported failure rather than a silently different constraint.
    std::vector<std::uint16_t> group_cols;
    group_cols.reserve(stmt_value.group_columns.size());
    for (const parser::IndexColumnRef& col : stmt_value.group_columns) {
        auto pos = ResolveColumn(*access.value(), col, stmt_value.table_name);
        if (!pos.ok()) {
            return pos.status().WithContext("reviving assertion \"" + def.name + "\"");
        }
        group_cols.push_back(pos.value());
    }

    std::uint16_t sum_col = 0;
    if (stmt_value.func == parser::AggFunc::kSum) {
        auto pos = ResolveColumn(*access.value(), stmt_value.sum_column,
                                 stmt_value.table_name);
        if (!pos.ok()) {
            return pos.status().WithContext("reviving assertion \"" + def.name + "\"");
        }
        sum_col = pos.value();

        // **The type check `CreateAssertion` makes, made here too**, because this
        // function's header promises it and did not perform it: a `uint64` column
        // does not fit the int64 accumulator a group header keeps (§10), and a
        // non-int64 one is outside v1 at all (§3.1). Inert while `ALTER TABLE`
        // cannot change a column's type - but a false claim about *what a
        // constraint enforces* is the one place this codebase says truthfulness
        // beats convenience, and a revive that skipped it would resume enforcing
        // an aggregate the declaration could not have meant.
        const catalog::SysColumnRow& col = access.value()->schema.columns[sum_col];
        if (col.type_val != catalog::kTypeValInt64) {
            return Status::InvalidArgument(
                "reviving assertion \"" + def.name + "\": its SUM column '" +
                stmt_value.sum_column.name + "' is type_val=" +
                std::to_string(col.type_val) +
                ", and an assertion's SUM column must be int64 (docs/spec/assertion.md §3.1, §10)");
        }
    }

    LiveAssertion live;
    live.assertion_id = def.id;
    live.target_oid = def.target_oid;
    live.name = def.name;
    live.aggregate = stmt_value.func == parser::AggFunc::kSum ? BoundAggregate::kSum
                                                               : BoundAggregate::kCount;
    live.group_cols = group_cols;
    live.sum_col = sum_col;
    live.sum_col_name = stmt_value.sum_column.name;
    for (std::size_t i = 0; i < group_cols.size(); ++i) {
        live.group_col_names.push_back(stmt_value.group_columns[i].name);
        live.group_type_vals.push_back(access.value()->schema.columns[group_cols[i]].type_val);
    }

    // The writer takes over the chain the entries are already on; a fresh one
    // would grow a second chain beside it.
    live.chain = BoundCabinChainWriter(def.id);
    if (Status s = live.chain.AdoptChain(store, def.cabin_root); !s.ok()) {
        return s.WithContext("reviving assertion \"" + def.name + "\"");
    }

    // The directory is deliberately empty: `exec::RecoverAssertions` fills it
    // from the checkpoint snapshot and the records after it (AS6a). Adopting
    // this as-is would enforce against zero, which admits every write.
    live.cabin = BoundCabin(live.aggregate, stmt_value.enforced_max());
    return live;
}

StatusOr<std::uint64_t> DropAssertion(catalog::Catalog& catalog, storage::PageStore& store,
                                      const parser::AssertionStmt& stmt, wal::WalManager* wal) {
    auto found = FindAssertionByName(catalog, store, stmt.name);
    if (!found.ok()) return found.status();
    if (!found.value().has_value()) {
        return Status::NotFound("no assertion named '" + stmt.name + "' (byte " +
                                std::to_string(stmt.byte_offset) + ")");
    }
    const std::uint64_t id = found.value()->id;
    // The record before the row - WAL before data, though the row itself is
    // unlogged: a stream must not end holding live-looking ASSERT records
    // for an assertion whose catalog row is already gone.
    if (Status s = EmitAssertDrop(wal, id, found.value()->cabin_root); !s.ok()) return s;
    if (Status s = DeleteAssertion(catalog, store, wal, stmt.name); !s.ok()) return s;
    // No BumpVersion(), CreateAssertion's reasoning: nothing cached is
    // derived from these rows until AST07's check steps are.
    return id;
}

}  // namespace kds::exec
