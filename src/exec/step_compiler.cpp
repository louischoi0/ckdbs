#include "kds/exec/step_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kds::exec {

namespace {

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// One relation of the chain being compiled, with everything resolution
// needs: what the statement calls it, and what columns it has.
struct BoundRelation {
    std::string binding;  // alias if written, else the table name
    const catalog::TableAccess* access = nullptr;
};

// The scope a name resolves against. One per query block; sub-chains
// (V15) push another and resolve outward through `parent`.
struct Scope {
    std::vector<BoundRelation> relations;
    const Scope* parent = nullptr;
};

std::string Position(std::uint32_t byte_offset) {
    return " at byte " + std::to_string(byte_offset);
}

// Finds `name` among a relation's columns. Returns the schema position,
// which is exactly what ColumnRef::col_pos is - BuildSchemaFromColumns()
// returns columns sorted by `pos`, so the vector index is the position.
bool FindColumnPos(const catalog::Schema& schema, std::string_view name, std::uint16_t& out) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        if (IEquals(catalog::NameView(schema.columns[i].name), name)) {
            out = static_cast<std::uint16_t>(i);
            return true;
        }
    }
    return false;
}

// Resolves one written column name to a compiled reference.
//
// The two rules, from docs/parser-v2.md's resolution section:
//
//   qualified     `a.x` names a relation or alias in this chain's FROM
//                 list or an enclosing one.
//   unqualified   `x` resolves iff **exactly one** visible relation has
//                 that column, searching innermost-first and stopping at
//                 the first level that matches.
//
// Stopping at the first matching level is the part worth stating: it
// means adding a column to an *outer* relation can never silently change
// an inner chain's meaning. Without it, a schema change to an unrelated
// table would quietly repoint a correlated subquery's predicate.
//
// Ambiguity is an error, never a choice. Two relations with the same
// column and no qualifier has no correct reading, and picking the first
// would make the answer depend on written order in a way the client
// never asked for.
StatusOr<ColumnRef> ResolveColumn(const Scope& scope, const parser::ColumnName& name) {
    std::uint16_t up = 0;
    for (const Scope* s = &scope; s != nullptr; s = s->parent, ++up) {
        if (name.qualified()) {
            for (std::size_t i = 0; i < s->relations.size(); ++i) {
                if (!IEquals(s->relations[i].binding, name.qualifier)) continue;

                std::uint16_t col_pos = 0;
                if (!FindColumnPos(s->relations[i].access->schema, name.name, col_pos)) {
                    return Status::InvalidArgument("relation '" + name.qualifier +
                                                    "' has no column '" + name.name + "'" +
                                                    Position(name.byte_offset));
                }
                return ColumnRef{up, static_cast<std::uint16_t>(i), col_pos};
            }
            continue;  // not at this level; try the enclosing one
        }

        // Unqualified: count matches at this level only.
        std::optional<ColumnRef> found;
        for (std::size_t i = 0; i < s->relations.size(); ++i) {
            std::uint16_t col_pos = 0;
            if (!FindColumnPos(s->relations[i].access->schema, name.name, col_pos)) continue;
            if (found.has_value()) {
                return Status::InvalidArgument(
                    "column '" + name.name + "' is ambiguous" + Position(name.byte_offset) +
                    ": more than one relation in scope has it; qualify it (`a." + name.name + "`)");
            }
            found = ColumnRef{up, static_cast<std::uint16_t>(i), col_pos};
        }
        if (found.has_value()) return *found;
        // No match at this level: keep going outward. This is the
        // innermost-first rule; a level that matches stops the search
        // above, so an outer relation is only consulted when no inner one
        // has the column at all.
    }

    if (name.qualified()) {
        return Status::InvalidArgument("'" + name.qualifier + "." + name.name +
                                        "' names no relation in scope" +
                                        Position(name.byte_offset));
    }
    return Status::InvalidArgument("no relation in scope has a column '" + name.name + "'" +
                                    Position(name.byte_offset));
}

