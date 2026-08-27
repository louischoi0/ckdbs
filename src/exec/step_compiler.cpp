#include "kds/exec/step_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// For `CoerceLiteralToColumn`. The literal parsers themselves are no
// longer reached from here: the compiler asks the codec to coerce, and the
// codec owns which parser that means - which is what keeps the value a
// predicate compares and the value a write keys on identical by
// construction rather than by two call sites agreeing.
#include "kds/exec/index_key.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/storage/index/index_page.hpp"  // kIndexPkWidth - the sort-key suffix

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
// The two rules, from docs/spec/parser-v2.md's resolution section:
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
StatusOr<ColumnRef> ResolveColumn(const Scope& scope, const parser::ColumnName& name);

// The column heading one fold item carries: `b`, `count(*)`,
// `sum(distinct x)`. Built from what was *written*, so a client sees back
// the shape it sent - which is the one place a name is allowed to survive
// compilation, for the reason `column_names` already existed.
std::string AggregateLabel(const parser::SelectItem& item) {
    const std::string written =
        item.column.qualified() ? item.column.qualifier + "." + item.column.name
                                : item.column.name;
    if (!item.is_aggregate) return written;

    std::string out(parser::AggFuncText(item.func));
    out += '(';
    if (item.distinct) out += "distinct ";
    out += item.star_arg ? "*" : written;
    out += ')';
    return out;
}

// The type a resolved reference points at. The scope holds every relation's
// schema already, so this is an index rather than a catalog read.
const catalog::SysColumnRow& ColumnAt(const Scope& scope, const ColumnRef& ref) {
    const Scope* s = &scope;
    for (std::uint16_t i = 0; i < ref.up; ++i) s = s->parent;
    return s->relations[ref.rel_slot].access->schema.columns[ref.col_pos];
}

// The whole right-hand side of one lowered conjunct, coerced or refused.
//
// Called from **both** lowering sites - the SELECT chain's and the write
// filter's - because a literal that means one thing in a WHERE and another
// in an UPDATE's WHERE is exactly the drift this is here to stop.
Status CoercePredicate(const Scope& scope, StepPredicate& pred, std::uint32_t byte_offset) {
    const catalog::SysColumnRow& lhs = ColumnAt(scope, pred.lhs);

    if (pred.rhs.kind == OperandKind::kLiteral) {
        // The shared coercion (row_codec.hpp), so the value a predicate
        // compares and the value the write path keys a Cabin on are
        // produced by one routine. They were not, once: the Cabin's write
        // hook keyed on the raw literal while the read path keyed on the
        // coerced one, and an observed date silently stopped seeing new
        // rows.
        //
        // Errors come back unpositioned - the parsers have no idea where a
        // literal was written - so the position is added here, where the
        // offset is.
        Status s = CoerceLiteralToColumn(lhs, pred.rhs.literal);
        if (!s.ok()) {
            return s.WithContext("column '" + std::string(catalog::NameView(lhs.name)) +
                                 "' literal" + Position(pred.rhs.literal.byte_offset));
        }
        return Status::OK();
    }

    // Column against column. Only one thing is checked here, and it is the
    // one the runtime cannot recover from: two DECIMALs of different scale
    // compare unscaled integers that mean different things, so `1.50` would
    // equal `1.500`'s stored 1500 only by accident of digits. Refused at
    // compile rather than rescaled, because rescaling either drops digits
    // or invents them - TY6 defers that decision whole, and a residual is
    // the worst place to pre-empt it.
    const catalog::SysColumnRow& rhs = ColumnAt(scope, pred.rhs.column);
    const auto is_decimal = [](std::uint32_t tv) {
        return tv == catalog::kTypeValDecimal || tv == catalog::kTypeValDecimalWide;
    };
    if (is_decimal(lhs.type_val) && is_decimal(rhs.type_val)) {
        // Different *widths* are refused before scales are even looked at:
        // an 8-byte and a 16-byte decimal can never share a (p, s) - the
        // width is a function of p - and letting the pair through would
        // reach CompareValues as a kind mismatch, which answers false per
        // row. A statement that can only ever answer no rows is a
        // statement to refuse with a reason, not to run.
        if (lhs.type_val != rhs.type_val) {
            return Status::Unsupported(
                "cannot compare decimal columns of different width: '" +
                std::string(catalog::NameView(lhs.name)) + "' and '" +
                std::string(catalog::NameView(rhs.name)) +
                "' are on opposite sides of the 18-digit precision split; this engine does "
                "not rescale" + Position(byte_offset));
        }
        if (catalog::DecimalScaleOf(lhs.len) != catalog::DecimalScaleOf(rhs.len)) {
            return Status::Unsupported(
                "cannot compare decimal columns of different scale: '" +
                std::string(catalog::NameView(lhs.name)) + "' has scale " +
                std::to_string(catalog::DecimalScaleOf(lhs.len)) + " and '" +
                std::string(catalog::NameView(rhs.name)) + "' has scale " +
                std::to_string(catalog::DecimalScaleOf(rhs.len)) +
                "; this engine does not rescale" + Position(byte_offset));
        }
    }
    return Status::OK();
}

// AG3's arithmetic constraints, stated as product facts rather than
// discovered at execute time (spec §3.3).
//
// The accumulator is int64 with checked addition, so the two refusals are
// different in kind and get different codes. A `SUM` over text is a
// statement that does not typecheck - InvalidArgument, the client wrote the
// wrong column. A `SUM` over `uint64` typechecks and is *declined*: half its
// range does not fit the accumulator, and a sum of Keystone ids is a
// statement nobody meant. `MIN`/`MAX` over `uint64` are exact and stay
// allowed, because comparison goes through the digit-text path rather than
// through a signed reading.
Status CheckAggregateArgType(const parser::SelectItem& item, std::uint32_t type_val,
                             const std::string& label) {
    // ---- AVG (aggregate.md §3.4, decided 2026-08-07) ---------------
    //
    // One principle answers all three of §10's questions: **AVG never
    // invents digits and never drops declared ones** - it answers at
    // exactly the scale the schema declared, rounding half-even. A decimal
    // column declared its scale, `DECIMAL(p, 0)` included, so it averages;
    // an integer column declared none, so any fractional answer would
    // manufacture a scale and a whole-number one would silently discard
    // the remainder - refused, with the client's two honest options named.
    if (item.func == parser::AggFunc::kAvg) {
        if (type_val == catalog::kTypeValDate || type_val == catalog::kTypeValTimestamp) {
            return Status::InvalidArgument(
                "AVG over a date or timestamp column is not a value (" + label + ")" +
                Position(item.byte_offset) +
                "; it is SUM over one wearing a divide, and a sum of dates is a statement "
                "nobody meant");
        }
        if (type_val != catalog::kTypeValDecimal && type_val != catalog::kTypeValDecimalWide) {
            return Status::InvalidArgument(
                "AVG requires a decimal column (" + label + ")" + Position(item.byte_offset) +
                "; the answer is given at the column's declared scale, and this column "
                "declares none - declare DECIMAL(p, s), or compute SUM and COUNT and choose "
                "your own rounding");
        }
        return Status::OK();
    }

    if (item.func != parser::AggFunc::kSum) return Status::OK();

    if (type_val == catalog::kTypeValUint64) {
        return Status::Unsupported(
            "SUM over a uint64 column is not supported (" + label + ")" +
            Position(item.byte_offset) +
            "; half its range does not fit the int64 accumulator, and a wrapped sum is wrong "
            "in a way no reader can detect");
    }
    // TY05 / types.md §3.2. A `DECIMAL` sums: its unscaled int64 goes
    // through the same checked adder, and the answer's scale is the
    // column's, so nothing about the accumulator changes. A `DATE` or
    // `TIMESTAMP` does not - both are integers underneath, so summing one
    // would *work* and produce a number that is not a date, a time, or an
    // interval. A sum of dates is a statement nobody meant, and this is
    // the one chance to say so.
    if (type_val == catalog::kTypeValDate || type_val == catalog::kTypeValTimestamp) {
        return Status::InvalidArgument("SUM over a date or timestamp column is not a value (" +
                                        label + ")" + Position(item.byte_offset) +
                                        "; MIN and MAX over one are exact and are what this "
                                        "engine offers");
    }
    // The wide decimal sums too, through an int128 accumulator of its own
    // (aggregate.cpp) - same checked-addition discipline, wider register.
    if (type_val != catalog::kTypeValDecimal && type_val != catalog::kTypeValDecimalWide &&
        !catalog::IsIntegerTypeVal(type_val)) {
        return Status::InvalidArgument("SUM requires a signed integer or decimal column (" +
                                        label + ")" + Position(item.byte_offset));
    }
    return Status::OK();
}

