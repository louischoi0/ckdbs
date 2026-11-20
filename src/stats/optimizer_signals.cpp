#include "kds/stats/optimizer_signals.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace kds::stats {

namespace {

// A note's home slot. A pattern id is a hash already and a cabin id dense;
// the multiply spreads the dense ones.
std::size_t HomeOf(std::uint64_t id, std::size_t slots) noexcept {
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 32) % slots;
}
std::size_t HomeOf(const SignalKey& key, std::size_t slots) noexcept {
    return SignalKeyHash{}(key) % slots;
}

// The entry for `key` in a part's table of `slots`, claimed where the key is
// new and fewer than `bound` are used - half the slots, so the walk meets an
// empty one. Null past the bound.
template <class Note, class Key>
Note* Claim(Note* table, std::size_t slots, std::size_t bound, std::size_t& used,
            const Key& key) noexcept {
    for (std::size_t i = HomeOf(key, slots), n = 0; n < slots; i = i + 1 == slots ? 0 : i + 1, ++n) {
        Note& note = table[i];
        if (note.used) {
            if (note.key == key) return &note;
            continue;
        }
        if (used >= bound) return nullptr;
        note = Note{};
        note.key = key;
        note.used = true;
        ++used;
        return &note;
    }
    return nullptr;
}

// Every used entry of a part's table copied into `out`, whose room the
// caller reserved, and the table left empty.
template <class Note>
void TakeAll(Note* table, std::size_t slots, std::size_t& used, std::vector<Note>& out) {
    if (used == 0) return;
    for (std::size_t i = 0; i < slots; ++i) {
        Note& note = table[i];
        if (!note.used) continue;
        out.push_back(note);
        note.used = false;
    }
    used = 0;
}

void MergeInto(FingerprintSignal& into, const FingerprintSignal& from,
               sched::MonoTimeNs half_life) noexcept {
    MergeDecay(into.executions, from.executions, half_life);
    MergeDecay(into.pages, from.pages, half_life);
    // The key's candidate, with the Cabin serving it as last noted.
    if (from.candidate.valid()) into.candidate = from.candidate;
}

void MergeInto(CabinQualitySignal& into, const CabinQualitySignal& from,
               sched::MonoTimeNs half_life) noexcept {
    MergeDecay(into.lookups, from.lookups, half_life);
    MergeDecay(into.hint_failures, from.hint_failures, half_life);
    MergeDecay(into.coverage_misses, from.coverage_misses, half_life);
}

// Room in a bounded map for `incoming` new keys: the coldest entries, by
// decayed primary score at one reading of the clock, erased **in one pass** -
// a scan a fold's part, never a scan a newcomer, since the fold runs on a
// reactor. `evicted(key)` is told each victim. Restarting a victim's count
// is a performance event; refusing the newcomers would starve every pattern
// that turns hot after the table filled, which is exactly the pattern a
// heat tracker exists to notice.
//
// **Ranked in the log domain**, and that is a correctness point rather
// than a refinement: a full table is mostly *idle* entries, and Q24.8
// reads every score older than ~16 half-lives as exactly 0. Comparing
// those finds a tie among all of them, so the victim would be arbitrary
// among the cold, and a signal idle for an hour could outlive one idle for
// a week. The log-domain read keeps them ordered however long they idle
// (decay.hpp). A tie that remains goes to the lower key, so the victims do
// not depend on the map's iteration order.
template <typename Map, typename Primary, typename Evicted>
void MakeRoom(Map& map, std::size_t cap, std::size_t incoming, const sched::Clock* clock,
              sched::MonoTimeNs half_life, Primary primary,
              std::vector<std::pair<Log2Q16, typename Map::key_type>>& scratch,
              Evicted evicted) {
    if (map.size() + incoming <= cap) return;
    const std::size_t drop = std::min(map.size(), map.size() + incoming - cap);
    const bool timed = clock != nullptr;
    const sched::MonoTimeNs now = timed ? clock->Now() : 0;
    scratch.clear();
    scratch.reserve(map.size());
    for (const auto& [key, signal] : map) {
        const DecayState& score = primary(signal);
        scratch.emplace_back(timed ? DecayedLog2At(score, now, half_life) : Log2OfQ16(score.scaled),
                             key);
    }
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(drop),
                     scratch.end());
    for (std::size_t i = 0; i < drop; ++i) {
        map.erase(scratch[i].second);
        evicted(scratch[i].second);
    }
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