// The step a reference is available at: for `up == 0`, its own rel_slot.
// A reference into an enclosing chain is available immediately, since the
// outer row is already bound before this chain runs.
std::uint16_t AvailableAt(const ColumnRef& ref) { return ref.up == 0 ? ref.rel_slot : 0; }

// The latest step a predicate depends on - the earliest point at which it
// can be evaluated, and therefore where it is attached. Deterministic
// from the predicate alone, which is why this is placement and not
// optimization.
std::uint16_t PredicateReadyAt(const StepPredicate& pred) {
    std::uint16_t at = AvailableAt(pred.lhs);
    if (pred.rhs.kind == OperandKind::kColumn) {
        at = std::max(at, AvailableAt(pred.rhs.column));
    }
    return at;
}

bool IsPrimaryKey(const ColumnRef& ref) {
    // Invariant 11: a relation's first column is its system-generated pk,
    // carried by the Keystone word. It is the only column a descent can
    // address, so it is the only one that can make a step a lookup.
    return ref.col_pos == 0;
}

// Whether `ref` names a column of the step at `slot` in this chain.
bool IsOwnColumn(const ColumnRef& ref, std::uint16_t slot) {
    return ref.up == 0 && ref.rel_slot == slot;
}

// A non-negative integer literal, as a pk bound. Anything else - a string,
// a NULL, a declared `$param`, a negative number - is not a pk value
// (invariant 7: ids are zero-extended 40-bit), so it cannot bound a range.
std::optional<std::uint64_t> PkBound(const Operand& operand) {
    if (operand.kind != OperandKind::kLiteral) return std::nullopt;
    if (operand.literal.type != parser::ValueType::kInt) return std::nullopt;
    if (operand.literal.int_val < 0) return std::nullopt;
    return static_cast<std::uint64_t>(operand.literal.int_val);
}

// The pk bounds this step's residual implies, if any.
//
// Reads the *lowered* conjuncts rather than the AST's `BETWEEN`, which is
// deliberate: it means a hand-written `id >= 1 AND id <= 5` gets the same
// range as `id BETWEEN 1 AND 5`, because they are the same statement once
// the parser is done with them. A range from one spelling and not the other
// would be an optimizer that rewards phrasing.
std::optional<RangeBounds> PkRangeOf(const Step& step, std::uint16_t slot) {
    std::optional<std::uint64_t> low;
    std::optional<std::uint64_t> high;
    for (const StepPredicate& pred : step.residual) {
        if (!IsOwnColumn(pred.lhs, slot) || !IsPrimaryKey(pred.lhs)) continue;
        auto bound = PkBound(pred.rhs);
        if (!bound.has_value()) continue;
        // Only the inclusive forms, which is what BETWEEN lowers to. A
        // strict `>` would need low+1 and an underflow check for nothing:
        // the grammar has no way to write one against a pk that BETWEEN
        // does not already cover.
        if (pred.op == parser::CompareOp::kGte && (!low || *bound > *low)) low = bound;
        if (pred.op == parser::CompareOp::kLte && (!high || *bound < *high)) high = bound;
    }
    if (!low.has_value() || !high.has_value()) return std::nullopt;
    // An inverted range is legal to write and matches nothing. Left as a
    // plain scan: the residual returns the correct empty answer, and a
    // range walk would have to special-case it anyway.
    if (*low > *high) return std::nullopt;
    return RangeBounds{*low, *high};
}

// Whether this step has at least one equality against a literal on a
// non-pk column that carries no index - the thing kFilterScan names.
//
// The index check is `Catalog::FindIndexOnColumn`, which until now had no
// production reader at all. Nothing creates an index today, so the answer
// is always "unindexed" - but asking makes the kind mean what it says, so
// the day index scans land this stops classifying an indexed column as a
// filter scan without anyone having to remember to come back.
bool HasUnindexedEqualityFilter(catalog::Catalog& catalog, const Step& step,
                                std::uint16_t slot) {
    for (const StepPredicate& pred : step.residual) {
        if (pred.op != parser::CompareOp::kEq) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (!IsOwnColumn(pred.lhs, slot) || IsPrimaryKey(pred.lhs)) continue;
        if (catalog.FindIndexOnColumn(step.rel_oid, pred.lhs.col_pos).ok()) continue;
        return true;
    }
    return false;
}