// Resolves the fold: the GROUP BY keys, then the output items (spec §4).
//
// Every check here is positioned, and every one of them is a *compile*
// check on purpose - the compile stays pure, so the same statement over the
// same catalog produces the same spec, which is what keeps the chain
// `f(shape, catalog)` and lets `pattern_id` go on naming it.
StatusOr<AggregateSpec> CompileAggregate(const Scope& scope, const parser::SelectStmt& stmt) {
    AggregateSpec spec;

    for (const parser::ColumnName& key : stmt.group_by) {
        auto ref = ResolveColumn(scope, key);
        if (!ref.ok()) return ref.status();

        // A duplicate key is always a slip, and it would double the key
        // encoding for nothing - the same group, named twice.
        for (const ColumnRef& seen : spec.group_keys) {
            if (seen != ref.value()) continue;
            return Status::InvalidArgument("column '" + key.name +
                                            "' appears twice in GROUP BY" +
                                            Position(key.byte_offset));
        }
        spec.group_keys.push_back(ref.value());
    }

    for (const parser::SelectItem& item : stmt.agg_items) {
        AggregateItem out;
        out.is_aggregate = item.is_aggregate;
        out.func = item.func;
        out.star_arg = item.star_arg;
        out.distinct = item.distinct;

        if (item.is_aggregate && item.star_arg) {
            // `COUNT(*)` reads no column, so there is nothing to resolve
            // and no type to carry.
            spec.items.push_back(out);
            continue;
        }

        auto ref = ResolveColumn(scope, item.column);
        if (!ref.ok()) return ref.status();
        out.ref = ref.value();
        const catalog::SysColumnRow& arg = ColumnAt(scope, out.ref);
        out.type_val = arg.type_val;
        if (arg.type_val == catalog::kTypeValDecimal ||
            arg.type_val == catalog::kTypeValDecimalWide) {
            out.scale = catalog::DecimalScaleOf(arg.len);
        }

        if (item.is_aggregate) {
            if (Status s = CheckAggregateArgType(item, out.type_val, AggregateLabel(item));
                !s.ok()) {
                return s;
            }
        } else {
            // AG5: a bare column in an aggregated select list must be a
            // grouping key. There is no "any row" mode and there will not
            // be one - an answer that depends on scan order is an answer
            // this engine refuses to give.
            //
            // Compared as *resolved* references, so `SELECT a.b ... GROUP
            // BY b` is accepted when both name the same column, and
            // `SELECT b ... GROUP BY c` is refused however they are
            // spelled.
            bool grouped = false;
            for (const ColumnRef& key : spec.group_keys) {
                if (key == out.ref) { grouped = true; break; }
            }
            if (!grouped) {
                return Status::InvalidArgument(
                    "column '" + item.column.name +
                    "' is selected beside an aggregate but is not in GROUP BY" +
                    Position(item.byte_offset) +
                    "; add it to GROUP BY, or aggregate it");
            }
        }
        spec.items.push_back(out);
    }
    return spec;
}

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

