#include "kds/storage/page_header.hpp"
#include "kds/exec/heap_store.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/exec/cabin_expr.hpp"

#include "kds/sched/coro.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include <string>

#include "kds/exec/index_key.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/inner_build.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/miss_refusal.hpp"
#include "kds/storage/index/index_tree.hpp"

namespace kds::exec {

// The decoders' "everything" and the compiler's are one value, pinned here
// because this is the only translation unit that sees both declarations
// (row_codec.hpp deliberately knows nothing about a chain).
static_assert(kAllColumnsMask == Step::kAllColumns,
              "row_codec's kAllColumnsMask and Step::kAllColumns must agree");

namespace {

// ---- R1: the page-span guard --------------------------------------------
//
// A relation walk hands the visitor a span into a live page frame. If the
// visitor descends into the next step while still holding it, a page fetch
// happens with that span registered - and nothing pins the frame it points
// into. The pool evicts, so that is a use-after-free that reads as
// occasional wrong rows. A step registers its span, decodes, and releases
// before descending; any fetch made while a span is registered trips the
// guard.
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
// `NOT IN` is not `!IN`: if the subquery result contains a NULL and the
// probe matches nothing, `x NOT IN (S)` is UNKNOWN rather than TRUE, so a
// boolean negation would be wrong (docs/spec/null.md: WHERE is
// three-valued). `TriState`, `Collapse` and `IsNull` live in
// chain_frame.hpp since BU-S1, whose boolean conjuncts collapse through
// the same function.

// A `$param` is a value position with nothing bound to it, so a chain
// carrying one **must never be executed**: any answer it produced would be
// an answer to a statement nobody wrote.
//
// Unreachable since declared patterns were withdrawn - `$x` is now a parse
// error wherever it is written (ast.hpp) - so it is reported as Corruption,
// alongside this file's other malformed-chain checks, and it fails loudly
// rather than treating the placeholder as a non-matching value. A quiet
// false here would turn the defect that produced it into a query that
// silently returns no rows.
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
        // Not a tuple-key-shaped value. Ids are zero-extended and unsigned (invariant
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

// Drives a spine coroutine to completion synchronously (the walk visitor
// call sites and the sync Execute() wrapper). A coroutine that parks on a
// wait beneath it is a hard error: the driver cannot deliver what the wait
// waits for, and resuming past it would fabricate the reply (P4d-3).
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
// resume from. That is this mark (BD-R3 E4,
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`): the leaf the
// walk stopped on and `last_tuple_key`, the last row it covered, and a
// resume skips every row at or below it. The statement writes nothing
// between its walks, but another core may: a leaf places a row where its
// key sorts, so an insert on the marked leaf shifts the rows after it, and
// an ordinal would then skip a row it never covered or cover one twice.
// Keys only move right, so every key above `last_tuple_key` is on the marked
// leaf or to its right, and the resume still starts there.
//
// `page == kInvalidPageId` is "from the head", which is where a first walk
// starts and what an unset mark means.
struct WalkMark {
    PageId page = kInvalidPageId;
    std::uint64_t last_tuple_key = 0;  // 0 is below every id (kFirstRowId)
    // A heap's mark is a place (DN): its rows are in no key order and never
    // move, so what a walk covered on `page` is its slots up to this one.
    std::uint16_t last_slot = 0;
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

// What one statement keeps across the runners it builds per outer row.
struct StatementStore {
    InnerBuildStore builds;
    // A sub-chain's hoisted chains' answers, by chain. A hoisted chain
    // refers to nothing outside itself, so under the statement's one
    // snapshot it answers once; one inside a correlated subquery ran once
    // per outer row without this. A write evaluates its conjuncts through a
    // runner of its own per row (`EvaluateConjuncts`), so only a statement
    // run by `Execute` - a read - keeps an answer past one row.
    std::unordered_map<const SubChain*, bool> hoisted;
    // BM-R6's once, by step id: present once a step's decode shape and
    // residual have been checked in this statement, holding the scan kernel
    // (BM-R4) compiled with the check - an empty one holds no term. One
    // vector, so "checked" and "has a kernel" cannot fall out of step
    // (BT-S3). Here and not in the runner because a sub-chain's runner is
    // rebuilt per outer row, and a flag rebuilt per outer row is a check
    // (and an allocation) per outer row.
    std::vector<std::optional<ScanKernel>> kernels;
};

// One chain's execution state. Recreated per sub-chain, which is what
// makes the frame stack a stack.
class ChainRunner {
public:
    // `statement` is the statement's store, shared the way `stats` and
    // `budget` are: a nested runner is rebuilt per outer row, and a map
    // rebuilt per outer row is not a map (JB6). Null means "this runner is
    // the statement" and it owns one.
    ChainRunner(catalog::Catalog& catalog, storage::PageStore& store, const RowSink& sink,
                std::uint32_t depth, const ChainFrame* parent, ExecStats& stats, Budget& budget,
                TrailCollector* trail, const TrailReplay* replay, stats::CabinStore* cabins,
                const txn::Snapshot* snapshot, bool indexes, StatementStore* statement = nullptr)
        // In declaration order. No initializer dereferences a member
        // (`statement_` only takes `&owned_statement_`'s address), so the
        // list is order-independent; a reorder that dereferenced one would
        // not be.
        : catalog_(catalog), store_(store),
          snapshot_(snapshot != nullptr ? *snapshot : kSeesEverything), sink_(sink),
          depth_(depth), parent_(parent), stats_(stats), budget_(budget), trail_(trail),
          replay_(replay), cabins_(cabins), indexes_(indexes),
          statement_(statement != nullptr ? statement : &owned_statement_) {}

    // Sub-chain mode - see `record_through_stops_`'s comment for what it
    // licenses and why only a sub-chain may have it.
    void RecordThroughStops() { record_through_stops_ = true; }

    // The statement runner's slice (BA-S15, `SlicePolicy`). Never set on a
    // nested runner: only the outermost walk yields.
    void SetSlice(const SlicePolicy* slice) noexcept { slice_ = slice; }

    // The session's cancel, taken at the slice boundary (§10).
    Status TakeCancel() const { return slice_->TakeCancel(); }

    // Whether the walk this runner drives stopped before its end.
    bool stopped() const noexcept { return stopped_; }

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
            if (Status s = StillOwned(*access.value()); !s.ok()) return s;
            bound_.push_back(Bound{access.value()});
            schemas_.push_back(&access.value()->schema);
        }
        return Status::OK();
    }

    // **BF-R10's walk check, once per bind**: the relation's anchor and its
    // first page still carry its oid. A memo that outlived a drop's reclaim -
    // a defect in BF-R8/R9's gate, which is the authority - meets a freed or
    // reused page here, and the statement answers that the relation is gone
    // rather than reading another relation's rows. Every walk and descent
    // starts from these two pages. A system relation has no anchor and is
    // never dropped.
    Status StillOwned(const catalog::TableAccess& access) {
        if (access.anchor_page_id == kInvalidPageId) return Status::OK();
        for (const PageId page : {access.anchor_page_id, access.desc_page_id}) {
            auto bytes = store_.GetForRead(page);
            const bool owned =
                bytes.ok() && storage::GetOwnerOid(bytes.value().bytes()) == access.oid;
            if (!owned) {
                return Status::NotFound("relation oid " + std::to_string(access.oid) +
                                        " no longer exists: its page " + std::to_string(page) +
                                        " was reclaimed");
            }
        }
        return Status::OK();
    }

public:
    // Runs a chain's hoisted sub-chains once each, in order, and says whether
    // every one was true; the first that is not stops the rest. They run
    // against an empty frame with no parent: a hoisted chain refers to
    // nothing outside itself, which is what "uncorrelated" means. The
    // top-level chain's and a sub-chain's are run by this one definition.
    // Each answers once per statement (`StatementStore::hoisted`).
    StatusOr<bool> HoistedAllTrue(const std::vector<SubChain>& hoisted) {
        if (hoisted.empty()) return true;
        std::optional<ChainFrame> empty;
        auto& answers = statement_->hoisted;
        for (const SubChain& sub : hoisted) {
            if (!answers.contains(&sub) && !empty.has_value()) {
                empty.emplace();
                empty->Open({}, /*parent=*/nullptr);
            }
            auto all = AnsweredOnce(sub, empty.has_value() ? &*empty : nullptr);
            if (!all.ok()) return all.status();
            if (!all.value()) return false;
        }
        return true;
    }

    // An uncorrelated sub-chain's collapsed answer, walked the first time it
    // is asked and read from `StatementStore::hoisted` after. Only for a
    // sub-chain whose answer does not vary per row and is never unknown;
    // `frame` is walked against only on that first ask.
    StatusOr<bool> AnsweredOnce(const SubChain& sub, const ChainFrame* frame) {
        auto& answers = statement_->hoisted;
        if (const auto known = answers.find(&sub); known != answers.end()) return known->second;
        auto value = EvaluateSubChain(sub, *frame);
        if (!value.ok()) return value.status();
        // Inserted after the walk: a nested hoisted chain's own answer may
        // have been inserted during it.
        const bool answer = Collapse(value.value());
        answers.emplace(&sub, answer);
        return answer;
    }

    // The step's boolean conjuncts that read a sub-chain (BU-S3), against
    // `outer`. An in-tree sub-chain is never hoisted, but an uncorrelated
    // `EXISTS`/`NOT EXISTS` - never unknown - still answers once per
    // statement, from the store the hoisted ones answer from.
    StatusOr<bool> EvaluateTreeConjuncts(const std::vector<const catalog::Schema*>& schemas,
                                         const Step& step, const ChainFrame& outer) {
        const auto answer = [&](std::uint32_t index) -> StatusOr<TriState> {
            const SubChain& sub = step.sub_chains[index];
            if (sub.correlated || sub.has_value) return EvaluateSubChain(sub, outer);
            auto exists = AnsweredOnce(sub, &outer);
            if (!exists.ok()) return exists.status();
            return exists.value() ? TriState::kTrue : TriState::kFalse;
        };
        const SubChainAnswer sub_chains(answer);
        return EvaluateBoolConjuncts(schemas, step.bool_residual, outer, &sub_chains);
    }

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
            // `correlated_scans` is counted at the walk itself
            // (RunWalkStep), not from the compile-time kind here: a
            // kFilterScan driver walks every run and a correlated
            // CabinProbe walks only on a miss.
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
                        // `UPDATE ... FROM`'s reads a second row, which is
                        // its refusal (BU-Q16).
                        if (sub.at_most_one) {
                            ++scalar_rows;
                            return scalar_rows >= 2 ? storage::VisitControl::kStop
                                                    : storage::VisitControl::kContinue;
                        }
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
                        if (probe == nullptr) {
                            return Status::Corruption("an IN sub-chain has no probe");
                        }
                        // A NULL probe against any row is UNKNOWN for IN
                        // and NOT IN alike, and no later row can change
                        // that, so one row settles it.
                        if (IsNull(*probe)) {
                            saw_null = true;
                            return storage::VisitControl::kStop;
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
                        if (CompareValues(/*type_val=*/0, *probe, candidate,
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
                    // A function conjunct lowers to `fn_residual` (AP-S4).
                    case parser::PredicateKind::kCompareFunction:
                    // A boolean conjunct lowers to `bool_residual` (BU-S1).
                    case parser::PredicateKind::kBoolTree:
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
        // The statement's store (builds and hoisted answers) is shared for
        // the reason the collector and the snapshot are, and one more of its
        // own: this runner is built per outer row, so a map it owned would
        // be filled and destroyed once per outer row - which is the whole
        // cost the map exists to remove (JB6).
        //
        // **What this sub-chain hoisted runs first, once.** One that is not
        // true leaves this sub-chain with no rows, so its steps never open
        // and the accumulators below stay at zero.
        //
        // The empty case is tested here and not left to the helper: this
        // runs once per outer row of a correlated subquery, and building a
        // `StatusOr` to say "nothing hoisted" measured +35 ns a row.
        bool opens = true;
        if (!sub.hoisted.empty()) {
            auto all = HoistedAllTrue(sub.hoisted);
            if (!all.ok()) return all.status();
            opens = all.value();
        }
        if (opens) {
            ChainRunner inner(catalog_, store_, collect, depth_ + 1, &outer, stats_, budget_,
                              trail_, replay_, cabins_, &snapshot_, indexes_, statement_);
            // Sub-chain mode - `record_through_stops_`'s comment carries the
            // argument.
            inner.RecordThroughStops();
            Status ran = RunToCompletionAtWalkBoundary(inner.Run(sub.steps));
            if (!ran.ok()) return ran;
        }

        switch (sub.kind) {
            case parser::PredicateKind::kExists:
                if (sub.at_most_one && scalar_rows > 1) {
                    return Status::CardinalityViolation(
                        "UPDATE ... FROM matched a row with more than one FROM row; which one "
                        "sets it would be a guess (BU-Q16)");
                }
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
            case parser::PredicateKind::kCompareFunction:
            case parser::PredicateKind::kBoolTree:
                break;
        }
        return Status::Corruption("unhandled sub-chain kind");
    }

private:

    // The terminal emit, shared by RunStep's index==size edge and by
    // AcceptTupleAt's inlined terminal descent - the split that keeps a
    // coroutine frame off the per-tuple path (coro.hpp's rule). A row only
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

    // Emits, or descends. The recursion is over *steps*, not over rows:
    // one frame holds every bound relation, so reaching the end means one
    // complete output row is sitting in it.
    sched::Coro RunStep(const std::vector<Step>& steps, std::size_t index) {
        if (stopped_) co_return Status::OK();
        if (index == steps.size()) {
            co_return EmitRow();
        }

        const Step& step = steps[index];
        const catalog::TableAccess& access = *bound_[index].access;

        ++stats_.For(step.step_id).relation_opens;
        if (step.kind == AccessKind::kLookup || step.kind == AccessKind::kProbe) {
            // A key set is on `kLookup` and has no `key` (CU).
            if (step.key_set.has_value()) co_return RunKeySetStep(steps, index, step, access);
            co_return co_await RunPointStep(steps, index, step, access);
        }
        if (step.kind == AccessKind::kCabinProbe) {
            co_return co_await RunCabinStep(steps, index, step, access);
        }
        if (step.kind == AccessKind::kIndexProbe || step.kind == AccessKind::kIndexRange) {
            co_return co_await RunIndexStep(steps, index, step, access);
        }
        // The build annotation's arm (`BuiltWalk`).
        if (step.build.has_value()) co_return co_await BuiltWalk(steps, index, step, access);
        // An elided `ORDER BY <tuple key> DESC` rests on this walk (CV O2).
        if (step.emit_order == EmitOrder::kTupleKeyBackward) {
            co_return co_await RunReverseWalkStep(steps, index, step, access);
        }
        // A kFilterScan walks exactly as a kScan does - the kind is a
        // statistics distinction, not an execution one, and there is
        // deliberately no branch for it here. If one ever appears, the
        // "same rows either way" tests are what should stop it.
        co_return co_await RunWalkStep(steps, index, step, access);
    }

    // A tuple key descent. Its answer is authoritative - a miss means the row
    // does not exist. The compiler also left the key in `residual`, so the
    // row the descent finds is filtered like any other.
    sched::Coro RunPointStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        auto key = KeyFromOperand(*step.key, frame_);
        if (!key.ok()) {
            // A key that cannot match: no row, and not an error.
            if (key.status().code() == StatusCode::kNotFound) co_return Status::OK();
            co_return key.status();
        }
        co_return AcceptByKey(steps, index, step, access, key.value());
    }

    // A key set (CU; `KeySet`, step_chain.hpp): the single-value descent,
    // once per value, ascending - so the step emits in tuple key order, and
    // a value with no visible row emits nothing. Every row found is filtered
    // by the whole residual, the list's own conjunct included (CU-R1); the
    // leaf that named the value is asked first (CU-Q9).
    //
    // No trail is recorded or replayed for it (`IsTrailReplayable(const
    // Step&)`), so `AcceptByKey`'s replay finds no entry. Not a coroutine and
    // never yields (CU-Q8): BU-Q2's cap on a conjunct bounds the values.
    Status RunKeySetStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                         const catalog::TableAccess& access) {
        for (const KeySet::Item& item : step.key_set->items) {
            if (stopped_) break;
            // Set per value, not once: a deeper step's own set overwrites it.
            key_set_leaf_ = item.leaf;
            if (Status s = AcceptByKey(steps, index, step, access, item.key); !s.ok()) return s;
        }
        return Status::OK();
    }