// The Cabin this step can probe, or nullopt.
//
// The shape is exactly `HasUnindexedEqualityFilter`'s - an own-relation,
// non-pk equality against a literal - and that is the point: a cabined
// column's equality *is* the filter scan, and the Cabin is the reason it
// need not be one.
//
// **Catalog state only.** `TableAccess::cabin_mask` is a DDL fact carried on
// the relation's cache entry (schema.hpp), so this asks no question about
// the data and the plan stays `f(shape, catalog)`. Whether the *value* has
// been observed is runtime state and belongs to the executor's branch, not
// to the kind.
//
// The **first** such equality wins when a statement filters two cabined
// columns. Deterministic and written-order, like everything else here: it
// is not an optimizer choosing the more selective one, because choosing
// would need statistics the compiler does not consult and would make the
// same statement compile differently as the data changed - which is exactly
// what a recorded pattern must not do.
std::optional<CabinProbe> CabinProbeOf(const catalog::TableAccess& access, const Step& step,
                                       std::uint16_t slot) {
    if (access.cabin_mask == 0) return std::nullopt;
    for (const StepPredicate& pred : step.residual) {
        if (pred.op != parser::CompareOp::kEq) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (!IsOwnColumn(pred.lhs, slot) || IsPrimaryKey(pred.lhs)) continue;

        const catalog::TableAccess::CabinRef cabin = access.CabinOn(pred.lhs.col_pos);
        if (cabin.id == 0) continue;

        // A `$param` never probes a Cabin. A declared pattern's body is
        // compiled to be type-checked and fingerprinted, never run, so
        // there is no value to key an entry set on - and unlike the pk case
        // above, nothing is lost by declining: kCabinProbe is search-class,
        // so the declaration's replayability verdict is the same either way.
        if (pred.rhs.literal.type == parser::ValueType::kParam) continue;

        CabinProbe probe;
        probe.cabin_id = cabin.id;
        probe.col_pos = pred.lhs.col_pos;
        probe.value = pred.rhs.literal;
        probe.declared = cabin.origin == catalog::kCabinOriginUser;
        return probe;
    }
    return std::nullopt;
}

