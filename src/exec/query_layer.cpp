#include "kds/exec/query_layer.hpp"

// The query layer (BU-S4); `query_layer.hpp` states its contract.
//
// **Compile, then execute.** A `parser::Query` is compiled once into a
// `CompiledQuery` tree - every relation described, every expression typed,
// every `WHERE` term either handed to its relation's walk or kept as a
// residual - before any row is read. Execution then walks the tree; a
// correlated subquery is executed once per outer row against the same
// compiled tree, an uncorrelated one once per statement.
//
// **One evaluation row per context**: the row an expression reads is the
// context's own columns followed by the enclosing query's evaluation row,
// so a column a subquery resolves outward is `own width + its index there`.
// A block's own columns are its joined relations' (the "wide" row) for
// `ON`, `WHERE` and an ungrouped select list, and its group row - keys,
// then aggregates - for a grouped select list, `HAVING` and `ORDER BY`.
//
// No locks or atomics: one statement's state, on its own thread.

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "kds/base/int128.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/functions.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/type_literals.hpp"
#include "kds/parser/parser.hpp"

namespace kds::exec {

namespace {

using parser::AggFunc;
using parser::AstValue;
using parser::Expr;
using parser::ExprOp;
using parser::JoinKind;
using parser::Query;
using parser::QueryBlock;
using parser::ValueType;
using Row = std::vector<AstValue>;

std::string At(std::uint32_t byte) { return " (byte " + std::to_string(byte) + ")"; }

SetType Concrete(std::uint32_t type_val, std::uint32_t len) {
    SetType t;
    t.kind = SetType::Kind::kConcrete;
    t.type_val = type_val;
    t.len = len;
    return t;
}

bool IsText(std::uint32_t t) { return TypeFamilyOf(t) == TypeFamily::kText; }
bool IsDecimalType(std::uint32_t t) {
    return t == catalog::kTypeValDecimal || t == catalog::kTypeValDecimalWide;
}
bool IsNull(const AstValue& v) { return v.type == ValueType::kNull; }
bool IsTrue(const AstValue& v) { return v.type == ValueType::kInt && v.int_val != 0; }

// ---- Values as keys --------------------------------------------------------

// Appends a value's identity to `out`: equal values - two NULLs included,
// as grouping, DISTINCT and the set operations need - append equal bytes.
Status AppendKey(std::string& out, const AstValue& v, const SetType& t) {
    auto key = OrderKeyOf(t.type_val, v);
    if (!key.ok()) return key.status();
    const OrderKey& k = key.value();
    if (k.is_null) {
        out.push_back('\0');
    } else if (k.is_str) {
        out.push_back('\2');
        const std::uint32_t n = static_cast<std::uint32_t>(k.str.size());
        out.append(reinterpret_cast<const char*>(&n), sizeof n);
        out += k.str;
    } else {
        out.push_back('\1');
        out.append(reinterpret_cast<const char*>(&k.num), sizeof k.num);
    }
    return Status::OK();
}

StatusOr<std::string> KeyOf(const Row& row, std::size_t from, std::size_t count,
                            const std::vector<SetType>& types) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (Status s = AppendKey(out, row[from + i], types[i]); !s.ok()) return s;
    }
    return out;
}

// ---- Numbers ---------------------------------------------------------------

StatusOr<Int128> NumberOf(const AstValue& v) {
    switch (v.type) {
        case ValueType::kInt:
        case ValueType::kDecimal: return static_cast<Int128>(v.int_val);
        case ValueType::kDecimalWide: return Int128FromHalves(v.dec_hi, v.int_val);
        default: return Status::Corruption("an aggregate read a value that is not a number");
    }
}

// A SUM's or AVG's answer `v` as a value of `t` (an int64 or a decimal).
// No precision bound: the accumulator was checked at every addition
// (int64, int128 for a DECIMAL128), and the native fold answers any sum
// that fits it - a decimal(18) SUM may carry 19 digits there too
// (`AggregateOutputType`, aggregate.md AG3).
AstValue AggregateValue(Int128 v, const SetType& t) {
    AstValue out;
    if (t.type_val == catalog::kTypeValDecimalWide) {
        out.type = ValueType::kDecimalWide;
        out.int_val = Int128Low(v);
        out.dec_hi = Int128High(v);
    } else {
        out.type = t.type_val == catalog::kTypeValDecimal ? ValueType::kDecimal : ValueType::kInt;
        out.int_val = static_cast<std::int64_t>(v);
    }
    if (out.type != ValueType::kInt) out.scale = catalog::DecimalScaleOf(t.len);
    return out;
}

// ---- Structure -------------------------------------------------------------

// The top-level `AND` terms of a WHERE as `ParseQueryBlock` built it: a
// left-deep chain, one term per recorded span.
std::vector<const Expr*> WhereTerms(const QueryBlock& b) {
    std::vector<const Expr*> terms;
    if (b.where == nullptr) return terms;
    if (b.where_spans.size() <= 1) {
        terms.push_back(b.where.get());
        return terms;
    }
    const Expr* node = b.where.get();
    for (std::size_t i = b.where_spans.size() - 1; i > 0; --i) {
        terms.push_back(node->rhs.get());
        node = node->lhs.get();
    }
    terms.push_back(node);
    std::reverse(terms.begin(), terms.end());
    return terms;
}

// Calls `f` on every aggregate under `e`, never descending into a subquery
// (its aggregates are its own) or into an aggregate's argument.
template <typename F>
void ForEachAggregate(const Expr& e, F&& f) {
    if (e.kind == Expr::Kind::kAggregate) {
        f(e);
        return;
    }
    if (e.kind == Expr::Kind::kSubquery) {
        if (e.lhs != nullptr) ForEachAggregate(*e.lhs, f);
        return;
    }
    if (e.lhs != nullptr) ForEachAggregate(*e.lhs, f);
    if (e.rhs != nullptr) ForEachAggregate(*e.rhs, f);
    for (const auto& a : e.args) ForEachAggregate(*a, f);
}

bool HasAggregate(const Expr* e) {
    if (e == nullptr) return false;
    bool found = false;
    ForEachAggregate(*e, [&](const Expr&) { found = true; });
    return found;
}

bool SameValue(const AstValue& a, const AstValue& b) {
    return a.type == b.type && a.int_val == b.int_val && a.str_val == b.str_val &&
           a.scale == b.scale && a.dec_hi == b.dec_hi && a.raw_int_text == b.raw_int_text;
}

bool IEq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Whether two expressions are the same computation: what a GROUP BY key, an
// aggregate or an ORDER BY expression is matched by. A column is compared
// by what it resolves to (`same_column`), so `x` and `t.x` are one key.
template <typename SameColumn>
bool SameExpr(const Expr& a, const Expr& b, SameColumn&& same_column) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Expr::Kind::kLiteral:
            return a.bare_numeric == b.bare_numeric && SameValue(a.literal, b.literal);
        case Expr::Kind::kColumn: return same_column(a, b);
        // A subquery is the same computation only as the same node: an
        // aggregate over one is matched back to itself (HAVING
        // `SUM((SELECT ...))`), never to another spelling of it.
        case Expr::Kind::kSubquery: return &a == &b;
        case Expr::Kind::kAggregate:
            if (a.agg_func != b.agg_func || a.agg_distinct != b.agg_distinct) return false;
            if ((a.lhs == nullptr) != (b.lhs == nullptr)) return false;
            return a.lhs == nullptr || SameExpr(*a.lhs, *b.lhs, same_column);
        case Expr::Kind::kUnary:
            if (a.op != b.op) return false;
            if (a.op == ExprOp::kCast &&
                (!IEq(a.cast_to.name, b.cast_to.name) || a.cast_to.precision != b.cast_to.precision ||
                 a.cast_to.scale != b.cast_to.scale || a.cast_to.width != b.cast_to.width ||
                 a.cast_to.has_width != b.cast_to.has_width ||
                 a.cast_to.has_precision != b.cast_to.has_precision)) {
                return false;
            }
            return SameExpr(*a.lhs, *b.lhs, same_column);
        case Expr::Kind::kBinary:
            return a.op == b.op && SameExpr(*a.lhs, *b.lhs, same_column) &&
                   SameExpr(*a.rhs, *b.rhs, same_column);
        case Expr::Kind::kCall:
            if (a.op != b.op || a.args.size() != b.args.size()) return false;
            if (a.op == ExprOp::kFunction && !IEq(a.function, b.function)) return false;
            for (std::size_t i = 0; i < a.args.size(); ++i) {
                if (!SameExpr(*a.args[i], *b.args[i], same_column)) return false;
            }
            return true;
    }
    return false;
}

