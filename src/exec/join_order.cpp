#include "kds/exec/join_order.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "kds/exec/transition.hpp"

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

// What an equality on `col_pos` keeps: `assumed`, raised to `1 / states`
// where a Transition closes the column's values (CC-Q11). An upper bound on
// the distinct count only ever raises a selectivity - for a literal and for
// a join key alike, since either side's average match is at least one
// row in `states`.
double EqualitySelectivity(const catalog::TableAccess* access, std::uint16_t col_pos,
                           double assumed) {
    if (access == nullptr) return assumed;
    const catalog::TransitionRef* t = access->TransitionOn(col_pos);
    if (t == nullptr) return assumed;
    return std::max(assumed, 1.0 / static_cast<double>(StateCount(*t)));
}

// The block-wide reads a step's estimate needs: which written position each
// executed slot holds, and each written position's relation and rows.
struct Block {
    const StepChain& chain;
    const CostInputs& in;

    std::size_t WrittenOf(std::size_t slot) const {
        return chain.steps[slot].step_id - in.base_step_id;
    }
    const catalog::TableAccess* AccessAt(std::size_t written) const {
        return written < in.written_access.size() ? in.written_access[written] : nullptr;
    }
    double RowsAt(std::size_t written) const {
        return written < in.written_rows.size() ? in.written_rows[written] : kAssumedRelationRows;
    }
    // The rows of the relation an earlier column belongs to - the constant
    // for an enclosing chain's, which a planned block never has.
    double RowsOfColumn(const ColumnRef& ref) const {
        if (ref.up > 0 || ref.rel_slot >= chain.steps.size()) return kAssumedRelationRows;
        return RowsAt(WrittenOf(ref.rel_slot));
    }
};

// What one step is estimated to do, per execution and once.
struct StepEstimate {
    double per_execution = 0;  // cost each time an outer row reaches the step
    double once = 0;           // cost paid on the first execution alone
    double matches = 0;        // rows it passes on per execution
};

StepEstimate Estimate(const Block& block, std::size_t slot) {
    const Step& step = block.chain.steps[slot];
    const CostInputs& in = block.in;
    const std::size_t written = block.WrittenOf(slot);
    const catalog::TableAccess* access = block.AccessAt(written);
    double relation_rows = block.RowsAt(written);

    // The step's own rows after its literal conjuncts, and how one outer
    // row's join-key equalities narrow them.
    double own = relation_rows;
    bool key_equality = false;  // tuple key = literal
    bool key_join = false;      // tuple key = an earlier column
    // Each other column equal to an earlier one, by what it keeps.
    std::vector<double> join_selectivity;

    // A column already equal to a literal keeps no fewer rows for also
    // equalling an earlier column: propagation makes the two conjuncts one
    // fact, and counting both would price the step as matching almost
    // nothing.
    std::vector<std::uint16_t> pinned;
    // Every own column bound by an equality to a literal or an earlier
    // column - what a unique index's bound asks (CC-Q11).
    std::vector<std::uint16_t> bound;
    for (const StepPredicate& pred : step.residual) {
        if (pred.op == parser::CompareOp::kEq && pred.rhs.kind == OperandKind::kLiteral &&
            IsOwn(pred.lhs, slot)) {
            pinned.push_back(pred.lhs.col_pos);
            bound.push_back(pred.lhs.col_pos);
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
            const ColumnRef* other = nullptr;
            if (IsOwn(pred.lhs, slot) && IsEarlier(pred.rhs.column, slot)) {
                own_side = &pred.lhs;
                other = &pred.rhs.column;
            } else if (IsOwn(pred.rhs.column, slot) && IsEarlier(pred.lhs, slot)) {
                own_side = &pred.rhs.column;
                other = &pred.lhs;
            }
            if (pred.op != parser::CompareOp::kEq || own_side == nullptr) {
                own *= kOtherSelectivity;
                continue;
            }
            bound.push_back(own_side->col_pos);
            if (is_pinned(own_side->col_pos)) {
                // the literal's selectivity already counts it
            } else if (own_side->col_pos == 0) {
                key_join = true;
            } else {
                // A non-key column against an earlier one keeps a row in the
                // key side's rows when the earlier column is its relation's
                // tuple key - the parent's - and otherwise one in the larger
                // relation's, the textbook 1 / max(distinct) bound with
                // distinct <= rows. Either way the join's size is the same
                // from both sides: a key join prices own / rows(own) the
                // other way round.
                const double other_rows = block.RowsOfColumn(*other);
                const double keeps = other->col_pos == 0
                                         ? 1.0 / other_rows
                                         : 1.0 / std::max(relation_rows, other_rows);
                join_selectivity.push_back(EqualitySelectivity(access, own_side->col_pos, keeps));
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
                continue;
            }
            own *= EqualitySelectivity(access, pred.lhs.col_pos, kEqualitySelectivity);
        } else if (IsRangeEnd(pred.op)) {
            own *= kRangeEndSelectivity;
        } else {
            own *= kOtherSelectivity;
        }
    }
    for (std::size_t i = 0; i < step.fn_residual.size() + step.bool_residual.size(); ++i) {
        own *= kOtherSelectivity;
    }

    double span = relation_rows;  // what a kRange walks
    if (step.kind == AccessKind::kRange && step.range.has_value()) {
        // The span's half-octave band (CC-Q12), dense keys assumed: exact
        // for issued keys before deletes.
        const double width = static_cast<double>(step.range->high - step.range->low) + 1.0;
        span = std::min(relation_rows, stats::BandCentre(stats::BandOf(width)));
        own *= span / relation_rows;
    }
    if (key_equality) own = std::min(own, 1.0);

    // The relation's observed rows under its own conjuncts, where a run had
    // it drive (card({w}), CC-S3): what a build would bucket, and a floor
    // under the relation's rows.
    const bool posterior_slot = in.posterior != nullptr && written < stats::kMaxPosteriorRelations;
    if (posterior_slot) {
        const std::size_t alone = std::size_t{1} << written;
        if ((in.posterior->present & (1u << alone)) != 0) {
            own = std::max(stats::BandCentre(in.posterior->band[alone]), 1.0);
            relation_rows = std::max(relation_rows, own);
        }
    }

    double matches = own;
    if (key_join) matches = std::min(own / relation_rows, 1.0);  // at most one row
    for (const double selectivity : join_selectivity) matches *= selectivity;

    // A unique index whose every key column is bound holds at most one row
    // per binding - never on its leading column alone (CC-Q11).
    if (access != nullptr) {
        const auto is_bound = [&bound](std::uint16_t col) {
            return std::find(bound.begin(), bound.end(), col) != bound.end();
        };
        for (const catalog::TableAccess::IndexRef& ix : access->indexes) {
            if (!ix.unique || ix.nkeys == 0) continue;
            if (!std::all_of(ix.keys().begin(), ix.keys().end(), is_bound)) continue;
            if (std::all_of(ix.keys().begin(), ix.keys().end(), is_pinned)) {
                own = std::min(own, 1.0);
            }
            matches = std::min(matches, 1.0);
            break;
        }
    }

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
        case AccessKind::kScan: {
            // A build over the cap is declined at its walk, and the step
            // walks per outer row. Where statistics price the block a build
            // is taken only under a quarter of the cap, and never where one
            // declined (CC-Q9).
            const double cap = static_cast<double>(in.limits.join_build_max_rows) /
                               (in.informed ? kBuildCapMargin : 1.0);
            const bool declined =
                posterior_slot && (in.posterior->declines & (1u << written)) != 0;
            if (step.build.has_value() && !declined && own <= cap) {
                // One walk buckets the rows passing the non-correlated
                // conjuncts; every execution probes the map.
                e.once = relation_rows + own * kBuildInsertCost;
                e.per_execution = kBuildProbeCost + matches;
            } else {
                e.per_execution = relation_rows;
            }
            break;
        }
    }
    return e;
}

}  // namespace

