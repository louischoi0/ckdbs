#include "kds/exec/plan_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/declaration_relation.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/storage/varheap.hpp"

namespace kds::exec {

namespace {

inline constexpr std::string_view kKind = "plan";
inline constexpr std::string_view kVersion = "v1";

void AppendList(std::string& out, std::string_view field, const auto& values, std::size_t n) {
    out.append(field);
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) out.push_back(',');
        out.append(std::to_string(static_cast<std::uint64_t>(values[i])));
    }
}

// The unsigned decimal `text` is, whole; false otherwise.
bool ReadNumber(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty()) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && end == text.data() + text.size();
}

// A comma-separated list of up to `kMaxPlannedRelations` numbers.
bool ReadList(std::string_view text, std::vector<std::uint64_t>& out) {
    out.clear();
    while (true) {
        const std::size_t comma = text.find(',');
        std::uint64_t value = 0;
        if (!ReadNumber(text.substr(0, comma), value)) return false;
        if (out.size() == kMaxPlannedRelations) return false;
        out.push_back(value);
        if (comma == std::string_view::npos) return true;
        text.remove_prefix(comma + 1);
    }
}

// `<field>=<value>` at the head of `line`, followed by a space or its end;
// the value, with `line` moved past it.
bool TakeField(std::string_view& line, std::string_view field, std::string_view& value) {
    if (!line.starts_with(field) || line.size() <= field.size() || line[field.size()] != '=') {
        return false;
    }
    line.remove_prefix(field.size() + 1);
    const std::size_t space = line.find(' ');
    value = line.substr(0, space);
    line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
    return true;
}

PlanRow ToPlanRow(DeclarationRow row) {
    PlanRow plan;
    plan.id = row.id;
    plan.name = std::move(row.name);
    plan.captured = (row.flags & kPlanFlagCaptured) != 0;
    plan.readable = DecodePlanText(row.source_text, plan).ok();
    if (plan.readable) {
        plan.key.fetch_id = static_cast<std::uint64_t>(row.aux);
    } else {
        plan.statement = std::move(row.source_text);
    }
    return plan;
}

}  // namespace

std::string EncodePlanText(const CapturedPlan& plan, std::int64_t written_at_us) {
    std::string text(kVersion);
    text.append(" fp=").append(std::to_string(parser::kFingerprintVersion));
    AppendList(text, " oids=", plan.key.oids, plan.key.n);
    AppendList(text, " order=", plan.order, plan.key.n);
    text.append(" at=").append(std::to_string(written_at_us));
    text.push_back('\n');
    // One var-heap value holds a row's text (`InsertDeclaration`), so a
    // statement past what is left is cut: the text is for a reader, and the
    // key and the order before it are what a mount loads.
    const std::size_t room = varheap::kMaxValueSize - text.size();
    text.append(std::string_view(plan.statement).substr(0, room));
    return text;
}

Status DecodePlanText(std::string_view text, PlanRow& out) {
    const auto corrupt = [](std::string_view what) {
        return Status::Corruption("sys.plans: " + std::string(what));
    };
    const std::size_t newline = text.find('\n');
    if (newline == std::string_view::npos) return corrupt("a row's text has no statement line");
    std::string_view head = text.substr(0, newline);
    if (!head.starts_with(kVersion) || head.size() <= kVersion.size() ||
        head[kVersion.size()] != ' ') {
        return corrupt("a row's text is not format v1");
    }
    head.remove_prefix(kVersion.size() + 1);
    std::string_view fp_text;
    std::string_view oids_text;
    std::string_view order_text;
    std::string_view at_text;
    // `TakeField` eats the space after a value, so a line ending in one would
    // read as whole.
    if (head.ends_with(' ') || !TakeField(head, "fp", fp_text) ||
        !TakeField(head, "oids", oids_text) || !TakeField(head, "order", order_text) ||
        !TakeField(head, "at", at_text) || !head.empty()) {
        return corrupt("a row's text does not read fp, oids, order and at, in that order");
    }
    std::vector<std::uint64_t> oids;
    std::vector<std::uint64_t> order;
    std::uint64_t fp = 0;
    std::uint64_t at = 0;
    if (!ReadNumber(fp_text, fp) || !ReadList(oids_text, oids) || !ReadList(order_text, order) ||
        !ReadNumber(at_text, at)) {
        return corrupt("a row's version, oids, order or time is not a number list");
    }
    if (fp > UINT32_MAX || at > static_cast<std::uint64_t>(INT64_MAX)) {
        return corrupt("a row's version or time is out of range");
    }
    // A join block has two relations or more, and an order names each
    // written position once.
    if (oids.size() < 2 || order.size() != oids.size()) {
        return corrupt("a row's order does not cover its relations");
    }
    std::vector<bool> seen(oids.size(), false);
    for (std::size_t i = 0; i < oids.size(); ++i) {
        if (oids[i] > UINT32_MAX) return corrupt("a row names an oid past 32 bits");
        if (order[i] >= oids.size() || seen[order[i]]) {
            return corrupt("a row's order is not a permutation of its relations");
        }
        seen[order[i]] = true;
    }
    // Nothing of `out` is written before this line: a refused text leaves it.
    out.fingerprint_version = static_cast<std::uint32_t>(fp);
    out.key = CapturedKey{};
    out.order = CapturedOrder{};
    out.key.n = static_cast<std::uint8_t>(oids.size());
    for (std::size_t i = 0; i < oids.size(); ++i) {
        out.key.oids[i] = static_cast<catalog::Oid>(oids[i]);
        out.order[i] = static_cast<std::uint8_t>(order[i]);
    }
    out.written_at_us = static_cast<std::int64_t>(at);
    out.statement = std::string(text.substr(newline + 1));
    return Status::OK();
}