// A heading with no text to fall back on: a column's name, a function's or
// an aggregate's, and otherwise PostgreSQL's `?column?`.
std::string NameOf(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::kColumn: return e.column;
        case Expr::Kind::kAggregate: return std::string(parser::AggFuncText(e.agg_func));
        case Expr::Kind::kCall:
            if (e.op == ExprOp::kFunction) {
                std::string out;
                for (char c : e.function) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                return out;
            }
            if (e.op == ExprOp::kCase) return "case";
            if (e.op == ExprOp::kCoalesce) return "coalesce";
            if (e.op == ExprOp::kNullif) return "nullif";
            if (e.op == ExprOp::kGreatest) return "greatest";
            if (e.op == ExprOp::kLeast) return "least";
            return "?column?";
        case Expr::Kind::kUnary:
            if (e.op == ExprOp::kCast) return NameOf(*e.lhs);
            return "?column?";
        case Expr::Kind::kSubquery: return e.op == ExprOp::kExists ? "exists" : "?column?";
        default: return "?column?";
    }
}

// The heading an unaliased item gets: a column as written (`o.id`), as the
// native path heads one; an aggregate over a column or `*` in the native
// fold's canonical form (`count(*)`, `sum(amount)`); anything else its text
// as written.
std::string HeadingOf(const Expr& e, const std::string& text) {
    if (e.kind == Expr::Kind::kColumn) {
        return e.qualifier.empty() ? e.column : e.qualifier + "." + e.column;
    }
    if (e.kind == Expr::Kind::kAggregate &&
        (e.lhs == nullptr || e.lhs->kind == Expr::Kind::kColumn)) {
        std::string out = std::string(parser::AggFuncText(e.agg_func)) + "(";
        if (e.agg_distinct) out += "distinct ";
        out += e.lhs == nullptr ? "*" : HeadingOf(*e.lhs, {});
        return out + ")";
    }
    return text.empty() ? NameOf(e) : text;
}

// Calls `f(index)` for every column node of a typed tree.
template <typename F>
void ForEachColumn(const TypedSetExpr& e, F&& f) {
    if (e.kind == Expr::Kind::kColumn) f(e.column_no);
    if (e.lhs != nullptr) ForEachColumn(*e.lhs, f);
    if (e.rhs != nullptr) ForEachColumn(*e.rhs, f);
    for (const auto& a : e.args) ForEachColumn(*a, f);
}

bool HasSubquery(const TypedSetExpr& e) {
    if (e.kind == Expr::Kind::kSubquery) return true;
    if (e.lhs != nullptr && HasSubquery(*e.lhs)) return true;
    if (e.rhs != nullptr && HasSubquery(*e.rhs)) return true;
    for (const auto& a : e.args) {
        if (HasSubquery(*a)) return true;
    }
    return false;
}

// Whether a native conjunct may ride a relation's walk: a comparison, a
// range, a function comparison, or a boolean tree of them - never one that
// carries a subquery, whose correlation the walk could not resolve.
// Nor one calling a function the native catalog does not hold: the native
// compile declines it (`step_compiler.cpp`'s `ResolveCall`), so the term is
// the layer's to evaluate, not the walk's.
bool CallsNative(const parser::Condition& c) {
    return (!c.lhs_fn.has_value() || FindFunction(c.lhs_fn->name) != nullptr) &&
           (!c.rhs_fn.has_value() || FindFunction(c.rhs_fn->name) != nullptr);
}

bool BoolTreeWalkable(const parser::BoolExpr& node) {
    if (node.op == parser::BoolExpr::Op::kLeaf) {
        return !node.leaf.has_subquery() && CallsNative(node.leaf);
    }
    return std::all_of(node.children.begin(), node.children.end(), BoolTreeWalkable);
}

bool Walkable(const parser::Condition& c) {
    switch (c.kind) {
        case parser::PredicateKind::kCompareValue:
        case parser::PredicateKind::kBetween: return true;
        case parser::PredicateKind::kCompareFunction: return CallsNative(c);
        case parser::PredicateKind::kBoolTree: return BoolTreeWalkable(*c.tree);
        default: return false;
    }
}

// ---- The compiled tree -----------------------------------------------------

struct CompiledQuery;

struct Source {
    parser::RelationRef rel;
    std::unique_ptr<CompiledQuery> derived;  // a derived table
    std::vector<LayerColumn> columns;
    std::size_t offset = 0;
    bool nullable = false;  // on an outer join's nullable side
    std::vector<parser::Condition> pushed;
    std::string pushed_key;  // the pushed terms' text, which keys the walk's cache
};

struct Join {
    JoinKind kind = JoinKind::kInner;
    std::unique_ptr<TypedSetExpr> on;  // null for a cross join
    std::size_t left_width = 0;
    std::size_t right_width = 0;
    // Equalities of a left column and a right column among `on`'s top-level
    // `AND` terms, which the join hashes on: (left index, right index within
    // the right row), with the type the two share.
    std::vector<std::pair<std::size_t, std::size_t>> eq;
    std::vector<SetType> eq_types;
};

struct Agg {
    AggFunc func = AggFunc::kCount;
    bool distinct = false;
    const Expr* node = nullptr;
    std::unique_ptr<TypedSetExpr> arg;  // null for COUNT(*)
    SetType out;
};

struct CompiledBlock {
    std::vector<Source> sources;
    std::vector<Join> joins;  // joins[i] attaches sources[i + 1]
    std::vector<LayerColumn> wide;
    std::map<std::string, std::size_t> merged;  // a USING column's name, folded (`FoldName`) -> the index it reads
    std::vector<std::unique_ptr<TypedSetExpr>> where;
    bool grouped = false;
    std::vector<std::unique_ptr<TypedSetExpr>> keys;  // GROUP BY's; empty for one global group
    std::vector<Agg> aggs;
    std::vector<LayerColumn> group_columns;  // keys, then aggregates
    std::unique_ptr<TypedSetExpr> having;
    std::vector<std::unique_ptr<TypedSetExpr>> items;  // visible, then ORDER BY's hidden keys
    std::vector<SetType> item_types;
    std::size_t visible = 0;
    bool distinct = false;
    std::vector<std::unique_ptr<CompiledQuery>> subqueries;
};

struct CompiledQuery {
    Query::Op op = Query::Op::kBlock;
    bool all = false;
    std::unique_ptr<CompiledBlock> block;
    std::unique_ptr<CompiledQuery> lhs;
    std::unique_ptr<CompiledQuery> rhs;
    std::vector<LayerColumn> columns;  // what it returns
    // Sort keys over the row a block emits (visible, then hidden) or over a
    // set operation's output row.
    std::vector<std::pair<std::size_t, bool>> sort;
    std::vector<SetType> sort_types;
    std::optional<std::uint64_t> limit;
    std::uint64_t offset = 0;
    bool correlated = false;  // reads a column of an enclosing query
    std::optional<RowSet> cached;
};

// A column name as `catalog::ColumnNamesCollide` compares it: ASCII case
// folded, so a USING column found by one spelling is found by every other.
std::string FoldName(std::string name) {
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return name;
}

// How a context resolves a column name - ignoring case, as every read does
// (`catalog::ColumnNamesCollide`): its own columns, then its parent's.
struct Scope {
    const std::vector<LayerColumn>* columns = nullptr;
    std::size_t width = 0;  // the context's own part of the evaluation row
    const Scope* parent = nullptr;
    CompiledQuery* owner = nullptr;
    const std::map<std::string, std::size_t>* merged = nullptr;
    // A grouped context: a subquery may not reach into it for a column.
    bool grouped = false;
};

StatusOr<std::optional<ResolvedColumn>> ResolveIn(const Scope& s, const Expr& e) {
    std::vector<std::size_t> hits;
    if (s.columns != nullptr) {
        for (std::size_t i = 0; i < s.columns->size(); ++i) {
            const LayerColumn& c = (*s.columns)[i];
            if (catalog::ColumnNamesCollide(c.name, e.column) &&
                (e.qualifier.empty() || IEq(c.qualifier, e.qualifier))) {
                hits.push_back(i);
            }
        }
    }
    if (hits.size() > 1 && e.qualifier.empty() && s.merged != nullptr) {
        if (auto it = s.merged->find(FoldName(e.column)); it != s.merged->end()) {
            hits.assign(1, it->second);
        }
    }
    if (hits.size() > 1) {
        return Status::InvalidArgument("column reference '" + e.column + "' is ambiguous" +
                                       At(e.byte_offset) + "; qualify it");
    }
    if (hits.size() == 1) {
        if (s.grouped) {
            return Status::NotImplemented(
                "a subquery that reads a column of a grouped query is not supported" +
                At(e.byte_offset));
        }
        return std::optional<ResolvedColumn>(ResolvedColumn{hits[0], (*s.columns)[hits[0]].type});
    }
    if (s.parent == nullptr) return std::optional<ResolvedColumn>();
    auto outer = ResolveIn(*s.parent, e);
    if (!outer.ok() || !outer.value().has_value()) return outer;
    if (s.owner != nullptr) s.owner->correlated = true;
    return std::optional<ResolvedColumn>(
        ResolvedColumn{s.width + outer.value()->index, outer.value()->type});
}

