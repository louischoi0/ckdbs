#pragma once

// **The query layer** (BU-S4, `instructions/v3.0.0/workorder-bu-basic-query-surface.md`
// BU-Q8..Q12): the evaluator for every SELECT form a step chain cannot carry -
// value expressions in the select list, `DISTINCT`, `ORDER BY` an expression,
// an alias or an ordinal, `GROUP BY`/`HAVING` over expressions, every join
// kind, derived tables, set operations and a subquery as a value.
//
// **Native walks below, rows in hand above.** Each relation a block names is
// walked as an ordinary step chain by the host (`LayerHost::WalkRelation`),
// under the statement's one snapshot, carrying every top-level `WHERE` term
// that names that relation alone and that the native grammar reads - so a pk
// or index term keeps its access path. Everything else - joins, the rest of
// the `WHERE`, grouping, the select list, ordering, set operations - is
// evaluated here over the rows the walks returned. A term is never moved
// below an outer join onto its nullable side (BU-Q7).
//
// **Bounded** (BU-Q10): every row set held - a walk's rows, a join's output,
// a set operation, a sort - is capped at `LayerLimits::max_rows`
// (`sort_max_rows`), a group table at `max_groups` (`aggregate_max_groups`),
// each refused `ResourceExhausted` naming the setting.
//
// **Typed at compile, no implicit conversion** (BU-Q11): every expression is
// typed by BJ's typer (`set_expr.hpp`, `TypeQueryExpression`) before any row
// is read, so a mistyped statement is refused whether or not a row matches.
// Three-valued throughout; a `WHERE`, `ON` or `HAVING` keeps a row only when
// its condition is true (BU-R4). NULLs group together and sort largest.
//
// Pure of storage: the host does every page read. No Waystone trail is
// recorded or read for a layer statement (BU-Q12).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/exec/set_expr.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/parser/ast.hpp"

namespace kds::exec {

// A column of a row set the layer holds or returns.
struct LayerColumn {
    std::string qualifier;  // the binding a relation's column is reached by; empty for output
    std::string name;
    SetType type;           // concrete
    bool keystone = false;  // a relation's column 0, which travels as a u64
    // The FROM's first relation's column 0: the one the wire flags as the
    // tuple key, as the native describe does (`DescribeFields`).
    bool driving_keystone = false;
};

struct RowSet {
    std::vector<LayerColumn> columns;
    std::vector<std::vector<parser::AstValue>> rows;
};

struct LayerLimits {
    std::size_t max_rows = 0;    // sort_max_rows
    std::size_t max_groups = 0;  // aggregate_max_groups
    // aggregate_max_distinct: the DISTINCT sets' entries, summed over every
    // set of the statement, as the native fold counts them.
    std::size_t max_distinct = 0;
};

// What the layer asks of the dispatcher.
class LayerHost {
public:
    virtual ~LayerHost() = default;

    // `rel`'s columns, in schema order and typed as the schema declares, its
    // column 0 marked the Keystone. Asked at compile, before any walk.
    virtual StatusOr<std::vector<LayerColumn>> DescribeRelation(const parser::RelationRef& rel) = 0;

    // Every column of `rel`, in schema order and typed as the schema declares,
    // of the rows that pass `where` - native conjuncts, positioned in the
    // statement - walked under the statement's snapshot.
    virtual StatusOr<RowSet> WalkRelation(const parser::RelationRef& rel,
                                          const std::vector<parser::Condition>& where) = 0;
};

// Runs `query`, whose text is `sql`, and returns its result.
StatusOr<RowSet> RunQuery(const parser::Query& query, std::string_view sql, LayerHost& host,
                          const StatementContext& context, const LayerLimits& limits);

}  // namespace kds::exec
