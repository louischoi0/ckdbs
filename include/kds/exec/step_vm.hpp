#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/chain_frame.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/exec/trail_collector.hpp"
#include "kds/exec/trail_replay.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"
#include "kds/txn/visibility.hpp"

// Executes a compiled StepChain (docs/parser-v2-workplan.md V17).
//
// ---- What this is not --------------------------------------------------
//
// There is **no shape analysis here**. The executor does not look at a
// statement and decide anything: which relation is read in which order,
// which step descends and which walks, where each predicate is evaluated -
// all of it was settled by the compiler and is sitting in the chain. This
// file is a loop that does what it is told. That is deliberate and
// grep-checkable: a second place that reasons about shape is a second
// answer to "what does this statement do", and the first one is the one a
// Waystone trail was recorded against.
//
// ---- The three rules for nested access (spec I15) -----------------------
//
// R1 - **decode before descending.** A relation walk hands out a span into
//      a live page frame. Descending into the next step fetches another
//      page, and nothing pins the first: the span may be invalidated
//      underneath. So a step decodes its row into the chain frame and
//      *releases the span* before the nested loop runs. Enforced by
//      PageSpanGuard below rather than left to discipline - it is safe
//      today only because nothing evicts, which is exactly the kind of
//      accident that stops being safe silently.
//
// R2 - **nested steps are read-only.** Every page a nested step touches is
//      fetched for read. Structural: the walk is opened with
//      PageAccess::kRead and there is no write path in this file at all.
//
// R3 - **recursion is bounded at both ends.** The parser caps nesting and
//      so does the compiler; the executor carries its own depth counter,
//      because a bound only one layer enforces is not a bound.

namespace kds::exec {

// Called once per output row, with the frame holding every bound relation.
// Returning kStop ends the statement successfully - which is what `LIMIT`
// and a top-level `EXISTS` will use, and which V03 made expressible.
using RowSink = std::function<StatusOr<storage::VisitControl>(const ChainFrame&)>;

// What one step did.
//
// `relation_opens` counts the times the step began reading its relation -
// a walk started, a descent made, or a trail replayed, since all three read
// a page. It is how "a false uncorrelated EXISTS opens zero pages" is
// checkable: the claim is about work not done, and work not done leaves no
// other trace. Pair it with `trail_replays` to tell a replay from a
// descent; `relation_opens` alone cannot, and deliberately does not try -
// it answers "did this step touch its relation at all".
//
// `rows_examined` and `rows_matched` are the pair that makes **selectivity
// visible**: how many tuples the step decoded, and how many of those
// survived its residual list. A statement that is slow because a step
// reads a hundred rows to keep one says so here, and nowhere else - the
// totals alone cannot, because they cannot say *which* step.
struct StepStats {
    std::uint64_t relation_opens = 0;
    std::uint64_t rows_examined = 0;   // tuples decoded and filtered
    std::uint64_t rows_matched = 0;    // of those, the ones passing the residual
    std::uint64_t sub_chain_runs = 0;  // sub-chain evaluations, hoisted or per row

    // V19's meters.
    //
    // `probe_memo_hits` counts descents skipped because the step's key
    // repeated. `correlated_scans` counts sub-chain evaluations whose
    // first step is a Scan - the shape that makes a statement quadratic,
    // and the one worth being able to point at when a statement is
    // refused for its budget.
    std::uint64_t probe_memo_hits = 0;
    std::uint64_t correlated_scans = 0;

    // Var-heap values fetched to complete a row (row_codec.hpp's
    // ResolveSpills). Counted per step because it is a property of the
    // *relation* a step reads - a wide-string table pays it and a numeric
    // one never does - and because it is the one page fetch on the decode
    // path that a row count does not imply.
    std::uint64_t spill_fetches = 0;

