#include "kds/exec/step_compiler.hpp"
#include "kds/base/function_ref.hpp"
#include "kds/exec/set_expr.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// For `CoerceLiteralToColumn`: the codec owns literal coercion, so the
// value a predicate compares and the value a write keys on are identical by
// construction.
#include "kds/storage/keystone.hpp"  // kIdSpaceEnd - a bind's whole-relation declaration
#include "kds/exec/cabin_expr.hpp"
#include "kds/exec/index_key.hpp"
#include "kds/exec/join_order.hpp"
#include "kds/exec/plan_memo.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_vm.hpp"  // PositionSink - the declaration sink AT-R1 threads

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

    const catalog::Schema& schema() const noexcept { return access->schema; }
};

// The scope a name resolves against. One per query block; sub-chains
// (V15) push another and resolve outward through `parent`.
struct Scope {
    std::vector<BoundRelation> relations;
    const Scope* parent = nullptr;
    // One name space with the parent (`SelectStmt::shares_parent_names`).
    bool shares_parent_names = false;

    // The statement's constants (exec/functions.hpp), set on the root scope.
    // A SELECT is compiled once per execution, so taking them at compile is
    // taking them once per statement; a parked write hands its own back
    // (CompileWhere). Not part of the chain's identity: it lands in a
    // function conjunct's context, which no step, kind, residual or class
    // reads.
    StatementContext context;
};

const StatementContext& ContextOf(const Scope& scope) {
    const Scope* s = &scope;
    while (s->parent != nullptr) s = s->parent;
    return s->context;
}


std::string Position(std::uint32_t byte_offset) {
    return " at byte " + std::to_string(byte_offset);
}

// Finds `name` among a relation's live columns, ignoring case
// (`catalog::Schema::FindColumn`). Sets `out` to the schema position,
// which is exactly what ColumnRef::col_pos is - BuildSchemaFromColumns()
// returns columns sorted by `pos`, so the vector index is the position -
// and answers whether it matched. Two or more matches is refused rather
// than resolved to the first: column names are unique ignoring case since
// CREATE TABLE, ADD COLUMN and RENAME COLUMN refuse a twin, but a relation
// created before that can still hold `id` beside `ID`, and taking the
// first would answer `SELECT ID` with `id`'s values.
StatusOr<bool> FindColumnPos(const catalog::Schema& schema, const parser::ColumnName& name,
                             std::uint16_t& out) {
    auto found = schema.FindColumn(name.name, name.byte_offset);
    if (!found.ok()) return found.status();
    if (found.value() == nullptr) return false;
    out = static_cast<std::uint16_t>(found.value()->pos);  // the vector index (BW-R6)
    return true;
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
    return s->relations[ref.rel_slot].schema().columns[ref.col_pos];
}

// Whether two columns' values compare at all (types.md §3.1a). Called for a
// column-column conjunct and for an outer column against the value a
// subquery projects, because a pair this refuses would reach CompareValues
// as a kind mismatch and answer false per row - a statement that can only
// ever answer no rows is a statement to refuse with a reason, not to run.
//
// **No conversion is made.** Two columns compare only inside one family
// (`TypeFamilyOf`): every integer width and signedness, by value; a decimal
// of the same width and scale; char and varchar, as the text they hold; a
// date with a date and a timestamp with a timestamp.
Status CheckColumnsCompare(std::uint32_t lhs_type, std::uint32_t lhs_len, std::string_view lhs_name,
                           std::uint32_t rhs_type, std::uint32_t rhs_len, std::string_view rhs_name,
                           std::uint32_t byte_offset) {
    const TypeFamily family = TypeFamilyOf(lhs_type);
    if (family != TypeFamilyOf(rhs_type) || family == TypeFamily::kOther) {
        const auto name = [](std::uint32_t type_val, std::uint32_t len) {
            SetType t;
            t.kind = SetType::Kind::kConcrete;
            t.type_val = type_val;
            t.len = len;
            // A subquery's side carries a type mod, which is 0 for char.
            return type_val == catalog::kTypeValChar && len == 0 ? std::string("char")
                                                                 : SetTypeName(t);
        };
        return Status::InvalidArgument("cannot compare '" + std::string(lhs_name) + "' (" +
                                       name(lhs_type, lhs_len) + ") with '" +
                                       std::string(rhs_name) + "' (" + name(rhs_type, rhs_len) +
                                       "); this engine converts no type implicitly" +
                                       Position(byte_offset));
    }
    if (family != TypeFamily::kDec) return Status::OK();
    // Two DECIMALs of different scale compare unscaled integers that mean
    // different things, so `1.50` would equal `1.500`'s stored 1500 only by
    // accident of digits. Refused rather than rescaled, because rescaling
    // either drops digits or invents them - TY6 defers that decision whole.
    // Different *widths* are refused first: an 8-byte and a 16-byte decimal
    // can never share a (p, s) - the width is a function of p.
    if (lhs_type != rhs_type) {
        return Status::Unsupported(
            "cannot compare decimal columns of different width: '" + std::string(lhs_name) +
            "' and '" + std::string(rhs_name) +
            "' are on opposite sides of the 18-digit precision split; this engine does "
            "not rescale" + Position(byte_offset));
    }
    if (catalog::DecimalScaleOf(lhs_len) != catalog::DecimalScaleOf(rhs_len)) {
        return Status::Unsupported(
            "cannot compare decimal columns of different scale: '" + std::string(lhs_name) +
            "' has scale " + std::to_string(catalog::DecimalScaleOf(lhs_len)) + " and '" +
            std::string(rhs_name) + "' has scale " +
            std::to_string(catalog::DecimalScaleOf(rhs_len)) + "; this engine does not rescale" +
            Position(byte_offset));
    }
    return Status::OK();
}

// An outer column against the one value a subquery projects (`IN`, `NOT IN`,
// the scalar form): the same pair rule, with the inner side's type read off
// the compiled sub-chain, whose schemas the outer scope does not hold.
Status CheckSubqueryValue(const Scope& scope, const ColumnRef& lhs, const StepChain& inner,
                          std::uint32_t byte_offset) {
    const catalog::SysColumnRow& outer = ColumnAt(scope, lhs);
    return CheckColumnsCompare(outer.type_val, outer.len, catalog::NameView(outer.name),
                               inner.projection_types[0], inner.projection_type_mods[0],
                               inner.column_names[0], byte_offset);
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
        // produced by one routine (two routines once let a Cabin's write
        // hook and read path key one date differently).
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

    const catalog::SysColumnRow& rhs = ColumnAt(scope, pred.rhs.column);
    return CheckColumnsCompare(lhs.type_val, lhs.len, catalog::NameView(lhs.name), rhs.type_val,
                               rhs.len, catalog::NameView(rhs.name), byte_offset);
}

// ---- Function conjuncts (AP-S4) ----------------------------------------
//
// A `kCompareFunction` condition lowered to a FunctionPredicate - never to
// a StepPredicate, which is what keeps it from being a key (step_chain.hpp).
// Called from both lowering sites, for CoercePredicate's reason.

StatusOr<FunctionTerm> ResolveCall(const Scope& scope, const parser::FunctionCall& call) {
    const FunctionEntry* fn = FindFunction(call.name);
    if (fn == nullptr && IsScalarFunctionName(call.name)) {
        // A value function the native catalog does not hold is the query
        // layer's (BU-S4, BU-Q9): declined here so the layer is asked. The
        // message is the reply only where no layer answers - an UPDATE's or
        // a DELETE's WHERE, or a SELECT the query grammar refuses too.
        return Status::NotImplemented("function '" + call.name +
                                      "' is not supported in this WHERE comparison" +
                                      Position(call.byte_offset));
    }
    if (fn == nullptr) {
        // AP-Q3: InvalidArgument, as an unknown column is. The parser cannot
        // tell a function this engine lacks from a misspelling.
        return Status::InvalidArgument("unknown function '" + call.name + "'" +
                                       Position(call.byte_offset));
    }
    if (call.args.size() != fn->arity) {
        return Status::InvalidArgument(
            "function '" + std::string(fn->name) + "' takes " + std::to_string(fn->arity) +
            " argument(s), got " + std::to_string(call.args.size()) + Position(call.byte_offset));
    }
    FunctionTerm term;
    term.fn = fn;
    for (const parser::ColumnName& arg : call.args) {
        auto ref = ResolveColumn(scope, arg);
        if (!ref.ok()) return ref.status();
        if (ColumnAt(scope, ref.value()).type_val != fn->arg_type_val) {
            return Status::InvalidArgument("function '" + std::string(fn->name) +
                                           "' does not take column '" + arg.name +
                                           "': it is not of the argument's type" +
                                           Position(arg.byte_offset));
        }
        term.args.push_back(ref.value());
    }
    return term;
}

StatusOr<FunctionPredicate> LowerFunctionConjunct(const Scope& scope,
                                                  const parser::Condition& cond) {
    FunctionPredicate out;
    out.op = cond.op;
    out.context = ContextOf(scope);

    // The left side fixes the type both sides compare as. The parser makes a
    // condition a function comparison only when a call stands on a side, so
    // a plain column on the left has a call on the right.
    if (cond.lhs_fn.has_value()) {
        auto term = ResolveCall(scope, *cond.lhs_fn);
        if (!term.ok()) return term.status();
        out.type_val = term.value().fn->result_type_val;
        out.lhs.call = std::move(term.value());
    } else {
        if (!cond.rhs_fn.has_value()) {
            return Status::Corruption("a function comparison with no call on either side");
        }
        auto ref = ResolveColumn(scope, cond.col);
        if (!ref.ok()) return ref.status();
        out.type_val = ColumnAt(scope, ref.value()).type_val;
        out.lhs.plain.kind = OperandKind::kColumn;
        out.lhs.plain.column = ref.value();
    }
    if (cond.op == parser::CompareOp::kIsNull || cond.op == parser::CompareOp::kIsNotNull) {
        return out;
    }

    const auto mismatch = [](std::uint32_t offset) {
        return Status::InvalidArgument(
            "the two sides of this comparison are of different types" + Position(offset));
    };
    if (cond.rhs_fn.has_value()) {
        auto term = ResolveCall(scope, *cond.rhs_fn);
        if (!term.ok()) return term.status();
        if (term.value().fn->result_type_val != out.type_val) {
            return mismatch(cond.rhs_fn->byte_offset);
        }
        out.rhs.call = std::move(term.value());
        return out;
    }
    if (cond.rhs_kind == parser::RhsKind::kColumn) {
        auto ref = ResolveColumn(scope, cond.rhs_col);
        if (!ref.ok()) return ref.status();
        if (ColumnAt(scope, ref.value()).type_val != out.type_val) {
            return mismatch(cond.rhs_col.byte_offset);
        }
        out.rhs.plain.kind = OperandKind::kColumn;
        out.rhs.plain.column = ref.value();
        return out;
    }

    // A literal, so the call is on the left: coerced against the call's
    // result type as a column of that type would coerce it. **Only
    // `type_val` is set** - enough for DATE and TIMESTAMP, the two result
    // types that exist; a result type that reads `len` (char, decimal)
    // needs its width or scale here before an entry may return one.
    out.rhs.plain.kind = OperandKind::kLiteral;
    out.rhs.plain.literal = cond.val;
    catalog::SysColumnRow result_type{};
    result_type.type_val = out.type_val;
    if (Status s = CoerceLiteralToColumn(result_type, out.rhs.plain.literal); !s.ok()) {
        return s.WithContext("a function comparison's literal" +
                             Position(out.rhs.plain.literal.byte_offset));
    }
    return out;
}

