#include "kds/exec/step_vm.hpp"

#include "kds/sched/coro.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <vector>

#include <string>

#include "kds/exec/index_key.hpp"
#include "kds/exec/inner_build.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/index/index_tree.hpp"

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
// executes on one core (docs/rules/rules.md §3). An atomic here would suggest a
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

// A chain compiled from a declared pattern's body carries `$param`
// placeholders and **must never be executed**: nothing binds them, so any
// answer it produced would be an answer to a statement nobody wrote.
//
// Reachable only through a defect - `$x` is a parse error outside a
// CREATE PATTERN body, and CREATE PATTERN compiles for validation and
// discards the chain - so it is reported as Corruption, alongside this
// file's other malformed-chain checks, and it fails loudly rather than
// treating the placeholder as a non-matching value. A quiet false here would
// turn the defect into a query that silently returns no rows.
Status RefuseUnboundParam(const parser::AstValue& value) {
    if (value.type != parser::ValueType::kParam) return Status::OK();
    return Status::Corruption("a declared pattern's chain reached execution: parameter '$" +
                              value.param_name() + "' has no bound value");
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
    if (Status s = RefuseUnboundParam(*value); !s.ok()) return s;
    if (value->type != parser::ValueType::kInt || value->int_val < 0) {
        // Not a pk-shaped value. Ids are zero-extended 40-bit (invariant
        // 7), so this cannot match anything - reported so the caller can
        // treat it as a miss rather than probing a wrapped key.
        return Status::NotFound("probe key is not a non-negative integer");
    }
    return static_cast<std::uint64_t>(value->int_val);
}

// The snapshot a caller who passed none gets: every writer visible, no undo
// log, no copy ever taken. Named rather than default-constructed inline so
// that "a runner without a transaction behaves exactly as the pre-MVCC
// executor did" is one identifier a reader can follow.
const txn::Snapshot kSeesEverything{};

// One chain's execution state. Recreated per sub-chain, which is what
// makes the frame stack a stack.
// Drives a spine coroutine to completion synchronously - the two walk
// visitor call sites and the sync Execute() wrapper use it. Correct
// precisely while nothing suspends beneath (P4d-2's contract), and as of
// P4d-3 that contract is *enforced* rather than assumed: a coroutine that
// parks on a wait beneath this driver is a hard error, because the driver
// is synchronous and cannot deliver what the wait waits for - resuming
// past it would fabricate the reply (the 5ec61da review finding).
// Multi-step descent through this seam is P4d-4's batching to dissolve.
Status RunToCompletionAtWalkBoundary(sched::Coro coro) {
    while (!coro.done()) {
        if (!coro.TryResumeDeepest()) {
            return Status::InvalidArgument(
                "a coroutine parked on a wait beneath a synchronous walk boundary; awaits "
                "belong at the page boundary, never beneath a visitor "
                "(docs/inflight/in-progress/workplan-crosscore.md P4d-3)");
        }
    }
    return coro.result();
}

// ---- The resumable walk (docs/spec/join-inner-build.md §6, JB6) ---------
//
// A stopping sub-chain's walk is cut short by its own sink, so the map it
// filled covers a *prefix* of the relation. Making that partiality safe
// needs one thing the executor did not have: a position a later walk can
// resume from. That is this mark - a page, and how many of that page's
// rows the walk covered in the page's own emission order.
//
// **Rows, not slots.** The emission order is the page's, and it is not
// always slot order (`Step::emit_in_key_order` sorts a page by Keystone id
// first), so the ordinal counts accepted rows in whatever order the walk
// emits them. Within one statement that sequence is fixed - nothing writes
// to the relation between the outer rows of a SELECT (spec §4, the same
// argument JB4's location hints rest on) - so an ordinal taken by one walk
// names the same row to the next.
//
// `page == kInvalidPageId` is "from the head", which is where a first walk
// starts and what an unset mark means.
struct WalkMark {
    PageId page = kInvalidPageId;
    std::uint32_t visited = 0;
};

// What a resumable walk reads and writes: where to start, how far it got,
// and whether it reached the relation's end - which is what turns a bucket
// miss from "walk from the mark" into a conclusive absence.
struct WalkPrefix {
    WalkMark resume;
    WalkMark mark;
    bool complete = false;
};

// ---- The statement-local inner build's state (JB3/JB5/JB6) --------------
//
// kDeclined is the cap's verdict for the *join* form and it is per
// statement (JB5): a capped build re-attempted on every later outer row
// would bucket to the cap and discard each time - strictly worse than the
// walk the fall-back protects - so a declined step walks plain for the
// rest. **The stopping form never declines** (spec §6): its map is a
// prefix, so a capped one keeps serving what it covers while the mark
// stops advancing, which is what `frozen` records.
//
// kBuilt means the map covers the whole relation: published by a completed
// unstopped walk (join, JB3) or reached by a prefix walk that ran to the
// relation's end (JB6). Both make a bucket miss a conclusive absence,
// which is the one thing a partial map may not conclude.
enum class BuildPhase : std::uint8_t { kUnbuilt, kBuilt, kDeclined };

struct InnerBuildState {
    InnerBuild map;
    BuildPhase phase = BuildPhase::kUnbuilt;
    // JB6: how far the prefix reaches, and whether the cap stopped it
    // reaching further. Unused by the join form, whose map is total or
    // nothing.
    WalkMark mark;
    bool frozen = false;
};

// One map per annotated step, statement-lifetime. **Statement, not
// runner**: a sub-chain runner is rebuilt per outer row, so a map living
// in the runner would be built and thrown away once per outer row - the
// store is owned by the top-level runner and every nested runner shares it
// by pointer, the way `stats_` and `budget_` are shared. Step ids are
// global across a statement (step_chain.hpp), so one map per id needs no
// scoping beyond that.
//
// Node-based on purpose, like the map's own buckets (inner_build.hpp): a
// build holds `InnerBuildState&` across its co_await, during which a
// deeper annotated step's entry may rehash this container - nodes relink,
// references hold.
using InnerBuildStore = std::unordered_map<std::uint32_t, InnerBuildState>;

class ChainRunner {
public:
    // `builds` is the statement's inner-build store, shared the way
    // `stats` and `budget` are: a nested runner is rebuilt per outer row,
    // and a map rebuilt per outer row is not a map (JB6). Null means "this
    // runner is the statement" and it owns one.
    ChainRunner(catalog::Catalog& catalog, storage::PageStore& store, const RowSink& sink,
                std::uint32_t depth, const ChainFrame* parent, ExecStats& stats, Budget& budget,
                TrailCollector* trail, const TrailReplay* replay, stats::CabinStore* cabins,
                const txn::Snapshot* snapshot, bool indexes,
                const std::function<bool()>* resume_gate = nullptr,
                InnerBuildStore* builds = nullptr)
        // Listed in declaration order, which is the order they are actually
        // initialized in - `snapshot_` sits third among the members even
        // though it is eleventh in the parameter list. Every initializer but
        // the last reads a constructor parameter, so no value here depends on
        // the order. `builds_` is the one that names a member - it stores
        // `&owned_builds_` - but only takes its address, which is valid
        // whatever the order, so the list is order-independent in fact as
        // well as in value. Recorded because a reorder that *dereferenced*
        // a member would not be, and this is the line to check first.
        : catalog_(catalog), store_(store),
          snapshot_(snapshot != nullptr ? *snapshot : kSeesEverything), sink_(sink),
          depth_(depth), parent_(parent), stats_(stats), budget_(budget), trail_(trail),
          replay_(replay), cabins_(cabins), indexes_(indexes), resume_gate_(resume_gate),
          builds_(builds != nullptr ? builds : &owned_builds_) {}

    // Sub-chain mode - see `record_through_stops_`'s comment for what it
    // licenses and why only a sub-chain may have it.
    void RecordThroughStops() { record_through_stops_ = true; }

    sched::Coro Run(const std::vector<Step>& steps) {
        if (depth_ > kMaxExecDepth) {
            co_return Status::Unsupported("chain nesting deeper than " +
                                        std::to_string(kMaxExecDepth) + " at execute");
        }
        if (Status s = Bind(steps); !s.ok()) co_return s;
        frame_.Open(schemas_, parent_);
        stopped_ = false;
        co_return co_await RunStep(steps, 0);
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
        // Charged to the sub-chain's own first step. A sub-chain is not a
        // step and has no step_id of its own, and attributing its runs to
        // the *outer* step that triggered it would double-count the work
        // its steps then report themselves.
        if (!sub.steps.empty()) {
            StepStats& sub_stats = stats_.For(sub.steps[0].step_id);
            ++sub_stats.sub_chain_runs;
            // `correlated_scans` - the quadratic-shape signal - is counted
            // at the walk itself (RunWalkStep), not from the compile-time
            // kind here: a kFilterScan driver walks every run and a
            // correlated CabinProbe walks on a miss, and the old kind test
            // reported zero for both while the statement stayed quadratic.
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
                    // Same category, same refusal: `kBetween` lowers to two
                    // ordinary conjuncts at compile time (ast.hpp), so it
                    // reaches a step's residual and never a sub-chain.
                    case parser::PredicateKind::kBetween:
                        return Status::Corruption("a plain comparison is not a sub-chain");
                }
                return storage::VisitControl::kContinue;
            };

        // The collector is shared with the sub-chain, not rebuilt: step
        // ids are global across the whole statement (step_chain.hpp), so a
        // nested step's entries belong in the same trail and are already
        // distinguishable by their step_id.
        // The snapshot is shared with the sub-chain for the same reason the
        // collector is: a subquery is part of one statement, and a nested
        // step reading a relation through a different view than its outer
        // step would make one statement see two databases.
        // The inner-build store is shared for the reason the collector and
        // the snapshot are, and one more of its own: this runner is built
        // per outer row, so a map it owned would be filled and destroyed
        // once per outer row - which is the whole cost the map exists to
        // remove (JB6).
        ChainRunner inner(catalog_, store_, collect, depth_ + 1, &outer, stats_, budget_,
                          trail_, replay_, cabins_, &snapshot_, indexes_,
                          /*resume_gate=*/nullptr, builds_);
        // Sub-chain mode - `record_through_stops_`'s comment carries the
        // argument.
        inner.RecordThroughStops();
        Status ran = RunToCompletionAtWalkBoundary(inner.Run(sub.steps));
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
            // Lowered to two conjuncts at compile time, so it is never a
            // sub-chain kind - the same reason `kCompareValue` falls here.
            case parser::PredicateKind::kBetween:
                break;
        }
        return Status::Corruption("unhandled sub-chain kind");
    }

