#include "kds/exec/step_vm.hpp"

#include <string>

#include "kds/exec/row_codec.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"

namespace kds::exec {

namespace {

// ---- R1: the page-span guard --------------------------------------------
//
// A relation walk hands the visitor a span into a live page frame. If the
// visitor descends into the next step while still holding it, a page fetch
// happens with that span registered - and nothing pins the frame it points
// into. Today no page store evicts, so the span stays valid by accident;
// the moment one does, this becomes a use-after-free that reads as
// occasional wrong rows.
//
// So the rule is enforced now, while it is free: a step registers its span,
// decodes, and releases before descending. Any fetch made while a span is
// registered trips the guard.
//
// Core-local rather than atomic: the engine is thread-per-core and a chain
// executes on one core (docs/rules.md §3). An atomic here would suggest a
// cross-core protocol that does not exist.
thread_local int g_live_spans = 0;
thread_local bool g_guard_tripped = false;

class PageSpanGuard {
public:
    PageSpanGuard() { ++g_live_spans; }
    ~PageSpanGuard() { Release(); }

    PageSpanGuard(const PageSpanGuard&) = delete;
    PageSpanGuard& operator=(const PageSpanGuard&) = delete;

    // Called explicitly once the row is decoded and the span is finished
    // with. The destructor is the backstop, not the mechanism - releasing
    // by hand is what makes "decode before descending" visible at the
    // point it happens.
    void Release() noexcept {
        if (!released_) {
            --g_live_spans;
            released_ = true;
        }
    }

private:
    bool released_ = false;
};

// Every page fetch the VM makes goes through here, so the guard sees all
// of them.
void NoteFetch() {
    if (g_live_spans > 0) g_guard_tripped = true;
}

// ---- Bounds --------------------------------------------------------------

// R3's execute-time half. The parser and compiler both cap nesting; this
// is the third, because a bound only one layer enforces is not a bound.
constexpr std::uint32_t kMaxExecDepth = 8;

struct Bound {
    const catalog::TableAccess* access = nullptr;
};

// ---- Three-valued logic, collapsed in exactly one place (spec I16) -------
//
// `NOT IN` is not `!IN`. Under SQL's standard semantics, if the subquery
// result contains a NULL and the probe matches nothing, `x NOT IN (S)` is
// UNKNOWN rather than TRUE - so a boolean negation is silently wrong the
// day NULLs become storable.
//
// The engine is two-valued today: the row codec rejects NULL, so the only
// reachable NULL is an inline literal or a zero-row scalar subquery. That
// makes the tri-state free right now, which is the point of paying for it
// now: when the null bitmap lands, `Collapse` is the one function that
// changes, and no wrong answer shipped in between.
enum class TriState : std::uint8_t { kFalse, kTrue, kUnknown };

// **The single collapse point.** UNKNOWN becomes false at the conjunct -
// one function, and every caller below routes through it.
bool Collapse(TriState value) noexcept { return value == TriState::kTrue; }

bool IsNull(const parser::AstValue& value) noexcept {
    return value.type == parser::ValueType::kNull;
}

StatusOr<std::uint64_t> KeyFromOperand(const Operand& operand, const ChainFrame& frame) {
    const parser::AstValue* value = nullptr;
    if (operand.kind == OperandKind::kLiteral) {
        value = &operand.literal;
    } else {
        if (!frame.CanResolve(operand.column)) {
            return Status::Corruption("a probe key references a column the frame cannot resolve");
        }
        value = &frame.Get(operand.column);
    }
    if (value->type != parser::ValueType::kInt || value->int_val < 0) {
        // Not a pk-shaped value. Ids are zero-extended 40-bit (invariant
        // 7), so this cannot match anything - reported so the caller can
        // treat it as a miss rather than probing a wrapped key.
        return Status::NotFound("probe key is not a non-negative integer");
    }
    return static_cast<std::uint64_t>(value->int_val);
}

// One chain's execution state. Recreated per sub-chain, which is what
// makes the frame stack a stack.
class ChainRunner {
public:
    ChainRunner(catalog::Catalog& catalog, storage::PageStore& store, const RowSink& sink,
                std::uint32_t depth, const ChainFrame* parent, ExecStats& stats, Budget& budget)
        : catalog_(catalog), store_(store), sink_(sink), depth_(depth), parent_(parent),
          stats_(stats), budget_(budget) {}

