#include "kds/exec/chain_frame.hpp"

#include "kds/exec/row_codec.hpp"

#include <array>
#include <span>

namespace kds::exec {

namespace {

// The schema a ColumnRef points into, walking the frame stack the same
// way ChainFrame::Get does. Needed because how two values compare depends
// on the column's declared type - a uint64 column compares through its
// digit text, not through int_val.
const catalog::Schema* SchemaFor(const std::vector<const catalog::Schema*>& schemas,
                                 const ColumnRef& ref) {
    // Only `up == 0` indexes this chain's schema list. An outward
    // reference belongs to an enclosing chain, whose schemas this frame
    // does not carry - and it does not need to: the value is already
    // decoded, and the *left* side's type decides the comparison.
    if (ref.up != 0) return nullptr;
    if (ref.rel_slot >= schemas.size()) return nullptr;
    return schemas[ref.rel_slot];
}

// EvaluatePredicate's checks, with its messages: the left side resolves
// and is a column of its relation, and a column right side resolves.
Status CheckPredicate(const catalog::Schema& lhs_schema, const StepPredicate& pred,
                      const ChainFrame& frame) {
    if (!frame.CanResolve(pred.lhs)) {
        return Status::Corruption("a compiled predicate references a column the frame cannot "
                                  "resolve; the chain is malformed");
    }
    if (pred.lhs.col_pos >= lhs_schema.columns.size()) {
        return Status::Corruption("a compiled predicate references column " +
                                  std::to_string(pred.lhs.col_pos) + " of a relation with " +
                                  std::to_string(lhs_schema.columns.size()) + " column(s)");
    }
    if (pred.rhs.kind != OperandKind::kLiteral && !frame.CanResolve(pred.rhs.column)) {
        return Status::Corruption("a compiled predicate references a column the frame cannot "
                                  "resolve; the chain is malformed");
    }
    return Status::OK();
}

// The comparison alone, on a predicate its check has passed. `type_val` is
// the left column's declared type, or 0 for an outward left side.
bool ComparePredicate(std::uint32_t type_val, const StepPredicate& pred,
                      const ChainFrame& frame) {
    const parser::AstValue& lhs = frame.Get(pred.lhs);
    const parser::AstValue& rhs =
        pred.rhs.kind == OperandKind::kLiteral ? pred.rhs.literal : frame.Get(pred.rhs.column);
    return CompareValues(type_val, lhs, rhs, pred.op);
}

// CheckConjunct's checks as a predicate, so a well-formed conjunct - every
// one a compiled chain holds - builds no Status (AP02); the message is
// worded only on failure, by CheckConjunct itself.
bool ConjunctWellFormed(const std::vector<const catalog::Schema*>& schemas,
                        const StepPredicate& pred, const ChainFrame& frame) {
    if (pred.rhs.kind == OperandKind::kLiteral &&
        pred.rhs.literal.type == parser::ValueType::kParam) {
        return false;
    }
    if (!frame.CanResolve(pred.lhs)) return false;
    const catalog::Schema* lhs_schema = SchemaFor(schemas, pred.lhs);
    if (lhs_schema != nullptr && pred.lhs.col_pos >= lhs_schema->columns.size()) return false;
    return pred.rhs.kind == OperandKind::kLiteral || frame.CanResolve(pred.rhs.column);
}

// One conjunct's comparison, on a conjunct `CheckConjunct` has passed.
bool CompareConjunct(const std::vector<const catalog::Schema*>& schemas,
                     const StepPredicate& pred, const ChainFrame& frame) {
    const catalog::Schema* lhs_schema = SchemaFor(schemas, pred.lhs);
    const std::uint32_t type_val =
        lhs_schema == nullptr ? 0 : lhs_schema->columns[pred.lhs.col_pos].type_val;
    return ComparePredicate(type_val, pred, frame);
}

}  // namespace

StatusOr<bool> EvaluatePredicate(const catalog::Schema& lhs_schema, const StepPredicate& pred,
                                 const ChainFrame& frame) {
    if (Status s = CheckPredicate(lhs_schema, pred, frame); !s.ok()) return s;
    return ComparePredicate(lhs_schema.columns[pred.lhs.col_pos].type_val, pred, frame);
}

Status CheckConjunct(const std::vector<const catalog::Schema*>& schemas,
                     const StepPredicate& pred, const ChainFrame& frame) {
    // A `$param` is a value position with nothing bound to it. Nothing
    // constructs one since declared patterns were withdrawn (ast.hpp), so
    // this arm is unreachable; it stays because the enumerator does, and
    // because what it prevents is a wrong answer rather than an error.
    //
    // Checked here, because every residual conjunct in the engine is
    // checked through this body; the other way a value reaches a
    // comparison is a probe key, guarded at KeyFromOperand(). Corruption
    // rather than a quiet false: it sits beside the other malformed-chain
    // checks here, and a false would turn the defect that produced it into
    // a query that silently returns nothing.
    if (pred.rhs.kind == OperandKind::kLiteral &&
        pred.rhs.literal.type == parser::ValueType::kParam) {
        return Status::Corruption(
            "a declared pattern's chain reached execution: parameter '$" +
            pred.rhs.literal.param_name() + "' has no bound value");
    }

    const catalog::Schema* lhs_schema = SchemaFor(schemas, pred.lhs);
    if (lhs_schema != nullptr) return CheckPredicate(*lhs_schema, pred, frame);

    // An outward lhs. The value is bound and comparable, but this frame
    // does not hold the schema that says how - so the comparison falls
    // back to the value kinds themselves, which is what CompareValues does
    // for every type but uint64.
    //
    // Reachable only from a correlated sub-chain whose predicate is
    // written `outer.col = inner.col`; the compiler emits the inner column
    // on the left for the common spelling.
    if (!frame.CanResolve(pred.lhs)) {
        return Status::Corruption("a compiled predicate references an unresolvable outer "
                                  "column; the chain is malformed");
    }
    if (pred.rhs.kind != OperandKind::kLiteral && !frame.CanResolve(pred.rhs.column)) {
        return Status::Corruption("a compiled predicate references an unresolvable "
                                  "column; the chain is malformed");
    }
    return Status::OK();
}

StatusOr<bool> EvaluateConjunct(const std::vector<const catalog::Schema*>& schemas,
                                const StepPredicate& pred, const ChainFrame& frame) {
    if (!ConjunctWellFormed(schemas, pred, frame)) {
        if (Status s = CheckConjunct(schemas, pred, frame); !s.ok()) return s;
    }
    return CompareConjunct(schemas, pred, frame);
}

StatusOr<bool> EvaluateAll(const std::vector<const catalog::Schema*>& schemas,
                           const std::vector<StepPredicate>& predicates, const ChainFrame& frame) {
    for (const StepPredicate& pred : predicates) {
        auto matched = EvaluateConjunct(schemas, pred, frame);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return false;  // AND: the first false ends it
    }
    return true;
}

ScanKernel CompileScanKernel(const catalog::Schema& schema, const catalog::RowLayout& layout,
                             const std::vector<StepPredicate>& residual, std::uint16_t rel_slot) {
    ScanKernel kernel;
    for (const StepPredicate& pred : residual) {
        if (pred.lhs.up != 0 || pred.lhs.rel_slot != rel_slot) continue;
        if (pred.rhs.kind != OperandKind::kLiteral) continue;
        if (pred.rhs.literal.type == parser::ValueType::kParam) continue;
        auto term = CompileScanKernelTerm(schema, layout, pred.lhs.col_pos, pred.op,
                                          pred.rhs.literal);
        if (term.has_value()) kernel.push_back(std::move(*term));
    }
    return kernel;
}

bool EvaluateAllChecked(const std::vector<const catalog::Schema*>& schemas,
                        const std::vector<StepPredicate>& predicates, const ChainFrame& frame) {
    for (const StepPredicate& pred : predicates) {
        if (!CompareConjunct(schemas, pred, frame)) return false;  // AND: the first false ends it
    }
    return true;
}

StatusOr<bool> EvaluateAllExcept(const std::vector<const catalog::Schema*>& schemas,
                                 const std::vector<StepPredicate>& predicates,
                                 const ChainFrame& frame, std::size_t skip) {
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        if (i == skip) continue;
        auto matched = EvaluateConjunct(schemas, predicates[i], frame);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return false;
    }
    return true;
}

