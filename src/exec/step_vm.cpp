#include "kds/exec/step_vm.hpp"

#include "kds/sched/coro.hpp"

#include <algorithm>
#include <unordered_set>

#include <string>

#include "kds/exec/row_codec.hpp"
#include "kds/exec/tuple_verify.hpp"
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
class ChainRunner {
public:
    ChainRunner(catalog::Catalog& catalog, storage::PageStore& store, const RowSink& sink,
                std::uint32_t depth, const ChainFrame* parent, ExecStats& stats, Budget& budget,
                TrailCollector* trail, const TrailReplay* replay, stats::CabinStore* cabins,
                const txn::Snapshot* snapshot)
        : catalog_(catalog), store_(store), sink_(sink), depth_(depth), parent_(parent),
          stats_(stats), budget_(budget), trail_(trail), replay_(replay), cabins_(cabins),
          snapshot_(snapshot != nullptr ? *snapshot : kSeesEverything) {}

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
        // Charged to the sub-chain's own first step. A sub-chain is not a
        // step and has no step_id of its own, and attributing its runs to
        // the *outer* step that triggered it would double-count the work
        // its steps then report themselves.
        if (!sub.steps.empty()) {
            StepStats& sub_stats = stats_.For(sub.steps[0].step_id);
            ++sub_stats.sub_chain_runs;
            // The shape that makes a statement quadratic: a sub-chain
            // whose first step walks its relation, evaluated once per
            // outer row. Counted rather than refused - it is legitimate
            // over small relations, and the budget is what bounds it over
            // large ones.
            if (sub.steps[0].kind == AccessKind::kScan) ++sub_stats.correlated_scans;
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

        // The collector is shared with the sub-chain, not rebuilt: step
        // ids are global across the whole statement (step_chain.hpp), so a
        // nested step's entries belong in the same trail and are already
        // distinguishable by their step_id.
        // The snapshot is shared with the sub-chain for the same reason the
        // collector is: a subquery is part of one statement, and a nested
        // step reading a relation through a different view than its outer
        // step would make one statement see two databases.
        ChainRunner inner(catalog_, store_, collect, depth_ + 1, &outer, stats_, budget_,
                          trail_, replay_, cabins_, &snapshot_);
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

        ++stats_.For(step.step_id).relation_opens;
        if (step.kind == AccessKind::kLookup || step.kind == AccessKind::kProbe) {
            return RunPointStep(steps, index, step, access);
        }
        if (step.kind == AccessKind::kCabinProbe) {
            return RunCabinStep(steps, index, step, access);
        }
        // A kFilterScan walks exactly as a kScan does - the kind is a
        // statistics distinction, not an execution one, and there is
        // deliberately no branch for it here. If one ever appears, the
        // "same rows either way" tests are what should stop it.
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
            if (memo_valid_ && memo_step_ == step.step_id && memo_key_ == key.value()) {
                ++stats_.For(step.step_id).probe_memo_hits;
                NoteFetch();
                auto bytes = store_.GetForRead(memo_page_);
                if (bytes.ok()) {
                    heap::PageView page(bytes.value());
                    return AcceptTupleAt(steps, index, step, access, memo_page_, page,
                                         memo_slot_);
                }
                // The page went away. Fall through to the descent rather
                // than fail: the memo is an accelerator, never an oracle.
                memo_valid_ = false;
            }

            if (Status s = TryReplay(steps, index, step, access, key.value()); !s.ok()) {
                return s;
            }
            if (replayed_) return Status::OK();

            NoteFetch();
            auto found = btree::BtreeLookup(store_, access.desc_page_id, key.value());
            if (found.ok()) {
                memo_valid_ = true;
                memo_step_ = step.step_id;
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
                return AcceptTupleAt(steps, index, step, access, found.value().page_id, leaf,
                                     found.value().slot);
            }
            if (found.status().code() == StatusCode::kNotFound) return Status::OK();
            return found.status();
        }

        // Heap: no index to descend. The walk below is the authoritative
        // path, and the residual carries the key.
        //
        // **This is the case spec section 7 calls large**: a trail turns a
        // full chain scan into one read, because a heap relation has no pk
        // index for a descent to use in the first place.
        if (Status s = TryReplay(steps, index, step, access, key.value()); !s.ok()) return s;
        if (replayed_) return Status::OK();

