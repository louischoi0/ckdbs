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
#include "kds/sched/coro.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"
#include "kds/txn/visibility.hpp"

// Executes a compiled StepChain (docs/inflight/in-progress/parser-v2-workplan.md V17).
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
    // driving step **actually walked** its relation - the shape that makes
    // a statement quadratic, and the one worth being able to point at when
    // a statement is refused for its budget. Execution-level since
    // 2026-08-19: the compile-time kind test it replaced missed a
    // kFilterScan driver outright and counted a cabined driver never -
    // including on the misses that walked - while a served probe, which
    // walks nothing, now honestly counts as nothing.
    std::uint64_t probe_memo_hits = 0;
    std::uint64_t correlated_scans = 0;

    // Var-heap values fetched to complete a row (row_codec.hpp's
    // ResolveSpills). Counted per step because it is a property of the
    // *relation* a step reads - a wide-string table pays it and a numeric
    // one never does - and because it is the one page fetch on the decode
    // path that a row count does not imply.
    std::uint64_t spill_fetches = 0;

    // Pages this step demonstrably read (physical-optimizer.md §II.2
    // S2 - the cost-benefit model's currency is page accesses, and this is
    // its collector). Counted exactly where the count is exact: a walk
    // counts each chain page once as the visitor crosses onto it, and every
    // keyed access - a descent, a replay verify, a memo re-read, an index
    // seek or resolve - counts **one** per access rather than one per
    // b-tree level, because the VM sees the descent as a call, not a path.
    // An undercount on descents is the honest direction: the signal exists
    // to price *scans*, where it is exact.
    std::uint64_t pages_fetched = 0;

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

    // Secondary index (docs/spec/index.md §§1, 7).
    //
    // `index_entries_scanned` counts entries the walk read between its two
    // bounds; `index_rows_resolved` counts the pks it then descended for.
    // The gap between them is what the layer actually saved, and it has two
    // sources the third counter separates: a duplicate pk (maintenance is
    // append-only, so a key round trip leaves two entries naming one row)
    // and `index_entries_filtered`, a row a **covered** column already
    // disqualified.
    //
    // That last one is the only number that prices covering, and it prices
    // it honestly: a covered column saves a base *descent*, never a base
    // read, because visibility still requires the tuple (spec §7). So
    // `index_entries_filtered` counts descents avoided and nothing else -
    // if it is zero, a COVERING clause bought exactly the write cost it
    // added.
    std::uint64_t index_entries_scanned = 0;
    std::uint64_t index_entries_filtered = 0;
    std::uint64_t index_rows_resolved = 0;

    // Cabin (docs/spec/cabin.md §7's "ANALYZE narrates all three").
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

    // The statement-local inner build (join-inner-build.md §4's
    // honesty clause; workplan JB3/JB4 collect, JB7 prints).
    //
    // `inner_builds` counts maps this step *published* - a completed first
    // walk on an annotated step, so 0 or 1 per step. Per *statement* it is
    // one per annotated step, which is more than one as soon as the join
    // is: a three-relation walked join annotates two steps and totals 2.
    // `build_rows`
    // counts entries bucketed while building: every walked row that passed
    // the step's non-correlated residual, matching or not, which is why it
    // can exceed `rows_matched` and is the number that pins "the map holds
    // the whole relation's match sets" (JB3's done-condition).
    // `build_probes` counts the outer rows served from a built map instead
    // of walking - spec §4's `probes=k` minus the first row, whose walk
    // was the build.
    std::uint64_t inner_builds = 0;
    std::uint64_t build_rows = 0;
    std::uint64_t build_probes = 0;

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
// docs/spec/cabin.md). **Passing it cannot change what this returns either**,
// and the argument is a third variation on the same theme: a Cabin supplies a
// set of *locations*, each of which is then read and filtered by the code a
// walk would have fed - visibility, the residual (which still carries the
// cabin's own equality), sub-chains, the next step. What it changes is which
// rows are *looked at*: a served value reads its entries instead of the
// relation. Deleting every Cabin in the database costs latency and nothing
// else, one value at a time (spec §1's corollary).
// `snapshot` is the read view every tuple is filtered through, plus the
// undo log an invisible writer is stepped back through (docs/spec/txn.md §4).
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
// `indexes` is the read-path switch (`indexes`, default on). Off makes an
// index step take the walk it would have taken had the index not existed -
// **the compiled chain is unchanged either way**, exactly as `cabins` leaves
// a `kCabinProbe` step compiled and steers only the branch inside it. That is
// what keeps the plan `f(shape, catalog)`, and it makes the A/B comparison a
// sharper test than a compiler switch would: identical plan, identical rows,
// different work.
//
// There is deliberately no switch for index **maintenance**. An index that
// stops being maintained is *wrong* rather than slow, and a config key that
// can produce a wrong answer is not a config key.
Status Execute(catalog::Catalog& catalog, storage::PageStore& store, const StepChain& chain,
               const RowSink& sink, ExecStats* stats = nullptr,
               const Budget& budget = Budget(), TrailCollector* trail = nullptr,
               const TrailReplay* replay = nullptr, stats::CabinStore* cabins = nullptr,
               const txn::Snapshot* snapshot = nullptr, bool indexes = true,
               const ChainFrame* parent = nullptr);

