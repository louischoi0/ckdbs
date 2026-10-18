#include "kds/stats/plan_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "kds/catalog/catalog.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::stats {

namespace {

// splitmix64's finalizer: a deterministic spread for the hash and for the
// prior's descent choices, which must not depend on a clock or an address.
std::uint64_t Mix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::uint64_t MonotonicNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// A page a prior may read: the expected type, owned by the relation (or by
// nothing, a page that predates owner stamping). A freed and reused page
// fails here and discards the seed (PS1).
Status CheckOwned(std::span<std::byte, kPageSize> page, PageType type, catalog::Oid oid) {
    const storage::PageHeaderFields header = storage::ReadPageHeader(page);
    if (header.page_type != static_cast<std::uint8_t>(type) ||
        (header.owner_oid != 0 && header.owner_oid != oid)) {
        return Status::NotFound("a prior's page is not the relation's");
    }
    return Status::OK();
}

Status SpentBudget() { return Status::ResourceExhausted("the steward's seed budget is spent"); }

// What one page of a prior's read gives, copied out under its hold. The page
// is let go when this returns, before the caller asks for another (the
// header's rule).
struct NodeFacts {
    bool leaf = false;
    std::uint16_t level = 0;   // an internal node's
    std::uint16_t fanout = 0;  // an internal node's children
    // The children asked for, by position: up to `kPriorDescents` of them.
    std::array<PageId, PlanStats::kPriorDescents> children{};
    std::size_t nchildren = 0;
    std::size_t live = 0;  // a leaf's live slots
};

// Reads one page. For an internal node `want` children are named: with
// `want` 1, the one `salt` picks (child 0 for salt 0); with more, every
// child where the node has no more than `want`, else the rightmost and
// `want - 1` spread evenly over the rest - the rightmost apart because an
// append-built tree's rightmost subtree is the one not full.
StatusOr<NodeFacts> ReadNode(storage::PageStore& store, PageId id, catalog::Oid oid,
                             std::size_t& budget, std::size_t want, std::uint64_t salt) {
    if (budget == 0) return SpentBudget();
    --budget;
    auto page = store.GetForRead(id);
    if (!page.ok()) return page.status();
    NodeFacts facts;
    const storage::PageHeaderFields header = storage::ReadPageHeader(page.value().bytes());
    if (header.page_type == static_cast<std::uint8_t>(PageType::kBtreeLeaf)) {
        if (Status s = CheckOwned(page.value().bytes(), PageType::kBtreeLeaf, oid); !s.ok()) return s;
        facts.leaf = true;
        heap::PageView leaf(page.value().bytes());
        const std::uint16_t slots = leaf.slot_count();
        heap::PageView::Tuple tuple{};
        for (std::uint16_t slot = 0; slot < slots; ++slot) {
            if (leaf.TryReadTupleIn(slot, slots, tuple) == heap::PageView::TupleRead::kRead &&
                !tuple.deleted) {
                ++facts.live;
            }
        }
        return facts;
    }
    if (Status s = CheckOwned(page.value().bytes(), PageType::kBtreeInternal, oid); !s.ok()) {
        return s;
    }
    const btree::InternalView node(page.value().bytes());
    facts.level = node.level();
    facts.fanout = static_cast<std::uint16_t>(node.entry_count() + 1);
    const auto child = [&node](std::size_t position) -> StatusOr<PageId> {
        if (position == 0) return node.leftmost_child();
        auto entry = node.Entry(static_cast<std::uint16_t>(position - 1));
        if (!entry.ok()) return entry.status();
        return entry.value().child;
    };
    std::array<std::size_t, PlanStats::kPriorDescents> positions{};
    std::size_t count = 0;
    want = std::min(want, positions.size());
    if (want <= 1) {
        positions[count++] = salt == 0 ? 0 : static_cast<std::size_t>(Mix(salt) % facts.fanout);
    } else if (facts.fanout <= want) {
        for (std::size_t p = 0; p < facts.fanout; ++p) positions[count++] = p;
    } else {
        const std::size_t rest = facts.fanout - 1;  // children before the rightmost
        for (std::size_t k = 0; k + 1 < want; ++k) positions[count++] = k * rest / (want - 1);
        positions[count++] = rest;
    }
    for (std::size_t k = 0; k < count; ++k) {
        auto id_at = child(positions[k]);
        if (!id_at.ok()) return id_at.status();
        facts.children[k] = id_at.value();
    }
    facts.nchildren = count;
    return facts;
}

}  // namespace

