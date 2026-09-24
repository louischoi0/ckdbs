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
        // 6, appended rather than inserted: these numbers are persisted in
        // sys.access_stats, so a value here may never change meaning and a
        // new kind takes the next free one however the enum is ordered.
        case AccessKind::kCabinProbe: return 6;
        // Appended, and the numbers are what `sys.access_stats` stores - so
        // this mapping is explicit rather than the enum's own values,
        // precisely so a kind can be inserted where it reads best without
        // silently re-labelling every row already on disk.
        case AccessKind::kIndexProbe: return 7;
        case AccessKind::kIndexRange: return 8;
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
        case 6: return AccessKind::kCabinProbe;
        case 7: return AccessKind::kIndexProbe;
        case 8: return AccessKind::kIndexRange;
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
        case parser::PredicateKind::kBetween: return "BETWEEN";
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
        // type_val 0: a plan shows the **compiled** form, and a date
        // literal is an epoch integer by the time a chain exists (TY05).
        // Rendering it back as a date would show something the chain does
        // not contain.
        return "'" + FormatValue(/*type_val=*/0, operand.literal) + "'";
    }
    return FormatValue(/*type_val=*/0, operand.literal);
}

std::string FormatPredicate(const StepPredicate& pred) {
    // `IS [NOT] NULL` is the whole predicate: its `rhs` is the kNull filler
    // the shared carrier needs (ast.hpp), never an operand, so printing it
    // would read `IS NULL NULL` - a plan describing a predicate nobody
    // wrote, on the surface whose whole job is showing what will run.
    const std::string head = FormatColumnRef(pred.lhs) + " " + CompareOpName(pred.op);
    if (pred.op == parser::CompareOp::kIsNull || pred.op == parser::CompareOp::kIsNotNull) {
        return head;
    }
    return head + " " + FormatOperand(pred.rhs);
}

std::string Indent(int depth) { return std::string(static_cast<std::size_t>(depth) * 2, ' '); }

void PrintSubChain(std::ostringstream& os, const SubChain& sub, int depth, bool hoisted);