OptimizerSignals::OptimizerSignals(const sched::Clock* clock, sched::MonoTimeNs half_life_ns,
                                   std::uint32_t cores)
    : clock_(clock), half_life_ns_(half_life_ns), armed_(cores > 1) {
    const std::uint32_t parts = std::max<std::uint32_t>(cores, 1);
    parts_.reserve(parts);
    for (std::uint32_t c = 0; c < parts; ++c) parts_.push_back(std::make_unique<Part>());
    taken_signals_.reserve(kNotedSignalsPerCore);
    taken_waits_.reserve(kNotedWaitsPerCore);
    taken_cabins_.reserve(kNotedCabinsPerCore);
}

OptimizerSignals::~OptimizerSignals() = default;

OptimizerSignals::Part* OptimizerSignals::Enter(std::uint32_t core,
                                                std::optional<LatchGuard>& hold) noexcept {
    if (core >= parts_.size()) {
        stray_drops_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    Part& part = *parts_[core];
    // **Tried, never waited for** (`rules.md` §3a (b)): the one other taker
    // is the fold's copy, and a note that meets it is the count's loss.
    hold.emplace(Armed(part), std::try_to_lock);
    if (!hold->owns()) {
        hold.reset();
        part.busy_drops.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (!part.signals) {
        // The one allocation, at the part's first note, and never retried
        // once it failed: a note must not try an allocation every time.
        if (!part.failed) {
            part.signals.reset(new (std::nothrow) SignalNote[kSignalSlots]());
            part.waits.reset(new (std::nothrow) WaitNote[kWaitSlots]());
            part.cabins.reset(new (std::nothrow) CabinNote[kCabinSlots]());
        }
        if (!part.signals || !part.waits || !part.cabins) {
            part.failed = true;
            part.signals.reset();
            part.waits.reset();
            part.cabins.reset();
            part.full_drops.fetch_add(1, std::memory_order_relaxed);
            hold.reset();
            return nullptr;
        }
    }
    return &part;
}

void OptimizerSignals::NoteExecution(std::uint32_t core, std::uint64_t pattern_id,
                                     std::uint64_t pages_fetched,
                                     CandidateRef candidate) noexcept {
    const StatisticsRecordScope recording("the Cabin controller's execution signal");
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    // The pattern and what it targeted (the header, DI-S2): an execution
    // with no candidacy is the pattern's `{0, 0}` signal.
    const SignalKey key = candidate.valid()
                              ? SignalKey{pattern_id, candidate.rel_oid, candidate.col_pos}
                              : SignalKey{pattern_id, 0, 0};
    SignalNote* note =
        Claim(part->signals.get(), kSignalSlots, kNotedSignalsPerCore, part->signals_used, key);
    if (note == nullptr) {
        part->full_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The key's candidate, with the Cabin serving it now.
    if (candidate.valid()) note->signal.candidate = candidate;
    Touch(note->signal.executions, clock_, half_life_ns_);
    // Clamped into Accumulate's point width; a statement that read four
    // billion pages has problems this signal is not among.
    const std::uint32_t points =
        pages_fetched > std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(pages_fetched);
    Accumulate(note->signal.pages, clock_, half_life_ns_, points);
}

void OptimizerSignals::NoteWaitSample(std::uint32_t core, std::uint64_t pattern_id) noexcept {
    const StatisticsRecordScope recording("the Cabin controller's wait signal");
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    WaitNote* note =
        Claim(part->waits.get(), kWaitSlots, kNotedWaitsPerCore, part->waits_used, pattern_id);
    if (note == nullptr) {
        part->full_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Touch(note->signal, clock_, half_life_ns_);
}

void OptimizerSignals::NoteCabinLookup(std::uint32_t core, std::uint64_t cabin_id,
                                       bool served) noexcept {
    const StatisticsRecordScope recording("the Cabin controller's lookup signal");
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    CabinNote* note =
        Claim(part->cabins.get(), kCabinSlots, kNotedCabinsPerCore, part->cabins_used, cabin_id);
    if (note == nullptr) {
        part->full_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Touch(note->signal.lookups, clock_, half_life_ns_);
    if (!served) Touch(note->signal.coverage_misses, clock_, half_life_ns_);
}

void OptimizerSignals::NoteCabinHint(std::uint32_t core, std::uint64_t cabin_id,
                                     bool ok) noexcept {
    const StatisticsRecordScope recording("the Cabin controller's hint signal");
    if (ok) return;  // only failures move the quality signal
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    CabinNote* note =
        Claim(part->cabins.get(), kCabinSlots, kNotedCabinsPerCore, part->cabins_used, cabin_id);
    if (note == nullptr) {
        part->full_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Touch(note->signal.hint_failures, clock_, half_life_ns_);
}

void OptimizerSignals::Fold() {
    // A pattern's last signal gone, its waits go with it (DI-S2).
    const auto release = [this](std::uint64_t pattern_id) {
        if (auto waits = patterns_.find(pattern_id);
            waits != patterns_.end() && --waits->second.signals == 0) {
            patterns_.erase(waits);
        }
    };
    for (const std::unique_ptr<Part>& part_ptr : parts_) {
        Part& part = *part_ptr;
        taken_signals_.clear();
        taken_waits_.clear();
        taken_cabins_.clear();
        {
            // A bounded copy into memory reserved at construction, then the
            // part is let go: a core's note meets this hold or none. Tried
            // too - the fold runs on a reactor (DL-Q17) - and the only other
            // holder is that core's note, a few words long; a part found
            // busy keeps its notes for the next fold.
            const LatchGuard hold(Armed(part), std::try_to_lock);
            if (!hold.owns() || !part.signals) continue;
            TakeAll(part.signals.get(), kSignalSlots, part.signals_used, taken_signals_);
            TakeAll(part.waits.get(), kWaitSlots, part.waits_used, taken_waits_);
            TakeAll(part.cabins.get(), kCabinSlots, part.cabins_used, taken_cabins_);
        }
        // In key order, so what the fold merges and evicts does not depend
        // on where a part's hash put an entry (DL-R5).
        const auto by_key = [](const auto& a, const auto& b) { return a.key < b.key; };
        std::sort(taken_signals_.begin(), taken_signals_.end(), by_key);
        std::sort(taken_waits_.begin(), taken_waits_.end(), by_key);
        std::sort(taken_cabins_.begin(), taken_cabins_.end(), by_key);

        // **Each newcomer is counted to its pattern before a victim is
        // chosen** (DI-S2): a victim of the same pattern then leaves the
        // pattern's entry, and the waits in it, standing for the signal
        // about to be made.
        std::size_t fresh = 0;
        for (const SignalNote& note : taken_signals_) {
            if (fingerprints_.contains(note.key)) continue;
            ++patterns_[note.key.pattern_id].signals;
            ++fresh;
        }
        MakeRoom(fingerprints_, kMaxTrackedFingerprints, fresh, clock_, half_life_ns_,
                 [](const FingerprintSignal& s) -> const DecayState& { return s.executions; },
                 evict_signals_,
                 [&](const SignalKey& victim) { release(victim.pattern_id); });
        for (std::size_t at = 0; at < taken_signals_.size(); ++at) {
            const SignalNote& note = taken_signals_[at];
            try {
                MergeInto(fingerprints_[note.key], note.signal, half_life_ns_);
            } catch (...) {
                // The signal was not made, nor are the newcomers after it:
                // their patterns are not left counting them.
                for (std::size_t rest = at; rest < taken_signals_.size(); ++rest) {
                    if (!fingerprints_.contains(taken_signals_[rest].key)) {
                        release(taken_signals_[rest].key.pattern_id);
                    }
                }
                throw;
            }
        }
        // A wait sample is its pattern's, kept beside the pattern's signals
        // and counted only while it has one: a wait alone admits nothing.
        for (const WaitNote& note : taken_waits_) {
            if (const auto found = patterns_.find(note.key); found != patterns_.end()) {
                MergeDecay(found->second.wait_samples, note.signal, half_life_ns_);
            }
        }

        fresh = 0;
        for (const CabinNote& note : taken_cabins_) {
            if (!cabins_.contains(note.key)) ++fresh;
        }
        MakeRoom(cabins_, kMaxTrackedCabins, fresh, clock_, half_life_ns_,
                 [](const CabinQualitySignal& s) -> const DecayState& { return s.lookups; },
                 evict_cabins_, [](std::uint64_t) {});
        for (const CabinNote& note : taken_cabins_) {
            MergeInto(cabins_[note.key], note.signal, half_life_ns_);
        }
    }
}

OptimizerSignals::Drops OptimizerSignals::drops() const noexcept {
    Drops total;
    total.full = stray_drops_.load(std::memory_order_relaxed);
    for (const std::unique_ptr<Part>& part : parts_) {
        total.busy += part->busy_drops.load(std::memory_order_relaxed);
        total.full += part->full_drops.load(std::memory_order_relaxed);
    }
    return total;
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
    Fold();
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