    Status Run(const std::vector<Step>& steps) {
        if (depth_ > kMaxExecDepth) {
            return Status::Unsupported("chain nesting deeper than " +
                                        std::to_string(kMaxExecDepth) + " at execute");
        }
        if (Status s = Bind(steps); !s.ok()) return s;
        frame_.Open(schemas_, parent_);
        stopped_ = false;
        return RunStep(steps, 0);
    }

private:
    Status Bind(const std::vector<Step>& steps) {
        bound_.clear();
        schemas_.clear();
        for (const Step& step : steps) {
            auto access = catalog_.InitTableAccess(step.rel_oid);
            if (!access.ok()) return access.status();
            bound_.push_back(Bound{access.value()});
            schemas_.push_back(&access.value()->schema);
        }
        return Status::OK();
    }

public:
    // Evaluates one sub-chain against the *current* frame, which becomes
    // the parent of the nested one. Correlation values are read through
    // that link and **never written into the AST** - the AST is shared
    // (`shared_ptr<SelectStmt>`) and mutating it per outer row would make
    // a statement's meaning depend on how far execution had got.
    StatusOr<TriState> EvaluateSubChain(const SubChain& sub, const ChainFrame& outer) {
        ++stats_.sub_chain_runs;
        // The shape that makes a statement quadratic: a sub-chain whose
        // first step walks its relation, evaluated once per outer row.
        // Counted rather than refused - it is legitimate over small
        // relations, and the budget is what bounds it over large ones.
        if (!sub.steps.empty() && sub.steps[0].kind == AccessKind::kScan) {
            ++stats_.correlated_scans;
        }

        const bool wants_value = sub.has_value;
        const parser::AstValue* probe = nullptr;
        if (wants_value) {
            if (!outer.CanResolve(sub.lhs)) {
                return Status::Corruption("a sub-chain's outer column is unresolvable");
            }
            probe = &outer.Get(sub.lhs);
        }

        // What the walk over the inner rows accumulates.
        bool saw_row = false;
        bool saw_match = false;
        bool saw_null = false;
        int scalar_rows = 0;
        parser::AstValue scalar_value;

        const RowSink collect =
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                saw_row = true;

                switch (sub.kind) {
                    case parser::PredicateKind::kExists:
                        // Short-circuit: one qualifying row is the whole
                        // answer, and V03 made stopping expressible.
                        return storage::VisitControl::kStop;

                    case parser::PredicateKind::kNotExists:
                        // Same walk, negated - and it stops for the same
                        // reason: one row settles it.
                        return storage::VisitControl::kStop;

                    case parser::PredicateKind::kInSubquery:
                    case parser::PredicateKind::kNotInSubquery: {
                        if (!frame.CanResolve(sub.value)) {
                            return Status::Corruption("a sub-chain's value column is "
                                                      "unresolvable");
                        }
                        const parser::AstValue& candidate = frame.Get(sub.value);
                        if (IsNull(candidate)) {
                            // The dangerous half of NOT IN: a NULL in the
                            // result makes "matched nothing" UNKNOWN, not
                            // TRUE. Recorded and carried out; the walk
                            // continues, since a later row may still match
                            // outright and settle it either way.
                            saw_null = true;
                            return storage::VisitControl::kContinue;
                        }
                        if (probe != nullptr && !IsNull(*probe) &&
                            CompareValues(/*type_val=*/0, *probe, candidate,
                                          parser::CompareOp::kEq)) {
                            saw_match = true;
                            return storage::VisitControl::kStop;
                        }
                        return storage::VisitControl::kContinue;
                    }

                    case parser::PredicateKind::kCompareSubquery: {
                        if (!frame.CanResolve(sub.value)) {
                            return Status::Corruption("a sub-chain's value column is "
                                                      "unresolvable");
                        }
                        if (scalar_rows == 0) scalar_value = frame.Get(sub.value);
                        ++scalar_rows;
                        // Two rows is already a violation, so there is no
                        // reason to read a third.
                        return scalar_rows >= 2 ? storage::VisitControl::kStop
                                                : storage::VisitControl::kContinue;
                    }

                    case parser::PredicateKind::kCompareValue:
                        return Status::Corruption("a plain comparison is not a sub-chain");
                }
                return storage::VisitControl::kContinue;
            };

        ChainRunner inner(catalog_, store_, collect, depth_ + 1, &outer, stats_, budget_);
        Status ran = inner.Run(sub.steps);
        if (!ran.ok()) return ran;