// ---- Equality propagation (docs/spec/parser-v2.md §5) -------------------------
//
// From `A = B` - two columns of this chain - and `B = <literal>`, append
// the implied `A = <literal>`, so the step owning A can be keyed instead
// of walked - the join whose restriction sits on the other relation,
// bench/results-scenario3-library.md §9's 10,086-page shape. The contract -
// what may be derived, why results cannot change, and what stays out of
// scope - is the §5 amendment; two facts a reader of this function needs
// that the spec cannot supply:
//
//  - The literal is copied bytes-for-bytes, so it crosses only an
//    identical (type_val, len) descriptor: it was coerced against the
//    column it was written on (CoercePredicate), and re-coercing a coerced
//    decimal would rescale it twice.
//  - At most one conjunct is derived per column, and only for a column
//    with no written equality-to-literal of its own. A second literal on
//    one column is plan-inert (a keyed candidate already exists there) and
//    result-inert (the written conjuncts fully express the predicate,
//    contradiction included) - and without the bound, a class of M columns
//    carrying L written literals appends L*(M-1) conjuncts, which turned a
//    10 KB statement into seconds of compile and tens of thousands of
//    per-row comparisons when measured.
//
// Derived conjuncts carry `StepPredicate::derived`, so ANALYZE can mark
// them and CREATE PATTERN's parameter checks can name only what the client
// wrote.
void PropagateEqualities(const Scope& scope, std::vector<StepPredicate>& predicates) {
    // Equivalence classes over this chain's columns (`up == 0` on both
    // sides) connected by written column-column equalities, in first-seen
    // order throughout - same statement, same classes, same appended
    // conjuncts (V14's purity). A merged class is folded into the
    // earlier-seen one and the later left empty rather than erased, because
    // both indices are live within one iteration.
    std::vector<std::vector<ColumnRef>> classes;
    const auto find_class = [&classes](const ColumnRef& ref) -> std::size_t {
        for (std::size_t c = 0; c < classes.size(); ++c) {
            for (const ColumnRef& member : classes[c]) {
                if (member == ref) return c;
            }
        }
        return classes.size();
    };

    const std::size_t written = predicates.size();
    for (std::size_t i = 0; i < written; ++i) {
        const StepPredicate& pred = predicates[i];
        if (pred.op != parser::CompareOp::kEq) continue;
        if (pred.rhs.kind != OperandKind::kColumn) continue;
        if (pred.lhs.up != 0 || pred.rhs.column.up != 0) continue;
        std::size_t a = find_class(pred.lhs);
        if (a == classes.size()) classes.push_back({pred.lhs});
        std::size_t b = find_class(pred.rhs.column);
        if (b == classes.size()) classes.push_back({pred.rhs.column});
        if (a == b) continue;
        if (a > b) std::swap(a, b);
        classes[a].insert(classes[a].end(), classes[b].begin(), classes[b].end());
        classes[b].clear();
    }

    // A written equality-to-literal on this exact column, in the written
    // prefix alone - derived conjuncts never beget further ones.
    const auto has_own_literal = [&predicates, written](const ColumnRef& ref) {
        for (std::size_t i = 0; i < written; ++i) {
            const StepPredicate& p = predicates[i];
            if (p.op == parser::CompareOp::kEq && p.rhs.kind == OperandKind::kLiteral &&
                p.lhs == ref) {
                return true;
            }
        }
        return false;
    };

    for (const std::vector<ColumnRef>& cls : classes) {
        for (const ColumnRef& member : cls) {
            if (has_own_literal(member)) continue;
            const catalog::SysColumnRow& dst = ColumnAt(scope, member);
            for (std::size_t i = 0; i < written; ++i) {
                const StepPredicate& p = predicates[i];
                if (p.op != parser::CompareOp::kEq) continue;
                if (p.rhs.kind != OperandKind::kLiteral) continue;
                const bool in_class = std::any_of(cls.begin(), cls.end(),
                                                  [&p](const ColumnRef& m) { return m == p.lhs; });
                if (!in_class) continue;
                const catalog::SysColumnRow& src = ColumnAt(scope, p.lhs);
                if (dst.type_val != src.type_val || dst.len != src.len) continue;
                StepPredicate derived;
                derived.lhs = member;
                derived.op = parser::CompareOp::kEq;
                derived.rhs = p.rhs;
                derived.derived = true;
                // Constructed whole before the push_back invalidates `p`,
                // and the break keeps anything dangling from being read.
                predicates.push_back(std::move(derived));
                break;
            }
        }
    }
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
// **Asks the cached `index_mask`, not the catalog.** It called
// `Catalog::FindIndexOnColumn` when nothing created indexes and the answer
// was always "unindexed"; that was a `sys.indexes` scan per equality per
// compile, and IX04 put the same fact on the relation's cache entry as one
// bit. The mask names an index's **leading** key column only, which is
// exactly the right question: an index on (a, b) cannot be entered by an
// equality on `b`, so such an equality really is an unindexed filter.
//
// Reached only after `IndexProbeOf` declined, so the two cannot disagree
// about a usable index - they can only disagree about one this step's
// literals could not be encoded into, and then calling it a filter scan is
// the truthful answer: the walk is what will happen.
bool HasUnindexedEqualityFilter(const catalog::TableAccess& access, const Step& step,
                                std::uint16_t slot) {
    for (const StepPredicate& pred : step.residual) {
        if (pred.op != parser::CompareOp::kEq) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (!IsOwnColumn(pred.lhs, slot) || IsPrimaryKey(pred.lhs)) continue;
        if (access.IndexOn(pred.lhs.col_pos) != nullptr) continue;
        return true;
    }
    return false;
}

// ---- Index selection (docs/spec/index.md §9) ----------------------------
//
// **`f(shape, catalog)`, and nothing else.** No statistics, no cardinality
// estimate, no property of the data - because a recorded pattern must not
// compile differently as the rows change, or `pattern_id` stops naming a
// plan. Same argument `CabinProbeOf` gives for taking the *first* cabined
// equality rather than the most selective one.

// The inclusive literal bounds the residual pins `col_pos` between, if any.
// `BETWEEN` lowers to `>=` and `<=`, which is the only form the grammar can
// produce, so only those two are read.
struct ColumnBounds {
    const parser::AstValue* low = nullptr;
    const parser::AstValue* high = nullptr;
};

ColumnBounds BoundsOnColumn(const Step& step, std::uint16_t slot, std::uint16_t col_pos) {
    ColumnBounds out;
    for (const StepPredicate& pred : step.residual) {
        if (!IsOwnColumn(pred.lhs, slot) || pred.lhs.col_pos != col_pos) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (pred.rhs.literal.type == parser::ValueType::kParam) continue;
        if (pred.op == parser::CompareOp::kGte) out.low = &pred.rhs.literal;
        if (pred.op == parser::CompareOp::kLte) out.high = &pred.rhs.literal;
    }
    return out;
}

// The equality literal the residual pins `col_pos` to, or nullptr.
const parser::AstValue* EqualityOnColumn(const Step& step, std::uint16_t slot,
                                          std::uint16_t col_pos) {
    for (const StepPredicate& pred : step.residual) {
        if (pred.op != parser::CompareOp::kEq) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (!IsOwnColumn(pred.lhs, slot) || pred.lhs.col_pos != col_pos) continue;
        // A `$param` never enters an index, for the reason CabinProbeOf
        // declines one: a declared pattern's body is compiled to be
        // type-checked and fingerprinted, never run, so there is no value to
        // encode a key from - and nothing is lost, since these kinds are
        // search-class and the replayability verdict is the same either way.
        if (pred.rhs.literal.type == parser::ValueType::kParam) continue;
        return &pred.rhs.literal;
    }
    return nullptr;
}

// Everything about a probe that comes from the index alone: the catalog
// fields, the column lists, and the padding templates the bounds are built
// on top of - `low` all 0x00 and `high` all 0xFF, the true tail bounds
// because a key column's discriminator byte is 1 for every value that
// exists. One constructor for both selection paths, so a field added to
// `IndexProbe` cannot be set at one site and forgotten at the other; the
// caller sets only what its own selection decided.
IndexProbe ProbeOverIndex(const catalog::TableAccess::IndexRef& ix) {
    IndexProbe probe;
    probe.index_oid = ix.index_oid;
    probe.root_page_id = ix.root_page_id;
    probe.key_width = ix.key_width;
    probe.entry_width = ix.entry_width;
    probe.key_cols.assign(ix.keys().begin(), ix.keys().end());
    probe.covered_cols.assign(ix.covered().begin(), ix.covered().end());
    const std::size_t sort_key_width =
        static_cast<std::size_t>(ix.key_width) + index::kIndexPkWidth;
    probe.low.assign(sort_key_width, std::byte{0x00});
    probe.high.assign(sort_key_width, std::byte{0xFF});
    return probe;
}

// The index this step should enter, or nullopt.
//
// Picks the **longest usable key prefix**, ties broken by lowest
// `index_oid` - which is free, because `TableAccess::indexes` is sorted by
// it and this keeps the first index to reach a given score.
std::optional<IndexProbe> IndexProbeOf(const catalog::TableAccess& access, const Step& step,
                                       std::uint16_t slot) {
    if (access.indexes.empty()) return std::nullopt;

    std::optional<IndexProbe> best;
    int best_score = 0;

    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        // How many leading key columns carry an equality, in the index's
        // declared order. Stops at the first that does not: an index on
        // (a, b) cannot be entered by `b` alone.
        std::uint8_t eq = 0;
        while (eq < ix.nkeys && EqualityOnColumn(step, slot, ix.keys()[eq]) != nullptr) ++eq;

        ColumnBounds bounds;
        const bool ranged =
            eq < ix.nkeys &&
            (bounds = BoundsOnColumn(step, slot, ix.keys()[eq])).low != nullptr &&
            bounds.high != nullptr;

        // An equality is worth more than a range on the same column, and a
        // longer prefix more than a shorter one.
        const int score = static_cast<int>(eq) * 2 + (ranged ? 1 : 0);
        if (score == 0 || score <= best_score) continue;

        // The bounds are encoded into the templates now: coercion is a
        // compile-time act and so is the encoding that follows it.
        IndexProbe probe = ProbeOverIndex(ix);
        probe.eq_prefix = eq;
        probe.ranged = ranged;

        std::size_t at = 0;
        bool encoded = true;
        for (std::uint8_t i = 0; i < eq + (ranged ? 1 : 0) && encoded; ++i) {
            const catalog::SysColumnRow& col = access.schema.columns[ix.keys()[i]];
            auto width = IndexKeyColumnWidth(col);
            if (!width.ok() || at + width.value() > ix.key_width) {
                encoded = false;
                break;
            }
            const parser::AstValue* low_value =
                i < eq ? EqualityOnColumn(step, slot, ix.keys()[i]) : bounds.low;
            const parser::AstValue* high_value =
                i < eq ? low_value : bounds.high;
            // A value the key encoder refuses - an integer wider than its
            // column, say - means this index cannot be entered for it. The
            // step falls through to the walk, which returns the identical
            // rows because the residual is untouched.
            encoded = EncodeIndexKeyColumn(col, *low_value,
                                            std::span<std::byte>(probe.low).subspan(
                                                at, width.value()))
                          .ok() &&
                      EncodeIndexKeyColumn(col, *high_value,
                                            std::span<std::byte>(probe.high).subspan(
                                                at, width.value()))
                          .ok();
            at += width.value();
        }
        if (!encoded) continue;

        best = std::move(probe);
        best_score = score;
    }
    return best;
}