// A plain comparison or a `BETWEEN`, lowered into `out`: one
// `StepPredicate`, or two for a `BETWEEN`. Shared by both WHERE lowerings
// and by a boolean conjunct's leaves, so a comparison means one thing
// wherever it is written.
//
// `BETWEEN` lowers to its two ordinary conjuncts and nothing else. The
// range that may later be put on the step is a hint *on top of* these
// (step_chain.hpp) - so a chain that ignored every range would still return
// the same rows, which is the property that makes the kind safe to add.
Status LowerComparison(const Scope& scope, const parser::Condition& cond,
                       std::vector<StepPredicate>& out) {
    auto lhs = ResolveColumn(scope, cond.col);
    if (!lhs.ok()) return lhs.status();

    if (cond.kind == parser::PredicateKind::kBetween) {
        for (const auto& [op, bound] :
             {std::pair{parser::CompareOp::kGte, &cond.val},
              std::pair{parser::CompareOp::kLte, &cond.val_high}}) {
            StepPredicate pred;
            pred.lhs = lhs.value();
            pred.op = op;
            pred.rhs.kind = OperandKind::kLiteral;
            pred.rhs.literal = *bound;
            if (Status s = CoercePredicate(scope, pred, cond.col.byte_offset); !s.ok()) return s;
            out.push_back(std::move(pred));
        }
        return Status::OK();
    }

    if (cond.op == parser::CompareOp::kLike) {
        const std::uint32_t type_val = ColumnAt(scope, lhs.value()).type_val;
        if (type_val != catalog::kTypeValVarchar && type_val != catalog::kTypeValChar) {
            return Status::InvalidArgument("LIKE takes a char or varchar column; '" +
                                           cond.col.name + "' is not one" +
                                           Position(cond.col.byte_offset));
        }
    }

    StepPredicate pred;
    pred.lhs = lhs.value();
    pred.op = cond.op;
    if (cond.rhs_kind == parser::RhsKind::kColumn) {
        // Resolved against the same scope, so an outward reference here is
        // exactly what makes an enclosing sub-chain correlated.
        auto rhs = ResolveColumn(scope, cond.rhs_col);
        if (!rhs.ok()) return rhs.status();
        pred.rhs.kind = OperandKind::kColumn;
        pred.rhs.column = rhs.value();
    } else {
        pred.rhs.kind = OperandKind::kLiteral;
        pred.rhs.literal = cond.val;
    }
    // A LIKE pattern is not a value of the column - `char(2) LIKE 'a%b'` is a
    // fair question - so it is not coerced to it; the parser took a string.
    if (cond.op != parser::CompareOp::kLike) {
        if (Status s = CoercePredicate(scope, pred, cond.col.byte_offset); !s.ok()) return s;
    }
    out.push_back(std::move(pred));
    return Status::OK();
}

// ---- Boolean conjuncts (BU-S1) -----------------------------------------
//
// A `kBoolTree` condition lowered to a BoolPredicate - never to the flat
// residual, which is what keeps every comparison under an `OR` or a `NOT`
// from being a key (step_chain.hpp, BU-R5). Each leaf goes through the
// lowering its flat counterpart takes; a subquery leaf (BU-S3) goes through
// `lower`, and its sub-chain is appended to `subs`, which the leaf indexes.
using SubqueryLowering = FunctionRef<StatusOr<SubChain>(const parser::Condition&)>;

StatusOr<BoolPredicate> LowerBoolTree(const Scope& scope, const parser::BoolExpr& node,
                                      SubqueryLowering lower, std::vector<SubChain>& subs) {
    BoolPredicate out;
    switch (node.op) {
        case parser::BoolExpr::Op::kAnd: out.op = BoolPredicate::Op::kAnd; break;
        case parser::BoolExpr::Op::kOr: out.op = BoolPredicate::Op::kOr; break;
        case parser::BoolExpr::Op::kNot: out.op = BoolPredicate::Op::kNot; break;
        case parser::BoolExpr::Op::kLeaf: {
            const parser::Condition& cond = node.leaf;
            if (cond.kind == parser::PredicateKind::kBoolTree) {
                // The parser flattens no tree into a leaf.
                return Status::Corruption("a boolean conjunct leaf that is a boolean conjunct");
            }
            if (cond.has_subquery()) {
                auto sub = lower(cond);
                if (!sub.ok()) return sub.status();
                sub.value().in_tree = true;
                out.op = BoolPredicate::Op::kSubChain;
                out.sub_chain = static_cast<std::uint32_t>(subs.size());
                out.reads_sub_chain = true;
                subs.push_back(std::move(sub.value()));
                return out;
            }
            if (cond.kind == parser::PredicateKind::kCompareFunction) {
                auto fn = LowerFunctionConjunct(scope, cond);
                if (!fn.ok()) return fn.status();
                out.op = BoolPredicate::Op::kFunction;
                out.fn = std::move(fn.value());
                return out;
            }
            std::vector<StepPredicate> preds;
            if (Status s = LowerComparison(scope, cond, preds); !s.ok()) return s;
            if (preds.size() == 1) {
                out.op = BoolPredicate::Op::kCompare;
                out.compare = std::move(preds.front());
                return out;
            }
            out.op = BoolPredicate::Op::kAnd;  // a BETWEEN's two halves
            for (StepPredicate& pred : preds) {
                BoolPredicate leaf;
                leaf.op = BoolPredicate::Op::kCompare;
                leaf.compare = std::move(pred);
                out.children.push_back(std::move(leaf));
            }
            return out;
        }
    }
    for (const parser::BoolExpr& child : node.children) {
        auto lowered = LowerBoolTree(scope, child, lower, subs);
        if (!lowered.ok()) return lowered.status();
        out.reads_sub_chain = out.reads_sub_chain || lowered.value().reads_sub_chain;
        out.children.push_back(std::move(lowered.value()));
    }
    return out;
}

// A lowered boolean conjunct with the sub-chains its leaves name, not yet
// placed.
struct BoolConjunct {
    BoolPredicate pred;
    std::vector<SubChain> subs;
};

StatusOr<BoolConjunct> LowerBoolConjunct(const Scope& scope, const parser::BoolExpr& tree,
                                         SubqueryLowering lower) {
    BoolConjunct out;
    auto pred = LowerBoolTree(scope, tree, lower, out.subs);
    if (!pred.ok()) return pred.status();
    out.pred = std::move(pred.value());
    return out;
}

void RebaseSubChains(BoolPredicate& pred, std::uint32_t base) {
    if (!pred.reads_sub_chain) return;
    if (pred.op == BoolPredicate::Op::kSubChain) pred.sub_chain += base;
    for (BoolPredicate& child : pred.children) RebaseSubChains(child, base);
}

// Onto `step`: its sub-chains join the step's, and each leaf is rebased onto
// where they landed.
void AttachBoolConjunct(Step& step, BoolConjunct conjunct) {
    RebaseSubChains(conjunct.pred, static_cast<std::uint32_t>(step.sub_chains.size()));
    for (SubChain& sub : conjunct.subs) step.sub_chains.push_back(std::move(sub));
    step.bool_residual.push_back(std::move(conjunct.pred));
}