std::size_t PlanKeyHash::operator()(const PlanKey& key) const noexcept {
    std::uint64_t h = Mix(key.fetch_id ^ key.n);
    for (std::size_t i = 0; i < key.n && i < key.oids.size(); ++i) h = Mix(h ^ key.oids[i]);
    return static_cast<std::size_t>(h);
}

std::int16_t BandOf(double v) noexcept {
    if (!(v > 1.0)) return 0;
    return static_cast<std::int16_t>(std::lround(2.0 * std::log2(v)));
}

double BandCentre(std::int16_t band) noexcept { return std::exp2(band / 2.0); }

std::size_t LeafRowCapacity(std::uint32_t row_size) noexcept {
    // A leaf's body after the common and slotted headers and the tail's
    // sibling link, over one tuple's slot entry, MVCC header and row.
    constexpr std::size_t kBody =
        kPageSize - storage::kPageHeaderSize - heap::kHeaderSize - sizeof(PageId);
    const std::size_t per_row = heap::kSlotOnDiskSize + heap::kTupleHeaderOnDiskSize + row_size;
    return std::max<std::size_t>(1, kBody / per_row);
}

StatusOr<double> ReadRelationPrior(storage::PageStore& store, const catalog::TableAccess& access,
                                   std::size_t& budget) {
    PageId root = access.desc_page_id;
    if (access.anchor_page_id != kInvalidPageId) {
        if (budget == 0) return SpentBudget();
        --budget;
        auto anchor = store.GetForRead(access.anchor_page_id);
        if (!anchor.ok()) return anchor.status();
        if (Status s = CheckOwned(anchor.value().bytes(), PageType::kAnchor, access.oid); !s.ok()) {
            return s;
        }
        root = storage::AnchorClusteredRoot(anchor.value().bytes());
    }  // the anchor is let go before the root is asked for

    auto top = ReadNode(store, root, access.oid, budget, PlanStats::kPriorDescents, 0);
    if (!top.ok()) return top.status();
    // Height 1: the root's live slots, exact up to undecided versions.
    if (top.value().leaf) return static_cast<double>(top.value().live);

    // Height 2 or more, stratified by the root's children (CC-Q6 (c)): a
    // descent under each child the root named - every child when there are
    // no more than `kPriorDescents`, else the rightmost and the rest spread -
    // multiplying the fan-outs below it, then reading the leaf it reaches for
    // its live slots. Exact in leaves up to height 3 with few root children;
    // an append-built tree's under-full rightmost subtree is measured, not
    // averaged in. A budget spent mid-way keeps what finished.
    const NodeFacts& root_facts = top.value();
    const bool sampled_rest = root_facts.fanout > root_facts.nchildren;
    double rest_sum = 0;
    std::size_t rest_done = 0;
    std::optional<double> rightmost;
    double live_sum = 0;
    std::size_t sampled = 0;
    bool spent = false;
    for (std::size_t c = 0; c < root_facts.nchildren && !spent; ++c) {
        PageId at = root_facts.children[c];
        double product = 1;  // the leaves under this child
        bool complete = true;
        for (std::uint16_t level = static_cast<std::uint16_t>(root_facts.level - 1); level >= 1;
             --level) {
            auto node = ReadNode(store, at, access.oid, budget, 1,
                                 Mix(access.oid) ^ (c * 131u + level));
            if (!node.ok()) {
                spent = node.status().code() == StatusCode::kResourceExhausted;
                complete = false;
                break;
            }
            if (node.value().leaf || node.value().level != level) {
                complete = false;  // the tree moved under the read
                break;
            }
            product *= node.value().fanout;
            at = node.value().children[0];
        }
        if (!complete) continue;
        const bool is_rightmost = sampled_rest && c + 1 == root_facts.nchildren;
        if (is_rightmost) {
            rightmost = product;
        } else {
            rest_sum += product;
            ++rest_done;
        }
        auto leaf = ReadNode(store, at, access.oid, budget, 1, 0);
        if (!leaf.ok()) {
            spent = leaf.status().code() == StatusCode::kResourceExhausted;
            continue;
        }
        if (!leaf.value().leaf) continue;  // the tree moved under the read
        live_sum += static_cast<double>(leaf.value().live);
        ++sampled;
    }
    // No leaf read: a later step retries with a whole budget rather than
    // publish the constant fill; a tree whose leaves no read could reach
    // keeps it.
    if (sampled == 0 && spent) return SpentBudget();
    if (rest_done == 0 && !rightmost.has_value()) {
        return Status::NotFound("no descent of the prior completed");
    }
    const double rest_mean = rest_done > 0 ? rest_sum / static_cast<double>(rest_done)
                                           : rightmost.value_or(0);
    double leaves = 0;
    if (sampled_rest) {
        // The rightmost as measured, the others as their sample's mean.
        leaves = rightmost.value_or(rest_mean) +
                 static_cast<double>(root_facts.fanout - 1) * rest_mean;
    } else {
        leaves = static_cast<double>(root_facts.fanout) * rest_mean;
    }
    const double per_leaf =
        sampled > 0 ? live_sum / static_cast<double>(sampled)
                    : static_cast<double>(LeafRowCapacity(access.layout.row_size)) *
                          PlanStats::kAssumedLeafFill;
    return leaves * per_leaf;
}

