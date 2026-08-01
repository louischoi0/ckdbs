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

StatusOr<bool> EvaluateAll(const std::vector<const catalog::Schema*>& schemas,
                           const std::vector<StepPredicate>& predicates, const ChainFrame& frame) {
    for (const StepPredicate& pred : predicates) {
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
            bool matched;
            if (pred.rhs.kind == OperandKind::kLiteral) {
                matched = CompareValues(/*type_val=*/0, lhs, pred.rhs.literal, pred.op);
            } else {
                if (!frame.CanResolve(pred.rhs.column)) {
                    return Status::Corruption("a compiled predicate references an unresolvable "
                                              "column; the chain is malformed");
                }
                matched = CompareValues(/*type_val=*/0, lhs, frame.Get(pred.rhs.column), pred.op);
            }
            if (!matched) return false;
            continue;
        }

        auto matched = EvaluatePredicate(*lhs_schema, pred, frame);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return false;  // AND: the first false ends it
    }
    return true;
}

}  // namespace kds::exec
