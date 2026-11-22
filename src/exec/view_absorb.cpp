#include "kds/exec/view_absorb.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kds/parser/parser.hpp"

// Concurrency: none of its own. It reads the catalog through the reader's
// view (`Catalog::ResolveView`, `GetView`) and builds a new block; the input
// is never written (`step_vm.cpp`: the AST is shared, `shared_ptr`).

namespace kds::exec {
namespace {

std::string At(std::uint32_t byte) { return " (byte " + std::to_string(byte) + ")"; }

// Everything a spliced body's text carries, stamped onto each of its nodes:
// its lens, where its refusals point, the namespace and the reads its
// relations resolve under, and the path its relations are named by.
struct Stamp {
    std::uint32_t lens = 0;
    std::uint32_t byte = 0;  // the view's reference in the reader's text
    std::uint64_t namespace_oid = 0;
    std::shared_ptr<const parser::ViewReads> reads;
    std::string path;  // "v.", "v.w."
};

void StampBlock(parser::SelectStmt& block, const Stamp& stamp);

void StampName(parser::ColumnName& name, const Stamp& stamp) {
    name.lens = stamp.lens;
    name.byte_offset = stamp.byte;
}

void StampRelation(parser::RelationRef& rel, const Stamp& stamp) {
    rel.lens = stamp.lens;
    rel.byte_offset = stamp.byte;
    // DP-Q21: an unqualified name in a body means its view's namespace.
    if (rel.schema.empty()) rel.namespace_oid = stamp.namespace_oid;
    rel.reads = stamp.reads;
    rel.path = stamp.path + rel.path;
}

void StampCondition(parser::Condition& cond, const Stamp& stamp);

parser::BoolExpr StampTree(const parser::BoolExpr& tree, const Stamp& stamp) {
    parser::BoolExpr out = tree;
    out.byte_offset = stamp.byte;
    if (out.op == parser::BoolExpr::Op::kLeaf) StampCondition(out.leaf, stamp);
    for (parser::BoolExpr& child : out.children) child = StampTree(child, stamp);
    return out;
}

void StampCondition(parser::Condition& cond, const Stamp& stamp) {
    StampName(cond.col, stamp);
    StampName(cond.rhs_col, stamp);
    for (std::optional<parser::FunctionCall>* fn : {&cond.lhs_fn, &cond.rhs_fn}) {
        if (!fn->has_value()) continue;
        (*fn)->byte_offset = stamp.byte;
        for (parser::ColumnName& arg : (*fn)->args) StampName(arg, stamp);
    }
    // Shared nodes are copied before they are stamped: the parse that owns
    // them is the view's, and nothing writes a node another tree may hold.
    if (cond.subquery != nullptr) {
        auto copy = std::make_shared<parser::SelectStmt>(*cond.subquery);
        StampBlock(*copy, stamp);
        cond.subquery = std::move(copy);
    }
    if (cond.tree != nullptr) {
        cond.tree = std::make_shared<const parser::BoolExpr>(StampTree(*cond.tree, stamp));
    }
}

void StampBlock(parser::SelectStmt& block, const Stamp& stamp) {
    for (parser::ColumnName& item : block.projection) StampName(item, stamp);
    StampRelation(block.from, stamp);
    for (parser::JoinClause& join : block.joins) {
        StampRelation(join.relation, stamp);
        StampName(join.left, stamp);
        StampName(join.right, stamp);
    }
    for (parser::Condition& cond : block.where) StampCondition(cond, stamp);
    for (parser::SelectItem& item : block.agg_items) StampName(item.column, stamp);
    for (parser::ColumnName& key : block.group_by) StampName(key, stamp);
    for (parser::HavingCondition& having : block.having) StampName(having.agg.column, stamp);
    for (parser::SortKey& key : block.order_by) StampName(key.key.column, stamp);
}

StatusOr<std::optional<parser::SelectStmt>> AbsorbAt(catalog::Catalog& catalog,
                                                     const parser::SelectStmt& stmt,
                                                     const txn::ReadView* view,
                                                     catalog::Oid default_namespace,
                                                     std::uint32_t& next_lens,
                                                     std::uint32_t depth);

// One view's body, read, stamped and itself absorbed: the block to splice
// at `ref`, and the view as the reader's names reach it.
struct Expanded {
    parser::SelectStmt body;
    parser::AbsorbedView view;
};

StatusOr<Expanded> Expand(catalog::Catalog& catalog, const catalog::ViewDef& def,
                          const parser::RelationRef& ref, const txn::ReadView* view,
                          std::uint32_t& next_lens, std::uint32_t depth) {
    const std::string at = At(ref.byte_offset);
    if (!def.absorbable) {
        // DP-Q2: the query layer's, as a derived table, nothing pushed in.
        return Status::NotImplemented("view '" + def.name + "'" + at +
                                      " is not absorbable, so it is answered by the query layer "
                                      "as a derived table");
    }
    if (depth > kMaxViewDepth) {
        return Status::Unsupported("views nest deeper than " + std::to_string(kMaxViewDepth) +
                                   " at view '" + def.name + "'" + at);
    }
    parser::Parser parser(def.text);
    auto parsed = parser.Parse();
    if (!parsed.ok() || !std::holds_alternative<parser::SelectStmt>(parsed.value())) {
        return Status::Corruption("view '" + def.name + "' is marked absorbable and its text "
                                  "is no native SELECT");
    }
    Expanded out;
    out.body = std::move(std::get<parser::SelectStmt>(parsed.value()));
    if (out.body.star() || out.body.projection.size() != def.columns.size()) {
        return Status::Corruption("view '" + def.name + "' names " +
                                  std::to_string(def.columns.size()) +
                                  " columns and its absorbable text returns another list");
    }

    Stamp stamp;
    stamp.lens = ++next_lens;
    stamp.byte = ref.byte_offset;
    stamp.namespace_oid = def.namespace_oid;
    stamp.reads = ReadsOf(def);
    stamp.path = ref.display() + ".";
    StampBlock(out.body, stamp);

    // The body's own views, inside out (DP-Q8).
    auto nested = AbsorbAt(catalog, out.body, view, def.namespace_oid, next_lens, depth);
    if (!nested.ok()) return nested.status();
    if (nested.value().has_value()) out.body = std::move(*nested.value());

    out.view.oid = def.oid;
    out.view.lens = ref.lens;
    out.view.binding = ref.binding();
    for (std::size_t i = 0; i < def.columns.size(); ++i) {
        out.view.columns.emplace_back(def.columns[i], out.body.projection[i]);
    }
    return out;
}

StatusOr<std::optional<parser::SelectStmt>> AbsorbAt(catalog::Catalog& catalog,
                                                     const parser::SelectStmt& stmt,
                                                     const txn::ReadView* view,
                                                     catalog::Oid default_namespace,
                                                     std::uint32_t& next_lens,
                                                     std::uint32_t depth) {
    if (!catalog.AnyView()) return std::optional<parser::SelectStmt>();

    // Which written relations are views, before anything is built: a block
    // that names none leaves as it came, at the cost of these lookups.
    std::vector<const parser::RelationRef*> refs;
    refs.push_back(&stmt.from);
    for (const parser::JoinClause& join : stmt.joins) refs.push_back(&join.relation);
    std::vector<std::optional<catalog::ViewDef>> defs(refs.size());
    bool any = false;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const parser::RelationRef& ref = *refs[i];
        const catalog::Oid ns = ref.namespace_oid != 0 ? ref.namespace_oid : default_namespace;
        auto oid = catalog.ResolveView(ref.schema, ref.table_name, ns, view);
        if (!oid.ok()) return oid.status();
        if (!oid.value().has_value()) continue;
        // A body's view must be one it was created over (DP-Q22 (b)).
        if (Status s = CheckViewReads(ref, *oid.value()); !s.ok()) return s;
        auto def = catalog.GetView(*oid.value(), view);
        if (!def.ok()) return def.status();
        defs[i].emplace(std::move(def.value()));
        any = true;
    }
    if (!any) return std::optional<parser::SelectStmt>();

