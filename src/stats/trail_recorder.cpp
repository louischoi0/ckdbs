#include "kds/stats/trail_recorder.hpp"

#include "kds/stats/trail_store.hpp"
#include "kds/stats/waystone_dir.hpp"

namespace kds::stats {

bool TrailRecorder::WouldRecord(std::uint8_t sightings, std::uint8_t origin) const noexcept {
    // A declared pattern records from its first execution: the declaration
    // is the evidence n=2 exists to gather, so gathering it again is asking
    // a question the operator already answered
    // (create-pattern-user-defined-patterns-v1.md section 7).
    if (origin == catalog::kOriginUser) return sightings >= 1;
    // An observed one waits for the second sighting. The first execution
    // only counts - which is what keeps a one-shot query from paying for a
    // page write it will never read back (waystone-concpets.md section 9).
    return sightings >= kAutoRecordThreshold;
}

std::uint8_t TrailRecorder::Observe(const InstanceKey& key) {
    if (sightings_.size() >= kMaxSightings && sightings_.find(key) == sightings_.end()) {
        // Cleared wholesale rather than evicted one at a time. Every
        // instance loses its accumulated count and needs one more execution
        // to record, which is a performance event and never a correctness
        // one - the trail that does not get written is one nothing was
        // going to read yet. A real eviction policy here would be inventing
        // one for a population nobody has measured.
        sightings_.clear();
        ++stats_.sighting_table_clears;
    }

    std::uint8_t& count = sightings_[key];
    // Saturating: the policy only ever asks "is it at least n", so a count
    // that wrapped to 0 would silently un-record a hot instance.
    if (count != 0xFF) ++count;
    ++stats_.sightings;
    return count;
}

const catalog::PatternAccess* TrailRecorder::EnsurePattern(std::uint64_t pattern_id,
                                                           std::uint8_t stmt_class) {
    if (auto found = catalog_.FindPattern(pattern_id); found.ok()) return found.value();

    // Not registered - so this shape has now been seen enough times to be
    // worth a catalog row.
    //
    // **Registered here, not on first sight.** A shape whose instances are
    // each executed once never reaches this line, so a database full of
    // one-shot queries accumulates no pattern rows at all. That reading of
    // n=2 costs nothing (an unregistered pattern has no trail to lose) and
    // keeps sys.patterns a record of what actually repeats.
    //
    // Safe on the statement path because registration bumps no catalog
    // version (waystone-concpets.md section 4): absences are never cached,
    // so no cached entry claims this pattern is missing, and the
    // `const TableAccess*` the running statement is holding cannot dangle.
    auto registered = catalog_.RegisterPattern(pattern_id, stmt_class, catalog::kOriginAuto,
                                                /*flags=*/0);
    if (!registered.ok()) return nullptr;
    ++stats_.patterns_registered;
    return registered.value();
}

StatusOr<std::pair<PageId, std::uint8_t>> TrailRecorder::EnsureDirectory(
    const catalog::PatternAccess& pattern) {
    if (pattern.has_waystone_directory()) {
        return std::make_pair(pattern.waystone_root, pattern.dir_depth);
    }

    // An auto-registered pattern is created with no directory
    // (`dir_depth == 0`), so the first trail is what pays for one. A
    // declared pattern already has one, pre-sized from
    // `expected_instances`, and never reaches here.
    //
    // Depth 1 addresses 2048 instances before it has to grow, and growing
    // is a cache flush (waystone_dir.hpp) - so starting shallow and letting
    // a pattern that earns it deepen later is the cheap direction to be
    // wrong in.
    auto root = CreateDirPage(store_);
    if (!root.ok()) return root.status();

    // The single writer of the root/depth pair, which validates them
    // together: a root without its depth is unwalkable, and a depth that
    // disagrees sends every walk to the wrong leaf.
    if (Status s = catalog_.SetPatternWaystoneRoot(pattern.pattern_id, root.value(), 1);
        !s.ok()) {
        return s;
    }
    return std::make_pair(root.value(), std::uint8_t{1});
}

void TrailRecorder::OnPatternResult(const InstanceKey& key, const exec::TrailCollector& trail,
                                    std::uint8_t stmt_class) {
    // Nothing to record. A statement whose steps are all searches
    // (invariant 9) collects nothing and is not a candidate for a trail -
    // counted as a sighting all the same, since the instance did execute.
    if (trail.empty()) {
        Observe(key);
        return;
    }

    // The collector filled up, so what it holds is an incomplete account of
    // the execution. Recording it would produce a trail that covers only
    // the first rows with no way for a reader to tell (trail_store.hpp), so
    // the instance keeps no trail at all.
    if (trail.overflowed()) {
        Observe(key);
        ++stats_.skipped_overflow;
        return;
    }

    const std::uint8_t seen = Observe(key);

    // The origin decides the threshold, so the pattern row has to be looked
    // at before the policy can be applied - but only if it already exists.
    // An unregistered shape is auto by definition (nothing has declared
    // it), which lets the common first-execution case answer without
    // touching the catalog at all.
    const catalog::PatternAccess* known = nullptr;
    if (auto found = catalog_.FindPattern(key.pattern_id); found.ok()) known = found.value();
    const std::uint8_t origin = known != nullptr ? known->origin : catalog::kOriginAuto;

    if (!WouldRecord(seen, origin)) return;

    const catalog::PatternAccess* pattern =
        known != nullptr ? known : EnsurePattern(key.pattern_id, stmt_class);
    if (pattern == nullptr) {
        ++stats_.write_failures;
        return;
    }

    auto directory = EnsureDirectory(*pattern);
    if (!directory.ok()) {
        ++stats_.write_failures;
        return;
    }

    const std::uint64_t now = clock_ != nullptr ? static_cast<std::uint64_t>(clock_->Now()) : 0;
    if (Status s = WriteTrail(store_, directory.value().first, directory.value().second, key,
                              trail.entries(), now);
        !s.ok()) {
        // Never propagated. A trail that could not be written is a trail
        // that does not exist, which is where every instance starts and
        // which every consumer already handles - so this costs a future
        // replay and never an answer (invariant 8). Counted so that
        // "recording quietly does nothing" is visible.
        ++stats_.write_failures;
        return;
    }
    ++stats_.trails_written;

    // Heat last, and its failure is ignored for the same reason: these
    // counters rank patterns for retention and nothing reports them, so a
    // dropped bump loses a statistic rather than a trail.
    (void)catalog_.TouchPattern(key.pattern_id, now);
}

}  // namespace kds::stats