// The correlated-equality shape both structure arms select on: a kEq
// column-column conjunct with one side owned by this step and the other
// available before it runs - an earlier slot, or an enclosing chain's row
// (`up > 0`), which is bound before this chain opens. Both orientations
// are examined, as the pk-probe arm does and for its reason; a
// same-relation equality excludes itself through the availability test.
// One home for the rule, so the index and cabin arms cannot drift.
struct CorrelatedEquality {
    ColumnRef own;
    ColumnRef other;
};
std::optional<CorrelatedEquality> CorrelatedEqualityOf(const StepPredicate& pred,
                                                       std::uint16_t slot) {
    if (pred.op != parser::CompareOp::kEq) return std::nullopt;
    if (pred.rhs.kind != OperandKind::kColumn) return std::nullopt;
    CorrelatedEquality out;
    if (IsOwnColumn(pred.lhs, slot)) {
        out.own = pred.lhs;
        out.other = pred.rhs.column;
    } else if (IsOwnColumn(pred.rhs.column, slot)) {
        out.own = pred.rhs.column;
        out.other = pred.lhs;
    } else {
        return std::nullopt;
    }
    if (out.other.up == 0 && AvailableAt(out.other) >= slot) return std::nullopt;
    return out;
}

// The descriptor guard every deferred-value form shares (equality
// propagation states the argument): the value crossing was produced under
// one column's descriptor, and only an identical (type_val, len) makes it
// byte-valid under the other's.
bool SameDescriptor(const catalog::SysColumnRow& a, const catalog::SysColumnRow& b) {
    return a.type_val == b.type_val && a.len == b.len;
}

// The correlated index probe (docs/spec/index.md §8a), or nullopt: an index
// whose **leading** key column is bound by equality to a column of an
// *earlier* step or an enclosing chain - a join key - so the descent is
// keyed per outer row instead of the relation being walked per outer row.
// This is what an index-served inner join side is; before it, a join on an
// indexed non-pk column read the whole inner relation once per outer row
// (bench/results-scenario3-library.md §9a.6 named the gap).
//
// Reached only after `IndexProbeOf` declined, so a literal equality that
// can be encoded at compile time always wins over a deferred one - it
// serves the same descent without the per-row encode. Selection is the
// first index in oid order whose leading column has such an equality,
// taking the first qualifying conjunct in residual order: no score, since
// `eq_prefix` is always exactly 1 here (a deeper prefix would mix deferred
// and literal encoding; out of scope by decision, not omission).
//
// Two guards, each a correctness statement:
//  - **Identical (type_val, len) descriptors** between the outer column and
//    the index key column, for equality propagation's reason read in the
//    other direction: the executor encodes the outer row's decoded value
//    into this column's key format, and only an identical descriptor makes
//    that encoding the one the index was built from.
//  - **The other side must be available before this step runs** (an earlier
//    slot, or `up > 0` - an enclosing chain's row is bound before this
//    chain opens). A reference to this step or a later one has no value
//    when the descent happens; same rule as the pk-probe arm.
std::optional<IndexProbe> CorrelatedIndexProbeOf(const Scope& scope,
                                                 const catalog::TableAccess& access,
                                                 const Step& step, std::uint16_t slot) {
    if (access.indexes.empty()) return std::nullopt;

    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        if (ix.nkeys == 0) continue;
        const std::uint16_t k0 = ix.keys()[0];

        // Index-level facts, settled once per index rather than once per
        // conjunct: if the leading column's key width cannot fit, no
        // conjunct can enter this index.
        const catalog::SysColumnRow& own_col = access.schema.columns[k0];
        auto width = IndexKeyColumnWidth(own_col);
        if (!width.ok() || width.value() > ix.key_width) continue;

        for (const StepPredicate& pred : step.residual) {
            auto eq = CorrelatedEqualityOf(pred, slot);
            if (!eq.has_value() || eq->own.col_pos != k0) continue;
            if (!SameDescriptor(own_col, ColumnAt(scope, eq->other))) continue;

            // The templates stay as pure padding - the leading width is
            // encoded per outer row by the executor, from `key_from`.
            IndexProbe probe = ProbeOverIndex(ix);
            probe.eq_prefix = 1;
            probe.key_from = eq->other;
            return probe;
        }
    }
    return std::nullopt;
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
        probe.managed = cabin.origin == catalog::kCabinOriginAuto;
        return probe;
    }
    return std::nullopt;
}