    parser::SelectStmt out = stmt;
    out.joins.clear();
    std::vector<parser::JoinClause> joins;
    bool have_from = false;
    const auto place = [&](parser::RelationRef rel, const parser::JoinClause* attached) {
        if (!have_from) {
            out.from = std::move(rel);
            have_from = true;
            return;
        }
        parser::JoinClause join = attached != nullptr ? *attached : parser::JoinClause{};
        join.relation = std::move(rel);
        joins.push_back(std::move(join));
    };
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const parser::JoinClause* attached = i == 0 ? nullptr : &stmt.joins[i - 1];
        if (!defs[i].has_value()) {
            place(*refs[i], attached);
            continue;
        }
        auto expanded = Expand(catalog, *defs[i], *refs[i], view, next_lens, depth + 1);
        if (!expanded.ok()) return expanded.status();
        parser::SelectStmt& body = expanded.value().body;
        // The view's bind is declared before its body's first relation's
        // (DP-R5); a nested view's own entry is already there, after it.
        parser::OpenedView opened;
        opened.oid = defs[i]->oid;
        opened.unqualified_in =
            refs[i]->lens == 0 && refs[i]->schema.empty() ? default_namespace : 0;
        body.from.opens.insert(body.from.opens.begin(), opened);
        // The body's first relation takes the view's position, and the ON
        // that attached the view stays with it: an inner join's ON and WHERE
        // are one conjunct list (`step_compiler.cpp`), so which relation
        // carries a conjunct changes no answer.
        place(std::move(body.from), attached);
        for (parser::JoinClause& join : body.joins) joins.push_back(std::move(join));
        for (parser::Condition& cond : body.where) out.where.push_back(std::move(cond));
        // The view, then the views its body absorbed: the order the binds
        // above open them (DP-Q17's declaration order).
        out.views.push_back(std::move(expanded.value().view));
        for (parser::AbsorbedView& nested : body.views) out.views.push_back(std::move(nested));
    }
    out.joins = std::move(joins);
    out.lens_high = next_lens;
    out.views_settled = true;