// The suspendable form (workplan-crosscore.md P4d-4a): identical
// semantics and identical arguments, returned as a `sched::Coro` the
// caller submits or polls. Execute() is this with no gate, driven to
// completion inline.
//
// `resume_gate`, when given, is consulted at every page boundary of the
// outermost walk - the one place a statement holds no pin and no span
// (P4d-3) - and the statement parks there until it answers true
// (WaitUntil semantics: one predicate call per poll, no resume while
// false). This is how a streaming producer applies backpressure: the
// gate answers false while a sealed batch waits for credit. The pointer
// must outlive the coroutine, which in practice means it lives in the
// same per-request state the batches do.
//
// Everything both entries share - sub-chains, nested steps - still runs
// through the synchronous gated driver: a wait beneath a walk visitor is
// a hard error, not a park, until P4d-4c moves that descent to the page
// boundary.
// `parent`, on either entry, is an outer frame the chain's references may
// reach through `up` links - the consuming pipeline stage's shape
// (workplan-crosscore.md P4d-4b fact 4): the caller fills a one-slot
// frame from an upstream batch row and runs a local step against it,
// exactly as a correlated sub-chain reads its outer row. The frame must
// outlive the execution; the chain runs at nesting depth 1, so the
// sub-chain depth guard still counts honestly.
sched::Coro ExecuteAsync(catalog::Catalog& catalog, storage::PageStore& store,
                         const StepChain& chain, const RowSink& sink, ExecStats* stats = nullptr,
                         const Budget& budget = Budget(), TrailCollector* trail = nullptr,
                         const TrailReplay* replay = nullptr, stats::CabinStore* cabins = nullptr,
                         const txn::Snapshot* snapshot = nullptr, bool indexes = true,
                         const std::function<bool()>* resume_gate = nullptr,
                         const ChainFrame* parent = nullptr);

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
// `store`, when given, extends the audit with the pin half of the same
// rule (workplan-crosscore.md P4d-3): with the PageRef migration landed,
// "suspending while holding a page" is mechanically
// `store->live_pins() != 0` at the suspension point, checked against the
// installing core's own store - pins are per-store and cores share none.
// Null keeps the span check alone, for callers with no store at hand.
//
// Call once per core, on the core's own thread, before running
// statements. It is idempotent and core-local, like the guard counters
// and the store pointer it reads.
void InstallSuspendAudit(const storage::PageStore* store = nullptr) noexcept;

// Clears the audit and its store pointer. Owed at core teardown: the
// audit is thread-local but the store it reads is not immortal, and a
// coroutine polled on this thread after the runtime died would otherwise
// dereference a freed store (debug builds only, but a crash in the
// detector is still a crash).
void UninstallSuspendAudit() noexcept;

}  // namespace kds::exec
