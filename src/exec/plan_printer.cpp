#include "kds/exec/plan_printer.hpp"

#include "kds/catalog/rows.hpp"

#include <sstream>

#include "kds/exec/row_codec.hpp"

namespace kds::exec {

namespace {

bool StepsHaveReplayable(const std::vector<Step>& steps) noexcept {
    for (const Step& step : steps) {
        if (IsTrailReplayable(step.kind)) return true;
        for (const SubChain& sub : step.sub_chains) {
            if (StepsHaveReplayable(sub.steps)) return true;
        }
    }
    return false;
}

}  // namespace

bool HasReplayableStep(const StepChain& chain) noexcept {
    if (StepsHaveReplayable(chain.steps)) return true;
    for (const SubChain& sub : chain.hoisted) {
        if (StepsHaveReplayable(sub.steps)) return true;
    }
    return false;
}

std::uint8_t StoredAccessKind(AccessKind kind) noexcept {
    switch (kind) {
        case AccessKind::kLookup: return 1;
        case AccessKind::kProbe: return 2;
        case AccessKind::kRange: return 3;
        case AccessKind::kFilterScan: return 4;
        case AccessKind::kScan: return 5;
    }
    return catalog::kAccessKindUnset;
}

std::optional<AccessKind> AccessKindOfStored(std::uint8_t stored) noexcept {
    switch (stored) {
        case 1: return AccessKind::kLookup;
        case 2: return AccessKind::kProbe;
        case 3: return AccessKind::kRange;
        case 4: return AccessKind::kFilterScan;
        case 5: return AccessKind::kScan;
        default: return std::nullopt;
    }
}

std::uint8_t StoredStatementClass(StatementClass klass) noexcept {
    switch (klass) {
        case StatementClass::kPointSelect: return 1;
        case StatementClass::kRangeSelect: return 2;
        case StatementClass::kJoinSelect: return 3;
        case StatementClass::kUnclassified: break;
    }
    return catalog::kStmtClassUnclassified;
}

namespace {

// parser::CompareOpName, not a local copy: two renderings of one operator
// set would eventually disagree, and the one in a plan is the one a reader
// compares against the statement they typed.
using parser::CompareOpName;

const char* PredicateKindName(parser::PredicateKind kind) noexcept {
    switch (kind) {
        case parser::PredicateKind::kCompareValue: return "compare";
        case parser::PredicateKind::kCompareSubquery: return "scalar";
        case parser::PredicateKind::kInSubquery: return "IN";
        case parser::PredicateKind::kNotInSubquery: return "NOT IN";
        case parser::PredicateKind::kExists: return "EXISTS";
        case parser::PredicateKind::kNotExists: return "NOT EXISTS";
    }
    return "?";
}

// A column as the executor holds it. Structural on purpose - see the
// header: naming it would mean resolving against a catalog on a path that
// must not carry identifiers.
std::string FormatColumnRef(const ColumnRef& ref) {
    std::ostringstream os;
    os << ref.up << ':' << ref.rel_slot << '.' << ref.col_pos;
    return os.str();
}

std::string FormatOperand(const Operand& operand) {
    if (operand.kind == OperandKind::kColumn) return FormatColumnRef(operand.column);
    // Quoted when it is text, so `= 7` and `= '7'` are distinguishable -
    // they compare differently (row_codec.hpp's CompareValues), and a plan
    // that hid the difference would hide the reason for a wrong answer.
    if (operand.literal.type == parser::ValueType::kStr) {
        return "'" + FormatValue(operand.literal) + "'";
    }
    return FormatValue(operand.literal);
}

std::string FormatPredicate(const StepPredicate& pred) {
    return FormatColumnRef(pred.lhs) + " " + CompareOpName(pred.op) + " " + FormatOperand(pred.rhs);
}

std::string Indent(int depth) { return std::string(static_cast<std::size_t>(depth) * 2, ' '); }

void PrintSubChain(std::ostringstream& os, const SubChain& sub, int depth, bool hoisted);

void PrintStep(std::ostringstream& os, const Step& step, int depth) {
    os << Indent(depth) << "step " << step.step_id << ' ' << AccessKindName(step.kind) << ' '
       << (step.rel_name.empty() ? "oid=" + std::to_string(step.rel_oid) : step.rel_name);
    if (step.key.has_value()) os << " key=" << FormatOperand(*step.key);
    if (step.range.has_value()) {
        os << " range=[" << step.range->low << ", " << step.range->high << ']';
    }
    os << '\n';

    for (const StepPredicate& pred : step.residual) {
        os << Indent(depth + 1) << "filter " << FormatPredicate(pred) << '\n';
    }
    // Correlated by placement: a sub-chain attached to a step runs once per
    // row that step accepts, which is the fact worth seeing next to it.
    for (const SubChain& sub : step.sub_chains) {
        PrintSubChain(os, sub, depth + 1, /*hoisted=*/false);
    }
}

void PrintSubChain(std::ostringstream& os, const SubChain& sub, int depth, bool hoisted) {
    os << Indent(depth) << (hoisted ? "hoisted " : "correlated ") << PredicateKindName(sub.kind);
    if (sub.has_value) {
        os << ' ' << FormatColumnRef(sub.lhs);
        if (sub.kind == parser::PredicateKind::kCompareSubquery) {
            os << ' ' << CompareOpName(sub.op);
        }
        os << " <- " << FormatColumnRef(sub.value);
    }
    // Stated rather than implied by the indentation: `hoisted` is the
    // placement and `correlated` is the structural property, and an
    // uncorrelated sub-chain sitting under a step (which cannot happen
    // today) would be a compiler bug worth being able to see.
    os << (sub.correlated ? " [correlated]" : " [uncorrelated]") << '\n';

    for (const Step& step : sub.steps) PrintStep(os, step, depth + 1);
}

}  // namespace

const char* AccessKindName(AccessKind kind) noexcept {
    switch (kind) {
        case AccessKind::kLookup: return "Lookup";
        case AccessKind::kProbe: return "Probe";
        case AccessKind::kRange: return "Range";
        case AccessKind::kFilterScan: return "FilterScan";
        case AccessKind::kScan: return "Scan";
    }
    return "?";
}

const char* StatementClassName(StatementClass klass) noexcept {
    switch (klass) {
        case StatementClass::kPointSelect: return "PointSelect";
        case StatementClass::kRangeSelect: return "RangeSelect";
        case StatementClass::kJoinSelect: return "JoinSelect";
        case StatementClass::kUnclassified: return "Unclassified";
    }
    return "?";
}

std::string FormatPlan(const StepChain& chain) {
    std::ostringstream os;
    os << "class=" << StatementClassName(chain.klass) << " steps=" << chain.steps.size()
       << " hoisted=" << chain.hoisted.size() << '\n';

    // Hoisted first, because that is the execution order: an uncorrelated
    // sub-chain runs once *before* the outer chain opens, and a false one
    // answers the statement without opening the outer relation at all.
    for (const SubChain& sub : chain.hoisted) {
        PrintSubChain(os, sub, /*depth=*/0, /*hoisted=*/true);
    }
    for (const Step& step : chain.steps) PrintStep(os, step, /*depth=*/0);

    os << "project ";
    if (chain.star()) {
        os << "* (" << chain.column_names.size() << " column(s) of step 0)";
    } else {
        for (std::size_t i = 0; i < chain.projection.size(); ++i) {
            if (i > 0) os << ", ";
            os << (i < chain.column_names.size() ? chain.column_names[i] : std::string("?")) << '='
               << FormatColumnRef(chain.projection[i]);
        }
    }
    return os.str();
}

namespace {

// One line per step that recorded anything, in step_id order. Sub-chain
// steps appear here too, since step_id is global across the statement -
// which is exactly why the stats vector needs no parent linkage.
void CollectStepIds(const Step& step, std::vector<std::uint32_t>& out);

void CollectStepIds(const SubChain& sub, std::vector<std::uint32_t>& out) {
    for (const Step& step : sub.steps) CollectStepIds(step, out);
}

void CollectStepIds(const Step& step, std::vector<std::uint32_t>& out) {
    out.push_back(step.step_id);
    for (const SubChain& sub : step.sub_chains) CollectStepIds(sub, out);
}

const Step* FindStep(const StepChain& chain, std::uint32_t step_id);

const Step* FindStep(const SubChain& sub, std::uint32_t step_id) {
    for (const Step& step : sub.steps) {
        if (step.step_id == step_id) return &step;
        for (const SubChain& nested : step.sub_chains) {
            if (const Step* found = FindStep(nested, step_id)) return found;
        }
    }
    return nullptr;
}

const Step* FindStep(const StepChain& chain, std::uint32_t step_id) {
    for (const SubChain& sub : chain.hoisted) {
        if (const Step* found = FindStep(sub, step_id)) return found;
    }
    for (const Step& step : chain.steps) {
        if (step.step_id == step_id) return &step;
        for (const SubChain& sub : step.sub_chains) {
            if (const Step* found = FindStep(sub, step_id)) return found;
        }
    }
    return nullptr;
}

}  // namespace

std::string FormatStepStats(const StepChain& chain, const ExecStats& stats) {
    std::vector<std::uint32_t> ids;
    for (const SubChain& sub : chain.hoisted) CollectStepIds(sub, ids);
    for (const Step& step : chain.steps) CollectStepIds(step, ids);

    std::ostringstream os;
    bool first = true;
    for (std::uint32_t id : ids) {
        if (id >= stats.steps.size()) continue;
        const StepStats& counters = stats.steps[id];
        // A step that recorded nothing is omitted - see the header: for a
        // chain with sub-chains, "which steps ran at all" is usually the
        // question, and a row of zeros buries the answer.
        if (counters.relation_opens == 0 && counters.rows_examined == 0 &&
            counters.sub_chain_runs == 0 && counters.trail_replays == 0 &&
            counters.trail_misses == 0 && counters.range_pages_pruned == 0) {
            continue;
        }

        if (!first) os << '\n';
        first = false;

        const Step* step = FindStep(chain, id);
        os << "step " << id << ' '
           << (step != nullptr ? AccessKindName(step->kind) : "?") << ' '
           << (step != nullptr && !step->rel_name.empty() ? step->rel_name : std::string("?"))
           << " opens=" << counters.relation_opens << " examined=" << counters.rows_examined
           << " matched=" << counters.rows_matched;

        // Selectivity, spelled out rather than left as division. It is the
        // number this whole per-step split exists to make visible.
        if (counters.rows_examined > 0) {
            os << " sel=" << (counters.rows_matched * 100 / counters.rows_examined) << '%';
        }
        if (counters.sub_chain_runs > 0) os << " sub_runs=" << counters.sub_chain_runs;
        if (counters.correlated_scans > 0) os << " corr_scans=" << counters.correlated_scans;
        if (counters.probe_memo_hits > 0) os << " memo_hits=" << counters.probe_memo_hits;
        if (counters.spill_fetches > 0) os << " spills=" << counters.spill_fetches;
        // Waystone. `replays` is descents this step skipped because a
        // recorded location validated; `trail_misses` is consultations that
        // found an entry and fell through anyway. Both are printed only
        // when non-zero, and a step that shows `replays` where its kind is
        // a Scan would be a correctness bug, not a fast query - which is
        // exactly why the number is here to be looked at.
        if (counters.trail_replays > 0) os << " replays=" << counters.trail_replays;
        if (counters.trail_misses > 0) os << " trail_misses=" << counters.trail_misses;
        if (counters.range_pages_pruned > 0) os << " range_stopped_early=1";
    }
    return os.str();
}

}  // namespace kds::exec