// Whether two column references in `scope` name one column of its own -
// asked of the context alone, so a comparison never marks a query
// correlated.
auto SameColumnIn(const Scope& scope) {
    Scope local = scope;
    local.parent = nullptr;
    return [local](const Expr& x, const Expr& y) {
        auto rx = ResolveIn(local, x);
        auto ry = ResolveIn(local, y);
        return rx.ok() && ry.ok() && rx.value().has_value() && ry.value().has_value() &&
               rx.value()->index == ry.value()->index;
    };
}

// ---- The runner ------------------------------------------------------------

class Runner {
public:
    Runner(std::string_view sql, LayerHost& host, const StatementContext& context,
           const LayerLimits& limits)
        : sql_(sql), host_(host), context_(context), limits_(limits) {}

    StatusOr<std::unique_ptr<CompiledQuery>> Compile(const Query& q, const Scope* parent);
    StatusOr<RowSet> Execute(CompiledQuery& q, const Row& outer);

private:
    Status CompileBlock(const QueryBlock& b, const std::vector<parser::OrderItem>& order,
                        const Scope* parent, CompiledQuery& q);
    Status CompileSources(const QueryBlock& b, const Scope* parent, CompiledQuery& q,
                          CompiledBlock& cb);
    Status CompileWhere(const QueryBlock& b, const Scope& wide, CompiledBlock& cb);
    Status CompileGrouping(const QueryBlock& b, const std::vector<parser::OrderItem>& order,
                           const Scope& wide, CompiledBlock& cb);
    StatusOr<std::unique_ptr<TypedSetExpr>> Type(const Expr& e, const Scope& scope,
                                                 CompiledBlock& cb, const char* clause,
                                                 const std::function<std::optional<ResolvedColumn>(
                                                     const Expr&)>& computed = {});
    Status CompileSetOrder(const Query& q, CompiledQuery& cq);
    std::function<StatusOr<SetType>(const Expr&, std::size_t&)> SubqueryTyper(const Scope& scope,
                                                                              CompiledBlock& cb);

    StatusOr<std::vector<Row>> ExecuteBlock(CompiledBlock& b, const Row& outer);
    StatusOr<std::vector<Row>> SourceRows(Source& s);
    StatusOr<std::vector<Row>> JoinRows(const Join& j, std::vector<Row> left,
                                        const std::vector<Row>& right, CompiledBlock& b,
                                        const Row& outer);
    StatusOr<std::vector<Row>> GroupRows(CompiledBlock& b, const std::vector<Row>& rows,
                                         const Row& outer);
    StatusOr<AstValue> Eval(const TypedSetExpr& e, const Row& row, CompiledBlock& b);
    StatusOr<bool> Holds(const TypedSetExpr& e, const Row& row, CompiledBlock& b);
    StatusOr<AstValue> AnswerSubquery(CompiledBlock& b, const TypedSetExpr& node, const Row& row,
                                      const AstValue* operand);
    Status SortAndCut(CompiledQuery& q, std::vector<Row>& rows);
    Status Cap(std::size_t n, const char* what) const;

    std::string_view sql_;
    LayerHost& host_;
    const StatementContext& context_;
    LayerLimits limits_;
    // A relation's rows under the statement's snapshot, keyed by relation
    // and pushed terms: a correlated subquery re-reads them per outer row.
    std::map<std::string, std::vector<Row>> walks_;
};

Status Runner::Cap(std::size_t n, const char* what) const {
    if (n <= limits_.max_rows) return Status::OK();
    return Status::ResourceExhausted(
        std::string("this statement's ") + what + " would hold more than sort_max_rows (" +
        std::to_string(limits_.max_rows) +
        ") rows; the query layer answers from rows it holds (BU-Q10) - raise sort_max_rows, or "
        "narrow the statement");
}

StatusOr<std::unique_ptr<TypedSetExpr>> Runner::Type(
    const Expr& e, const Scope& scope, CompiledBlock& cb, const char* clause,
    const std::function<std::optional<ResolvedColumn>(const Expr&)>& computed) {
    ExprScope es;
    es.clause = clause;
    es.computed = computed;
    es.resolve = [&scope](const Expr& col) { return ResolveIn(scope, col); };
    es.subquery = SubqueryTyper(scope, cb);
    return TypeQueryExpression(e, es);
}

// Types a subquery `node` names as a block of `scope`'s, and files it in
// `cb.subqueries` under `id`.
std::function<StatusOr<SetType>(const Expr&, std::size_t&)> Runner::SubqueryTyper(
    const Scope& scope, CompiledBlock& cb) {
    return [this, &scope, &cb](const Expr& node, std::size_t& id) -> StatusOr<SetType> {
        auto sub = Compile(*node.query, &scope);
        if (!sub.ok()) return sub.status();
        const CompiledQuery& q = *sub.value();
        if (node.op != ExprOp::kExists && q.columns.size() != 1) {
            return Status::InvalidArgument("a subquery used as a value returns one column, not " +
                                           std::to_string(q.columns.size()) +
                                           At(node.op_byte_offset));
        }
        id = cb.subqueries.size();
        SetType t = q.columns.front().type;
        cb.subqueries.push_back(std::move(sub.value()));
        return t;
    };
}

StatusOr<std::unique_ptr<CompiledQuery>> Runner::Compile(const Query& q, const Scope* parent) {
    auto cq = std::make_unique<CompiledQuery>();
    cq->op = q.op;
    cq->all = q.all;
    cq->limit = q.limit;
    cq->offset = q.offset;
    if (q.op == Query::Op::kBlock) {
        if (Status s = CompileBlock(*q.block, q.order_by, parent, *cq); !s.ok()) return s;
        return cq;
    }
    auto lhs = Compile(*q.lhs, parent);
    if (!lhs.ok()) return lhs.status();
    auto rhs = Compile(*q.rhs, parent);
    if (!rhs.ok()) return rhs.status();
    cq->lhs = std::move(lhs.value());
    cq->rhs = std::move(rhs.value());
    cq->correlated = cq->lhs->correlated || cq->rhs->correlated;
    const auto& l = cq->lhs->columns;
    const auto& r = cq->rhs->columns;
    if (l.size() != r.size()) {
        return Status::InvalidArgument("each side of a set operation returns the same number of "
                                       "columns, not " + std::to_string(l.size()) + " and " +
                                       std::to_string(r.size()) + At(q.rhs->byte_offset));
    }
    for (std::size_t i = 0; i < l.size(); ++i) {
        LayerColumn c = l[i];
        c.qualifier.clear();
        c.keystone = l[i].keystone && r[i].keystone;
        c.driving_keystone = l[i].driving_keystone && r[i].driving_keystone;
        const SetType& a = l[i].type;
        const SetType& b = r[i].type;
        const bool same = a.type_val == b.type_val && (IsText(a.type_val) || a.len == b.len);
        const bool text = IsText(a.type_val) && IsText(b.type_val);
        if (!same && !text) {
            return Status::InvalidArgument(
                "column " + std::to_string(i + 1) + " of a set operation is " + SetTypeName(a) +
                " on one side and " + SetTypeName(b) +
                " on the other; there is no implicit conversion (BJ-R3), CAST one" +
                At(q.rhs->byte_offset));
        }
        if (text && a.type_val != b.type_val) c.type = Concrete(catalog::kTypeValVarchar, 0);
        cq->columns.push_back(std::move(c));
    }
    if (Status s = CompileSetOrder(q, *cq); !s.ok()) return s;
    return cq;
}

// ORDER BY over a set operation names an output column, by name or position.
Status Runner::CompileSetOrder(const Query& q, CompiledQuery& cq) {
    for (const parser::OrderItem& item : q.order_by) {
        const Expr& e = *item.expr;
        std::optional<std::size_t> index;
        if (e.kind == Expr::Kind::kLiteral && e.literal.type == ValueType::kInt && !e.bare_numeric) {
            const std::int64_t k = e.literal.int_val;
            if (k < 1 || static_cast<std::size_t>(k) > cq.columns.size()) {
                return Status::InvalidArgument("ORDER BY position " + std::to_string(k) +
                                               " is not in the select list" + At(e.byte_offset));
            }
            index = static_cast<std::size_t>(k - 1);
        } else if (e.kind == Expr::Kind::kColumn && e.qualifier.empty()) {
            for (std::size_t i = 0; i < cq.columns.size(); ++i) {
                if (!catalog::ColumnNamesCollide(cq.columns[i].name, e.column)) continue;
                if (index.has_value()) {
                    return Status::InvalidArgument("ORDER BY '" + e.column + "' is ambiguous" +
                                                   At(e.byte_offset));
                }
                index = i;
            }
        }
        if (!index.has_value()) {
            return Status::NotImplemented(
                "ORDER BY over a set operation takes an output column's name or position" +
                At(e.byte_offset));
        }
        cq.sort.emplace_back(*index, item.descending);
        cq.sort_types.push_back(cq.columns[*index].type);
    }
    return Status::OK();
}