        switch (sub.kind) {
            case parser::PredicateKind::kExists:
                return saw_row ? TriState::kTrue : TriState::kFalse;

            case parser::PredicateKind::kNotExists:
                // Absence has no witness, which is why this step kind is
                // never trail-replayable - but it is perfectly computable.
                return saw_row ? TriState::kFalse : TriState::kTrue;

            case parser::PredicateKind::kInSubquery:
                if (saw_match) return TriState::kTrue;
                return saw_null ? TriState::kUnknown : TriState::kFalse;

            case parser::PredicateKind::kNotInSubquery:
                // NOT the boolean negation of the above: a match is FALSE,
                // but "no match with a NULL present" is UNKNOWN rather
                // than TRUE.
                if (saw_match) return TriState::kFalse;
                return saw_null ? TriState::kUnknown : TriState::kTrue;

            case parser::PredicateKind::kCompareSubquery: {
                if (scalar_rows > 1) {
                    return Status::CardinalityViolation(
                        "a scalar subquery returned more than one row; parse time cannot prove "
                        "cardinality, so this is checked per execution");
                }
                if (scalar_rows == 0) {
                    // Zero rows is NULL, and a comparison against NULL is
                    // not true. Picking a first row instead would make the
                    // answer depend on physical order.
                    return TriState::kUnknown;
                }
                if (probe == nullptr || IsNull(*probe) || IsNull(scalar_value)) {
                    return TriState::kUnknown;
                }
                return CompareValues(/*type_val=*/0, *probe, scalar_value, sub.op)
                           ? TriState::kTrue
                           : TriState::kFalse;
            }

            case parser::PredicateKind::kCompareValue:
                break;
        }
        return Status::Corruption("unhandled sub-chain kind");
    }

private:

    // Emits, or descends. The recursion is over *steps*, not over rows:
    // one frame holds every bound relation, so reaching the end means one
    // complete output row is sitting in it.
    Status RunStep(const std::vector<Step>& steps, std::size_t index) {
        if (stopped_) return Status::OK();
        if (index == steps.size()) {
            auto outcome = sink_(frame_);
            if (!outcome.ok()) return outcome.status();
            if (!outcome.has_value()) {
                return Status::InvalidArgument("a row sink returned an ok Status with no "
                                               "VisitControl");
            }
            if (outcome.value() == storage::VisitControl::kStop) stopped_ = true;
            return Status::OK();
        }

        const Step& step = steps[index];
        const catalog::TableAccess& access = *bound_[index].access;

        ++stats_.relation_opens;
        if (step.kind == AccessKind::kLookup || step.kind == AccessKind::kProbe) {
            return RunPointStep(steps, index, step, access);
        }
        return RunWalkStep(steps, index, step, access);
    }

    // A pk descent. Its answer is authoritative on a btree relation - a
    // miss means the row does not exist - and a heap relation has no pk
    // index, so it falls through to the same walk a Scan would do, with
    // the key kept as a residual. That fall-through is safe *because* the
    // compiler also left the key in `residual`: filtering on the residual
    // list alone gives the same rows.
    Status RunPointStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        auto key = KeyFromOperand(*step.key, frame_);
        if (!key.ok()) {
            // A key that cannot match: no row, and not an error.
            if (key.status().code() == StatusCode::kNotFound) return Status::OK();
            return key.status();
        }

        if (access.clustered_type == catalog::ClusteredType::kBtree) {
            // The memo, checked before the descent. It holds a *location*,
            // not a row, so a hit re-reads and re-filters exactly what a
            // fresh descent would have handed to the same code - which is
            // what makes "results identical with the memo on and off" a
            // property of the structure rather than of the test data.
            if (memo_valid_ && memo_key_ == key.value()) {
                ++stats_.probe_memo_hits;
                NoteFetch();
                auto bytes = store_.GetForRead(memo_page_);
                if (bytes.ok()) {
                    heap::PageView page(bytes.value());
                    return AcceptTupleAt(steps, index, step, access, page, memo_slot_);
                }
                // The page went away. Fall through to the descent rather
                // than fail: the memo is an accelerator, never an oracle.
                memo_valid_ = false;
            }

            NoteFetch();
            auto found = btree::BtreeLookup(store_, access.desc_page_id, key.value());
            if (found.ok()) {
                memo_valid_ = true;
                memo_key_ = key.value();
                memo_page_ = found.value().page_id;
                memo_slot_ = found.value().slot;
                // The descent carries its leaf out, so the row is read
                // without asking the store for a page the lookup just had
                // in hand. The span is dynamic-extent (it has to have an
                // unset state) and always either empty or exactly a page.
                if (found.value().leaf.size() != kPageSize) {
                    return Status::Corruption("a descent returned a leaf that is not one page");
                }
                std::span<std::byte, kPageSize> fixed(found.value().leaf.data(), kPageSize);
                heap::PageView leaf(fixed);
                return AcceptTupleAt(steps, index, step, access, leaf, found.value().slot);
            }
            if (found.status().code() == StatusCode::kNotFound) return Status::OK();
            return found.status();
        }