private:

    // Emits, or descends. The recursion is over *steps*, not over rows:
    // one frame holds every bound relation, so reaching the end means one
    // complete output row is sitting in it.
    // The terminal emit, shared by RunStep's index==size edge and by
    // AcceptTupleAt's inlined terminal descent - the split that keeps a
    // coroutine frame off the per-tuple path (coro.hpp's rule; measured at
    // ~55 ns/row before this, bench/results-p4d-executor.md). A row only
    // ever awaits at a step boundary above, never per emit.
    Status EmitRow() {
        auto outcome = sink_(frame_);
        if (!outcome.ok()) return outcome.status();
        if (!outcome.has_value()) {
            return Status::InvalidArgument("a row sink returned an ok Status with no "
                                           "VisitControl");
        }
        if (outcome.value() == storage::VisitControl::kStop) stopped_ = true;
        return Status::OK();
    }

    sched::Coro RunStep(const std::vector<Step>& steps, std::size_t index) {
        if (stopped_) co_return Status::OK();
        if (index == steps.size()) {
            co_return EmitRow();
        }

        const Step& step = steps[index];
        const catalog::TableAccess& access = *bound_[index].access;

        ++stats_.For(step.step_id).relation_opens;
        if (step.kind == AccessKind::kLookup || step.kind == AccessKind::kProbe) {
            co_return co_await RunPointStep(steps, index, step, access);
        }
        if (step.kind == AccessKind::kCabinProbe) {
            co_return co_await RunCabinStep(steps, index, step, access);
        }
        if (step.kind == AccessKind::kIndexProbe || step.kind == AccessKind::kIndexRange) {
            co_return co_await RunIndexStep(steps, index, step, access);
        }
        // The build annotation's arm, in its two forms. The test is the
        // *walk's* class and not the predicate that spelled it
        // (`record_through_stops_` is set for every sub-chain runner): a
        // walk a sink can cut needs the prefix rule, and one that always
        // runs to the end does not.
        if (step.build.has_value()) {
            if (record_through_stops_) {
                co_return co_await WalkPrefixAndProbe(steps, index, step, access);
            }
            co_return co_await WalkAndBuild(steps, index, step, access);
        }
        // A kFilterScan walks exactly as a kScan does - the kind is a
        // statistics distinction, not an execution one, and there is
        // deliberately no branch for it here. If one ever appears, the
        // "same rows either way" tests are what should stop it.
        co_return co_await RunWalkStep(steps, index, step, access);
    }

    // A pk descent. Its answer is authoritative on a btree relation - a
    // miss means the row does not exist - and a heap relation has no pk
    // index, so it falls through to the same walk a Scan would do, with
    // the key kept as a residual. That fall-through is safe *because* the
    // compiler also left the key in `residual`: filtering on the residual
    // list alone gives the same rows.
    sched::Coro RunPointStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        auto key = KeyFromOperand(*step.key, frame_);
        if (!key.ok()) {
            // A key that cannot match: no row, and not an error.
            if (key.status().code() == StatusCode::kNotFound) co_return Status::OK();
            co_return key.status();
        }

        if (access.clustered_type == catalog::ClusteredType::kBtree) {
            // The memo, checked before the descent. It holds a *location*,
            // not a row, so a hit re-reads and re-filters exactly what a
            // fresh descent would have handed to the same code - which is
            // what makes "results identical with the memo on and off" a
            // property of the structure rather than of the test data.
            if (memo_valid_ && memo_step_ == step.step_id && memo_key_ == key.value()) {
                ++stats_.For(step.step_id).probe_memo_hits;
                NoteFetch();
                ++stats_.For(step.step_id).pages_fetched;
                auto bytes = store_.GetForRead(memo_page_);
                if (bytes.ok()) {
                    heap::PageView page(bytes.value().bytes());
                    co_return AcceptTupleAt(steps, index, step, access, memo_page_, page,
                                         memo_slot_);
                }
                // The page went away. Fall through to the descent rather
                // than fail: the memo is an accelerator, never an oracle.
                memo_valid_ = false;
            }

            if (Status s = co_await TryReplay(steps, index, step, access, key.value()); !s.ok()) {
                co_return s;
            }
            if (replayed_) co_return Status::OK();

            NoteFetch();
            ++stats_.For(step.step_id).pages_fetched;
            auto found = btree::BtreeLookup(store_, access.desc_page_id, key.value());
            if (found.ok()) {
                memo_valid_ = true;
                memo_step_ = step.step_id;
                memo_key_ = key.value();
                memo_page_ = found.value().page_id;
                memo_slot_ = found.value().slot;
                // Re-fetched by id rather than carried out of the lookup:
                // the span Location used to carry outlived the descent's
                // pin (workplan-pageref.md Shape C). A hash hit on a
                // still-resident frame, held for the read below.
                auto leaf_page = store_.GetForRead(found.value().page_id);
                if (!leaf_page.ok()) co_return leaf_page.status();
                heap::PageView leaf(leaf_page.value().bytes());
                co_return AcceptTupleAt(steps, index, step, access, found.value().page_id, leaf,
                                     found.value().slot);
            }
            if (found.status().code() == StatusCode::kNotFound) co_return Status::OK();
            co_return found.status();
        }

        // Heap: no index to descend. The walk below is the authoritative
        // path, and the residual carries the key.
        //
        // **This is the case spec section 7 calls large**: a trail turns a
        // full chain scan into one read, because a heap relation has no pk
        // index for a descent to use in the first place.
        if (Status s = co_await TryReplay(steps, index, step, access, key.value()); !s.ok()) co_return s;
        if (replayed_) co_return Status::OK();

        co_return co_await RunWalkStep(steps, index, step, access);
    }

    // ---- Replay (workplan P11/P13) ---------------------------------------
    //
    // Consults the trail for this step's key and, if it validates, reads the
    // row from the recorded location instead of descending for it.
    //
    // Sets `replayed_` rather than returning a bool, because the caller has
    // to distinguish three outcomes and only two of them are a Status: the
    // row was replayed (stop), nothing was replayed (descend), or something
    // went wrong downstream of a *successful* replay (propagate). Folding
    // the first two into a bool and the third into the Status keeps every
    // caller's shape identical.
    //
    // **Every miss falls through to the authoritative path for this step
    // alone** (spec section 2 rule 4). A missing trail, a stale entry, a
    // page that went away and a slot whose occupant changed are all the same
    // answer here - the caller descends - which is deliberate: a caller that
    // could tell them apart would be tempted to treat one of them as
    // authoritative.
    sched::Coro TryReplay(const std::vector<Step>& steps, std::size_t index, const Step& step,
                     const catalog::TableAccess& access, std::uint64_t key) {
        replayed_ = false;
        if (replay_ == nullptr) co_return Status::OK();

        // Rule 0 is **this lookup**. The index is keyed on (step_id, pk) and
        // `key` was just re-derived from the current outer row, so an entry
        // is only found by matching it. There is no separate check to
        // forget, which is what the P13 amendment asks for.
        const TrailLocation* at = replay_->Find(step.step_id, key);
        if (at == nullptr) co_return Status::OK();

        StepStats& step_stats = stats_.For(step.step_id);

        NoteFetch();
        ++step_stats.pages_fetched;
        // Spec section 2 rules 1-2, through the one verifier both this and
        // Cabin's location hints go through (exec/tuple_verify.hpp). The
        // page gone, the slot retired, and a different tuple at the target
        // are all the same answer here - the caller descends - which is
        // deliberate: a caller that could tell them apart would be tempted
        // to treat one of them as authoritative.
        VerifiedTuple verified =
            VerifyTupleAt(store_, at->page_id, at->slot, key, at->page_epoch);
        if (!verified.ok()) {
            ++step_stats.trail_misses;
            co_return Status::OK();
        }

        // Validated. From here it is the authoritative path's own code on
        // the authoritative path's own bytes - decode, residual,
        // sub-chains, descend - so rule 3's "apply visibility exactly as
        // the authoritative path would" is not implemented here so much as
        // inherited.
        ++step_stats.trail_replays;
        replayed_ = true;
        co_return AcceptTupleAt(steps, index, step, access, at->page_id, *verified.page, at->slot);
    }

    // ---- Cabin (docs/spec/cabin.md §4) -----------------------------------
    //
    // A non-pk equality on a cabined column: probe the Cabin's observed set,
    // and fall back to the walk when the value has not been observed.
    //
    // **What makes this the third trust class** is the hit branch. A trail
    // may only replace a *lookup*, because a stored set of locations has no
    // witness for a row inserted since it was recorded. A Cabin has one -
    // the write hook of §5 - so for an observed value its entry set is a
    // superset of the qualifying pks, and serving from it is authoritative
    // rather than advisory. The read subtracts the surplus by re-checking
    // exactly what the authoritative path would: visibility, and the key
    // equality, which is sitting in `step.residual` because the compiler
    // deliberately left it there.
    //
    // Everything else about the step is unchanged: the value is a compile-
    // time literal, the residual is the whole predicate, and downgrading
    // this step to a plain kScan returns the same rows in the same order.
    // The value a cabin step probes for: the literal, or the correlated
    // form's frame value (cabin.md §4a). Re-read rather than threaded
    // through the call chain - the outer row is fixed for this step's
    // whole execution, so every call within it resolves to the same value,
    // and the stability argument lives here once instead of alongside
    // three parameters. The caller has already checked resolvability.
    const parser::AstValue& ProbeValue(const Step& step) const {
        return step.cabin->key_from.has_value() ? frame_.Get(*step.cabin->key_from)
                                                : step.cabin->value;
    }

    sched::Coro RunCabinStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        // Nothing configured, or a value that must never be observed (NULL,
        // an unbound `$param`). Both take the walk, which is what the step
        // would have compiled to had the Cabin not existed. An unresolvable
        // correlated reference is Corruption, per KeyFromOperand's
        // discipline; a frame value MakeCabinKey declines simply walks.
        std::optional<stats::CabinKey> key;
        if (cabins_ != nullptr && step.cabin.has_value()) {
            if (step.cabin->key_from.has_value() && !frame_.CanResolve(*step.cabin->key_from)) {
                co_return Status::Corruption(
                    "a correlated cabin probe references a column the frame cannot resolve");
            }
            key = stats::MakeCabinKey(step.cabin->cabin_id, ProbeValue(step));
        }
        if (!key.has_value()) co_return co_await RunWalkStep(steps, index, step, access);

        if (std::vector<stats::CabinEntry>* entries = cabins_->Find(*key); entries != nullptr) {
            cabins_->NoteHit(step.cabin->cabin_id);
            ++stats_.For(step.step_id).cabin_hits;
            co_return co_await ServeFromCabin(steps, index, step, access, *key, *entries);
        }

        cabins_->NoteMiss(step.cabin->cabin_id);
        ++stats_.For(step.step_id).cabin_misses;

        // The miss path is why the first query for a value costs nothing
        // extra: it was going to walk anyway, and recording is a side
        // effect of the walk (§4). `n = 2` - the first miss only counts.
        // `MayObserve` is the value-cap gate, asked before a recording walk
        // is paid for: a value Commit could never accept must not start
        // one - under the sub-chain completion license each doomed attempt
        // was a full relation walk, re-armed on every probe.
        //
        // **The correlated form earns observation per key** (§4a, amended):
        // a declaration's n = 1 speaks for the literal shape, where the
        // operator named the one value the statement probes - a join
        // probes a value per outer row that nobody named, and under a
        // never-repeating key distribution n = 1 recorded a dead set and
        // its forever write-hook tax for every first touch. At n = 2 a
        // never-repeating key costs one sighting insert and records
        // nothing; a genuinely repeating key records on its second touch
        // and serves from its third.
        const bool declared_here = step.cabin->declared && !step.cabin->key_from.has_value();
        const bool record =
            cabins_->WouldRecord(cabins_->Observe(*key), declared_here) &&
            [&] {
                if (cabins_->MayObserve(*key)) return true;
                cabins_->NoteCapRefusal();
                return false;
            }();
        if (!record) co_return co_await RunWalkStep(steps, index, step, access);
        co_return co_await WalkAndRecord(steps, index, step, access, *key);
    }

    // Walks `step` with a recording for `key` live, and commits the set iff
    // the walk completed. **The only place a Recording is created** - the
    // miss path and the heap hint-failure path both end here, which is what
    // keeps every rule below one rule with one home.
    sched::Coro WalkAndRecord(const std::vector<Step>& steps, std::size_t index, const Step& step,
                         const catalog::TableAccess& access, const stats::CabinKey& key) {
        // The recording reads the key column out of this row's frame slots,
        // so the partial decode must have filled it. Guaranteed by the
        // compiler - the cabined equality is a residual conjunct, and
        // `filter_columns` is derived from that same residual - and checked
        // here because the failure would not be an error: it would be a
        // stale slot recorded into a set that is then served as
        // authoritative while missing qualifying pks, the C1 break.
        // Declining to record is always legal (§1's corollary): this walk
        // still answers, and the value simply stays unobserved.
        if (step.filter_columns != Step::kAllColumns &&
            (step.cabin->col_pos >= 64 ||
             ((step.filter_columns >> step.cabin->col_pos) & 1) == 0)) {
            co_return co_await RunWalkStep(steps, index, step, access);
        }

        // **A set may only be banked from a view nothing can later
        // contradict** (cabin.md §6a, which carries the argument and
        // the assumption it rests on). The set outlives this statement and
        // is authoritative for every later reader, so two things forbid
        // recording: an **in-flight** transaction, whose write this walk
        // cannot see and which is live the moment it commits, and the
        // walk's **own** transaction, which may have hidden a row from
        // itself with an uncommitted DELETE or a value-changing UPDATE that
        // its ROLLBACK restores. Either leaves the set missing a live pk,
        // which is the C1 break cabin_store.hpp's header forbids.
        //
        // Declining is free by §1's corollary - the value stays unobserved
        // and the authoritative scan answers it - and under autocommit with
        // nothing in flight this is two comparisons and no change.
        if (snapshot_.view.in_flight_count != 0 ||
            snapshot_.view.own_trx_id != txn::kNoTrxId) {
            cabins_->NoteUnbankableView();
            co_return co_await RunWalkStep(steps, index, step, access);
        }

        Recording recording;
        recording.step_id = step.step_id;
        recording.col_pos = step.cabin->col_pos;
        recording.value = &ProbeValue(step);
        recording.key = &key;
        // **Saved and restored, never cleared.** The walk below descends into
        // the next step, which may be a cabin step recording a walk of its
        // own - and clearing to null on its way out would cancel *this*
        // recording from the second row onwards. The set would then be
        // committed as observed while missing qualifying pks, which is the
        // C1 break the completed-walk check below exists to prevent.
        // `completing_recording_` travels in lockstep: it is the completion
        // license for exactly this walk, and the entry-cap lapse in the
        // recording block may revoke it mid-flight.
        Recording* outer_recording = recording_;
        const bool outer_completing = completing_recording_;
        recording_ = &recording;
        completing_recording_ = record_through_stops_;
        Status walked = co_await RunWalkStep(steps, index, step, access);
        // Read before the restore: whether THIS walk's license survived -
        // the entry-cap lapse revokes it, after which a stop ends the walk
        // and the set is partial.
        const bool walked_through_stops = completing_recording_;
        recording_ = outer_recording;
        completing_recording_ = outer_completing;

        if (!walked.ok()) co_return walked;
        // **Only a completed walk may be committed.** A walk that a sink
        // stopped, or that the budget ended, collected the rows it reached
        // and not the rows it did not - and a set marked observed while
        // missing qualifying pks is precisely the break C1 forbids. This is
        // the one place that check can be made, because it is the only place
        // that knows whether the walk finished. A stopped walk whose
        // completion license survived ran to the relation's end *through*
        // the stop, so its set is whole.
        if (stopped_ && !walked_through_stops) co_return Status::OK();

        // Sorted here, once, rather than on every hit: entries served in
        // page order batch same-page tuples into one fetch (§3), and a set
        // is written far less often than it is read. Appends by the write
        // hook land at the end and leave it *nearly* sorted, which costs a
        // little locality and no correctness.
        std::sort(recording.entries.begin(), recording.entries.end(),
                  [](const stats::CabinEntry& a, const stats::CabinEntry& b) {
                      return a.page_id < b.page_id || (a.page_id == b.page_id && a.slot < b.slot);
                  });
        if (cabins_->Commit(key, std::move(recording.entries))) {
            ++stats_.For(step.step_id).cabin_recordings;
        }
        co_return Status::OK();
    }

    // Whether an annotated step may build at all - a property of the step
    // and its relation, not of the run, so both build forms ask it once
    // and in the same words.
    //
    // WalkAndRecord's guard, for its reason: the bucketing reads the join
    // column out of this row's frame slots, so the partial decode must
    // have filled it. The compiler guarantees it - the correlated conjunct
    // is a residual conjunct and `filter_columns` derives from that same
    // residual - and declining to build is always legal: the walk still
    // answers, per row, as it always did. The range and bounds checks are
    // for a Step built by something other than the compiler (the ladder
    // never pairs `build` with a kind or a range): a pruned walk ends
    // early with `stopped_` false, so building under one would publish a
    // partial map as the whole relation - declined here so "a published
    // map is a full relation" is structural, not an else-if two files
    // away. A range would also give the prefix mark a second meaning, the
    // pruned tail being a position no resume could tell from the end.
    bool BuildIsArmable(const Step& step, const catalog::TableAccess& access) const {
        return !step.range.has_value() && step.build.has_value() &&
               step.build->residual_pos < step.residual.size() &&
               step.build->col_pos < access.schema.columns.size() &&
               (step.filter_columns == Step::kAllColumns ||
                (step.build->col_pos < 64 &&
                 ((step.filter_columns >> step.build->col_pos) & 1) != 0));
    }

    // The walked join's lazy build (docs/spec/join-inner-build.md §2,
    // workplan JB3): the annotated step's first walk runs exactly as
    // written and buckets, as a side effect, every row passing the step's
    // non-correlated residual - including rows failing the current outer
    // key's equality, which emit nothing and enter the map under their own
    // value. WalkAndRecord's pattern with the one difference stated up
    // front: the Cabin records the probed key's set, this records the
    // whole map, because one full pass is the point. A later walk of a
    // built step still walks - JB4 replaces that branch with the probe.
    sched::Coro WalkAndBuild(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        if (!BuildIsArmable(step, access)) {
            co_return co_await RunWalkStep(steps, index, step, access);
        }

        InnerBuildState& state = (*builds_)[step.step_id];
        // The off-switch and the cap's standing verdict (workplan JB5):
        // `join_build_max_rows == 0` disables the build outright - no map
        // can hold a row - and a declined step already took the Cabin's
        // fall-back refusal. Both walk plain, never error.
        if (budget_.join_build_max_rows() == 0 || state.phase == BuildPhase::kDeclined) {
            co_return co_await RunWalkStep(steps, index, step, access);
        }
        if (state.phase == BuildPhase::kBuilt) {
            co_return co_await ProbeBuild(steps, index, step, access, state.map);
        }

        BuildRecording build;
        build.step_id = step.step_id;
        build.col_pos = step.build->col_pos;
        build.residual_pos = step.build->residual_pos;
        build.max_rows = budget_.join_build_max_rows();
        build.map = &state.map;
        BuildRecording* outer = building_;
        building_ = &build;
        Status walked = co_await RunWalkStep(steps, index, step, access);
        building_ = outer;

        // The publish site, and the one home of its rule: only a walk that
        // reached the relation's end unstopped and under the cap publishes
        // - WalkAndRecord's completed-walk argument, since a cut or capped
        // walk bucketed the rows it reached and not the rows it did not,
        // and a probe would serve that map as the whole relation. Every
        // non-publishing exit resets, so "a cut build is never served" is
        // a fact about state on the error path too.
        if (walked.ok() && !stopped_ && !build.over_cap) {
            state.phase = BuildPhase::kBuilt;
            ++stats_.For(step.step_id).inner_builds;
        } else {
            state.map = InnerBuild();
            if (build.over_cap) state.phase = BuildPhase::kDeclined;
        }
        co_return walked;
    }

    // The stopping sub-chain's prefix map (docs/spec/join-inner-build.md
    // §6, workplan JB6). An `EXISTS`-class sub-chain's walk is cut by its
    // own sink at the first qualifying row, so the map it filled covers a
    // *prefix* of the relation. This does not complete the map - it makes
    // partiality safe, in three steps that are each one property:
    //
    //  1. **Replay the prefix.** The bucket for this outer row's key holds
    //     exactly the rows before the mark that passed the non-correlated
    //     residual and carry this value. Rows before the mark that are
    //     *not* in the bucket either failed that residual or carry another
    //     value, and the walk would have emitted neither - so the replay
    //     reproduces the prefix's emissions exactly, in walk order, for
    //     every sub-chain kind. That is what makes this safe for `IN`'s
    //     per-row form too, whose sink walks past a non-matching row
    //     rather than stopping at it: the answer is the sink's, and the
    //     replay only has to hand it the same rows in the same order.
    //  2. **A covered relation has answered.** With `kBuilt` the map holds
    //     every row, so a bucket the replay exhausted without a stop is a
    //     conclusive absence - the one thing a partial map may never
    //     conclude, which is why it is gated on the phase and not on the
    //     bucket being empty.
    //  3. **Otherwise resume at the mark**, extending the map with every
    //     row the walk visits. The rows before the mark were answered by
    //     step 1, so the walk starts where the last one stopped, and every
    //     inner row is visited at most once per statement - spec §6's
    //     economics, and the reason this needs no earn gate, no
    //     publication gate and no budget carve-out.
    //
    // The plain join of §2 is the degenerate case of the same rule and is
    // *not* routed here: its first walk never stops, so its map is total
    // from the second outer row on, and WalkAndBuild's simpler form says
    // so in fewer moving parts.
    sched::Coro WalkPrefixAndProbe(const std::vector<Step>& steps, std::size_t index,
                                   const Step& step, const catalog::TableAccess& access) {
        if (!BuildIsArmable(step, access) || budget_.join_build_max_rows() == 0) {
            // The off-switch is the A/B lever (JB5) and it gates here, at
            // the arm: nothing maps, nothing marks, and the walk is the
            // walk this statement always ran.
            co_return co_await RunWalkStep(steps, index, step, access);
        }

        InnerBuildState& state = (*builds_)[step.step_id];

        // 1. The prefix, replayed for this key.
        if (state.map.rows() > 0) {
            if (Status probed = co_await ProbeBuild(steps, index, step, access, state.map);
                !probed.ok()) {
                co_return probed;
            }
            if (stopped_) co_return Status::OK();
        }

        // 2. A map that covers the relation has said everything there is.
        if (state.phase == BuildPhase::kBuilt) co_return Status::OK();

        // 3. Resume, and extend.
        WalkPrefix prefix;
        prefix.resume = state.mark;
        BuildRecording build;
        build.step_id = step.step_id;
        build.col_pos = step.build->col_pos;
        build.residual_pos = step.build->residual_pos;
        build.max_rows = budget_.join_build_max_rows();
        build.map = &state.map;
        // A frozen map starts the walk already over its cap, which is how
        // "keeps serving its prefix, never extends it" needs no second
        // switch: the bucketing site declines every row and the mark stays
        // where the cap left it (spec §6's capped form).
        build.over_cap = state.frozen;
        BuildRecording* outer = building_;
        building_ = &build;
        Status walked = co_await RunWalkStep(steps, index, step, access, &prefix);
        building_ = outer;

        if (!walked.ok()) {
            // A walk that failed mid-row may have bucketed a row the mark
            // does not cover, and a map and a mark that disagree would
            // emit that row twice - once from the bucket, once from the
            // resumed walk. The statement is over either way (the status
            // propagates out of the sub-chain), so this costs nothing and
            // is state rather than luck: the next use starts from the head.
            state = InnerBuildState{};
            co_return walked;
        }

        state.mark = prefix.mark;
        if (build.over_cap) state.frozen = true;
        if (prefix.complete) {
            // The map now covers the relation, by walking to its end
            // rather than by a publish - and from here it answers like any
            // built map, misses included.
            state.phase = BuildPhase::kBuilt;
            ++stats_.For(step.step_id).inner_builds;
        }
        co_return walked;
    }

    // The probe (workplan JB4): a later outer row of a built step replays
    // its key's bucket instead of walking the relation. Three deliberate
    // differences from ServeFromCabin, each a correctness statement:
    //
    //  - **No sort.** Bucket order is walk order (the map's one
    //    load-bearing property, inner_build.hpp), which is exactly what
    //    the per-row walk emitted for this key - sorting is what would
    //    change a reply.
    //  - **No dedup, no hint verification.** One entry per walked row by
    //    construction, and no location moves between the build and the
    //    probe. That second half is a fact about *this statement*, not
    //    about the engine: a btree leaf division does move tuples -
    //    SplitLeafAndInsert (storage/btree/btree.cpp) calls itself a
    //    relayout in everything but name and bumps `relayout_epoch` for
    //    exactly that reason, which is what `VerifyTupleAt` checks on the
    //    Cabin's behalf. It cannot reach a probe because nothing can write
    //    to the inner relation in between: the statement is a SELECT (spec
    //    §8 excludes a DML `WHERE` sub-chain at *compile* - `inner_build`
    //    is false from `CompileWhere` down, JB1 - so no such step is ever
    //    annotated; the runtime gate that used to say so a second time is
    //    JB6's prefix arm now), and an annotated step has no park at all -
    //    `BuildKey` is never encoded by the descriptor codec
    //    (step_chain.hpp), so a built step never runs under a
    //    `resume_gate_`, which is the executor's only suspension point.
    //    **JB6's resumed walk does not change that**: a sub-chain runner
    //    is constructed without a resume gate, so the walk it resumes has
    //    no more suspension point than the walk it continues - which is
    //    also what lets a physical mark name the same row twice.
    //    **Give a built step a park - JB8's peer-side build, P4d-4c's
    //    multi-step gate - and this arm owes VerifyTupleAt with a pk
    //    fallback**, because the in-place-update and
    //    slots-never-compact cases are not the only way a location dies.
    //    What stands in for both checks meanwhile: every entry goes through
    //    `AcceptTupleAt`, which re-applies MVCC under the statement's
    //    fixed snapshot and re-evaluates the **full** residual - the
    //    superset-plus-recheck idiom, so correctness never rests on build
    //    bookkeeping, only cost does (spec §4).
    //  - **A missing bucket concludes nothing here.** Whether it means
    //    "the relation holds no such row" belongs to the caller and to
    //    the map's phase: WalkAndBuild probes only a published map, which
    //    walked to the relation's end, and JB6's prefix form probes a
    //    partial one and walks on from the mark unless the phase says the
    //    map is total. This function replays a bucket; it never decides
    //    what an empty one means.
    sched::Coro ProbeBuild(const std::vector<Step>& steps, std::size_t index, const Step& step,
                      const catalog::TableAccess& access, const InnerBuild& map) {
        if (!frame_.CanResolve(step.build->key_from)) {
            co_return Status::Corruption(
                "a build annotation's outer column is unresolvable; the chain is malformed");
        }
        // Stable across the descent: every entry point pre-sizes the stats
        // vector, which is what ServeFromCabin already relies on.
        StepStats& step_stats = stats_.For(step.step_id);
        ++step_stats.build_probes;

        // A NULL outer key matches no equality; emitting nothing is the
        // walk's answer too (every comparison against it is false).
        auto key = stats::MakeValueKey(frame_.Get(step.build->key_from));
        if (!key.has_value()) co_return Status::OK();

        const InnerBuild::Bucket bucket = map.Find(*key);
        if (bucket.empty()) co_return Status::OK();

        // Bucket order, front to back. The Bucket is stable across the
        // descent by construction (inner_build.hpp's contract: it holds an
        // index into the arena, not a pointer, so not even an Add under
        // its own key could invalidate it - and nothing adds to a built
        // map anyway), and the page is fetched per entry rather than held
        // - AcceptTupleAt descends, and anything below it may fetch (R1).
        //
        // `pages_fetched` counts page *transitions*, as the walk it
        // replaced counted them - bucket order is walk order, so
        // consecutive same-page entries are the common case, and a
        // per-entry count would price the probe above the walk it beat.
        PageId last_page = kInvalidPageId;
        for (const stats::CabinEntry& entry : bucket) {
            if (stopped_) break;
            NoteFetch();
            if (entry.page_id != last_page) {
                ++step_stats.pages_fetched;
                last_page = entry.page_id;
            }
            auto bytes = store_.GetForRead(entry.page_id);
            if (!bytes.ok()) co_return bytes.status();
            heap::PageView page(bytes.value().bytes());
            if (Status s =
                    AcceptTupleAt(steps, index, step, access, entry.page_id, page, entry.slot);
                !s.ok()) {
                co_return s;
            }
        }
        co_return Status::OK();
    }

    // A secondary-index probe or range (docs/spec/index.md §§1, 7).
    //
    // **Two phases, as ServeFromCabin is, and for a related reason.** Phase 1
    // walks the index between the bounds the compiler encoded and collects
    // the pks it names; phase 2 resolves each through the clustered tree and
    // emits. The split is what keeps R1: `AcceptTupleAt` descends into the
    // next step and anything below it may fetch, so an index-leaf span held
    // across emission is exactly the span that rule forbids.
    //
    // What this must **not** do, both stated because both look like
    // optimizations:
    //
    //   - It does not decide visibility. The predicate lives at exactly one
    //     site and this is not it; every located row goes through
    //     `AcceptTupleAt` like every other kind.
    //   - It does not emit a row from the entry. There is no visibility
    //     witness outside the tuple (spec §7), so covered columns are a
    //     **filter** and never a projection source - they let a row that
    //     will be dropped avoid its base descent, and nothing more.
    sched::Coro RunIndexStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        // Any reason to decline takes the walk, which is what the step would
        // have compiled to had the index not existed - and returns the
        // identical rows, because the equalities are still in the residual.
        // `indexes = off`: take the walk the step would have taken had the
        // index not existed. The chain is **unchanged** - the kind is still
        // kIndexProbe and ANALYZE still says so - which is what keeps the
        // plan `f(shape, catalog)` and makes the A/B comparison compare
        // execution rather than compilation.
        if (!indexes_ || !step.index.has_value()) co_return co_await RunWalkStep(steps, index, step, access);
        const IndexProbe& probe = *step.index;

        // The **live** root, not the one compiled in: a split republishes it
        // in place on the cached entry (index.md §12a), and a chain
        // compiled before a write may name a page that is no longer the top
        // of the tree.
        const catalog::TableAccess::IndexRef* ix = nullptr;
        for (const catalog::TableAccess::IndexRef& candidate : access.indexes) {
            if (candidate.index_oid == probe.index_oid) ix = &candidate;
        }
        // Dropped between compile and execution. The chain is stale, not
        // wrong: walking answers it.
        if (ix == nullptr) co_return co_await RunWalkStep(steps, index, step, access);

        index::IndexLayout layout;
        layout.key_width = ix->key_width;
        layout.covered_width =
            static_cast<std::uint16_t>(ix->entry_width - ix->key_width - index::kIndexPkWidth);
        const std::size_t sort_key_width = layout.sort_key_width();
        if (probe.low.size() != sort_key_width || probe.high.size() != sort_key_width) {
            // The index was redefined under a compiled chain. Same answer as
            // a dropped one.
            co_return co_await RunWalkStep(steps, index, step, access);
        }

        // The correlated form (index.md §8a): the bounds are built
        // here, per outer row - the padding templates copied, the frame
        // value encoded once into the leading width. Every decline takes
        // the walk, per this function's opening rule.
        const std::vector<std::byte>* low = &probe.low;
        const std::vector<std::byte>* high = &probe.high;
        if (probe.key_from.has_value()) {
            // The live index's leading column must be the one the compiler
            // chose: the value is encoded into *that* column's key format,
            // and a different one would narrow the range to a column the
            // residual never bound - a lost row, where the stale cases
            // above merely walk. Unreachable while index oids are never
            // reissued; guarded because this is the one decline whose
            // absence would be a wrong answer rather than an error.
            if (ix->nkeys == 0 || probe.key_cols.empty() ||
                probe.key_cols[0] != ix->keys()[0]) {
                co_return co_await RunWalkStep(steps, index, step, access);
            }
            if (!frame_.CanResolve(*probe.key_from)) {
                co_return Status::Corruption(
                    "a correlated index probe references a column the frame cannot resolve");
            }
            const parser::AstValue& value = frame_.Get(*probe.key_from);
            // A frame never holds a param at execute; refused rather than
            // encoded if one ever appears, per KeyFromOperand's discipline.
            if (value.type == parser::ValueType::kParam) {
                co_return co_await RunWalkStep(steps, index, step, access);
            }
            const catalog::SysColumnRow& col = access.schema.columns[ix->keys()[0]];
            auto width = IndexKeyColumnWidth(col);
            if (!width.ok() || width.value() > ix->key_width) {
                co_return co_await RunWalkStep(steps, index, step, access);
            }
            corr_low_.assign(probe.low.begin(), probe.low.end());
            corr_high_.assign(probe.high.begin(), probe.high.end());
            if (!EncodeIndexKeyColumn(col, value,
                                      std::span<std::byte>(corr_low_).subspan(0, width.value()))
                     .ok()) {
                co_return co_await RunWalkStep(steps, index, step, access);
            }
            // The equality's two bounds share their leading bytes; only the
            // padding tails differ, so the encode happens once.
            std::memcpy(corr_high_.data(), corr_low_.data(), width.value());
            low = &corr_low_;
            high = &corr_high_;
        }

        StepStats& step_stats = stats_.For(step.step_id);
        index_scratch_.clear();
        seen_pks_.clear();

        auto first = index::IndexSeekLeaf(store_, ix->root_page_id, layout, *low);
        if (!first.ok()) co_return first.status();
        NoteFetch();
        ++step_stats.pages_fetched;

        Status walked = index::IndexVisitFrom(
            store_, first.value(), layout, storage::PageAccess::kRead,
            [&](PageId, index::IndexLeafView& leaf,
                std::uint16_t at) -> StatusOr<storage::VisitControl> {
                auto entry = leaf.Entry(at);
                if (!entry.ok()) return entry.status();
                const std::span<const std::byte> sort_key =
                    entry.value().subspan(0, sort_key_width);

                // The descent lands on the first leaf that *could* hold the
                // low bound, so the entries before it are skipped rather
                // than assumed absent.
                if (std::memcmp(sort_key.data(), low->data(), sort_key_width) < 0) {
                    return storage::VisitControl::kContinue;
                }
                // Past the high bound: the leaves to the right are never
                // fetched, which is what makes a range cost its range.
                if (std::memcmp(sort_key.data(), high->data(), sort_key_width) > 0) {
                    return storage::VisitControl::kStop;
                }
                ++step_stats.index_entries_scanned;

                // **Duplicates are expected, not damage.** Maintenance is
                // append-only, so a key round trip (v -> v' -> v) leaves two
                // entries naming one pk (§2) - and resolving it twice would
                // emit its row twice.
                const std::uint64_t pk =
                    index::GetIndexPk(entry.value().subspan(ix->key_width));
                if (!seen_pks_.insert(pk).second) return storage::VisitControl::kContinue;

                // The covering filter (§7). A row this entry's own values
                // already disqualify never costs a base descent - which is
                // the whole of what covering buys, since visibility still
                // requires the tuple.
                if (layout.covered_width > 0) {
                    auto kept = CoveredRowSurvives(step, access, *ix,
                                                   entry.value().subspan(sort_key_width));
                    if (!kept.ok()) return kept.status();
                    if (!kept.value()) {
                        ++step_stats.index_entries_filtered;
                        return storage::VisitControl::kContinue;
                    }
                }

                index_scratch_.push_back(pk);
                return storage::VisitControl::kContinue;
            });
        if (!walked.ok()) co_return walked;

        // **Sorted, and this is a correctness property rather than a
        // locality one.** The walk collects pks in *index key* order; a scan
        // of the same relation emits them in pk order. Without this sort,
        // creating an index would reorder a reply - and "an accelerator may
        // cost performance and must never change a query result" is the
        // standard invariant 8 holds Waystone to, which an authoritative
        // structure does not get to fall below. The equivalence tests
        // compare byte for byte precisely so this cannot be missed.
        //
        // It buys locality as well: on a btree relation pk order *is* leaf
        // order, so the descents below walk the tree forwards instead of
        // jumping. The cost is one sort of the matched set, against one
        // descent per element of it.
        std::sort(index_scratch_.begin(), index_scratch_.end());

        // **Moved out of the member for phase 2, and that is a correctness
        // property.** The descent below re-enters `RunStep` for the *next*
        // step, which may itself be an index step on this same runner - and
        // phase 1 above starts by clearing this vector. A chain with two
        // index steps (`... FROM b AS a JOIN b AS c ON a.qty = c.qty WHERE
        // a.owner = 1 AND c.owner = 2`) therefore had the inner step clear
        // and refill the very buffer the outer loop was walking, so the
        // outer step resolved the *inner* step's pks and the reply silently
        // lost rows - an accelerator changing a query result, which
        // index.md §1 forbids outright.
        //
        // The buffer is handed back after the loop, so the single-index-step
        // chain - the common one - still reuses one allocation across rows,
        // which is why the scratch lives on the runner at all.
        std::vector<std::uint64_t> pks;
        pks.swap(index_scratch_);

        // Phase 2. Every pk resolves through the clustered tree - the index
        // is refused on a heap relation precisely so this descent exists
        // (spec IX3).
        for (const std::uint64_t pk : pks) {
            if (stopped_) break;
            NoteFetch();
            ++step_stats.pages_fetched;
            // Counted per descent actually made, not from the scratch size:
            // a sink that stopped mid-resolve (V09's quota) ends this loop
            // early, and a meter that still claimed the whole set would
            // report work not done - the exact number the quota exists to
            // save, and the one ANALYZE prices covering with.
            ++step_stats.index_rows_resolved;
            auto found = btree::BtreeLookup(store_, access.desc_page_id, pk);
            if (!found.ok()) {
                if (found.status().code() == StatusCode::kNotFound) {
                    // Dangling: the pk is in no clustered tree. By K1 it can
                    // never resurface under a different tuple, so the entry
                    // is dead forever - a skip, never an error.
                    continue;
                }
                co_return found.status();
            }

            auto bytes = store_.GetForRead(found.value().page_id);
            if (!bytes.ok()) co_return bytes.status();
            heap::PageView page(bytes.value().bytes());
            if (Status s = AcceptTupleAt(steps, index, step, access, found.value().page_id, page,
                                         found.value().slot);
                !s.ok()) {
                co_return s;
            }
        }
        pks.clear();
        pks.swap(index_scratch_);
        co_return Status::OK();
    }

    // Whether an entry's covered columns already disqualify its row.
    //
    // A **conservative** test: it answers false only when a residual
    // predicate this entry's own values can decide says no. Anything it
    // cannot decide - a predicate on an uncovered column, a spilled covered
    // value, an operand that is not a literal - keeps the row, and the base
    // read filters it exactly as it would have. Getting that direction wrong
    // is the difference between a lost row and a wasted descent.
    StatusOr<bool> CoveredRowSurvives(const Step& step, const catalog::TableAccess& access,
                                      const catalog::TableAccess::IndexRef& ix,
                                      std::span<const std::byte> covered) {
        std::size_t at = 0;
        for (const std::uint16_t col_pos : ix.covered()) {
            auto width = catalog::RowLayout::ColumnWidth(access.schema.columns[col_pos],
                                                          access.layout.inline_cell_width);
            if (!width.ok()) return width.status();
            if (at + width.value() > covered.size()) {
                return Status::Corruption("index entry is shorter than its covered columns");
            }
            const std::span<const std::byte> cell = covered.subspan(at, width.value());
            at += width.value();

            bool decoded = false;
            for (const StepPredicate& pred : step.residual) {
                if (pred.lhs.up != 0 || pred.lhs.col_pos != col_pos) continue;
                if (pred.rhs.kind != OperandKind::kLiteral) continue;

                if (!decoded) {
                    // A spilled covered value carries a pointer and not the
                    // bytes, and resolving one here would be a page fetch
                    // under the index leaf's span - R1's exact prohibition.
                    // So it is undecidable *here* and the row is kept.
                    std::vector<PendingSpill> spills;
                    if (Status s = DecodeOneValueInto(access.schema.columns[col_pos], cell,
                                                      col_pos, covered_scratch_, &spills);
                        !s.ok()) {
                        return s;
                    }
                    if (!spills.empty()) break;
                    decoded = true;
                }
                if (!CompareValues(access.schema.columns[col_pos].type_val, covered_scratch_,
                                   pred.rhs.literal, pred.op)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Serves an observed value's entry set.
    //
    // **Two phases, and the split is not an optimization.** Phase 1 resolves
    // every entry to a verified location without emitting anything; phase 2
    // emits. That ordering is what makes the heap fallback safe: on a heap
    // relation a failed hint cannot be resolved per entry - there is no pk
    // descent - so the step abandons the Cabin and walks instead, and it may
    // only do that if it has not already emitted rows the walk would emit
    // again. Resolving first means the abandon decision is always taken
    // before the first row goes out.
    sched::Coro ServeFromCabin(const std::vector<Step>& steps, std::size_t index, const Step& step,
                          const catalog::TableAccess& access, const stats::CabinKey& key,
                          std::vector<stats::CabinEntry>& entries) {
        const bool is_btree = access.clustered_type == catalog::ClusteredType::kBtree;
        StepStats& step_stats = stats_.For(step.step_id);

        serve_scratch_.clear();
        seen_pks_.clear();

        for (std::size_t i = 0; i < entries.size(); ++i) {
            stats::CabinEntry& entry = entries[i];

            // **Duplicates are expected, not damage.** A value round trip
            // (v→v′→v) appends the same pk twice under append-only
            // maintenance (§5), so the set is deduped as it is served and
            // the duplicate is left for pruning to collapse.
            if (!seen_pks_.insert(entry.pk).second) continue;

            if (entry.hint_valid()) {
                NoteFetch();
                ++step_stats.pages_fetched;
                VerifiedTuple verified =
                    VerifyTupleAt(store_, entry.page_id, entry.slot, entry.pk, entry.page_epoch);
                if (verified.ok()) {
                    ++step_stats.cabin_hint_hits;
                    cabins_->NoteHint(key.cabin_id, /*ok=*/true);
                    serve_scratch_.push_back(Located{entry.pk, entry.page_id, entry.slot});
                    continue;
                }
                ++step_stats.cabin_hint_misses;
                cabins_->NoteHint(key.cabin_id, /*ok=*/false);
            }

            if (!is_btree) {
                // No descent to resolve the pk with. Abandon the Cabin for
                // this step and take the authoritative path, re-recording
                // the value's set from it - one bulk heal rather than a
                // chain scan per entry. Nothing has been emitted yet, so
                // this cannot duplicate a row.
                co_return co_await FallBackAndReRecord(steps, index, step, access, key);
            }

            NoteFetch();
            ++step_stats.pages_fetched;
            auto found = btree::BtreeLookup(store_, access.desc_page_id, entry.pk);
            if (!found.ok()) {
                if (found.status().code() == StatusCode::kNotFound) {
                    // Dangling: the pk is in no clustered tree. By K1 it can
                    // never resurface under a new tuple, so this entry is
                    // dead forever - a **skip**, never an error (§5).
                    continue;
                }
                co_return found.status();
            }

            // Healed in place. The hint was wrong and the pk was right,
            // which is C6's whole shape: authority in the pk, speed in the
            // location, and the location repaired from the authority. The
            // healed page's *current* epoch is stamped with it (the rule
            // lives in CurrentRelayoutEpoch, beside the check that reads
            // it). One extra fetch, on the heal path only, which
            // storage-stability makes rare.
            entry.page_id = found.value().page_id;
            entry.slot = found.value().slot;
            entry.page_epoch = CurrentRelayoutEpoch(store_, entry.page_id);
            entry.flags |= stats::kCabinHintValid;
            serve_scratch_.push_back(Located{entry.pk, entry.page_id, entry.slot});
        }

        // Moved out of the member for phase 2, for the reason spelled out in
        // RunIndexStep: the descent below may reach another cabin step on
        // this same runner, whose phase 1 clears this vector - and it would
        // be clearing the one this loop is walking. Handed back after the
        // loop so a single cabin step still reuses one allocation.
        std::vector<Located> located;
        located.swap(serve_scratch_);

        // **Sorted into the walk's order before emission - IX8a's rule,
        // and a bug until 2026-08-19.** A committed set starts in that
        // order (WalkAndRecord sorts by page and slot) but the write hook
        // appends at the *end*, so an UPDATE that moves an earlier row
        // into an observed value leaves the set out of order, and serving
        // it in entry order reordered a reply against I12's within-step
        // contract. Found by the §4a join queries, which emit a set whole
        // where the original list's one exposed value was always filtered
        // down to a single row.
        //
        // **Which order that is depends on whether this relation has taken
        // an out-of-order key**, and getting it from the pk alone is wrong
        // for one of the two (heap-and-tuple.md §4.1). While `key_order` is
        // kAscending, ids ascend with insertion, so pk order *is* the walk's
        // order - across pages by `min_key`, within a page because slots are
        // appended - and sorting by pk is exact even after a leaf division
        // has given a later page a lower id. Once an id has been admitted
        // below the relation's high-water mark, it can sit in a page below
        // the ids already there, so the walk emits that page out of key
        // order and a pk sort here would make a *served* execution disagree
        // with the recording one that preceded it. There the entry's own
        // (page, slot) is what the walk gives, which is the order
        // WalkAndRecord committed - so an unordered relation keeps it and
        // gains only the repositioning of an appended entry.
        if (access.key_order == catalog::KeyOrder::kUnordered) {
            std::sort(located.begin(), located.end(), [](const Located& a, const Located& b) {
                return a.page_id < b.page_id || (a.page_id == b.page_id && a.slot < b.slot);
            });
        } else {
            std::sort(located.begin(), located.end(),
                      [](const Located& a, const Located& b) { return a.pk < b.pk; });
        }

        // Phase 2. The page is fetched per entry rather than carried out of
        // phase 1: `AcceptTupleAt` descends into the next step, and anything
        // below it may fetch, so a page view held across entries is exactly
        // the span R1 forbids.
        for (const Located& at : located) {
            if (stopped_) break;
            NoteFetch();
            ++step_stats.pages_fetched;
            auto bytes = store_.GetForRead(at.page_id);
            if (!bytes.ok()) {
                // The page went away between the two phases. Nothing evicts
                // and nothing frees pages, so this is unreachable today -
                // and it is a skip rather than an error because a Cabin
                // entry pointing at nothing is a dead entry, which §5 says
                // to drop on sight.
                ++step_stats.cabin_hint_misses;
                continue;
            }
            heap::PageView page(bytes.value().bytes());
            if (Status s = AcceptTupleAt(steps, index, step, access, at.page_id, page, at.slot);
                !s.ok()) {
                co_return s;
            }
        }
        step_stats.cabin_entries_served += located.size();
        located.clear();
        located.swap(serve_scratch_);
        co_return Status::OK();
    }

    // The heap hint-failure path: un-observe, walk, and re-record.
    //
    // Un-observing first is what keeps this honest. The walk below is the
    // authoritative path, so the rows are right either way - but the set
    // that produced a bad hint has just been shown to disagree with storage,
    // and §1's corollary makes dropping it always legal.
    sched::Coro FallBackAndReRecord(const std::vector<Step>& steps, std::size_t index,
                               const Step& step, const catalog::TableAccess& access,
                               const stats::CabinKey& key) {
        cabins_->Unobserve(key);
        co_return co_await WalkAndRecord(steps, index, step, access, key);
    }

    // `prefix` is JB6's resumable form and is null for every other walk:
    // the walk then starts at the head, counts nothing, and is the walk it
    // has always been. When set, it says where to resume, and the walk
    // reports back where it stopped and whether it reached the end.
    sched::Coro RunWalkStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                       const catalog::TableAccess& access, WalkPrefix* prefix = nullptr) {
        Status inner = Status::OK();

        // ---- The resumable prefix (spec §6, workplan JB6) ----------------
        //
        // A mark is a page and a count of that page's rows already covered.
        // The mark starts where the last walk left it, so a walk that
        // visits nothing leaves it untouched.
        const bool prefixed = prefix != nullptr;
        const PageId resume_page = prefixed ? prefix->resume.page : kInvalidPageId;
        const std::uint32_t resume_visited = prefixed ? prefix->resume.visited : 0;
        if (prefixed) prefix->mark = prefix->resume;
        // The build this walk extends, when it is this step's own: read
        // after each accepted row, because the cap trips inside one - and
        // the row that trips it is visited without being bucketed, so the
        // mark may not advance past it.
        const bool extending =
            prefixed && building_ != nullptr && building_->step_id == step.step_id;
        std::uint32_t visited_on_page = 0;
        bool mark_frozen = false;
        // Whether the walk ended on a stop rather than on the relation's
        // end - the two are one return value from the page primitives
        // (kInvalidPageId either way), and only the walk knows which.
        bool cut = false;

        // ---- Range pruning (kRange) --------------------------------------
        //
        // Both storage forms are ordered **page-wise** by `min_key`: every
        // id in a page is below the next page's min_key (heap_chain.hpp's
        // ordering property, and the btree leaf chain by construction). So
        // the first page whose min_key exceeds the high bound proves that
        // nothing after it can qualify, and the walk can stop.
        //
        // **This prunes the tail, not the head.** A page whose min_key is
        // below `low` may still hold qualifying rows, and nothing here can
        // tell without looking - skipping leading pages needs a seek to the
        // first qualifying leaf, which `BtreeLookup` is close to providing
        // and the heap chain would need lookahead for. So a range near the
        // start of a relation is cheap and one near the end is not, which
        // is worth knowing before quoting a number.
        //
        // Rows below `low` are dropped by the residual, which still carries
        // both bounds (step_chain.hpp) - so this is an accelerator that
        // cannot change the answer even if the pruning were wrong.
        const std::uint64_t range_high =
            step.range.has_value() ? step.range->high : 0;
        const bool pruning = step.range.has_value();

        // The PageId used to be discarded here. It is the tuple's address,
        // and a trail that has to re-derive an address is a trail that has
        // to search for it - which is the search the trail exists to avoid
        // (workplan P09).
        PageId walk_last_page = kInvalidPageId;
        // The page whose rows have already been emitted whole, in key order.
        // Only used when `emit_in_key_order` is set - see below.
        PageId ordered_page = kInvalidPageId;
        std::vector<std::pair<std::uint64_t, std::uint16_t>> by_key;

        // The access the visitor reads, through a pointer rather than the
        // parameter: a park at the page boundary can cross a catalog
        // invalidation, after which the re-Bind below points this at the
        // refilled entry while the parameter's referent is freed memory.
        const catalog::TableAccess* live_access = &access;

        // The quadratic-shape signal (step_vm.hpp): a sub-chain's driving
        // step actually walking its relation, once per evaluation. Counted
        // here rather than from the compiled kind, so a kFilterScan driver
        // and a cabined miss count - the statement really walked - and a
        // served probe, which walks nothing, does not. Counted rather than
        // refused: quadratic is legitimate over small relations, and the
        // budget bounds it over large ones.
        if (record_through_stops_ && index == 0) {
            ++stats_.For(step.step_id).correlated_scans;
        }

        // Whether a stop ends this walk. Normally yes (V03; the quota's
        // bounded-work property rides on it) - the one exception is
        // `completing_recording_`'s license, held by the walk whose own
        // recording is live, and read per call because the entry-cap lapse
        // can revoke it mid-flight. The step-id refinement scopes it: a
        // deeper step walking *under* a recording stops normally.
        const auto walk_ends_on_stop = [&] {
            return stopped_ && !(completing_recording_ && recording_ != nullptr &&
                                 recording_->step_id == step.step_id);
        };

        // A stop the walk itself decided, recorded as it is taken: every
        // stop return goes through here so `cut` cannot fall out of step
        // with the mark it qualifies.
        const auto stop_here = [&] {
            cut = true;
            return storage::VisitControl::kStop;
        };

        auto visitor = [&](PageId page_id, heap::PageView& page,
                           std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (walk_ends_on_stop()) return stop_here();

            // The walk's page count, exact by construction: the chain hands
            // the visitor one page's slots consecutively, so a transition
            // is a fetch (physical-optimizer.md §II.2 S2).
            if (page_id != walk_last_page) {
                ++stats_.For(step.step_id).pages_fetched;
                walk_last_page = page_id;
                visited_on_page = 0;
            }

            // Checked per slot rather than per page because the walk has no
            // per-page hook; it costs one compare against a header field
            // already in cache, and it fires on the first slot of the first
            // page past the range.
            if (pruning && page.min_key() > range_high) {
                ++stats_.For(step.step_id).range_pages_pruned;
                return stop_here();
            }

            // One row of this page, in the page's own emission order, with
            // JB6's skip and mark applied. **Every accepted tuple of a walk
            // goes through here**, so the ordinal a resume skips cannot
            // drift from the ordinal a mark recorded - that drift would
            // silently drop rows or emit them twice, and no test of a
            // completed walk would see it.
            const auto accept = [&](std::uint16_t at) -> Status {
                // One branch, loop-invariant, for every walk in the engine
                // that is not a prefix: the ordinal is not even counted.
                // The walk is the hottest path there is, and JB5's gate
                // priced a two-nanosecond per-row guard as visible.
                if (!prefixed) {
                    return AcceptTupleAt(steps, index, step, *live_access, page_id, page, at);
                }
                const std::uint32_t ordinal = visited_on_page++;
                if (page_id == resume_page && ordinal < resume_visited) {
                    // Covered by an earlier walk of this step: the map holds
                    // whatever it qualified for, and re-examining it is the
                    // double visit spec §6's economics forbids.
                    return Status::OK();
                }
                Status s = AcceptTupleAt(steps, index, step, *live_access, page_id, page, at);
                if (!s.ok() || mark_frozen) return s;
                if (extending && building_->over_cap) {
                    // The cap stopped the map *at* this row, which was
                    // visited and not bucketed - so the mark may not claim
                    // it, and stops here for the statement. Spec §6's
                    // capped form: the prefix keeps serving, and every
                    // later miss walks from this point.
                    mark_frozen = true;
                    return s;
                }
                prefix->mark = WalkMark{page_id, visited_on_page};
                return s;
            };

            // ---- Emitting a page in key order (step_chain.hpp) -----------
            //
            // The walk has no per-page hook, so the first slot of a page
            // stands in for one: it emits every live slot of that page in key
            // order and marks the page done, and the remaining slots of the
            // same page are then no-ops. Across pages nothing changes -
            // they are already visited in ascending `min_key`.
            if (step.emit_in_key_order) {
                if (page_id == ordered_page) return storage::VisitControl::kContinue;

                by_key.clear();
                const std::uint16_t n = page.slot_count();
                by_key.reserve(n);
                for (std::uint16_t i = 0; i < n; ++i) {
                    auto payload = page.PayloadAt(i, n);
                    if (!payload.ok()) continue;  // retired or out-of-range slot
                    auto id = KeystoneIdOfPayload(payload.value());
                    if (!id.ok()) {
                        inner = id.status();
                        return id.status();
                    }
                    by_key.emplace_back(id.value(), i);
                }
                std::sort(by_key.begin(), by_key.end());

                // Marked before emitting, not after: AcceptTupleAt can stop
                // the walk mid-page (a LIMIT filling up), and a page left
                // unmarked would be re-emitted from its next slot.
                ordered_page = page_id;
                for (const auto& [key, ordered_slot] : by_key) {
                    auto ok = accept(ordered_slot);
                    if (!ok.ok()) {
                        inner = ok;
                        return ok;
                    }
                    if (walk_ends_on_stop()) return stop_here();
                }
                return storage::VisitControl::kContinue;
            }

            auto accepted = accept(slot);
            if (!accepted.ok()) {
                inner = accepted;
                return accepted;
            }
            return walk_ends_on_stop() ? stop_here() : storage::VisitControl::kContinue;
        };

        NoteFetch();

        // Converted to std::function once, outside the page loop: the
        // primitives take `const std::function&`, and re-wrapping the
        // by-reference lambda per page would be a heap allocation per page
        // where the whole-chain walk paid one per walk.
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>
            visit_fn = visitor;

        // ---- The head seek (kRange on a clustered relation) --------------
        //
        // Tail pruning above ends the walk; this is what stops it starting
        // at the beginning. A `BETWEEN` on a btree relation descends to the
        // leaf holding the low bound and walks siblings from there, so a
        // range near the end of the relation costs the range rather than
        // everything before it - measured at 44% of a full scan when the
        // bound is drawn uniformly (bench/results-scenario1-vs-pg.md).
        //
        // **A heap relation still starts at the head**, and that is not an
        // oversight: it has no index to descend, so finding the low bound
        // *is* the walk. The residual carries both bounds either way, which
        // is what makes the seek an accelerator that cannot change the
        // answer - the same property tail pruning rests on.
        const bool is_btree = access.clustered_type == catalog::ClusteredType::kBtree;
        PageId cur = kInvalidPageId;
        if (resume_page != kInvalidPageId) {
            // Resuming (JB6): the mark names the page and the skip above
            // names the row within it, so there is no seek and no leftmost
            // descent - the walk order is the one the mark was taken in,
            // and re-deriving a start would be a second answer to a
            // question already answered.
            cur = resume_page;
        } else if (is_btree) {
            if (pruning) {
                auto first = btree::BtreeSeekLeaf(store_, access.desc_page_id, step.range->low);
                // A descent that failed is a reason to walk, not to fail:
                // the walk is the authoritative path and reaches the same
                // rows.
                if (first.ok()) cur = first.value();
            }
            if (cur == kInvalidPageId) {
                auto first = btree::BtreeLeftmostLeaf(store_, access.desc_page_id);
                if (!first.ok()) co_return first.status();
                cur = first.value();
            }
        } else {
            cur = access.desc_page_id;
        }

        // ---- The page loop, owned by the coroutine (P4d-3) ---------------
        //
        // The walk used to hand the whole chain to storage; now this
        // coroutine steps it page by page. Each *OnePage call holds its
        // page's pin only for the call, so the bottom of this loop - no
        // pin, no span - is the executor's legal suspension point: the
        // pipeline's awaits (credit, cancellation) land exactly there
        // (P4d-4). Nothing suspends yet, which is what keeps this
        // bit-identical to the whole-chain walk it replaces.
        //
        // The first page is visited unconditionally, exactly as the
        // whole-chain forms do: a bad head fails inside the fetch, where
        // testing it here would answer a corrupt catalog row with an empty
        // result instead of an error.
        const PageId walk_origin = cur;
        for (std::uint32_t pages = 0;; ++pages) {
            if (Status s = storage::CheckPageWalkBudget(pages, walk_origin, "relation walk");
                !s.ok()) {
                co_return s;
            }
            // Per page, not once per walk: the VM makes these fetches
            // itself now, so R1's guard gets to see each one.
            NoteFetch();
            auto next = is_btree ? btree::BtreeVisitLeafPage(store_, cur,
                                                             storage::PageAccess::kRead, visit_fn)
                                 : heap::ChainVisitOnePage(store_, cur,
                                                           storage::PageAccess::kRead, visit_fn);
            if (!inner.ok()) co_return inner;
            if (!next.ok()) co_return next.status();
            if (next.value() == kInvalidPageId) {
                // Both endings arrive here - the page primitives answer a
                // visitor stop and a chain end with the same id - and only
                // `cut` tells them apart. A walk that ran out of relation
                // covered all of it, which is what lets a later bucket miss
                // conclude absence; a frozen mark withholds that, because
                // the rows past the cap were visited and never bucketed.
                if (prefixed && !cut && !mark_frozen) prefix->complete = true;
                co_return Status::OK();
            }
            cur = next.value();

            // ---- The page boundary: no pin, no span (P4d-3) --------------
            //
            // The one place a statement may park (P4d-4a): the pipeline's
            // producer waits here for batch credit, and a cancel is seen
            // here at the latest. Outermost walk only - a deeper step runs
            // beneath a visitor through the gated synchronous driver until
            // P4d-4c moves that descent, and consulting the gate there
            // would turn a wait into the driver's hard error.
            if (resume_gate_ != nullptr && index == 0 && !(*resume_gate_)()) {
                co_await sched::WaitUntil{resume_gate_};

                // The park ran other tasks on this core, and any DDL
                // anywhere broadcasts kCatalogInvalidate, whose handler
                // clears the whole TableAccess cache - killing every
                // borrow this runner holds (bound_, schemas_, the frame's
                // schema pointers, the visitor's access). Re-take them: a
                // refill from catalog storage restores the same physical
                // values for a live relation (no DDL can change a live
                // relation's shape), and a relation dropped while we were
                // parked surfaces here as a clean error instead of a read
                // through freed memory. The frame re-opens too - legal at
                // a boundary because the gated shape is single-step, so
                // no outer row is live in it; P4d-4c owns the multi-step
                // form of this seam.
                if (Status s = Bind(steps); !s.ok()) co_return s;
                frame_.Open(schemas_, parent_);
                live_access = bound_[index].access;
            }
        }
    }

    // The location-and-pk half of a recorded entry, shared by the Cabin
    // recording and the build's bucketing - both feed the same 24-byte
    // struct, and the *gate* (a key comparison there, MakeValueKey here)
    // stays at each call site.
    //
    // **The pk comes out of the Keystone word, not out of a column
    // decode.** The id sits at a fixed offset precisely so finding it
    // costs no schema walk (`RowKeystoneId`, row_codec.hpp), and an entry
    // wants the id - not an AstValue holding it. The decode this replaced
    // measured **11.6 ns of the build's 83.7 ns/row constant** on the
    // walked join at 10,000 inner rows (the JB5 gate's follow-up), which
    // is what the recording walk paid per recorded row too. It also drops
    // a signed-to-unsigned guard that could only ever have hidden a
    // corrupt decode: a Keystone id is 40 bits and unsigned by
    // construction.
    //
    // The epoch was captured under this row's span - the hint names the
    // page as it was at observation (R4).
    StatusOr<stats::CabinEntry> EntryForRow(PageId page_id, std::uint16_t slot,
                                            std::uint64_t observed_epoch) {
        auto pk = RowKeystoneId(version_);
        if (!pk.ok()) return pk.status();
        stats::CabinEntry entry;
        entry.pk = pk.value();
        entry.page_id = page_id;
        entry.page_epoch = static_cast<std::uint32_t>(observed_epoch);
        entry.slot = slot;
        entry.flags = stats::kCabinHintValid;
        return entry;
    }

    // Decodes one tuple into this step's frame slots, evaluates the
    // predicates attached to the step, and descends if they hold.
    //
    // **This is where R1 lives.** The span handed in by the walk is
    // registered, used for exactly one decode, and released before
    // anything else can fetch a page.
    Status AcceptTupleAt(const std::vector<Step>& steps, std::size_t index, const Step& step,
                         const catalog::TableAccess& access, PageId page_id,
                         heap::PageView& page, std::uint16_t slot) {
        bool decoded = false;
        // Filled by the decode, drained after the span is released. A
        // spilled value lives in the var-heap and fetching it is a page
        // fetch, so it is exactly the thing that must not happen while the
        // span below is live (row_codec.hpp). Reused across rows so a scan
        // that never spills allocates nothing extra.
        spills_.clear();

        // ---- MVCC, phase 1 (docs/spec/txn.md section 4.3) --------------------
        //
        // **This is the one place visibility is applied**, and every access
        // kind reaches it: the chain walk, the btree descent, the probe
        // memo, a Waystone replay and a Cabin resolve all hand a
        // (page, slot) to this function. That is what makes Waystone
        // section 3.1 rule 2 structural rather than remembered.
        //
        // Stepping back an undo record is a page fetch, and R1 forbids one
        // under the span below - so the classification happens here (no
        // fetch, one integer comparison) and the *walk* happens after the
        // release. A visible writer, which is every row of a
        // single-transaction workload and every catalog row forever, is
        // decided here and pays nothing at all.
        bool needs_walk = false;
        std::uint64_t walk_trx_id = 0;
        std::uint64_t walk_undo_ptr = txn::kNoUndoPtr;
        bool walk_deleted = false;
        // The page's relayout epoch at the moment of access, captured under
        // the span because the trail record below runs after its release -
        // recorded so replay's rule 2 has something real to compare
        // (docs/spec/physical-optimizer.md R4, PX04). One u64 load per
        // accepted tuple.
        std::uint64_t observed_epoch = 0;
        {
            PageSpanGuard span;
            observed_epoch = page.RelayoutEpoch();
            auto tuple = page.ReadTuple(slot);
            if (tuple.ok()) {
                switch (txn::Classify(snapshot_.view, tuple.value())) {
                    case txn::Visibility::kNoVersion:
                        // No version of this tuple exists for this reader.
                        // Not an error and not a row: the statement simply
                        // does not see it.
                        span.Release();
                        return Status::OK();
                    case txn::Visibility::kNeedsUndoWalk:
                        // R1's copy. Fixed-size, because invariant 13 makes
                        // a row's size a schema constant - the same
                        // property that makes relayout a memcpy.
                        version_.assign(tuple.value().payload.begin(),
                                        tuple.value().payload.end());
                        walk_trx_id = tuple.value().trx_id;
                        walk_undo_ptr = tuple.value().undo_ptr;
                        walk_deleted = tuple.value().deleted;
                        needs_walk = true;
                        break;
                    case txn::Visibility::kVisible:
                        // **The row's bytes, copied; the row itself, not
                        // built yet.** The copy is fixed-size (invariant 13)
                        // and costs a memcpy; building the row costs an
                        // 80-byte AstValue per column, and this step may be
                        // about to reject it on one of them. So the bytes
                        // come out from under the span and the decode
                        // happens below, in two halves.
                        version_.assign(tuple.value().payload.begin(),
                                        tuple.value().payload.end());
                        decoded = true;
                        break;
                }
            }
            // Released here, explicitly, while the tuple bytes are still
            // in scope but finished with. Everything below may fetch.
            span.Release();
        }

        // ---- MVCC, phase 2: the walk, with no span live -----------------
        if (needs_walk) {
            if (snapshot_.undo == nullptr) {
                // A view that needs the chain with no log to walk. Reported
                // rather than guessed at: guessing either way invents a row
                // or hides one.
                return Status::InvalidArgument(
                    "a read view that cannot see a tuple's writer was given no undo log");
            }
            auto verdict = txn::ResolveThroughUndo(snapshot_.view, *snapshot_.undo, walk_trx_id,
                                                    walk_deleted, walk_undo_ptr, version_);
            if (!verdict.ok()) return verdict.status();
            if (verdict.value() == txn::Visibility::kNoVersion) return Status::OK();

            // The reconstructed version's bytes are already in `version_`,
            // which is where the decode below reads from - the same buffer
            // the visible path copies into, and for the same reason: the
            // bytes on the page belong to a version this reader is not
            // entitled to.
            decoded = true;
        }

        if (!decoded) return Status::OK();  // dead or out-of-range slot

        // ---- Decode what the filter reads, and only that ----------------
        //
        // `Step::filter_columns` is the compiler's answer to "which of this
        // relation's columns does the residual look at" (step_chain.hpp).
        // Decoding those, testing, and building the rest only for a row that
        // survives is what separates *reading* a row from *materialising*
        // one: a 12-column scan returning 8 rows of 60,480 was paying twelve
        // AstValues on every row it rejected, which measured as 75% of the
        // scan (bench/results-scenario1-vs-pg.md).
        //
        // A step with a sub-chain, or a relation wider than 64 columns,
        // carries kAllColumns and takes the whole row here as before.
        const std::span<parser::AstValue> slots =
            frame_.SlotsFor(static_cast<std::uint16_t>(index));
        // **Recording a Cabin adds nothing to this decode.** The cabined
        // equality *is* this step's filter, so the key column already sits
        // in `filter_columns` (the invariant WalkAndRecord's guard checks);
        // the pk is decoded on demand below, only for a row whose key
        // matches. The full-decode form this replaced priced a recording
        // walk at 2x the FilterScan it shadows
        // (bench/results-scenario3-library.md §7a).
        const bool recording_here =
            recording_ != nullptr && recording_->step_id == step.step_id;
        const bool partial = step.filter_columns != Step::kAllColumns;
        if (partial) {
            if (Status s = DecodeColumnsInto(access.schema, access.layout, version_, slots,
                                             step.filter_columns, &spills_);
                !s.ok()) {
                return s;
            }
        } else if (Status s = DecodeRowInto(access.schema, access.layout, version_, slots,
                                            &spills_);
                   !s.ok()) {
            return s;
        }

        // Now that nothing is live, the spilled values can be resolved.
        if (!spills_.empty()) {
            stats_.For(step.step_id).spill_fetches += spills_.size();
            if (Status s = ResolveSpills(store_, spills_, slots); !s.ok()) return s;
        }

        StepStats& step_stats = stats_.For(step.step_id);
        ++step_stats.rows_examined;

        // ---- Cabin recording (docs/spec/cabin.md §4's miss path) ---------
        //
        // **Before the residual, and that is the whole subtlety.** The set
        // being recorded is the set of rows whose *key column* equals the
        // probed value - not the set of rows this statement wants. A
        // statement is `WHERE sym = 'AAPL' AND qty > 5`; the Cabin's entry
        // set for 'AAPL' must hold every row with that sym, or the next
        // statement asking only `WHERE sym = 'AAPL'` would be served a set
        // that is missing rows and told it is authoritative.
        //
        // So exactly one conjunct is evaluated here, the cabin's own, and
        // the rest of the residual continues to decide what this statement
        // emits - which happens below and changes nothing about what was
        // recorded.
        if (recording_here) {
            // `slots` is this step's own frame span and the recording is
            // scoped to this step by its id, so the key column is read
            // directly rather than through a per-row frame resolution. One
            // comparison per walked row is the recording's whole per-row
            // cost.
            if (recording_->col_pos < slots.size() &&
                CompareValues(/*type_val=*/0, slots[recording_->col_pos], *recording_->value,
                              parser::CompareOp::kEq)) {
                auto entry = EntryForRow(page_id, slot, observed_epoch);
                if (!entry.ok()) return entry.status();
                recording_->entries.push_back(entry.value());
                // The entry-cap lapse: once the set is past what Commit can
                // accept, walking on for it is doomed work, so the
                // completion license is revoked - the next stop check ends
                // the walk and the guard refuses the partial set. A walk
                // the sink never stops is untouched: it is the statement's
                // own answer and runs to the end regardless, with Commit
                // refusing at the cap exactly as before.
                if (completing_recording_ &&
                    recording_->entries.size() > cabins_->max_entries_per_value()) {
                    completing_recording_ = false;
                    // Marked sticky, not merely counted: an append-only set
                    // can only grow, so re-attempting on the next probe is
                    // doomed work re-armed - MayObserve refuses the key
                    // until the store's clear signals say the world moved.
                    cabins_->NoteEntryCapRefusal(*recording_->key);
                }
            }

            // Completion mode: the statement is already stopped and this
            // row was visited for the recording above alone - **uncharged**,
            // because a completion row is the Cabin's work and billing the
            // statement for it turned a within-budget statement into
            // ResourceExhausted, the result change §1 forbids (the work is
            // bounded per key: a commitable set completes once ever, and
            // the caps' gate and sticky mark stop the doomed forms). The
            // check lives *inside* `recording_here` so a plain walk pays
            // nothing for the feature: `stopped_` can only be true here
            // while this step's own recording is live - the walk visitor
            // and both phase-2 loops gate on it otherwise, and a stopped
            // recording walk never descends, so no other step's
            // AcceptTupleAt runs at all. Nothing further may run for this
            // row - not the residual, not a sub-chain, least of all a
            // second emit.
            if (stopped_) return Status::OK();
        }

        // Charged where the tuple was actually decoded, which is the unit
        // that tracks work: a page fetch amortizes over its tuples, but
        // every tuple is decoded and filtered on its own. Below the
        // completion exit, so it is unconditional again on the plain walk -
        // the guard form measured as ~2 ns on every examined row of every
        // walk in the engine (bench/results-scenario3-library.md §7c.5).
        if (Status s = budget_.ChargeRow(); !s.ok()) return s;

        // ---- The inner build's bucketing (workplan JB3) -----------------
        //
        // Split evaluation while this step's build is live: the
        // non-correlated conjuncts decide *bucketing*, the correlated one
        // then decides *emission*. On a well-formed chain the two forms
        // give one verdict - AND over total predicates commutes, and both
        // run the same EvaluateConjunct body, so the split cannot drift
        // from the whole. (On a malformed chain they may name *different*
        // Corruption sites, as any short-circuiting conjunction already
        // may.) The map holds every row passing the non-correlated
        // residual (spec §2): a row failing the current outer key's
        // equality emits nothing today and is precisely what a later
        // outer row's probe will ask for.
        const bool building_here = building_ != nullptr && building_->step_id == step.step_id;
        auto matched = building_here
                           ? EvaluateAllExcept(schemas_, step.residual, frame_,
                                               building_->residual_pos)
                           : EvaluateAll(schemas_, step.residual, frame_);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return Status::OK();
        if (building_here) {
            // The cap (workplan JB5): a row that would push the map past
            // `max_rows` trips `over_cap` instead of entering - the rest
            // of this walk stops bucketing and the publish site declines
            // the step. Emission is untouched: this row, and every later
            // one, still answers through the conjuncts below. The key is
            // made *before* the cap is consulted, so an unkeyable row - a
            // NULL keys nothing, no equality matches it (MakeValueKey's
            // contract) - neither consumes a slot nor trips a cap it
            // never pressed. Real since null.md landed (NU1-NU8,
            // 2026-08-20); the contract suite's NULL case pins it.
            if (!building_->over_cap) {
                if (auto key = stats::MakeValueKey(slots[building_->col_pos]);
                    key.has_value()) {
                    if (building_->map->rows() >= building_->max_rows) {
                        building_->over_cap = true;
                    } else {
                        auto entry = EntryForRow(page_id, slot, observed_epoch);
                        if (!entry.ok()) return entry.status();
                        // A refused Add is the map's own index limit
                        // (inner_build.hpp), reached only by a
                        // `join_build_max_rows` above 2^32-1. It takes the
                        // cap's verdict rather than a private one: a row
                        // the map could not store must never leave a
                        // published map claiming to hold the relation.
                        if (building_->map->Add(*key, entry.value())) {
                            ++step_stats.build_rows;
                        } else {
                            building_->over_cap = true;
                        }
                    }
                }
            }

            // The correlated conjunct alone decides emission from here on.
            auto emit =
                EvaluateConjunct(schemas_, step.residual[building_->residual_pos], frame_);
            if (!emit.ok()) return emit.status();
            if (!emit.value()) return Status::OK();
        }

        // ---- The row survived: build the rest of what is read ------------
        //
        // Everything past this point reads columns the filter had no reason
        // to touch - the projection, the next step's probe key, the trail's
        // pk, the fold's items. Until now those slots still hold the
        // *previous* row's values, so this is not an optimization to skip:
        // it is what makes the partial decode above correct.
        //
        // **`read_columns`, not everything else** (workplan AP01). The
        // filter's mask narrows the decode of a row about to be *rejected*
        // and does nothing for one that survives, so a statement with no
        // WHERE got no benefit at all - every row survived and every row
        // decoded every column. `SELECT COUNT(*) FROM t` built an AstValue
        // per column per row to fold none of them, which made adding a
        // predicate 3.1x *faster* than not having one
        // (`bench/results-aggregate.md`).
        //
        // A step that reads nothing further - a bare `COUNT(*)` over a
        // relation with no predicate - decodes nothing here, and the walk
        // becomes page iteration and a counter.
        const std::uint64_t rest = step.read_columns & ~step.filter_columns;
        if (partial && rest != 0) {
            if (Status s = DecodeColumnsInto(access.schema, access.layout, version_, slots,
                                             rest, &spills_);
                !s.ok()) {
                return s;
            }
            if (!spills_.empty()) {
                stats_.For(step.step_id).spill_fetches += spills_.size();
                if (Status s = ResolveSpills(store_, spills_, slots); !s.ok()) return s;
            }
        }

        // Counted after the residual and *before* the sub-chains: this is
        // the ordinary-predicate selectivity, and folding a sub-chain's
        // verdict into it would make a step reading ten rows to keep one
        // indistinguishable from a step whose subquery rejected nine.
        ++step_stats.rows_matched;

        // Correlated sub-chains attached to this step, evaluated only
        // once the ordinary predicates have accepted the row - a
        // sub-chain is the expensive conjunct, so a cheap one that
        // already rejected the row should never pay for it.
        for (const SubChain& sub : step.sub_chains) {
            auto value = EvaluateSubChain(sub, frame_);
            if (!value.ok()) return value.status();
            if (!Collapse(value.value())) return Status::OK();
        }

        // ---- The trail (workplan P09) ------------------------------------
        //
        // Recorded here, once the row has survived every conjunct attached
        // to this step: an entry naming a tuple the statement rejected
        // would point a future replay at a row it must then discard, which
        // is worse than no entry at all.
        //
        // **Only trail-replayable steps.** Invariant 9 lets a trail replace
        // a lookup and never a search, so a kScan step's rows could only
        // ever be prefetched - and nothing prefetches. Recording them would
        // be paying a write per scanned row for a read nobody makes. The
        // test is the step's *kind*, not how the row was found: a kLookup
        // on a heap relation falls through to a chain walk, and that is the
        // case spec section 7 says pays off most.
        if (trail_ != nullptr && IsTrailReplayable(step.kind)) {
            TouchedTuple touched;
            touched.rel_oid = access.oid;
            // Column 0 is the Keystone pk, already decoded into the frame
            // by this step - so the id costs a read, never a lookup.
            const parser::AstValue& pk =
                frame_.Get(ColumnRef{0, static_cast<std::uint16_t>(index), 0});
            touched.pk = pk.int_val < 0 ? 0 : static_cast<std::uint64_t>(pk.int_val);
            touched.page_id = page_id;
            touched.page_epoch = observed_epoch;
            touched.slot = slot;
            touched.step_id = static_cast<std::uint16_t>(step.step_id);
            trail_->Add(touched);
        }

        if (index + 1 == steps.size()) {
            // Terminal: the emit is a plain call, so a single-step scan
            // allocates zero frames per row. This is where P4d-4's remote
            // forward will buffer the row instead; the await it needs
            // happens at the page boundary above (workplan §3), never here.
            return EmitRow();
        }
        // A deeper local step: driven synchronously until P4d-4 batches
        // rows across this edge. One frame per row on multi-step chains
        // only - the shape the pipeline rebuilds anyway.
        return RunToCompletionAtWalkBoundary(RunStep(steps, index + 1));
    }

    catalog::Catalog& catalog_;
    storage::PageStore& store_;

    // Scratch for AcceptTupleAt()'s decode, reused across rows so a scan
    // that spills nothing allocates nothing for the possibility.
    std::vector<PendingSpill> spills_;

    // The read view every tuple is filtered through, and the log an
    // invisible writer is stepped back through (docs/spec/txn.md section 4).
    // Held by value: a Snapshot is a POD and copying one is cheaper than
    // a pointer indirection on every row.
    txn::Snapshot snapshot_;

    // Where an invisible writer's visible version is reconstructed. Reused
    // across rows, and **touched only when the undo chain is actually
    // walked** - a scan whose rows are all visible never writes a byte
    // here, which is what keeps the R1 copy off the common path.
    std::vector<std::byte> version_;
    const RowSink& sink_;
    std::uint32_t depth_;
    const ChainFrame* parent_;
    ExecStats& stats_;

    // One budget for the whole statement, shared by reference with every
    // sub-chain: a per-chain budget would let a correlated subquery spend
    // the full allowance once per outer row, which is exactly the shape
    // the budget exists to bound.
    Budget& budget_;

    // Where trail-replayable steps report the tuples they accepted, or
    // null when nothing is recording. Shared with every sub-chain for the
    // reason given at its construction. Non-owning: the caller outlives
    // the execution.
    TrailCollector* trail_ = nullptr;

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
    //
    // **The memo is per step, and `memo_step_` is what makes that true.**
    // The paragraph above reasons about one probe step descending for its
    // outer row's key - but a ChainRunner owns one memo and runs *every*
    // step of its chain through it. Without the step in the key, a chain
    // with two pk-descending steps whose keys happen to coincide - say a
    // `WHERE c.id = 7` lookup followed by a probe into another relation on
    // `c.parent_id`, when that row's parent_id is also 7 - serves the
    // second step the *first* relation's location. The row is then decoded
    // with the wrong schema.
    //
    // The failure is data-dependent and rare (it needs two keys to
    // collide), and its two outcomes are not equally visible: relations of
    // different row widths give a Corruption error, and relations of the
    // *same* width silently return a row from the wrong table. Found by
    // tools/join_benchmark.py, which hit it once in 1000 point joins over
    // 2000 rows - almost exactly the 1/2000 the coincidence predicts.
    // The statement's recorded trail, or null when nothing is replaying.
    // Shared with every sub-chain for the reason the collector is: step ids
    // are global across the statement.
    const TrailReplay* replay_ = nullptr;

    // Whether TryReplay() served the row, so the caller knows not to
    // descend. Scratch, valid only across that call and its return.
    bool replayed_ = false;

    bool memo_valid_ = false;
    std::uint32_t memo_step_ = 0;
    std::uint64_t memo_key_ = 0;
    PageId memo_page_ = kInvalidPageId;
    std::uint16_t memo_slot_ = 0;

    // ---- Cabin (docs/spec/cabin.md) --------------------------------------
    //
    // The core-local observed sets, or null when no Cabin is configured.
    // Shared with every sub-chain for the reason the collector and the
    // replay index are: a Cabin belongs to a relation, and a sub-chain reads
    // relations too. Non-owning; the caller outlives the execution.
    stats::CabinStore* cabins_ = nullptr;

    // A recording in progress: which step is collecting, and into what.
    //
    // Non-null only for the duration of one recording walk, and checked
    // against `step_id` at every accepted tuple - a chain may hold several
    // cabin steps, and a nested one may run while an outer one is
    // recording. Pointing at a stack local of RunCabinStep is safe for
    // exactly the same reason the walk's own visitor lambda is: the walk
    // cannot outlive the call that started it.
    struct Recording {
        std::uint32_t step_id = 0;
        std::uint16_t col_pos = 0;
        const parser::AstValue* value = nullptr;
        // The set's key, for the entry-cap lapse to mark it sticky in the
        // store. Points at WalkAndRecord's parameter, which outlives the
        // walk for the reason `value` does.
        const stats::CabinKey* key = nullptr;
        std::vector<stats::CabinEntry> entries;
    };
    Recording* recording_ = nullptr;

    // The completion license for the walk whose recording `recording_` is,
    // saved and restored in lockstep with it. True only while
    // `record_through_stops_` (sub-chain mode) held at the walk's start
    // and the entry-cap lapse has not revoked it; the walk's stop checks
    // and WalkAndRecord's commit guard both read it, which is what keeps
    // "may walk past a stop" and "may commit a stopped walk" one fact.
    bool completing_recording_ = false;

    // The build in progress - WalkAndBuild's stack state, exactly as
    // `Recording` is WalkAndRecord's: non-null only for the duration of
    // one building walk, checked against `step_id` at every accepted
    // tuple, saved and restored around the walk because a deeper
    // annotated step may build while an outer one is.
    //
    // `over_cap` is how the cap crosses AcceptTupleAt's Status boundary
    // (JB5): set at the bucketing site, read beside the publish rule.
    // Never signalled by clearing `building_` - that pointer also drives
    // the split evaluation and must stay stable for the walk's lifetime.
    struct BuildRecording {
        std::uint32_t step_id = 0;
        std::uint16_t col_pos = 0;       // the join column the map keys on
        std::uint16_t residual_pos = 0;  // the correlated conjunct, excluded
        std::size_t max_rows = 0;        // the statement's join_build_max_rows
        bool over_cap = false;
        InnerBuild* map = nullptr;
    };
    BuildRecording* building_ = nullptr;

    // Phase 1's output (see ServeFromCabin): the verified locations, held
    // until every entry has been resolved so that the heap fallback can
    // still abandon without having emitted a row.
    struct Located {
        std::uint64_t pk = 0;
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
    };
    std::vector<Located> serve_scratch_;
    std::unordered_set<std::uint64_t> seen_pks_;

    // RunIndexStep's phase-1 output and its covered-column decode slot. Held
    // on the runner rather than built per step, for TrailCollector's reason:
    // a per-statement allocation on the read path measured as most of an 18%
    // regression once.
    std::vector<std::uint64_t> index_scratch_;
    parser::AstValue covered_scratch_;

    // A correlated index probe's per-row bounds (IndexProbe::key_from): the
    // compile-time padding templates with the outer row's value encoded into
    // the leading width. On the runner for index_scratch_'s allocation
    // reason, and safe against its re-entrancy hazard by a narrower
    // argument: these are read only inside phase 1's leaf visit, which never
    // re-enters RunStep - phase 2, which does, no longer touches them.
    std::vector<std::byte> corr_low_;
    std::vector<std::byte> corr_high_;

    std::vector<Bound> bound_;
    std::vector<const catalog::Schema*> schemas_;
    ChainFrame frame_;
    bool stopped_ = false;
    bool indexes_ = true;

    // Sub-chain mode (EvaluateSubChain sets it on the runner it builds):
    // every stop in a sub-chain is its own short-circuit - no quota exists
    // at subquery depth (V09) - so a stop decides the answer without
    // bounding the work a *recording* is entitled to. Its second consumer
    // is the `correlated_scans` counter, which uses it as the "this walk
    // is a sub-chain's driving walk" fact. With this set, a
    // walk carrying a live recording runs to completion past a stop,
    // visiting the remaining rows for the recording block alone, and
    // WalkAndRecord may then commit the whole set. Never set on a
    // top-level runner: there a stop can be a quota, whose bounded-work
    // property (pagination_exec_test) a completed walk would break.
    bool record_through_stops_ = false;

    // The page-boundary resume gate (workplan-crosscore.md P4d-4a). Null
    // for every local statement. When set, the outermost walk consults it
    // at each page boundary and parks - holding no pin and no span, which
    // P4d-3 made structural - until it answers true. It is a pointer to a
    // caller-owned predicate for WaitFor's lifetime reason: the poller
    // re-reads it while this runner sits suspended in its frame.
    const std::function<bool()>* resume_gate_ = nullptr;

    // ---- The statement-local inner build (workplan JB3/JB5/JB6) ----------
    //
    // The store and its state types are above the class, because the
    // constructor takes one: the statement owns it, every nested runner
    // borrows it. Owned only by a top-level runner; a nested one leaves
    // `owned_builds_` empty and points `builds_` at its parent's.
    InnerBuildStore owned_builds_;
    InnerBuildStore* builds_ = nullptr;
};

// The highest step_id anywhere under `step`/`chain`, sub-chains included.
// step_ids are global across the statement (step_chain.hpp), so this is
// how many slots the per-step stats vector needs.
std::uint32_t MaxStepId(const Step& step);

std::uint32_t MaxStepId(const SubChain& sub) {
    std::uint32_t max = 0;
    for (const Step& step : sub.steps) max = std::max(max, MaxStepId(step));
    return max;
}

std::uint32_t MaxStepId(const Step& step) {
    std::uint32_t max = step.step_id;
    for (const SubChain& sub : step.sub_chains) max = std::max(max, MaxStepId(sub));
    return max;
}

std::uint32_t MaxStepId(const StepChain& chain) {
    std::uint32_t max = 0;
    for (const SubChain& sub : chain.hoisted) max = std::max(max, MaxStepId(sub));
    for (const Step& step : chain.steps) max = std::max(max, MaxStepId(step));
    return max;
}

}  // namespace

StepStats& StepStats::operator+=(const StepStats& other) noexcept {
    relation_opens += other.relation_opens;
    rows_examined += other.rows_examined;
    rows_matched += other.rows_matched;
    sub_chain_runs += other.sub_chain_runs;
    probe_memo_hits += other.probe_memo_hits;
    correlated_scans += other.correlated_scans;
    spill_fetches += other.spill_fetches;
    pages_fetched += other.pages_fetched;
    trail_replays += other.trail_replays;
    trail_misses += other.trail_misses;
    range_pages_pruned += other.range_pages_pruned;
    // The index trio was missing from this sum until 2026-08-09, so
    // Total() silently dropped them - found while adding pages_fetched,
    // fixed beside it.
    index_entries_scanned += other.index_entries_scanned;
    index_entries_filtered += other.index_entries_filtered;
    index_rows_resolved += other.index_rows_resolved;
    cabin_hits += other.cabin_hits;
    cabin_misses += other.cabin_misses;
    cabin_entries_served += other.cabin_entries_served;
    cabin_hint_hits += other.cabin_hint_hits;
    cabin_hint_misses += other.cabin_hint_misses;
    cabin_recordings += other.cabin_recordings;
    inner_builds += other.inner_builds;
    build_rows += other.build_rows;
    build_probes += other.build_probes;
    return *this;
}

StepStats& ExecStats::For(std::uint32_t step_id) {
    if (step_id >= steps.size()) steps.resize(step_id + 1);
    return steps[step_id];
}

StepStats ExecStats::Total() const noexcept {
    StepStats total;
    for (const StepStats& step : steps) total += step;
    return total;
}

bool PageSpanGuardTripped() noexcept { return g_guard_tripped; }
void ResetPageSpanGuard() noexcept { g_guard_tripped = false; }

int LivePageSpans() noexcept { return g_live_spans; }

// The installing core's store, for the audit's pin check. Thread-local
// for the reason g_live_spans is: the audit is installed per core, on the
// core's own thread, and each core has its own store.
thread_local const storage::PageStore* g_audit_store = nullptr;

void InstallSuspendAudit(const storage::PageStore* store) noexcept {
    // The executor's answer to "is it safe to be suspended right now?".
    //
    // R1 already forbids a page *fetch* under a live span, because nothing
    // pins the frame the span points into. Suspending under one is strictly
    // worse: the span stays live for arbitrary wall time, across every other
    // statement that runs on this core in between - so a store that ever
    // evicts turns it from a latent bug into a routine one.
    //
    // The pin half is the same rule one layer down (P4d-3): a span is a
    // window into a pinned frame, and a suspension holding *any* pin keeps
    // a frame unreclaimable for arbitrary wall time. Every legal
    // suspension point - between statements, between pages of a walk -
    // holds no pin, so the check is an equality with zero, not a budget.
    //
    // Installed here rather than checked here: `sched/` sits below this
    // layer and must not know what a page is (coro.hpp's SuspendAuditFn).
    g_audit_store = store;
    sched::SetSuspendAudit([]() -> std::string_view {
        if (g_live_spans > 0) {
            return "a coroutine suspended while holding a page span (parser-v2.md I15 R1)";
        }
        if (g_audit_store != nullptr && g_audit_store->live_pins() != 0) {
            return "a coroutine suspended while holding a page pin "
                   "(workplan-crosscore.md P4d-3)";
        }
        return {};
    });
}

void UninstallSuspendAudit() noexcept {
    g_audit_store = nullptr;
    sched::SetSuspendAudit(nullptr);
}

StatusOr<bool> EvaluateConjuncts(catalog::Catalog& catalog, storage::PageStore& store,
                                 const std::vector<const catalog::Schema*>& schemas,
                                 const Step& step, const ChainFrame& frame, ExecStats* stats,
                                 const Budget& budget, const txn::Snapshot* snapshot) {
    auto matched = EvaluateAll(schemas, step.residual, frame);
    if (!matched.ok()) return matched.status();
    if (!matched.value()) return false;
    if (step.sub_chains.empty()) return true;

    ExecStats local;
    ExecStats& counters = stats != nullptr ? *stats : local;
    counters.For(MaxStepId(step));  // see Execute() for why this is pre-sized
    Budget spend = budget.Fresh();
    // A runner with no sink of its own: it exists only to lend its
    // sub-chain evaluation, which builds its own sink per sub-chain.
    static const RowSink kUnused = nullptr;
    // No collector: this path evaluates one already-located row's
    // conjuncts for UPDATE, which is not a chain execution and has no trail
    // of its own to contribute to.
    ChainRunner runner(catalog, store, kUnused, /*depth=*/0, /*parent=*/nullptr, counters, spend,
                       /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr, snapshot, /*indexes=*/true);

    for (const SubChain& sub : step.sub_chains) {
        auto value = runner.EvaluateSubChain(sub, frame);
        if (!value.ok()) return value.status();
        // The same collapse point every other caller uses.
        if (!Collapse(value.value())) return false;
    }
    return true;
}

sched::Coro ExecuteAsync(catalog::Catalog& catalog, storage::PageStore& store,
                         const StepChain& chain, const RowSink& sink, ExecStats* stats,
                         const Budget& budget, TrailCollector* trail, const TrailReplay* replay,
                         stats::CabinStore* cabins, const txn::Snapshot* snapshot, bool indexes,
                         const std::function<bool()>* resume_gate, const ChainFrame* parent) {
    if (chain.steps.empty()) {
        co_return Status::InvalidArgument("a step chain with no steps reads nothing");
    }

    // Locals live in this coroutine's frame, which outlives every
    // suspension beneath - the same lifetime Execute's stack gave them
    // while nothing suspended.
    ExecStats local;
    ExecStats& counters = stats != nullptr ? *stats : local;

    // Sized once, up front, for the highest step_id the chain contains.
    // Not an optimization: the runner holds a `StepStats&` across nested
    // calls that also touch the stats, and a vector that grew underneath
    // one would leave it dangling. Pre-sizing makes every reference stable
    // for the whole execution, which is a property worth having by
    // construction rather than by reviewing every call site.
    counters.For(MaxStepId(chain));

    // One mutable budget for this statement: the caller's limits and
    // knobs, the spend counter at zero. `Fresh()` owns what survives the
    // copy, so no entry point can drop a field.
    Budget spend = budget.Fresh();

    ChainRunner runner(catalog, store, sink, /*depth=*/parent != nullptr ? 1u : 0u, parent,
                       counters, spend, trail, replay, cabins, snapshot, indexes, resume_gate);

    // Hoisted sub-chains run **once**, before the outer chain opens. An
    // uncorrelated subquery's answer is the same for every outer row by
    // definition, so running it per row would compute one value n times.
    //
    // The consequence worth having: a false uncorrelated `EXISTS` answers
    // the whole statement here, and the outer relation is never opened at
    // all. That is a real saving on a large relation, and it is only
    // available because hoisting is decided structurally at compile.
    //
    // They run synchronously even here: whether a sub-chain may ever
    // await is P4d-4's open decision, and until it is taken they stay on
    // the gated driver exactly as a nested step does.
    if (!chain.hoisted.empty()) {
        // An empty frame with no parent: a hoisted sub-chain refers to
        // nothing outside itself, which is what "uncorrelated" means.
        ChainFrame empty;
        empty.Open({}, /*parent=*/nullptr);
        for (const SubChain& sub : chain.hoisted) {
            auto value = runner.EvaluateSubChain(sub, empty);
            if (!value.ok()) co_return value.status();
            if (!Collapse(value.value())) co_return Status::OK();  // no rows, nothing opened
        }
    }

    co_return co_await runner.Run(chain.steps);
}

Status Execute(catalog::Catalog& catalog, storage::PageStore& store, const StepChain& chain,
               const RowSink& sink, ExecStats* stats, const Budget& budget,
               TrailCollector* trail, const TrailReplay* replay, stats::CabinStore* cabins,
               const txn::Snapshot* snapshot, bool indexes, const ChainFrame* parent) {
    // The synchronous wrapper (P4d-2's staging): with no resume gate
    // nothing beneath can park, so the gated driver completes the
    // coroutine inline and this is bit-identical to the pre-coroutine
    // executor. A caller that wants the walk to actually wait passes a
    // gate to ExecuteAsync and polls the Coro instead.
    return RunToCompletionAtWalkBoundary(ExecuteAsync(catalog, store, chain, sink, stats, budget,
                                                      trail, replay, cabins, snapshot, indexes,
                                                      /*resume_gate=*/nullptr, parent));
}

}  // namespace kds::exec