Status Runner::CompileSources(const QueryBlock& b, const Scope* parent, CompiledQuery& q,
                              CompiledBlock& cb) {
    // A FROM item cannot read an enclosing query (no LATERAL); an ON can.
    std::vector<const parser::FromItem*> items{&b.from};
    for (const parser::FromJoin& j : b.joins) items.push_back(&j.item);
    std::set<std::string> bindings;
    for (const parser::FromItem* item : items) {
        Source s;
        s.rel = item->rel;
        std::vector<LayerColumn> columns;
        if (item->derived != nullptr) {
            auto sub = Compile(*item->derived, nullptr);
            if (!sub.ok()) return sub.status();
            s.derived = std::move(sub.value());
            columns = s.derived->columns;
        } else {
            auto described = host_.DescribeRelation(item->rel);
            if (!described.ok()) return described.status();
            columns = std::move(described.value());
        }
        const std::string binding = item->rel.binding();
        if (!bindings.insert(FoldName(binding)).second) {
            return Status::InvalidArgument("the name '" + binding +
                                           "' is bound twice in FROM; give one an alias with AS" +
                                           At(item->byte_offset));
        }
        for (LayerColumn& c : columns) {
            c.qualifier = binding;
            c.driving_keystone = c.keystone && cb.sources.empty();
        }
        s.columns = std::move(columns);
        s.offset = cb.wide.size();
        cb.wide.insert(cb.wide.end(), s.columns.begin(), s.columns.end());
        cb.sources.push_back(std::move(s));
    }
    // Nullable sides: a LEFT join's right relation, a RIGHT join's every
    // relation left of it, both sides of a FULL one.
    for (std::size_t i = 0; i < b.joins.size(); ++i) {
        const JoinKind k = b.joins[i].kind;
        if (k == JoinKind::kLeft || k == JoinKind::kFull) cb.sources[i + 1].nullable = true;
        if (k == JoinKind::kRight || k == JoinKind::kFull) {
            for (std::size_t j = 0; j <= i; ++j) cb.sources[j].nullable = true;
        }
    }

    // The joins, each typed over the relations it joins.
    for (std::size_t i = 0; i < b.joins.size(); ++i) {
        const parser::FromJoin& fj = b.joins[i];
        const Source& right = cb.sources[i + 1];
        Join j;
        j.kind = fj.kind;
        j.left_width = right.offset;
        j.right_width = right.columns.size();
        auto prefix = std::make_shared<std::vector<LayerColumn>>(
            cb.wide.begin(), cb.wide.begin() + static_cast<std::ptrdiff_t>(right.offset + right.columns.size()));

        std::shared_ptr<const Expr> on = fj.on;
        if (!fj.using_columns.empty()) {
            if (fj.kind == JoinKind::kFull) {
                return Status::NotImplemented("FULL JOIN ... USING is not supported" +
                                              At(fj.byte_offset) + "; write the ON condition");
            }
            for (const parser::ColumnName& col : fj.using_columns) {
                std::optional<std::size_t> left_hit;
                for (std::size_t c = 0; c < right.offset; ++c) {
                    if (!catalog::ColumnNamesCollide(cb.wide[c].name, col.name)) continue;
                    if (left_hit.has_value()) {
                        return Status::InvalidArgument("USING column '" + col.name +
                                                       "' is ambiguous on the left" +
                                                       At(col.byte_offset));
                    }
                    left_hit = c;
                }
                std::optional<std::size_t> right_hit;
                for (std::size_t c = 0; c < right.columns.size(); ++c) {
                    if (catalog::ColumnNamesCollide(right.columns[c].name, col.name)) {
                        right_hit = right.offset + c;
                    }
                }
                if (!left_hit.has_value() || !right_hit.has_value()) {
                    return Status::InvalidArgument("USING column '" + col.name +
                                                   "' is not on both sides of the join" +
                                                   At(col.byte_offset));
                }
                auto l = std::make_shared<Expr>();
                l->kind = Expr::Kind::kColumn;
                l->qualifier = cb.wide[*left_hit].qualifier;
                l->column = col.name;
                l->byte_offset = col.byte_offset;
                auto r = std::make_shared<Expr>(*l);
                r->qualifier = cb.wide[*right_hit].qualifier;
                auto eq = std::make_shared<Expr>();
                eq->kind = Expr::Kind::kBinary;
                eq->op = ExprOp::kEq;
                eq->byte_offset = col.byte_offset;
                eq->op_byte_offset = col.byte_offset;
                eq->lhs = l;
                eq->rhs = r;
                if (on == nullptr) {
                    on = eq;
                } else {
                    auto both = std::make_shared<Expr>();
                    both->kind = Expr::Kind::kBinary;
                    both->op = ExprOp::kAnd;
                    both->byte_offset = col.byte_offset;
                    both->op_byte_offset = col.byte_offset;
                    both->lhs = on;
                    both->rhs = eq;
                    on = both;
                }
                cb.merged[FoldName(col.name)] = fj.kind == JoinKind::kRight ? *right_hit : *left_hit;
            }
        }
        if (on != nullptr) {
            Scope scope;
            scope.columns = prefix.get();
            scope.width = prefix->size();
            scope.parent = parent;
            scope.owner = &q;
            scope.merged = &cb.merged;
            auto typed = Type(*on, scope, cb, "in an ON condition");
            if (!typed.ok()) return typed.status();
            if (typed.value()->type.type_val != catalog::kTypeValBool) {
                return Status::InvalidArgument("ON needs a bool, not " +
                                               SetTypeName(typed.value()->type) +
                                               At(on->byte_offset));
            }
            j.on = std::move(typed.value());
            // The equalities the join can hash on.
            std::vector<const TypedSetExpr*> stack{j.on.get()};
            while (!stack.empty()) {
                const TypedSetExpr* t = stack.back();
                stack.pop_back();
                if (t->kind == Expr::Kind::kBinary && t->op == ExprOp::kAnd) {
                    stack.push_back(t->lhs.get());
                    stack.push_back(t->rhs.get());
                    continue;
                }
                if (t->kind != Expr::Kind::kBinary || t->op != ExprOp::kEq) continue;
                const TypedSetExpr* a = t->lhs.get();
                const TypedSetExpr* c = t->rhs.get();
                if (a->kind != Expr::Kind::kColumn || c->kind != Expr::Kind::kColumn) continue;
                if (a->column_no >= right.offset) std::swap(a, c);
                const bool left_side = a->column_no < right.offset;
                const bool right_side =
                    c->column_no >= right.offset && c->column_no < right.offset + right.columns.size();
                if (!left_side || !right_side || a->type.type_val != c->type.type_val ||
                    a->type.len != c->type.len) {
                    continue;
                }
                j.eq.emplace_back(a->column_no, c->column_no - right.offset);
                j.eq_types.push_back(a->type);
            }
        }
        cb.joins.push_back(std::move(j));
    }
    return Status::OK();
}

Status Runner::CompileWhere(const QueryBlock& b, const Scope& wide, CompiledBlock& cb) {
    const std::vector<const Expr*> terms = WhereTerms(b);
    for (std::size_t t = 0; t < terms.size(); ++t) {
        auto typed = Type(*terms[t], wide, cb, "in WHERE");
        if (!typed.ok()) return typed.status();
        if (typed.value()->type.type_val != catalog::kTypeValBool) {
            return Status::InvalidArgument("WHERE needs a bool, not " +
                                           SetTypeName(typed.value()->type) +
                                           At(terms[t]->byte_offset));
        }
        // Which relation the term reads, if one: then its walk may carry it.
        std::optional<std::size_t> only;
        bool several = false;
        ForEachColumn(*typed.value(), [&](std::size_t index) {
            if (index >= cb.wide.size()) {
                several = true;  // an enclosing query's column
                return;
            }
            std::size_t s = 0;
            while (s + 1 < cb.sources.size() && cb.sources[s + 1].offset <= index) ++s;
            if (only.has_value() && *only != s) several = true;
            only = s;
        });
        bool pushed = false;
        if (only.has_value() && !several && b.where_spans.size() == terms.size() &&
            !HasSubquery(*typed.value())) {
            Source& s = cb.sources[*only];
            if (s.derived == nullptr && !s.nullable) {
                const auto [begin, end] = b.where_spans[t];
                std::string text(begin, ' ');
                text.append(sql_.substr(begin, end - begin));
                parser::Parser native(text);
                auto conds = native.ParseConditions();
                if (conds.ok() &&
                    std::all_of(conds.value().begin(), conds.value().end(), Walkable)) {
                    for (parser::Condition& c : conds.value()) s.pushed.push_back(std::move(c));
                    s.pushed_key += text.substr(begin);
                    s.pushed_key.push_back('\x1f');
                    pushed = true;
                }
            }
        }
        if (!pushed) cb.where.push_back(std::move(typed.value()));
    }
    return Status::OK();
}

