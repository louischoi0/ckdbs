#include "kds/exec/plan_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <memory>
#include <unordered_map>
#include <utility>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/declaration_relation.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/lexer.hpp"
#include "kds/parser/parser.hpp"
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
    plan.enabled = (row.flags & kPlanFlagDisabled) == 0;
    plan.readable = (plan.captured ? DecodePlanText(row.source_text, plan)
                                   : DecodeAdminPlanText(row.source_text, plan))
                        .ok();
    if (plan.readable) {
        plan.key.fetch_id = static_cast<std::uint64_t>(row.aux);
    } else {
        plan.statement = std::move(row.source_text);
    }
    return plan;
}

// An object's name folded, as a map's key.
std::string Lowered(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// The head fields a captured and an administrator's row share, checked as
// a pair: the fingerprint version, the oids and the time.
Status ReadShared(std::string_view fp_text, std::string_view oids_text, std::string_view at_text,
                  std::uint64_t& fp, std::vector<std::uint64_t>& oids, std::uint64_t& at,
                  std::size_t min_relations) {
    const auto corrupt = [](std::string_view what) {
        return Status::Corruption("sys.plans: " + std::string(what));
    };
    if (!ReadNumber(fp_text, fp) || !ReadList(oids_text, oids) || !ReadNumber(at_text, at)) {
        return corrupt("a row's version, oids or time is not a number list");
    }
    if (fp > UINT32_MAX || at > static_cast<std::uint64_t>(INT64_MAX)) {
        return corrupt("a row's version or time is out of range");
    }
    // A captured order is a join block's, two relations or more; an
    // administrator's plan may be a one-relation statement's (CX-S3b).
    if (oids.size() < min_relations) return corrupt("a row names too few relations");
    for (const std::uint64_t oid : oids) {
        if (oid > UINT32_MAX) return corrupt("a row names an oid past 32 bits");
    }
    return Status::OK();
}

// Whether `order` names each of `n` written positions once.
bool IsPermutation(const std::vector<std::uint64_t>& order, std::size_t n) {
    if (order.size() != n) return false;
    std::vector<bool> seen(n, false);
    for (const std::uint64_t position : order) {
        if (position >= n || seen[position]) return false;
        seen[position] = true;
    }
    return true;
}

// A variant's pins as its line's ` access=` field, or nothing where it has
// none.
std::string EncodeAccess(const std::vector<ArmPin>& access) {
    std::string out;
    for (std::size_t w = 0; w < access.size(); ++w) {
        const ArmPin& pin = access[w];
        if (pin.arm == ArmPin::Arm::kLadder) continue;
        out.append(out.empty() ? " access=" : ";");
        out.append(std::to_string(w)).push_back(':');
        if (pin.forbid) out.push_back('!');
        out.append(ArmName(pin.arm));
        if (!pin.name.empty()) out.append(":").append(pin.name);
    }
    return out;
}

// A variant's build pins as its line's ` build=` field (CX-S5), after
// `access=`, or nothing where it has none.
std::string EncodeBuild(const std::vector<ArmPin>& access) {
    std::string out;
    for (std::size_t w = 0; w < access.size(); ++w) {
        if (access[w].build == ArmPin::Build::kLadder) continue;
        out.append(out.empty() ? " build=" : ";");
        out.append(std::to_string(w));
        out.append(":").append(BuildName(access[w].build));
    }
    return out;
}

// The field's reading onto `pins`, sized to the plan's relations already;
// false where it is not what `EncodeBuild` writes.
bool DecodeBuild(std::string_view text, std::vector<ArmPin>& pins) {
    while (true) {
        const std::size_t semi = text.find(';');
        const std::string_view item = text.substr(0, semi);
        const std::size_t colon = item.find(':');
        std::uint64_t position = 0;
        if (colon == std::string_view::npos || !ReadNumber(item.substr(0, colon), position) ||
            position >= pins.size() || pins[position].build != ArmPin::Build::kLadder) {
            return false;
        }
        const std::string_view word = item.substr(colon + 1);
        ArmPin& pin = pins[position];
        if (word == BuildName(ArmPin::Build::kOn)) {
            // `ON` beside a keyed pin is what the DDL refuses.
            if (pin.keyed()) return false;
            pin.build = ArmPin::Build::kOn;
        } else if (word == BuildName(ArmPin::Build::kOff)) {
            pin.build = ArmPin::Build::kOff;
        } else {
            return false;
        }
        if (semi == std::string_view::npos) return true;
        text.remove_prefix(semi + 1);
    }
}

// The field's reading into `n` pins by written position; false where it is
// not what `EncodeAccess` writes: a position out of range or pinned twice,
// an unknown pin, a forbidden walk, a name on anything but a pinned index
// or a pinned Cabin.
bool DecodeAccess(std::string_view text, std::size_t n, std::vector<ArmPin>& out) {
    std::vector<ArmPin> pins(n);
    while (true) {
        const std::size_t semi = text.find(';');
        std::string_view item = text.substr(0, semi);
        const std::size_t colon = item.find(':');
        std::uint64_t position = 0;
        if (colon == std::string_view::npos || !ReadNumber(item.substr(0, colon), position) ||
            position >= n || pins[position].pins()) {
            return false;
        }
        item.remove_prefix(colon + 1);
        ArmPin pin;
        if (item.starts_with("!")) {
            pin.forbid = true;
            item.remove_prefix(1);
        }
        const std::size_t name_at = item.find(':');
        // The word as `EncodeAccess` writes it, and no other spelling.
        const std::string_view word = item.substr(0, name_at);
        const auto arm = ArmFromName(word);
        if (!arm.has_value() || ArmName(*arm) != word) return false;
        pin.arm = *arm;
        if (pin.forbid && pin.arm == ArmPin::Arm::kScan) return false;
        if (name_at != std::string_view::npos) {
            const std::string_view name = item.substr(name_at + 1);
            if ((pin.arm != ArmPin::Arm::kIndex && pin.arm != ArmPin::Arm::kCabin) ||
                pin.forbid || !parser::IsIdentifier(name)) {
                return false;
            }
            pin.name = std::string(name);
        }
        pins[position] = std::move(pin);
        if (semi == std::string_view::npos) break;
        text.remove_prefix(semi + 1);
    }
    out = std::move(pins);
    return true;
}

std::shared_ptr<const AdminPlan> ToAdminPlan(const PlanRow& row) {
    auto plan = std::make_shared<AdminPlan>();
    plan->name = row.name;
    plan->key = row.key;
    plan->variants = row.variants;
    return plan;
}

DeclarationRow ToDeclaration(const PlanRow& row, std::int64_t now_us) {
    DeclarationRow out;
    out.id = row.id;
    out.target_oid = row.key.oids[0];
    out.aux = static_cast<std::int64_t>(row.key.fetch_id);
    out.flags = row.enabled ? 0 : kPlanFlagDisabled;
    out.name = row.name;
    out.source_text = EncodeAdminPlanText(row, now_us);
    return out;
}

}  // namespace