        // Heap: no index to descend. The walk below is the authoritative
        // path, and the residual carries the key.
        return RunWalkStep(steps, index, step, access);
    }

    Status RunWalkStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                       const catalog::TableAccess& access) {
        Status inner = Status::OK();

        auto visitor = [&](PageId, heap::PageView& page,
                           std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (stopped_) return storage::VisitControl::kStop;

            auto accepted = AcceptTupleAt(steps, index, step, access, page, slot);
            if (!accepted.ok()) {
                inner = accepted;
                return accepted;
            }
            return stopped_ ? storage::VisitControl::kStop : storage::VisitControl::kContinue;
        };

        NoteFetch();
        Status walked = access.clustered_type == catalog::ClusteredType::kBtree
                            ? btree::BtreeVisit(store_, access.desc_page_id,
                                                storage::PageAccess::kRead, visitor)
                            : heap::ChainVisit(store_, access.desc_page_id,
                                               storage::PageAccess::kRead, visitor);
        if (!inner.ok()) return inner;
        return walked;
    }

    // Decodes one tuple into this step's frame slots, evaluates the
    // predicates attached to the step, and descends if they hold.
    //
    // **This is where R1 lives.** The span handed in by the walk is
    // registered, used for exactly one decode, and released before
    // anything else can fetch a page.
    Status AcceptTupleAt(const std::vector<Step>& steps, std::size_t index, const Step& step,
                         const catalog::TableAccess& access, heap::PageView& page,
                         std::uint16_t slot) {
        bool decoded = false;
        // Filled by the decode, drained after the span is released. A
        // spilled value lives in the var-heap and fetching it is a page
        // fetch, so it is exactly the thing that must not happen while the
        // span below is live (row_codec.hpp). Reused across rows so a scan
        // that never spills allocates nothing extra.
        spills_.clear();
        {
            PageSpanGuard span;
            auto tuple = page.ReadTuple(slot);
            if (tuple.ok()) {
                Status s = DecodeRowInto(access.schema, access.layout, tuple.value().payload,
                                         frame_.SlotsFor(static_cast<std::uint16_t>(index)),
                                         &spills_);
                if (!s.ok()) {
                    span.Release();
                    return s;
                }
                decoded = true;
            }
            // Released here, explicitly, while the tuple bytes are still
            // in scope but finished with. Everything below may fetch.
            span.Release();
        }
        if (!decoded) return Status::OK();  // dead or out-of-range slot

        // Now that nothing is live, the spilled values can be resolved.
        if (!spills_.empty()) {
            if (Status s = ResolveSpills(store_, spills_,
                                          frame_.SlotsFor(static_cast<std::uint16_t>(index)));
                !s.ok()) {
                return s;
            }
        }

        ++stats_.rows_examined;
        // Charged where the tuple was actually decoded, which is the unit
        // that tracks work: a page fetch amortizes over its tuples, but
        // every tuple is decoded and filtered on its own.
        if (Status s = budget_.ChargeRow(); !s.ok()) return s;

        auto matched = EvaluateAll(schemas_, step.residual, frame_);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return Status::OK();

        // Correlated sub-chains attached to this step, evaluated only
        // once the ordinary predicates have accepted the row - a
        // sub-chain is the expensive conjunct, so a cheap one that
        // already rejected the row should never pay for it.
        for (const SubChain& sub : step.sub_chains) {
            auto value = EvaluateSubChain(sub, frame_);
            if (!value.ok()) return value.status();
            if (!Collapse(value.value())) return Status::OK();
        }

        return RunStep(steps, index + 1);
    }

    catalog::Catalog& catalog_;
    storage::PageStore& store_;

    // Scratch for AcceptTupleAt()'s decode, reused across rows so a scan
    // that spills nothing allocates nothing for the possibility.
    std::vector<PendingSpill> spills_;
    const RowSink& sink_;
    std::uint32_t depth_;
    const ChainFrame* parent_;
    ExecStats& stats_;

    // One budget for the whole statement, shared by reference with every
    // sub-chain: a per-chain budget would let a correlated subquery spend
    // the full allowance once per outer row, which is exactly the shape
    // the budget exists to bound.
    Budget& budget_;

    // ---- The one-entry probe memo (V19) ---------------------------------
    //
    // A probe step descends for the key its outer row produced. The outer
    // chain walks its relation in page order, and a heap page's ids are
    // ascending by construction (invariant 3 plus a system-issued
    // sequence), so a foreign key that repeats - many trades for one
    // account, the ordinary shape - repeats *consecutively*. One entry is
    // therefore worth almost as much as a full cache and costs a
    // comparison.
    //
    // It must be provably result-identical, not merely usually right, so
    // it caches only the *location*: the page id and slot the descent
    // returned. The row is re-read and re-filtered from that location
    // exactly as a fresh descent's would be, so a memo hit and a memo miss
    // run the same code from the tuple onward.
    bool memo_valid_ = false;
    std::uint64_t memo_key_ = 0;
    PageId memo_page_ = kInvalidPageId;
    std::uint16_t memo_slot_ = 0;

    std::vector<Bound> bound_;
    std::vector<const catalog::Schema*> schemas_;
    ChainFrame frame_;
    bool stopped_ = false;
};

}  // namespace