        return RunWalkStep(steps, index, step, access);
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
    Status TryReplay(const std::vector<Step>& steps, std::size_t index, const Step& step,
                     const catalog::TableAccess& access, std::uint64_t key) {
        replayed_ = false;
        if (replay_ == nullptr) return Status::OK();

        // Rule 0 is **this lookup**. The index is keyed on (step_id, pk) and
        // `key` was just re-derived from the current outer row, so an entry
        // is only found by matching it. There is no separate check to
        // forget, which is what the P13 amendment asks for.
        const TrailLocation* at = replay_->Find(step.step_id, key);
        if (at == nullptr) return Status::OK();

        StepStats& step_stats = stats_.For(step.step_id);

        NoteFetch();
        // Spec section 2 rules 1-2, through the one verifier both this and
        // Cabin's location hints go through (exec/tuple_verify.hpp). The
        // page gone, the slot retired, and a different tuple at the target
        // are all the same answer here - the caller descends - which is
        // deliberate: a caller that could tell them apart would be tempted
        // to treat one of them as authoritative.
        VerifiedTuple verified = VerifyTupleAt(store_, at->page_id, at->slot, key);
        if (!verified.ok()) {
            ++step_stats.trail_misses;
            return Status::OK();
        }

        // Validated. From here it is the authoritative path's own code on
        // the authoritative path's own bytes - decode, residual,
        // sub-chains, descend - so rule 3's "apply visibility exactly as
        // the authoritative path would" is not implemented here so much as
        // inherited.
        ++step_stats.trail_replays;
        replayed_ = true;
        return AcceptTupleAt(steps, index, step, access, at->page_id, *verified.page, at->slot);
    }