std::string EncodeAdminPlanText(const PlanRow& row, std::int64_t written_at_us) {
    std::string text(kVersion);
    text.append(" fp=").append(std::to_string(parser::kFingerprintVersion));
    AppendList(text, " oids=", row.key.oids, row.key.n);
    text.append(" bindings=");
    for (std::size_t i = 0; i < row.bindings.size(); ++i) {
        if (i != 0) text.push_back(',');
        text.append(row.bindings[i]);
    }
    text.append(" args=").append(std::to_string(row.arguments));
    text.append(" at=").append(std::to_string(written_at_us));
    text.append(" variants=").append(std::to_string(row.variants.size()));
    text.push_back('\n');
    for (const PlanVariant& variant : row.variants) {
        text.append(variant.name);
        AppendList(text, " ", variant.order, row.key.n);
        text.append(EncodeAccess(variant.access));
        text.append(EncodeBuild(variant.access));
        if (variant.rule.is_else()) {
            text.append(" ELSE");
        } else {
            text.append(" WHEN ").append(PlanRuleText(variant.rule));
        }
        text.push_back('\n');
    }
    // The statement is for a reader and is cut to what one var-heap value
    // has left; a head and rules that leave none are the insert's refusal.
    if (text.size() < varheap::kMaxValueSize) {
        text.append(std::string_view(row.statement)
                        .substr(0, varheap::kMaxValueSize - text.size()));
    }
    return text;
}

