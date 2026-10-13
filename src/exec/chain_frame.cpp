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

// CheckConjunct's in-frame half, with its messages: the left side resolves
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

// One conjunct's check: the `$param` refusal and the malformed-chain checks.
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

}  // namespace

namespace {

// A boolean conjunct's comparisons, each through CheckConjunct; a function
// leaf checks its own sides as it evaluates (`SideValue`).
Status CheckBoolConjunct(const std::vector<const catalog::Schema*>& schemas,
                         const BoolPredicate& pred, const ChainFrame& frame) {
    if (pred.op == BoolPredicate::Op::kCompare) return CheckConjunct(schemas, pred.compare, frame);
    for (const BoolPredicate& child : pred.children) {
        if (Status s = CheckBoolConjunct(schemas, child, frame); !s.ok()) return s;
    }
    return Status::OK();
}

}  // namespace

Status CheckConjuncts(const std::vector<const catalog::Schema*>& schemas, const Step& step,
                      const ChainFrame& frame) {
    for (const StepPredicate& pred : step.residual) {
        if (Status s = CheckConjunct(schemas, pred, frame); !s.ok()) return s;
    }
    for (const BoolPredicate& pred : step.bool_residual) {
        if (Status s = CheckBoolConjunct(schemas, pred, frame); !s.ok()) return s;
    }
    return Status::OK();
}

bool EvaluateConjunctChecked(const std::vector<const catalog::Schema*>& schemas,
                             const StepPredicate& pred, const ChainFrame& frame) {
    const catalog::Schema* lhs_schema = SchemaFor(schemas, pred.lhs);
    const std::uint32_t type_val =
        lhs_schema == nullptr ? 0 : lhs_schema->columns[pred.lhs.col_pos].type_val;
    return ComparePredicate(type_val, pred, frame);
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
        // AND: the first false ends it.
        if (!EvaluateConjunctChecked(schemas, pred, frame)) return false;
    }
    return true;
}

bool EvaluateAllExceptChecked(const std::vector<const catalog::Schema*>& schemas,
                              const std::vector<StepPredicate>& predicates,
                              const ChainFrame& frame, std::size_t skip) {
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        if (i == skip) continue;
        if (!EvaluateConjunctChecked(schemas, predicates[i], frame)) return false;
    }
    return true;
}

namespace {

// One side's value on the current row.
StatusOr<parser::AstValue> SideValue(const FunctionSide& side, const StatementContext& context,
                                     const ChainFrame& frame) {
    if (!side.call.has_value()) {
        if (side.plain.kind == OperandKind::kLiteral) {
            // CheckConjunct's `$param` refusal, for the same reason.
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

namespace {

bool IsNullTest(parser::CompareOp op) {
    return op == parser::CompareOp::kIsNull || op == parser::CompareOp::kIsNotNull;
}

TriState Known(bool matched) { return matched ? TriState::kTrue : TriState::kFalse; }

// A comparison inside a tree: unknown when an operand is NULL, which
// `CompareValues` collapses to false (BU-R4). Sound to ask after the fact:
// a relational operator is never true on a NULL operand, and `IS [NOT]
// NULL` is never unknown.
StatusOr<TriState> CompareTri(const std::vector<const catalog::Schema*>& schemas,
                              const StepPredicate& pred, const ChainFrame& frame) {
    const bool matched = EvaluateConjunctChecked(schemas, pred, frame);
    if (matched || IsNullTest(pred.op)) return Known(matched);
    const parser::AstValue& rhs =
        pred.rhs.kind == OperandKind::kLiteral ? pred.rhs.literal : frame.Get(pred.rhs.column);
    return IsNull(frame.Get(pred.lhs)) || IsNull(rhs) ? TriState::kUnknown : TriState::kFalse;
}

StatusOr<TriState> FunctionTri(const FunctionPredicate& pred, const ChainFrame& frame) {
    auto lhs = SideValue(pred.lhs, pred.context, frame);
    if (!lhs.ok()) return lhs.status();
    parser::AstValue rhs;
    if (!IsNullTest(pred.op)) {
        auto value = SideValue(pred.rhs, pred.context, frame);
        if (!value.ok()) return value.status();
        rhs = std::move(value.value());
        if (IsNull(lhs.value()) || IsNull(rhs)) return TriState::kUnknown;
    }
    return Known(CompareValues(pred.type_val, lhs.value(), rhs, pred.op));
}

// Kleene logic. `AND` stops at the first false and `OR` at the first true,
// which no unknown can overturn; an unknown is remembered and decides only
// when nothing decisive follows.
StatusOr<TriState> EvaluateTri(const std::vector<const catalog::Schema*>& schemas,
                          const BoolPredicate& pred, const ChainFrame& frame) {
    switch (pred.op) {
        case BoolPredicate::Op::kCompare: return CompareTri(schemas, pred.compare, frame);
        case BoolPredicate::Op::kFunction: return FunctionTri(*pred.fn, frame);
        case BoolPredicate::Op::kNot: {
            auto inner = EvaluateTri(schemas, pred.children.front(), frame);
            if (!inner.ok()) return inner;
            if (inner.value() == TriState::kUnknown) return TriState::kUnknown;
            return inner.value() == TriState::kTrue ? TriState::kFalse : TriState::kTrue;
        }
        case BoolPredicate::Op::kAnd:
        case BoolPredicate::Op::kOr: {
            const TriState decisive = pred.op == BoolPredicate::Op::kAnd ? TriState::kFalse : TriState::kTrue;
            bool unknown = false;
            for (const BoolPredicate& child : pred.children) {
                auto t = EvaluateTri(schemas, child, frame);
                if (!t.ok()) return t;
                if (t.value() == decisive) return decisive;
                unknown = unknown || t.value() == TriState::kUnknown;
            }
            if (unknown) return TriState::kUnknown;
            return decisive == TriState::kFalse ? TriState::kTrue : TriState::kFalse;
        }
    }
    return Status::Corruption("unhandled boolean conjunct node");
}

}  // namespace

StatusOr<bool> EvaluateBoolConjuncts(const std::vector<const catalog::Schema*>& schemas,
                                     const std::vector<BoolPredicate>& predicates,
                                     const ChainFrame& frame) {
    for (const BoolPredicate& pred : predicates) {
        auto t = EvaluateTri(schemas, pred, frame);
        if (!t.ok()) return t.status();
        if (!Collapse(t.value())) return false;  // at the root (BU-R4)
    }
    return true;
}

}  // namespace kds::exec