    // ---- Cabin (docs/feat-cabin.md §4) -----------------------------------
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
    Status RunCabinStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                        const catalog::TableAccess& access) {
        // Nothing configured, or a value that must never be observed (NULL,
        // an unbound `$param`). Both take the walk, which is what the step
        // would have compiled to had the Cabin not existed.
        std::optional<stats::CabinKey> key;
        if (cabins_ != nullptr && step.cabin.has_value()) {
            key = stats::MakeCabinKey(step.cabin->cabin_id, step.cabin->value);
        }
        if (!key.has_value()) return RunWalkStep(steps, index, step, access);

        if (std::vector<stats::CabinEntry>* entries = cabins_->Find(*key); entries != nullptr) {
            cabins_->NoteHit(step.cabin->cabin_id);
            ++stats_.For(step.step_id).cabin_hits;
            return ServeFromCabin(steps, index, step, access, *key, *entries);
        }

        cabins_->NoteMiss(step.cabin->cabin_id);
        ++stats_.For(step.step_id).cabin_misses;

        // The miss path is why the first query for a value costs nothing
        // extra: it was going to walk anyway, and recording is a side
        // effect of the walk (§4). `n = 2` - the first miss only counts.
        const bool record =
            cabins_->WouldRecord(cabins_->Observe(*key), step.cabin->declared);
        if (!record) return RunWalkStep(steps, index, step, access);

        Recording recording;
        recording.step_id = step.step_id;
        recording.rel_slot = static_cast<std::uint16_t>(index);
        recording.col_pos = step.cabin->col_pos;
        recording.value = &step.cabin->value;
        recording_ = &recording;
        Status walked = RunWalkStep(steps, index, step, access);
        recording_ = nullptr;

        if (!walked.ok()) return walked;
        // **Only a completed walk may be committed.** A walk that a sink
        // stopped, or that the budget ended, collected the rows it reached
        // and not the rows it did not - and a set marked observed while
        // missing qualifying pks is precisely the break C1 forbids. This is
        // the one place that check can be made, because it is the only place
        // that knows whether the walk finished.
        if (stopped_) return Status::OK();

        // Sorted here, once, rather than on every hit: entries served in
        // page order batch same-page tuples into one fetch (§3), and a set
        // is written far less often than it is read. Appends by the write
        // hook land at the end and leave it *nearly* sorted, which costs a
        // little locality and no correctness.
        std::sort(recording.entries.begin(), recording.entries.end(),
                  [](const stats::CabinEntry& a, const stats::CabinEntry& b) {
                      return a.page_id < b.page_id || (a.page_id == b.page_id && a.slot < b.slot);
                  });
        if (cabins_->Commit(*key, std::move(recording.entries))) {
            ++stats_.For(step.step_id).cabin_recordings;
        }
        return Status::OK();
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
    Status ServeFromCabin(const std::vector<Step>& steps, std::size_t index, const Step& step,
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
                VerifiedTuple verified =
                    VerifyTupleAt(store_, entry.page_id, entry.slot, entry.pk);
                if (verified.ok()) {
                    ++step_stats.cabin_hint_hits;
                    serve_scratch_.push_back(Located{entry.page_id, entry.slot});
                    continue;
                }
                ++step_stats.cabin_hint_misses;
            }

            if (!is_btree) {
                // No descent to resolve the pk with. Abandon the Cabin for
                // this step and take the authoritative path, re-recording
                // the value's set from it - one bulk heal rather than a
                // chain scan per entry. Nothing has been emitted yet, so
                // this cannot duplicate a row.
                return FallBackAndReRecord(steps, index, step, access, key);
            }

            NoteFetch();
            auto found = btree::BtreeLookup(store_, access.desc_page_id, entry.pk);
            if (!found.ok()) {
                if (found.status().code() == StatusCode::kNotFound) {
                    // Dangling: the pk is in no clustered tree. By K1 it can
                    // never resurface under a new tuple, so this entry is
                    // dead forever - a **skip**, never an error (§5).
                    continue;
                }
                return found.status();
            }

            // Healed in place. The hint was wrong and the pk was right,
            // which is C6's whole shape: authority in the pk, speed in the
            // location, and the location repaired from the authority.
            entry.page_id = found.value().page_id;
            entry.slot = found.value().slot;
            entry.page_epoch = 0;
            entry.flags |= stats::kCabinHintValid;
            serve_scratch_.push_back(Located{entry.page_id, entry.slot});
        }

        // Phase 2. The page is fetched per entry rather than carried out of
        // phase 1: `AcceptTupleAt` descends into the next step, and anything
        // below it may fetch, so a page view held across entries is exactly
        // the span R1 forbids.
        for (const Located& at : serve_scratch_) {
            if (stopped_) break;
            NoteFetch();
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
            heap::PageView page(bytes.value());
            if (Status s = AcceptTupleAt(steps, index, step, access, at.page_id, page, at.slot);
                !s.ok()) {
                return s;
            }
        }
        step_stats.cabin_entries_served += serve_scratch_.size();
        return Status::OK();
    }

    // The heap hint-failure path: un-observe, walk, and re-record.
    //
    // Un-observing first is what keeps this honest. The walk below is the
    // authoritative path, so the rows are right either way - but the set
    // that produced a bad hint has just been shown to disagree with storage,
    // and §1's corollary makes dropping it always legal.
    Status FallBackAndReRecord(const std::vector<Step>& steps, std::size_t index,
                               const Step& step, const catalog::TableAccess& access,
                               const stats::CabinKey& key) {
        cabins_->Unobserve(key);

        Recording recording;
        recording.step_id = step.step_id;
        recording.rel_slot = static_cast<std::uint16_t>(index);
        recording.col_pos = step.cabin->col_pos;
        recording.value = &step.cabin->value;
        recording_ = &recording;
        Status walked = RunWalkStep(steps, index, step, access);
        recording_ = nullptr;

        if (!walked.ok()) return walked;
        if (stopped_) return Status::OK();  // a partial walk records nothing

        std::sort(recording.entries.begin(), recording.entries.end(),
                  [](const stats::CabinEntry& a, const stats::CabinEntry& b) {
                      return a.page_id < b.page_id || (a.page_id == b.page_id && a.slot < b.slot);
                  });
        if (cabins_->Commit(key, std::move(recording.entries))) {
            ++stats_.For(step.step_id).cabin_recordings;
        }
        return Status::OK();
    }

    Status RunWalkStep(const std::vector<Step>& steps, std::size_t index, const Step& step,
                       const catalog::TableAccess& access) {
        Status inner = Status::OK();

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
        auto visitor = [&](PageId page_id, heap::PageView& page,
                           std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (stopped_) return storage::VisitControl::kStop;

            // Checked per slot rather than per page because the walk has no
            // per-page hook; it costs one compare against a header field
            // already in cache, and it fires on the first slot of the first
            // page past the range.
            if (pruning && page.min_key() > range_high) {
                ++stats_.For(step.step_id).range_pages_pruned;
                return storage::VisitControl::kStop;
            }

            auto accepted = AcceptTupleAt(steps, index, step, access, page_id, page, slot);
            if (!accepted.ok()) {
                inner = accepted;
                return accepted;
            }
            return stopped_ ? storage::VisitControl::kStop : storage::VisitControl::kContinue;
        };

        NoteFetch();

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
        if (pruning && access.clustered_type == catalog::ClusteredType::kBtree) {
            auto first = btree::BtreeSeekLeaf(store_, access.desc_page_id, step.range->low);
            if (first.ok()) {
                Status seeked =
                    btree::BtreeVisitFrom(store_, first.value(), storage::PageAccess::kRead,
                                          visitor);
                if (!inner.ok()) return inner;
                return seeked;
            }
            // A descent that failed is a reason to walk, not to fail: the
            // walk is the authoritative path and reaches the same rows.
        }

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
                         const catalog::TableAccess& access, PageId page_id,
                         heap::PageView& page, std::uint16_t slot) {
        bool decoded = false;
        // Filled by the decode, drained after the span is released. A
        // spilled value lives in the var-heap and fetching it is a page
        // fetch, so it is exactly the thing that must not happen while the
        // span below is live (row_codec.hpp). Reused across rows so a scan
        // that never spills allocates nothing extra.
        spills_.clear();

        // ---- MVCC, phase 1 (docs/txn.md section 4.3) --------------------
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
        {
            PageSpanGuard span;
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
        // **Not while recording a Cabin.** The write-hook block below reads
        // the cabin's key column and the pk before the residual runs, and a
        // slot this row did not decode still holds the *previous* row's
        // value - which would record an entry for a row that does not carry
        // the value. A miss walk is already committed to reading everything,
        // so it decodes everything.
        const bool recording_here =
            recording_ != nullptr && recording_->step_id == step.step_id;
        const bool partial = step.filter_columns != Step::kAllColumns && !recording_here;
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
        // Charged where the tuple was actually decoded, which is the unit
        // that tracks work: a page fetch amortizes over its tuples, but
        // every tuple is decoded and filtered on its own.
        if (Status s = budget_.ChargeRow(); !s.ok()) return s;

        // ---- Cabin recording (docs/feat-cabin.md §4's miss path) ---------
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
        if (recording_ != nullptr && recording_->step_id == step.step_id) {
            const ColumnRef ref{0, recording_->rel_slot, recording_->col_pos};
            if (frame_.CanResolve(ref) &&
                CompareValues(/*type_val=*/0, frame_.Get(ref), *recording_->value,
                              parser::CompareOp::kEq)) {
                // Column 0 is the Keystone pk, already decoded into the
                // frame by this step - so the id costs a read, never a
                // lookup. Same source the trail collector uses, deliberately.
                const parser::AstValue& pk =
                    frame_.Get(ColumnRef{0, static_cast<std::uint16_t>(index), 0});
                stats::CabinEntry entry;
                entry.pk = pk.int_val < 0 ? 0 : static_cast<std::uint64_t>(pk.int_val);
                entry.page_id = page_id;
                // Written 0: there is no page epoch to read (cabin_store.hpp).
                entry.page_epoch = 0;
                entry.slot = slot;
                entry.flags = stats::kCabinHintValid;
                recording_->entries.push_back(entry);
            }
        }

        auto matched = EvaluateAll(schemas_, step.residual, frame_);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return Status::OK();

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
            touched.slot = slot;
            touched.step_id = static_cast<std::uint16_t>(step.step_id);
            trail_->Add(touched);
        }

        return RunStep(steps, index + 1);
    }

    catalog::Catalog& catalog_;
    storage::PageStore& store_;

    // Scratch for AcceptTupleAt()'s decode, reused across rows so a scan
    // that spills nothing allocates nothing for the possibility.
    std::vector<PendingSpill> spills_;

    // The read view every tuple is filtered through, and the log an
    // invisible writer is stepped back through (docs/txn.md section 4).
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

    // ---- Cabin (docs/feat-cabin.md) --------------------------------------
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
        std::uint16_t rel_slot = 0;
        std::uint16_t col_pos = 0;
        const parser::AstValue* value = nullptr;
        std::vector<stats::CabinEntry> entries;
    };
    Recording* recording_ = nullptr;

    // Phase 1's output (see ServeFromCabin): the verified locations, held
    // until every entry has been resolved so that the heap fallback can
    // still abandon without having emitted a row.
    struct Located {
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
    };
    std::vector<Located> serve_scratch_;
    std::unordered_set<std::uint64_t> seen_pks_;

    std::vector<Bound> bound_;
    std::vector<const catalog::Schema*> schemas_;
    ChainFrame frame_;
    bool stopped_ = false;
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
    trail_replays += other.trail_replays;
    trail_misses += other.trail_misses;
    range_pages_pruned += other.range_pages_pruned;
    cabin_hits += other.cabin_hits;
    cabin_misses += other.cabin_misses;
    cabin_entries_served += other.cabin_entries_served;
    cabin_hint_hits += other.cabin_hint_hits;
    cabin_hint_misses += other.cabin_hint_misses;
    cabin_recordings += other.cabin_recordings;
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

