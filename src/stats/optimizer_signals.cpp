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
typename Map::key_type ColdestOf(Map& map, const sched::Clock* clock,
                                 sched::MonoTimeNs half_life, Primary primary) {
    if (map.empty()) return {};
    // Seeded from a real entry rather than from the type's maximum: a table
    // whose every score has saturated at the ceiling would leave a
    // sentinel key nothing matches, the erase would remove nothing, and the
    // cap would quietly stop being a cap.
    typename Map::key_type coldest_key = map.begin()->first;
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

std::size_t SignalKeyHash::operator()(const SignalKey& k) const noexcept {
    // splitmix64's finalizer over the three words: a pattern's signals
    // differ in a small oid and a column position.
    std::uint64_t h = k.pattern_id ^ (k.rel_oid * 0x9E3779B97F4A7C15ULL) ^
                      (static_cast<std::uint64_t>(k.col_pos) << 48);
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
    return static_cast<std::size_t>(h ^ (h >> 31));
}

FingerprintSignal* OptimizerSignals::FingerprintFor(const SignalKey& key) {
    auto found = fingerprints_.find(key);
    if (found != fingerprints_.end()) return &found->second;
    // The newcomer is counted to its pattern **before** a victim is chosen:
    // a victim of the same pattern then leaves the pattern's entry, and the
    // waits in it, standing for the signal about to be made.
    ++patterns_[key.pattern_id].signals;
    if (fingerprints_.size() >= kMaxTrackedFingerprints) {
        // Evict the coldest to admit the newcomer. Restarting the victim's
        // count is a performance event; refusing the newcomer would starve
        // every pattern that turns hot after the table filled, which is
        // exactly the pattern a heat tracker exists to notice.
        const SignalKey victim =
            ColdestOf(fingerprints_, clock_, half_life_ns_,
                      [](const FingerprintSignal& s) { return s.executions; });
        fingerprints_.erase(victim);
        // Its pattern's waits go with the pattern's last signal.
        if (auto waits = patterns_.find(victim.pattern_id);
            waits != patterns_.end() && --waits->second.signals == 0) {
            patterns_.erase(waits);
        }
    }
    try {
        return &fingerprints_[key];
    } catch (...) {
        // The signal was not made: its pattern is not left counting it.
        if (auto waits = patterns_.find(key.pattern_id);
            waits != patterns_.end() && --waits->second.signals == 0) {
            patterns_.erase(waits);
        }
        throw;
    }
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
    LatchGuard hold(latch_);
    // The pattern and what it targeted (`optimizer_signals.hpp`, DI-S2): an
    // execution with no candidacy is the pattern's `{0, 0}` signal.
    FingerprintSignal* signal = FingerprintFor(
        candidate.valid() ? SignalKey{pattern_id, candidate.rel_oid, candidate.col_pos}
                          : SignalKey{pattern_id, 0, 0});
    // The key's candidate, with the Cabin serving it now.
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

void OptimizerSignals::NoteWaitSample(std::uint64_t pattern_id) noexcept {
    if (wait_gate_ != nullptr && !wait_gate_->load(std::memory_order_relaxed)) return;
    LatchGuard hold(latch_);
    const auto found = patterns_.find(pattern_id);
    if (found == patterns_.end()) return;
    Touch(found->second.wait_samples, clock_, half_life_ns_);
}

void OptimizerSignals::NoteCabinLookup(std::uint64_t cabin_id, bool served) {
    LatchGuard hold(latch_);
    CabinQualitySignal* signal = CabinFor(cabin_id);
    Touch(signal->lookups, clock_, half_life_ns_);
    if (!served) Touch(signal->coverage_misses, clock_, half_life_ns_);
}

void OptimizerSignals::NoteCabinHint(std::uint64_t cabin_id, bool ok) {
    if (ok) return;  // only failures move the quality signal
    LatchGuard hold(latch_);
    Touch(CabinFor(cabin_id)->hint_failures, clock_, half_life_ns_);
}

SnapshotCabin OptimizerSignals::QualityOf(std::uint64_t cabin_id) const {
    LatchGuard hold(latch_);
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
    LatchGuard hold(latch_);
    OptimizerSnapshot snapshot;
    snapshot.version = ++version_;
    snapshot.decay_epoch = clock_ != nullptr ? clock_->Now() : 0;

    snapshot.fingerprints.reserve(fingerprints_.size());
    for (const auto& [key, signal] : fingerprints_) {
        SnapshotFingerprint out;
        out.pattern_id = key.pattern_id;
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
    snapshot.waits.reserve(patterns_.size());
    for (const auto& [id, waits] : patterns_) {
        snapshot.waits.push_back({id, ValueAt(waits.wait_samples, clock_, half_life_ns_)});
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
                  if (a.pattern_id != b.pattern_id) return a.pattern_id < b.pattern_id;
                  if (a.candidate.rel_oid != b.candidate.rel_oid) {
                      return a.candidate.rel_oid < b.candidate.rel_oid;
                  }
                  return a.candidate.col_pos < b.candidate.col_pos;
              });
    std::sort(snapshot.waits.begin(), snapshot.waits.end(),
              [](const SnapshotPatternWaits& a, const SnapshotPatternWaits& b) {
                  return a.pattern_id < b.pattern_id;
              });
    std::sort(snapshot.cabins.begin(), snapshot.cabins.end(),
              [](const SnapshotCabin& a, const SnapshotCabin& b) {
                  return a.cabin_id < b.cabin_id;
              });
    return snapshot;
}

}  // namespace kds::stats