// A decimal SUM's or AVG's type: the argument's scale at its family's
// widest precision, as the native fold answers (aggregate.md AG3).
SetType DecimalResult(const SetType& arg) {
    const bool wide = arg.type_val == catalog::kTypeValDecimalWide;
    return Concrete(arg.type_val,
                    catalog::PackDecimalLen(wide ? kMaxDecimalPrecisionWide : kMaxDecimalPrecision,
                                            catalog::DecimalScaleOf(arg.len)));
}

Status Runner::CompileGrouping(const QueryBlock& b, const std::vector<parser::OrderItem>& order,
                               const Scope& wide, CompiledBlock& cb) {
    const auto same_column = SameColumnIn(wide);
    for (const auto& g : b.group_by) {
        if (g->kind == Expr::Kind::kLiteral) {
            return Status::NotImplemented("GROUP BY a position or a constant is not supported" +
                                          At(g->byte_offset) + "; name the expression");
        }
        if (HasAggregate(g.get())) {
            return Status::InvalidArgument("an aggregate is not allowed in GROUP BY" +
                                           At(g->byte_offset));
        }
        // A key named twice is the native compile's slip (aggregate.md §2).
        for (const auto& earlier : b.group_by) {
            if (earlier.get() == g.get()) break;
            if (SameExpr(*earlier, *g, same_column)) {
                return Status::InvalidArgument("'" + NameOf(*g) + "' appears twice in GROUP BY" +
                                               At(g->byte_offset));
            }
        }
        auto typed = Type(*g, wide, cb, "in GROUP BY");
        if (!typed.ok()) return typed.status();
        LayerColumn col;
        col.name = NameOf(*g);
        if (g->kind == Expr::Kind::kColumn) col.qualifier = cb.wide[typed.value()->column_no].qualifier;
        col.type = typed.value()->type;
        cb.group_columns.push_back(col);
        cb.keys.push_back(std::move(typed.value()));
    }

    // Every aggregate the select list, HAVING and ORDER BY name, once each.
    std::vector<const Expr*> agg_nodes;
    const auto collect = [&](const Expr* e) {
        if (e == nullptr) return;
        ForEachAggregate(*e, [&](const Expr& a) {
            for (const Expr* seen : agg_nodes) {
                if (SameExpr(*seen, a, same_column)) return;
            }
            agg_nodes.push_back(&a);
        });
    };
    for (const parser::OutputItem& item : b.items) collect(item.expr.get());
    collect(b.having.get());
    for (const parser::OrderItem& o : order) collect(o.expr.get());

    for (const Expr* node : agg_nodes) {
        Agg agg;
        agg.func = node->agg_func;
        agg.distinct = node->agg_distinct;
        agg.node = node;
        SetType arg_type;
        if (node->lhs != nullptr) {
            auto typed = Type(*node->lhs, wide, cb, "inside another aggregate");
            if (!typed.ok()) return typed.status();
            arg_type = typed.value()->type;
            agg.arg = std::move(typed.value());
        }
        const std::string label = std::string(parser::AggFuncText(agg.func));
        const std::uint32_t tv = arg_type.type_val;
        switch (agg.func) {
            case AggFunc::kCount: agg.out = Concrete(catalog::kTypeValInt64, 0); break;
            case AggFunc::kMin:
            case AggFunc::kMax: agg.out = arg_type; break;
            case AggFunc::kSum:
                if (tv == catalog::kTypeValUint64) {
                    return Status::Unsupported("SUM over a uint64 value is not supported" +
                                               At(node->byte_offset) +
                                               "; half its range does not fit the int64 "
                                               "accumulator");
                }
                if (catalog::IsIntegerTypeVal(tv)) {
                    agg.out = Concrete(catalog::kTypeValInt64, 0);
                } else if (IsDecimalType(tv)) {
                    agg.out = DecimalResult(arg_type);
                } else {
                    return Status::InvalidArgument("SUM requires a signed integer or decimal "
                                                   "value, not " + SetTypeName(arg_type) +
                                                   At(node->byte_offset));
                }
                break;
            case AggFunc::kAvg:
                if (!IsDecimalType(tv)) {
                    return Status::InvalidArgument(
                        "AVG requires a decimal value, not " + SetTypeName(arg_type) +
                        At(node->byte_offset) +
                        "; the answer is given at the value's declared scale - CAST it to "
                        "DECIMAL(p, s), or compute SUM and COUNT");
                } else {
                    agg.out = DecimalResult(arg_type);
                }
                break;
        }
        LayerColumn col;
        col.name = label;
        col.type = agg.out;
        cb.group_columns.push_back(col);
        cb.aggs.push_back(std::move(agg));
    }

    return Status::OK();
}

