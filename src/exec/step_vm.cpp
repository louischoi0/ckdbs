#include "kds/exec/step_vm.hpp"

#include <algorithm>

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

// One chain's execution state. Recreated per sub-chain, which is what
// makes the frame stack a stack.
class ChainRunner {
public:
    ChainRunner(catalog::Catalog& catalog, storage::PageStore& store, const RowSink& sink,
                std::uint32_t depth, const ChainFrame* parent, ExecStats& stats, Budget& budget,
                TrailCollector* trail, const TrailReplay* replay)
        : catalog_(catalog), store_(store), sink_(sink), depth_(depth), parent_(parent),
          stats_(stats), budget_(budget), trail_(trail), replay_(replay) {}

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
        ChainRunner inner(catalog_, store_, collect, depth_ + 1, &outer, stats_, budget_,
                          trail_, replay_);
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
        auto bytes = store_.GetForRead(at->page_id);
        if (!bytes.ok()) {
            // The page is gone. A miss, not an error: the trail is advisory
            // and this is exactly the fall-through it promises.
            ++step_stats.trail_misses;
            return Status::OK();
        }

        heap::PageView page(bytes.value());
        auto tuple = page.ReadTuple(at->slot);
        if (!tuple.ok()) {
            // Retired or out of range - the slot no longer holds anything.
            ++step_stats.trail_misses;
            return Status::OK();
        }

        // Spec section 2 rule 1, the storage half: the tuple *actually
        // there* must be the one the entry named. This is what makes a
        // stale trail a miss rather than somebody else's row, and it is the
        // check the corrupted-trail contract test exists to prove
        // load-bearing.
        auto found_pk = RowKeystoneId(tuple.value().payload);
        if (!found_pk.ok() || found_pk.value() != key) {
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
        return AcceptTupleAt(steps, index, step, access, at->page_id, page, at->slot);
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
            stats_.For(step.step_id).spill_fetches += spills_.size();
            if (Status s = ResolveSpills(store_, spills_,
                                          frame_.SlotsFor(static_cast<std::uint16_t>(index)));
                !s.ok()) {
                return s;
            }
        }

        StepStats& step_stats = stats_.For(step.step_id);
        ++step_stats.rows_examined;
        // Charged where the tuple was actually decoded, which is the unit
        // that tracks work: a page fetch amortizes over its tuples, but
        // every tuple is decoded and filtered on its own.
        if (Status s = budget_.ChargeRow(); !s.ok()) return s;

        auto matched = EvaluateAll(schemas_, step.residual, frame_);
        if (!matched.ok()) return matched.status();
        if (!matched.value()) return Status::OK();

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
    counters.For(MaxStepId(step));  // see Execute() for why this is pre-sized
    Budget spend(budget.limit());
    // A runner with no sink of its own: it exists only to lend its
    // sub-chain evaluation, which builds its own sink per sub-chain.
    static const RowSink kUnused = nullptr;
    // No collector: this path evaluates one already-located row's
    // conjuncts for UPDATE, which is not a chain execution and has no trail
    // of its own to contribute to.
    ChainRunner runner(catalog, store, kUnused, /*depth=*/0, /*parent=*/nullptr, counters, spend,
                       /*trail=*/nullptr, /*replay=*/nullptr);

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
               TrailCollector* trail, const TrailReplay* replay) {
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
                       trail, replay);

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