    // One tuple key descent and the row it finds, through the memo, a
    // replayable trail and the tree, in that order.
    Status AcceptByKey(const std::vector<Step>& steps, std::size_t index, const Step& step,
                       const catalog::TableAccess& access, const std::uint64_t key) {
        // The memo, checked before the descent. It holds a *location*,
        // not a row, so a hit re-reads and re-filters exactly what a
        // fresh descent would have handed to the same code - which is
        // what makes "results identical with the memo on and off" a
        // property of the structure rather than of the test data.
        if (memo_valid_ && memo_step_ == step.step_id && memo_key_ == key) {
            NoteFetch();
            ++stats_.For(step.step_id).pages_fetched;
            // Through the one verifier, as a trail or a Cabin hint is:
            // the memo outlives the descent's hold, and a divide
            // renumbers the slot and bumps the epoch (btree.hpp
            // `Location`).
            VerifiedTuple verified =
                VerifyTupleAt(store_, memo_page_, memo_slot_, key, memo_epoch_,
                              access.oid);
            if (verified.ok()) {
                ++stats_.For(step.step_id).probe_memo_hits;
                return AcceptTupleAt(steps, index, step, access, memo_page_,
                                     *verified.page, memo_slot_);
            }
            // Moved or gone. Fall through to the descent rather than
            // fail: the memo is an accelerator, never an oracle.
            memo_valid_ = false;
        }

        // ---- Replay (workplan P11/P13) -----------------------------------
        //
        // Consults the trail for this step's key and, if it validates, reads
        // the row from the recorded location instead of descending for it.
        //
        // **A hit returns from here, and nothing else reports it.** A replay
        // hit runs the next step inside `AcceptTupleAt`, so an outcome kept
        // anywhere the next step reaches is that step's to overwrite: a flag
        // shared by the chain was cleared by an inner miss before the outer
        // step read it, and the outer step descended and emitted the row a
        // second time.
        //
        // **Every miss falls through to the authoritative path for this step
        // alone** (spec section 2 rule 4). A missing trail, a stale entry, a
        // page that went away and a slot whose occupant changed are all the
        // same answer here - the descent below - which is deliberate: a
        // caller that could tell them apart would be tempted to treat one of
        // them as authoritative.
        //
        // Rule 0 is **this lookup** (spec section 2 rule 0, the driving step
        // included). The index is keyed on (step_id, tuple key) and `key` was
        // just derived for this step, so an entry is only found by matching
        // it. There is no separate check to forget.
        if (const TrailLocation* at =
                replay_ != nullptr ? replay_->Find(step.step_id, key) : nullptr;
            at != nullptr) {
            StepStats& step_stats = stats_.For(step.step_id);
            NoteFetch();
            ++step_stats.pages_fetched;
            // Spec section 2 rules 1-2, through the one verifier both this
            // and Cabin's location hints go through (exec/tuple_verify.hpp).
            // Verified against the derived `key`, not an entry tuple key -
            // the location carries none - which is rule 0's second hold.
            VerifiedTuple verified = VerifyTupleAt(store_, at->page_id, at->slot, key,
                                                   at->page_epoch, access.oid);
            if (verified.ok()) {
                // Validated. From here it is the authoritative path's own
                // code on the authoritative path's own bytes - decode,
                // residual, sub-chains, descend - so rule 3's "apply
                // visibility exactly as the authoritative path would" is not
                // implemented here so much as inherited.
                ++step_stats.trail_replays;
                return AcceptTupleAt(steps, index, step, access, at->page_id, *verified.page,
                                        at->slot);
            }
            ++step_stats.trail_misses;
        }

        NoteFetch();
        ++stats_.For(step.step_id).pages_fetched;
        auto found = exec::LocateRow(store_, access, key);
        if (found.ok()) {
            memo_valid_ = true;
            memo_step_ = step.step_id;
            memo_key_ = key;
            memo_page_ = found.value().page_id;
            memo_slot_ = found.value().slot;
            heap::PageView leaf(found.value().leaf.bytes());  // btree.hpp `Location`
            memo_epoch_ = CurrentPageEpoch(leaf);
            return AcceptTupleAt(steps, index, step, access, found.value().page_id, leaf,
                                 found.value().slot);
        }
        if (found.status().code() == StatusCode::kNotFound) return Status::OK();
        return found.status();
    }

    // ---- Cabin (docs/spec/cabin.md §4) -----------------------------------
    //
    // A non-tuple-key equality on a cabined column: probe the Cabin's observed set,
    // and fall back to the walk when the value has not been observed.
    //
    // **What makes this the third trust class** is the hit branch. A trail
    // may only replace a *lookup*, because a stored set of locations has no
    // witness for a row inserted since it was recorded. A Cabin has one -
    // the write hook of §5 - so for an observed value its entry set is a
    // superset of the qualifying tuple keys, and serving from it is authoritative
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

    // One observed value's set and its key, as `ServeFromCabin` takes it.
    struct ServedSet {
        const stats::CabinKey* key;
        const stats::CabinSet* set;
        // A Cabin set's (CU): the conjunct leaf that names this value.
        std::uint32_t leaf = 0;
    };

    // One value a multi-value Cabin step asks, and the list leaf that named
    // it (CU-Q9; 0 where no list did).
    struct AskedValue {
        parser::AstValue value;
        std::uint32_t leaf = 0;
    };

    // A Cabin step over more than one value - a cover's days, a list's
    // items: served from the union of every value's set when **every** one
    // is observed, and walked otherwise - all or nothing, since one
    // unobserved value's rows are in no set. More values than the Cabin's
    // cap (`cabin_max_values`) could never be all observed and are walked
    // without asking, and so is a value the Cabin cannot key, which may
    // still match a row. Nothing is recorded: such a walk names no one
    // value it could bank.
    sched::Coro ServeAllOrWalk(const std::vector<Step>& steps, std::size_t index, const Step& step,
                               const catalog::TableAccess& access,
                               std::vector<AskedValue> values) {
        if (cabins_ == nullptr || values.size() > cabins_->max_values()) {
            co_return co_await RunWalkStep(steps, index, step, access);
        }
        // Keys and handles held here, for the serve that points into them:
        // reserved whole, so no push moves one.
        std::vector<stats::CabinKey> keys;
        std::vector<stats::CabinSet> handles;
        std::vector<ServedSet> sets;
        keys.reserve(values.size());
        handles.reserve(values.size());
        sets.reserve(values.size());
        for (const AskedValue& asked : values) {
            auto key = stats::MakeCabinKey(step.cabin->cabin_id, asked.value);
            if (!key.has_value()) co_return co_await RunWalkStep(steps, index, step, access);
            stats::CabinSet set = cabins_->Find(*key);
            if (!set.valid()) {
                cabins_->NoteMiss(step.cabin->cabin_id);
                ++stats_.For(step.step_id).cabin_misses;
                co_return co_await RunWalkStep(steps, index, step, access);
            }
            keys.push_back(std::move(*key));
            handles.push_back(std::move(set));
            sets.push_back({&keys.back(), &handles.back(), asked.leaf});
        }
        cabins_->NoteHit(step.cabin->cabin_id);
        ++stats_.For(step.step_id).cabin_hits;
        co_return co_await ServeFromCabin(steps, index, step, access, sets);
    }