Status DecodeAdminPlanText(std::string_view text, PlanRow& out) {
    const auto corrupt = [](std::string_view what) {
        return Status::Corruption("sys.plans: " + std::string(what));
    };
    const auto take_line = [&](std::string_view& line) {
        const std::size_t newline = text.find('\n');
        if (newline == std::string_view::npos) return false;
        line = text.substr(0, newline);
        text.remove_prefix(newline + 1);
        return true;
    };
    std::string_view head;
    if (!take_line(head)) return corrupt("a plan's text has no head line");
    if (!head.starts_with(kVersion) || head.size() <= kVersion.size() ||
        head[kVersion.size()] != ' ') {
        return corrupt("a plan's text is not format v1");
    }
    head.remove_prefix(kVersion.size() + 1);
    std::string_view fp_text;
    std::string_view oids_text;
    std::string_view bindings_text;
    std::string_view args_text;
    std::string_view at_text;
    std::string_view variants_text;
    if (head.ends_with(' ') || !TakeField(head, "fp", fp_text) ||
        !TakeField(head, "oids", oids_text) || !TakeField(head, "bindings", bindings_text) ||
        !TakeField(head, "args", args_text) || !TakeField(head, "at", at_text) ||
        !TakeField(head, "variants", variants_text) || !head.empty()) {
        return corrupt(
            "a plan's text does not read fp, oids, bindings, args, at and variants, in that "
            "order");
    }
    std::vector<std::uint64_t> oids;
    std::uint64_t fp = 0;
    std::uint64_t at = 0;
    if (Status s = ReadShared(fp_text, oids_text, at_text, fp, oids, at, 1); !s.ok()) return s;
    std::uint64_t arguments = 0;
    std::uint64_t count = 0;
    if (!ReadNumber(args_text, arguments) || arguments > UINT32_MAX ||
        !ReadNumber(variants_text, count) || count < 1 || count > kMaxPlanVariants) {
        return corrupt("a plan's argument or variant count is out of range");
    }
    // One binding a relation, each a name and no two the same.
    std::vector<std::string> bindings;
    while (true) {
        const std::size_t comma = bindings_text.find(',');
        const std::string_view binding = bindings_text.substr(0, comma);
        if (!parser::IsIdentifier(binding)) return corrupt("a plan's binding is not a name");
        for (const std::string& earlier : bindings) {
            if (SameName(earlier, binding)) {
                return corrupt("a plan names one binding twice");
            }
        }
        bindings.emplace_back(binding);
        if (comma == std::string_view::npos) break;
        bindings_text.remove_prefix(comma + 1);
    }
    if (bindings.size() != oids.size()) return corrupt("a plan's bindings do not cover its relations");

    std::vector<PlanVariant> variants;
    for (std::uint64_t v = 0; v < count; ++v) {
        std::string_view line;
        if (!take_line(line)) return corrupt("a plan's text ends inside its variants");
        const std::size_t name_end = line.find(' ');
        if (name_end == std::string_view::npos) return corrupt("a variant has no order");
        PlanVariant variant;
        variant.name = std::string(line.substr(0, name_end));
        if (!parser::IsIdentifier(variant.name)) return corrupt("a variant's name is not a name");
        for (const PlanVariant& earlier : variants) {
            if (SameName(earlier.name, variant.name)) {
                return corrupt("a plan names one variant twice");
            }
        }
        line.remove_prefix(name_end + 1);
        const std::size_t order_end = line.find(' ');
        if (order_end == std::string_view::npos) return corrupt("a variant has no rule");
        std::vector<std::uint64_t> order;
        if (!ReadList(line.substr(0, order_end), order) || !IsPermutation(order, oids.size())) {
            return corrupt("a variant's order is not a permutation of the plan's relations");
        }
        for (std::size_t i = 0; i < order.size(); ++i) {
            variant.order[i] = static_cast<std::uint8_t>(order[i]);
        }
        line.remove_prefix(order_end + 1);
        if (line.starts_with("access=")) {
            const std::size_t access_end = line.find(' ');
            if (access_end == std::string_view::npos ||
                !DecodeAccess(line.substr(7, access_end - 7), oids.size(), variant.access)) {
                return corrupt("a variant's access pins do not read");
            }
            line.remove_prefix(access_end + 1);
        }
        if (line.starts_with("build=")) {
            const std::size_t build_end = line.find(' ');
            if (variant.access.empty()) variant.access.resize(oids.size());
            if (build_end == std::string_view::npos ||
                !DecodeBuild(line.substr(6, build_end - 6), variant.access)) {
                return corrupt("a variant's build pins do not read");
            }
            line.remove_prefix(build_end + 1);
        }
        // `ELSE` last and only last (CX-R4).
        const bool last = v + 1 == count;
        if (line == "ELSE") {
            if (!last) return corrupt("a plan's ELSE variant is not its last");
        } else {
            if (last || !line.starts_with("WHEN ")) {
                return corrupt("a plan's last variant is not ELSE, or a variant has no rule");
            }
            parser::Lexer lexer(line.substr(5));
            auto rule = ParsePlanRule(lexer);
            if (!rule.ok() || lexer.Next().type != parser::TokenType::kEof) {
                return corrupt("a variant's rule does not read");
            }
            if (rule.value().max_arg() > arguments) {
                return corrupt("a variant's rule reads past the statement's arguments");
            }
            variant.rule = std::move(rule.value());
        }
        variants.push_back(std::move(variant));
    }
    // Nothing of `out` is written before this line: a refused text leaves it.
    out.fingerprint_version = static_cast<std::uint32_t>(fp);
    out.key = CapturedKey{};
    out.key.n = static_cast<std::uint8_t>(oids.size());
    for (std::size_t i = 0; i < oids.size(); ++i) {
        out.key.oids[i] = static_cast<catalog::Oid>(oids[i]);
    }
    out.order = CapturedOrder{};
    out.bindings = std::move(bindings);
    out.arguments = static_cast<std::uint32_t>(arguments);
    out.variants = std::move(variants);
    out.written_at_us = static_cast<std::int64_t>(at);
    out.statement = std::string(text);
    return Status::OK();
}

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
    if (Status s = ReadShared(fp_text, oids_text, at_text, fp, oids, at, 2); !s.ok()) return s;
    // An order names each written position once.
    if (!ReadList(order_text, order) || !IsPermutation(order, oids.size())) {
        return corrupt("a row's order is not a permutation of its relations");
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
    // A captured row only: an administrator's plan over a dropped relation
    // stays, `invalid`, until its `DROP PLAN` (`plan_catalog.hpp`).
    for (const PlanRow& plan : plans.value()) {
        if (plan.captured && plan.readable &&
            std::any_of(dropped.begin(), dropped.end(),
                        [&](catalog::Oid oid) { return plan.key.Names(oid); })) {
            gone.push_back(plan.id);
        }
    }
    // A row this step could not remove is inert, and the next mount removes
    // it.
    auto removed = DeleteDeclarationsById(catalog, store, wal, catalog::kSysPlansTable, gone);
    return removed.ok() ? failed : removed.status();
}