// The correlated cabin probe (docs/spec/cabin.md §4a), or nullopt: a
// cabined column bound by equality to an earlier step's or an enclosing
// chain's column - the join shape, IX17's selection one trust class over.
// Reached only after the literal form declined; first qualifying conjunct
// in residual order; both ON orientations, the pk excluded on either side
// (the pk arm already served it better). The identical-descriptor guard is
// what makes the frame value the form the set was keyed on: the write hook
// observes values coerced to the cabin column's type, so only a column of
// the same (type_val, len) produces byte-identical keys.
//
// This is the one *banked* structure a **heap** relation's join column can
// carry at all - IX3 refuses it an index - which is the shape this arm
// exists for. Since JB1 the same shape without a Cabin takes the build
// annotation one arm later (a per-statement map, not a banked structure);
// ladder order is the economics, join-inner-build.md §5: a converged
// serve beats any per-statement rebuild.
std::optional<CabinProbe> CorrelatedCabinProbeOf(const Scope& scope,
                                                 const catalog::TableAccess& access,
                                                 const Step& step, std::uint16_t slot) {
    if (access.cabin_mask == 0) return std::nullopt;
    for (const StepPredicate& pred : step.residual) {
        auto eq = CorrelatedEqualityOf(pred, slot);
        // The pk exclusion is this arm's own: an index's leading column can
        // never be the pk (CREATE INDEX refuses it), but a join written on
        // the pk reaches here and is already served better by the pk arm.
        if (!eq.has_value() || IsPrimaryKey(eq->own)) continue;

        const catalog::TableAccess::CabinRef cabin = access.CabinOn(eq->own.col_pos);
        if (cabin.id == 0) continue;
        if (!SameDescriptor(access.schema.columns[eq->own.col_pos], ColumnAt(scope, eq->other))) {
            continue;
        }

        CabinProbe probe;
        probe.cabin_id = cabin.id;
        probe.col_pos = eq->own.col_pos;
        probe.key_from = eq->other;
        probe.declared = cabin.origin == catalog::kCabinOriginUser;
        probe.managed = cabin.origin == catalog::kCabinOriginAuto;
        return probe;
    }
    return std::nullopt;
}

// The walked-join annotation (docs/spec/join-inner-build.md §5, workplan
// JB1), or nullopt. Still `f(shape, catalog)` - the residual and two schema
// descriptors are all it reads - and declining is never a wrong answer,
// only a forgone build. Two declines are this loop's own, spec §8's by
// name:
//  - **Multi-column join keys** (CB12's scope rule): a second correlated
//    equality declines the arm outright rather than keying a map on one
//    column of a key the statement wrote as two.
//  - The pk skip is defensive: a pk correlated equality became kProbe in
//    the kind pass, so this arm never sees one unless the ladder is ever
//    reordered.
// The kFilterScan, CompileWhere and scalar-sub-chain declines are the call
// site's - they gate whether this arm runs at all.
std::optional<BuildKey> BuildKeyOf(const Scope& scope, const catalog::TableAccess& access,
                                   const Step& step, std::uint16_t slot) {
    std::optional<BuildKey> out;
    bool correlated_seen = false;
    for (std::size_t i = 0; i < step.residual.size(); ++i) {
        auto eq = CorrelatedEqualityOf(step.residual[i], slot);
        if (!eq.has_value() || IsPrimaryKey(eq->own)) continue;
        if (correlated_seen) return std::nullopt;  // multi-column join key
        correlated_seen = true;
        // Nothing bounds a statement's conjunct count - the parser's WHERE
        // loop appends while `AND` follows and stops at no cap - so a
        // position past the field's width is expressible. A truncated
        // `residual_pos` would name a *different* conjunct, and JB3 would
        // then partition the residual around the wrong one: the build
        // would bucket only the first outer row's matches and every later
        // probe would miss rows the walk returns. Declining is always
        // legal, so the arm declines rather than narrowing.
        constexpr std::size_t kMaxResidualPos = 0xFFFF;  // BuildKey::residual_pos is uint16_t
        if (i > kMaxResidualPos) return std::nullopt;
        if (!SameDescriptor(access.schema.columns[eq->own.col_pos], ColumnAt(scope, eq->other))) {
            continue;  // mixed-descriptor: this conjunct cannot key a map
        }
        BuildKey key;
        key.col_pos = eq->own.col_pos;
        key.key_from = eq->other;
        key.residual_pos = static_cast<std::uint16_t>(i);
        out = key;
    }
    return out;
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
        case AccessKind::kIndexProbe:
        case AccessKind::kIndexRange:
            // The key columns the index was entered by, and only those. The
            // rest of the index's key is not what the statement addressed
            // it on, and the residual's other columns are residual whatever
            // the kind - reporting them would merge this shape with the
            // filter scan below and lose the distinction the statistics
            // exist to draw.
            if (step.index.has_value()) {
                const std::size_t pinned = static_cast<std::size_t>(step.index->eq_prefix) +
                                           (step.index->ranged ? 1 : 0);
                for (std::size_t i = 0; i < pinned && i < step.index->key_cols.size(); ++i) {
                    out.push_back(step.index->key_cols[i]);
                }
            }
            break;
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
// Whether any sub-chain exists anywhere in `chain`.
//
// A correlated sub-chain reads outward through the frame stack, and from
// out here those references are invisible - they live inside the sub-chain's
// own steps with `up > 0`. Rather than map them back, a chain containing one
// gives every step `kAllColumns` and keeps every answer. Conservative, and
// the shape AP01 is aimed at (a fold over a walk) carries no sub-chain.
bool HasAnySubChain(const StepChain& chain) {
    if (!chain.hoisted.empty()) return true;
    for (const Step& step : chain.steps) {
        if (!step.sub_chains.empty()) return true;
    }
    return false;
}

// Every column of step `index`'s relation that anything reads (AP01).
//
// Walks the **whole chain**, not this step's residual: a join predicate
// attached to a later step reads an earlier step's columns out of the frame,
// and so does a probe key. Missing one leaves a slot holding the previous
// row's value, which is a wrong answer rather than a crash - so when in
// doubt this answers kAllColumns.
std::uint64_t ReadColumnsOf(const StepChain& chain, const Step& step, std::uint16_t index) {
    std::uint64_t mask = 0;
    bool all = false;
    auto note = [&](const ColumnRef& ref) {
        if (all || ref.up != 0 || ref.rel_slot != index) return;
        if (ref.col_pos >= 64) {
            all = true;
            return;
        }
        mask |= std::uint64_t{1} << ref.col_pos;
    };

    for (const Step& other : chain.steps) {
        for (const StepPredicate& pred : other.residual) {
            note(pred.lhs);
            if (pred.rhs.kind == OperandKind::kColumn) note(pred.rhs.column);
        }
        if (other.key.has_value() && other.key->kind == OperandKind::kColumn) {
            note(other.key->column);
        }
    }

    // The sink. `SELECT *` emits every column of the step it projects, and
    // an aggregated chain reads its items and its grouping keys.
    if (chain.star()) {
        if (index == 0) all = true;
    } else {
        for (const ColumnRef& ref : chain.projection) note(ref);
    }
    if (chain.aggregate.has_value()) {
        for (const AggregateItem& item : chain.aggregate->items) {
            if (!item.star_arg) note(item.ref);
        }
        for (const ColumnRef& key : chain.aggregate->group_keys) note(key);
    }
    // A sort key has to be decoded to be compared, and it need not be
    // projected: `SELECT a FROM t ORDER BY b` orders by a column the client
    // never sees. `note` files each key against its own step, so a join
    // ordered by the inner relation's column decodes it there and not on
    // the driving one.
    for (const SortKey& key : chain.sort_keys) note(key.ref);

    // The trail records the Keystone pk of every row a replayable step
    // accepts, and reads it from the frame rather than looking it up. The
    // *kind* is a compile-time fact, so this costs a column only on the
    // steps that can record one.
    if (IsTrailReplayable(step.kind)) {
        mask |= 1;  // column 0 is the pk (invariant 11)
    }

    // A Cabin's miss walk records the key column and the pk. At execute
    // time the walk decodes `filter_columns` per row, plus the pk for a
    // row whose key matches (step_vm.cpp - the full-decode form it
    // replaced was scenario3 §7's cost inversion), and naming both here
    // keeps the survivors' remaining-columns decode honest about what was
    // already read.
    if (step.cabin.has_value() && step.cabin->col_pos < 64) {
        mask |= std::uint64_t{1} << step.cabin->col_pos;
        mask |= 1;
    }

    return all ? Step::kAllColumns : mask;
}

// `inner_build`: whether steps of this block may take the walked-join
// annotation (workplan JB1). True from `Compile` down; false from
// `CompileWhere` down (v1 is SELECT-only - a DML statement's own writes
// between outer rows are exactly what would invalidate a map, spec §4) and
// from a scalar sub-chain down (conclusiveness needs `Exists` semantics,
// spec §6). Once off it stays off for everything nested, which is the
// conservative reading and costs only a forgone build.
StatusOr<StepChain> CompileBlock(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                                 const Scope* parent, std::uint32_t& next_step_id,
                                 std::uint32_t depth, const txn::ReadView* view,
                                 bool inner_build);

}  // namespace

