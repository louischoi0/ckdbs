#include "kds/stats/optimizer_signals.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace kds::stats {

namespace {

// A note's home slot. The ids are hashes already (a fingerprint) or dense
// (a cabin id); the multiply spreads the dense ones.
std::size_t HomeOf(std::uint64_t id, std::size_t slots) noexcept {
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> 32) % slots;
}

// The entry for `id` in a part's table of `slots`, claimed where the id is
// new and fewer than `bound` are used - half the slots, so the walk meets an
// empty one. Null past the bound.
template <class Note>
Note* Claim(Note* table, std::size_t slots, std::size_t bound, std::size_t& used,
            std::uint64_t id) noexcept {
    for (std::size_t i = HomeOf(id, slots), n = 0; n < slots; i = i + 1 == slots ? 0 : i + 1, ++n) {
        Note& note = table[i];
        if (note.used) {
            if (note.id == id) return &note;
            continue;
        }
        if (used >= bound) return nullptr;
        note = Note{};
        note.id = id;
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
    MergeDecay(into.wait_samples, from.wait_samples, half_life);
    if (from.candidate.valid()) into.candidate = from.candidate;
}

void MergeInto(CabinQualitySignal& into, const CabinQualitySignal& from,
               sched::MonoTimeNs half_life) noexcept {
    MergeDecay(into.lookups, from.lookups, half_life);
    MergeDecay(into.hint_failures, from.hint_failures, half_life);
    MergeDecay(into.coverage_misses, from.coverage_misses, half_life);
}

// Room in a bounded map for `incoming` new keys: the coldest entries, by
// decayed primary score at `now`, erased **in one pass** - a scan a fold's
// part, never a scan a newcomer, since the fold runs on a reactor. Restarting
// a victim's count is a performance event; refusing the newcomers would
// starve every pattern that turns hot after the table filled, which is
// exactly the pattern a heat tracker exists to notice.
//
// **Ranked in the log domain**, and that is a correctness point rather
// than a refinement: a full table is mostly *idle* entries, and Q24.8
// reads every score older than ~16 half-lives as exactly 0. Comparing
// those finds a tie among all of them, so the victim would be arbitrary
// among the cold, and a fingerprint idle for an hour could outlive one idle
// for a week. The log-domain read keeps them ordered however long they
// idle (decay.hpp). A tie that remains goes to the lower key, so the
// victims do not depend on the map's iteration order.
template <typename Map, typename Primary>
void MakeRoom(Map& map, std::size_t cap, std::size_t incoming, const sched::Clock* clock,
              sched::MonoTimeNs half_life, Primary primary,
              std::vector<std::pair<Log2Q16, std::uint64_t>>& scratch) {
    if (map.size() + incoming <= cap) return;
    const std::size_t drop = std::min(map.size(), map.size() + incoming - cap);
    // One clock reading for the whole ranking.
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
    for (std::size_t i = 0; i < drop; ++i) map.erase(scratch[i].second);
}

}  // namespace

OptimizerSignals::OptimizerSignals(const sched::Clock* clock, sched::MonoTimeNs half_life_ns,
                                   std::uint32_t cores)
    : clock_(clock), half_life_ns_(half_life_ns), armed_(cores > 1) {
    const std::uint32_t parts = std::max<std::uint32_t>(cores, 1);
    parts_.reserve(parts);
    for (std::uint32_t c = 0; c < parts; ++c) parts_.push_back(std::make_unique<Part>());
    taken_shapes_.reserve(kNotedShapesPerCore);
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
    if (!part.shapes) {
        // The one allocation, at the part's first note, and never retried
        // once it failed: a note must not try an allocation every time.
        if (!part.failed) {
            part.shapes.reset(new (std::nothrow) ShapeNote[kShapeSlots]());
            part.cabins.reset(new (std::nothrow) CabinNote[kCabinSlots]());
        }
        if (!part.shapes || !part.cabins) {
            part.failed = true;
            part.shapes.reset();
            part.cabins.reset();
            part.full_drops.fetch_add(1, std::memory_order_relaxed);
            hold.reset();
            return nullptr;
        }
    }
    return &part;
}

OptimizerSignals::ShapeNote* OptimizerSignals::ShapeFor(Part& part,
                                                        std::uint64_t pattern_id) noexcept {
    ShapeNote* note = Claim(part.shapes.get(), kShapeSlots, kNotedShapesPerCore, part.shapes_used,
                            pattern_id);
    if (note == nullptr) part.full_drops.fetch_add(1, std::memory_order_relaxed);
    return note;
}

OptimizerSignals::CabinNote* OptimizerSignals::CabinNoteFor(Part& part,
                                                            std::uint64_t cabin_id) noexcept {
    CabinNote* note = Claim(part.cabins.get(), kCabinSlots, kNotedCabinsPerCore, part.cabins_used,
                            cabin_id);
    if (note == nullptr) part.full_drops.fetch_add(1, std::memory_order_relaxed);
    return note;
}