StatusOr<std::vector<PlanRow>> ListPlans(catalog::Catalog& catalog, storage::PageStore& store) {
    auto rows = ScanDeclarations(catalog, store, catalog::kSysPlansTable);
    if (!rows.ok()) return rows.status();
    std::vector<PlanRow> out;
    out.reserve(rows.value().size());
    for (DeclarationRow& row : rows.value()) out.push_back(ToPlanRow(std::move(row)));
    return out;
}

Status PersistCapturedPlans(catalog::Catalog& catalog, storage::PageStore& store,
                            wal::WalManager* wal, CapturedPlans& table, std::int64_t now_us,
                            std::size_t max_rows) {
    Status failed = Status::OK();
    std::vector<CapturedPlan> pending = table.TakePending(max_rows);
    std::size_t written = 0;
    for (; written < pending.size(); ++written) {
        const CapturedPlan& plan = pending[written];
        auto id = catalog.AllocateRowId(catalog::kSysPlansTable);
        if (!id.ok()) {
            failed = id.status();
            break;
        }
        DeclarationRow row;
        row.id = id.value();
        row.target_oid = plan.key.oids[0];
        row.aux = static_cast<std::int64_t>(plan.key.fetch_id);
        row.flags = kPlanFlagCaptured;
        // The row id makes the name unique: one fetch id is two shapes when
        // one statement text binds other relations - another namespace's,
        // or a relation created again under its name. And so no insert pays
        // the name check's scan of the relation.
        row.name = "captured_" + std::to_string(plan.key.fetch_id) + "_" +
                   std::to_string(row.id);
        row.source_text = EncodePlanText(plan, now_us);
        if (Status s = InsertDeclaration(catalog, store, wal, catalog::kSysPlansTable, kKind, row,
                                         DeclarationName::kUniqueByConstruction);
            !s.ok()) {
            failed = s;
            break;
        }
    }
    table.CountPersisted(written);
    if (written < pending.size()) {
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(written));
        table.Requeue(std::move(pending));
    }

    // **After the writes, never before**: a relation dropped while one of
    // its captures was out with this step has its oid in the list by the
    // time the row is written, or in the next step's.
    const std::vector<catalog::Oid> dropped = table.TakeForgotten();
    if (dropped.empty()) return failed;
    auto plans = ListPlans(catalog, store);
    if (!plans.ok()) return plans.status();
    std::vector<std::uint64_t> gone;
    for (const PlanRow& plan : plans.value()) {
        if (plan.readable && std::any_of(dropped.begin(), dropped.end(), [&](catalog::Oid oid) {
                return plan.key.Names(oid);
            })) {
            gone.push_back(plan.id);
        }
    }
    // A row this step could not remove is inert, and the next mount removes
    // it.
    auto removed = DeleteDeclarationsById(catalog, store, wal, catalog::kSysPlansTable, gone);
    return removed.ok() ? failed : removed.status();
}

StatusOr<std::size_t> LoadCapturedPlans(catalog::Catalog& catalog, storage::PageStore& store,
                                        wal::WalManager* wal, CapturedPlans& table) {
    auto plans = ListPlans(catalog, store);
    if (!plans.ok()) return plans.status();
    std::size_t unreadable = 0;
    std::vector<std::uint64_t> remove;
    for (const PlanRow& plan : plans.value()) {
        if (!plan.captured) continue;
        if (!plan.readable) {
            ++unreadable;
            continue;
        }
        // A shape keyed by another fingerprint version matches no statement
        // of this build: its fetch id is another function's.
        if (plan.fingerprint_version != parser::kFingerprintVersion) {
            remove.push_back(plan.id);
            continue;
        }
        // `NotFound` is a relation that is gone - its row goes too. Any
        // other failure says nothing of the relation: the row stays and is
        // not loaded, so its shape is captured again in this run.
        bool gone = false;
        bool unknown = false;
        for (std::size_t i = 0; i < plan.key.n; ++i) {
            auto access = catalog.InitTableAccess(plan.key.oids[i]);
            if (access.ok()) continue;
            (access.status().code() == StatusCode::kNotFound ? gone : unknown) = true;
        }
        if (unknown && !gone) continue;
        // **One row a shape**: a second row for a key the table already
        // holds - a shape captured again in a run whose mount could not load
        // its first row - goes, or which of the two a later mount met first
        // would decide the order in force.
        if (gone || table.Find(plan.key).has_value()) {
            remove.push_back(plan.id);
            continue;
        }
        table.Load(plan.key, plan.order);
    }
    auto removed = DeleteDeclarationsById(catalog, store, wal, catalog::kSysPlansTable, remove);
    if (!removed.ok()) return removed.status();
    return unreadable;
}

StatusOr<PlanRow> DropPlan(catalog::Catalog& catalog, storage::PageStore& store,
                           wal::WalManager* wal, CapturedPlans& table, std::string_view name) {
    auto row = DeleteDeclaration(catalog, store, wal, catalog::kSysPlansTable, kKind, name);
    if (!row.ok()) return row.status();
    PlanRow plan = ToPlanRow(std::move(row.value()));
    if (plan.captured && plan.readable) table.Erase(plan.key);
    return plan;
}

}  // namespace kds::exec