ChainEstimate EstimateChain(const StepChain& chain, const CostInputs& in) {
    const Block block{chain, in};
    double once = 0;     // paid once whatever the reply's length
    double per_row = 0;  // paid in proportion to the rows the driver feeds
    double rows = 1;     // rows reaching the next step; the driver runs once
    std::uint16_t mask = 0;  // the written positions of the slots so far
    if (in.step_rows != nullptr) in.step_rows->clear();
    for (std::size_t slot = 0; slot < chain.steps.size(); ++slot) {
        const std::size_t written = block.WrittenOf(slot);
        const StepEstimate e = Estimate(block, slot);
        once += e.once;
        per_row += rows * e.per_execution;
        rows *= e.matches;
        // The observed rows of the prefix's join, where a run measured it
        // (card(T), CC-S3): order-independent, so any order's run prices
        // this one's prefix. At least 1 (PS9).
        if (in.posterior != nullptr && written < stats::kMaxPosteriorRelations) {
            mask = static_cast<std::uint16_t>(mask | (1u << written));
            if ((in.posterior->present & (1u << mask)) != 0) {
                rows = std::max(stats::BandCentre(in.posterior->band[mask]), 1.0);
            }
        }
        if (in.step_rows != nullptr) in.step_rows->push_back(rows);
    }

    ChainEstimate out;
    out.rows = std::max(rows, 1.0);
    // In double: `LIMIT` and `OFFSET` each reach 2^64-1, so their sum may
    // wrap as integers.
    const std::optional<double> wanted =
        chain.limit.has_value()
            ? std::optional<double>(static_cast<double>(*chain.limit) +
                                    static_cast<double>(chain.offset))
            : std::nullopt;
    if (chain.sorted()) {
        // A sort under LIMIT is a top-N heap of `wanted` rows that cannot
        // refuse (`OutputSort`, sort.cpp); any other sort holds the reply.
        const bool bounded =
            wanted.has_value() && *wanted <= static_cast<double>(in.limits.sort_max_rows);
        if (!in.written_sorted && !bounded) {
            out.cost = std::numeric_limits<double>::infinity();
            return out;
        }
        const double held = bounded ? std::min(out.rows, *wanted) : out.rows;
        out.cost = once + per_row + out.rows * std::log2(std::max(held, 2.0));
        return out;
    }
    // Without a sort, LIMIT stops the walk once its rows are out: the per-row
    // part is paid for that fraction of the reply.
    const double fraction = wanted.has_value() ? std::min(1.0, *wanted / out.rows) : 1.0;
    out.cost = once + per_row * fraction;
    return out;
}

}  // namespace kds::exec