bool PageSpanGuardTripped() noexcept { return g_guard_tripped; }
void ResetPageSpanGuard() noexcept { g_guard_tripped = false; }

StatusOr<bool> EvaluateConjuncts(catalog::Catalog& catalog, storage::PageStore& store,
                                 const std::vector<const catalog::Schema*>& schemas,
                                 const Step& step, const ChainFrame& frame, ExecStats* stats,
                                 const Budget& budget) {
    auto matched = EvaluateAll(schemas, step.residual, frame);
    if (!matched.ok()) return matched.status();
    if (!matched.value()) return false;
    if (step.sub_chains.empty()) return true;

    ExecStats local;
    ExecStats& counters = stats != nullptr ? *stats : local;
    Budget spend(budget.limit());
    // A runner with no sink of its own: it exists only to lend its
    // sub-chain evaluation, which builds its own sink per sub-chain.
    static const RowSink kUnused = nullptr;
    ChainRunner runner(catalog, store, kUnused, /*depth=*/0, /*parent=*/nullptr, counters, spend);

    for (const SubChain& sub : step.sub_chains) {
        auto value = runner.EvaluateSubChain(sub, frame);
        if (!value.ok()) return value.status();
        // The same collapse point every other caller uses.
        if (!Collapse(value.value())) return false;
    }
    return true;
}

Status Execute(catalog::Catalog& catalog, storage::PageStore& store, const StepChain& chain,
               const RowSink& sink, ExecStats* stats, const Budget& budget) {
    if (chain.steps.empty()) {
        return Status::InvalidArgument("a step chain with no steps reads nothing");
    }

    ExecStats local;
    ExecStats& counters = stats != nullptr ? *stats : local;
    // One mutable budget for this statement, seeded from the caller's
    // limit. Taking the parameter by const reference and copying here is
    // what keeps a Budget reusable across statements without a caller
    // having to remember to reset it.
    Budget spend(budget.limit());

    ChainRunner runner(catalog, store, sink, /*depth=*/0, /*parent=*/nullptr, counters, spend);

    // Hoisted sub-chains run **once**, before the outer chain opens. An
    // uncorrelated subquery's answer is the same for every outer row by
    // definition, so running it per row would compute one value n times.
    //
    // The consequence worth having: a false uncorrelated `EXISTS` answers
    // the whole statement here, and the outer relation is never opened at
    // all. That is a real saving on a large relation, and it is only
    // available because hoisting is decided structurally at compile.
    if (!chain.hoisted.empty()) {
        // An empty frame with no parent: a hoisted sub-chain refers to
        // nothing outside itself, which is what "uncorrelated" means.
        ChainFrame empty;
        empty.Open({}, /*parent=*/nullptr);
        for (const SubChain& sub : chain.hoisted) {
            auto value = runner.EvaluateSubChain(sub, empty);
            if (!value.ok()) return value.status();
            if (!Collapse(value.value())) return Status::OK();  // no rows, nothing opened
        }
    }

    return runner.Run(chain.steps);
}

}  // namespace kds::exec