namespace {

// One side's value on the current row.
StatusOr<parser::AstValue> SideValue(const FunctionSide& side, const StatementContext& context,
                                     const ChainFrame& frame) {
    if (!side.call.has_value()) {
        if (side.plain.kind == OperandKind::kLiteral) {
            // EvaluateConjunct's `$param` refusal, for the same reason.
            if (side.plain.literal.type == parser::ValueType::kParam) {
                return Status::Corruption("a parameter reached execution with no bound value");
            }
            return side.plain.literal;
        }
        if (!frame.CanResolve(side.plain.column)) {
            return Status::Corruption("a function conjunct references a column the frame "
                                      "cannot resolve; the chain is malformed");
        }
        return frame.Get(side.plain.column);
    }
    // Pointers into the frame, on the stack: this runs per filtered row, and
    // every entry's arity is held to kMaxFunctionArgs (functions.hpp).
    const FunctionTerm& call = *side.call;
    std::array<const parser::AstValue*, kMaxFunctionArgs> args{};
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        if (!frame.CanResolve(call.args[i])) {
            return Status::Corruption("a function argument references a column the frame "
                                      "cannot resolve; the chain is malformed");
        }
        const parser::AstValue& arg = frame.Get(call.args[i]);
        if (arg.type == parser::ValueType::kNull) return parser::AstValue{};  // NULL in, NULL out
        args[i] = &arg;
    }
    return call.fn->evaluate(
        std::span<const parser::AstValue* const>(args.data(), call.args.size()), context);
}

}  // namespace

StatusOr<bool> EvaluateFunctionConjuncts(const std::vector<FunctionPredicate>& predicates,
                                         const ChainFrame& frame) {
    for (const FunctionPredicate& pred : predicates) {
        auto lhs = SideValue(pred.lhs, pred.context, frame);
        if (!lhs.ok()) return lhs.status();
        parser::AstValue rhs;
        if (pred.op != parser::CompareOp::kIsNull && pred.op != parser::CompareOp::kIsNotNull) {
            auto value = SideValue(pred.rhs, pred.context, frame);
            if (!value.ok()) return value.status();
            rhs = std::move(value.value());
        }
        if (!CompareValues(pred.type_val, lhs.value(), rhs, pred.op)) return false;
    }
    return true;
}

}  // namespace kds::exec