// The statement's class: the most volatile purity any function conjunct
// calls, through every sub-chain, hoisted or placed (AR1 §3).
DeterminismClass DeterminismOf(const StepChain& chain) {
    DeterminismClass out = DeterminismClass::kD0;
    ForEachStep(chain, [&](const Step& step) {
        const auto note = [&out](const FunctionPredicate& pred) {
            for (const FunctionSide* side : {&pred.lhs, &pred.rhs}) {
                if (side->call.has_value()) out = std::max(out, ClassOf(side->call->fn->purity));
            }
        };
        for (const FunctionPredicate& pred : step.fn_residual) note(pred);
        for (const BoolPredicate& pred : step.bool_residual) ForEachFunction(pred, note);
        return true;
    });
    return out;
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
            out.decimal_len = arg.len;
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
                auto matched = FindColumnPos(s->relations[i].access->schema, name, col_pos);
                if (!matched.ok()) return matched.status();
                if (!matched.value()) {
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
            auto matched = FindColumnPos(s->relations[i].access->schema, name, col_pos);
            if (!matched.ok()) return matched.status();
            if (!matched.value()) continue;
            if (found.has_value()) {
                return Status::InvalidArgument(
                    "column '" + name.name + "' is ambiguous" + Position(name.byte_offset) +
                    ": more than one relation in scope has it; qualify it (`a." + name.name + "`)");
            }
            found = ColumnRef{up, static_cast<std::uint16_t>(i), col_pos};
        }
        if (found.has_value() && s->shares_parent_names && s->parent != nullptr) {
            for (const BoundRelation& r : s->parent->relations) {
                std::uint16_t col_pos = 0;
                auto matched = FindColumnPos(r.access->schema, name, col_pos);
                if (!matched.ok()) return matched.status();
                if (matched.value()) {
                    return Status::InvalidArgument(
                        "column '" + name.name + "' is ambiguous" + Position(name.byte_offset) +
                        ": the statement's relation and its FROM relation both have it; "
                        "qualify it (`a." + name.name + "`)");
                }
            }
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

bool IsTupleKey(const ColumnRef& ref) {
    // Invariant 11: a relation's first column is its tuple key, carried by the
    // Keystone word. It is the only column a descent can
    // address, so it is the only one that can make a step a lookup.
    return ref.col_pos == 0;
}

// Whether `ref` names a column of the step at `slot` in this chain.
bool IsOwnColumn(const ColumnRef& ref, std::uint16_t slot) {
    return ref.up == 0 && ref.rel_slot == slot;
}

// The descriptor guard every deferred-value form shares (equality
// propagation states the argument): the value crossing was produced under
// one column's descriptor, and only an identical (type_val, len) makes it
// byte-valid under the other's.
bool SameDescriptor(const catalog::SysColumnRow& a, const catalog::SysColumnRow& b) {
    return a.type_val == b.type_val && a.len == b.len;
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
//    carrying L written literals appends L*(M-1) conjuncts.
//
// Derived conjuncts carry `StepPredicate::derived`, so ANALYZE can mark
// them and name only what the client wrote.
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

    // One derivation rule for the three kinds a literal conjunct can be:
    // an equality (§5), a lower bound and an upper bound (CB-S1,
    // workorder-cb-join-planner.md §3.1). For each member of a class and
    // each kind it carries no conjunct of yet - written, or derived by an
    // earlier kind - the first written conjunct of that kind on another
    // member crosses, under an identical descriptor. Equality runs first,
    // so a member that gains one takes no bound: the equality is the
    // stronger conjunct.
    using Kind = bool (*)(parser::CompareOp);
    const Kind is_equality = [](parser::CompareOp op) { return op == parser::CompareOp::kEq; };
    const Kind is_lower = [](parser::CompareOp op) {
        return op == parser::CompareOp::kGt || op == parser::CompareOp::kGte;
    };
    const Kind is_upper = [](parser::CompareOp op) {
        return op == parser::CompareOp::kLt || op == parser::CompareOp::kLte;
    };
    const auto literal_on = [&predicates](const ColumnRef& ref, Kind kind, std::size_t end) {
        return std::any_of(predicates.begin(), predicates.begin() + static_cast<std::ptrdiff_t>(end),
                           [&](const StepPredicate& p) {
                               return p.rhs.kind == OperandKind::kLiteral && p.lhs == ref &&
                                      kind(p.op);
                           });
    };
    for (const Kind kind : {is_equality, is_lower, is_upper}) {
        for (const std::vector<ColumnRef>& cls : classes) {
            for (const ColumnRef& member : cls) {
                // Written conjuncts and those derived so far, so a member
                // given an equality above takes no bound here.
                if (literal_on(member, kind, predicates.size())) continue;
                if (kind != is_equality && literal_on(member, is_equality, predicates.size())) {
                    continue;
                }
                const catalog::SysColumnRow& dst = ColumnAt(scope, member);
                for (std::size_t i = 0; i < written; ++i) {
                    const StepPredicate& p = predicates[i];
                    if (p.rhs.kind != OperandKind::kLiteral || !kind(p.op)) continue;
                    if (std::find(cls.begin(), cls.end(), p.lhs) == cls.end()) continue;
                    if (!SameDescriptor(dst, ColumnAt(scope, p.lhs))) continue;
                    StepPredicate derived;
                    derived.lhs = member;
                    derived.op = p.op;
                    derived.rhs = p.rhs;
                    derived.derived = true;
                    // Constructed whole before the push_back invalidates
                    // `p`, and the break keeps anything dangling from being
                    // read.
                    predicates.push_back(std::move(derived));
                    break;
                }
            }
        }
    }
}

// A non-negative integer literal, as a tuple key bound. Anything else - a string,
// a NULL, a `$param`, a negative number - is not a tuple key value
// (invariant 7: ids are zero-extended, kKeystoneIdBits wide), so it cannot bound a range.
std::optional<std::uint64_t> TupleKeyBound(const Operand& operand) {
    if (operand.kind != OperandKind::kLiteral) return std::nullopt;
    if (operand.literal.type != parser::ValueType::kInt) return std::nullopt;
    // A literal past int64 wrapped in the lexer, so `int_val` is not its
    // value. Coercion refuses every such literal an integer tuple key cannot hold
    // before this runs; kept so a bound is never keyed at the number one
    // wrapped to, whichever caller reaches here.
    if (parser::IntLiteralWrapped(operand.literal)) return std::nullopt;
    if (operand.literal.int_val < 0) return std::nullopt;
    return static_cast<std::uint64_t>(operand.literal.int_val);
}

// The tuple key bounds this step's residual implies, if any.
//
// Reads the *lowered* conjuncts rather than the AST's `BETWEEN`, which is
// deliberate: it means a hand-written `id >= 1 AND id <= 5` gets the same
// range as `id BETWEEN 1 AND 5`, because they are the same statement once
// the parser is done with them. A range from one spelling and not the other
// would be an optimizer that rewards phrasing. The strict forms are the
// same range with an inclusive end: `id > a` is `id >= a + 1` and `id < b`
// is `id <= b - 1`, so `id >= a AND id < b` walks its window rather than
// the relation. `id < 0` holds for no key and leaves the step a scan.
//
// Both ends, never one: a one-sided range promises nothing about how much
// of the relation it walks.
//
// `strict` says a `>` or `<` set one of the two ends. Such a range ranks
// last, not where an inclusive one does (operator's ruling (a),
// 2026-10-15): it replaces only a step every other arm declined, so it
// never displaces an index, a Cabin or a build. Where a range belongs
// against those arms is undecided; until it is, the spellings rank apart.
struct TupleKeyRange {
    RangeBounds bounds;
    bool strict = false;
};

std::optional<TupleKeyRange> TupleKeyRangeOf(const Step& step, std::uint16_t slot) {
    std::optional<std::uint64_t> low;
    std::optional<std::uint64_t> high;
    bool low_strict = false;
    bool high_strict = false;
    for (const StepPredicate& pred : step.residual) {
        if (!IsOwnColumn(pred.lhs, slot) || !IsTupleKey(pred.lhs)) continue;
        auto bound = TupleKeyBound(pred.rhs);
        if (!bound.has_value()) continue;
        std::optional<std::uint64_t> inclusive_low;
        std::optional<std::uint64_t> inclusive_high;
        switch (pred.op) {
            case parser::CompareOp::kGte: inclusive_low = *bound; break;
            // A literal is at most INT64_MAX, so `+ 1` cannot wrap.
            case parser::CompareOp::kGt: inclusive_low = *bound + 1; break;
            case parser::CompareOp::kLte: inclusive_high = *bound; break;
            case parser::CompareOp::kLt:
                if (*bound == 0) return std::nullopt;
                inclusive_high = *bound - 1;
                break;
            default: break;
        }
        // An end is strict only if no conjunct reaching the same bound wrote
        // it inclusively, so the rank does not hang on conjunct order.
        if (inclusive_low && (!low || *inclusive_low >= *low)) {
            const bool strict = pred.op == parser::CompareOp::kGt;
            low_strict = (low && *inclusive_low == *low) ? low_strict && strict : strict;
            low = inclusive_low;
        }
        if (inclusive_high && (!high || *inclusive_high <= *high)) {
            const bool strict = pred.op == parser::CompareOp::kLt;
            high_strict = (high && *inclusive_high == *high) ? high_strict && strict : strict;
            high = inclusive_high;
        }
    }
    if (!low.has_value() || !high.has_value()) return std::nullopt;
    // An inverted range is legal to write and matches nothing. Left as a
    // plain scan: the residual returns the correct empty answer, and a
    // range walk would have to special-case it anyway.
    if (*low > *high) return std::nullopt;
    return TupleKeyRange{RangeBounds{*low, *high}, low_strict || high_strict};
}

// Whether this step has at least one equality against a literal on a
// non-tuple-key column that carries no index - the thing kFilterScan names.
//
// **Asks the cached index list, not the catalog** (`TableAccess::IndexOn`).
// It called `Catalog::FindIndexOnColumn` when nothing created indexes and
// the answer was always "unindexed"; that was a `sys.indexes` scan per
// equality per compile. `IndexOn` names an index's **leading** key column
// only, which is exactly the right question: an index on (a, b) cannot be
// entered by an equality on `b`, so such an equality really is an
// unindexed filter. A building index (CI) is no index here either.
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
        if (!IsOwnColumn(pred.lhs, slot) || IsTupleKey(pred.lhs)) continue;
        if (access.IndexOn(pred.lhs.col_pos) != nullptr) continue;
        return true;
    }
    return false;
}

// ---- Index selection (docs/spec/index.md §9) ----------------------------
//
// **`f(shape, catalog)` given the step's predecessors, and nothing else.**
// No statistics, no cardinality estimate, no property of the data chooses
// an index - the join planner's statistics move only a block's order
// (CC-S3), which can change the predecessors. Same argument `CabinProbeOf`
// gives for taking the *first* cabined equality rather than the most
// selective one.

// The inclusive literal bounds the residual pins `col_pos` between, if any.
// `BETWEEN` lowers to `>=` and `<=`, which is the only form the grammar can
// produce, so only those two are read.
struct ColumnBounds {
    const parser::AstValue* low = nullptr;
    const parser::AstValue* high = nullptr;
};

// The correlated-equality shape both structure arms select on: a kEq
// column-column conjunct with one side owned by this step and the other
// available before it runs - an earlier slot, or an enclosing chain's row
// (`up > 0`), which is bound before this chain opens. Both orientations
// are examined, as the tuple-key-probe arm does and for its reason; a
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

// Whether a conjunct may choose this step's access. A derived range bound
// (CB-S1) on a column the step already keys per outer row - the own side
// of a correlated equality - may not: the range is a superset of every
// outer row's equality match, so letting it pick an index range or a
// predicate Cabin would walk the whole range per outer row where the
// correlated arm descends to the matches (workorder-cb-join-planner.md
// §3.1: plans may only be strengthened). It still filters, and still
// narrows a build.
bool SteersAccess(const Step& step, std::uint16_t slot, const StepPredicate& pred) {
    if (!pred.derived || pred.op == parser::CompareOp::kEq) return true;
    return std::none_of(step.residual.begin(), step.residual.end(), [&](const StepPredicate& p) {
        const auto eq = CorrelatedEqualityOf(p, slot);
        return eq.has_value() && eq->own == pred.lhs;
    });
}