StatusOr<StepChain> Compile(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                            const txn::ReadView* view) {
    std::uint32_t next_step_id = 0;
    return CompileBlock(catalog, stmt, /*parent=*/nullptr, next_step_id, /*depth=*/0,
                        view, /*inner_build=*/true);
}

Status CompileAssignments(const catalog::TableAccess& access,
                          const std::vector<parser::Assignment>& assignments) {
    // An empty schema cannot happen for a relation the catalog resolved
    // (invariant 11 makes column 0 mandatory), but the pk name is read
    // from `front()` and a check costs nothing against a wrong answer.
    if (access.schema.columns.empty()) {
        return Status::Corruption("relation oid " + std::to_string(access.oid) +
                                  " has no columns");
    }
    const std::string_view pk_name = catalog::NameView(access.schema.columns.front().name);

    for (const parser::Assignment& a : assignments) {
        if (access.schema.FindColumn(a.col_name) == nullptr) {
            return Status::InvalidArgument("unknown column '" + a.col_name + "' at byte " +
                                           std::to_string(a.byte_offset));
        }
        // K2, and the reason it is `Unsupported` rather than
        // `InvalidArgument`: the statement is understood and declined. The
        // column exists and the value would encode; what cannot happen is
        // the write, because the id names the tuple everywhere else in the
        // engine.
        if (a.col_name == pk_name) {
            return Status::Unsupported("primary-key column '" + std::string(pk_name) +
                                       "' cannot be updated at byte " +
                                       std::to_string(a.byte_offset) +
                                       "; it is the tuple's identity, not a field of it");
        }
    }
    return Status::OK();
}

StatusOr<Step> CompileWhere(catalog::Catalog& catalog, const catalog::TableAccess& access,
                            std::string_view binding,
                            const std::vector<parser::Condition>& where,
                            const txn::ReadView* view) {
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

            // `inner_build=false`, v1's SELECT-only rule (workplan JB1):
            // this sub-chain runs per row of a statement that *writes*, and
            // its own writes between outer rows are what would invalidate a
            // map the first row built (join-inner-build.md §4).
            auto inner = CompileBlock(catalog, *cond.subquery, &scope, next_step_id, /*depth=*/1,
                                      view, /*inner_build=*/false);
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
            if (Status s = CoercePredicate(scope, low, cond.col.byte_offset); !s.ok()) return s;
            out.residual.push_back(low);

            StepPredicate high;
            high.lhs = lhs.value();
            high.op = parser::CompareOp::kLte;
            high.rhs.kind = OperandKind::kLiteral;
            high.rhs.literal = cond.val_high;
            if (Status s = CoercePredicate(scope, high, cond.col.byte_offset); !s.ok()) return s;
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
        if (Status s = CoercePredicate(scope, pred, cond.col.byte_offset); !s.ok()) return s;
        out.residual.push_back(pred);
    }
    // **kAllColumns, deliberately.** UPDATE and DELETE walk the relation
    // themselves rather than through the step VM, and they need every column
    // of a matching row anyway - one to re-encode, one to hand the write
    // hook. A mask here would be a promise the caller does not keep.
    out.filter_columns = Step::kAllColumns;
    return out;
}

namespace {

// The columns of step `index`'s own relation that its residual reads.
//
// Only `up == 0 && rel_slot == index` references count: a predicate reaching
// into an earlier step reads a value that step already put in the frame, and
// an outward reference belongs to an enclosing chain. Both are present when
// this row is filtered and neither costs this row a decode.
//
// A column past bit 63 answers kAllColumns - a relation that wide loses the
// optimization and keeps every answer, which is the right way round.
std::uint64_t FilterColumnsOf(const Step& step, std::uint16_t index) {
    std::uint64_t mask = 0;
    auto note = [&](const ColumnRef& ref) {
        if (ref.up != 0 || ref.rel_slot != index) return;
        if (ref.col_pos >= 64) {
            mask = Step::kAllColumns;
            return;
        }
        if (mask != Step::kAllColumns) mask |= std::uint64_t{1} << ref.col_pos;
    };
    for (const StepPredicate& pred : step.residual) {
        note(pred.lhs);
        if (pred.rhs.kind == OperandKind::kColumn) note(pred.rhs.column);
    }
    return mask;
}

StatusOr<StepChain> CompileBlock(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                                 const Scope* parent, std::uint32_t& next_step_id,
                                 std::uint32_t depth, const txn::ReadView* view,
                                 bool inner_build) {
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
        auto oid = catalog.FindTableOidByName(rel->table_name, view);
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
        // A join on two decimal columns is subject to the same scale rule
        // as any other column-column comparison - joining `decimal(10,2)`
        // to `decimal(10,3)` on unscaled integers would match rows that
        // are not equal. Positioned at the ON clause's left column, which
        // is where a reader looks for the join it wrote.
        if (Status s = CoercePredicate(scope, pred, join.left.byte_offset); !s.ok()) return s;
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

            // A scalar sub-chain's steps never take the build annotation
            // (workplan JB1): its stopping walk has no `Exists` semantics
            // for a prefix map to be conclusive under (spec §6). Off stays
            // off for everything nested.
            const bool sub_inner_build =
                inner_build && cond.kind != parser::PredicateKind::kCompareSubquery;

            // Compiled against a scope whose parent is *this* one, which
            // is what turns an inner reference to an outer column into
            // `up == 1` rather than a resolution failure.
            auto inner =
                CompileBlock(catalog, *cond.subquery, &scope, next_step_id, depth + 1, view,
                             sub_inner_build);
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
            if (Status s = CoercePredicate(scope, low, cond.col.byte_offset); !s.ok()) return s;
            predicates.push_back(low);

            StepPredicate high;
            high.lhs = lhs.value();
            high.op = parser::CompareOp::kLte;
            high.rhs.kind = OperandKind::kLiteral;
            high.rhs.literal = cond.val_high;
            if (Status s = CoercePredicate(scope, high, cond.col.byte_offset); !s.ok()) return s;
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
        if (Status s = CoercePredicate(scope, pred, cond.col.byte_offset); !s.ok()) return s;
        predicates.push_back(pred);
    }