    // Waystone replay (workplan P11). `trail_replays` counts descents this
    // step skipped because a recorded location validated; `trail_misses`
    // counts consultations that found an entry and fell through anyway -
    // the page gone, the slot retired, or a different tuple at the target.
    //
    // **`trail_replays` is how spec §11-4 is checked rather than asserted.**
    // A trail may replace a lookup and never a search (invariant 9), so a
    // step whose predicate is not a pk equality must report zero here
    // forever. That is a property nothing else makes visible: a search that
    // was wrongly skipped returns *plausible* rows, so the only evidence is
    // the work not done.
    std::uint64_t trail_replays = 0;
    std::uint64_t trail_misses = 0;

    // A kRange walk that stopped early because a page's `min_key` proved
    // the rest of the relation is past the high bound. 0 or 1 per
    // execution - it counts the *stop*, not the pages skipped, because the
    // pages after it are never fetched and so were never counted.
    //
    // The honest reading: non-zero means the range ended before the
    // relation did. Zero means either the range reached the end or nothing
    // was pruned, and `rows_examined` is what tells those apart.
    std::uint64_t range_pages_pruned = 0;

    // Cabin (docs/feat-cabin.md §7's "ANALYZE narrates all three").
    //
    // `cabin_hits` counts probes served from an observed value's entry set -
    // authoritatively, so the relation was **not** walked; `cabin_misses`
    // counts probes for a value nothing has observed, which walked. Those
    // two are the layer's whole story, and the pair is what makes "the
    // second execution stops scanning" checkable rather than asserted:
    // a hit with `rows_examined` still equal to the relation's size would
    // mean the set was not actually serving.
    //
    // `cabin_hint_hits` / `cabin_hint_misses` split the C6 advisory tier out
    // of that: how often an entry's location was still right, and how often
    // it had to be resolved through the pk instead. A rising miss count is a
    // relation whose tuples are moving, which today should be impossible.
    //
    // `cabin_recordings` counts values that *became* observed during this
    // step - the miss path paying for the next execution's hit.
    std::uint64_t cabin_hits = 0;
    std::uint64_t cabin_misses = 0;
    std::uint64_t cabin_entries_served = 0;
    std::uint64_t cabin_hint_hits = 0;
    std::uint64_t cabin_hint_misses = 0;
    std::uint64_t cabin_recordings = 0;

    StepStats& operator+=(const StepStats& other) noexcept;
};

// What an execution did, per step.
//
// Indexed by `Step::step_id`, which is global across the whole statement:
// the outer chain and every sub-chain share one counter (step_chain.hpp),
// so a step_id identifies a step without any parent linkage and this
// vector needs none either.
//
// The statement-wide totals that used to be these fields are still
// available through Total(), which is what the existing callers want; what
// they could not answer before is *which* step spent the work.
struct ExecStats {
    std::vector<StepStats> steps;

    // The step's counters, growing the vector as needed. Called on the hot
    // path, so it does not check anything a compiled chain guarantees.
    StepStats& For(std::uint32_t step_id);