bool PlanRowCanMatch(catalog::Catalog& catalog, const PlanRow& row) {
    auto tables = catalog.ListTables(nullptr);
    return !tables.ok() || PlanRowCanMatch(catalog, row, tables.value());
}

bool PlanRowCanMatch(catalog::Catalog& catalog, const PlanRow& row,
                     const std::vector<catalog::SysObjectRow>& tables) {
    if (!row.readable || row.key.n == 0) return true;

    // The stored text is the statement's shape (`parser::ShapeText`): every
    // literal a `?`, which the native grammar does not read. Each is read as
    // a literal of the same width, so the parse sees the statement's names
    // where they were written.
    std::string text = row.statement;
    {
        parser::Lexer lexer(row.statement);
        for (parser::Token tok = lexer.Next(); tok.type != parser::TokenType::kEof;
             tok = lexer.Next()) {
            if (tok.type == parser::TokenType::kParam) text[tok.byte_offset] = '0';
        }
    }
    parser::Parser parser(text);
    auto parsed = parser.Parse();
    // A statement stored cut short, or one this grammar no longer reads:
    // nothing is established.
    if (!parsed.ok()) return true;
    const auto* stmt = std::get_if<parser::SelectStmt>(&parsed.value());
    if (stmt == nullptr) return true;
    std::vector<const parser::RelationRef*> refs;
    refs.push_back(&stmt->from);
    for (const parser::JoinClause& join : stmt->joins) refs.push_back(&join.relation);
    if (refs.size() != row.key.n) return true;
    // **The text must be the statement the key was made from, whole**: a row
    // stores it cut to one var-heap value, and a cut that falls inside a
    // name can still parse - as another name. A literal, a `?` and the `0`
    // read in its place are one marker to the fingerprint, so a whole text
    // hashes to the key's fetch id and a cut one does not.
    const auto fingerprint = parser.fingerprint();
    if (!fingerprint.has_value() || fingerprint->fetch_id != row.key.fetch_id) return true;

    // ---- The relations: each written name against the recorded oid's ------
    std::vector<const catalog::TableAccess*> relations(refs.size(), nullptr);
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const auto object =
            std::find_if(tables.begin(), tables.end(), [&](const catalog::SysObjectRow& t) {
                return t.oid == row.key.oids[i];
            });
        // Gone, or not listed: `PlanRowIsValid`'s to say, not this check's.
        if (object == tables.end()) return true;
        if (!SameName(refs[i]->table_name, catalog::NameView(object->name))) return false;
        if (!refs[i]->schema.empty() &&
            !SameName(refs[i]->schema, catalog.NamespaceSpelling(object->namespace_oid))) {
            return false;
        }
        auto access = catalog.InitTableAccess(row.key.oids[i]);
        if (access.ok()) relations[i] = access.value();
    }

    // ---- The columns written `binding.column` -----------------------------
    //
    // Token by token: an identifier, a dot, an identifier, where the first
    // is one of the statement's bindings and does not follow `FROM` or
    // `JOIN` (there it is a namespace qualifying a relation). An
    // unqualified column is not checked - which relation it means is the
    // compiler's to say.
    parser::Lexer lexer(text);
    parser::Token before{};
    parser::Token tok = lexer.Next();
    const auto word = [](const parser::Token& t) {
        return t.type == parser::TokenType::kIdent || t.type == parser::TokenType::kKeyword;
    };
    while (tok.type != parser::TokenType::kEof) {
        const bool relation_position =
            word(before) && (SameName(before.text, "FROM") || SameName(before.text, "JOIN"));
        if (word(tok) && !relation_position && lexer.Peek().type == parser::TokenType::kDot) {
            const parser::Token qualifier = tok;
            lexer.Next();  // the '.'
            const parser::Token column = lexer.Next();
            if (word(column)) {
                for (std::size_t i = 0; i < refs.size(); ++i) {
                    if (relations[i] == nullptr) continue;
                    if (!SameName(qualifier.text, refs[i]->binding())) continue;
                    bool found = false;
                    // **A dropped column still counts**: this check establishes
                    // a rename, which is decided when it is made (`alter.md`
                    // AL6), and a `DROP COLUMN` is a transaction's - undecided
                    // it would read as a rename that a rollback then undoes,
                    // after a drop was let through on it.
                    for (const auto& c : relations[i]->schema.columns) {
                        found = found || SameName(column.text, catalog::NameView(c.name));
                    }
                    if (!found) return false;
                }
            }
            before = column;
            tok = lexer.Next();
            continue;
        }
        before = tok;
        tok = lexer.Next();
    }
    return true;
}