ColumnBounds BoundsOnColumn(const Step& step, std::uint16_t slot, std::uint16_t col_pos) {
    ColumnBounds out;
    for (const StepPredicate& pred : step.residual) {
        if (!IsOwnColumn(pred.lhs, slot) || pred.lhs.col_pos != col_pos) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (!SteersAccess(step, slot, pred)) continue;
        // Unconstructible since declared patterns were withdrawn, and kept
        // for EqualityOnColumn's reason below.
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
        // A `$param` never enters an index. Unreachable (nothing constructs
        // one since declared patterns were withdrawn, ast.hpp) and kept
        // because the failure it prevents is silent: a kParam borrows
        // `str_val` for the parameter's *name*, so encoding one as a varchar
        // key would index the name and return a wrong answer.
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
    // A bound is a whole sort key: the entry, covered bytes included
    // (index_page.hpp).
    probe.low.assign(ix.entry_width, std::byte{0x00});
    probe.high.assign(ix.entry_width, std::byte{0xFF});
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
        // A building index's entries are not complete (CI-R1).
        if (ix.building) continue;
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

// The correlated index probe (docs/spec/index.md §8a), or nullopt: an index
// whose **leading** key column is bound by equality to a column of an
// *earlier* step or an enclosing chain - a join key - so the descent is
// keyed per outer row instead of the relation being walked per outer row:
// an index-served inner join side.
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
//    when the descent happens; same rule as the tuple-key-probe arm.
std::optional<IndexProbe> CorrelatedIndexProbeOf(const Scope& scope,
                                                 const catalog::TableAccess& access,
                                                 const Step& step, std::uint16_t slot) {
    if (access.indexes.empty()) return std::nullopt;

    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        if (ix.building || ix.nkeys == 0) continue;  // CI-R1
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
// non-tuple-key equality against a literal - and that is the point: a cabined
// column's equality *is* the filter scan, and the Cabin is the reason it
// need not be one.
//
// **Catalog state only.** `TableAccess::CabinOn` reads a DDL fact carried on
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
    for (const StepPredicate& pred : step.residual) {
        if (pred.op != parser::CompareOp::kEq) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (!IsOwnColumn(pred.lhs, slot) || IsTupleKey(pred.lhs)) continue;

        const catalog::TableAccess::CabinRef cabin = access.CabinOn(pred.lhs.col_pos);
        if (cabin.id == 0) continue;

        // A `$param` never probes a Cabin. Unreachable for
        // EqualityOnColumn's reason and kept for it, though the exposure
        // here is smaller: `MakeCabinKey` already refuses the kind, so this
        // guard saves the walk rather than the answer.
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

// A function conjunct's call as a Cabin term, when every argument is this
// step's own column - never a correlated or another relation's.
std::optional<CabinTerm> OwnCallTerm(const FunctionTerm& call, std::uint16_t slot) {
    CabinTerm term;
    term.fn = call.fn;
    for (const ColumnRef& arg : call.args) {
        if (!IsOwnColumn(arg, slot)) return std::nullopt;
        term.args.push_back(arg.col_pos);
    }
    return term;
}

CabinProbe ProbeFor(const catalog::TableAccess::ExprCabinRef& ref, parser::AstValue value) {
    CabinProbe probe;
    probe.cabin_id = ref.id;
    probe.col_pos = ref.f->inputs.front();
    probe.value = std::move(value);
    probe.f = ref.f;
    probe.declared = ref.origin == catalog::kCabinOriginUser;
    probe.managed = ref.origin == catalog::kCabinOriginAuto;
    return probe;
}

bool AnyShaped(const catalog::TableAccess& access, std::uint8_t shape) {
    return std::any_of(access.expr_cabins.begin(), access.expr_cabins.end(),
                       [shape](const auto& ref) { return ref.shape == shape && ref.f != nullptr; });
}

// The expression Cabin probe (AQ-R5), or nullopt: a function conjunct
// `F(cols) = literal` over this step's own columns whose `F` is an
// expression Cabin's - **confirmed by its canonical text** (AQ-R2), the
// identity `CREATE CABIN` refused a duplicate of, never by a hash. Reached
// after the column Cabin's arm, at its rank. The conjunct stays in
// `fn_residual`: the probe is a hint on top of it, as every probe is on top
// of its residual, so a served row is re-checked and a step downgraded to a
// walk answers the same.
//
// Never a key, a bound, an index or join key, or a `BuildKey`: this arm
// reads `fn_residual` and writes nothing but `Step::cabin`. The correlated
// form (§4a) is not extended to `F`.
std::optional<CabinProbe> ExprCabinProbeOf(const catalog::TableAccess& access, const Step& step,
                                           std::uint16_t slot) {
    if (!AnyShaped(access, catalog::kCabinShapeExpression)) return std::nullopt;
    for (const FunctionPredicate& pred : step.fn_residual) {
        if (pred.op != parser::CompareOp::kEq || !pred.lhs.call.has_value()) continue;
        if (pred.rhs.call.has_value() || pred.rhs.plain.kind != OperandKind::kLiteral) continue;
        const std::optional<CabinTerm> term = OwnCallTerm(*pred.lhs.call, slot);
        if (!term.has_value()) continue;
        const std::string text = CabinTermText(*term);
        for (const catalog::TableAccess::ExprCabinRef& ref : access.expr_cabins) {
            if (ref.shape == catalog::kCabinShapeExpression && ref.f != nullptr &&
                ref.f->text == text) {
                return ProbeFor(ref, pred.rhs.plain.literal);
            }
        }
    }
    return std::nullopt;
}

// The predicate Cabin probe (AQ-R6, `[quiet-wrong]`), or nullopt: a Cabin
// `p = (c1 AND c2 ...)` serves a step when **every** `ci` equals one of the
// step's own **lowered** conjuncts - a plain comparison with a literal or an
// `IS [NOT] NULL` in `residual`, an immutable call's in `fn_residual` - by
// canonical text: same column positions, same operator, same coerced
// literal. Nothing is normalised beyond the lowering every conjunct already
// gets (no `<` against `>`, no `>= 1001` against `> 1000`); a statement's
// `BETWEEN` lowers exactly to its two bounds, which are therefore its
// conjuncts (AQ-Q15). So the match errs toward the walk: a wrong match
// serves `E(true)` to a statement whose WHERE does not imply `p` and loses
// every row passing it and failing `p`. Every conjunct, `p`'s included,
// stays where it is and re-checks the served rows. After the expression
// arm, at the column Cabin's rank.
std::optional<CabinProbe> PredicateCabinProbeOf(const catalog::TableAccess& access,
                                                const Step& step, std::uint16_t slot) {
    if (!AnyShaped(access, catalog::kCabinShapePredicate)) return std::nullopt;
    std::vector<std::string> own;
    for (const StepPredicate& pred : step.residual) {
        if (!IsOwnColumn(pred.lhs, slot)) continue;
        if (TakesLiteral(pred.op) && pred.rhs.kind != OperandKind::kLiteral) continue;
        if (!SteersAccess(step, slot, pred)) continue;
        CabinConjunct c;
        c.term.args.push_back(pred.lhs.col_pos);
        c.op = pred.op;
        c.literal = pred.rhs.literal;
        own.push_back(CabinConjunctText(c));
    }
    for (const FunctionPredicate& pred : step.fn_residual) {
        if (!pred.lhs.call.has_value() || pred.rhs.call.has_value()) continue;
        if (TakesLiteral(pred.op) && pred.rhs.plain.kind != OperandKind::kLiteral) continue;
        std::optional<CabinTerm> term = OwnCallTerm(*pred.lhs.call, slot);
        if (!term.has_value()) continue;
        CabinConjunct c;
        c.term = std::move(*term);
        c.op = pred.op;
        c.literal = pred.rhs.plain.literal;
        own.push_back(CabinConjunctText(c));
    }
    if (own.empty()) return std::nullopt;
    std::sort(own.begin(), own.end());

    for (const catalog::TableAccess::ExprCabinRef& ref : access.expr_cabins) {
        if (ref.shape != catalog::kCabinShapePredicate || ref.f == nullptr) continue;
        const bool implied = std::all_of(
            ref.f->conjuncts.begin(), ref.f->conjuncts.end(), [&](const CabinConjunct& c) {
                return std::binary_search(own.begin(), own.end(), CabinConjunctText(c));
            });
        if (implied) return ProbeFor(ref, PredicateKey());
    }
    return std::nullopt;
}

// The cover probe (AQ-R8), or nullopt: a step whose residual bounds a column
// `x` on both sides - `>=`/`>` and `<=`/`<` against literals, a `BETWEEN`'s two
// halves, or an equality, which is both - where `x` is the one argument of an
// expression Cabin whose function declares a cover. The tightest bound on each
// side is taken; any pair would do, since every bound is a conjunct and the
// cover of any pair is a superset of the rows passing them all. The bounds
// stay in the residual and trim the cover's surplus.
std::optional<CabinProbe> CoverCabinProbeOf(const catalog::TableAccess& access, const Step& step,
                                            std::uint16_t slot) {
    if (!AnyShaped(access, catalog::kCabinShapeExpression)) return std::nullopt;
    for (const catalog::TableAccess::ExprCabinRef& ref : access.expr_cabins) {
        if (ref.shape != catalog::kCabinShapeExpression || ref.f == nullptr) continue;
        const CabinTerm& term = ref.f->expr;
        if (term.fn->cover == nullptr || term.args.size() != 1) continue;
        std::optional<std::int64_t> low;
        std::optional<std::int64_t> high;
        for (const StepPredicate& pred : step.residual) {
            if (!IsOwnColumn(pred.lhs, slot) || pred.lhs.col_pos != term.args.front()) continue;
            if (pred.rhs.kind != OperandKind::kLiteral ||
                pred.rhs.literal.type != parser::ValueType::kInt ||
                !SteersAccess(step, slot, pred)) {
                continue;
            }
            const std::int64_t v = pred.rhs.literal.int_val;
            const bool lower = pred.op == parser::CompareOp::kGte ||
                               pred.op == parser::CompareOp::kGt ||
                               pred.op == parser::CompareOp::kEq;
            const bool upper = pred.op == parser::CompareOp::kLte ||
                               pred.op == parser::CompareOp::kLt ||
                               pred.op == parser::CompareOp::kEq;
            if (lower) low = low.has_value() ? std::max(*low, v) : v;
            if (upper) high = high.has_value() ? std::min(*high, v) : v;
        }
        if (!low.has_value() || !high.has_value() || *low > *high) continue;
        std::int64_t first = 0;
        std::int64_t last = 0;
        term.fn->cover(*low, *high, first, last);
        if (first > last) continue;
        CabinProbe probe = ProbeFor(ref, parser::AstValue{});
        probe.cover = std::make_pair(first, last);
        return probe;
    }
    return std::nullopt;
}

// The correlated cabin probe (docs/spec/cabin.md §4a), or nullopt: a
// cabined column bound by equality to an earlier step's or an enclosing
// chain's column - the join shape, IX17's selection one trust class over.
// Reached only after the literal form declined; first qualifying conjunct
// in residual order; both ON orientations, the tuple key excluded on either side
// (the tuple key arm already served it better). The identical-descriptor guard is
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
    for (const StepPredicate& pred : step.residual) {
        auto eq = CorrelatedEqualityOf(pred, slot);
        // The tuple key exclusion is this arm's own: an index's leading column can
        // never be the tuple key (CREATE INDEX refuses it), but a join written on
        // the tuple key reaches here and is already served better by the tuple key arm.
        if (!eq.has_value() || IsTupleKey(eq->own)) continue;

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
// only a forgone build.
//
// **One rule: exactly one conjunct may read a column that is not this
// step's own, and it is the key** - a non-tuple-key correlated equality of
// one descriptor. The map buckets the rows passing every other conjunct
// once, against the first outer row, so each of those must answer the same
// for every outer row: one reading an earlier step's or the enclosing row's
// column would be judged for the first outer row alone, and every later
// probe would miss the rows it admits for theirs (spec §8). The rule also
// declines a multi-column join key (CB12's scope rule) and a
// mixed-descriptor equality; a tuple key equality became kProbe in the kind
// pass, so its decline here is defensive.
// The kFilterScan, CompileWhere and scalar-sub-chain declines are the call
// site's - they gate whether this arm runs at all.
std::optional<BuildKey> BuildKeyOf(const Scope& scope, const catalog::TableAccess& access,
                                   const Step& step, std::uint16_t slot) {
    // Nothing bounds a statement's conjunct count - the parser's WHERE loop
    // appends while `AND` follows and stops at no cap - so a position past
    // the field's width is expressible. A truncated `residual_pos` would name
    // a *different* conjunct and partition the residual around the wrong
    // one, so the arm declines rather than narrowing.
    constexpr std::size_t kMaxResidualPos = 0xFFFF;  // BuildKey::residual_pos is uint16_t
    std::optional<BuildKey> out;
    for (std::size_t i = 0; i < step.residual.size(); ++i) {
        const StepPredicate& pred = step.residual[i];
        if (IsOwnColumn(pred.lhs, slot) &&
            (pred.rhs.kind != OperandKind::kColumn || IsOwnColumn(pred.rhs.column, slot))) {
            continue;  // the same answer for every outer row: safe to bucket on
        }
        const auto eq = CorrelatedEqualityOf(pred, slot);
        if (out.has_value() || !eq.has_value() || IsTupleKey(eq->own) || i > kMaxResidualPos ||
            !SameDescriptor(access.schema.columns[eq->own.col_pos], ColumnAt(scope, eq->other))) {
            return std::nullopt;
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
            // All three address the relation by its tuple key and nothing else.
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
            // statistics exist to draw. **An expression Cabin's probe reports
            // none** (AQ-Q14): the statistics are keyed by column, `F` is not
            // one, and filing `DATE(ts) = d` under `ts` would merge it with a
            // column Cabin on `ts`.
            if (step.cabin.has_value() && step.cabin->f == nullptr) {
                out.push_back(step.cabin->col_pos);
            }
            return out;
        case AccessKind::kFilterScan:
            for (const StepPredicate& pred : step.residual) {
                if (pred.op != parser::CompareOp::kEq) continue;
                if (pred.rhs.kind != OperandKind::kLiteral) continue;
                if (!IsOwnColumn(pred.lhs, slot) || IsTupleKey(pred.lhs)) continue;
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
        for (const FunctionPredicate& pred : step.fn_residual) ForEachColumn(pred, consider);
        for (const BoolPredicate& pred : step.bool_residual) ForEachColumn(pred, consider);
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
        // A function argument reaching outward correlates the sub-chain as
        // surely as a predicate's column does; missing it would hoist the
        // sub-chain and run it once for every outer row.
        bool escapes = false;
        const auto escape = [&](const ColumnRef& ref) { escapes = escapes || ref.up > 0; };
        for (const FunctionPredicate& pred : step.fn_residual) ForEachColumn(pred, escape);
        for (const BoolPredicate& pred : step.bool_residual) ForEachColumn(pred, escape);
        if (escapes) return true;
        if (step.key.has_value() && OperandEscapes(*step.key)) return true;
        for (const SubChain& sub : step.sub_chains) {
            // A grandchild's reference to *this* level shows up as up==1
            // inside it; anything deeper escapes past us too.
            if (SubChainEscapes(sub)) return true;
            // Its tested column is written in this block's scope, so an
            // `up` above 0 is a reference past this block. `SubChainEscapes`
            // leaves it out for the sub-chain's *own* parent, which is the
            // wrong scope for this question.
            if (sub.has_value && sub.lhs.up > 0) return true;
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
        for (const FunctionPredicate& pred : other.fn_residual) ForEachColumn(pred, note);
        for (const BoolPredicate& pred : other.bool_residual) ForEachColumn(pred, note);
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

    // The trail records the Keystone tuple key of every row a replayable step
    // accepts, and reads it from the frame rather than looking it up. The
    // *kind* is a compile-time fact, so this costs a column only on the
    // steps that can record one.
    if (IsTrailReplayable(step.kind)) {
        mask |= 1;  // column 0 is the tuple key (invariant 11)
    }

    // A Cabin's miss walk records the key column and the tuple key. At execute
    // time the walk decodes `filter_columns` per row, plus the tuple key for a
    // row whose key matches (step_vm.cpp), and naming both here keeps the
    // survivors' remaining-columns decode honest about what was already read.
    // An expression Cabin's walk reads every input of `F` (AQ-R5).
    if (step.cabin.has_value()) {
        const auto note = [&mask](std::uint16_t pos) {
            if (pos < 64) mask |= std::uint64_t{1} << pos;
        };
        if (step.cabin->f != nullptr) {
            for (const std::uint16_t pos : step.cabin->f->inputs) note(pos);
        } else {
            note(step.cabin->col_pos);
        }
        mask |= 1;
    }

    return all ? Step::kAllColumns : mask;
}

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
    // A function conjunct is evaluated beside the residual, on the same
    // decode, so its arguments are what this row must have read too.
    for (const FunctionPredicate& pred : step.fn_residual) ForEachColumn(pred, note);
    for (const BoolPredicate& pred : step.bool_residual) ForEachColumn(pred, note);
    return mask;
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
                                 bool inner_build, PositionSink* declare,
                                 const StatementContext* context = nullptr,
                                 const PlanOptions* plan = nullptr);

// A predicate-position subquery lowered to its sub-chain (V15), for a flat
// conjunct and a boolean conjunct's leaf alike: compiled against a scope
// whose parent is `scope`, which is what turns an inner reference to an
// outer column into `up == 1` rather than a resolution failure, and
// classified. Where it runs is the caller's.
StatusOr<SubChain> LowerSubquery(catalog::Catalog& catalog, const Scope& scope,
                                 const parser::Condition& cond, std::uint32_t& next_step_id,
                                 std::uint32_t depth, const txn::ReadView* view,
                                 bool inner_build, PositionSink* declare) {
    SubChain sub;
    sub.kind = cond.kind;
    sub.op = cond.op;
    sub.at_most_one = cond.at_most_one;

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
    // (workplan JB1): its stopping walk has no `Exists` semantics for a
    // prefix map to be conclusive under (spec §6). Off stays off for
    // everything nested.
    const bool sub_inner_build =
        inner_build && cond.kind != parser::PredicateKind::kCompareSubquery;
    auto inner = CompileBlock(catalog, *cond.subquery, &scope, next_step_id, depth, view,
                              sub_inner_build, declare);
    if (!inner.ok()) return inner.status();
    sub.steps = std::move(inner.value().steps);
    // What the inner block hoisted travels with it: only the top-level
    // chain's list is run by the executor itself, so a block lowered to a
    // SubChain keeps the sub-chains it hoisted, or they never run.
    sub.hoisted = std::move(inner.value().hoisted);

    if (tests_a_column) {
        // IN and the scalar form read a value out of the sub-chain, so it
        // must project exactly one column. `*` over a relation with several
        // columns has no single value to mean, and picking the first would
        // make the answer depend on schema order.
        if (inner.value().projection.size() != 1) {
            return Status::Unsupported("a subquery used as a value must project exactly one column" +
                                       Position(cond.col.byte_offset));
        }
        if (Status s = CheckSubqueryValue(scope, sub.lhs, inner.value(), cond.col.byte_offset);
            !s.ok()) {
            return s;
        }
        sub.value = inner.value().projection[0];
        sub.has_value = true;
    }
    sub.correlated = SubChainEscapes(sub);
    return sub;
}

}  // namespace

StatusOr<StepChain> Compile(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                            const txn::ReadView* view, PositionSink* declare,
                            const StatementContext* context, const PlanOptions* plan) {
    std::uint32_t next_step_id = 0;
    auto chain = CompileBlock(catalog, stmt, /*parent=*/nullptr, next_step_id, /*depth=*/0,
                              view, /*inner_build=*/true, declare, context, plan);
    if (!chain.ok()) return chain;
    StepChain& out = chain.value();
    out.determinism = DeterminismOf(out);
    return chain;
}

Status CompileAssignments(const catalog::TableAccess& access,
                          const std::vector<parser::Assignment>& assignments,
                          std::vector<CompiledAssignment>* compiled,
                          const SubqueryResolver* subqueries) {
    // An empty schema cannot happen for a relation the catalog resolved
    // (invariant 11 makes column 0 mandatory), but the tuple key name is read
    // from `front()` and a check costs nothing against a wrong answer.
    if (access.schema.columns.empty()) {
        return Status::Corruption("relation oid " + std::to_string(access.oid) +
                                  " has no columns");
    }
    const std::string_view tuple_key_name = catalog::NameView(access.schema.columns.front().name);

    std::vector<const catalog::SysColumnRow*> assigned;
    // A refusal that is only a `NotImplemented` waits until every assignment
    // has been judged, so a later one's own refusal is not hidden behind it.
    std::optional<Status> deferred;
    if (compiled != nullptr) {
        compiled->clear();
        compiled->reserve(assignments.size());
    }
    for (const parser::Assignment& a : assignments) {
        auto found = access.schema.FindColumn(a.col_name, a.byte_offset);
        if (!found.ok()) return found.status();
        const catalog::SysColumnRow* column = found.value();
        if (column == nullptr) {
            return Status::InvalidArgument("unknown column '" + a.col_name + "' at byte " +
                                           std::to_string(a.byte_offset));
        }
        // BJ-R2: one assignment per column; last-wins let the foreign-key
        // hoist check the first value while the write kept the last.
        if (std::find(assigned.begin(), assigned.end(), column) != assigned.end()) {
            return Status::InvalidArgument("column '" + a.col_name +
                                           "' is assigned more than once at byte " +
                                           std::to_string(a.byte_offset));
        }
        assigned.push_back(column);
        // K2, and the reason it is `Unsupported` rather than
        // `InvalidArgument`: the statement is understood and declined. The
        // column exists and the value would encode; what cannot happen is
        // the write, because the id names the tuple everywhere else in the
        // engine. Asked of the resolved column, never the written bytes: a
        // name resolves ignoring case, so `SET ID` is the tuple key `id`.
        if (column == &access.schema.columns.front()) {
            return Status::Unsupported("tuple key column '" + std::string(tuple_key_name) +
                                       "' cannot be updated at byte " +
                                       std::to_string(a.byte_offset) +
                                       "; it is the tuple's identity, not a field of it");
        }

        CompiledAssignment entry;
        entry.column_no = static_cast<std::size_t>(column - access.schema.columns.data());
        const catalog::ForeignKeyRef* fk = nullptr;
        for (const catalog::ForeignKeyRef& candidate : access.fkeys_out) {
            if (candidate.column_no == entry.column_no) fk = &candidate;
        }
        if (a.expr == nullptr) {
            // A bare literal keeps today's per-row type gate (BJ-Q3 (c)); a
            // foreign-key column hoists its parent from the value itself.
            if (fk != nullptr) entry.constant = a.val;
        } else {
            // Typed first (BJ-S3): a mistyped expression is refused here by
            // what is wrong with it, whether or not a row matches.
            auto typed = TypeSetExpression(access, *a.expr, *column, subqueries);
            if (!typed.ok()) return typed.status();
            entry.expr = std::move(typed.value());
            if (fk != nullptr) {
                if (!IsConstantSetExpression(*entry.expr)) {
                    // BJ-Q10: the parent is a per-row value, and resolving it
                    // per row under the walk's exclusive leaf would be the
                    // page-latch cycle `rules.md` §3's page latch row
                    // forbids. Refused rather than
                    // checked against a key the row never gets.
                    if (!deferred.has_value()) {
                        deferred = Status::NotImplemented(
                            "an expression that reads a column or the clock cannot be assigned "
                            "to foreign-key column '" +
                            a.col_name + "' yet (BJ-Q10) (byte " +
                            std::to_string(a.expr->byte_offset) + ")");
                    }
                } else if (auto folded = EvaluateSetExpression(*entry.expr, {}, StatementContext{}); folded.ok()) {
                    entry.constant = std::move(folded.value());
                }
            }
        }
        if (compiled != nullptr) compiled->push_back(std::move(entry));
    }
    if (deferred.has_value()) return *deferred;
    return Status::OK();
}

StatusOr<Step> CompileWhere(catalog::Catalog& catalog, const catalog::TableAccess& access,
                            std::string_view binding,
                            const std::vector<parser::Condition>& where,
                            const txn::ReadView* view, PositionSink* declare,
                            const StatementContext* context) {
    Scope scope;
    scope.relations.push_back(BoundRelation{std::string(binding), &access});
    scope.context = context != nullptr ? *context : StatementContextNow();

    Step out;
    out.rel_oid = access.oid;
    out.kind = AccessKind::kScan;  // the caller walks the relation itself

    std::uint32_t next_step_id = 1;  // 0 is this step
    // `inner_build=false`, v1's SELECT-only rule (workplan JB1,
    // join-inner-build.md §8): its reason was a write's own rows landing
    // between outer rows, which the dispatcher's collection pass has since
    // ruled out - a write's WHERE runs over the relation before its first
    // write (parser-v2.md R2). Lifting the rule is its own decision.
    const auto lower_subquery = [&](const parser::Condition& cond) {
        return LowerSubquery(catalog, scope, cond, next_step_id, /*depth=*/1, view,
                             /*inner_build=*/false, declare);
    };
    for (const parser::Condition& cond : where) {
        if (cond.has_subquery()) {
            // Every sub-chain is attached to the step here; `correlated`
            // is still classified, for a later reader asking why one is
            // re-run per row.
            auto sub = lower_subquery(cond);
            if (!sub.ok()) return sub.status();
            out.sub_chains.push_back(std::move(sub.value()));
            continue;
        }

        if (cond.kind == parser::PredicateKind::kCompareFunction) {
            auto fn = LowerFunctionConjunct(scope, cond);
            if (!fn.ok()) return fn.status();
            out.fn_residual.push_back(std::move(fn.value()));
            continue;
        }

        if (cond.kind == parser::PredicateKind::kBoolTree) {
            auto conjunct = LowerBoolConjunct(scope, *cond.tree, lower_subquery);
            if (!conjunct.ok()) return conjunct.status();
            AttachBoolConjunct(out, std::move(conjunct.value()));
            continue;
        }

        if (Status s = LowerComparison(scope, cond, out.residual); !s.ok()) return s;
    }
    // **The mask the caller keeps** (OPT-001). UPDATE and DELETE need every
    // column of a *matching* row, not of a rejected one, so the dispatcher
    // decodes this mask, tests the predicate, and completes the row only
    // once it matches, exactly as the step VM does.
    //
    // Two gates, both structural, both matching the step VM's:
    //   - a **sub-chain** can correlate to any column of this row and reads
    //     it through the frame, so a step carrying one is unmaskable;
    //   - a relation **wider than 64 columns** cannot be named by a
    //     uint64_t mask, and a partial decode there would leave the tail
    //     holding the previous row's values.
    const bool maskable = out.sub_chains.empty() && access.schema.columns.size() <= 64;
    out.filter_columns = maskable ? FilterColumnsOf(out, /*index=*/0) : Step::kAllColumns;
    return out;
}

namespace {

// A greedy order above this many relations; every permutation at or below
// it (4! = 24 compiles of a statement, against one).
constexpr std::size_t kMaxExhaustiveJoinRelations = stats::kMaxPosteriorRelations;

// A cheaper order must win by more than this fraction, so float noise
// between two equal costs never moves a statement off its written order.
constexpr double kCostTieMargin = 1e-9;
// The margin once a published statistic prices the block (CC-Q8): a band is
// +-30 % wide, and an order that wins by less is inside its noise.
constexpr double kStatsSwitchMargin = 0.10;

bool HasSubquery(const parser::BoolExpr& node) {
    if (node.op == parser::BoolExpr::Op::kLeaf) return node.leaf.has_subquery();
    return std::any_of(node.children.begin(), node.children.end(),
                       [](const parser::BoolExpr& child) { return HasSubquery(child); });
}

// Whether a WHERE carries a subquery anywhere - a flat conjunct or a leaf
// of a boolean one. A block that does is not reordered (CB-Q4 (a)): its
// sub-chains are placed against slots and lowered with side effects.
bool HasSubqueryPredicate(const parser::SelectStmt& stmt) {
    return std::any_of(stmt.where.begin(), stmt.where.end(), [](const parser::Condition& cond) {
        return cond.has_subquery() ||
               (cond.kind == parser::PredicateKind::kBoolTree && cond.tree != nullptr &&
                HasSubquery(*cond.tree));
    });
}

StatusOr<StepChain> CompileBound(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                                 const Scope& scope,
                                 const std::vector<const parser::RelationRef*>& refs,
                                 std::uint32_t& next_step_id, std::uint32_t depth,
                                 const txn::ReadView* view, bool inner_build,
                                 PositionSink* declare);

StatusOr<StepChain> CompileBlock(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                                 const Scope* parent, std::uint32_t& next_step_id,
                                 std::uint32_t depth, const txn::ReadView* view,
                                 bool inner_build, PositionSink* declare,
                                 const StatementContext* context, const PlanOptions* plan) {
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
    scope.shares_parent_names = stmt.shares_parent_names;
    // A nested block reads its outermost's through `parent`.
    if (parent == nullptr) scope.context = context != nullptr ? *context : StatementContextNow();
    std::vector<const parser::RelationRef*> refs;
    refs.push_back(&stmt.from);
    for (const parser::JoinClause& j : stmt.joins) refs.push_back(&j.relation);

    // NS5: an unqualified name means the statement's namespace, which every
    // block reads off the outermost one's context.
    const catalog::Oid default_namespace = ContextOf(scope).default_namespace;
    for (const parser::RelationRef* rel : refs) {
        // One site for FROM, every JOIN and every subquery block - they all
        // bind through this loop.
        auto oid = catalog.ResolveRelation(rel->schema, rel->table_name, default_namespace,
                                           rel->byte_offset, view);
        if (!oid.ok()) return oid.status();
        // AT-R1: the declaration, between the name and the schema
        // (`step_compiler.hpp` on `declare`). The whole id space, because a
        // bind is a claim on the relation and not on any part of it; the
        // walk narrows it to a slice later through this same sink.
        if (declare != nullptr) {
            declare->Position(oid.value(), 0, kIdSpaceEnd);
        }
        auto access = catalog.InitTableAccess(oid.value());
        if (!access.ok()) return access.status();
        scope.relations.push_back(BoundRelation{rel->binding(), access.value()});
    }

    // ---- 1a. The join order (CB-S2, workorder-cb-join-planner.md §3.2) ---
    //
    // A top-level block of two or more relations with no subquery predicate
    // is compiled in the orders below, priced by `EstimateChain`
    // (join_order.hpp), and the cheapest runs if it beats the written order
    // by the margin (CC-Q8). Everything after binding reads slots, so a
    // chain compiled over a permuted binding is that order's chain,
    // unchanged otherwise. The written order is compiled first and its
    // refusal is the statement's.
    if (plan == nullptr || refs.size() < 2 || stmt.star() || HasSubqueryPredicate(stmt)) {
        return CompileBound(catalog, stmt, scope, refs, next_step_id, depth, view, inner_build,
                            declare);
    }
    const std::uint32_t base_step_id = next_step_id;
    const auto compile_order = [&](const std::vector<std::size_t>& order) -> StatusOr<StepChain> {
        Scope permuted = scope;  // whole, so no field is dropped
        permuted.relations.clear();
        std::vector<const parser::RelationRef*> permuted_refs;
        for (const std::size_t written : order) {
            permuted.relations.push_back(scope.relations[written]);
            permuted_refs.push_back(refs[written]);
        }
        std::uint32_t ids = base_step_id;
        auto chain = CompileBound(catalog, stmt, permuted, permuted_refs, ids, depth, view,
                                  inner_build, declare);
        if (!chain.ok()) return chain;
        // CB-Q3 (a): a step's id is its relation's written position, so a
        // trail entry names the same relation whatever order recorded it.
        for (std::size_t k = 0; k < order.size(); ++k) {
            chain.value().steps[k].step_id = base_step_id + static_cast<std::uint32_t>(order[k]);
        }
        return chain;
    };

    std::vector<std::size_t> order(refs.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
    auto best = compile_order(order);
    if (!best.ok()) return best;
    next_step_id = base_step_id + static_cast<std::uint32_t>(refs.size());
    // A written chain of tuple key descents alone - a lookup, then probes -
    // reads one row per step, and every order pays at least its driver's
    // descent: no search can repay its compiles. Measured at CB's close: the
    // six orders of a three-relation point join cost 15 us a statement
    // (bench/v3.2.0/results-cb-close-v3.2.0-174-g0640941f.md).
    const auto is_descent = [](const Step& step) {
        return step.kind == AccessKind::kLookup || step.kind == AccessKind::kProbe;
    };
    if (std::all_of(best.value().steps.begin(), best.value().steps.end(), is_descent)) {
        return best;
    }
    // The key the run's counts fold under and the posteriors are found by
    // (CC-S2): a searched block of at most `kMaxPosteriorRelations`
    // relations with a fetch id.
    std::optional<stats::PlanKey> key;
    if (plan->fetch_id != 0 && refs.size() <= stats::kMaxPosteriorRelations) {
        key.emplace();
        key->fetch_id = plan->fetch_id;
        key->n = static_cast<std::uint8_t>(refs.size());
        for (std::size_t w = 0; w < refs.size(); ++w) key->oids[w] = scope.relations[w].access->oid;
    }
    // The published statistics, pinned here and only here - after the
    // descent skip, so a point read never touches them - and held for the
    // compile (CC-S3).
    const std::shared_ptr<const stats::PlanStatsSnapshot> snapshot =
        plan->statistics != nullptr ? plan->statistics->Pin() : nullptr;

    // The written order's relations, for the identifier bounds (CC-Q11),
    // and their rows: a published prior, or the constant (CC-S3). Resolved
    // once here, not per order.
    std::vector<const catalog::TableAccess*> written_access;
    std::vector<double> written_rows;
    CostInputs inputs;
    for (const BoundRelation& relation : scope.relations) {
        written_access.push_back(relation.access);
        std::optional<double> prior;
        if (snapshot != nullptr) prior = snapshot->RelationRows(relation.access->oid);
        if (prior.has_value()) inputs.informed = true;
        written_rows.push_back(prior.has_value() ? std::max(*prior, 1.0) : kAssumedRelationRows);
    }
    inputs.limits = *plan;
    inputs.written_sorted = best.value().sorted();
    inputs.base_step_id = base_step_id;
    inputs.written_access = written_access;
    inputs.written_rows = written_rows;
    if (snapshot != nullptr && key.has_value()) inputs.posterior = snapshot->Find(*key);
    if (inputs.posterior != nullptr) inputs.informed = true;
    // Once a published fact prices the block an order must beat the
    // written one by `kStatsSwitchMargin`, so a band's movement within its
    // noise does not move a plan (CC-Q8).
    const double margin = inputs.informed ? kStatsSwitchMargin : kCostTieMargin;
    const double written_cost = EstimateChain(best.value(), inputs).cost;

    // CC-S4: the order this block chose before under the same inputs, where
    // the core remembers one - compiled again through the full ladder, so a
    // hit costs two compiles where the search costs up to 24. Never under a
    // minted view: a DDL is in flight and the catalog's stamps are moving.
    // A statement whose literals the fingerprint saw whole: a `?` bound at
    // BIND is outside `arg_hash`, so its block is searched every time.
    PlanMemo* memo = plan->memo != nullptr && key.has_value() && view == nullptr &&
                             plan->literals_known
                         ? plan->memo
                         : nullptr;
    PlanMemoKey memo_key;
    if (memo != nullptr) {
        memo_key.block = *key;
        memo_key.arg_hash = plan->arg_hash;
        memo_key.generation = snapshot != nullptr ? snapshot->generation() : 0;
        memo_key.catalog_word = catalog.cache_built_at();
        memo_key.catalog_version = catalog.catalog_version();
        memo_key.join_build_max_rows = plan->join_build_max_rows;
        memo_key.sort_max_rows = plan->sort_max_rows;
    }
    std::optional<StepChain> cheapest;
    bool from_memo = false;
    if (memo != nullptr) {
        if (const auto remembered = memo->Find(memo_key); remembered.has_value()) {
            std::vector<std::size_t> perm((*remembered).begin(), (*remembered).begin() + refs.size());
            if (perm == order) {  // `order` is still the written order here
                from_memo = true;
            } else if (auto chain = compile_order(perm); chain.ok()) {
                cheapest = std::move(chain.value());
                from_memo = true;
            }
            // A remembered order that no longer compiles is searched again,
            // and the search's answer replaces it.
        }
    }
    if (!from_memo) {
        // CC-Q8: the minimum over every order - the first among equals in
        // permutation order - taken only if it beats the **written** order's
        // cost by the margin, so the threshold starts there. Measured against
        // the written order rather than the best so far, the winner does not
        // depend on enumeration order.
        double cheapest_cost = written_cost * (1.0 - margin);
        const auto cost_of = [&](const std::vector<std::size_t>& candidate)
            -> std::optional<std::pair<double, StepChain>> {
            auto chain = compile_order(candidate);
            if (!chain.ok()) return std::nullopt;  // a refusal is the written order's to give
            const double cost = EstimateChain(chain.value(), inputs).cost;
            return std::make_pair(cost, std::move(chain.value()));
        };
        const auto consider = [&](const std::vector<std::size_t>& candidate) {
            auto costed = cost_of(candidate);
            if (costed.has_value() && costed->first < cheapest_cost) {
                cheapest_cost = costed->first;
                cheapest = std::move(costed->second);
            }
        };
        if (refs.size() <= kMaxExhaustiveJoinRelations) {
            while (std::next_permutation(order.begin(), order.end())) consider(order);
        } else {
            // Greedy: at each position the relation whose placement there - the
            // rest following in written order - costs least.
            std::vector<std::size_t> placed;
            std::vector<std::size_t> rest = order;
            while (!rest.empty()) {
                std::size_t pick = 0;
                std::optional<double> pick_cost;
                for (std::size_t r = 0; r < rest.size(); ++r) {
                    std::vector<std::size_t> candidate = placed;
                    candidate.push_back(rest[r]);
                    for (std::size_t o = 0; o < rest.size(); ++o) {
                        if (o != r) candidate.push_back(rest[o]);
                    }
                    auto costed = cost_of(candidate);
                    if (!costed.has_value()) continue;
                    if (!pick_cost.has_value() || costed->first < *pick_cost) {
                        pick = r;
                        pick_cost = costed->first;
                    }
                }
                placed.push_back(rest[pick]);
                rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(pick));
            }
            consider(placed);
        }
    }
    const bool reordered = cheapest.has_value();
    StepChain chosen = reordered ? std::move(*cheapest) : std::move(best.value());
    if (memo != nullptr && !from_memo) {
        std::array<std::uint8_t, stats::kMaxPosteriorRelations> remembered{};
        for (std::size_t k = 0; k < chosen.steps.size(); ++k) {
            remembered[k] = static_cast<std::uint8_t>(chosen.steps[k].step_id - base_step_id);
        }
        memo->Store(memo_key, remembered);
    }
    if (refs.size() <= kMaxPlannedRelations) {
        for (const BoundRelation& relation : scope.relations) {
            chosen.planned_relations.push_back(relation.access->oid);
        }
    }
    chosen.planned = key;
    // What ANALYZE says of the choice: each slot's estimated rows, the
    // generation they read and the chosen cost against the written order's.
    PlanNote note;
    note.generation = snapshot != nullptr ? snapshot->generation() : 0;
    note.from_memo = from_memo;
    inputs.step_rows = &note.estimated_rows;
    const double chosen_cost = EstimateChain(chosen, inputs).cost;
    note.cost_ratio = reordered && written_cost > 0 ? chosen_cost / written_cost : 1.0;
    chosen.plan_note = std::move(note);
    return chosen;
}

// Everything after binding: the chain of `scope`'s relations in the order
// they stand there (CB-S2 permutes them before calling this).
StatusOr<StepChain> CompileBound(catalog::Catalog& catalog, const parser::SelectStmt& stmt,
                                 const Scope& scope,
                                 const std::vector<const parser::RelationRef*>& refs,
                                 std::uint32_t& next_step_id, std::uint32_t depth,
                                 const txn::ReadView* view, bool inner_build,
                                 PositionSink* declare) {
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
    // Function conjuncts likewise (AP-S4), placed by the same rule.
    std::vector<FunctionPredicate> fn_predicates;
    // And boolean conjuncts (BU-S1), each with the sub-chains it reads.
    std::vector<BoolConjunct> bool_conjuncts;
    const auto lower_subquery = [&](const parser::Condition& cond) {
        return LowerSubquery(catalog, scope, cond, next_step_id, depth + 1, view, inner_build,
                             declare);
    };

    for (const parser::Condition& cond : stmt.where) {
        if (cond.has_subquery()) {
            auto sub = lower_subquery(cond);
            if (!sub.ok()) return sub.status();
            sub_chains.push_back(std::move(sub.value()));
            continue;
        }

        if (cond.kind == parser::PredicateKind::kCompareFunction) {
            auto fn = LowerFunctionConjunct(scope, cond);
            if (!fn.ok()) return fn.status();
            fn_predicates.push_back(std::move(fn.value()));
            continue;
        }

        if (cond.kind == parser::PredicateKind::kBoolTree) {
            auto conjunct = LowerBoolConjunct(scope, *cond.tree, lower_subquery);
            if (!conjunct.ok()) return conjunct.status();
            bool_conjuncts.push_back(std::move(conjunct.value()));
            continue;
        }

        if (Status s = LowerComparison(scope, cond, predicates); !s.ok()) return s;
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
    // A function conjunct goes to the latest step any of its columns
    // reaches - the first step every argument exists at. One with no
    // columns at all (`NOW() > '...'`) goes to the first step.
    for (FunctionPredicate& pred : fn_predicates) {
        std::uint16_t ready_at = 0;
        ForEachColumn(pred, [&](const ColumnRef& ref) {
            ready_at = std::max(ready_at, AvailableAt(ref));
        });
        chain.steps[ready_at].fn_residual.push_back(std::move(pred));
    }
    // A boolean conjunct likewise. Equality propagation above read only
    // `predicates`, the top-level conjuncts, so nothing under an `OR` or a
    // `NOT` was ever a source for it (BU-R5). One reading a sub-chain
    // (BU-S3) waits for it too, by the rule the sub-chains below are placed
    // by - and never hoists it.
    for (BoolConjunct& conjunct : bool_conjuncts) {
        std::uint16_t ready_at = 0;
        ForEachColumn(conjunct.pred, [&](const ColumnRef& ref) {
            ready_at = std::max(ready_at, AvailableAt(ref));
        });
        for (const SubChain& sub : conjunct.subs) {
            if (sub.has_value) ready_at = std::max(ready_at, AvailableAt(sub.lhs));
            ready_at = std::max(ready_at,
                                DeepestReferenceIntoThisChain(sub.steps, /*from_depth=*/1));
        }
        AttachBoolConjunct(chain.steps[ready_at], std::move(conjunct));
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
        if (!sub.correlated && !sub.has_value && !sub.at_most_one) {
            chain.hoisted.push_back(std::move(sub));
            continue;
        }
        std::uint16_t ready_at = sub.has_value ? AvailableAt(sub.lhs) : 0;
        ready_at = std::max(ready_at, DeepestReferenceIntoThisChain(sub.steps, /*from_depth=*/1));
        chain.steps[ready_at].sub_chains.push_back(std::move(sub));
    }

    // ---- 3a. What each step must decode before it can filter ------------
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
    // conjunct attached to it is an equality binding **this relation's tuple key**
    // to a value already available. Anything else is a scan.
    //
    // Deliberately NOT TupleKeyEqualityTarget (the dispatcher's point-statement
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
                                    IsTupleKey(pred.lhs);
            const bool rhs_is_key = pred.rhs.kind == OperandKind::kColumn &&
                                    pred.rhs.column.up == 0 && pred.rhs.column.rel_slot == i &&
                                    IsTupleKey(pred.rhs.column);

            // The value the descent would be keyed on: whichever side is
            // not this step's tuple key.
            std::optional<Operand> candidate;
            if (lhs_is_key) {
                candidate = pred.rhs;
            } else if (rhs_is_key) {
                Operand from_lhs;
                from_lhs.kind = OperandKind::kColumn;
                from_lhs.column = pred.lhs;
                candidate = from_lhs;
            } else {
                continue;  // neither side is this relation's tuple key
            }

            if (candidate->kind == OperandKind::kLiteral) {
                // A negative literal cannot be a tuple key: ids are zero-extended
                // unsigned values (invariant 7), so this equality can never
                // hold. Left as a scan with the residual intact, which
                // returns the correct empty answer rather than probing an
                // enormous unsigned key.
                //
                // A kParam falls through this check like any other
                // non-integer literal (background: it was tuple-key-eligible here
                // until declared patterns were withdrawn, 2026-08-31).
                if (!TupleKeyBound(*candidate).has_value()) continue;
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
        // relation with a tuple key equality is already served better than either
        // of these could serve it.
        if (step.kind == AccessKind::kScan) {
            const auto key_range = TupleKeyRangeOf(step, static_cast<std::uint16_t>(i));
            if (key_range.has_value() && !key_range->strict) {
                step.kind = AccessKind::kRange;
                step.range = key_range->bounds;
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
            } else if (auto ep = ExprCabinProbeOf(*scope.relations[i].access, step,
                                                   static_cast<std::uint16_t>(i));
                       ep.has_value()) {
                // An expression Cabin at the column Cabin's rank (AQ-R5):
                // a compile-time key, before the correlated form.
                step.kind = AccessKind::kCabinProbe;
                step.cabin = std::move(ep);
            } else if (auto pp = PredicateCabinProbeOf(*scope.relations[i].access, step,
                                                        static_cast<std::uint16_t>(i));
                       pp.has_value()) {
                // A predicate Cabin, after the expression arm: AQ-R6.
                step.kind = AccessKind::kCabinProbe;
                step.cabin = std::move(pp);
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
            } else if (auto cv = CoverCabinProbeOf(*scope.relations[i].access, step,
                                                    static_cast<std::uint16_t>(i));
                       cv.has_value()) {
                // An expression Cabin's cover of a two-sided range (AQ-R8),
                // after the correlated arm (AQ-Q16): a join's inner step keyed
                // per outer row by a Cabin is served one set at a time, where
                // a cover would re-serve its whole union for every outer row.
                step.kind = AccessKind::kCabinProbe;
                step.cabin = std::move(cv);
            } else if (HasUnindexedEqualityFilter(*scope.relations[i].access, step,
                                                   static_cast<std::uint16_t>(i))) {
                // Ahead of the build annotation by decision, not
                // impossibility (spec §8): the step's own literal already
                // bounds what a walk visits, and v1 declines to also build
                // for it.
                step.kind = AccessKind::kFilterScan;
            } else if (inner_build && step.fn_residual.empty() && step.bool_residual.empty()) {
                // A step carrying a function conjunct takes no build
                // (AP-S4): the map holds the rows passing the
                // *non-correlated* conjuncts, and a function conjunct is
                // not split into the two - one reading an outer row's
                // column would bucket the first outer row's answer.
                //
                // The last ladder arm (workplan JB1): every arm above
                // declined, so a correlated equality still on the step is
                // the walked join. An annotation, never a kind - `kind`
                // stays kScan, and `BuildKey` (step_chain.hpp) owns the
                // contract.
                step.build = BuildKeyOf(scope, *scope.relations[i].access, step,
                                        static_cast<std::uint16_t>(i));
            }
            // A strict tuple key range, last (ruling (a), at
            // TupleKeyRangeOf): only a step still a bare walk takes it.
            if (step.kind == AccessKind::kScan && !step.build.has_value() &&
                key_range.has_value()) {
                step.kind = AccessKind::kRange;
                step.range = key_range->bounds;
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
    // ---- The post-fold consumers -----------------------------------------
    //
    // `HAVING` and an aggregated `ORDER BY` parse (HV-1) and are not built.
    // Refused here, positioned, and **never dropped**: a clause parsed and
    // silently ignored answers the client wrongly (parser-v2.md I11).
    //
    // Outside the `aggregated()` arm below, deliberately: a HAVING makes a
    // statement aggregated at parse, and a refusal reachable only through
    // that second fact would stop holding the day it changes.
    if (!stmt.having.empty()) {
        return Status::NotImplemented(
            "HAVING is not supported yet (byte " +
            std::to_string(stmt.having.front().agg.byte_offset) +
            "); filter before the fold with WHERE, or filter the result client-side");
    }
    if (stmt.aggregated() && !stmt.order_by.empty()) {
        return Status::NotImplemented(
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
        const catalog::SysColumnRow& projected = ColumnAt(scope, ref.value());
        chain.projection_types.push_back(projected.type_val);
        chain.projection_type_mods.push_back(
            catalog::TypeModOf(projected.type_val, projected.len));
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
            // A dropped column is not in `*` (BW-R6), nor a heap's hidden
            // tuple key (BZ-R3); `DescribeStar` skips the same columns, so
            // names and values stay aligned.
            if (!column.visible()) continue;
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
        // above.
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
    // One key, ascending, on the driving relation's tuple key names the order the
    // chain already emits (V09; I12 - written order across steps, tuple key order
    // within one), so the keys are dropped and the path costs nothing.
    //
    // The premise is that step 0 emits in tuple key order. A walk or range does
    // (page-wise `min_key`, which a division preserves); an index step does
    // because IX8a sorts its tuple keys back into that order deliberately; a
    // lookup or probe emits one row. **A leaf's slot order is its key
    // order** by placement (BD-R1,
    // `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`), which is
    // the premise all of that rests on. A Cabin probe stays excluded by
    // name: a served set is sorted by tuple key into the walk's order, but the
    // exclusion is a fix, not a precaution - the discarding version of this
    // clause answered `ORDER BY <tuple key>` over a Cabin-probed relation with
    // whatever order the entry set happened to hold - and lifting it is not
    // BB's to decide.
    const bool one_ascending_tuple_key =
        chain.sort_keys.size() == 1 && !chain.sort_keys[0].descending &&
        chain.sort_keys[0].ref.rel_slot == 0 && IsTupleKey(chain.sort_keys[0].ref);
    const bool driving_emits_tuple_key_order =
        !chain.steps.empty() && EmitsTupleKeyOrder(chain.steps[0].kind);
    if (one_ascending_tuple_key && driving_emits_tuple_key_order) {
        chain.sort_keys.clear();
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
    // this line is unchanged by it - the steps, kinds, residuals and class an aggregated statement
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