Status Runner::CompileBlock(const QueryBlock& b, const std::vector<parser::OrderItem>& order,
                            const Scope* parent, CompiledQuery& q) {
    auto cbp = std::make_unique<CompiledBlock>();
    CompiledBlock& cb = *cbp;
    cb.distinct = b.distinct;
    if (b.has_from) {
        if (Status s = CompileSources(b, parent, q, cb); !s.ok()) return s;
    }

    Scope wide;
    wide.columns = &cb.wide;
    wide.width = cb.wide.size();
    wide.parent = parent;
    wide.owner = &q;
    wide.merged = &cb.merged;

    if (Status s = CompileWhere(b, wide, cb); !s.ok()) return s;

    bool aggregated = !b.group_by.empty() || b.having != nullptr;
    for (const parser::OutputItem& item : b.items) aggregated |= HasAggregate(item.expr.get());
    for (const parser::OrderItem& o : order) aggregated |= HasAggregate(o.expr.get());
    cb.grouped = aggregated;

    // The context the select list, HAVING and ORDER BY are typed in.
    Scope grouped_scope;
    std::function<std::optional<ResolvedColumn>(const Expr&)> computed;
    const auto same_column = SameColumnIn(wide);
    if (cb.grouped) {
        if (Status s = CompileGrouping(b, order, wide, cb); !s.ok()) return s;
        grouped_scope.columns = &cb.wide;  // names to refuse, see `resolve` below
        grouped_scope.width = cb.group_columns.size();
        grouped_scope.parent = parent;
        grouped_scope.owner = &q;
        grouped_scope.merged = &cb.merged;
        grouped_scope.grouped = true;
        computed = [&cb, &b, same_column](const Expr& e) -> std::optional<ResolvedColumn> {
            for (std::size_t i = 0; i < b.group_by.size(); ++i) {
                if (SameExpr(*b.group_by[i], e, same_column)) {
                    return ResolvedColumn{i, cb.keys[i]->type};
                }
            }
            if (e.kind == Expr::Kind::kAggregate) {
                for (std::size_t i = 0; i < cb.aggs.size(); ++i) {
                    if (SameExpr(*cb.aggs[i].node, e, same_column)) {
                        return ResolvedColumn{b.group_by.size() + i, cb.aggs[i].out};
                    }
                }
            }
            return std::nullopt;
        };
    }
    // A grouped context's typer: a column of this block that is not a key.
    const auto type_in_context = [&](const Expr& e, const char* clause)
        -> StatusOr<std::unique_ptr<TypedSetExpr>> {
        if (!cb.grouped) return Type(e, wide, cb, clause);
        ExprScope es;
        es.clause = clause;
        es.computed = computed;
        es.resolve = [&](const Expr& col) -> StatusOr<std::optional<ResolvedColumn>> {
            Scope local = wide;
            local.parent = nullptr;
            auto here = ResolveIn(local, col);
            if (!here.ok()) return here.status();
            if (here.value().has_value()) {
                return Status::InvalidArgument(
                    "column '" + (col.qualifier.empty() ? "" : col.qualifier + ".") + col.column +
                    "' must appear in GROUP BY or be used in an aggregate" + At(col.byte_offset));
            }
            if (parent == nullptr) return std::optional<ResolvedColumn>();
            auto outer = ResolveIn(*parent, col);
            if (!outer.ok() || !outer.value().has_value()) return outer;
            q.correlated = true;
            return std::optional<ResolvedColumn>(
                ResolvedColumn{cb.group_columns.size() + outer.value()->index,
                               outer.value()->type});
        };
        es.subquery = SubqueryTyper(grouped_scope, cb);
        return TypeQueryExpression(e, es);
    };
    if (b.having != nullptr) {
        auto typed = type_in_context(*b.having, "in HAVING");
        if (!typed.ok()) return typed.status();
        if (typed.value()->type.type_val != catalog::kTypeValBool) {
            return Status::InvalidArgument("HAVING needs a bool, not " +
                                           SetTypeName(typed.value()->type) +
                                           At(b.having->byte_offset));
        }
        cb.having = std::move(typed.value());
    }

    // ---- The select list ------------------------------------------------
    std::vector<const Expr*> item_exprs;  // per visible item, null for a star column
    for (const parser::OutputItem& item : b.items) {
        if (item.star) {
            if (cb.grouped) {
                return Status::InvalidArgument("* cannot be combined with GROUP BY or an "
                                               "aggregate" + At(item.byte_offset) +
                                               "; name the columns");
            }
            if (!b.has_from) {
                return Status::InvalidArgument("SELECT * needs a FROM" + At(item.byte_offset));
            }
            if (!cb.merged.empty()) {
                return Status::NotImplemented("* over a join with USING is not supported" +
                                              At(item.byte_offset) + "; name the columns");
            }
            bool any = false;
            for (std::size_t c = 0; c < cb.wide.size(); ++c) {
                if (!item.star_qualifier.empty() && !IEq(cb.wide[c].qualifier, item.star_qualifier)) {
                    continue;
                }
                any = true;
                auto n = std::make_unique<TypedSetExpr>();
                n->kind = Expr::Kind::kColumn;
                n->column_no = c;
                n->type = cb.wide[c].type;
                n->byte_offset = item.byte_offset;
                LayerColumn out = cb.wide[c];
                out.qualifier.clear();
                q.columns.push_back(out);
                cb.item_types.push_back(n->type);
                cb.items.push_back(std::move(n));
                item_exprs.push_back(nullptr);
            }
            if (!any) {
                return Status::InvalidArgument("no relation named '" + item.star_qualifier +
                                               "' in FROM" + At(item.byte_offset));
            }
            continue;
        }
        auto typed = type_in_context(*item.expr, "here");
        if (!typed.ok()) return typed.status();
        LayerColumn out;
        out.name = item.alias.empty() ? HeadingOf(*item.expr, item.text) : item.alias;
        out.type = typed.value()->type;
        if (!cb.grouped && item.expr->kind == Expr::Kind::kColumn &&
            typed.value()->column_no < cb.wide.size()) {
            out.keystone = cb.wide[typed.value()->column_no].keystone;
            out.driving_keystone = cb.wide[typed.value()->column_no].driving_keystone;
        }
        q.columns.push_back(out);
        cb.item_types.push_back(typed.value()->type);
        cb.items.push_back(std::move(typed.value()));
        item_exprs.push_back(item.expr.get());
    }
    cb.visible = cb.items.size();

    // ---- ORDER BY: a position, an output name, or an expression -----------
    for (const parser::OrderItem& o : order) {
        const Expr& e = *o.expr;
        std::optional<std::size_t> index;
        if (e.kind == Expr::Kind::kLiteral && e.literal.type == ValueType::kInt && !e.bare_numeric) {
            const std::int64_t k = e.literal.int_val;
            if (k < 1 || static_cast<std::size_t>(k) > cb.visible) {
                return Status::InvalidArgument("ORDER BY position " + std::to_string(k) +
                                               " is not in the select list" + At(e.byte_offset));
            }
            index = static_cast<std::size_t>(k - 1);
        } else if (e.kind == Expr::Kind::kColumn && e.qualifier.empty()) {
            for (std::size_t i = 0; i < cb.visible; ++i) {
                if (!catalog::ColumnNamesCollide(q.columns[i].name, e.column)) continue;
                // A star column or a bare column item of that name is the
                // same column the input name reads; only two different
                // output columns of one name are ambiguous.
                if (index.has_value()) {
                    const TypedSetExpr& a = *cb.items[*index];
                    const TypedSetExpr& c = *cb.items[i];
                    if (a.kind == Expr::Kind::kColumn && c.kind == Expr::Kind::kColumn &&
                        a.column_no == c.column_no) {
                        continue;
                    }
                    return Status::InvalidArgument("ORDER BY '" + e.column + "' is ambiguous" +
                                                   At(e.byte_offset));
                }
                index = i;
            }
        }
        if (!index.has_value()) {
            for (std::size_t i = 0; i < cb.visible; ++i) {
                if (item_exprs[i] != nullptr && SameExpr(*item_exprs[i], e, same_column)) {
                    index = i;
                    break;
                }
            }
        }
        if (!index.has_value()) {
            if (cb.distinct) {
                return Status::InvalidArgument(
                    "for SELECT DISTINCT, an ORDER BY expression must appear in the select list" +
                    At(e.byte_offset));
            }
            auto typed = type_in_context(e, "here");
            if (!typed.ok()) return typed.status();
            index = cb.items.size();
            cb.item_types.push_back(typed.value()->type);
            cb.items.push_back(std::move(typed.value()));
        }
        q.sort.emplace_back(*index, o.descending);
        q.sort_types.push_back(cb.item_types[*index]);
    }
    q.block = std::move(cbp);
    return Status::OK();
}

// ---- Execution -------------------------------------------------------------

StatusOr<AstValue> Runner::AnswerSubquery(CompiledBlock& b, const TypedSetExpr& node,
                                          const Row& row, const AstValue* operand) {
    CompiledQuery& q = *b.subqueries[node.subquery_id];
    auto result = Execute(q, row);
    if (!result.ok()) return result.status();
    const RowSet& rs = result.value();
    AstValue out;
    switch (node.op) {
        case ExprOp::kExists:
            out.type = ValueType::kInt;
            out.int_val = rs.rows.empty() ? 0 : 1;
            return out;
        case ExprOp::kScalarSubquery:
            if (rs.rows.size() > 1) {
                return Status::CardinalityViolation(
                    "a subquery used as a value returned " + std::to_string(rs.rows.size()) +
                    " rows" + At(node.byte_offset) + "; it may return at most one");
            }
            return rs.rows.empty() ? AstValue{} : rs.rows.front().front();
        case ExprOp::kInSubquery: {
            out.type = ValueType::kInt;
            out.int_val = 0;
            // Over no rows `IN` is false whatever its operand, so
            // `NULL NOT IN (empty)` is true; only then does a NULL operand
            // make the answer unknown.
            if (rs.rows.empty()) return out;
            if (IsNull(*operand)) return AstValue{};
            bool unknown = false;
            auto want = OrderKeyOf(node.lhs->type.type_val, *operand);
            if (!want.ok()) return want.status();
            for (const Row& r : rs.rows) {
                if (IsNull(r.front())) {
                    unknown = true;
                    continue;
                }
                auto got = OrderKeyOf(rs.columns.front().type.type_val, r.front());
                if (!got.ok()) return got.status();
                if (want.value().Compare(got.value()) == 0) {
                    out.int_val = 1;
                    return out;
                }
            }
            if (unknown) return AstValue{};
            return out;
        }
        default: return Status::Corruption("a subquery node of an unknown form");
    }
}

StatusOr<AstValue> Runner::Eval(const TypedSetExpr& e, const Row& row, CompiledBlock& b) {
    const SubqueryEvaluator sub = [this, &b](const TypedSetExpr& node, const Row& r,
                                             const AstValue* operand) {
        return AnswerSubquery(b, node, r, operand);
    };
    return EvaluateQueryExpression(e, row, context_, b.subqueries.empty() ? nullptr : &sub);
}

StatusOr<bool> Runner::Holds(const TypedSetExpr& e, const Row& row, CompiledBlock& b) {
    auto v = Eval(e, row, b);
    if (!v.ok()) return v.status();
    return IsTrue(v.value());  // UNKNOWN keeps no row (BU-R4)
}

