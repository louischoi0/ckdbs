#pragma once

#include <cstdint>
#include <functional>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/optimizer_signals.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/manager.hpp"

// The cabin optimizer's effectful half (docs/spec/physical-optimizer.md
// §II.3/§II.5, PO4/PO5/PO8, workplan PHY04): `Apply(ActionSet)` - and the
// per-tick `Tick()` that strings snapshot → Decide → Apply together for
// the expeditor's cadence task.
//
// Decide (PHY02, pure) and Execute (this, effectful) are separate phases
// by construction; the completion edges flow back through the controller's
// `NoteCreated`/`NoteBuildFailed`/`NoteDropped`, which is PO5's lifecycle
// executing "as single home-core steps" - everything here runs
// run-to-completion on the owning core, the AST06 builder's argument, so
// no write can interleave with a build's scan.
//
// ---- What each action means in this engine -------------------------------
//
// **CREATE** makes the catalog row (`Catalog::CreateCabin`, origin
// `kCabinOriginAuto` - the tag PHY03 pinned) and then runs the seeded
// build. For a first-time column the seed list is empty by construction -
// the column had no Cabin, so no probe was ever sighted under a CabinKey -
// and §II.5's "observation-based population" applies literally: the
// ordinary n=2 machinery populates it from traffic. A **policy refusal**
// (`NO CABIN`) parks the candidate in BUILDING rather than failing it
// back to CANDIDATE: a permanent "no" retried every confirm cycle would
// be churn wearing diligence's clothes.
//
// **EXTEND** is where the ring-routed build earns its keep: the seed is
// `SightedUnobservedOf(cabin_id)` - the values traffic probed that no set
// answers for - and **one complete scan through the scan ring** collects
// every seed's entry set at once, committing each only after the walk
// finished (the superset invariant's precondition, stated at both ends
// exactly as the VM's recording branch states it). An **empty** collected
// set commits too: an observed value with no rows is the authoritative
// zero-rows answer, the thing a Cabin exists to give. The scan judges
// rows by latest settled state (`txn::CheckVisibility`) and **aborts on a
// busy row** - an in-flight writer's row can neither be counted nor
// skipped without breaking completeness one way or the other (AST06's
// argument verbatim) - committing nothing; demand, if real, re-nominates.
//
// **HEAL** is the batch form of the read path's per-probe heal, through
// the same primitives ("reuses the read-path heal primitive"):
// `VerifyTupleAt` per hint, `BtreeLookup` + current-epoch stamp on a
// miss, a dangling pk erased outright (K1: dead forever, droppable on
// sight), and a heap relation's set un-observed - no descent to heal
// with, §5's rule.
//
// **DROP** is `Catalog::DropCabin` + `CabinStore::Forget` +
// `NoteDropped`, in that order: the compiler stops emitting probes the
// moment the row is gone, so the store-side forget is "late is fine".
//
// ---- PO8, the kill switch ------------------------------------------------
//
// `enabled` is consulted at every batch boundary: before the tick's
// snapshot, between actions, and **between a build's pages** - so an
// operator's OFF lands mid-build, the build discards cleanly (nothing was
// committed; commit happens only after a complete walk), and nothing
// destructive runs on the way down.
//
// PO4's ring mandate is structural: the build's page fetches go through
// `PageStore::OpenScanRing()`, so the optimizer cannot displace the
// foreground working set it exists to serve.

namespace kds::exec {

class CabinOptimizerExecutor {
public:
    // PO9's production counters (workplan PHY06), monotonic over the
    // process. **Applied**, not decided: the decision log already records
    // what Decide wanted, so counting there again would answer the same
    // question twice - these count what Execute actually did, and the gap
    // between the two surfaces (a decided CREATE beside zero `creates`) is
    // itself the diagnostic: the effectful half is refusing or deferring.
    struct Counters {
        std::uint64_t ticks = 0;            // enabled Tick() passes (cadence health)
        std::uint64_t creates = 0;          // CREATEs that built and reported
        std::uint64_t extends = 0;          // EXTENDs whose walk completed
        std::uint64_t heals = 0;            // HEAL passes run
        std::uint64_t drops = 0;            // DROPs applied
        std::uint64_t builds_deferred = 0;  // busy-row / kill-switch build aborts
        std::uint64_t build_failures = 0;   // failed builds (errors; deferrals excluded)
    };
    // `txn` may be null - the no-manager configuration reads everything,
    // exactly as the assertion builder's check view does.
    CabinOptimizerExecutor(catalog::Catalog& catalog, storage::PageStore& store,
                           stats::CabinStore& cabins, stats::CabinOptimizer& controller,
                           txn::TransactionManager* txn = nullptr) noexcept
        : catalog_(catalog), store_(store), cabins_(cabins), controller_(controller),
          txn_(txn) {}

    // One cadence tick: snapshot → Decide → Apply. A disabled switch skips
    // everything, including the snapshot - off means off.
    Status Tick(stats::OptimizerSignals& signals, const std::function<bool()>& enabled);

    // Applies `actions` in order, checking `enabled` between actions.
    Status Apply(const stats::ActionSet& actions, const std::function<bool()>& enabled);

    const Counters& counters() const noexcept { return counters_; }

private:
    Status ApplyCreate(const stats::ActionItem& action, const std::function<bool()>& enabled);
    Status ApplyExtend(const stats::ActionItem& action, const std::function<bool()>& enabled);
    Status ApplyHeal(const stats::ActionItem& action);
    Status ApplyDrop(const stats::ActionItem& action);

    // The seeded, ring-routed build. Commits nothing unless the walk
    // completed; `*aborted` reports a kill-switch or busy-row stop, which
    // is a deferral and never an error. Returns entries committed.
    StatusOr<std::size_t> BuildSeededSets(const catalog::TableAccess& access,
                                          std::uint16_t col_pos, std::uint64_t cabin_id,
                                          const std::function<bool()>& enabled, bool* aborted);

    txn::ReadView MintCheckView();

    // The budget's page proxy for a memory-resident set: 1 + entries/254,
    // 254 being what a Bound Cabin page holds - a stated proxy, not a
    // measurement, until Observational sets have durable pages (§9 phase 2).
    std::uint64_t PagesProxyOf(std::uint64_t cabin_id) const;

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    stats::CabinStore& cabins_;
    stats::CabinOptimizer& controller_;
    txn::TransactionManager* txn_;
    Counters counters_;
};

}  // namespace kds::exec