    // A cover step (AQ-R8): every day of a two-sided range, all or nothing
    // (`ServeAllOrWalk`). The width is asked before a day is made: a cover
    // wider than the Cabin's value cap is walked without building it.
    sched::Coro RunCoverStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        const auto [first, last] = *step.cabin->cover;
        const std::uint64_t width =
            static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first) + 1;
        if (cabins_ == nullptr || width > cabins_->max_values()) {
            co_return co_await RunWalkStep(steps, index, step, access);
        }
        std::vector<AskedValue> days;
        days.reserve(static_cast<std::size_t>(width));
        for (std::int64_t day = first;; ++day) {
            AskedValue asked;
            asked.value.type = parser::ValueType::kInt;
            asked.value.int_val = day;
            days.push_back(std::move(asked));
            if (day == last) break;
        }
        co_return co_await ServeAllOrWalk(steps, index, step, access, std::move(days));
    }

    // A Cabin set (CU; `KeySet`, step_chain.hpp): each value of the list
    // served from its own observed set, the sets unioned and deduplicated by
    // tuple key in `ServeFromCabin` (CU-R2) - all or nothing
    // (`ServeAllOrWalk`). Every row served is filtered by the whole
    // residual, the list's conjunct included (CU-R1), the leaf of the value
    // whose set held it asked first (CU-Q9). The items are distinct by
    // Cabin key since the compile (`CabinSetOf`).
    sched::Coro RunCabinSetStep(const std::vector<Step>& steps, std::size_t index,
                                const Step& step, const catalog::TableAccess& access) {
        const KeySet& list = *step.key_set;
        // A chain the compiler did not make: the walk answers it.
        if (!step.cabin.has_value() || list.conjunct >= step.bool_residual.size()) {
            co_return co_await RunWalkStep(steps, index, step, access);
        }
        const std::vector<BoolPredicate>& leaves = step.bool_residual[list.conjunct].children;
        std::vector<AskedValue> values;
        values.reserve(list.items.size());
        for (const KeySet::Item& item : list.items) {
            if (item.leaf >= leaves.size()) co_return co_await RunWalkStep(steps, index, step, access);
            values.push_back({leaves[item.leaf].compare.rhs.literal, item.leaf});
        }
        co_return co_await ServeAllOrWalk(steps, index, step, access, std::move(values));
    }

    // **The build annotation's arm, in its two forms** - a walked join's
    // step (`RunStep`) and a Cabin probe's miss (`MissWalk`). The test is
    // the *walk's* class and not the predicate that spelled it
    // (`record_through_stops_` is set for every sub-chain runner): a walk a
    // sink can cut needs the prefix rule (spec §6), and one that always runs
    // to the end does not.
    //
    //
    // Both forms are entered here alone, so their two guards are asked here
    // once: `BuildIsArmable`, the structural one, and the off-switch, the
    // A/B lever (JB5) - `join_build_max_rows == 0` maps nothing, marks
    // nothing, and the walk is the walk the statement always ran.
    //
    // **Not a coroutine**, nor is `MissWalk`: each hands back the one
    // coroutine that answers, so neither adds a frame.
    sched::Coro BuiltWalk(const std::vector<Step>& steps, std::size_t index, const Step& step,
                          const catalog::TableAccess& access) {
        if (!BuildIsArmable(step, access) || budget_.join_build_max_rows() == 0) {
            return RunWalkStep(steps, index, step, access);
        }
        if (record_through_stops_) return WalkPrefixAndProbe(steps, index, step, access);
        return WalkAndBuild(steps, index, step, access);
    }

    // **A correlated Cabin probe's miss that records nothing** (DQ E1,
    // join-inner-build.md §5a): a first sighting, a capped value, a
    // recording the banking gate declines, an unkeyable value, no store.
    // Where the step carries the annotation, `BuiltWalk` answers it - on a
    // top-level chain the map, built by the first such miss; in a sub-chain
    // §6's prefix form, frozen past the cap. Otherwise the walk, as before.
    // The recording walk never comes here -
    // its completion license runs through the stop in a walk of its own,
    // which neither reads nor moves the map or its mark (DQ-S0 (ii)) - so
    // the Cabin observes what it observed without the fallback (DQ-R3) and
    // nothing is banked from the map (DQ-R2). A miss of a step with no
    // annotation pays a branch and no frame (DQ-R6).
    sched::Coro MissWalk(const std::vector<Step>& steps, std::size_t index, const Step& step,
                         const catalog::TableAccess& access) {
        if (!step.build.has_value()) return RunWalkStep(steps, index, step, access);
        return BuiltWalk(steps, index, step, access);
    }

    sched::Coro RunCabinStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        if (step.cabin.has_value() && step.cabin->cover.has_value()) {
            co_return co_await RunCoverStep(steps, index, step, access);
        }
        if (step.key_set.has_value()) {
            co_return co_await RunCabinSetStep(steps, index, step, access);
        }
        // **No span rule** (`cabin.md` §4b): every walk here covers the
        // whole relation, so a set is neither banked from nor served to a
        // walk narrower than the relation.

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
        if (!key.has_value()) co_return co_await MissWalk(steps, index, step, access);

        if (const stats::CabinSet set = cabins_->Find(*key); set.valid()) {
            cabins_->NoteHit(step.cabin->cabin_id);
            ++stats_.For(step.step_id).cabin_hits;
            const ServedSet one[] = {{&*key, &set}};
            co_return co_await ServeFromCabin(steps, index, step, access, one);
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
        if (!record) co_return co_await MissWalk(steps, index, step, access);
        co_return co_await WalkAndRecord(steps, index, step, access, *key);
    }

    // Walks `step` with a recording for `key` live, and commits the set iff
    // the walk completed. **The only place a Recording is created** - called
    // from `RunCabinStep`'s miss path alone, which is what keeps every rule
    // below one rule with one home. Every decline before the Recording
    // exists answers the row by `MissWalk`, and the recording walk itself
    // never reads or fills the build's map (DQ-R2).
    sched::Coro WalkAndRecord(const std::vector<Step>& steps, std::size_t index, const Step& step,
                         const catalog::TableAccess& access, const stats::CabinKey& key) {
        // The recording reads the key column out of this row's frame slots,
        // so the partial decode must have filled it. Guaranteed by the
        // compiler - the cabined equality is a residual conjunct, and
        // `filter_columns` is derived from that same residual - and checked
        // here because the failure would not be an error: it would be a
        // stale slot recorded into a set that is then served as
        // authoritative while missing qualifying tuple keys, the C1 break.
        // Declining to record is always legal (§1's corollary): this walk
        // still answers, and the value simply stays unobserved.
        // An expression or predicate Cabin reads every input of `F`, each of
        // which is read by a conjunct the probe was matched against, and so in
        // `filter_columns` by the same derivation (AQ-R5, R6).
        const auto decoded = [&](std::uint16_t pos) {
            return step.filter_columns == Step::kAllColumns ||
                   (pos < 64 && ((step.filter_columns >> pos) & 1) != 0);
        };
        const bool keyed = step.cabin->f != nullptr
                               ? std::all_of(step.cabin->f->inputs.begin(),
                                             step.cabin->f->inputs.end(), decoded)
                               : decoded(step.cabin->col_pos);
        if (!keyed) co_return co_await MissWalk(steps, index, step, access);

        // **A set may only be banked from a view nothing can later
        // contradict** (cabin.md §6a, which carries the argument and the
        // assumption it rests on). Two things forbid recording at the
        // mint: an **in-flight** transaction anywhere in the instance,
        // whose write this walk cannot see and which is live the moment it
        // commits, and the walk's **own** transaction, which may have
        // hidden a row from itself with an uncommitted DELETE or a
        // value-changing UPDATE that its ROLLBACK restores. Either leaves
        // the set missing a live tuple key - the C1 break cabin_store.hpp forbids.
        if (snapshot_.view.in_flight_at_mint ||
            snapshot_.view.own_trx_id != txn::kNoTrxId) {
            cabins_->NoteUnbankableView();
            co_return co_await MissWalk(steps, index, step, access);
        }

        // **The build is announced before anything else is asked**, and
        // the order is the argument (AT-S7). The announce installs an
        // empty, unservable set that the write hook appends to, so every
        // write from this instant onward - on any core - is in the set
        // before this walk's own matches are merged in. It takes the
        // partition's latch, which is also what orders the two questions
        // below against every write the hook has already taken: a write
        // that ran before this line released that latch before it, so its
        // transaction's publication is visible to the loads that follow.
        if (!cabins_->BeginRecording(key)) {
            co_return co_await MissWalk(steps, index, step, access);
        }
        // What the announce cannot cover, and the mint's bit cannot
        // either, is the window **between** them: another core may begin a
        // transaction after this view was minted and write this value
        // before the announce installed the set. Two loads close it, and
        // both are the instance's because one store is:
        //
        //   - anything unresolved anywhere, *now*, may still commit rows
        //     this view cannot see;
        //   - a **published** commit order above this snapshot means
        //     something already did commit that the walk cannot see, and
        //     those rows would be missing from the set forever.
        //
        // **`CommitCeiling`, not `SnapshotCeiling`, and the difference is a
        // wrong answer.** A snapshot ceiling is the published maximum
        // *capped by every core's pending-commit marker*, and that marker
        // is cleared after the committing core has already published its
        // window entry and dropped out of the unresolved count
        // (`TransactionManager::Commit`: `PublishCommit`, then
        // `PublishCoreBounds`, then `EndCommit`). In that interval a writer
        // that committed above this snapshot answers `AnyUnresolved` false
        // *and* leaves `SnapshotCeiling` pinned at this view's own
        // snapshot - both loads pass, and a set banked here is missing
        // exactly that writer's rows. The published maximum is monotone,
        // is raised inside `PublishCommit`'s hold and therefore strictly
        // before the count drops, so the pair below has no such interval.
        // The cost is a decline for a commit that was merely *pending* at
        // the mint, which is the legal direction.
        //
        // Asked **after** the announce, which is what makes them answer
        // about that window at all: the announce takes the partition's
        // latch, so a write the hook has already taken released that latch
        // before it, and its transaction's publication is visible to these
        // loads. The count is read first and the ceiling second, so a
        // count read as zero was written by the `fetch_sub` its committer
        // made after publishing - and this core's ceiling load is ordered
        // after that publication. Null on a manager with no instance - a
        // fixture - where the mint's own list is the whole answer and
        // there is no second core to have a window with.
        //
        // Declining is free by §1's corollary - the value stays unobserved
        // and the authoritative scan answers it.
        const txn::InstanceVisibility* visibility = snapshot_.view.visibility;
        if (visibility != nullptr &&
            (visibility->AnyUnresolved() ||
             visibility->CommitCeiling() != snapshot_.view.snapshot_lsn)) {
            cabins_->CancelRecording(key);
            cabins_->NoteUnbankableView();
            co_return co_await MissWalk(steps, index, step, access);
        }

        Recording recording;
        recording.step_id = step.step_id;
        recording.col_pos = step.cabin->col_pos;
        recording.f = step.cabin->f.get();
        recording.value = &ProbeValue(step);
        recording.key = &key;
        // **Saved and restored, never cleared.** The walk below descends into
        // the next step, which may be a cabin step recording a walk of its
        // own - and clearing to null on its way out would cancel *this*
        // recording from the second row onwards. The set would then be
        // committed as observed while missing qualifying tuple keys, which is the
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

        // **Every exit from here ends the announce**, or the value stays
        // unservable for the life of the store (AT-S7).
        if (!walked.ok()) {
            cabins_->CancelRecording(key);
            co_return walked;
        }
        // **Only a completed walk may be committed.** A walk that a sink
        // stopped, or that the budget ended, collected the rows it reached
        // and not the rows it did not - and a set marked observed while
        // missing qualifying tuple keys is precisely the break C1 forbids. This is
        // the one place that check can be made, because it is the only place
        // that knows whether the walk finished. A stopped walk whose
        // completion license survived ran to the relation's end *through*
        // the stop, so its set is whole.
        if (stopped_ && !walked_through_stops) {
            cabins_->CancelRecording(key);
            co_return Status::OK();
        }

        // **A body that was refused a page commits nothing** (DE-Q10,
        // `storage/miss_refusal.hpp`): the run is discarded and made again,
        // and a caller between the refused fetch and here may have read the
        // refusal as an absent row - a set recorded from that walk would
        // outlive the discard and be authoritative for a value it did not
        // see whole.
        if (storage::MissRefusalNoted()) {
            cabins_->CancelRecording(key);
            co_return Status::OK();
        }

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
    // and its relation, not of the run, so `BuiltWalk` asks it once for
    // both build forms.
    //
    // WalkAndRecord's guard, for its reason: the bucketing reads the join
    // column out of this row's frame slots, so the partial decode must
    // have filled it. The compiler guarantees it - the correlated conjunct
    // is a residual conjunct and `filter_columns` derives from that same
    // residual - and declining to build is always legal: the walk still
    // answers, per row, as it always did. The range and bounds checks are
    // for a Step built by something other than the compiler (the ladder
    // pairs `build` with a kScan or a correlated kCabinProbe and never with
    // a range): a pruned walk ends early with `stopped_` false, so building
    // under one would publish a
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
    // whole map, because one full pass is the point. A later outer row of
    // a built step probes the map (ProbeBuild, JB4) instead of walking.
    sched::Coro WalkAndBuild(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        InnerBuildState& state = statement_->builds[step.step_id];
        // The cap's standing verdict (workplan JB5): a declined step already
        // took the Cabin's fall-back refusal, and walks plain, never error.
        // The off-switch and `BuildIsArmable` are `BuiltWalk`'s.
        if (state.phase == BuildPhase::kDeclined) {
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
        // The off-switch and `BuildIsArmable` are `BuiltWalk`'s.
        InnerBuildState& state = statement_->builds[step.step_id];

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
    //  - **No dedup; a btree entry verified.** One entry per walked row by
    //    construction. Its location does not stay true: nothing in *this
    //    statement* writes the inner relation (it is a SELECT - spec §8
    //    excludes a DML `WHERE` sub-chain at compile, JB1), but another
    //    core's insert does, and since BD-R2 a leaf places a row where its
    //    key sorts, so an insert below a bucketed row shifts it (BD-R3 E3,
    //    `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`). So a
    //    btree entry goes through `VerifyTupleAt` and a miss through a tuple key
    //    lookup, the Cabin serve's shape; a heap page never shifts and is
    //    read bare. Every entry then goes through `AcceptTupleAt`, which
    //    re-applies MVCC under the statement's fixed snapshot and
    //    re-evaluates the **full** residual - the superset-plus-recheck
    //    idiom, so correctness never rests on build bookkeeping, only cost
    //    does (spec §4).
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
        //
        // **A btree entry is verified, and a miss resolved by its tuple key** (BD-R3
        // E3, the second bullet above): `VerifyTupleAt` reads the slot's tuple key
        // against the entry's; a miss descends for the tuple key, as the Cabin
        // serve does.
        PageId last_page = kInvalidPageId;
        for (const stats::CabinEntry& entry : bucket) {
            if (stopped_) break;
            NoteFetch();
            if (entry.page_id != last_page) {
                ++step_stats.pages_fetched;
                last_page = entry.page_id;
            }
            VerifiedTuple verified = VerifyTupleAt(store_, entry.page_id, entry.slot, entry.tuple_key,
                                                   entry.page_epoch, access.oid);
            if (verified.ok()) {
                if (Status s = AcceptTupleAt(steps, index, step, access, entry.page_id,
                                             *verified.page, entry.slot);
                    !s.ok()) {
                    co_return s;
                }
                continue;
            }
            verified = {};
            auto found = exec::LocateRow(store_, access, entry.tuple_key);
            if (!found.ok()) {
                // Absent: the row the walk bucketed is gone from the
                // relation, which the walk would not emit either.
                if (found.status().code() == StatusCode::kNotFound) continue;
                co_return found.status();
            }
            heap::PageView leaf(found.value().leaf.bytes());
            if (Status s = AcceptTupleAt(steps, index, step, access, found.value().page_id,
                                         leaf, found.value().slot);
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
    // the tuple keys it names; phase 2 resolves each through the clustered tree and
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
        // An elided `ORDER BY` rests on this step's index order (CV O1, O3).
        if (step.emit_order == EmitOrder::kIndexKeyBackward) {
            co_return co_await RunReverseOrderedIndexStep(steps, index, step, access);
        }
        if (step.emit_order == EmitOrder::kIndexKeyForward) {
            co_return co_await RunOrderedIndexStep(steps, index, step, access);
        }
        // Any reason to decline - `indexes = off` among them - takes the
        // walk the step would have compiled to had the index not existed,
        // and returns the identical rows, because the equalities are still
        // in the residual. The chain is **unchanged** (still kIndexProbe in
        // ANALYZE), which keeps the plan `f(shape, catalog)`.
        const catalog::TableAccess::IndexRef* ix = LiveIndexOf(step, access);
        if (ix == nullptr) co_return co_await RunWalkStep(steps, index, step, access);
        const IndexProbe& probe = *step.index;
        const index::IndexLayout layout = LayoutOf(*ix);
        // A bound is a whole entry, which is the sort key (index_page.hpp).
        const std::size_t entry_width = ix->entry_width;
        // A key set's bounds are whole entries of this index too (CU;
        // `KeySet::bounds`), or the chain is stale in the same way.
        const KeySet* set = step.key_set.has_value() ? &*step.key_set : nullptr;
        if (set != nullptr && set->bounds.size() != set->items.size() * 2 * entry_width) {
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
        index_rowids_.clear();
        seen_tuple_keys_.clear();
        filtered_tuple_keys_.clear();

        // Phase 1 between one pair of bounds: the tuple keys of the entries
        // there, each once, appended to `index_scratch_`.
        const auto collect = [&](std::span<const std::byte> lo,
                                 std::span<const std::byte> hi) -> Status {
          auto first = index::IndexSeekLeaf(store_, ix->root_page_id, layout, lo);
          if (!first.ok()) return first.status();
          NoteFetch();
          ++step_stats.pages_fetched;
          ++step_stats.index_walks;

          return index::IndexVisitFrom(
            store_, first.value(), layout, storage::PageAccess::kRead,
            [&](PageId, index::IndexLeafView& leaf,
                std::uint16_t at) -> StatusOr<storage::VisitControl> {
                auto entry = leaf.Entry(at);
                if (!entry.ok()) return entry.status();

                // The descent lands on the first leaf that *could* hold the
                // low bound, so the entries before it are skipped rather
                // than assumed absent.
                if (std::memcmp(entry.value().data(), lo.data(), entry_width) < 0) {
                    return storage::VisitControl::kContinue;
                }
                // Past the high bound: the leaves to the right are never
                // fetched, which is what makes a range cost its range.
                if (std::memcmp(entry.value().data(), hi.data(), entry_width) > 0) {
                    return storage::VisitControl::kStop;
                }
                ++step_stats.index_entries_scanned;

                // **Duplicates are expected, not damage.** Maintenance is
                // append-only, so a key round trip (v -> v' -> v) leaves two
                // entries naming one tuple key (§2) - and resolving it twice would
                // emit its row twice.
                const std::uint64_t tuple_key =
                    index::GetIndexTupleKey(entry.value().subspan(ix->key_width));

                // The covering filter (§7). A row this entry's own values
                // already disqualify never costs a base descent - which is
                // the whole of what covering buys, since visibility still
                // requires the tuple.
                //
                // **An entry judges itself, never its row** (AT-S22,
                // index.md §7): the tuple key is claimed only once an entry
                // passes, so a stale entry that fails cannot drop a row
                // whose other entry would pass.
                if (layout.covered_width > 0) {
                    if (seen_tuple_keys_.contains(tuple_key)) return storage::VisitControl::kContinue;
                    auto kept = CoveredRowSurvives(step, access, *ix,
                                                   entry.value().subspan(layout.covered_offset()));
                    if (!kept.ok()) return kept.status();
                    if (!kept.value()) {
                        filtered_tuple_keys_.insert(tuple_key);
                        return storage::VisitControl::kContinue;
                    }
                    filtered_tuple_keys_.erase(tuple_key);  // an earlier entry failed; this one keeps the row
                    seen_tuple_keys_.insert(tuple_key);
                } else if (!seen_tuple_keys_.insert(tuple_key).second) {
                    return storage::VisitControl::kContinue;
                }

                index_scratch_.push_back(tuple_key);
                if (layout.rowid_width != 0) {
                    index_rowids_.emplace(tuple_key, index::GetIndexRowid(layout, entry.value()));
                }
                return storage::VisitControl::kContinue;
            });
        };
        if (set == nullptr) {
            if (Status walked = collect(*low, *high); !walked.ok()) co_return walked;
        } else {
            // **A key set walks once per value into the one seen set**
            // (CU-R2): an entry an earlier version left under one value of
            // the list names a row whose visible version may hold another
            // value of it, and the list's conjunct passes that row whichever
            // entry found it - so two seen sets would emit it twice. Each
            // key keeps the leaf of the value that found it first, packed
            // below it for the sort (`kKeySetLeafBits`).
            const std::span<const std::byte> bounds(set->bounds);
            for (std::size_t i = 0; i < set->items.size(); ++i) {
                const std::size_t before = index_scratch_.size();
                if (Status walked = collect(bounds.subspan(2 * i * entry_width, entry_width),
                                            bounds.subspan((2 * i + 1) * entry_width, entry_width));
                    !walked.ok()) {
                    co_return walked;
                }
                for (std::size_t k = before; k < index_scratch_.size(); ++k) {
                    if ((index_scratch_[k] >> (64 - kKeySetLeafBits)) != 0) {
                        co_return Status::Corruption(
                            "an index entry names a tuple key wider than 48 bits");
                    }
                    index_scratch_[k] = (index_scratch_[k] << kKeySetLeafBits) | set->items[i].leaf;
                }
            }
        }
        // Descents avoided: the rows every one of whose entries failed.
        step_stats.index_entries_filtered += filtered_tuple_keys_.size();

        // **Sorted, and this is a correctness property rather than a
        // locality one.** The walk collects tuple keys in *index key* order; a scan
        // of the same relation emits them in tuple key order. Without this sort,
        // creating an index would reorder a reply - and "an accelerator may
        // cost performance and must never change a query result" is the
        // standard invariant 8 holds Waystone to, which an authoritative
        // structure does not get to fall below. The equivalence tests
        // compare byte for byte precisely so this cannot be missed.
        //
        // It buys locality as well: on a btree relation tuple key order *is* leaf
        // order, so the descents below walk the tree forwards instead of
        // jumping. The cost is one sort of the matched set, against one
        // descent per element of it.
        std::sort(index_scratch_.begin(), index_scratch_.end());

        // **Moved out of the member for phase 2, a correctness property.**
        // The descent below re-enters `RunStep` for the next step, which may
        // be another index step on this runner whose phase 1 clears this
        // vector - the outer loop would then resolve the inner step's tuple keys
        // and lose rows (`FROM b AS a JOIN b AS c ON a.qty = c.qty WHERE
        // a.owner = 1 AND c.owner = 2`), which index.md §1 forbids. Handed back
        // after the loop so a single index step reuses one allocation.
        std::vector<std::uint64_t> tuple_keys;
        tuple_keys.swap(index_scratch_);
        std::unordered_map<std::uint64_t, std::uint64_t> rowids;
        rowids.swap(index_rowids_);

        // Phase 2. Every tuple key resolves through the clustered tree (spec
        // IX3), or on a heap at the rowid its entry carries (IX1).
        for (const std::uint64_t collected : tuple_keys) {
            if (stopped_) break;
            // A key set's keys carry the leaf of the value that found them.
            const std::uint64_t tuple_key =
                set != nullptr ? collected >> kKeySetLeafBits : collected;
            if (set != nullptr) {
                key_set_leaf_ = static_cast<std::uint32_t>(
                    collected & ((std::uint64_t{1} << kKeySetLeafBits) - 1));
            }
            NoteFetch();
            ++step_stats.pages_fetched;
            // Counted per descent actually made, not from the scratch size:
            // a sink that stopped mid-resolve (V09's quota) ends this loop
            // early, and a meter that still claimed the whole set would
            // report work not done - the exact number the quota exists to
            // save, and the one ANALYZE prices covering with.
            ++step_stats.index_rows_resolved;
            const auto named = rowids.find(tuple_key);
            auto found =
                ResolveIndexedRow(access, tuple_key, named != rowids.end() ? named->second : 0);
            if (!found.ok()) {
                if (found.status().code() == StatusCode::kNotFound) {
                    // Dangling: the tuple key names no row now - a skip, never an
                    // error. A committed key names another row only after a
                    // `PURGE` freed it (K1); a rolled-back or purged one may
                    // be named again (W12, BD-R4, BH), and a row later found
                    // under it is re-checked like any other (MVCC and the
                    // residual).
                    continue;
                }
                co_return found.status();
            }

            // The leaf the lookup holds (btree.hpp `Location`).
            heap::PageView page(found.value().leaf.bytes());
            if (Status s = AcceptTupleAt(steps, index, step, access, found.value().page_id, page,
                                         found.value().slot);
                !s.ok()) {
                co_return s;
            }
        }
        tuple_keys.clear();
        tuple_keys.swap(index_scratch_);
        rowids.clear();
        rowids.swap(index_rowids_);
        co_return Status::OK();
    }

    // The row an index entry named: on a heap at the entry's rowid, checked
    // to hold `tuple_key` (DN-R3), and through the Keystone index where it
    // does not - an entry left by a key a `PURGE` freed and an insert
    // placed again names the old slot, and the key's row is elsewhere. On
    // a btree, the descent.
    // `rowid` 0: none named (a btree, or an entry without one).
    StatusOr<btree::Location> ResolveIndexedRow(const catalog::TableAccess& access,
                                                std::uint64_t tuple_key, std::uint64_t rowid) {
        if (rowid != 0) {
            auto row = FetchHeapRow(store_, access, rowid, tuple_key);
            if (row.ok() || row.status().code() != StatusCode::kNotFound) return row;
        }
        return exec::LocateRow(store_, access, tuple_key);
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

    // The live index a step names, or null where the step must decline to
    // the walk: `indexes = off`, no index on the step, an index dropped
    // between compile and execution, or one redefined under the compiled
    // chain (its bounds no longer its entries' width). The **live** root,
    // not the one compiled in: a split republishes it in place on the cached
    // entry (index.md §12a), and a chain compiled before a write may name a
    // page that is no longer the top of the tree.
    const catalog::TableAccess::IndexRef* LiveIndexOf(const Step& step,
                                                      const catalog::TableAccess& access) const {
        if (!indexes_ || !step.index.has_value()) return nullptr;
        const IndexProbe& probe = *step.index;
        const catalog::TableAccess::IndexRef* ix = nullptr;
        for (const catalog::TableAccess::IndexRef& candidate : access.indexes) {
            if (candidate.index_oid == probe.index_oid) ix = &candidate;
        }
        if (ix == nullptr || probe.low.size() != ix->entry_width ||
            probe.high.size() != ix->entry_width) {
            return nullptr;
        }
        return ix;
    }

    static index::IndexLayout LayoutOf(const catalog::TableAccess::IndexRef& ix) {
        return IndexLayoutOf(ix);
    }

    // ---- The ordered index run (CV O1) ---------------------------------
    //
    // An elided `ORDER BY` rests on this step's index order: the entries are
    // walked in entry order - the whole key, then the tuple key (IX4b) - a
    // leaf at a time, each leaf's matching entries collected under its hold
    // and resolved after its release, in that order, so a LIMIT stops the
    // walk at the last row it lets out. IX8a's sort back into tuple key
    // order is what this step must not do, and the compiler took the index
    // only where its keys after the prefix are exactly the `ORDER BY`'s, so
    // the ties come out by ascending tuple key, the sort's own tie order.
    //
    // **Two checks at each entry** (CV-R2, `[quiet-wrong]`):
    //  - **the key check**: a row is emitted only from the entry whose key
    //    equals its visible version's (`OrderedKeyMatches`). Maintenance is
    //    append-only (IX1), so a row whose order column moved has an entry at
    //    each value it held, and only the one the reader's view sees is in
    //    its place;
    //  - **the seen set**: each tuple key at most once, for the entries a
    //    covered column's move left side by side under one key (IX4b).
    //
    // **A released leaf is re-sought by key, never revisited by pointer**
    // (CV-R3): from the last entry the leaf held, which no divide moves left
    // and no maintenance removes (index_tree.hpp), so a divide on another
    // core between two leaves neither skips an entry nor repeats one. Within
    // one pass a leaf that adds nothing to the batch is followed by its right
    // sibling, as `IndexVisitFrom` follows it: every entry the view needs was
    // in the tree before the walk began, and one above the leaf's last was
    // already to its right when the leaf was read. It yields at the bottom of
    // its leaf loop, holding no page, counting every leaf it reads (CV-R4).
    sched::Coro RunOrderedIndexStep(const std::vector<Step>& steps, std::size_t index,
                                    const Step& step, const catalog::TableAccess& access) {
        const catalog::TableAccess::IndexRef* ix = LiveIndexOf(step, access);
        if (ix == nullptr) co_return co_await RunOrderedDecline(steps, index, step, access);
        const IndexProbe& probe = *step.index;
        const index::IndexLayout layout = LayoutOf(*ix);
        const std::size_t entry_width = ix->entry_width;
        StepStats& step_stats = stats_.For(step.step_id);
        const bool outermost = index == 0 && parent_ == nullptr;

        std::unordered_set<std::uint64_t> emitted;
        std::vector<std::byte> batch;
        // The low bound, then the last entry read. No entry equals the low
        // bound - its tuple key and padding are zero, a tuple key never is -
        // so an entry equal to `from` is always one already read.
        std::vector<std::byte> from(probe.low);
        for (;;) {
            batch.clear();
            bool end = false;
            {
                NoteFetch();
                auto first = index::IndexSeekLeaf(store_, ix->root_page_id, layout, from);
                if (!first.ok()) co_return first.status();
                ++step_stats.index_walks;
                PageId cur = first.value();
                storage::PageChainGuard guard("ordered index walk");
                // Whether this pass has read an entry past the previous
                // pass's `from`: the seek re-reads the leaf that pass ended
                // on, and a due slice ending the pass there would end every
                // later pass there too, at a slice of one page.
                bool moved = false;
                // Leaf by leaf until one holds an entry for the batch or the
                // range ends: a seek lands at or left of the leaf it wants.
                for (;;) {
                    if (Status s = guard.Visit(cur); !s.ok()) co_return s;
                    auto held = index::IndexFetchLeaf(store_, cur, layout, storage::PageAccess::kRead);
                    if (!held.ok()) co_return held.status();
                    NoteFetch();
                    ++step_stats.pages_fetched;
                    index::IndexLeafView leaf(held.value().bytes());
                    const std::uint16_t n = leaf.entry_count();
                    const std::uint16_t start = leaf.LowerBound(from);
                    for (std::uint16_t at = start; at < n; ++at) {
                        auto entry = leaf.Entry(at);
                        if (!entry.ok()) co_return entry.status();
                        const std::span<const std::byte> e = entry.value();
                        if (std::memcmp(e.data(), from.data(), entry_width) == 0) {
                            continue;  // read already: entries are unique
                        }
                        moved = true;
                        if (std::memcmp(e.data(), probe.high.data(), entry_width) > 0) {
                            end = true;
                            break;
                        }
                        if (Status s = CollectEntry(step, access, *ix, e, batch); !s.ok()) {
                            co_return s;
                        }
                    }
                    // Only from a leaf holding an entry at or past `from`: a
                    // seek that lands left of a divide - its parent read
                    // before the separator was published - reaches a leaf
                    // whose entries all sort below `from`, and taking its
                    // last would move `from` back over entries already read
                    // (CV-R3) or below the low bound.
                    if (!end && start < n) {
                        auto last = leaf.Entry(static_cast<std::uint16_t>(n - 1));
                        if (!last.ok()) co_return last.status();
                        from.assign(last.value().begin(), last.value().end());
                    }
                    const PageId right = leaf.right_sibling();
                    if (right == kInvalidPageId) end = true;
                    // A slice due ends the pass too, once the pass has
                    // moved: `from` is set, so the next seek resumes past
                    // what this leaf held - by re-reading this leaf, a read
                    // counted like any other. A pass that has read nothing
                    // past the last one's `from` steps right first, its
                    // leaves still counted, and yields at the bottom.
                    const bool slice_due = outermost && slice_ != nullptr &&
                                           ++slice_pages_ >= slice_->pages_per_slice && moved;
                    if (end || !batch.empty() || slice_due) break;
                    cur = right;
                }
            }
            if (Status s = ResolveBatch(steps, index, step, access, *ix, batch, emitted); !s.ok()) {
                co_return s;
            }
            if (stopped_ || end) co_return Status::OK();
            if (outermost && slice_ != nullptr && slice_pages_ >= slice_->pages_per_slice) {
                if (Status s = co_await YieldAtSliceBoundary(); !s.ok()) co_return s;
            }
        }
    }

    // **The reverse ordered index run (CV O3)**: the same entries from the
    // high end down - so ties come out by descending tuple key (CV-Q4 (a)).
    // An index leaf links forward only (CV-Q1 (a)), so a step left is a
    // descent: once a leaf is read, the next holds the greatest entries
    // below its first, found by `index::IndexFetchLeafAtOrBelow` for that
    // entry less one (`index::PrecedingSortKey`), which moves right under
    // the leaf's hold while a divide has moved that range past where the
    // descent landed (CV-R3). A leaf's first entry never changes - a divide
    // keeps the lower half, no entry is removed - but the leftmost's, which
    // only gains entries below it, each newer than the walk's view; and the
    // walk ends there by id: when the descent returns the leaf it just read.
    // It ends too at the low bound.
    // It yields every `pages_per_slice` leaves, holding no page (CV-R4).
    sched::Coro RunReverseOrderedIndexStep(const std::vector<Step>& steps, std::size_t index,
                                           const Step& step, const catalog::TableAccess& access) {
        const catalog::TableAccess::IndexRef* ix = LiveIndexOf(step, access);
        if (ix == nullptr) co_return co_await RunOrderedDecline(steps, index, step, access);
        const IndexProbe& probe = *step.index;
        const index::IndexLayout layout = LayoutOf(*ix);
        const std::size_t entry_width = ix->entry_width;
        StepStats& step_stats = stats_.For(step.step_id);
        const bool outermost = index == 0 && parent_ == nullptr;

        std::unordered_set<std::uint64_t> emitted;
        std::vector<std::byte> batch;
        std::vector<std::byte> bound(probe.high);  // inclusive
        PageId previous = kInvalidPageId;
        for (;;) {
            batch.clear();
            bool end = false;
            {
                NoteFetch();
                auto held = index::IndexFetchLeafAtOrBelow(store_, ix->root_page_id, layout, bound);
                if (!held.ok()) co_return held.status();
                if (held.value().page_id() == previous) co_return Status::OK();
                ++step_stats.index_walks;
                ++step_stats.pages_fetched;
                index::IndexLeafView leaf(held.value().bytes());
                const std::uint16_t n = leaf.entry_count();
                // The entries at or below `bound`: those below the first at
                // or above it, and that one if it is `bound` itself.
                std::uint16_t top = leaf.LowerBound(bound);
                if (top < n) {
                    auto at_bound = leaf.Entry(top);
                    if (!at_bound.ok()) co_return at_bound.status();
                    if (std::memcmp(at_bound.value().data(), bound.data(), entry_width) == 0) ++top;
                }
                for (std::uint16_t at = top; at-- > 0;) {
                    auto entry = leaf.Entry(at);
                    if (!entry.ok()) co_return entry.status();
                    const std::span<const std::byte> e = entry.value();
                    if (std::memcmp(e.data(), probe.low.data(), entry_width) < 0) {
                        end = true;
                        break;
                    }
                    if (Status s = CollectEntry(step, access, *ix, e, batch); !s.ok()) co_return s;
                }
                if (n == 0) {
                    end = true;  // the empty root
                } else {
                    auto first = leaf.Entry(0);
                    if (!first.ok()) co_return first.status();
                    if (std::memcmp(first.value().data(), probe.low.data(), entry_width) <= 0) {
                        end = true;
                    } else {
                        // Above the low bound, so not all zeros.
                        bound.assign(first.value().begin(), first.value().end());
                        index::PrecedingSortKey(bound);
                    }
                }
                previous = held.value().page_id();
            }
            if (Status s = ResolveBatch(steps, index, step, access, *ix, batch, emitted); !s.ok()) {
                co_return s;
            }
            if (stopped_ || end) co_return Status::OK();
            if (outermost && slice_ != nullptr && ++slice_pages_ >= slice_->pages_per_slice) {
                if (Status s = co_await YieldAtSliceBoundary(); !s.ok()) co_return s;
            }
        }
    }

    // One entry within an ordered run's bounds, counted, and appended to the
    // batch unless the covering filter's verdict on it is no (§7).
    Status CollectEntry(const Step& step, const catalog::TableAccess& access,
                        const catalog::TableAccess::IndexRef& ix, std::span<const std::byte> e,
                        std::vector<std::byte>& batch) {
        StepStats& step_stats = stats_.For(step.step_id);
        ++step_stats.index_entries_scanned;
        if (ix.entry_width > ix.key_width + index::kIndexTupleKeyWidth) {
            auto kept = CoveredRowSurvives(step, access, ix,
                                           e.subspan(ix.key_width + index::kIndexTupleKeyWidth));
            if (!kept.ok()) return kept.status();
            if (!kept.value()) {
                ++step_stats.index_entries_filtered;
                return Status::OK();
            }
        }
        batch.insert(batch.end(), e.begin(), e.end());
        return Status::OK();
    }

    // A leaf's batch of entries, resolved in batch order, each tuple key at
    // most once (CV-R2's seen set).
    Status ResolveBatch(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access,
                        const catalog::TableAccess::IndexRef& ix, std::span<const std::byte> batch,
                        std::unordered_set<std::uint64_t>& emitted) {
        for (std::size_t off = 0; off < batch.size() && !stopped_; off += ix.entry_width) {
            const std::span<const std::byte> e = batch.subspan(off, ix.entry_width);
            const std::uint64_t tuple_key =
                index::GetIndexTupleKey(e.subspan(ix.key_width, index::kIndexTupleKeyWidth));
            if (emitted.contains(tuple_key)) continue;
            // A heap's entry carries its row's rowid (DN, IX1).
            const std::uint64_t rowid =
                ix.rowid ? index::GetIndexRowid(IndexLayoutOf(ix), e) : 0;
            auto matched = ResolveOrdered(steps, index, step, access, e.first(ix.key_width),
                                          tuple_key, rowid);
            if (!matched.ok()) return matched.status();
            if (matched.value()) emitted.insert(tuple_key);
        }
        return Status::OK();
    }

    // One entry's row, resolved through the clustered tree and handed to
    // `AcceptTupleAt` with the entry's key for the key check; whether the
    // row's visible version holds that key. A tuple key naming no row is a
    // skip (IX1's dangling entry), as in `RunIndexStep`.
    StatusOr<bool> ResolveOrdered(const std::vector<Step>& steps, std::size_t index,
                                  const Step& step, const catalog::TableAccess& access,
                                  std::span<const std::byte> key, std::uint64_t tuple_key,
                                  std::uint64_t rowid) {
        StepStats& step_stats = stats_.For(step.step_id);
        NoteFetch();
        ++step_stats.pages_fetched;
        ++step_stats.index_rows_resolved;
        auto found = ResolveIndexedRow(access, tuple_key, rowid);
        if (!found.ok()) {
            if (found.status().code() == StatusCode::kNotFound) return false;
            return found.status();
        }
        ordered_key_ = key;
        ordered_matched_ = false;
        heap::PageView page(found.value().leaf.bytes());
        Status s = AcceptTupleAt(steps, index, step, access, found.value().page_id, page,
                                 found.value().slot);
        ordered_key_ = {};
        if (!s.ok()) return s;
        return ordered_matched_;
    }

    // **The ordered step's decline** (CV-S0's census): `indexes = off`, or an
    // index gone or redefined under the chain, leaves the step a walk - and
    // a walk emits tuple key order, which an elided `ORDER BY` cannot take
    // (`[quiet-wrong]`). So the walk collects each matching row's encoded
    // key and tuple key instead of emitting it (`OrderedKeyMatches` in its
    // collecting form), the pairs are sorted - the index's own order, key
    // then tuple key - and each row is resolved and emitted in that order,
    // as the index walk would have. It holds every match at once, as IX8a's
    // collection does.
    sched::Coro RunOrderedDecline(const std::vector<Step>& steps, std::size_t index,
                                  const Step& step, const catalog::TableAccess& access) {
        std::vector<OrderedRecord> collected;
        ordered_collect_ = &collected;
        Status walked = co_await RunWalkStep(steps, index, step, access);
        ordered_collect_ = nullptr;
        if (!walked.ok()) co_return walked;
        // The key's bytes, then the tuple key: the pair's order is the entry
        // order, and its reverse the reverse walk's (O3, CV-Q4 (a)).
        std::sort(collected.begin(), collected.end());
        if (step.emit_order == EmitOrder::kIndexKeyBackward) {
            std::reverse(collected.begin(), collected.end());
        }
        for (const OrderedRecord& record : collected) {
            if (stopped_) break;
            auto matched = ResolveOrdered(steps, index, step, access, record.first, record.second,
                                          /*rowid=*/0);
            if (!matched.ok()) co_return matched.status();
        }
        co_return Status::OK();
    }

    // The key check (CV-R2), on a row that survived every conjunct: its
    // visible version's index key, encoded from the decoded slots, against
    // the entry it was reached by - or, while the decline collects, the
    // key and tuple key recorded and the row held back.
    StatusOr<bool> OrderedKeyMatches(const Step& step, const catalog::TableAccess& access,
                                     std::span<const parser::AstValue> slots) {
        const IndexProbe& probe = *step.index;
        ordered_scratch_.assign(probe.key_width, std::byte{0});
        std::size_t at = 0;
        for (const std::uint16_t col : probe.key_cols) {
            const catalog::SysColumnRow& column = access.schema.columns[col];
            auto width = IndexKeyColumnWidth(column);
            if (!width.ok()) return width.status();
            if (at + width.value() > ordered_scratch_.size()) {
                return Status::Corruption("an ordered index step's key columns encode wider than "
                                          "its key");
            }
            if (Status s = EncodeIndexKeyColumn(
                    column, slots[col], std::span<std::byte>(ordered_scratch_).subspan(at, width.value()));
                !s.ok()) {
                return s;
            }
            at += width.value();
        }
        if (at != ordered_scratch_.size()) {
            return Status::Corruption("an ordered index step's key columns encode narrower than "
                                      "its key");
        }
        if (ordered_collect_ != nullptr) {
            auto tuple_key = KeystoneIdOfPayload(version_);
            if (!tuple_key.ok()) return tuple_key.status();
            ordered_collect_->emplace_back(ordered_scratch_, tuple_key.value());
            return false;
        }
        ordered_matched_ = ordered_key_.size() == ordered_scratch_.size() &&
                           std::memcmp(ordered_key_.data(), ordered_scratch_.data(),
                                       ordered_scratch_.size()) == 0;
        return ordered_matched_;
    }

    // Serves an observed value's entry set.
    //
    // **Two phases, and the split is not an optimization.** Phase 1 resolves
    // every entry to a verified location without emitting anything; phase 2
    // emits, so the set is served in one order (below) and no row goes out
    // before every entry is resolved - a failed hint by the tuple key's
    // descent.
    //
    // **One set, or a cover's several** (AQ-R8): phase 1 resolves them all
    // into one list, deduped across sets, and phase 2 sorts and emits it, so
    // a cover's union is served in the walk's order like any one set.
    sched::Coro ServeFromCabin(const std::vector<Step>& steps, std::size_t index, const Step& step,
                          const catalog::TableAccess& access, std::span<const ServedSet> sets) {
        StepStats& step_stats = stats_.For(step.step_id);

        serve_scratch_.clear();
        seen_tuple_keys_.clear();

        for (const ServedSet& served : sets) {
            const stats::CabinKey& key = *served.key;
            const stats::CabinSet& entries = *served.set;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                // By value, under the partition's latch (AT-S7): the set is
                // the instance's since one store serves every core, and another
                // core may be healing this entry while this loop reads it.
                const stats::CabinEntry entry = entries.At(i);

                // **Duplicates are expected, not damage.** A value round trip
                // (v→v′→v) appends the same tuple key twice under append-only
                // maintenance (§5), so the set is deduped as it is served and
                // the duplicate is left for pruning to collapse.
                if (!seen_tuple_keys_.insert(entry.tuple_key).second) continue;

                if (entry.hint_valid()) {
                    NoteFetch();
                    ++step_stats.pages_fetched;
                    VerifiedTuple verified = VerifyTupleAt(store_, entry.page_id, entry.slot, entry.tuple_key,
                                                           entry.page_epoch, access.oid);
                    if (verified.ok()) {
                        ++step_stats.cabin_hint_hits;
                        cabins_->NoteHint(key.cabin_id, /*ok=*/true);
                        serve_scratch_.push_back(Located{entry.tuple_key, entry.page_id, entry.slot,
                                                         entry.page_epoch, served.leaf});
                        continue;
                    }
                    ++step_stats.cabin_hint_misses;
                    cabins_->NoteHint(key.cabin_id, /*ok=*/false);
                }

                NoteFetch();
                ++step_stats.pages_fetched;
                auto found = exec::LocateRow(store_, access, entry.tuple_key);
                if (!found.ok()) {
                    if (found.status().code() == StatusCode::kNotFound) {
                        // Dangling: the tuple key names no row now - a **skip**, never
                        // an error (§5). A committed key names another row only
                        // after a `PURGE` freed it (K1); a rolled-back or purged
                        // one may be named again (W12, BD-R4, BH), and a row
                        // later found under it is re-checked against the key
                        // column like any other.
                        continue;
                    }
                    co_return found.status();
                }

                // Healed in place. The hint was wrong and the tuple key was right,
                // which is C6's whole shape: authority in the tuple key, speed in the
                // location, and the location repaired from the authority. The
                // epoch stamped with it is read off the leaf the lookup holds,
                // so it is the epoch the slot is valid under - a re-fetch after
                // the hold could read a divide's epoch beside a pre-divide slot.
                // **The heal, by index** (AT-S7): the store writes it under
                // the partition's latch, so the descent above never ran under
                // one.
                const PageId healed_page = found.value().page_id;
                const std::uint16_t healed_slot = found.value().slot;
                const std::uint32_t healed_epoch =
                    CurrentPageEpoch(heap::PageView(found.value().leaf.bytes()));
                entries.Heal(i, healed_page, healed_slot, healed_epoch);
                serve_scratch_.push_back(
                    Located{entry.tuple_key, healed_page, healed_slot, healed_epoch, served.leaf});
            }
        }

        // Moved out of the member for phase 2, for the reason spelled out in
        // RunIndexStep: the descent below may reach another cabin step on
        // this same runner, whose phase 1 clears this vector - and it would
        // be clearing the one this loop is walking. Handed back after the
        // loop so a single cabin step still reuses one allocation.
        std::vector<Located> located;
        located.swap(serve_scratch_);

        // **Sorted into the walk's order before emission** (IX8a; fixed
        // 2026-08-19). A committed set starts in that order (WalkAndRecord
        // sorts by page and slot) but the write hook appends at the *end*,
        // so served in entry order a set could reorder a reply against
        // I12's within-step contract.
        //
        // **That order is tuple key order**: a leaf's slot order is its key
        // order by placement (BD-R1), so the walk emits ids ascending, across
        // leaves by `min_key`, and sorting by tuple key is exact even after a
        // leaf division has given a later page a lower id.
        std::sort(located.begin(), located.end(),
                  [](const Located& a, const Located& b) { return a.tuple_key < b.tuple_key; });

        // Phase 2. The page is fetched per entry rather than carried out of
        // phase 1: `AcceptTupleAt` descends into the next step, and anything
        // below it may fetch, so holding every entry's page across the loop
        // is a pin per entry for the whole serve.
        //
        // **So the location is verified again here**: phase 1's hold ended
        // with phase 1 (btree.hpp `Location`), and read unchecked a
        // renumbered slot is another row this step's residual may accept -
        // one row served twice and the moved one never. A miss re-descends,
        // which on a btree relation is authoritative and hands back the leaf
        // held.
        for (const Located& at : located) {
            if (stopped_) break;
            key_set_leaf_ = at.leaf;  // read only where the step carries a key set
            NoteFetch();
            ++step_stats.pages_fetched;
            VerifiedTuple verified =
                VerifyTupleAt(store_, at.page_id, at.slot, at.tuple_key, at.epoch, access.oid);
            if (verified.ok()) {
                if (Status s = AcceptTupleAt(steps, index, step, access, at.page_id,
                                             *verified.page, at.slot);
                    !s.ok()) {
                    co_return s;
                }
                continue;
            }
            ++step_stats.cabin_hint_misses;
            NoteFetch();
            ++step_stats.pages_fetched;
            auto found = exec::LocateRow(store_, access, at.tuple_key);
            if (!found.ok()) {
                if (found.status().code() == StatusCode::kNotFound) continue;
                co_return found.status();
            }
            heap::PageView page(found.value().leaf.bytes());
            if (Status s = AcceptTupleAt(steps, index, step, access, found.value().page_id,
                                         page, found.value().slot);
                !s.ok()) {
                co_return s;
            }
        }
        step_stats.cabin_entries_served += located.size();
        located.clear();
        located.swap(serve_scratch_);
        co_return Status::OK();
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
        // A mark is a leaf and the last tuple key covered. The mark starts where the last walk left it, so a walk that
        // visits nothing leaves it untouched.
        const bool prefixed = prefix != nullptr;
        const PageId resume_page = prefixed ? prefix->resume.page : kInvalidPageId;
        const std::uint64_t resume_tuple_key = prefixed ? prefix->resume.last_tuple_key : 0;
        const std::uint16_t resume_slot = prefixed ? prefix->resume.last_slot : 0;
        const bool heap_walk = access.heap();
        if (prefixed) prefix->mark = prefix->resume;
        // The build this walk extends, when it is this step's own: read
        // after each accepted row, because the cap trips inside one - and
        // the row that trips it is visited without being bucketed, so the
        // mark may not advance past it.
        const bool extending =
            prefixed && building_ != nullptr && building_->step_id == step.step_id;
        bool mark_frozen = false;
        // Whether the walk ended on a stop rather than on the relation's
        // end - the two are one return value from the page primitives
        // (kInvalidPageId either way), and only the walk knows which.
        bool cut = false;

        // ---- Range pruning (kRange) --------------------------------------
        //
        // The leaf chain is ordered **page-wise** by `min_key`: every id in
        // a leaf is below the next leaf's min_key. So the first leaf whose
        // min_key exceeds the high bound proves that nothing after it can
        // qualify, and the walk can stop.
        //
        // **This prunes the tail, not the head.** Leading leaves are
        // skipped by the head seek below.
        //
        // Rows below `low` are dropped by the residual, which still carries
        // both bounds (step_chain.hpp) - so this is an accelerator that
        // cannot change the answer even if the pruning were wrong.
        const std::uint64_t range_high =
            step.range.has_value() ? step.range->high : 0;
        // Never a heap's (DN): its pages are in no key order. The compiler
        // gives a heap's step no range; this keeps a hand-built one honest.
        const bool pruning =
            step.range.has_value() && !access.heap();

        // The page the visitor is on: the tuple's address, which a trail
        // records rather than re-derives by searching (workplan P09).
        PageId walk_last_page = kInvalidPageId;

        // The access the visitor reads. A slice boundary (BA-S15) can cross
        // a catalog invalidation; the statement's memo pin
        // (`Catalog::MemoPin`) keeps this entry alive across it, retired
        // rather than freed, so the pointer stays the one the walk bound.
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

        // One slot of the walk. **No `Status` and no `StatusOr` per slot**
        // (BM-R5): a failure is parked in `inner` and answered as a stop,
        // and the page loop below reads `inner` before anything else, so a
        // failed row still ends the walk with its own Status. `nr_slots` is
        // the page's slot count, read once per page by the loop that owns
        // it.
        const auto visit_slot = [&](PageId page_id, heap::PageView& page, std::uint16_t slot,
                                    std::uint16_t nr_slots) -> storage::VisitControl {
            if (walk_ends_on_stop()) return stop_here();

            // The walk's page count, exact by construction: the chain hands
            // the visitor one page's slots consecutively, so a transition
            // is a fetch (cabin-controller.md §II.2 S2).
            if (page_id != walk_last_page) {
                ++stats_.For(step.step_id).pages_fetched;
                walk_last_page = page_id;
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
            // goes through here**, so the key a resume skips to cannot drift
            // from the key a mark recorded - that drift would silently drop
            // rows or emit them twice, and no test of a completed walk would
            // see it.
            const auto accept = [&](std::uint16_t at) -> Status {
                // One branch, loop-invariant, for every walk in the engine
                // that is not a prefix: no key is even read.
                // The walk is the hottest path there is, and JB5's gate
                // priced a two-nanosecond per-row guard as visible.
                if (!prefixed) {
                    return AcceptTupleAt(steps, index, step, *live_access, page_id, page, at,
                                         nr_slots);
                }
                // The row's key, for the mark. A retired slot has none and
                // is no row: it is handed on unmarked, and `AcceptTupleAt`
                // declines it.
                std::uint64_t tuple_key = 0;
                auto payload = page.PayloadAt(at, nr_slots);
                if (payload.ok()) {
                    auto id = KeystoneIdOfPayload(payload.value());
                    if (!id.ok()) return id.status();
                    tuple_key = id.value();
                }
                // Covered by an earlier walk of this step: the map holds
                // whatever it qualified for, and re-examining it is the
                // double visit spec §6's economics forbids. On a heap by
                // place, since its keys are in no order (DN).
                const bool covered =
                    heap_walk ? page_id == resume_page && tuple_key != 0 && at <= resume_slot
                              : resume_page != kInvalidPageId && tuple_key != 0 &&
                                    tuple_key <= resume_tuple_key;
                if (covered) return Status::OK();
                Status s =
                    AcceptTupleAt(steps, index, step, *live_access, page_id, page, at, nr_slots);
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
                if (tuple_key != 0) prefix->mark = WalkMark{page_id, tuple_key, at};
                return s;
            };

            if (Status accepted = accept(slot); !accepted.ok()) {
                inner = std::move(accepted);
                return storage::VisitControl::kStop;
            }
            return walk_ends_on_stop() ? stop_here() : storage::VisitControl::kContinue;
        };

        NoteFetch();

        // **A btree leaf's slot loop is the walk's own** (BM-R5): one fetch
        // and one slot-count read per leaf, and the visitor called directly
        // per slot. `btree::BtreeVisitLeafPage` makes the same walk through
        // a `std::function`, for its other callers (UPDATE's and DELETE's
        // row loops); the page's type check is the one it makes.
        const auto visit_leaf = [&](PageId leaf_id,
                                    storage::PageAccess access_mode) -> StatusOr<PageId> {
            auto leaf_ref = btree::BtreeFetchLeaf(store_, leaf_id, access_mode);
            if (!leaf_ref.ok()) return leaf_ref.status();
            heap::PageView leaf(leaf_ref.value().bytes());
            const std::uint16_t n = leaf.slot_count();
            for (std::uint16_t i = 0; i < n; ++i) {
                // A stop leaves the leaves to the right unfetched, as the
                // primitive's early exit does.
                if (visit_slot(leaf_id, leaf, i, n) == storage::VisitControl::kStop) {
                    return kInvalidPageId;
                }
            }
            return leaf.next_page_id();
        };

        // ---- The head seek (kRange on a clustered relation) --------------
        //
        // Tail pruning above ends the walk; this is what stops it starting
        // at the beginning. A tuple key range on a btree relation descends to the
        // leaf holding the low bound and walks siblings from there, so a
        // range near the end of the relation costs the range rather than
        // everything before it. The residual carries both bounds, which is
        // what makes the seek an accelerator that cannot change the answer -
        // the same property tail pruning rests on.
        PageId cur = kInvalidPageId;
        if (resume_page != kInvalidPageId) {
            // Resuming (JB6): the mark names the page and the skip above
            // names the row within it, so there is no seek and no leftmost
            // descent - the walk order is the one the mark was taken in,
            // and re-deriving a start would be a second answer to a
            // question already answered.
            cur = resume_page;
        } else {
            if (pruning) {
                auto first = btree::BtreeSeekLeaf(store_, access.desc_page_id, step.range->low);
                // A descent that failed is a reason to walk, not to fail:
                // the walk is the authoritative path and reaches the same
                // rows.
                if (first.ok()) {
                    cur = first.value();
                } else {
                    storage::ClearStructuralRefusal();  // the walk answers it (BA-S14)
                }
            }
            if (cur == kInvalidPageId) {
                auto first = btree::BtreeLeftmostLeaf(store_, access.desc_page_id);
                if (!first.ok()) co_return first.status();
                cur = first.value();
            }
        }

        // ---- The page loop, owned by the coroutine (P4d-3) ---------------
        //
        // This coroutine steps the relation page by page. Each call holds its
        // page's pin only for the call, so the bottom of this loop - no
        // pin, no span - is the executor's one legal suspension point.
        // Since BA-S15 the statement's own outermost walk yields there every
        // `SlicePolicy::pages_per_slice` pages, which is the slice boundary
        // below. A walk with no policy - every sync `Execute` - never does.
        //
        // The first page is visited unconditionally, exactly as the
        // whole-chain forms do: a bad head fails inside the fetch, where
        // testing it here would answer a corrupt catalog row with an empty
        // result instead of an error. The walk declares nothing to the lock
        // family (CP-S2a): its relations were declared at the bind (AT-S1),
        // and the per-page slice `IS` it reported until then had no consumer.

        // **The outermost walk faults cold** (BE-R5): a page it reads once is
        // the hand's first victim, so a scan larger than the pool does not
        // displace the working set, and a page it finds resident - or reads
        // again - warms as any hit. Nested inner walks stay `kRead`: a join's
        // inner relation is read once per outer row, which is heat.
        const bool outermost = index == 0 && parent_ == nullptr;
        const storage::PageAccess walk_access =
            outermost ? storage::PageAccess::kScan : storage::PageAccess::kRead;
        // Across the slice boundaries below too: the guard is the whole
        // walk's, and a yield resumes at a leaf the walk has not visited.
        storage::PageChainGuard guard("relation walk");
        // **DE-Q8: the walk gives its core back at a read that would wait.**
        // Its own leaf fetch, and only that: the refusal is this access
        // kind's, never the thread's, because the walk yields.
        const bool parks_at_reads =
            outermost && slice_ != nullptr && slice_->hand_page != nullptr;
        // `cur` was refused, parked for, and is being fetched once more: the
        // pass that follows a park. **One call of `visit_leaf` in the loop,
        // whichever pass it is**: with one call site the compiler inlines
        // the slot loop into this coroutine. DE's first form called it at
        // two, and at the close a resident walk read 8 % fewer pages a
        // second than before DE; with this form it is within about 1 %
        // (`bench/v3.4.14/results-de-close-v3.4.14-6-g1942cb9a.md`). The
        // cause of the 8 % was not isolated.
        bool refetch = false;
        for (;;) {
            if (!refetch) {
                if (Status s = guard.Visit(cur); !s.ok()) co_return s;
                // Per page, not once per walk: the VM makes these fetches
                // itself now, so R1's guard gets to see each one.
                NoteFetch();
            }
            // The fetch after a park is synchronous - whatever the read
            // did, the walk gets its leaf - and books no second
            // `io.page_read`: the park was the event (DE-R9).
            std::optional<storage::ParkedReadScope> parked;
            if (refetch) parked.emplace(cur);
            auto next = visit_leaf(cur, parks_at_reads && !refetch
                                            ? storage::PageAccess::kScanNoWait
                                            : walk_access);
            parked.reset();
            if (parks_at_reads && !refetch && !next.ok() &&
                next.status().code() == StatusCode::kNotResident) {
                // Refused at the fetch, before a slot was visited, and
                // nothing is held: no pin, no span (the suspend audit
                // checks), the view and the borrow as at a slice boundary.
                // The leaf goes to the page reader and this task parks; the
                // next pass fetches the same leaf once more - past the
                // guard and the count above, which have seen it - so a
                // refused leaf is visited exactly once whatever the read
                // did. What is sealed leaves first, as at a slice boundary:
                // the client reads while the device works.
                if (slice_->at_boundary) slice_->at_boundary();
                const std::function<bool()>* const read = slice_->hand_page(cur);
                {
                    sched::WaitScope wait(stats::WaitEvent::kIoPageRead, cur);
                    co_await sched::WaitUntil{read};
                }
                // As after a yield: other statements ran here meanwhile, and
                // a cancel is taken (a walk takes one where it gives its
                // core back, DE-Q15).
                storage::ClearStructuralRefusal();
                if (Status s = TakeCancel(); !s.ok()) co_return s;
                refetch = true;
                continue;
            }
            refetch = false;
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

            // ---- BA-S15: the slice boundary ----------------------------
            //
            // No pin and no span here (the suspend audit checks), the read
            // view and the borrow held - both statement-scoped and holding no
            // page - and `cur` names the next page to read. A split while
            // the walk is away leaves it neither missing nor repeating a
            // row: a leaf keeps its id and its lower half and links the new
            // page after itself, so a split of `cur` is walked in full and a
            // split of a page already walked moves only rows already emitted
            // to a page before `cur`; the view is fixed, so nothing written
            // since is seen. The same holds across cores today, where no pin
            // is held between pages either - the yield only widens the gap.
            if (outermost && slice_ != nullptr &&
                ++slice_pages_ >= slice_->pages_per_slice) {
                if (Status s = co_await YieldAtSliceBoundary(); !s.ok()) co_return s;
            }
        }
    }

    // **The slice boundary itself** (BA-S15): what the outermost walk does
    // every `SlicePolicy::pages_per_slice` pages, holding no pin and no
    // span - its read view and borrow are statement-scoped. The caller has
    // counted the pages and holds nothing.
    sched::Coro YieldAtSliceBoundary() {
        slice_pages_ = 0;
        if (Status s = TakeCancel(); !s.ok()) co_return s;
        if (slice_->at_boundary) slice_->at_boundary();
        if (slice_->resume_when != nullptr && !(*slice_->resume_when)()) {
            sched::WaitScope wait(stats::WaitEvent::kNetSendBackpressure);
            co_await sched::WaitUntil{slice_->resume_when};
        } else {
            co_await sched::Yield{};
        }
        // The note is the thread's, and other statements ran here
        // meanwhile: what this walk ends with must be its own (BA-S14's
        // re-run reads it).
        storage::ClearStructuralRefusal();
        co_return TakeCancel();
    }

    // **The reverse walk (CV O2)**: `ORDER BY <tuple key> DESC` over a walk
    // or a tuple key range of the driving relation, read from its high end
    // down, so the LIMIT stops it where a forward walk and a top-N heap read
    // every row.
    //
    // A leaf links forward only (CV-Q1 (a): no on-disk change), so a step
    // left is a descent: once a leaf is read, the next is the one holding
    // the greatest keys below its `min_key`, found by
    // `BtreeFetchLeafAtOrBelow`, which moves right while a split has moved
    // that range past where the descent landed. **No leaf is revisited by
    // pointer** (CV-R3): each step is a seek by key from the last leaf's
    // `min_key`, which is immutable (invariant 2), and every key below it is
    // in that leaf's left part of the tree - so a split on another core,
    // or a root growth, between two steps neither skips a row nor repeats
    // one; the view is fixed, so nothing written since is seen. A leaf's
    // slots are read from the last down, keyed rows ascending by placement
    // (BD-R1), and every slot above the bound is passed over.
    //
    // **It yields where the forward walk yields** (CV-R4), at the bottom of
    // its leaf loop, holding its view, its borrow and the next bound - no
    // page. A leaf it misses is a blocking read: the park DE gives the
    // forward walk's leaf fetch is not given here (`known-gaps.md`).
    sched::Coro RunReverseWalkStep(const std::vector<Step>& steps, std::size_t index,
                                   const Step& step, const catalog::TableAccess& access) {
        const std::uint64_t low = step.range.has_value() ? step.range->low : 0;
        std::uint64_t bound = step.range.has_value() ? step.range->high : kMaxKeystoneId;
        StepStats& step_stats = stats_.For(step.step_id);
        const bool outermost = index == 0 && parent_ == nullptr;
        const storage::PageAccess walk_access =
            outermost ? storage::PageAccess::kScan : storage::PageAccess::kRead;
        storage::PageChainGuard guard("reverse relation walk");
        for (;;) {
            NoteFetch();
            auto held = btree::BtreeFetchLeafAtOrBelow(store_, access.desc_page_id, bound,
                                                       walk_access);
            if (!held.ok()) co_return held.status();
            if (Status s = guard.Visit(held.value().id); !s.ok()) co_return s;
            ++step_stats.pages_fetched;
            heap::PageView leaf(held.value().ref.bytes());
            const std::uint16_t n = leaf.slot_count();
            for (std::uint16_t i = n; i-- > 0;) {
                // A retired slot carries no row, and `AcceptTupleAt` would
                // decline it.
                auto payload = leaf.PayloadAt(i, n);
                if (!payload.ok()) continue;
                auto tuple_key = KeystoneIdOfPayload(payload.value());
                if (!tuple_key.ok()) co_return tuple_key.status();
                if (tuple_key.value() > bound) continue;
                if (tuple_key.value() < low) break;  // and so `min_key <= low`
                if (Status s = AcceptTupleAt(steps, index, step, access, held.value().id, leaf, i, n);
                    !s.ok()) {
                    co_return s;
                }
                if (stopped_) co_return Status::OK();
            }
            const std::uint64_t min_key = leaf.min_key();
            held.value().ref.Release();
            // The one end: every key left of this leaf is below its
            // `min_key`, so at or below `low` nothing left qualifies - and the
            // leftmost leaf's `min_key` is 0 (`FormatRoot`; a split keeps the
            // left page's), so the head ends it too.
            if (min_key <= low) co_return Status::OK();
            bound = min_key - 1;
            if (outermost && slice_ != nullptr && ++slice_pages_ >= slice_->pages_per_slice) {
                if (Status s = co_await YieldAtSliceBoundary(); !s.ok()) co_return s;
            }
        }
    }

    // The location-and-tuple key half of a recorded entry, shared by the Cabin
    // recording and the build's bucketing - both feed the same 24-byte
    // struct, and the *gate* (a key comparison there, MakeValueKey here)
    // stays at each call site.
    //
    // **The tuple key comes out of the Keystone word, not out of a column
    // decode.** The id sits at a fixed offset precisely so finding it
    // costs no schema walk (`KeystoneIdOfPayload`, keystone.hpp), and an entry
    // wants the id - not an AstValue holding it (a column decode cost
    // ~12 ns a row at JB5's gate). A Keystone id is kKeystoneIdBits wide
    // and unsigned by construction, so no sign guard is needed.
    //
    // The epoch was captured under this row's span - the hint names the
    // page as it was at observation (R4).
    StatusOr<stats::CabinEntry> EntryForRow(PageId page_id, std::uint16_t slot,
                                            std::uint64_t observed_epoch) {
        auto tuple_key = KeystoneIdOfPayload(version_);
        if (!tuple_key.ok()) return tuple_key.status();
        stats::CabinEntry entry;
        entry.tuple_key = tuple_key.value();
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
        return AcceptTupleAt(steps, index, step, access, page_id, page, slot, page.slot_count());
    }

    // The same, with the page's slot count the caller has already read - a
    // walk reads it once per page (BM-R5).
    Status AcceptTupleAt(const std::vector<Step>& steps, std::size_t index, const Step& step,
                         const catalog::TableAccess& access, PageId page_id,
                         heap::PageView& page, std::uint16_t slot, std::uint16_t nr_slots) {
        bool decoded = false;

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
        // The page's page epoch at the moment of access, captured under
        // the span because the trail record below runs after its release -
        // recorded so replay's rule 2 has something real to compare
        // (docs/spec/heap-and-tuple.md §3.1a R4, PX04). One u64 load, on the
        // two paths that leave the span with a copied row; a visible row the
        // kernel rejects never reads it, and every reader is past the
        // rejected return (BT-S3).
        std::uint64_t observed_epoch = 0;
        // **The scan kernel** (BM-R1..R4): armed only once this step's
        // checks have run in this statement, so its first visible row always
        // takes the full path below. A row it rejects is one the residual
        // would reject: it is examined and charged as such (BM-R2), and never
        // copied or decoded.
        const bool recording_here =
            recording_ != nullptr && recording_->step_id == step.step_id;
        const ScanKernel* kernel = ArmedKernel(step, recording_here);
        // The length check first (invariant 13; BM-R3): a payload of another
        // length is the decode's `Corruption`, on the full path.
        const auto kernel_rejects = [&](std::span<const std::byte> payload) {
            return kernel != nullptr && payload.size() == access.layout.row_size &&
                   ScanKernelRejects(*kernel, access.layout, payload);
        };
        bool kernel_rejected = false;
        {
            PageSpanGuard span;
            // Read with no Status (BT-S2): a slot ReadTupleIn() would refuse -
            // out of range, retired, or malformed - is no row here, as before.
            heap::PageView::Tuple tuple{};
            if (page.TryReadTupleIn(slot, nr_slots, tuple) == heap::PageView::TupleRead::kRead) {
                switch (txn::Classify(snapshot_.view, visibility_memo_, tuple)) {
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
                        version_.assign(tuple.payload.begin(),
                                        tuple.payload.end());
                        observed_epoch = page.PageEpoch();
                        walk_trx_id = tuple.trx_id;
                        walk_undo_ptr = tuple.undo_ptr;
                        walk_deleted = tuple.deleted;
                        needs_walk = true;
                        break;
                    case txn::Visibility::kVisible:
                        // The kernel reads the visible version where it lies,
                        // under the span and the leaf's read hold (BM-R3),
                        // after the length check invariant 13 makes first;
                        // a rejected row is never copied.
                        if (kernel_rejects(tuple.payload)) {
                            kernel_rejected = true;
                            break;
                        }
                        // **The row's bytes, copied; the row itself, not
                        // built yet.** The copy is fixed-size (invariant 13)
                        // and costs a memcpy; building the row costs an
                        // 80-byte AstValue per column, and this step may be
                        // about to reject it on one of them. So the bytes
                        // come out from under the span and the decode
                        // happens below, in two halves.
                        version_.assign(tuple.payload.begin(),
                                        tuple.payload.end());
                        observed_epoch = page.PageEpoch();
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
            // entitled to. **The kernel reads them there too, never the
            // page** (BM-R3, `[quiet-wrong]`).
            if (kernel_rejects(version_)) {
                kernel_rejected = true;
            } else {
                decoded = true;
            }
        }

        if (kernel_rejected) {
            // What the full path does for a row its residual rejects, in its
            // order: examined, then charged (so `ResourceExhausted` falls on
            // the same row), and not matched.
            StepStats& rejected_stats = stats_.For(step.step_id);
            ++rejected_stats.rows_examined;
            ++rejected_stats.rows_kernel_rejected;
            return budget_.ChargeRow();
        }
        if (!decoded) return Status::OK();  // dead or out-of-range slot

        // ---- Decode what the filter reads, and only that ----------------
        //
        // `Step::filter_columns` is the compiler's answer to "which of this
        // relation's columns does the residual look at" (step_chain.hpp).
        // Decoding those, testing, and building the rest only for a row that
        // survives is what separates *reading* a row from *materialising*
        // one: a rejected row builds no AstValue for a column the residual
        // does not read.
        //
        // A step with a sub-chain, or a relation wider than 64 columns,
        // carries kAllColumns and takes the whole row here as before.
        const std::span<parser::AstValue> slots =
            frame_.SlotsFor(static_cast<std::uint16_t>(index));
        // **Recording a Cabin adds nothing to this decode.** The cabined
        // equality *is* this step's filter, so the key column already sits
        // in `filter_columns` (the invariant WalkAndRecord's guard checks);
        // the tuple key is read from the Keystone word below, only for a row whose
        // key matches.
        const bool partial = step.filter_columns != Step::kAllColumns;

        // **What cannot change within the statement is checked once**
        // (BM-R6): the decode's schema-and-layout checks and the residual's
        // malformed-chain checks depend on the step, its relation and the
        // frame's shape, never on a row. Here, at the first visible row of
        // the step by any access kind - a walk, a descent, the probe memo,
        // a replay, a Cabin resolve - and before anything reads a cell, so
        // a malformed chain is refused at the row it is refused at today
        // (an empty relation still is not), and every later row pays the
        // payload's length check alone. A later conjunct's malformation is
        // refused here even where an earlier conjunct false on every row
        // used to mask it (BM-Q7).
        if (!StepShapeChecked(step)) {
            if (Status s = CheckStepShapeOnce(step, index, access, slots); !s.ok()) return s;
        }

        // Decoded and then, once nothing is live, resolved - one call,
        // because the same three lines at four sites is how one of them
        // drifts into handing back the previous row's value (row_codec.hpp).
        if (Status s = DecodeAndResolve(store_, access.schema, access.layout, version_, slots,
                                        step.filter_columns, spills_, DecodeChecks::kPayloadOnly);
            !s.ok()) {
            return s;
        }
        StepStats& step_stats = stats_.For(step.step_id);
        step_stats.spill_fetches += spills_.size();
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
            if (RecordsRow(*recording_, slots)) {
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
        // completion exit, so the plain walk charges unconditionally, with
        // no per-row guard.
        if (Status s = budget_.ChargeRow(); !s.ok()) return s;

        // ---- The inner build's bucketing (workplan JB3) -----------------
        //
        // Split evaluation while this step's build is live: the
        // non-correlated conjuncts decide *bucketing*, the correlated one
        // then decides *emission*. Every conjunct but the key reads only
        // this step's own columns - `BuildKeyOf` arms no other step - so
        // the bucketing answers the same for every outer row. On a
        // well-formed chain the two forms give one verdict - AND over total predicates commutes, and both
        // run the same EvaluateConjunctChecked body, so the split cannot
        // drift from the whole; the step's conjuncts were checked once
        // above, so a malformed chain never reaches either. The map holds
        // every row passing the non-correlated residual (spec §2): a row
        // failing the current outer key's equality emits nothing today and
        // is precisely what a later outer row's probe will ask for.
        const bool building_here = building_ != nullptr && building_->step_id == step.step_id;
        if (building_here) {
            if (!EvaluateAllExceptChecked(schemas_, step.residual, frame_,
                                          building_->residual_pos)) {
                return Status::OK();
            }
        } else if (!EvaluateAllChecked(schemas_, step.residual, frame_)) {
            // Checked once above, so the comparisons alone.
            return Status::OK();
        }
        // The function conjuncts (AP-S4), on the row the residual kept.
        // A step carrying one takes no build annotation (step_compiler.cpp),
        // so this never runs while a map is being filled.
        if (!step.fn_residual.empty()) {
            auto fn_matched = EvaluateFunctionConjuncts(step.fn_residual, frame_);
            if (!fn_matched.ok()) return fn_matched.status();
            if (!fn_matched.value()) return Status::OK();
        }
        // The boolean conjuncts (BU-S1), after the function ones. A step
        // carrying one takes no build annotation either.
        if (!step.bool_residual.empty()) {
            // A key set's row asks the leaf its descent was made for first
            // (CU-Q9); the conjunct evaluated is still the written one.
            const bool set = step.key_set.has_value();
            const BoolLeafFirst first{set ? step.key_set->conjunct : 0, key_set_leaf_};
            auto bool_matched = EvaluateBoolConjuncts(schemas_, step.bool_residual, frame_,
                                                      nullptr, set ? &first : nullptr);
            if (!bool_matched.ok()) return bool_matched.status();
            if (!bool_matched.value()) return Status::OK();
        }
        if (building_here) {
            // The cap (workplan JB5): a row that would push the map past
            // `max_rows` trips `over_cap` instead of entering - the rest
            // of this walk stops bucketing and the publish site declines
            // the step. Emission is untouched: this row, and every later
            // one, still answers through the conjuncts below. The key is
            // made *before* the cap is consulted, so an unkeyable row - a
            // NULL keys nothing, no equality matches it (MakeValueKey's
            // contract) - neither consumes a slot nor trips a cap it
            // never pressed (null.md; the contract suite's NULL case pins
            // it).
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
            if (!EvaluateConjunctChecked(schemas_, step.residual[building_->residual_pos],
                                         frame_)) {
                return Status::OK();
            }
        }

        // ---- The row survived: build the rest of what is read ------------
        //
        // Everything past this point reads columns the filter had no reason
        // to touch - the projection, the next step's probe key, the trail's
        // tuple key, the fold's items. Until now those slots still hold the
        // *previous* row's values, so this is not an optimization to skip:
        // it is what makes the partial decode above correct.
        //
        // **`read_columns`, not everything else** (workplan AP01): the
        // filter's mask narrows only a rejected row's decode, so a surviving
        // row decodes what the statement reads and nothing more.
        //
        // A step that reads nothing further - a bare `COUNT(*)` over a
        // relation with no predicate - decodes nothing here, and the walk
        // becomes page iteration and a counter.
        const std::uint64_t rest = step.read_columns & ~step.filter_columns;
        if (partial && rest != 0) {
            if (Status s = DecodeAndResolve(store_, access.schema, access.layout, version_, slots,
                                            rest, spills_, DecodeChecks::kPayloadOnly);
                !s.ok()) {
                return s;
            }
            step_stats.spill_fetches += spills_.size();
        }

        // An ordered index step's key check (CV-R2): the row leaves only from
        // the entry its visible version is filed under.
        if (IsIndexOrder(step.emit_order)) {
            auto matched = OrderedKeyMatches(step, access, slots);
            if (!matched.ok()) return matched.status();
            if (!matched.value()) return Status::OK();
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
            if (sub.in_tree) continue;  // its boolean conjunct's, below
            auto value = EvaluateSubChain(sub, frame_);
            if (!value.ok()) return value.status();
            if (!Collapse(value.value())) return Status::OK();
        }
        // The boolean conjuncts that read a sub-chain (BU-S3), last for the
        // same reason.
        if (!step.sub_chains.empty() && !step.bool_residual.empty()) {
            auto matched = EvaluateTreeConjuncts(schemas_, step, frame_);
            if (!matched.ok()) return matched.status();
            if (!matched.value()) return Status::OK();
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
        // test is the step's *kind* and its key set (CU-Q4), not how the row
        // was found.
        if (trail_ != nullptr && IsTrailReplayable(step)) {
            TouchedTuple touched;
            touched.rel_oid = access.oid;
            // Column 0 is the Keystone tuple key, already decoded into the frame
            // by this step - so the id costs a read, never a lookup.
            const parser::AstValue& tuple_key =
                frame_.Get(ColumnRef{0, static_cast<std::uint16_t>(index), 0});
            touched.tuple_key = tuple_key.int_val < 0 ? 0 : static_cast<std::uint64_t>(tuple_key.int_val);
            touched.page_id = page_id;
            touched.page_epoch = observed_epoch;
            touched.slot = slot;
            touched.step_id = static_cast<std::uint16_t>(step.step_id);
            trail_->Add(touched);
        }

        if (index + 1 == steps.size()) {
            // Terminal: the emit is a plain call, so a single-step scan
            // allocates zero frames per row.
            return EmitRow();
        }
        // A deeper local step, driven synchronously: one frame per row on
        // multi-step chains only.
        return RunToCompletionAtWalkBoundary(RunStep(steps, index + 1));
    }

    // BM-R6's once: a step's decode shape and residual checked at its first
    // visible row of the statement, by whichever access kind reaches it.
    // The caller asks `StepShapeChecked` first, so a checked step's row
    // builds no Status (AP02).
    Status CheckStepShapeOnce(const Step& step, std::size_t index,
                              const catalog::TableAccess& access,
                              std::span<const parser::AstValue> slots) {
        if (Status s = CheckDecodeShape(access.schema, access.layout, slots); !s.ok()) return s;
        if (Status s = CheckConjuncts(schemas_, step, frame_); !s.ok()) return s;
        std::vector<std::optional<ScanKernel>>& kernels = statement_->kernels;
        if (step.step_id >= kernels.size()) kernels.resize(step.step_id + 1);
        kernels[step.step_id] = CompileScanKernel(access.schema, access.layout, step.residual,
                                                  static_cast<std::uint16_t>(index));
        return Status::OK();
    }

    // Whether BM-R6's check has run for `step` in this statement: asked
    // before the call, so a checked step's row builds no Status (AP02).
    bool StepShapeChecked(const Step& step) const {
        const std::vector<std::optional<ScanKernel>>& kernels = statement_->kernels;
        return step.step_id < kernels.size() && kernels[step.step_id].has_value();
    }

    // The step's kernel, if this row may be filtered by it: the switch is on
    // (BM-Q5), the step has passed its check this statement - so the kernel
    // never reads an offset before BM-R6's checks have run - it holds a term,
    // and no Cabin recording of this step is live (BM-R4: a row the other
    // conjuncts reject must still be recorded if its key matches). One
    // lookup answers both: a kernel is present only once its step's check
    // has passed (BT-S3).
    const ScanKernel* ArmedKernel(const Step& step, bool recording_here) const {
        if (!budget_.scan_kernel() || recording_here) return nullptr;
        const std::vector<std::optional<ScanKernel>>& kernels = statement_->kernels;
        if (step.step_id >= kernels.size()) return nullptr;
        const std::optional<ScanKernel>& kernel = kernels[step.step_id];
        return kernel.has_value() && !kernel->empty() ? &*kernel : nullptr;
    }

    catalog::Catalog& catalog_;
    storage::PageStore& store_;

    // Scratch for AcceptTupleAt()'s decode: filled by the decode and drained
    // by the resolve with no span live, since a spilled value is a var-heap
    // fetch (row_codec.hpp). Each decode clears it (row_codec.cpp), so a row
    // the kernel rejects never touches it (BT-S3); reused across rows so a
    // scan that spills nothing allocates nothing for the possibility.
    std::vector<PendingSpill> spills_;

    // The read view every tuple is filtered through, and the log an
    // invisible writer is stepped back through (docs/spec/txn.md section 4).
    // Held by value: a Snapshot is a POD and copying one is cheaper than
    // a pointer indirection on every row.
    txn::Snapshot snapshot_;
    // `snapshot_.view`'s answers, one per writer (txn/visibility.hpp).
    // Valid for the runner's life because `snapshot_` never changes in it.
    txn::VisibilityMemo visibility_memo_;

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

    // The statement's recorded trail, or null when nothing is replaying.
    // Shared with every sub-chain for the reason the collector is: step ids
    // are global across the statement.
    const TrailReplay* replay_ = nullptr;

    // ---- The one-entry probe memo (V19) ---------------------------------
    //
    // A probe step descends for the key its outer row produced, and a
    // foreign key that repeats - many trades for one account - repeats
    // consecutively in the outer walk's order, so one entry costs a
    // comparison and catches most repeats. It caches only the *location*;
    // the row is re-verified, re-read and re-filtered exactly as a fresh
    // descent's would be, so a hit and a miss run the same code from the
    // tuple onward.
    //
    // **The memo is per step, and `memo_step_` is what makes that true**:
    // one runner runs every step of its chain through one memo, and without
    // the step in the key two tuple-key-descending steps whose keys coincide would
    // serve the second step the first relation's location - a Corruption
    // or, at equal row widths, a silent row from the wrong table.
    bool memo_valid_ = false;
    std::uint32_t memo_step_ = 0;
    std::uint64_t memo_key_ = 0;
    PageId memo_page_ = kInvalidPageId;
    std::uint16_t memo_slot_ = 0;
    std::uint32_t memo_epoch_ = 0;  // the leaf's page epoch at the descent

    // The conjunct leaf that names the value whose descent found the row a
    // key set step is about to accept (`RunKeySetStep`, an index or Cabin
    // step's phase 2), read by that step's own residual before anything
    // deeper runs.
    std::uint32_t key_set_leaf_ = 0;

    // ---- Cabin (docs/spec/cabin.md) --------------------------------------
    //
    // The instance's observed sets (one store for every core), or null when
    // no Cabin is configured.
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
        // An expression Cabin's `F` (AQ-R5), or null for a column Cabin.
        // Points into the step's `CabinProbe`, which outlives the walk.
        const CompiledCabinExpr* f = nullptr;
        const parser::AstValue* value = nullptr;
        // The set's key, for the entry-cap lapse to mark it sticky in the
        // store. Points at WalkAndRecord's parameter, which outlives the
        // walk for the reason `value` does.
        const stats::CabinKey* key = nullptr;
        std::vector<stats::CabinEntry> entries;
    };
    Recording* recording_ = nullptr;

    // Whether the row in `slots` belongs to the set being recorded: its key
    // column equals the probed value, or - for an expression or predicate Cabin - `F` of
    // it does, the same evaluation the write hook keys the set by, so a
    // recorded set and a witnessed one are one function (AQ-R5).
    static bool RecordsRow(const Recording& recording, std::span<const parser::AstValue> slots) {
        if (recording.f != nullptr) {
            if (recording.f->inputs.back() >= slots.size()) return false;
            const std::optional<parser::AstValue> key = CabinKeyOf(*recording.f, slots);
            return key.has_value() &&
                   CompareValues(/*type_val=*/0, *key, *recording.value, parser::CompareOp::kEq);
        }
        return recording.col_pos < slots.size() &&
               CompareValues(/*type_val=*/0, slots[recording.col_pos], *recording.value,
                             parser::CompareOp::kEq);
    }

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
        std::uint64_t tuple_key = 0;
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
        std::uint32_t epoch = 0;  // the page's page epoch the slot was valid under
        std::uint32_t leaf = 0;   // its `ServedSet::leaf`
    };
    std::vector<Located> serve_scratch_;
    std::unordered_set<std::uint64_t> seen_tuple_keys_;

    // RunIndexStep's phase-1 output and its covered-column decode slot. Held
    // on the runner rather than built per step, for TrailCollector's reason:
    // a per-statement allocation on the read path is measurable.
    std::vector<std::uint64_t> index_scratch_;
    // A heap's index step (DN, IX1): the rowid of the entry that claimed
    // each tuple key, which phase 2 fetches instead of descending.
    std::unordered_map<std::uint64_t, std::uint64_t> index_rowids_;
    parser::AstValue covered_scratch_;
    // The tuple keys every entry of which the covering filter has rejected so far:
    // a tuple key leaves when a later entry keeps its row, so what remains after
    // the walk is the descents avoided.
    std::unordered_set<std::uint64_t> filtered_tuple_keys_;

    // A correlated index probe's per-row bounds (IndexProbe::key_from): the
    // compile-time padding templates with the outer row's value encoded into
    // the leading width. On the runner for index_scratch_'s allocation
    // reason, and safe against its re-entrancy hazard by a narrower
    // argument: these are read only inside phase 1's leaf visit, which never
    // re-enters RunStep - phase 2, which does, no longer touches them.
    std::vector<std::byte> corr_low_;
    std::vector<std::byte> corr_high_;

    // The ordered index run's key check (CV-R2): the entry key the row in
    // `AcceptTupleAt` was reached by, whether its visible version matched,
    // and the encode's buffer; set around one `AcceptTupleAt` and read only
    // by the driving step, whose `emit_order` no inner step carries. While
    // the decline walks, `ordered_collect_` takes the rows instead.
    std::span<const std::byte> ordered_key_;
    bool ordered_matched_ = false;
    std::vector<std::byte> ordered_scratch_;
    using OrderedRecord = std::pair<std::vector<std::byte>, std::uint64_t>;
    std::vector<OrderedRecord>* ordered_collect_ = nullptr;

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

    // ---- The statement's store: inner builds (workplan JB3/JB5/JB6), and
    // hoisted answers ----------------------------------------------------
    //
    // The store and its state types are above the class, because the
    // constructor takes one: the statement owns it, every nested runner
    // borrows it. Owned only by a top-level runner; a nested one leaves
    // `owned_statement_` empty and points `statement_` at its parent's.
    StatementStore owned_statement_;
    StatementStore* statement_ = nullptr;

    // BA-S15: the statement's slice, and the outermost walk's pages since
    // its last yield.
    const SlicePolicy* slice_ = nullptr;
    std::uint32_t slice_pages_ = 0;
};

// The highest step_id anywhere under `step`/`chain`, sub-chains included.
// step_ids are global across the statement (step_chain.hpp), so this is
// how many slots the per-step stats vector needs.
template <typename Tree>
std::uint32_t MaxStepId(const Tree& tree) {
    std::uint32_t max = 0;
    ForEachStep(tree, [&](const Step& step) {
        max = std::max(max, step.step_id);
        return true;
    });
    return max;
}

}  // namespace

StepStats& StepStats::operator+=(const StepStats& other) noexcept {
    relation_opens += other.relation_opens;
    rows_examined += other.rows_examined;
    rows_kernel_rejected += other.rows_kernel_rejected;
    rows_matched += other.rows_matched;
    sub_chain_runs += other.sub_chain_runs;
    probe_memo_hits += other.probe_memo_hits;
    correlated_scans += other.correlated_scans;
    spill_fetches += other.spill_fetches;
    pages_fetched += other.pages_fetched;
    trail_replays += other.trail_replays;
    trail_misses += other.trail_misses;
    range_pages_pruned += other.range_pages_pruned;
    index_entries_scanned += other.index_entries_scanned;
    index_entries_filtered += other.index_entries_filtered;
    index_rows_resolved += other.index_rows_resolved;
    index_walks += other.index_walks;
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

StepStats& ExecStats::Grow(std::uint32_t step_id) {
    steps.resize(step_id + 1);
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

void InstallSuspendAudit() noexcept {
    // The executor's answer to "is it safe to be suspended right now?".
    //
    // R1 already forbids a page *fetch* under a live span, because nothing
    // pins the frame the span points into. Suspending under one is strictly
    // worse: the span stays live for arbitrary wall time, across every other
    // statement that runs on this core in between, while the pool may
    // evict.
    //
    // The pin half is the same rule one layer down (P4d-3): a span is a
    // window into a pinned frame, and a suspension holding *any* pin keeps
    // a frame unreclaimable for arbitrary wall time. Every legal
    // suspension point - between statements, between pages of a walk -
    // holds no pin, so the check is an equality with zero, not a budget.
    //
    // The suspending thread's pins, never a store's total: a peer core's
    // ordinary pin on the shared pool is not this coroutine's.
    //
    // Installed here rather than checked here: `sched/` sits below this
    // layer and must not know what a page is (coro.hpp's SuspendAuditFn).
    sched::SetSuspendAudit([]() -> std::string_view {
        if (g_live_spans > 0) {
            return "a coroutine suspended while holding a page span (parser-v2.md I15 R1)";
        }
        if (storage::PinsHeldOnThisThread() != 0) {
            return "a coroutine suspended while holding a page pin "
                   "(workplan-crosscore.md P4d-3)";
        }
        // A window is per thread, which is sound only while no task parks
        // inside one: another task on this reactor would spend its share.
        if (storage::NoRefuseWindowOpenOnThisThread()) {
            return "a coroutine suspended inside a no-refuse window (BE-Q11)";
        }
        return {};
    });
}

void UninstallSuspendAudit() noexcept {
    sched::SetSuspendAudit(nullptr);
}

StatusOr<bool> EvaluateConjuncts(catalog::Catalog& catalog, storage::PageStore& store,
                                 const std::vector<const catalog::Schema*>& schemas,
                                 const Step& step, const ChainFrame& frame, ExecStats* stats,
                                 const Budget& budget, const txn::Snapshot* snapshot,
                                 const BoolLeafFirst* first) {
    if (!EvaluateAllChecked(schemas, step.residual, frame)) return false;
    if (!step.fn_residual.empty()) {
        auto fn_matched = EvaluateFunctionConjuncts(step.fn_residual, frame);
        if (!fn_matched.ok()) return fn_matched.status();
        if (!fn_matched.value()) return false;
    }
    if (!step.bool_residual.empty()) {
        auto bool_matched =
            EvaluateBoolConjuncts(schemas, step.bool_residual, frame, nullptr, first);
        if (!bool_matched.ok()) return bool_matched.status();
        if (!bool_matched.value()) return false;
    }
    if (step.sub_chains.empty()) return true;

    ExecStats local;
    ExecStats& counters = stats != nullptr ? *stats : local;
    counters.For(MaxStepId(step));  // see Execute() for why this is pre-sized
    Budget spend = budget.Fresh();
    // A runner with no sink of its own: it exists only to lend its
    // sub-chain evaluation, which builds its own sink per sub-chain.
    static const RowSink kUnused = nullptr;
    // No collector: this path evaluates one already-located row's
    // conjuncts for UPDATE or DELETE, which is not a chain execution and has no trail
    // of its own to contribute to.
    ChainRunner runner(catalog, store, kUnused, /*depth=*/0, /*parent=*/nullptr, counters, spend,
                       /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr, snapshot, /*indexes=*/true);

    for (const SubChain& sub : step.sub_chains) {
        if (sub.in_tree) continue;  // its boolean conjunct's, below
        auto value = runner.EvaluateSubChain(sub, frame);
        if (!value.ok()) return value.status();
        // The same collapse point every other caller uses.
        if (!Collapse(value.value())) return false;
    }
    return runner.EvaluateTreeConjuncts(schemas, step, frame);
}

namespace {

// Execute's body as a coroutine, because the runner's steps are. It yields
// only under a slice policy (ExecuteAsync); `Execute` passes none and
// drives it to completion inline.
sched::Coro ExecuteChain(catalog::Catalog& catalog, storage::PageStore& store,
                         const StepChain& chain, const RowSink& sink, ExecStats* stats,
                         const Budget& budget, TrailCollector* trail, const TrailReplay* replay,
                         stats::CabinStore* cabins, const txn::Snapshot* snapshot, bool indexes,
                         const SlicePolicy* slice = nullptr) {
    if (chain.steps.empty()) {
        co_return Status::InvalidArgument("a step chain with no steps reads nothing");
    }

    // Locals live in this coroutine's frame, which outlives every
    // suspension beneath.
    ExecStats local;
    ExecStats& counters = stats != nullptr ? *stats : local;

    // Sized once, up front, for the highest step_id the chain contains.
    // Not an optimization: the runner holds a `StepStats&` across nested
    // calls that also touch the stats, and a vector that grew underneath
    // one would leave it dangling. Pre-sizing makes every reference stable
    // for the whole execution, which is a property worth having by
    // construction rather than by reviewing every call site.
    counters.For(MaxStepId(chain));
    counters.stopped = false;  // a reused scratch carries the last run's

    // One mutable budget for this statement: the caller's limits and
    // knobs, the spend counter at zero. `Fresh()` owns what survives the
    // copy, so no entry point can drop a field.
    Budget spend = budget.Fresh();

    ChainRunner runner(catalog, store, sink, /*depth=*/0, /*parent=*/nullptr, counters, spend,
                       trail, replay, cabins, snapshot, indexes);
    runner.SetSlice(slice);

    // Hoisted sub-chains run **once**, before the outer chain opens. An
    // uncorrelated subquery's answer is the same for every outer row by
    // definition, so running it per row would compute one value n times.
    //
    // The consequence worth having: a false uncorrelated `EXISTS` answers
    // the whole statement here, and the outer relation is never opened at
    // all. That is a real saving on a large relation, and it is only
    // available because hoisting is decided structurally at compile.
    //
    // They run on the gated driver, as a nested step does.
    auto opens = runner.HoistedAllTrue(chain.hoisted);
    if (!opens.ok()) co_return opens.status();
    if (!opens.value()) co_return Status::OK();  // no rows, nothing opened

    Status ran = co_await runner.Run(chain.steps);
    counters.stopped = runner.stopped();
    co_return ran;
}

}  // namespace

sched::Coro ExecuteAsync(catalog::Catalog& catalog, storage::PageStore& store,
                         const StepChain& chain, const RowSink& sink, ExecStats* stats,
                         const Budget& budget, TrailCollector* trail, const TrailReplay* replay,
                         stats::CabinStore* cabins, const txn::Snapshot* snapshot, bool indexes,
                         const SlicePolicy& slice) {
    return ExecuteChain(catalog, store, chain, sink, stats, budget, trail, replay, cabins, snapshot,
                        indexes, &slice);
}

Status Execute(catalog::Catalog& catalog, storage::PageStore& store, const StepChain& chain,
               const RowSink& sink, ExecStats* stats, const Budget& budget,
               TrailCollector* trail, const TrailReplay* replay, stats::CabinStore* cabins,
               const txn::Snapshot* snapshot, bool indexes) {
    // No slice policy, so nothing beneath parks and the gated driver
    // completes the coroutine inline.
    return RunToCompletionAtWalkBoundary(ExecuteChain(catalog, store, chain, sink, stats, budget,
                                                      trail, replay, cabins, snapshot, indexes));
}

}  // namespace kds::exec