    // ---- 2a. Close the conjunct list under join-key equality -------------
    //
    // After lowering and before attachment, because the pass reads the
    // whole flat list - ON and WHERE conjuncts alike - and what it appends
    // must go through the same placement as everything written.
    PropagateEqualities(scope, predicates);

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

    // ---- 4. What each step must decode before it can filter -------------
    //
    // Last, because it reads the residual and the residual is only final
    // once every conjunct has been placed. A step carrying a **sub-chain**
    // answers kAllColumns: a sub-chain's correlation can reach any column of
    // this row, and the frame is where it reads them from.
    for (std::size_t i = 0; i < chain.steps.size(); ++i) {
        Step& step = chain.steps[i];
        // **A relation wider than 64 columns gets no mask at all.** A
        // `std::uint64_t` cannot name column 64, and `DecodeColumnsInto`
        // stops at that bound - its comment says "the caller decodes fully",
        // which is true of every caller *except* a partial decode, where the
        // tail would silently keep the previous row's values. Answering
        // kAllColumns here is what makes that comment true: the VM takes the
        // whole-row path and a wide relation is merely slow.
        const bool maskable = scope.relations[i].access->schema.columns.size() <= 64;
        step.filter_columns = (step.sub_chains.empty() && maskable)
                                  ? FilterColumnsOf(step, static_cast<std::uint16_t>(i))
                                  : Step::kAllColumns;
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
            } else if (auto ix = IndexProbeOf(*scope.relations[i].access, step,
                                               static_cast<std::uint16_t>(i));
                       ix.has_value()) {
                // Ahead of the Cabin, per spec §9: an index is complete for
                // every key value where a Cabin is authoritative only for
                // the observed ones, so a cabined column that also carries
                // an index is served by the index and the Cabin becomes
                // dead weight the operator may drop.
                step.kind = ix->ranged ? AccessKind::kIndexRange : AccessKind::kIndexProbe;
                step.index = std::move(ix);
            } else if (auto cx = CorrelatedIndexProbeOf(scope, *scope.relations[i].access, step,
                                                         static_cast<std::uint16_t>(i));
                       cx.has_value()) {
                // The correlated form, after the literal one for the reason
                // its comment gives, and ahead of the Cabin per spec §9's
                // ordering: an index is complete for every key value.
                // `IndexProbe::key_from` is the single authority - the plan
                // printer renders it, and `Step::key` keeps its two-kind
                // contract untouched.
                step.kind = AccessKind::kIndexProbe;
                step.index = std::move(cx);
            } else if (auto probe = CabinProbeOf(*scope.relations[i].access, step,
                                                  static_cast<std::uint16_t>(i));
                       probe.has_value()) {
                // Ahead of kFilterScan, and only ahead of it: a cabined
                // column's equality is exactly the shape a filter scan
                // names, and the Cabin is the reason it need not be one.
                step.kind = AccessKind::kCabinProbe;
                step.cabin = std::move(probe);
            } else if (auto cp = CorrelatedCabinProbeOf(scope, *scope.relations[i].access, step,
                                                         static_cast<std::uint16_t>(i));
                       cp.has_value()) {
                // The correlated form, last of the *banked*-structure arms:
                // after both index forms (an index is complete for every
                // key value where a Cabin is authoritative only for
                // observed ones) and after the literal cabin (a
                // compile-time key needs no per-row read). What it replaces
                // is the walked inner side of a join on a cabined column;
                // ahead of the build annotation below because a converged
                // serve beats any per-statement rebuild (spec §5).
                step.kind = AccessKind::kCabinProbe;
                step.cabin = std::move(cp);
            } else if (HasUnindexedEqualityFilter(*scope.relations[i].access, step,
                                                   static_cast<std::uint16_t>(i))) {
                // Ahead of the build annotation by decision, not
                // impossibility (spec §8): the step's own literal already
                // bounds what a walk visits, and v1 declines to also build
                // for it.
                step.kind = AccessKind::kFilterScan;
            } else if (inner_build) {
                // The last ladder arm (workplan JB1): every arm above
                // declined, so a correlated equality still on the step is
                // the walked join. An annotation, never a kind - `kind`
                // stays kScan, and `BuildKey` (step_chain.hpp) owns the
                // contract.
                step.build = BuildKeyOf(scope, *scope.relations[i].access, step,
                                        static_cast<std::uint16_t>(i));
            }
        }

        // The columns the kind was assigned for. Recorded here because the
        // compiler is the only place that already knows them; the statistics
        // layer would otherwise re-walk the residual per statement to answer
        // a question that was settled at compile time.
        step.access_columns = AccessColumnsOf(step, static_cast<std::uint16_t>(i));
    }

    // ---- 5. Projection, or the fold --------------------------------------
    //
    // An aggregated statement names its output through `AggregateSpec` and
    // leaves `projection` empty. Note what is *not* here: nothing above this
    // point read `stmt.agg_items` or `stmt.group_by`, so the steps, kinds,
    // residuals and access columns of an aggregated statement are the ones
    // its unaggregated twin compiles to, bit for bit (AG1). The chain
    // identity test is what keeps that from drifting.
    // ---- The post-fold consumers, until HV-2 and HV-4 --------------------
    //
    // HV-1 parses `HAVING` and an aggregated `ORDER BY`; the fold's filter
    // and the fold's sort are the next two tasks (docs/inflight/in-progress/workplan-having.md).
    // Refused here, positioned, and **never dropped**: a clause parsed,
    // accepted and silently ignored is the failure parser-v2.md I11 records
    // having already made once on a catalog view, and it is worse than the
    // refusal it replaces because the client is answered rather than told.
    //
    // Outside the `aggregated()` arm below, deliberately. A HAVING makes a
    // statement aggregated at parse, so the two conditions coincide today -
    // and a refusal that can only be reached through a second fact is one
    // that stops holding the day that fact changes.
    if (!stmt.having.empty()) {
        return Status::Unsupported(
            "HAVING is not supported yet (byte " +
            std::to_string(stmt.having.front().agg.byte_offset) +
            "); filter before the fold with WHERE, or filter the result client-side");
    }
    if (stmt.aggregated() && !stmt.order_by.empty()) {
        return Status::Unsupported(
            "ORDER BY is not supported yet over an aggregated statement (byte " +
            std::to_string(stmt.order_by.front().key.byte_offset) +
            "); an aggregated statement's output rows are not chain rows");
    }

    if (stmt.aggregated()) {
        auto spec = CompileAggregate(scope, stmt);
        if (!spec.ok()) return spec.status();
        chain.aggregate = std::move(spec.value());
        for (const parser::SelectItem& item : stmt.agg_items) {
            chain.column_names.push_back(AggregateLabel(item));
        }
    }
    for (const parser::ColumnName& col : stmt.projection) {
        auto ref = ResolveColumn(scope, col);
        if (!ref.ok()) return ref.status();
        chain.projection.push_back(ref.value());
        chain.column_names.push_back(col.qualified() ? col.qualifier + "." + col.name : col.name);
        // Resolved here so the emission boundary never asks the catalog
        // per row (TY06). Same list, same order, same lifetime as the
        // names beside it.
        chain.projection_types.push_back(ColumnAt(scope, ref.value()).type_val);
    }
    if (stmt.star()) {
        // `SELECT *`, which the grammar admits only for a single relation
        // (V06) - so it means every column of the one step, in schema
        // order. Left as an empty projection with names filled in, since
        // the executor emits the whole decoded row in that case.
        //
        // `star()`, not `projection.empty()`: an aggregated statement leaves
        // the projection empty too, and labelling its output with the
        // relation's columns would name a row it never emits.
        for (const auto& column : scope.relations[0].access->schema.columns) {
            chain.column_names.push_back(std::string(catalog::NameView(column.name)));
        }
    }

    // ---- 5a. The pagination tail (spec I11, V09; sort per OB3) -----------
    //
    // Every written key is resolved. Any relation in the top-level scope
    // may be named: by the time the sink sees a row the frame holds every
    // step's values, so a joined relation's column costs a sort exactly
    // what the driving relation's does. `up != 0` is unreachable - the
    // parser refuses the clause at depth > 0 - and is refused here anyway
    // so the rule is a property of this function rather than of a
    // guarantee two files away.
    for (const parser::SortKey& written : stmt.order_by) {
        // Every key here is a plain column reference: an aggregate one is
        // parseable only over a fold, and the fold's whole tail is refused
        // above until HV-4.
        auto ref = ResolveColumn(scope, written.key.column);
        if (!ref.ok()) return ref.status();
        if (ref.value().up != 0) {
            return Status::Unsupported("ORDER BY cannot name an outer query's column" +
                                       Position(written.key.column.byte_offset));
        }
        chain.sort_keys.push_back(SortKey{ref.value(), ColumnAt(scope, ref.value()).type_val,
                                          written.descending});
    }

    // ---- The elision: an order the chain already emits needs no sort -----
    //
    // One key, ascending, on the driving relation's pk is exactly V09's
    // accepted form, and V09's argument for it still holds: it names the
    // order the chain already emits (I12 - written order across steps, pk
    // order within one), so there is nothing to do. Dropping the keys here
    // rather than sorting-and-noticing-it-was-sorted is what keeps that
    // path at literally zero cost instead of merely at low cost.
    //
    // The premise is that step 0 emits in pk order. A walk or range does
    // (page-wise `min_key`, which a division preserves); an index step does
    // because IX8a sorts its pks back into that order deliberately; a
    // lookup or probe emits one row. A Cabin probe stays excluded by name:
    // since 2026-08-19 a served set *is* sorted to the walk's order, but
    // that order is pk only while a relation's keys have ascended - once one
    // has landed below the mark it is page-and-slot (heap-and-tuple.md §4.1)
    // - so the elision's premise holds for one key order and not the other,
    // and an exclusion that depended on `key_order` would be a second copy
    // of that rule. The
    // exclusion is a fix, not a precaution: the discarding version of this
    // clause answered `ORDER BY <pk>` over a Cabin-probed relation with
    // whatever order the entry set happened to hold.
    const bool one_ascending_pk =
        chain.sort_keys.size() == 1 && !chain.sort_keys[0].descending &&
        chain.sort_keys[0].ref.rel_slot == 0 && IsPrimaryKey(chain.sort_keys[0].ref);
    const bool driving_emits_pk_order =
        !chain.steps.empty() && chain.steps[0].kind != AccessKind::kCabinProbe;
    if (one_ascending_pk && driving_emits_pk_order) {
        chain.sort_keys.clear();

        // ...and on a relation that has taken an out-of-order key, "the order
        // the chain already emits" is not quite true (docs/spec/heap-and-tuple.md
        // §4.1): a page's slots are in insertion order, which equals key
        // order only while every id was appended above every id already
        // there. An id admitted below the relation's high-water mark was not.
        //
        // Read off `key_order` rather than off the storage type, which is
        // what makes this cost nothing on the relations that never took one -
        // a btree relation fed only ascending keys is exactly as free here as
        // an ASSIGNED relation used to be (well_known.hpp's KeyOrder). A heap
        // relation can never be kUnordered.
        //
        // The divergence is **within a page only** - pages stay key-ordered
        // by `min_key`, which a leaf division preserves - so the fix is a
        // per-page emission order, not an output sort. Asked for here rather
        // than always, because reading every live slot's Keystone word up
        // front is a real cost on a walk that otherwise reads one per row.
        if (!scope.relations.empty() && scope.relations[0].access != nullptr &&
            scope.relations[0].access->key_order == catalog::KeyOrder::kUnordered) {
            chain.steps[0].emit_in_key_order = true;
        }
    }
    chain.limit = stmt.limit;
    chain.offset = stmt.offset;

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

    // ---- 7. What each step must decode *after* it has filtered (AP01) ---
    //
    // Last of all, because it reads the projection, the fold and the sort
    // keys, which sections 5 and 5a only just resolved. Everything above
    // this line is unchanged
    // by it - the steps, kinds, residuals and class an aggregated statement
    // compiles to are still its unaggregated twin's, which is AG1 and which
    // the contract suite pins.
    const bool sub_chains_anywhere = HasAnySubChain(chain);
    for (std::size_t i = 0; i < chain.steps.size(); ++i) {
        Step& step = chain.steps[i];
        const bool maskable = scope.relations[i].access->schema.columns.size() <= 64;
        step.read_columns =
            (sub_chains_anywhere || !maskable)
                ? Step::kAllColumns
                : ReadColumnsOf(chain, step, static_cast<std::uint16_t>(i));
    }

    return chain;
}

}  // namespace

}  // namespace kds::exec