    // `SELECT *` from one view is its columns, under the view's names
    // (DP-Q12): never the base relations' `*`.
    if (stmt.star() && stmt.joins.empty() && defs[0].has_value()) {
        for (const std::string& column : defs[0]->columns) {
            parser::ColumnName name;
            name.name = column;
            name.lens = stmt.from.lens;
            name.byte_offset = stmt.from.byte_offset;
            out.projection.push_back(std::move(name));
        }
    }
    return std::optional<parser::SelectStmt>(std::move(out));
}

}  // namespace

std::shared_ptr<const parser::ViewReads> ReadsOf(const catalog::ViewDef& def) {
    auto reads = std::make_shared<parser::ViewReads>();
    reads->view = def.name;
    for (const catalog::ViewDef::Dependency& d : def.dependencies) {
        if (std::find(reads->oids.begin(), reads->oids.end(), d.oid) == reads->oids.end()) {
            reads->oids.push_back(d.oid);
        }
    }
    return reads;
}

bool FillKeyViews(const parser::SelectStmt& block, CapturedKey& key) {
    if (block.views.size() > kMaxPlannedRelations) return false;
    key.v = static_cast<std::uint8_t>(block.views.size());
    for (std::size_t k = 0; k < block.views.size(); ++k) key.views[k] = block.views[k].oid;
    return true;
}

StatusOr<std::optional<parser::SelectStmt>> AbsorbViews(catalog::Catalog& catalog,
                                                        const parser::SelectStmt& stmt,
                                                        const txn::ReadView* view,
                                                        catalog::Oid default_namespace,
                                                        std::uint32_t& next_lens) {
    return AbsorbAt(catalog, stmt, view, default_namespace, next_lens, 0);
}

Status CheckViewReads(const parser::RelationRef& rel, catalog::Oid oid) {
    if (rel.reads == nullptr ||
        std::find(rel.reads->oids.begin(), rel.reads->oids.end(), oid) != rel.reads->oids.end()) {
        return Status::OK();
    }
    return Status::InvalidArgument("view '" + rel.reads->view +
                                   "' no longer reads what it was created over: '" +
                                   rel.table_name + "' names another object now" +
                                   At(rel.byte_offset) + "; DROP VIEW and CREATE VIEW again");
}

std::vector<parser::Condition> WhereOnRelation(const parser::SelectStmt& absorbed) {
    // One relation, so each text holds one view at most: a name of a view's
    // text is that view's column, until the relation's own text is reached.
    const auto on_relation = [&](parser::ColumnName& name) {
        for (std::size_t hop = 0; name.lens != absorbed.from.lens; ++hop) {
            const parser::ColumnName* target = nullptr;
            for (const parser::AbsorbedView& v : absorbed.views) {
                if (v.lens == name.lens && (target = v.Column(name.name)) != nullptr) break;
            }
            if (target == nullptr || hop == absorbed.views.size()) {
                name.name.clear();
                return;
            }
            name = *target;
        }
    };
    std::vector<parser::Condition> out = absorbed.where;
    for (parser::Condition& cond : out) {
        if (!cond.col.name.empty()) on_relation(cond.col);
        if (!cond.rhs_col.name.empty()) on_relation(cond.rhs_col);
    }
    return out;
}

}  // namespace kds::exec
