#include "kds/stats/optimizer_signals.hpp"

#include <algorithm>
#include <limits>

namespace kds::stats {

namespace {

// The coldest entry's key, by decayed primary score at `now`. The caller
// only asks when its map is full, so the O(n) scan is bounded by the cap
// and paid on the eviction path alone.
//
// **Ranked in the log domain**, and that is a correctness point rather
// than a refinement: a full table is mostly *idle* entries, and Q24.8
// reads every score older than ~16 half-lives as exactly 0. Comparing
// those, the scan found a tie and kept the first the unordered_map
// happened to yield - so the victim was arbitrary among the cold, and a
// fingerprint idle for an hour could outlive one idle for a week.
// `Log2ValueAt` reads the same states through a projection that keeps
// them ordered however long they idle (decay.hpp).
template <typename Map, typename Primary>
std::uint64_t ColdestOf(Map& map, const sched::Clock* clock, sched::MonoTimeNs half_life,
                        Primary primary) {
    if (map.empty()) return 0;
    // Seeded from a real entry rather than from the type's maximum: a table
    // whose every score has saturated at the ceiling would leave a
    // sentinel key nothing matches, the erase would remove nothing, and the
    // cap would quietly stop being a cap.
    std::uint64_t coldest_key = map.begin()->first;
    Log2Q16 coldest = Log2ValueAt(primary(map.begin()->second), clock, half_life);
    for (auto& [key, signal] : map) {
        const Log2Q16 value = Log2ValueAt(primary(signal), clock, half_life);
        if (value < coldest) {
            coldest = value;
            coldest_key = key;
        }
    }
    return coldest_key;
}

}  // namespace

FingerprintSignal* OptimizerSignals::FingerprintFor(std::uint64_t pattern_id) {
    auto found = fingerprints_.find(pattern_id);
    if (found != fingerprints_.end()) return &found->second;
    if (fingerprints_.size() >= kMaxTrackedFingerprints) {
        // Evict the coldest to admit the newcomer. Restarting the victim's
        // count is a performance event; refusing the newcomer would starve
        // every pattern that turns hot after the table filled, which is
        // exactly the pattern a heat tracker exists to notice.
        fingerprints_.erase(ColdestOf(fingerprints_, clock_, half_life_ns_,
                                      [](const FingerprintSignal& s) { return s.executions; }));
    }
    return &fingerprints_[pattern_id];
}

CabinQualitySignal* OptimizerSignals::CabinFor(std::uint64_t cabin_id) {
    auto found = cabins_.find(cabin_id);
    if (found != cabins_.end()) return &found->second;
    if (cabins_.size() >= kMaxTrackedCabins) {
        cabins_.erase(ColdestOf(cabins_, clock_, half_life_ns_,
                                [](const CabinQualitySignal& s) { return s.lookups; }));
    }
    return &cabins_[cabin_id];
}

void OptimizerSignals::NoteExecution(std::uint64_t pattern_id, std::uint64_t pages_fetched,
                                     CandidateRef candidate) {
    FingerprintSignal* signal = FingerprintFor(pattern_id);
    if (candidate.valid()) signal->candidate = candidate;
    Touch(signal->executions, clock_, half_life_ns_);
    // Clamped into Accumulate's point width; a statement that read four
    // billion pages has problems this signal is not among.
    const std::uint32_t points =
        pages_fetched > std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(pages_fetched);
    Accumulate(signal->pages, clock_, half_life_ns_, points);
}

void OptimizerSignals::NoteCabinLookup(std::uint64_t cabin_id, bool served) {
    CabinQualitySignal* signal = CabinFor(cabin_id);
    Touch(signal->lookups, clock_, half_life_ns_);
    if (!served) Touch(signal->coverage_misses, clock_, half_life_ns_);
}

void OptimizerSignals::NoteCabinHint(std::uint64_t cabin_id, bool ok) {
    if (ok) return;  // only failures move the quality signal
    Touch(CabinFor(cabin_id)->hint_failures, clock_, half_life_ns_);
}

SnapshotCabin OptimizerSignals::QualityOf(std::uint64_t cabin_id) const {
    SnapshotCabin out;
    out.cabin_id = cabin_id;
    auto found = cabins_.find(cabin_id);
    if (found == cabins_.end()) return out;
    out.lookups_q8 = ValueAt(found->second.lookups, clock_, half_life_ns_);
    out.hint_failures_q8 = ValueAt(found->second.hint_failures, clock_, half_life_ns_);
    out.coverage_misses_q8 = ValueAt(found->second.coverage_misses, clock_, half_life_ns_);
    return out;
}

OptimizerSnapshot OptimizerSignals::Snapshot() {
    OptimizerSnapshot snapshot;
    snapshot.version = ++version_;
    snapshot.decay_epoch = clock_ != nullptr ? clock_->Now() : 0;

    snapshot.fingerprints.reserve(fingerprints_.size());
    for (const auto& [id, signal] : fingerprints_) {
        SnapshotFingerprint out;
        out.pattern_id = id;
        out.frequency_q8 = ValueAt(signal.executions, clock_, half_life_ns_);
        out.pages_q8 = ValueAt(signal.pages, clock_, half_life_ns_);
        // The same reads in the log domain, for the consumer that must
        // still order two shapes after both have decayed past Q24.8's
        // floor. Same state, same instant - only the projection differs.
        out.frequency_log2 = Log2ValueAt(signal.executions, clock_, half_life_ns_);
        out.pages_log2 = Log2ValueAt(signal.pages, clock_, half_life_ns_);
        out.candidate = signal.candidate;
        snapshot.fingerprints.push_back(out);
    }
    snapshot.cabins.reserve(cabins_.size());
    for (const auto& [id, signal] : cabins_) {
        SnapshotCabin out;
        out.cabin_id = id;
        out.lookups_q8 = ValueAt(signal.lookups, clock_, half_life_ns_);
        out.hint_failures_q8 = ValueAt(signal.hint_failures, clock_, half_life_ns_);
        out.coverage_misses_q8 = ValueAt(signal.coverage_misses, clock_, half_life_ns_);
        snapshot.cabins.push_back(out);
    }

    // Sorted so identical states produce byte-identical snapshots - the
    // determinism PHY02's golden-trace tests will stand on. The map's
    // iteration order is not part of the state.
    std::sort(snapshot.fingerprints.begin(), snapshot.fingerprints.end(),
              [](const SnapshotFingerprint& a, const SnapshotFingerprint& b) {
                  return a.pattern_id < b.pattern_id;
              });
    std::sort(snapshot.cabins.begin(), snapshot.cabins.end(),
              [](const SnapshotCabin& a, const SnapshotCabin& b) {
                  return a.cabin_id < b.cabin_id;
              });
    return snapshot;
}

}  // namespace kds::stats