Row Concat(const Row& a, const Row& b) {
    Row out;
    out.reserve(a.size() + b.size());
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

StatusOr<std::vector<Row>> Runner::SourceRows(Source& s) {
    if (s.derived != nullptr) {
        auto rs = Execute(*s.derived, Row{});
        if (!rs.ok()) return rs.status();
        return rs.value().rows;
    }
    const std::string key = s.rel.schema + "." + s.rel.table_name + "\x1e" + s.pushed_key;
    if (auto it = walks_.find(key); it != walks_.end()) return it->second;
    auto walked = host_.WalkRelation(s.rel, s.pushed);
    if (!walked.ok()) return walked.status();
    if (Status c = Cap(walked.value().rows.size(), "relation walk"); !c.ok()) return c;
    // Every expression was typed against the description; a row of another
    // width - a relation changed under the statement - would be read out of
    // bounds, so it is refused instead.
    for (const Row& r : walked.value().rows) {
        if (r.size() != s.columns.size()) {
            return Status::TxnConflict("relation '" + s.rel.table_name +
                                       "' changed while the statement read it" +
                                       At(s.rel.byte_offset) + "; run it again");
        }
    }
    return walks_.emplace(key, std::move(walked.value().rows)).first->second;
}

StatusOr<std::vector<Row>> Runner::JoinRows(const Join& j, std::vector<Row> left,
                                            const std::vector<Row>& right, CompiledBlock& b,
                                            const Row& outer) {
    std::vector<Row> out;
    std::vector<bool> right_matched(right.size(), false);
    const Row left_nulls(j.left_width);
    const Row right_nulls(j.right_width);

    // Candidates by the hashed equalities; every pair when there are none.
    std::unordered_map<std::string, std::vector<std::size_t>> index;
    if (!j.eq.empty()) {
        for (std::size_t r = 0; r < right.size(); ++r) {
            std::string key;
            bool has_null = false;
            for (std::size_t k = 0; k < j.eq.size(); ++k) {
                const AstValue& v = right[r][j.eq[k].second];
                if (IsNull(v)) has_null = true;
                if (Status s = AppendKey(key, v, j.eq_types[k]); !s.ok()) return s;
            }
            if (!has_null) index[key].push_back(r);
        }
    }
    std::vector<std::size_t> all;
    if (j.eq.empty()) {
        all.resize(right.size());
        for (std::size_t r = 0; r < right.size(); ++r) all[r] = r;
    }
    static const std::vector<std::size_t> kNone;
    for (Row& l : left) {
        const std::vector<std::size_t>* candidates = &all;
        if (!j.eq.empty()) {
            std::string key;
            bool has_null = false;
            for (std::size_t k = 0; k < j.eq.size(); ++k) {
                const AstValue& v = l[j.eq[k].first];
                if (IsNull(v)) has_null = true;
                if (Status s = AppendKey(key, v, j.eq_types[k]); !s.ok()) return s;
            }
            auto it = has_null ? index.end() : index.find(key);
            candidates = it == index.end() ? &kNone : &it->second;
        }
        bool matched = false;
        for (std::size_t r : *candidates) {
            Row combined = Concat(l, right[r]);
            if (j.on != nullptr) {
                auto ok = Holds(*j.on, outer.empty() ? combined : Concat(combined, outer), b);
                if (!ok.ok()) return ok.status();
                if (!ok.value()) continue;
            }
            matched = true;
            right_matched[r] = true;
            out.push_back(std::move(combined));
            if (Status c = Cap(out.size(), "join"); !c.ok()) return c;
        }
        if (!matched && (j.kind == JoinKind::kLeft || j.kind == JoinKind::kFull)) {
            out.push_back(Concat(l, right_nulls));
            if (Status c = Cap(out.size(), "join"); !c.ok()) return c;
        }
    }
    if (j.kind == JoinKind::kRight || j.kind == JoinKind::kFull) {
        for (std::size_t r = 0; r < right.size(); ++r) {
            if (right_matched[r]) continue;
            out.push_back(Concat(left_nulls, right[r]));
            if (Status c = Cap(out.size(), "join"); !c.ok()) return c;
        }
    }
    return out;
}

// One group's running state for one aggregate.
struct AggState {
    std::int64_t count = 0;
    Int128 sum = 0;
    AstValue best;  // MIN/MAX
    std::set<std::string> seen;  // DISTINCT
};

StatusOr<std::vector<Row>> Runner::GroupRows(CompiledBlock& b, const std::vector<Row>& rows,
                                             const Row& outer) {
    struct Group {
        Row keys;
        std::vector<AggState> states;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> by_key;
    // The global group is founded uncapped, as the native fold founds it
    // (`Aggregator::Reset`): only a GROUP BY's groups count, so 0 refuses
    // every grouped statement and no global one.
    if (b.keys.empty()) groups.push_back(Group{{}, std::vector<AggState>(b.aggs.size())});
    std::size_t distinct_entries = 0;  // summed over every set (aggregate.md AG11)
    for (const Row& wide_row : rows) {
        const Row eval = outer.empty() ? wide_row : Concat(wide_row, outer);
        std::size_t g = 0;
        if (!b.keys.empty()) {
            Row keys;
            keys.reserve(b.keys.size());
            std::string key;
            for (const auto& k : b.keys) {
                auto v = Eval(*k, eval, b);
                if (!v.ok()) return v.status();
                if (Status s = AppendKey(key, v.value(), k->type); !s.ok()) return s;
                keys.push_back(std::move(v.value()));
            }
            auto it = by_key.find(key);
            if (it == by_key.end()) {
                if (groups.size() >= limits_.max_groups) {
                    return Status::ResourceExhausted(
                        "this statement would hold more than aggregate_max_groups (" +
                        std::to_string(limits_.max_groups) + ") groups (BU-Q10)");
                }
                groups.push_back(Group{std::move(keys), std::vector<AggState>(b.aggs.size())});
                it = by_key.emplace(std::move(key), groups.size() - 1).first;
            }
            g = it->second;
        }
        for (std::size_t a = 0; a < b.aggs.size(); ++a) {
            const Agg& agg = b.aggs[a];
            AggState& st = groups[g].states[a];
            if (agg.arg == nullptr) {  // COUNT(*)
                ++st.count;
                continue;
            }
            auto v = Eval(*agg.arg, eval, b);
            if (!v.ok()) return v.status();
            if (IsNull(v.value())) continue;
            if (agg.distinct) {
                std::string key;
                if (Status s = AppendKey(key, v.value(), agg.arg->type); !s.ok()) return s;
                if (st.seen.count(key) != 0) continue;
                if (distinct_entries >= limits_.max_distinct) {
                    return Status::ResourceExhausted(
                        "this statement exceeded aggregate_max_distinct (" +
                        std::to_string(limits_.max_distinct) + ")" + At(agg.node->byte_offset) +
                        "; no partial answer is emitted");
                }
                ++distinct_entries;
                st.seen.insert(std::move(key));
            }
            ++st.count;
            switch (agg.func) {
                case AggFunc::kCount: break;
                case AggFunc::kSum:
                case AggFunc::kAvg: {
                    auto n = NumberOf(v.value());
                    if (!n.ok()) return n.status();
                    // Checked at every step in the native fold's accumulator
                    // (aggregate.md AG3): int64, int128 for a DECIMAL128 -
                    // so a sum crossing INT64_MAX fails even if it comes back.
                    const bool wide = agg.out.type_val == catalog::kTypeValDecimalWide;
                    if (__builtin_add_overflow(st.sum, n.value(), &st.sum) ||
                        (!wide && (st.sum > std::numeric_limits<std::int64_t>::max() ||
                                   st.sum < std::numeric_limits<std::int64_t>::min()))) {
                        return Status::OutOfRange("SUM overflow in " +
                                                  std::string(parser::AggFuncText(agg.func)) +
                                                  At(agg.node->byte_offset));
                    }
                    break;
                }
                case AggFunc::kMin:
                case AggFunc::kMax: {
                    if (IsNull(st.best)) {
                        st.best = v.value();
                        break;
                    }
                    auto x = OrderKeyOf(agg.arg->type.type_val, v.value());
                    if (!x.ok()) return x.status();
                    auto y = OrderKeyOf(agg.arg->type.type_val, st.best);
                    if (!y.ok()) return y.status();
                    const int c = x.value().Compare(y.value());
                    if (agg.func == AggFunc::kMin ? c < 0 : c > 0) st.best = v.value();
                    break;
                }
            }
        }
    }

    std::vector<Row> out;
    for (Group& grp : groups) {
        Row group_row = std::move(grp.keys);
        for (std::size_t a = 0; a < b.aggs.size(); ++a) {
            const Agg& agg = b.aggs[a];
            const AggState& st = grp.states[a];
            switch (agg.func) {
                case AggFunc::kCount: {
                    AstValue v;
                    v.type = ValueType::kInt;
                    v.int_val = st.count;
                    group_row.push_back(v);
                    break;
                }
                case AggFunc::kSum: {
                    if (st.count == 0) {
                        group_row.emplace_back();
                        break;
                    }
                    group_row.push_back(AggregateValue(st.sum, agg.out));
                    break;
                }
                case AggFunc::kAvg: {
                    if (st.count == 0) {
                        group_row.emplace_back();
                        break;
                    }
                    // Half to even at the value's own scale (aggregate.md §3.4).
                    const Int128 n = st.count;
                    Int128 q = st.sum / n;
                    const Int128 r = st.sum % n;
                    const Int128 twice = (r < 0 ? -r : r) * 2;
                    if (twice > n || (twice == n && (q % 2 != 0))) q += st.sum < 0 ? -1 : 1;
                    group_row.push_back(AggregateValue(q, agg.out));
                    break;
                }
                case AggFunc::kMin:
                case AggFunc::kMax: group_row.push_back(st.best); break;
            }
        }
        out.push_back(std::move(group_row));
    }
    return out;
}

StatusOr<std::vector<Row>> Runner::ExecuteBlock(CompiledBlock& b, const Row& outer) {
    std::vector<Row> rows;
    if (b.sources.empty()) {
        rows.emplace_back();  // SELECT without FROM: one row of no columns
    } else {
        auto first = SourceRows(b.sources[0]);
        if (!first.ok()) return first.status();
        rows = first.value();
        for (std::size_t j = 0; j < b.joins.size(); ++j) {
            auto right = SourceRows(b.sources[j + 1]);
            if (!right.ok()) return right.status();
            auto joined = JoinRows(b.joins[j], std::move(rows), right.value(), b, outer);
            if (!joined.ok()) return joined.status();
            rows = std::move(joined.value());
        }
    }
    if (!b.where.empty()) {
        std::vector<Row> kept;
        for (Row& r : rows) {
            const Row eval = outer.empty() ? r : Concat(r, outer);
            bool keep = true;
            for (const auto& w : b.where) {
                auto ok = Holds(*w, eval, b);
                if (!ok.ok()) return ok.status();
                if (!ok.value()) {
                    keep = false;
                    break;
                }
            }
            if (keep) kept.push_back(std::move(r));
        }
        rows = std::move(kept);
    }
    if (b.grouped) {
        auto grouped = GroupRows(b, rows, outer);
        if (!grouped.ok()) return grouped.status();
        rows = std::move(grouped.value());
        if (b.having != nullptr) {
            std::vector<Row> kept;
            for (Row& r : rows) {
                auto ok = Holds(*b.having, outer.empty() ? r : Concat(r, outer), b);
                if (!ok.ok()) return ok.status();
                if (ok.value()) kept.push_back(std::move(r));
            }
            rows = std::move(kept);
        }
    }
    std::vector<Row> out;
    out.reserve(rows.size());
    std::set<std::string> seen;
    for (const Row& r : rows) {
        const Row eval = outer.empty() ? r : Concat(r, outer);
        Row item_row;
        item_row.reserve(b.items.size());
        for (const auto& item : b.items) {
            auto v = Eval(*item, eval, b);
            if (!v.ok()) return v.status();
            item_row.push_back(std::move(v.value()));
        }
        if (b.distinct) {
            auto key = KeyOf(item_row, 0, b.visible, b.item_types);
            if (!key.ok()) return key.status();
            if (!seen.insert(std::move(key.value())).second) continue;
        }
        out.push_back(std::move(item_row));
    }
    return out;
}

Status Runner::SortAndCut(CompiledQuery& q, std::vector<Row>& rows) {
    if (!q.sort.empty()) {
        if (Status c = Cap(rows.size(), "ORDER BY"); !c.ok()) return c;
        std::vector<std::vector<OrderKey>> keys(rows.size());
        for (std::size_t r = 0; r < rows.size(); ++r) {
            for (std::size_t k = 0; k < q.sort.size(); ++k) {
                auto key = OrderKeyOf(q.sort_types[k].type_val, rows[r][q.sort[k].first]);
                if (!key.ok()) return key.status();
                keys[r].push_back(std::move(key.value()));
            }
        }
        std::vector<std::size_t> order(rows.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            for (std::size_t k = 0; k < q.sort.size(); ++k) {
                const int c = keys[a][k].Compare(keys[b][k]);
                if (c != 0) return q.sort[k].second ? c > 0 : c < 0;
            }
            return false;
        });
        std::vector<Row> sorted;
        sorted.reserve(rows.size());
        for (std::size_t i : order) sorted.push_back(std::move(rows[i]));
        rows = std::move(sorted);
    }
    const std::size_t skip = static_cast<std::size_t>(std::min<std::uint64_t>(q.offset, rows.size()));
    rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(skip));
    if (q.limit.has_value() && rows.size() > *q.limit) rows.resize(static_cast<std::size_t>(*q.limit));
    return Status::OK();
}

