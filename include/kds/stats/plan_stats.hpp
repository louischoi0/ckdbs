#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/stats/managed_statistics.hpp"
#include "kds/stats/plan_key.hpp"
#include "kds/storage/visit.hpp"

namespace kds::catalog {
class Catalog;
}
namespace kds::storage {
class PageStore;
}

// **The join planner's statistics** (docs/spec/plan-stats.md; work order
// instructions/v3.2.0/workorder-cc-plan-statistics.md). Memory-resident:
// a restart or a restore empties it, and nothing here is logged.
//
// ---- The latch protocol ------------------------------------------------
//
// One `Part` per core under its own `LatchKind::kStatsRing` latch, written
// only by its core - at a planned statement's end - and emptied only by the
// steward on core 0, which copies a part's entries out under its latch and
// folds them after letting it go (`statement_stats.hpp`'s protocol). The
// steward's own state (`master_`, `priors_`, `wanted_`) is touched by the
// steward alone. The published snapshot is immutable; it is swapped under
// `publish_latch_` and announced by `generation_` (release), which a compile
// reads with one acquire load (`PlanStatsSource`).
//
// The steward reads pages **one at a time** (`ReadRelationPrior`): it lets a
// page go before asking for the next, as every read descent does
// (btree.hpp), because a split holds pages bottom-up.

namespace kds::stats {

// One complete run of a planned block (PS2, PS3).
struct PlanObservation {
    PlanKey key;
    std::array<std::uint64_t, kPosteriorMasks> rows{};  // card(T), by mask
    std::uint16_t present = 0;  // a bit per mask `rows` holds
    std::uint8_t declines = 0;  // a bit per written position
};

// A value's half-octave band, `round(2 log2 v)` for `v >= 1`, and back.
std::int16_t BandOf(double v) noexcept;
double BandCentre(std::int16_t band) noexcept;

// What a compile reads: immutable once published.
class PlanStatsSnapshot {
public:
    struct Posterior {
        std::array<std::int16_t, kPosteriorMasks> band{};
        std::uint16_t present = 0;
        std::uint8_t declines = 0;
    };

    PlanStatsSnapshot() = default;
    PlanStatsSnapshot(std::uint64_t generation,
                      std::vector<std::pair<catalog::Oid, std::int16_t>> priors,
                      std::vector<std::pair<PlanKey, Posterior>> posteriors,
                      std::vector<std::pair<catalog::Oid, std::int16_t>> managed = {});

    std::uint64_t generation() const noexcept { return generation_; }
    // The relation's rows as the planner prices them: a published managed
    // object's band centre (CY-Q16 (a): it outranks the steward's prior
    // while it exists), else the steward's prior's, else nullopt.
    std::optional<double> RelationRows(catalog::Oid oid) const noexcept;
    // The steward's prior alone (PS1), whatever a managed object says - what
    // `sys.statistics` shows as the `observed` row.
    std::optional<double> ObservedRows(catalog::Oid oid) const noexcept;
    // Whether a managed object prices the relation.
    bool Managed(catalog::Oid oid) const noexcept;
    // The steward's priors, by oid.
    const std::vector<std::pair<catalog::Oid, std::int16_t>>& observed() const noexcept {
        return priors_;
    }
    const Posterior* Find(const PlanKey& key) const noexcept;
    std::size_t relations() const noexcept { return priors_.size(); }
    std::size_t posteriors() const noexcept { return posteriors_.size(); }

private:
    friend class PlanStats;  // a managed publish carries the steward's facts over

    std::uint64_t generation_ = 0;
    std::vector<std::pair<catalog::Oid, std::int16_t>> priors_;     // by oid
    std::vector<std::pair<PlanKey, Posterior>> posteriors_;         // by key
    std::vector<std::pair<catalog::Oid, std::int16_t>> managed_;    // by oid
};

class PlanStats {
public:
    // Fixed per core, allocated here so a statement allocates nothing.
    static constexpr std::size_t kKeysPerPart = 1024;
    static constexpr std::size_t kWantedPerPart = 64;
    // The steward's bounds (PS4).
    static constexpr std::size_t kSeedPageBudget = 16;
    static constexpr std::uint64_t kReseedNs = 10'000'000'000;
    static constexpr std::size_t kMaxMasterKeys = 16384;
    // How long a decline fact stands without a new decline (PS3): the
    // planner prices no build where one declined, so no run re-tests it;
    // past this the fact lapses and the build is priced, and tried, again.
    static constexpr std::uint64_t kDeclineTtlNs = 60'000'000'000;
    // A leaf's fill the prior assumes where no leaf could be sampled
    // (CC-Q6 (c)): an append-split leaf is full and a mid-split one about
    // half.
    static constexpr double kAssumedLeafFill = 0.75;
    // The root children a prior descends under at most, and so the leaves
    // it samples (PS1).
    static constexpr std::size_t kPriorDescents = 4;

