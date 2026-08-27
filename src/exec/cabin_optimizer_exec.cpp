#include "kds/exec/cabin_optimizer_exec.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

#include "kds/exec/row_codec.hpp"
#include "kds/exec/tuple_verify.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/txn/visibility.hpp"

namespace kds::exec {

namespace {

// What a Bound Cabin page holds; the memory-resident sets' page proxy
// (see the header). Kept as a local constant rather than an include of the
// bound-page layout: the number is a pricing convention here, not a format.
constexpr std::uint64_t kEntriesPerPageProxy = 254;

}  // namespace

txn::ReadView CabinOptimizerExecutor::MintCheckView() {
    // No manager means no transactions: every row carries kBootstrapXid and
    // is committed, which is what the all-visible view says.
    if (txn_ == nullptr) return txn::ReadView::Everything();
    auto minted = txn_->MintReadView(txn::kNoTrxId);
    if (minted.ok()) return minted.value();

    // **A failed mint must not fall back to the all-visible view.** Under one,
    // `CheckVisibility` can never answer kBusy - every id below UINT64_MAX is
    // visible to it - so the busy-row abort that protects completeness stops
    // firing, and an in-flight transaction's uncommitted delete-mark reads as
    // kAbsent: the row is skipped, its ROLLBACK restores it, and the banked
    // set is missing a live pk. That is cabin.md §6a's break exactly, in
    // the other site that banks a set.
    //
    // The default view is the opposite fallback and the safe one: it makes
    // every writer but the always-visible id kBusy, so the first user row
    // defers the whole build (which the caller retries). Unreachable today -
    // Begin() caps live transactions at kMaxTrackedLiveTxns, which is the
    // exact width AddInFlight holds - and written for the day either bound
    // moves.
    return txn::ReadView{};
}

std::uint64_t CabinOptimizerExecutor::PagesProxyOf(std::uint64_t cabin_id) const {
    return 1 + cabins_.InfoFor(cabin_id).entries / kEntriesPerPageProxy;
}

Status CabinOptimizerExecutor::Tick(stats::OptimizerSignals& signals,
                                    const std::function<bool()>& enabled) {
    if (!enabled()) return Status::OK();  // off means off: not even a snapshot
    ++counters_.ticks;
    const stats::OptimizerSnapshot snapshot = signals.Snapshot();
    return Apply(controller_.Decide(snapshot), enabled);
}

Status CabinOptimizerExecutor::Apply(const stats::ActionSet& actions,
                                     const std::function<bool()>& enabled) {
    for (const stats::ActionItem& action : actions) {
        if (!enabled()) return Status::OK();  // PO8: checked between actions
        Status applied = Status::OK();
        switch (action.action) {
            case stats::CabinAction::kCreate:
                applied = ApplyCreate(action, enabled);
                break;
            case stats::CabinAction::kExtend:
                applied = ApplyExtend(action, enabled);
                break;
            case stats::CabinAction::kHeal:
                applied = ApplyHeal(action);
                break;
            case stats::CabinAction::kDrop:
                applied = ApplyDrop(action);
                break;
        }
        if (!applied.ok()) return applied;
    }
    return Status::OK();
}

StatusOr<std::size_t> CabinOptimizerExecutor::BuildSeededSets(
    const catalog::TableAccess& access, std::uint16_t col_pos, std::uint64_t cabin_id,
    const std::function<bool()>& enabled, bool* aborted) {
    *aborted = false;
    const std::vector<stats::CabinKey> seeds = cabins_.SightedUnobservedOf(cabin_id);
    if (seeds.empty()) return std::size_t{0};

    std::unordered_map<stats::CabinKey, std::vector<stats::CabinEntry>, stats::CabinKeyHash>
        collected;
    for (const stats::CabinKey& seed : seeds) collected.emplace(seed, std::vector<stats::CabinEntry>{});

    const txn::ReadView check_view = MintCheckView();

    // PO4's mandate, structural: the walk's pages come from the scan ring,
    // so the build cannot displace the foreground working set.
    auto ring = store_.OpenScanRing();

    // A btree leaf is a heap page, so one loop serves both clustered forms
    // - only the first leaf differs (the assertion builder's shape).
    PageId leaf = access.desc_page_id;
    if (access.clustered_type == catalog::ClusteredType::kBtree) {
        auto first = btree::BtreeLeftmostLeaf(store_, access.desc_page_id);
        if (!first.ok()) return first.status();
        leaf = first.value();
    }

    struct StagedRow {
        std::uint64_t pk = 0;
        std::uint16_t slot = 0;
        std::vector<std::byte> payload;
    };

    const PageId walk_origin = leaf;
    for (std::uint32_t leaves = 0; leaf != kInvalidPageId; ++leaves) {
        if (Status s = storage::CheckPageWalkBudget(leaves, walk_origin, "relation chain");
            !s.ok()) {
            return s;
        }
        // PO8, between pages: an OFF mid-build discards cleanly, because
        // nothing commits until the walk completes.
        if (!enabled()) {
            *aborted = true;
            return std::size_t{0};
        }

        // Phase 1: copy out under the ring page, no fetch beneath it. The
        // ring's stricter lifetime is honored the same way: this page is
        // finished before anything can rotate it away.
        std::vector<StagedRow> staged;
        PageId next = kInvalidPageId;
        std::uint32_t page_epoch = 0;
        {
            auto bytes = ring->Fetch(leaf);
            if (!bytes.ok()) return bytes.status();
            heap::PageView page(bytes.value());
            page_epoch = static_cast<std::uint32_t>(page.RelayoutEpoch());
            const std::uint16_t n = page.slot_count();
            staged.reserve(n);
            for (std::uint16_t i = 0; i < n; ++i) {
                auto tuple = page.ReadTuple(i);
                if (tuple.status().code() == StatusCode::kNotFound) continue;  // retired
                if (!tuple.ok()) return tuple.status();

                switch (txn::CheckVisibility(check_view, tuple.value().trx_id,
                                             tuple.value().deleted)) {
                    case txn::CheckVerdict::kBusy:
                        // An in-flight row can neither be counted (its
                        // abort would leave a phantom entry) nor skipped
                        // (its commit already passed the write hook while
                        // the value was unobserved). Defer the whole build;
                        // demand re-nominates (AST06's argument).
                        *aborted = true;
                        return std::size_t{0};
                    case txn::CheckVerdict::kAbsent:
                        continue;
                    case txn::CheckVerdict::kLive:
                        break;
                }

                auto pk = kds::KeystoneIdOfPayload(tuple.value().payload);
                if (!pk.ok()) return pk.status();
                StagedRow row;
                row.pk = pk.value();
                row.slot = i;
                row.payload.assign(tuple.value().payload.begin(), tuple.value().payload.end());
                staged.push_back(std::move(row));
            }
            next = page.next_page_id();
        }

        // Phase 2: decode and collect. Spill fetches are legal here - the
        // page span is finished with - and they go through the ordinary
        // path, never the ring.
        for (const StagedRow& row : staged) {
            std::vector<PendingSpill> spills;
            auto decoded = DecodeRow(access.schema, access.layout, row.payload, &spills);
            if (!decoded.ok()) return decoded.status();
            if (Status s = ResolveSpills(store_, spills, decoded.value()); !s.ok()) return s;

            auto key = stats::MakeCabinKey(cabin_id, decoded.value()[col_pos]);
            if (!key.has_value()) continue;  // NULL: never observable
            auto bucket = collected.find(*key);
            if (bucket == collected.end()) continue;  // not a seeded value

            stats::CabinEntry entry;
            entry.pk = row.pk;
            entry.page_id = leaf;
            entry.page_epoch = page_epoch;
            entry.slot = row.slot;
            entry.flags = stats::kCabinHintValid;
            bucket->second.push_back(entry);
        }
        leaf = next;
    }

    // The walk completed - the superset invariant's precondition - so
    // every seed commits, **empty sets included**: an observed value with
    // no rows is the authoritative zero-rows answer.
    std::size_t committed = 0;
    for (const stats::CabinKey& seed : seeds) {
        auto bucket = collected.find(seed);
        const std::size_t entries = bucket->second.size();
        if (cabins_.Commit(seed, std::move(bucket->second))) committed += entries;
        // A cap refusal is a complete answer, never an error (§1).
    }
    return committed;
}

Status CabinOptimizerExecutor::ApplyCreate(const stats::ActionItem& action,
                                           const std::function<bool()>& enabled) {
    auto created = catalog_.CreateCabin(action.rel_oid,
                                        static_cast<std::uint16_t>(action.col_pos),
                                        catalog::kCabinOriginAuto);
    if (!created.ok()) {
        // A *policy* refusal (`NO CABIN`, a vanished relation) is a
        // permanent "no": the candidate parks in BUILDING rather than
        // resetting to CANDIDATE, or the controller would retry a settled
        // refusal every confirm cycle forever. A transient failure resets
        // and retries.
        if (created.status().code() != StatusCode::kInvalidArgument &&
            created.status().code() != StatusCode::kUnsupported &&
            created.status().code() != StatusCode::kNotFound) {
            controller_.NoteBuildFailed(action.rel_oid, action.col_pos);
            ++counters_.build_failures;
        }
        return Status::OK();
    }
    const std::uint64_t cabin_id = created.value();

    auto access = catalog_.InitTableAccess(action.rel_oid);
    if (!access.ok()) {
        (void)catalog_.DropCabin(cabin_id);
        cabins_.Forget(cabin_id);
        controller_.NoteBuildFailed(action.rel_oid, action.col_pos);
        ++counters_.build_failures;
        return Status::OK();
    }

    // The seeded build. Empty for a first-time column by construction (no
    // Cabin meant no CabinKey sightings), in which case the ordinary n=2
    // machinery populates from traffic - §II.5's observation-based
    // population, literally.
    bool aborted = false;
    auto built = BuildSeededSets(*access.value(), static_cast<std::uint16_t>(action.col_pos),
                                 cabin_id, enabled, &aborted);
    if (!built.ok() || aborted) {
        // BUILDING → discard, cleanly: drop the row, forget the store
        // side, re-nominate from scratch (PO5). Nothing was committed.
        (void)catalog_.DropCabin(cabin_id);
        cabins_.Forget(cabin_id);
        controller_.NoteBuildFailed(action.rel_oid, action.col_pos);
        if (aborted) {
            ++counters_.builds_deferred;
        } else {
            ++counters_.build_failures;
        }
        return built.ok() ? Status::OK() : built.status();
    }

    controller_.NoteCreated(action.rel_oid, action.col_pos, cabin_id, PagesProxyOf(cabin_id));
    ++counters_.creates;
    return Status::OK();
}

Status CabinOptimizerExecutor::ApplyExtend(const stats::ActionItem& action,
                                           const std::function<bool()>& enabled) {
    auto access = catalog_.InitTableAccess(action.rel_oid);
    if (!access.ok()) return Status::OK();  // the relation went away: nothing to widen

    bool aborted = false;
    auto built = BuildSeededSets(*access.value(), static_cast<std::uint16_t>(action.col_pos),
                                 action.cabin_id, enabled, &aborted);
    if (!built.ok()) {
        ++counters_.build_failures;
        return built.status();
    }
    // An aborted extend committed nothing and needs no unwinding; the
    // sighting evidence survives and the next tick may retry.
    if (aborted) {
        ++counters_.builds_deferred;
        return Status::OK();
    }
    // The completion edge (PHY06): the widened sets moved the page proxy,
    // and an unreported extend undercounts PO6's budget by the growth
    // forever. The new total, not a delta - idempotent by construction.
    controller_.NoteExtended(action.cabin_id, PagesProxyOf(action.cabin_id));
    ++counters_.extends;
    return Status::OK();
}

Status CabinOptimizerExecutor::ApplyHeal(const stats::ActionItem& action) {
    auto access = catalog_.InitTableAccess(action.rel_oid);
    if (!access.ok()) return Status::OK();
    const bool is_btree =
        access.value()->clustered_type == catalog::ClusteredType::kBtree;

    for (const stats::CabinKey& key : cabins_.ObservedValuesOf(action.cabin_id)) {
        std::vector<stats::CabinEntry>* entries = cabins_.Find(key);
        if (entries == nullptr) continue;

        for (std::size_t i = 0; i < entries->size();) {
            stats::CabinEntry& entry = (*entries)[i];
            if (!entry.hint_valid()) {
                ++i;
                continue;
            }
            VerifiedTuple verified =
                VerifyTupleAt(store_, entry.page_id, entry.slot, entry.pk, entry.page_epoch);
            if (verified.ok()) {
                ++i;
                continue;
            }
            if (!is_btree) {
                // No descent to heal with: the whole value un-observes,
                // §5's heap answer, and this key's loop is over.
                cabins_.Unobserve(key);
                break;
            }
            auto found = btree::BtreeLookup(store_, access.value()->desc_page_id, entry.pk);
            if (!found.ok()) {
                if (found.status().code() == StatusCode::kNotFound) {
                    // Dangling: by K1 dead forever, droppable on sight.
                    entries->erase(entries->begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                return found.status();
            }
            entry.page_id = found.value().page_id;
            entry.slot = found.value().slot;
            // The healed page's current epoch, through the one producer the
            // read path's in-place heal also uses - a stamp of 0 against a
            // bumped page misses on every later resolve and re-heals forever.
            entry.page_epoch = CurrentRelayoutEpoch(store_, entry.page_id);
            entry.flags |= stats::kCabinHintValid;
            ++i;
        }
    }
    ++counters_.heals;
    return Status::OK();
}

Status CabinOptimizerExecutor::ApplyDrop(const stats::ActionItem& action) {
    // Row first, then the sets: the compiler stops emitting probes the
    // moment the row is gone, so the store-side forget is "late is fine".
    Status dropped = catalog_.DropCabin(action.cabin_id);
    if (!dropped.ok() && dropped.code() != StatusCode::kNotFound) return dropped;
    cabins_.Forget(action.cabin_id);
    controller_.NoteDropped(action.cabin_id);
    ++counters_.drops;
    return Status::OK();
}

}  // namespace kds::exec