StatusOr<RowSet> Runner::Execute(CompiledQuery& q, const Row& outer) {
    if (!q.correlated && q.cached.has_value()) return *q.cached;
    std::vector<Row> rows;
    if (q.op == Query::Op::kBlock) {
        auto r = ExecuteBlock(*q.block, q.correlated ? outer : Row{});
        if (!r.ok()) return r.status();
        rows = std::move(r.value());
        if (Status s = SortAndCut(q, rows); !s.ok()) return s;
        for (Row& row : rows) row.resize(q.block->visible);
    } else {
        auto l = Execute(*q.lhs, outer);
        if (!l.ok()) return l.status();
        auto r = Execute(*q.rhs, outer);
        if (!r.ok()) return r.status();
        std::vector<SetType> types;
        for (const LayerColumn& c : q.columns) types.push_back(c.type);
        const std::size_t width = types.size();
        const auto key_of = [&](const Row& row) { return KeyOf(row, 0, width, types); };
        if (q.op == Query::Op::kUnion) {
            rows = std::move(l.value().rows);
            for (Row& row : r.value().rows) rows.push_back(std::move(row));
            if (Status c = Cap(rows.size(), "UNION"); !c.ok()) return c;
            if (!q.all) {
                std::set<std::string> seen;
                std::vector<Row> kept;
                for (Row& row : rows) {
                    auto k = key_of(row);
                    if (!k.ok()) return k.status();
                    if (seen.insert(std::move(k.value())).second) kept.push_back(std::move(row));
                }
                rows = std::move(kept);
            }
        } else {
            // INTERSECT and EXCEPT: by multiplicity on the right.
            std::map<std::string, std::size_t> right;
            for (const Row& row : r.value().rows) {
                auto k = key_of(row);
                if (!k.ok()) return k.status();
                ++right[k.value()];
            }
            std::set<std::string> emitted;
            for (Row& row : l.value().rows) {
                auto k = key_of(row);
                if (!k.ok()) return k.status();
                auto it = right.find(k.value());
                const bool present = it != right.end() && it->second > 0;
                const bool keep = q.op == Query::Op::kIntersect ? present : !present;
                if (q.all) {
                    if (q.op == Query::Op::kIntersect && present) {
                        --it->second;
                        rows.push_back(std::move(row));
                    } else if (q.op == Query::Op::kExcept) {
                        if (present) {
                            --it->second;
                        } else {
                            rows.push_back(std::move(row));
                        }
                    }
                } else if (keep && emitted.insert(k.value()).second) {
                    rows.push_back(std::move(row));
                }
            }
        }
        if (Status s = SortAndCut(q, rows); !s.ok()) return s;
    }
    RowSet out;
    out.columns = q.columns;
    out.rows = std::move(rows);
    if (!q.correlated) q.cached = out;
    return out;
}

}  // namespace

namespace {

bool QueryLanded(const parser::Query& q);

// `in_condition`: the node sits under a WHERE or a HAVING, where a
// subquery is a condition's (S4) rather than a value's.
bool ExprLanded(const Expr& e, bool in_condition) {
    if (e.kind == Expr::Kind::kSubquery) {
        if (!in_condition) return false;  // S7: a subquery as a value
        if (e.query != nullptr && !QueryLanded(*e.query)) return false;
    }
    if (e.lhs != nullptr && !ExprLanded(*e.lhs, in_condition)) return false;
    if (e.rhs != nullptr && !ExprLanded(*e.rhs, in_condition)) return false;
    return std::all_of(e.args.begin(), e.args.end(),
                       [&](const auto& a) { return a == nullptr || ExprLanded(*a, in_condition); });
}

bool QueryLanded(const parser::Query& q) {
    if (q.op != parser::Query::Op::kBlock || q.block == nullptr) return false;  // S7
    const parser::QueryBlock& b = *q.block;
    // S7: a derived table, first or joined.
    if (b.from.derived != nullptr) return false;
    for (const parser::FromJoin& j : b.joins) {
        if (j.item.derived != nullptr) return false;
        if (j.on != nullptr && !ExprLanded(*j.on, true)) return false;
    }
    for (const parser::OutputItem& item : b.items) {
        if (item.expr != nullptr && !ExprLanded(*item.expr, false)) return false;
    }
    for (const parser::OrderItem& o : q.order_by) {
        if (o.expr != nullptr && !ExprLanded(*o.expr, false)) return false;
    }
    for (const auto& g : b.group_by) {
        if (!ExprLanded(*g, false)) return false;
    }
    if (b.having != nullptr && !ExprLanded(*b.having, true)) return false;
    return b.where == nullptr || ExprLanded(*b.where, true);
}

}  // namespace

bool FormsLanded(const parser::Query& query) { return QueryLanded(query); }

StatusOr<RowSet> RunQuery(const parser::Query& query, std::string_view sql, LayerHost& host,
                          const StatementContext& context, const LayerLimits& limits) {
    Runner runner(sql, host, context, limits);
    auto compiled = runner.Compile(query, nullptr);
    if (!compiled.ok()) return compiled.status();
    return runner.Execute(*compiled.value(), Row{});
}

}  // namespace kds::exec