    explicit PlanStats(std::uint32_t cores);

    // ---- The statement's path: its own core's part only -------------------
    void Fold(std::uint32_t core, const PlanObservation& observation) noexcept;
    // Relations a planned run met without a prior, under one hold.
    void WantPriors(std::uint32_t core, std::span<const catalog::Oid> oids) noexcept;

    // ---- The steward: core 0's `system` group alone (PS4) ------------------
    Status StewardStep(catalog::Catalog& catalog, storage::PageStore& store,
                       std::uint64_t now_ns);

    // ---- Reading -----------------------------------------------------------
    std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }
    std::shared_ptr<const PlanStatsSnapshot> Current() const;

    // ---- Space (DF-S4, DF-Q9 (a)) -------------------------------------------
    //
    // A relation's delete-marked rows as the steward's last seed that read
    // a leaf sampled them: the delete-marked slots of the leaves it read for
    // the prior, scaled as PS1 scales their live slots. `nullopt` for a
    // relation no seed has sampled a leaf of. **Not a planner fact** (DF-R8): a side
    // table only `sys.relation_stats` reads, in no snapshot, moving no
    // generation. Any core; the steward is its one writer.
    std::optional<std::uint64_t> EstDeleteMarked(catalog::Oid oid) const;

    // ---- Across a clean shutdown (CY-S1, plan-stats.md §8) -----------------
    //
    // Both run with no steward step and no statement in flight: the first at
    // a mount, before the listener binds, the second at a shutdown, after
    // the cores are joined.
    //
    // Seeds each relation's prior at its band and publishes them as the
    // next generation, so the first compile of the run prices from them. A
    // carried prior is due a seed whatever the clock reads, so the tree's
    // own pages replace it as the steward comes round (PS4).
    void SeedPriors(std::span<const std::pair<catalog::Oid, std::int16_t>> bands);
    // Every banded prior with its band, by oid.
    std::vector<std::pair<catalog::Oid, std::int16_t>> ExportPriors() const;
    // What a shutdown writes: every banded prior, then every managed slot's
    // versions and its slot row (plan-stats.md §9). No closing mark.
    std::vector<catalog::SysStatisticsRow> ExportStatistics() const;
    // What a mount does with the rows a whole chain held: the managed store
    // loaded, the priors seeded (`SeedPriors`), one generation published.
    void LoadStatistics(std::span<const catalog::SysStatisticsRow> rows);

    // ---- Managed statistics (CY-S1b, CY-S4; plan-stats.md §9) -------------
    //
    // The operator's objects. Any core's statement changes them, then asks
    // `RepublishManaged`, so the next compile on every core prices from them.
    ManagedStatistics& managed() noexcept { return managed_; }
    const ManagedStatistics& managed() const noexcept { return managed_; }
    // The next generation - the published snapshot's steward facts as they
    // stand, with the managed objects' bands read again - where a published
    // band moved; nothing where none did (CY-Q31 (a)). Any thread.
    void RepublishManaged();

    // A test's seam: publishes these facts as the next generation.
    void PublishForTest(std::vector<std::pair<catalog::Oid, std::int16_t>> priors,
                        std::vector<std::pair<PlanKey, PlanStatsSnapshot::Posterior>> posteriors);

    // The Schmitt step (PS5): an unbanded value takes its band; a banded
    // one moves only once `v` lies 0.75 of a band from the published band's
    // centre. Answers whether the band changed.
    static bool Reband(std::int16_t& band, bool& banded, double v) noexcept;