    // Summed across every step - the old statement-wide view.
    StepStats Total() const noexcept;
};

// Runs `chain` and calls `sink` for each row that satisfies it.
//
// Rows arrive in the order the chain produces them: steps[0] in relation
// order, and for each of its rows, steps[1] in relation order, and so on.
// That is the written order of the statement, which spec §1 makes a client
// contract.
//
// Fails with `CardinalityViolation` when a scalar subquery returns more
// than one row - a runtime verdict, since parse time cannot prove
// cardinality in general (spec §2) - and with `ResourceExhausted` when the
// statement spends `budget` (exec/budget.hpp).
//
// `trail`, when given, collects the tuples that trail-replayable steps
// accepted, in execution order (exec/trail_collector.hpp, workplan P09).
// Null means nothing is recording, which is a valid production
// configuration and costs one null check per accepted row.
//
// **Passing a collector cannot change what this returns.** It is written
// to and never read here; no branch below depends on its contents, and a
// full one drops silently rather than failing the statement. That is what
// makes "results are identical with recording on and off" a property of
// the structure rather than of the test data.
//
// `replay`, when given, is the trail a previous execution of this instance
// recorded (exec/trail_replay.hpp, workplan P11/P13). **Passing it cannot
// change what this returns either**, and for a sharper reason than the
// collector's: a trail supplies only a *location*, which is then read and
// filtered by the same code a descent's location would have been. Every
// entry is validated first, and any failure falls through to the descent
// for that step alone. Deleting every trail in the database changes
// latency and nothing else (invariant 8).
// `cabins`, when given, is the core-local Cabin store (stats/cabin_store.hpp,
// docs/feat-cabin.md). **Passing it cannot change what this returns either**,
// and the argument is a third variation on the same theme: a Cabin supplies a
// set of *locations*, each of which is then read and filtered by the code a
// walk would have fed - visibility, the residual (which still carries the
// cabin's own equality), sub-chains, the next step. What it changes is which
// rows are *looked at*: a served value reads its entries instead of the
// relation. Deleting every Cabin in the database costs latency and nothing
// else, one value at a time (spec §1's corollary).
// `snapshot` is the read view every tuple is filtered through, plus the
// undo log an invisible writer is stepped back through (docs/txn.md §4).
// **It is applied at exactly one place** - `AcceptTupleAt` - which every
// access kind funnels into: the chain walk, the btree descent, the probe
// memo, a Waystone replay and a Cabin resolve. That is what makes Waystone
// §3.1 rule 2 ("MVCC visibility is applied exactly as it would be on the
// authoritative path") true by construction rather than by discipline, and
// it is a stronger statement than txn.md §4.4's, which was written when
// two statement handlers owned the read path and the step VM did not exist.
//
// The default snapshot admits every writer and needs no undo log, which is
// precisely what the engine did before MVCC: every row carried
// kBootstrapXid and every row was visible. So a caller that passes nothing
// gets byte-identical results to the pre-transactional executor.
Status Execute(catalog::Catalog& catalog, storage::PageStore& store, const StepChain& chain,
               const RowSink& sink, ExecStats* stats = nullptr,
               const Budget& budget = Budget(), TrailCollector* trail = nullptr,
               const TrailReplay* replay = nullptr, stats::CabinStore* cabins = nullptr,
               const txn::Snapshot* snapshot = nullptr);

// Evaluates one step's whole conjunct list - ordinary predicates *and*
// sub-chains - against a frame already holding that step's row.
//
// For statements that walk a relation themselves rather than through
// `Execute`: UPDATE today, DELETE when it lands. They need it so their
// WHERE means exactly what a SELECT's does, down to `NOT IN`'s tri-state
// collapse. A second evaluator would be a second answer to "does this row
// qualify", and the two would drift on the first NULL.
// `snapshot` is threaded through for the sub-chains: a correlated subquery
// inside an UPDATE's WHERE reads relations, and it must read them through
// the same view the UPDATE does.
StatusOr<bool> EvaluateConjuncts(catalog::Catalog& catalog, storage::PageStore& store,
                                 const std::vector<const catalog::Schema*>& schemas,
                                 const Step& step, const ChainFrame& frame,
                                 ExecStats* stats = nullptr, const Budget& budget = Budget(),
                                 const txn::Snapshot* snapshot = nullptr);

// Whether a debug-build page-span guard has fired during this process.
// Exposed for the test that deliberately violates R1; production code has
// no reason to read it.
bool PageSpanGuardTripped() noexcept;
void ResetPageSpanGuard() noexcept;

// Page spans live right now. Zero everywhere outside a step's decode
// window, which is what makes it a usable suspension-safety predicate.
int LivePageSpans() noexcept;

// Installs the executor's suspension audit into `sched` (coro.hpp's
// SuspendAuditFn), so a coroutine that suspends while holding a page span
// is detected rather than merely forbidden in prose.
//
// Call once per core, before running statements. It is idempotent and
// core-local, like the guard counters it reads.
void InstallSuspendAudit() noexcept;

}  // namespace kds::exec