void OptimizerSignals::NoteExecution(std::uint32_t core, std::uint64_t pattern_id,
                                     std::uint64_t pages_fetched,
                                     CandidateRef candidate) noexcept {
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    ShapeNote* note = ShapeFor(*part, pattern_id);
    if (note == nullptr) return;
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
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    if (ShapeNote* note = ShapeFor(*part, pattern_id); note != nullptr) {
        Touch(note->signal.wait_samples, clock_, half_life_ns_);
    }
}

void OptimizerSignals::NoteCabinLookup(std::uint32_t core, std::uint64_t cabin_id,
                                       bool served) noexcept {
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    CabinNote* note = CabinNoteFor(*part, cabin_id);
    if (note == nullptr) return;
    Touch(note->signal.lookups, clock_, half_life_ns_);
    if (!served) Touch(note->signal.coverage_misses, clock_, half_life_ns_);
}

void OptimizerSignals::NoteCabinHint(std::uint32_t core, std::uint64_t cabin_id,
                                     bool ok) noexcept {
    if (ok) return;  // only failures move the quality signal
    if (!collecting()) return;
    std::optional<LatchGuard> hold;
    Part* part = Enter(core, hold);
    if (part == nullptr) return;
    if (CabinNote* note = CabinNoteFor(*part, cabin_id); note != nullptr) {
        Touch(note->signal.hint_failures, clock_, half_life_ns_);
    }
}

void OptimizerSignals::Fold() {
    for (const std::unique_ptr<Part>& part_ptr : parts_) {
        Part& part = *part_ptr;
        taken_shapes_.clear();
        taken_cabins_.clear();
        {
            // A bounded copy into memory reserved at construction, then the
            // part is let go: a core's note meets this hold or none. Tried
            // too - the fold runs on a reactor (DL-Q17) - and the only other
            // holder is that core's note, a few words long; a part found
            // busy keeps its notes for the next fold.
            const LatchGuard hold(Armed(part), std::try_to_lock);
            if (!hold.owns() || !part.shapes) continue;
            TakeAll(part.shapes.get(), kShapeSlots, part.shapes_used, taken_shapes_);
            TakeAll(part.cabins.get(), kCabinSlots, part.cabins_used, taken_cabins_);
        }
        // In id order, so what the fold merges and evicts does not depend on
        // where a part's hash put an entry (DL-R5).
        const auto by_id = [](const auto& a, const auto& b) { return a.id < b.id; };
        std::sort(taken_shapes_.begin(), taken_shapes_.end(), by_id);
        std::sort(taken_cabins_.begin(), taken_cabins_.end(), by_id);

        // An execution admits a shape; wait samples alone never do. A part's
        // pair that was executed was touched, and a touch leaves a score of
        // at least one point that nothing decays in place, so a zero score
        // is "no execution since the last fold".
        const auto executed = [](const ShapeNote& note) {
            return note.signal.executions.scaled != 0;
        };
        std::size_t fresh = 0;
        for (const ShapeNote& note : taken_shapes_) {
            if (executed(note) && !fingerprints_.contains(note.id)) ++fresh;
        }
        MakeRoom(fingerprints_, kMaxTrackedFingerprints, fresh, clock_, half_life_ns_,
                 [](const FingerprintSignal& s) -> const DecayState& { return s.executions; },
                 evict_scratch_);
        for (const ShapeNote& note : taken_shapes_) {
            if (executed(note)) {
                MergeInto(fingerprints_[note.id], note.signal, half_life_ns_);
            } else if (const auto found = fingerprints_.find(note.id);
                       found != fingerprints_.end()) {
                MergeInto(found->second, note.signal, half_life_ns_);
            }
        }

        fresh = 0;
        for (const CabinNote& note : taken_cabins_) {
            if (!cabins_.contains(note.id)) ++fresh;
        }
        MakeRoom(cabins_, kMaxTrackedCabins, fresh, clock_, half_life_ns_,
                 [](const CabinQualitySignal& s) -> const DecayState& { return s.lookups; },
                 evict_scratch_);
        for (const CabinNote& note : taken_cabins_) {
            MergeInto(cabins_[note.id], note.signal, half_life_ns_);
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
    for (const auto& [id, signal] : fingerprints_) {
        SnapshotFingerprint out;
        out.pattern_id = id;
        out.frequency_q8 = ValueAt(signal.executions, clock_, half_life_ns_);
        out.pages_q8 = ValueAt(signal.pages, clock_, half_life_ns_);
        out.wait_samples_q8 = ValueAt(signal.wait_samples, clock_, half_life_ns_);
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