    struct Counters {
        std::uint64_t part_drops = 0;
        std::uint64_t seeds = 0;
        std::uint64_t seed_pages = 0;
        std::uint64_t publishes = 0;
        std::uint64_t steward_ns_max = 0;
    };
    Counters counters() const noexcept;

private:
    struct PartEntry {
        PlanKey key;
        std::array<std::uint64_t, kPosteriorMasks> sum{};
        std::array<std::uint32_t, kPosteriorMasks> count{};
        std::uint8_t declines = 0;
        bool used = false;
    };
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::vector<PartEntry> entries = std::vector<PartEntry>(kKeysPerPart);
        std::size_t used = 0;
        std::array<catalog::Oid, kWantedPerPart> wanted{};
        std::size_t nwanted = 0;
    };
    struct Master {
        std::array<double, kPosteriorMasks> value{};
        std::array<std::int16_t, kPosteriorMasks> band{};
        std::uint16_t present = 0;
        std::uint8_t declines = 0;
        std::uint64_t declined_ns = 0;  // the newest decline's step
        std::uint64_t seen_ns = 0;
    };
    struct Prior {
        std::int16_t band = 0;
        bool banded = false;
        // Seeded at a mount from the previous run (`SeedPriors`), and so due
        // a seed from the tree's own pages whatever the clock reads.
        bool carried = false;
        std::uint64_t seeded_ns = 0;
    };

    void Merge(std::uint64_t now_ns);
    Status Seed(catalog::Catalog& catalog, storage::PageStore& store, catalog::Oid oid,
                std::uint64_t now_ns, std::size_t& budget);
    void Publish();
    // Builds a snapshot of these facts as the next generation and swaps it in.
    void Install(std::vector<std::pair<catalog::Oid, std::int16_t>> priors,
                 std::vector<std::pair<PlanKey, PlanStatsSnapshot::Posterior>> posteriors);
    // `Install`'s body, `publish_latch_` held: the managed bands read here.
    void InstallLocked(std::vector<std::pair<catalog::Oid, std::int16_t>> priors,
                       std::vector<std::pair<PlanKey, PlanStatsSnapshot::Posterior>> posteriors);

    std::vector<std::unique_ptr<Part>> parts_;
    std::vector<catalog::Oid> wanted_;  // merged, the steward's
    std::unordered_map<PlanKey, Master, PlanKeyHash> master_;
    std::map<catalog::Oid, Prior> priors_;
    catalog::Oid reseed_cursor_ = 0;
    bool dirty_ = false;
    // `EstDeleteMarked`'s side table, under its own latch: the steward
    // writes, a view on any core reads, and nothing else touches it.
    mutable Latch space_latch_{LatchKind::kStatsRing};
    std::unordered_map<catalog::Oid, std::uint64_t> delete_marked_;

    // Taken before `managed_`'s latch, never after it: `Install` and
    // `RepublishManaged` read the bands under this one.
    mutable Latch publish_latch_{LatchKind::kStatsRing};
    ManagedStatistics managed_;
    std::shared_ptr<const PlanStatsSnapshot> current_;
    std::atomic<std::uint64_t> generation_{0};

    std::atomic<std::uint64_t> part_drops_{0};
    std::atomic<std::uint64_t> seeds_{0};
    std::atomic<std::uint64_t> seed_pages_{0};
    std::atomic<std::uint64_t> publishes_{0};
    std::atomic<std::uint64_t> steward_ns_max_{0};
};

// A core's view of the published snapshot (PS5): one acquire load a pin,
// and the publish latch and a reference copy only when the generation
// moved. Owned by one core's dispatcher; not shared.
class PlanStatsSource {
public:
    explicit PlanStatsSource(const PlanStats* stats = nullptr) noexcept : stats_(stats) {}
    std::shared_ptr<const PlanStatsSnapshot> Pin();

private:
    const PlanStats* stats_;
    std::uint64_t cached_generation_ = 0;
    std::shared_ptr<const PlanStatsSnapshot> cached_;
};

// **A `GATHER STATISTICS` walk** (CY-S1b; CY-Q26 (a)): every leaf of a
// relation's clustered tree, through the sibling links, counting its live
// slots as PS1's prior does - so a gathered count and an observed one are
// one quantity - and its leaves. Pages, not rows: no view is read and no
// purge horizon held, so the count is of a relation that moved under the
// walk (CY-Q7 (a)), undecided versions as they lie. One page at a time, each
// checked to be a leaf the relation owns, the chain under a cycle check.
// Its caller owns the slicing, the borrow and the yields.
class RelationGather {
public:
    explicit RelationGather(catalog::Oid oid) noexcept : oid_(oid) {}

    // The leftmost leaf, from the relation's anchor or descriptor.
    Status Start(storage::PageStore& store, const catalog::TableAccess& access);
    // Up to `leaves` more leaves. A page that is not one of the relation's
    // leaves - the tree moved under the walk in a way no live relation's
    // does - ends the walk `TxnConflict`, with nothing to publish.
    Status Step(storage::PageStore& store, std::uint32_t leaves);

    bool started() const noexcept { return started_; }
    bool done() const noexcept { return started_ && next_ == kInvalidPageId; }
    catalog::Oid oid() const noexcept { return oid_; }
    std::uint64_t rows() const noexcept { return rows_; }
    std::uint64_t leaves() const noexcept { return leaves_; }

private:
    catalog::Oid oid_;
    bool started_ = false;
    PageId next_ = kInvalidPageId;
    std::uint64_t rows_ = 0;
    std::uint64_t leaves_ = 0;
    storage::PageChainGuard chain_{"a statistics gather's leaf chain"};
};

// The rows a leaf of a relation with `row_size`-byte rows holds when full.
std::size_t LeafRowCapacity(std::uint32_t row_size) noexcept;

// PS1's prior for one relation, reading at most `budget` pages, one at a
// time, and decrementing it. Exposed for its tests. `delete_marked`, where
// given, takes the delete-marked slots of the leaves the prior read, scaled
// as their live slots are (DF-S4); it is left as it was where no leaf was
// read.
StatusOr<double> ReadRelationPrior(storage::PageStore& store, const catalog::TableAccess& access,
                                   std::size_t& budget,
                                   std::optional<double>* delete_marked = nullptr);

}  // namespace kds::stats