// The columns a step's access is keyed or filtered on, ascending and
// deduplicated. This is the statistics key (stats/access_stats.hpp).
std::vector<std::uint16_t> AccessColumnsOf(const Step& step, std::uint16_t slot) {
    std::vector<std::uint16_t> out;
    switch (step.kind) {
        case AccessKind::kLookup:
        case AccessKind::kProbe:
        case AccessKind::kRange:
            // All three address the relation by its pk and nothing else.
            out.push_back(0);
            return out;
        case AccessKind::kCabinProbe:
            // The cabined column alone, not every filtered column: the
            // access was assigned *for* that one, and the rest are residual
            // whatever the kind. Reporting them all would merge this shape
            // with the filter scan below and lose the distinction the
            // statistics exist to draw.
            if (step.cabin.has_value()) out.push_back(step.cabin->col_pos);
            return out;
        case AccessKind::kFilterScan:
            for (const StepPredicate& pred : step.residual) {
                if (pred.op != parser::CompareOp::kEq) continue;
                if (pred.rhs.kind != OperandKind::kLiteral) continue;
                if (!IsOwnColumn(pred.lhs, slot) || IsPrimaryKey(pred.lhs)) continue;
                out.push_back(pred.lhs.col_pos);
            }
            break;
        case AccessKind::kScan:
            // Nothing steered it. A bare walk is one shape however many
            // non-equality conjuncts it happens to carry.
            return out;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

namespace {

// True if any reference inside a compiled sub-chain points outward. This
// is the whole of correlation analysis: structural, not heuristic, so the
// same statement classifies the same way on every execution - which is
// what lets a trail be recorded against the decision.
bool ReferencesAnOuterChain(const std::vector<Step>& steps);

bool OperandEscapes(const Operand& operand) {
    return operand.kind == OperandKind::kColumn && operand.column.up > 0;
}

bool SubChainEscapes(const SubChain& sub) {
    // `lhs` belongs to the enclosing chain by construction (it is the
    // outer column being tested), so it is not what makes the sub-chain
    // correlated - only a reference from *inside* pointing out is.
    return ReferencesAnOuterChain(sub.steps);
}

// The latest relation of an enclosing chain that a nested chain reaches
// into, expressed as that chain's rel_slot. `from_depth` is how many
// levels up the chain in question is: 1 while walking a direct child.
//
// A grandchild referring to the same chain carries a larger `up`, so the
// walk descends with from_depth + 1 rather than ignoring it - otherwise a
// correlated sub-sub-query would be placed a step too early, before the
// row it reads exists.
std::uint16_t DeepestReferenceIntoThisChain(const std::vector<Step>& steps,
                                            std::uint16_t from_depth) {
    std::uint16_t deepest = 0;
    auto consider = [&](const ColumnRef& ref) {
        if (ref.up == from_depth) deepest = std::max(deepest, ref.rel_slot);
    };
    for (const Step& step : steps) {
        for (const StepPredicate& pred : step.residual) {
            consider(pred.lhs);
            if (pred.rhs.kind == OperandKind::kColumn) consider(pred.rhs.column);
        }
        if (step.key.has_value() && step.key->kind == OperandKind::kColumn) {
            consider(step.key->column);
        }
        for (const SubChain& sub : step.sub_chains) {
            if (sub.has_value) consider(sub.lhs);
            deepest = std::max(deepest,
                               DeepestReferenceIntoThisChain(sub.steps,
                                                             static_cast<std::uint16_t>(
                                                                 from_depth + 1)));
        }
    }
    return deepest;
}

bool ReferencesAnOuterChain(const std::vector<Step>& steps) {
    for (const Step& step : steps) {
        for (const StepPredicate& pred : step.residual) {
            if (pred.lhs.up > 0) return true;
            if (OperandEscapes(pred.rhs)) return true;
        }
        if (step.key.has_value() && OperandEscapes(*step.key)) return true;
        for (const SubChain& sub : step.sub_chains) {
            // A grandchild's reference to *this* level shows up as up==1
            // inside it; anything deeper escapes past us too.
            if (SubChainEscapes(sub)) return true;
        }
    }
    return false;
}

// Compiles one query block. `parent` is the enclosing scope, or nullptr at
// the top level; `next_step_id` is the statement-wide counter every block
// shares, so a step_id is unambiguous without parent linkage.
StatusOr<StepChain> CompileBlock(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                                 const Scope* parent, std::uint32_t& next_step_id,
                                 std::uint32_t depth);

}  // namespace

StatusOr<StepChain> Compile(catalog::Catalog& catalog, const parser::SelectStmt& stmt) {
    std::uint32_t next_step_id = 0;
    return CompileBlock(catalog, stmt, /*parent=*/nullptr, next_step_id, /*depth=*/0);
}

StatusOr<Step> CompileWhere(catalog::Catalog& catalog, const catalog::TableAccess& access,
                            std::string_view binding,
                            const std::vector<parser::Condition>& where) {
    Scope scope;
    scope.relations.push_back(BoundRelation{std::string(binding), &access});

    Step out;
    out.rel_oid = access.oid;
    out.kind = AccessKind::kScan;  // the caller walks the relation itself

    std::uint32_t next_step_id = 1;  // 0 is this step
    for (const parser::Condition& cond : where) {
        if (cond.has_subquery()) {
            SubChain sub;
            sub.kind = cond.kind;
            sub.op = cond.op;

            const bool tests_a_column = cond.kind != parser::PredicateKind::kExists &&
                                        cond.kind != parser::PredicateKind::kNotExists;
            if (tests_a_column) {
                auto lhs = ResolveColumn(scope, cond.col);
                if (!lhs.ok()) return lhs.status();
                sub.lhs = lhs.value();
            }

            auto inner = CompileBlock(catalog, *cond.subquery, &scope, next_step_id, /*depth=*/1);
            if (!inner.ok()) return inner.status();
            sub.steps = std::move(inner.value().steps);

            if (tests_a_column) {
                if (inner.value().projection.size() != 1) {
                    return Status::Unsupported(
                        "a subquery used as a value must project exactly one column" +
                        Position(cond.col.byte_offset));
                }
                sub.value = inner.value().projection[0];
                sub.has_value = true;
            }
            // Correlation is still classified, even though every
            // sub-chain is attached to the step here - the flag is what a
            // later reader needs to know why one is re-run per row.
            sub.correlated = SubChainEscapes(sub);
            out.sub_chains.push_back(std::move(sub));
            continue;
        }

        auto lhs = ResolveColumn(scope, cond.col);
        if (!lhs.ok()) return lhs.status();

        // `BETWEEN` lowers to its two ordinary conjuncts and nothing else.
        // The range that may later be put on the step is a hint *on top of*
        // these (step_chain.hpp) - so a chain that ignored every range would
        // still return the same rows, which is the property that makes the
        // kind safe to add.
        if (cond.kind == parser::PredicateKind::kBetween) {
            StepPredicate low;
            low.lhs = lhs.value();
            low.op = parser::CompareOp::kGte;
            low.rhs.kind = OperandKind::kLiteral;
            low.rhs.literal = cond.val;
            out.residual.push_back(low);

            StepPredicate high;
            high.lhs = lhs.value();
            high.op = parser::CompareOp::kLte;
            high.rhs.kind = OperandKind::kLiteral;
            high.rhs.literal = cond.val_high;
            out.residual.push_back(high);
            continue;
        }

        StepPredicate pred;
        pred.lhs = lhs.value();
        pred.op = cond.op;
        if (cond.rhs_kind == parser::RhsKind::kColumn) {
            auto rhs = ResolveColumn(scope, cond.rhs_col);
            if (!rhs.ok()) return rhs.status();
            pred.rhs.kind = OperandKind::kColumn;
            pred.rhs.column = rhs.value();
        } else {
            pred.rhs.kind = OperandKind::kLiteral;
            pred.rhs.literal = cond.val;
        }
        out.residual.push_back(pred);
    }
    return out;
}

namespace {

StatusOr<StepChain> CompileBlock(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                                 const Scope* parent, std::uint32_t& next_step_id,
                                 std::uint32_t depth) {
    // The execute-time half of spec I15 R3: recursion is bounded at both
    // ends. The parser caps nesting too, but a chain can also be built by
    // something other than a parse, and a bound that only one producer
    // enforces is not a bound.
    if (depth > parser::kMaxSubqueryDepth) {
        return Status::Unsupported("subquery nesting deeper than " +
                                    std::to_string(parser::kMaxSubqueryDepth) +
                                    " is not supported");
    }

    // ---- 1. Bind every relation in written order --------------------------
    Scope scope;
    scope.parent = parent;
    std::vector<const parser::RelationRef*> refs;
    refs.push_back(&stmt.from);
    for (const parser::JoinClause& j : stmt.joins) refs.push_back(&j.relation);

    for (const parser::RelationRef* rel : refs) {
        auto oid = catalog.FindTableOidByName(rel->table_name);
        if (!oid.ok()) return oid.status();
        auto access = catalog.InitTableAccess(oid.value());
        if (!access.ok()) return access.status();
        scope.relations.push_back(BoundRelation{rel->binding(), access.value()});
    }

    StepChain chain;
    chain.steps.resize(scope.relations.size());
    for (std::size_t i = 0; i < scope.relations.size(); ++i) {
        chain.steps[i].step_id = next_step_id++;
        chain.steps[i].rel_oid = scope.relations[i].access->oid;
        // Display only - see Step::rel_name. The written table name, plus
        // the alias when one was given, because a plan naming only the
        // alias cannot be matched back to a table and one naming only the
        // table cannot be matched back to the ON clause.
        chain.steps[i].rel_name = refs[i]->table_name;
        if (scope.relations[i].binding != refs[i]->table_name) {
            chain.steps[i].rel_name += " AS " + scope.relations[i].binding;
        }
        chain.steps[i].kind = AccessKind::kScan;  // upgraded below, never down
    }

    // ---- 2. Lower every conjunct into a StepPredicate ---------------------
    //
    // The ON clauses and the WHERE clause become one flat list. They are
    // the same thing to the executor - a condition a row must satisfy -
    // and keeping them apart would mean two evaluation paths that can
    // disagree. An inner join's ON is not semantically distinct from a
    // WHERE conjunct; only an outer join would make it so, and outer
    // joins are Unsupported (spec I9).
    std::vector<StepPredicate> predicates;

    for (const parser::JoinClause& join : stmt.joins) {
        auto lhs = ResolveColumn(scope, join.left);
        if (!lhs.ok()) return lhs.status();
        auto rhs = ResolveColumn(scope, join.right);
        if (!rhs.ok()) return rhs.status();

        StepPredicate pred;
        pred.lhs = lhs.value();
        pred.op = parser::CompareOp::kEq;
        pred.rhs.kind = OperandKind::kColumn;
        pred.rhs.column = rhs.value();
        predicates.push_back(pred);
    }

    // Sub-chains, kept beside the flat predicates until placement.
    std::vector<SubChain> sub_chains;

    for (const parser::Condition& cond : stmt.where) {
        if (cond.has_subquery()) {
            SubChain sub;
            sub.kind = cond.kind;
            sub.op = cond.op;

            // The outer column being tested, for the forms that have one.
            // EXISTS and NOT EXISTS have nothing on their left.
            const bool tests_a_column = cond.kind != parser::PredicateKind::kExists &&
                                        cond.kind != parser::PredicateKind::kNotExists;
            if (tests_a_column) {
                auto lhs = ResolveColumn(scope, cond.col);
                if (!lhs.ok()) return lhs.status();
                sub.lhs = lhs.value();
            }

            // Compiled against a scope whose parent is *this* one, which
            // is what turns an inner reference to an outer column into
            // `up == 1` rather than a resolution failure.
            auto inner = CompileBlock(catalog, *cond.subquery, &scope, next_step_id, depth + 1);
            if (!inner.ok()) return inner.status();
            sub.steps = std::move(inner.value().steps);

            if (tests_a_column) {
                // IN and the scalar form read a value out of the
                // sub-chain, so it must project exactly one column. `*`
                // over a relation with several columns has no single
                // value to mean, and picking the first would make the
                // answer depend on schema order.
                if (inner.value().projection.size() != 1) {
                    return Status::Unsupported(
                        "a subquery used as a value must project exactly one column" +
                        Position(cond.col.byte_offset));
                }
                sub.value = inner.value().projection[0];
                sub.has_value = true;
            }

            sub.correlated = SubChainEscapes(sub);
            sub_chains.push_back(std::move(sub));
            continue;
        }

        auto lhs = ResolveColumn(scope, cond.col);
        if (!lhs.ok()) return lhs.status();

        // `BETWEEN` lowers to its two ordinary conjuncts and nothing else.
        // The range that may later be put on the step is a hint *on top of*
        // these (step_chain.hpp) - so a chain that ignored every range would
        // still return the same rows, which is the property that makes the
        // kind safe to add.
        if (cond.kind == parser::PredicateKind::kBetween) {
            StepPredicate low;
            low.lhs = lhs.value();
            low.op = parser::CompareOp::kGte;
            low.rhs.kind = OperandKind::kLiteral;
            low.rhs.literal = cond.val;
            predicates.push_back(low);

            StepPredicate high;
            high.lhs = lhs.value();
            high.op = parser::CompareOp::kLte;
            high.rhs.kind = OperandKind::kLiteral;
            high.rhs.literal = cond.val_high;
            predicates.push_back(high);
            continue;
        }

        StepPredicate pred;
        pred.lhs = lhs.value();
        pred.op = cond.op;
        if (cond.rhs_kind == parser::RhsKind::kColumn) {
            // Resolved against the same scope, so an outward reference
            // here is exactly what makes an enclosing sub-chain
            // correlated.
            auto rhs = ResolveColumn(scope, cond.rhs_col);
            if (!rhs.ok()) return rhs.status();
            pred.rhs.kind = OperandKind::kColumn;
            pred.rhs.column = rhs.value();
        } else {
            pred.rhs.kind = OperandKind::kLiteral;
            pred.rhs.literal = cond.val;
        }
        predicates.push_back(pred);
    }

    // ---- 3. Attach each conjunct to the step that makes it evaluable -----
    for (const StepPredicate& pred : predicates) {
        chain.steps[PredicateReadyAt(pred)].residual.push_back(pred);
    }

    // Sub-chains are placed by the same rule. An uncorrelated one depends
    // on no outer row at all, so it is hoisted out of the loop entirely;
    // a correlated one attaches to the latest step it reaches into, which
    // is the earliest point its correlation values exist.
    for (SubChain& sub : sub_chains) {
        // Only a sub-chain with **no outer column** can be lifted out of
        // the row loop entirely - which is EXISTS and NOT EXISTS, the two
        // that ask whether a row appeared and nothing else.
        //
        // A value-bearing form (`IN`, `NOT IN`, scalar) is a different
        // shape even when uncorrelated: its *set* is row-independent, but
        // the comparison against each outer row is not. Spec §2 calls
        // that a "hoisted probe set", which is a materialized set plus a
        // per-row test - not a predicate evaluated once. Until the set is
        // materialized, it attaches to the step carrying its outer
        // column and re-runs per row: correct, and slower than it needs
        // to be for an uncorrelated one.
        if (!sub.correlated && !sub.has_value) {
            chain.hoisted.push_back(std::move(sub));
            continue;
        }
        std::uint16_t ready_at = sub.has_value ? AvailableAt(sub.lhs) : 0;
        ready_at = std::max(ready_at, DeepestReferenceIntoThisChain(sub.steps, /*from_depth=*/1));
        chain.steps[ready_at].sub_chains.push_back(std::move(sub));
    }

    // ---- 4. Assign an access kind ----------------------------------------
    //
    // One rule, applied per step: the step is a lookup or a probe iff some
    // conjunct attached to it is an equality binding **this relation's pk**
    // to a value already available. Anything else is a scan.
    //
    // Deliberately NOT PkEqualityTarget (the dispatcher's point-statement
    // check): that refuses whenever the WHERE holds more than one
    // condition, which is right for a statement that must answer with a
    // single tuple and cannot shortcut a second predicate - but a chain
    // step only *locates* a candidate, and every residual is evaluated on
    // the located row before it is accepted. Reusing it would degrade
    // every chain carrying a WHERE clause to a full scan per step.
    for (std::size_t i = 0; i < chain.steps.size(); ++i) {
        Step& step = chain.steps[i];
        for (const StepPredicate& pred : step.residual) {
            if (pred.op != parser::CompareOp::kEq) continue;

            // Equality is symmetric, and an ON clause can be written
            // either way round - `ON a.b_id = b.id` and `ON b.id = a.b_id`
            // are the same join. Both orientations are examined, or which
            // relation could probe would depend on the order the client
            // happened to type the two sides in.
            const bool lhs_is_key = pred.lhs.up == 0 && pred.lhs.rel_slot == i &&
                                    IsPrimaryKey(pred.lhs);
            const bool rhs_is_key = pred.rhs.kind == OperandKind::kColumn &&
                                    pred.rhs.column.up == 0 && pred.rhs.column.rel_slot == i &&
                                    IsPrimaryKey(pred.rhs.column);

            // The value the descent would be keyed on: whichever side is
            // not this step's pk.
            std::optional<Operand> candidate;
            if (lhs_is_key) {
                candidate = pred.rhs;
            } else if (rhs_is_key) {
                Operand from_lhs;
                from_lhs.kind = OperandKind::kColumn;
                from_lhs.column = pred.lhs;
                candidate = from_lhs;
            } else {
                continue;  // neither side is this relation's pk
            }

            if (candidate->kind == OperandKind::kLiteral) {
                // A declared pattern's `$param` is pk-eligible, and it has
                // to be. The access kind *is* Waystone's trust model
                // (step_chain.hpp), so a `WHERE id = $x` body that compiled
                // to kScan would be reported as un-replayable at CREATE
                // PATTERN - a warning about precisely the shape declaring a
                // pattern exists to make replayable. A param stands for an
                // integer the traffic will supply, so it is treated as one
                // here; the chain still never executes.
                const bool param = candidate->literal.type == parser::ValueType::kParam;
                // A negative literal cannot be a pk: ids are zero-extended
                // 40-bit values (invariant 7), so this equality can never
                // hold. Left as a scan with the residual intact, which
                // returns the correct empty answer rather than probing an
                // enormous unsigned key.
                if (!param && (candidate->literal.type != parser::ValueType::kInt ||
                               candidate->literal.int_val < 0)) {
                    continue;
                }
                step.kind = AccessKind::kLookup;
                step.key = candidate;
                break;
            }
            // A column: this is a probe iff the value is produced by an
            // *earlier* step, or by an enclosing chain's row. A reference
            // to this step or a later one is not available when the
            // descent would happen.
            if (candidate->column.up > 0 || AvailableAt(candidate->column) < i) {
                step.kind = AccessKind::kProbe;
                step.key = candidate;
                break;
            }
        }

        // ---- Range and filter scans -------------------------------------
        //
        // Only reached when the step did not become a keyed descent: a
        // relation with a pk equality is already served better than either
        // of these could serve it.
        if (step.kind == AccessKind::kScan) {
            if (auto bounds = PkRangeOf(step, static_cast<std::uint16_t>(i));
                bounds.has_value()) {
                step.kind = AccessKind::kRange;
                step.range = bounds;
            } else if (auto probe = CabinProbeOf(*scope.relations[i].access, step,
                                                  static_cast<std::uint16_t>(i));
                       probe.has_value()) {
                // Ahead of kFilterScan, and only ahead of it: a cabined
                // column's equality is exactly the shape a filter scan
                // names, and the Cabin is the reason it need not be one.
                step.kind = AccessKind::kCabinProbe;
                step.cabin = std::move(probe);
            } else if (HasUnindexedEqualityFilter(catalog, step,
                                                   static_cast<std::uint16_t>(i))) {
                step.kind = AccessKind::kFilterScan;
            }
        }

        // The columns the kind was assigned for. Recorded here because the
        // compiler is the only place that already knows them; the statistics
        // layer would otherwise re-walk the residual per statement to answer
        // a question that was settled at compile time.
        step.access_columns = AccessColumnsOf(step, static_cast<std::uint16_t>(i));
    }

    // ---- 5. Projection ---------------------------------------------------
    for (const parser::ColumnName& col : stmt.projection) {
        auto ref = ResolveColumn(scope, col);
        if (!ref.ok()) return ref.status();
        chain.projection.push_back(ref.value());
        chain.column_names.push_back(col.qualified() ? col.qualifier + "." + col.name : col.name);
    }
    if (stmt.projection.empty()) {
        // `SELECT *`, which the grammar admits only for a single relation
        // (V06) - so it means every column of the one step, in schema
        // order. Left as an empty projection with names filled in, since
        // the executor emits the whole decoded row in that case.
        for (const auto& column : scope.relations[0].access->schema.columns) {
            chain.column_names.push_back(std::string(catalog::NameView(column.name)));
        }
    }

    // ---- 6. Class --------------------------------------------------------
    //
    // J3: every step-chain statement is kJoinSelect, read as "step-chain
    // select". A single-relation statement keeps its point/range class.
    // Projection shape must never affect this - two statements differing
    // only in which columns they name read the same rows by the same
    // access path - which is why nothing below looks at `projection`.
    if (chain.steps.size() > 1) {
        chain.klass = StatementClass::kJoinSelect;
    } else if (chain.steps[0].kind == AccessKind::kLookup) {
        chain.klass = StatementClass::kPointSelect;
    } else if (chain.steps[0].kind == AccessKind::kRange) {
        chain.klass = StatementClass::kRangeSelect;
    } else {
        chain.klass = StatementClass::kJoinSelect;  // a one-relation scan is a chain of one
    }

    return chain;
}

}  // namespace

}  // namespace kds::exec