bool PlanRowIsValid(catalog::Catalog& catalog, const PlanRow& row) {
    if (!row.readable || row.fingerprint_version != parser::kFingerprintVersion) return false;
    for (std::size_t i = 0; i < row.key.n; ++i) {
        auto access = catalog.InitTableAccess(row.key.oids[i]);
        if (!access.ok() && access.status().code() == StatusCode::kNotFound) return false;
    }
    return true;
}

StatusOr<std::size_t> LoadPlans(catalog::Catalog& catalog, storage::PageStore& store,
                                wal::WalManager* wal, CapturedPlans& table, AdminPlans& admin) {
    auto plans = ListPlans(catalog, store);
    if (!plans.ok()) return plans.status();
    std::size_t unreadable = 0;
    std::vector<std::uint64_t> remove;
    // ---- The administrator's rows ----------------------------------------
    // The newest row of a name is the plan: an `ALTER PLAN` a crash cut
    // short left its old row beside the new one (`ReplaceAdminPlan`).
    std::unordered_map<std::string, const PlanRow*> newest;
    for (const PlanRow& plan : plans.value()) {
        if (plan.captured) continue;
        if (!plan.readable) {
            ++unreadable;
            continue;
        }
        auto [at, fresh] = newest.try_emplace(Lowered(plan.name), &plan);
        if (fresh) continue;
        remove.push_back(std::min(at->second->id, plan.id));
        if (plan.id > at->second->id) at->second = &plan;
    }
    // In id order, so which plan a shape runs never depends on a hash's
    // order: the DDL admits one plan a shape, and were two rows ever to
    // share one, the newer is the one installed last.
    std::vector<const PlanRow*> kept;
    for (const auto& [name, plan] : newest) kept.push_back(plan);
    std::sort(kept.begin(), kept.end(),
              [](const PlanRow* a, const PlanRow* b) { return a->id < b->id; });
    for (const PlanRow* plan : kept) {
        // An invalid plan is the administrator's to drop, not a mount's.
        if (PlanRowIsValid(catalog, *plan)) admin.Install(ToAdminPlan(*plan), plan->enabled);
    }
    // ---- The captured rows -----------------------------------------------
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

namespace {

// Every row named `name`, the newest last: one, but where a crash cut an
// `ALTER PLAN` short.
StatusOr<std::vector<PlanRow>> RowsNamed(catalog::Catalog& catalog, storage::PageStore& store,
                                         std::string_view name) {
    auto plans = ListPlans(catalog, store);
    if (!plans.ok()) return plans.status();
    std::vector<PlanRow> named;
    for (PlanRow& plan : plans.value()) {
        if (SameName(plan.name, name)) named.push_back(std::move(plan));
    }
    if (named.empty()) {
        return Status::NotFound("no plan named \"" + std::string(name) + "\"");
    }
    std::sort(named.begin(), named.end(),
              [](const PlanRow& a, const PlanRow& b) { return a.id < b.id; });
    return named;
}

}  // namespace

StatusOr<PlanRow> FindPlan(catalog::Catalog& catalog, storage::PageStore& store,
                           std::string_view name) {
    auto named = RowsNamed(catalog, store, name);
    if (!named.ok()) return named.status();
    return std::move(named.value().back());
}

StatusOr<PlanRow> CreateAdminPlan(catalog::Catalog& catalog, storage::PageStore& store,
                                  wal::WalManager* wal, AdminPlans& admin, PlanRow row,
                                  std::int64_t now_us) {
    auto id = catalog.AllocateRowId(catalog::kSysPlansTable);
    if (!id.ok()) return id.status();
    row.id = id.value();
    row.captured = false;
    row.readable = true;
    row.fingerprint_version = parser::kFingerprintVersion;
    row.written_at_us = now_us;
    if (Status s = InsertDeclaration(catalog, store, wal, catalog::kSysPlansTable, kKind,
                                     ToDeclaration(row, now_us));
        !s.ok()) {
        return s;
    }
    admin.Install(ToAdminPlan(row), row.enabled);
    return row;
}

StatusOr<PlanRow> ReplaceAdminPlan(catalog::Catalog& catalog, storage::PageStore& store,
                                   wal::WalManager* wal, AdminPlans& admin,
                                   std::uint64_t old_id, PlanRow next, std::int64_t now_us) {
    auto id = catalog.AllocateRowId(catalog::kSysPlansTable);
    if (!id.ok()) return id.status();
    next.id = id.value();
    next.written_at_us = now_us;
    // The name is the old row's, which is still there: the check would
    // refuse it, and the DDL claim is what keeps the name this plan's.
    if (Status s = InsertDeclaration(catalog, store, wal, catalog::kSysPlansTable, kKind,
                                     ToDeclaration(next, now_us),
                                     DeclarationName::kUniqueByConstruction);
        !s.ok()) {
        return s;
    }
    // From here the new row is the plan, whatever becomes of the old one: it
    // has the higher id, which is what a mount and `FindPlan` go by.
    admin.Install(ToAdminPlan(next), next.enabled);
    const std::uint64_t old_ids[] = {old_id};
    if (auto removed = DeleteDeclarationsById(catalog, store, wal, catalog::kSysPlansTable,
                                              old_ids);
        !removed.ok()) {
        return removed.status();
    }
    return next;
}

StatusOr<PlanRow> DropPlan(catalog::Catalog& catalog, storage::PageStore& store,
                           wal::WalManager* wal, CapturedPlans& table, AdminPlans& admin,
                           std::string_view name) {
    auto named = RowsNamed(catalog, store, name);
    if (!named.ok()) return named.status();
    std::vector<std::uint64_t> ids;
    for (const PlanRow& row : named.value()) ids.push_back(row.id);
    auto removed = DeleteDeclarationsById(catalog, store, wal, catalog::kSysPlansTable, ids);
    if (!removed.ok()) return removed.status();
    PlanRow plan = std::move(named.value().back());
    if (plan.readable) {
        if (plan.captured) {
            table.Erase(plan.key);
        } else if (admin.Holder(plan.key) == plan.name) {
            // The shape's plan is this one - not another's made for the
            // shape while this row stood invalid.
            admin.Erase(plan.key);
        }
    }
    return plan;
}

}  // namespace kds::exec