void InstallSuspendAudit() noexcept {
    // The executor's answer to "is it safe to be suspended right now?".
    //
    // R1 already forbids a page *fetch* under a live span, because nothing
    // pins the frame the span points into. Suspending under one is strictly
    // worse: the span stays live for arbitrary wall time, across every other
    // statement that runs on this core in between - so a store that ever
    // evicts turns it from a latent bug into a routine one.
    //
    // Installed here rather than checked here: `sched/` sits below this
    // layer and must not know what a page is (coro.hpp's SuspendAuditFn).
    sched::SetSuspendAudit([]() -> std::string_view {
        if (g_live_spans > 0) {
            return "a coroutine suspended while holding a page span (parser-v2.md I15 R1)";
        }
        return {};
    });
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
    Budget spend(budget.limit());
    // A runner with no sink of its own: it exists only to lend its
    // sub-chain evaluation, which builds its own sink per sub-chain.
    static const RowSink kUnused = nullptr;
    // No collector: this path evaluates one already-located row's
    // conjuncts for UPDATE, which is not a chain execution and has no trail
    // of its own to contribute to.
    ChainRunner runner(catalog, store, kUnused, /*depth=*/0, /*parent=*/nullptr, counters, spend,
                       /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr, snapshot);

    for (const SubChain& sub : step.sub_chains) {
        auto value = runner.EvaluateSubChain(sub, frame);
        if (!value.ok()) return value.status();
        // The same collapse point every other caller uses.
        if (!Collapse(value.value())) return false;
    }
    return true;
}

Status Execute(catalog::Catalog& catalog, storage::PageStore& store, const StepChain& chain,
               const RowSink& sink, ExecStats* stats, const Budget& budget,
               TrailCollector* trail, const TrailReplay* replay, stats::CabinStore* cabins,
               const txn::Snapshot* snapshot) {
    if (chain.steps.empty()) {
        return Status::InvalidArgument("a step chain with no steps reads nothing");
    }

    ExecStats local;
    ExecStats& counters = stats != nullptr ? *stats : local;

    // Sized once, up front, for the highest step_id the chain contains.
    // Not an optimization: the runner holds a `StepStats&` across nested
    // calls that also touch the stats, and a vector that grew underneath
    // one would leave it dangling. Pre-sizing makes every reference stable
    // for the whole execution, which is a property worth having by
    // construction rather than by reviewing every call site.
    counters.For(MaxStepId(chain));

    // One mutable budget for this statement, seeded from the caller's
    // limit. Taking the parameter by const reference and copying here is
    // what keeps a Budget reusable across statements without a caller
    // having to remember to reset it.
    Budget spend(budget.limit());

    ChainRunner runner(catalog, store, sink, /*depth=*/0, /*parent=*/nullptr, counters, spend,
                       trail, replay, cabins, snapshot);

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
