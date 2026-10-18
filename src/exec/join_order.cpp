#include "kds/exec/join_order.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kds::exec {

namespace {

bool IsOwn(const ColumnRef& ref, std::size_t slot) {
    return ref.up == 0 && ref.rel_slot == slot;
}

// A reference this step can read before it runs: an earlier step's column,
// or an enclosing chain's.
bool IsEarlier(const ColumnRef& ref, std::size_t slot) {
    return ref.up > 0 || ref.rel_slot < slot;
}

bool IsRangeEnd(parser::CompareOp op) {
    return op == parser::CompareOp::kGt || op == parser::CompareOp::kGte ||
           op == parser::CompareOp::kLt || op == parser::CompareOp::kLte;
}

// What one step is estimated to do, per execution and once.
struct StepEstimate {
    double per_execution = 0;  // cost each time an outer row reaches the step
    double once = 0;           // cost paid on the first execution alone
    double matches = 0;        // rows it passes on per execution
};

StepEstimate Estimate(const Step& step, std::size_t slot, std::size_t build_cap) {
    // The step's own rows after its literal conjuncts, and how one outer
    // row's join-key equalities narrow them.
    double own = kAssumedRelationRows;
    bool key_equality = false;  // tuple key = literal
    bool key_join = false;      // tuple key = an earlier column
    std::size_t joins = 0;      // any other column = an earlier column

    // A column already equal to a literal keeps no fewer rows for also
    // equalling an earlier column: propagation makes the two conjuncts one
    // fact, and counting both would price the step as matching almost
    // nothing.
    std::vector<std::uint16_t> pinned;
    for (const StepPredicate& pred : step.residual) {
        if (pred.op == parser::CompareOp::kEq && pred.rhs.kind == OperandKind::kLiteral &&
            IsOwn(pred.lhs, slot)) {
            pinned.push_back(pred.lhs.col_pos);
        }
    }
    const auto is_pinned = [&pinned](std::uint16_t col) {
        return std::find(pinned.begin(), pinned.end(), col) != pinned.end();
    };

    for (const StepPredicate& pred : step.residual) {
        if (pred.rhs.kind == OperandKind::kColumn) {
            // A join key: this step's column equal to an earlier one, in
            // either orientation - placement puts a conjunct on the latest
            // step it reads, so the other side is always earlier.
            const ColumnRef* own_side = nullptr;
            if (IsOwn(pred.lhs, slot) && IsEarlier(pred.rhs.column, slot)) {
                own_side = &pred.lhs;
            } else if (IsOwn(pred.rhs.column, slot) && IsEarlier(pred.lhs, slot)) {
                own_side = &pred.rhs.column;
            }
            if (pred.op != parser::CompareOp::kEq || own_side == nullptr) {
                own *= kOtherSelectivity;
            } else if (is_pinned(own_side->col_pos)) {
                // the literal's selectivity already counts it
            } else if (own_side->col_pos == 0) {
                key_join = true;
            } else {
                ++joins;
            }
            continue;
        }
        if (!IsOwn(pred.lhs, slot)) {
            own *= kOtherSelectivity;  // unreachable by placement; costed, not trusted
            continue;
        }
        if (step.kind == AccessKind::kRange && pred.lhs.col_pos == 0 && IsRangeEnd(pred.op)) {
            continue;  // the walked span below is this bound's estimate
        }
        if (pred.op == parser::CompareOp::kEq) {
            if (pred.lhs.col_pos == 0) {
                key_equality = true;
            } else {
                own *= kEqualitySelectivity;
            }
        } else if (IsRangeEnd(pred.op)) {
            own *= kRangeEndSelectivity;
        } else {
            own *= kOtherSelectivity;
        }
    }
    for (std::size_t i = 0; i < step.fn_residual.size() + step.bool_residual.size(); ++i) {
        own *= kOtherSelectivity;
    }

    double span = kAssumedRelationRows;  // what a kRange walks
    if (step.kind == AccessKind::kRange && step.range.has_value()) {
        span = std::min(kAssumedRelationRows,
                        static_cast<double>(step.range->high - step.range->low) + 1.0);
        own *= span / kAssumedRelationRows;
    }
    if (key_equality) own = std::min(own, 1.0);

    double matches = own;
    if (key_join) matches = own / kAssumedRelationRows;  // at most one row
    for (std::size_t i = 0; i < joins; ++i) matches *= kJoinSelectivity;

    StepEstimate e;
    e.matches = matches;
    switch (step.kind) {
        case AccessKind::kLookup:
        case AccessKind::kProbe:
            e.per_execution = kDescentCost;
            break;
        case AccessKind::kRange:
            e.per_execution = kDescentCost + span;
            break;
        case AccessKind::kIndexProbe:
        case AccessKind::kIndexRange:
        case AccessKind::kCabinProbe:
            // Each entry resolved through the clustered tree.
            e.per_execution = kDescentCost + matches * (1.0 + kDescentCost);
            break;
        case AccessKind::kFilterScan:
        case AccessKind::kScan:
            // A build over the cap is declined at its walk, and the step
            // walks per outer row.
            if (step.build.has_value() && own <= static_cast<double>(build_cap)) {
                // One walk buckets the rows passing the non-correlated
                // conjuncts; every execution probes the map.
                e.once = kAssumedRelationRows + own * kBuildInsertCost;
                e.per_execution = kBuildProbeCost + matches;
            } else {
                e.per_execution = kAssumedRelationRows;
            }
            break;
    }
    return e;
}

}  // namespace

double ChainCost(const StepChain& chain, std::size_t build_cap) {
    double cost = 0;
    double rows = 1;  // rows reaching the next step; the driver runs once
    for (std::size_t slot = 0; slot < chain.steps.size(); ++slot) {
        const StepEstimate e = Estimate(chain.steps[slot], slot, build_cap);
        cost += e.once + rows * e.per_execution;
        rows *= e.matches;
    }
    return cost;
}

}  // namespace kds::exec
