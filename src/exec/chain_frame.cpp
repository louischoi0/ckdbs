#include "kds/exec/chain_frame.hpp"

#include "kds/exec/row_codec.hpp"

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

}  // namespace

StatusOr<bool> EvaluatePredicate(const catalog::Schema& lhs_schema, const StepPredicate& pred,
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

    const parser::AstValue& lhs = frame.Get(pred.lhs);
    const std::uint32_t type_val = lhs_schema.columns[pred.lhs.col_pos].type_val;

    if (pred.rhs.kind == OperandKind::kLiteral) {
        return CompareValues(type_val, lhs, pred.rhs.literal, pred.op);
    }
    if (!frame.CanResolve(pred.rhs.column)) {
        return Status::Corruption("a compiled predicate references a column the frame cannot "
                                  "resolve; the chain is malformed");
    }
    return CompareValues(type_val, lhs, frame.Get(pred.rhs.column), pred.op);
}

StatusOr<bool> EvaluateConjunct(const std::vector<const catalog::Schema*>& schemas,
                                const StepPredicate& pred, const ChainFrame& frame) {
    // A chain compiled from a declared pattern's body carries `$param`
    // placeholders and must never be executed - nothing binds them, so
    // any verdict here would answer a statement nobody wrote.
    //
    // Checked once, here, because every residual conjunct in the engine
    // is evaluated through this body; the other way a value reaches a
    // comparison is a probe key, guarded at KeyFromOperand(). Corruption
    // rather than a quiet false: this is reachable only through a
    // defect - it sits beside the other malformed-chain checks here -
    // and a false would turn that defect into a query that silently
    // returns nothing.
    if (pred.rhs.kind == OperandKind::kLiteral &&
        pred.rhs.literal.type == parser::ValueType::kParam) {
        return Status::Corruption(
            "a declared pattern's chain reached execution: parameter '$" +
            pred.rhs.literal.param_name() + "' has no bound value");
    }

    const catalog::Schema* lhs_schema = SchemaFor(schemas, pred.lhs);
    if (lhs_schema == nullptr) {
        // An outward lhs. The value is bound and comparable, but this
        // frame does not hold the schema that says how - so the
        // comparison falls back to the value kinds themselves, which
        // is what CompareValues does for every type but uint64.
        //
        // Reachable only from a correlated sub-chain whose predicate
        // is written `outer.col = inner.col`; the compiler emits the
        // inner column on the left for the common spelling.
        if (!frame.CanResolve(pred.lhs)) {
            return Status::Corruption("a compiled predicate references an unresolvable outer "
                                      "column; the chain is malformed");
        }
        const parser::AstValue& lhs = frame.Get(pred.lhs);
        if (pred.rhs.kind == OperandKind::kLiteral) {
            return CompareValues(/*type_val=*/0, lhs, pred.rhs.literal, pred.op);
        }
        if (!frame.CanResolve(pred.rhs.column)) {
            return Status::Corruption("a compiled predicate references an unresolvable "
                                      "column; the chain is malformed");
        }
        return CompareValues(/*type_val=*/0, lhs, frame.Get(pred.rhs.column), pred.op);
    }

    return EvaluatePredicate(*lhs_schema, pred, frame);
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

}  // namespace kds::exec