void PrintStep(std::ostringstream& os, const Step& step, int depth) {
    os << Indent(depth) << "step " << step.step_id << ' ' << AccessKindName(step.kind) << ' '
       << (step.rel_name.empty() ? "oid=" + std::to_string(step.rel_oid) : step.rel_name);
    if (step.key.has_value()) os << " key=" << FormatOperand(*step.key);
    // A correlated index probe's key source (docs/spec/index.md §8a),
    // rendered from the probe's own field - the executor's single
    // authority; nothing is mirrored into `Step::key` for it.
    if (step.index.has_value() && step.index->key_from.has_value()) {
        os << " key=" << FormatColumnRef(*step.index->key_from);
    }
    if (step.range.has_value()) {
        os << " range=[" << step.range->low << ", " << step.range->high << ']';
    }
    if (step.cabin.has_value()) {
        os << " cabin=" << step.cabin->cabin_id << " on=col" << step.cabin->col_pos;
        // A correlated probe (cabin.md §4a) is keyed per outer row;
        // printing the default-constructed `value` for it would show a
        // key the probe never uses.
        if (step.cabin->key_from.has_value()) {
            os << " key=" << FormatColumnRef(*step.cabin->key_from);
        } else {
            os << " value=" << FormatValue(/*type_val=*/0, step.cabin->value);
        }
        // PO9: an optimizer-managed Cabin is marked, a declared one is
        // not - the reader needs to know when the structure serving a
        // probe is one the engine may drop on its own judgement.
        if (step.cabin->managed) os << " cabin_optimizer=true";
    }
    // The walked join's build annotation (docs/spec/join-inner-build.md,
    // workplan JB7), rendered in the Cabin line's own vocabulary because
    // it names the same two things: the own column the map is keyed on,
    // and the outer column each key is read from. **Visible before
    // execution** - the annotation is compile-time state, so a reader
    // sees which step *may* build without running the statement, the way
    // `derived` marks a conjunct nobody wrote. Whether it did build is
    // FormatStepStats' `inner_built`.
    if (step.build.has_value()) {
        os << " build on=col" << step.build->col_pos
           << " key=" << FormatColumnRef(step.build->key_from);
    }
    os << '\n';

    for (const StepPredicate& pred : step.residual) {
        // `derived` marks a conjunct equality propagation added
        // (docs/spec/parser-v2.md §5): without it, ANALYZE would print a filter
        // the reader cannot find in the statement they wrote.
        os << Indent(depth + 1) << "filter " << FormatPredicate(pred)
           << (pred.derived ? " derived" : "") << '\n';
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
        case AccessKind::kCabinProbe: return "CabinProbe";
        case AccessKind::kIndexProbe: return "IndexProbe";
        case AccessKind::kIndexRange: return "IndexRange";
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

    // ---- The fold (AG08) ------------------------------------------------
    //
    // **One line, after the steps and instead of the projection**, which is
    // where it sits in execution order: the chain produces rows and the
    // fold consumes them. Printing it beside a projection an aggregated
    // chain does not have would describe a statement that does not exist.
    //
    // A non-aggregated plan reaches none of this and its output is
    // byte-identical to what it was before aggregation existed - which is
    // the property AG08 is done when it has, and one the test checks
    // rather than assumes.
    if (chain.aggregated()) {
        const AggregateSpec& spec = *chain.aggregate;
        os << "aggregate keys=";
        if (spec.group_keys.empty()) {
            // Not "none": the global form is a different shape, not an
            // absent one, and a reader has to be able to tell "one output
            // row whatever the input" from "one per group".
            os << "(global)";
        } else {
            for (std::size_t i = 0; i < spec.group_keys.size(); ++i) {
                if (i > 0) os << ", ";
                os << FormatColumnRef(spec.group_keys[i]);
            }
        }

        os << " items=";
        for (std::size_t i = 0; i < spec.items.size(); ++i) {
            if (i > 0) os << ", ";
            os << (i < chain.column_names.size() ? chain.column_names[i] : std::string("?"));
            if (!spec.items[i].is_aggregate) {
                os << "=key" << FormatColumnRef(spec.items[i].ref);
                continue;
            }
            if (spec.items[i].star_arg) {
                os << "=*";
                continue;
            }
            // The flag as *stored*, not as written: MIN/MAX accept the word
            // and keep no set, so a plan that echoed the spelling would
            // claim work the fold does not do.
            os << '=' << (spec.items[i].distinct ? "distinct " : "")
               << FormatColumnRef(spec.items[i].ref);
        }
        return os.str();
    }

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

    // ---- The sort (OB6) --------------------------------------------------
    //
    // Between the projection and the quota, which is where it sits in
    // execution order, and absent from an unsorted plan for the reason the
    // quota line is absent from an unlimited one. A plan that prints no
    // `sort` for a statement that wrote `ORDER BY` is not a printer bug: it
    // is the compiler having elided a clause the chain already satisfies
    // (OB3), and seeing that is most of why this line is worth printing.
    if (chain.sorted()) {
        os << "\nsort ";
        for (std::size_t i = 0; i < chain.sort_keys.size(); ++i) {
            if (i > 0) os << ", ";
            os << FormatColumnRef(chain.sort_keys[i].ref)
               << (chain.sort_keys[i].descending ? " desc" : " asc");
        }
    }

    // ---- The quota (V09) ------------------------------------------------
    //
    // After the projection, which is where it sits in execution order: the
    // dispatcher's emission quota gates formatted rows. `offset` before
    // `limit`, since rows are skipped before they are counted. Absent from
    // an unlimited plan, whose output stays byte-identical to what it was
    // before the tail existed - the property the fold line above keeps,
    // kept again. (An aggregated chain never reaches here with one: the
    // parser refuses the tail over aggregated output.)
    if (chain.limit.has_value() || chain.offset != 0) {
        os << "\nquota";
        if (chain.offset != 0) os << " offset=" << chain.offset;
        if (chain.limit.has_value()) os << " limit=" << chain.limit.value();
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
            counters.trail_misses == 0 && counters.range_pages_pruned == 0 &&
            counters.cabin_hits == 0 && counters.cabin_misses == 0 &&
            counters.index_entries_scanned == 0) {
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
        // The physical optimizer's S2 currency (physical-optimizer.md
        // §II.2): pages this step read - exact for walks, one per keyed
        // access. Printed whenever the step examined anything, because
        // "how many pages did that cost" is the number the cost-benefit
        // model prices and an operator sanity-checks it here.
        if (counters.pages_fetched > 0) os << " pages=" << counters.pages_fetched;
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
        // Cabin (docs/spec/cabin.md §7). A hit means the step served an
        // observed value's entry set **authoritatively** and did not walk;
        // the entries/hint pair below says how much of that was the C6
        // location advice and how much needed a pk resolution.
        //
        // A hit beside an `examined` count the size of the relation is the
        // shape to look for: it would mean the set was not actually serving.
        if (counters.cabin_hits > 0) os << " cabin_hits=" << counters.cabin_hits;
        if (counters.cabin_misses > 0) os << " cabin_misses=" << counters.cabin_misses;
        if (counters.cabin_entries_served > 0) {
            os << " cabin_entries=" << counters.cabin_entries_served;
        }
        if (counters.cabin_hint_hits > 0) os << " hint_hits=" << counters.cabin_hint_hits;
        if (counters.cabin_hint_misses > 0) os << " hint_misses=" << counters.cabin_hint_misses;
        if (counters.cabin_recordings > 0) os << " cabin_recorded=" << counters.cabin_recordings;

        // The index's three numbers (docs/spec/index.md §7). The gap between
        // `index_scanned` and `index_resolved` is what the layer saved, and
        // `index_filtered` is the part of that gap a COVERING clause bought -
        // the only honest price for one, since a covered column saves a
        // base *descent* and never a base read.
        if (counters.index_entries_scanned > 0) {
            os << " index_scanned=" << counters.index_entries_scanned;
        }
        if (counters.index_entries_filtered > 0) {
            os << " index_filtered=" << counters.index_entries_filtered;
        }
        if (counters.index_rows_resolved > 0) {
            os << " index_resolved=" << counters.index_rows_resolved;
        }

        // The statement-local inner build (join-inner-build.md §4's
        // honesty clause). Printed for **every annotated step, including
        // `inner_built=0`**: a step carrying the annotation that did not
        // publish paid per-row walks for the statement, and that is
        // precisely what a reader chasing a slow join needs to see - the
        // other counters cannot distinguish it from a step that was never
        // eligible. Keyed off the compiled annotation rather than off a
        // non-zero counter, for that reason.
        //
        // `build_rows` is rows **bucketed**, not rows held: it is the
        // published map's size when `inner_built=1`, and how far a
        // discarded build got when it is 0 - a tripped cap, or a `LIMIT`
        // that stopped the first walk, both of which reset the map and
        // leave the count standing. `build_probes` is the outer rows
        // **served from** the map, k-1 for a k-row outer side because the
        // first row's walk *was* the build - which, with the `Probe` kind
        // printed on the plan line above, is why neither is spelled §4's
        // `probes=k` (workplan JB7 carries the argument).
        if (step != nullptr && step->build.has_value()) {
            os << " inner_built=" << counters.inner_builds;
            if (counters.build_rows > 0) os << " build_rows=" << counters.build_rows;
            if (counters.build_probes > 0) os << " build_probes=" << counters.build_probes;
        }
    }
    return os.str();
}

}  // namespace kds::exec