// ---- The snapshot ----------------------------------------------------------

PlanStatsSnapshot::PlanStatsSnapshot(std::uint64_t generation,
                                     std::vector<std::pair<catalog::Oid, std::int16_t>> priors,
                                     std::vector<std::pair<PlanKey, Posterior>> posteriors)
    : generation_(generation), priors_(std::move(priors)), posteriors_(std::move(posteriors)) {
    std::sort(priors_.begin(), priors_.end());
    std::sort(posteriors_.begin(), posteriors_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::optional<double> PlanStatsSnapshot::RelationRows(catalog::Oid oid) const noexcept {
    const auto it = std::lower_bound(
        priors_.begin(), priors_.end(), oid,
        [](const std::pair<catalog::Oid, std::int16_t>& p, catalog::Oid o) { return p.first < o; });
    if (it == priors_.end() || it->first != oid) return std::nullopt;
    return BandCentre(it->second);
}

const PlanStatsSnapshot::Posterior* PlanStatsSnapshot::Find(const PlanKey& key) const noexcept {
    const auto it = std::lower_bound(
        posteriors_.begin(), posteriors_.end(), key,
        [](const std::pair<PlanKey, Posterior>& p, const PlanKey& k) { return p.first < k; });
    if (it == posteriors_.end() || !(it->first == key)) return nullptr;
    return &it->second;
}

// ---- The store -------------------------------------------------------------

PlanStats::PlanStats(std::uint32_t cores) {
    const std::uint32_t parts = std::max<std::uint32_t>(cores, 1);
    parts_.reserve(parts);
    for (std::uint32_t c = 0; c < parts; ++c) parts_.push_back(std::make_unique<Part>());
}

void PlanStats::Fold(std::uint32_t core, const PlanObservation& observation) noexcept {
    if (core >= parts_.size()) return;
    Part& part = *parts_[core];
    const LatchGuard hold(&part.latch);
    // Open addressing over a fixed table: no allocation on this path. A new
    // key past half the table is dropped and counted, so probes stay short;
    // the steward empties the table every step.
    const std::size_t start = PlanKeyHash{}(observation.key) % kKeysPerPart;
    for (std::size_t probe = 0; probe < kKeysPerPart; ++probe) {
        PartEntry& entry = part.entries[(start + probe) % kKeysPerPart];
        if (entry.used && !(entry.key == observation.key)) continue;
        if (!entry.used) {
            if (part.used >= kKeysPerPart / 2) break;
            entry = PartEntry{};
            entry.key = observation.key;
            entry.used = true;
            ++part.used;
        }
        for (std::size_t m = 1; m < kPosteriorMasks; ++m) {
            if ((observation.present & (1u << m)) == 0) continue;
            entry.sum[m] += observation.rows[m];
            ++entry.count[m];
        }
        entry.declines |= observation.declines;
        return;
    }
    part_drops_.fetch_add(1, std::memory_order_relaxed);
}

void PlanStats::WantPriors(std::uint32_t core, std::span<const catalog::Oid> oids) noexcept {
    if (core >= parts_.size() || oids.empty()) return;
    Part& part = *parts_[core];
    const LatchGuard hold(&part.latch);
    for (const catalog::Oid oid : oids) {
        const auto end = part.wanted.begin() + static_cast<std::ptrdiff_t>(part.nwanted);
        if (std::find(part.wanted.begin(), end, oid) != end) continue;
        if (part.nwanted < part.wanted.size()) part.wanted[part.nwanted++] = oid;
    }
}

bool PlanStats::Reband(std::int16_t& band, bool& banded, double v) noexcept {
    const double at = v > 1.0 ? 2.0 * std::log2(v) : 0.0;
    if (banded && std::abs(at - band) < 0.75) return false;
    const auto next = static_cast<std::int16_t>(std::lround(at));
    const bool moved = !banded || next != band;
    band = next;
    banded = true;
    return moved;
}

void PlanStats::Merge(std::uint64_t now_ns) {
    // Every part's entries are copied out under its latch, and the latch is
    // let go before anything here touches `master_` - a core's statement end
    // waits for a copy, never for a fold.
    std::unordered_map<PlanKey, PartEntry, PlanKeyHash> merged;
    for (const std::unique_ptr<Part>& part_ptr : parts_) {
        Part& part = *part_ptr;
        std::vector<PartEntry> taken;
        {
            const LatchGuard hold(&part.latch);
            taken.reserve(part.used);
            for (PartEntry& entry : part.entries) {
                if (!entry.used) continue;
                taken.push_back(entry);
                entry.used = false;
            }
            part.used = 0;
            for (std::size_t i = 0; i < part.nwanted; ++i) {
                if (std::find(wanted_.begin(), wanted_.end(), part.wanted[i]) == wanted_.end()) {
                    wanted_.push_back(part.wanted[i]);
                }
            }
            part.nwanted = 0;
        }
        // Summed across the cores first, so a posterior moves by one step
        // however many cores ran its statement.
        for (const PartEntry& entry : taken) {
            PartEntry& sum = merged[entry.key];
            sum.key = entry.key;
            for (std::size_t m = 1; m < kPosteriorMasks; ++m) {
                sum.sum[m] += entry.sum[m];
                sum.count[m] += entry.count[m];
            }
            sum.declines |= entry.declines;
        }
    }

    // Room for the new keys, made once a step: the least recently seen go.
    std::size_t fresh = 0;
    for (const auto& [key, entry] : merged) fresh += master_.count(key) == 0 ? 1 : 0;
    if (master_.size() + fresh > kMaxMasterKeys) {
        std::vector<std::pair<std::uint64_t, PlanKey>> by_age;
        by_age.reserve(master_.size());
        for (const auto& [key, master] : master_) by_age.emplace_back(master.seen_ns, key);
        const std::size_t drop = std::min(by_age.size(), master_.size() + fresh - kMaxMasterKeys);
        std::nth_element(by_age.begin(), by_age.begin() + static_cast<std::ptrdiff_t>(drop),
                         by_age.end());
        for (std::size_t i = 0; i < drop; ++i) master_.erase(by_age[i].second);
        dirty_ = true;
    }

    for (const auto& [key, entry] : merged) {
        Master& master = master_[key];
        master.seen_ns = now_ns;
        for (std::size_t m = 1; m < kPosteriorMasks; ++m) {
            if (entry.count[m] == 0) continue;
            const double mean =
                static_cast<double>(entry.sum[m]) / static_cast<double>(entry.count[m]);
            const auto bit = static_cast<std::uint16_t>(1u << m);
            bool banded = (master.present & bit) != 0;
            // The first observation sets the value; later ones move it a
            // quarter of the way (alpha = 1/4, PS4).
            master.value[m] = banded ? master.value[m] + (mean - master.value[m]) / 4.0 : mean;
            if (Reband(master.band[m], banded, master.value[m])) dirty_ = true;
            master.present |= bit;
        }
        if (entry.declines != 0) {
            if ((entry.declines & ~master.declines) != 0) dirty_ = true;  // published at once
            master.declines |= entry.declines;
            master.declined_ns = now_ns;
        }
    }
    // A decline no run has repeated within the TTL lapses (PS3).
    for (auto& [key, master] : master_) {
        if (master.declines != 0 && now_ns - master.declined_ns >= kDeclineTtlNs) {
            master.declines = 0;
            dirty_ = true;
        }
    }
}

Status PlanStats::Seed(catalog::Catalog& catalog, storage::PageStore& store, catalog::Oid oid,
                       std::uint64_t now_ns, std::size_t& budget) {
    auto access = catalog.InitTableAccess(oid);
    if (!access.ok()) {
        // Dropped or unreadable: its prior is gone; posteriors naming it are
        // unreachable, since oids are never reissued.
        if (priors_.erase(oid) != 0) dirty_ = true;
        return Status::OK();
    }
    const std::size_t before = budget;
    auto rows = ReadRelationPrior(store, *access.value(), budget);
    seed_pages_.fetch_add(before - budget, std::memory_order_relaxed);
    if (!rows.ok()) {
        if (rows.status().code() == StatusCode::kResourceExhausted) return rows.status();
        return Status::OK();  // a mismatched page: this seed is discarded
    }
    seeds_.fetch_add(1, std::memory_order_relaxed);
    Prior& prior = priors_[oid];
    prior.seeded_ns = now_ns;
    if (Reband(prior.band, prior.banded, std::max(rows.value(), 1.0))) dirty_ = true;
    return Status::OK();
}

void PlanStats::Install(std::vector<std::pair<catalog::Oid, std::int16_t>> priors,
                        std::vector<std::pair<PlanKey, PlanStatsSnapshot::Posterior>> posteriors) {
    const std::uint64_t next = generation_.load(std::memory_order_relaxed) + 1;
    auto snapshot =
        std::make_shared<const PlanStatsSnapshot>(next, std::move(priors), std::move(posteriors));
    const LatchGuard hold(&publish_latch_);
    current_ = std::move(snapshot);
    generation_.store(next, std::memory_order_release);
}

void PlanStats::Publish() {
    std::vector<std::pair<catalog::Oid, std::int16_t>> priors;
    priors.reserve(priors_.size());
    for (const auto& [oid, prior] : priors_) {
        if (prior.banded) priors.emplace_back(oid, prior.band);
    }
    std::vector<std::pair<PlanKey, PlanStatsSnapshot::Posterior>> posteriors;
    posteriors.reserve(master_.size());
    for (const auto& [key, master] : master_) {
        PlanStatsSnapshot::Posterior p;
        p.band = master.band;
        p.present = master.present;
        p.declines = master.declines;
        posteriors.emplace_back(key, p);
    }
    Install(std::move(priors), std::move(posteriors));
    publishes_.fetch_add(1, std::memory_order_relaxed);
    dirty_ = false;
}

Status PlanStats::StewardStep(catalog::Catalog& catalog, storage::PageStore& store,
                              std::uint64_t now_ns) {
    const std::uint64_t started = MonotonicNs();
    Merge(now_ns);

    std::size_t budget = kSeedPageBudget;
    Status seeded = Status::OK();
    // Wanted oids first, then the known ones round-robin past the cursor.
    while (!wanted_.empty() && seeded.ok()) {
        const catalog::Oid oid = wanted_.back();
        const auto known = priors_.find(oid);
        if (known != priors_.end() && now_ns - known->second.seeded_ns < kReseedNs) {
            wanted_.pop_back();
            continue;
        }
        seeded = Seed(catalog, store, oid, now_ns, budget);
        if (seeded.ok()) wanted_.pop_back();
    }
    for (std::size_t visited = 0; seeded.ok() && visited < priors_.size(); ++visited) {
        auto it = priors_.upper_bound(reseed_cursor_);
        if (it == priors_.end()) it = priors_.begin();
        reseed_cursor_ = it->first;
        if (now_ns - it->second.seeded_ns < kReseedNs) continue;
        seeded = Seed(catalog, store, it->first, now_ns, budget);
    }

    if (dirty_) Publish();
    // One writer, the steward: a plain compare and store.
    const std::uint64_t took = MonotonicNs() - started;
    if (took > steward_ns_max_.load(std::memory_order_relaxed)) {
        steward_ns_max_.store(took, std::memory_order_relaxed);
    }
    // A spent budget is the step's normal end, not a failure.
    if (!seeded.ok() && seeded.code() != StatusCode::kResourceExhausted) return seeded;
    return Status::OK();
}

std::shared_ptr<const PlanStatsSnapshot> PlanStats::Current() const {
    const LatchGuard hold(&publish_latch_);
    return current_;
}

void PlanStats::PublishForTest(
    std::vector<std::pair<catalog::Oid, std::int16_t>> priors,
    std::vector<std::pair<PlanKey, PlanStatsSnapshot::Posterior>> posteriors) {
    Install(std::move(priors), std::move(posteriors));
}

PlanStats::Counters PlanStats::counters() const noexcept {
    Counters c;
    c.part_drops = part_drops_.load(std::memory_order_relaxed);
    c.seeds = seeds_.load(std::memory_order_relaxed);
    c.seed_pages = seed_pages_.load(std::memory_order_relaxed);
    c.publishes = publishes_.load(std::memory_order_relaxed);
    c.steward_ns_max = steward_ns_max_.load(std::memory_order_relaxed);
    return c;
}

// ---- A core's source -------------------------------------------------------

std::shared_ptr<const PlanStatsSnapshot> PlanStatsSource::Pin() {
    if (stats_ == nullptr) return nullptr;
    const std::uint64_t generation = stats_->generation();
    if (generation != cached_generation_) {
        cached_ = stats_->Current();
        cached_generation_ = cached_ != nullptr ? cached_->generation() : generation;
    }
    return cached_;
}

}  // namespace kds::stats
