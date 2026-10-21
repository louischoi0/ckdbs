#include "kds/stats/trail_recorder.hpp"

#include "kds/stats/trail_store.hpp"
#include "kds/stats/waystone_dir.hpp"

namespace kds::stats {

bool TrailRecorder::WouldRecord(std::uint8_t sightings) const noexcept {
    // An instance waits for its second sighting. The first execution only
    // counts - which is what keeps a one-shot query from paying for a page
    // write it will never read back (waystone-concpets.md section 9). One
    // threshold for every opted pattern (CF-O-Q8 (a)).
    return sightings >= kRecordThreshold;
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

const catalog::PatternAccess* TrailRecorder::EnsurePattern(std::uint64_t fetch_id) {
    // **A lookup, never a registration** (CF-S1b). A shape is recorded only
    // once an operator opted it in, and `WAYSTONE ON` is the one writer of
    // the row, so a `SELECT` writes no catalog page and appends nothing to
    // the log. Served from the catalog cache after the first ask; the opt-in
    // moves the schema word, so a cached answer is at most one statement
    // old.
    auto found = catalog_.FindPattern(fetch_id);
    if (!found.ok()) {
        // NotFound is "never opted", which is silent. Any other answer is a
        // row that could not be read, and an opted pattern behind it would
        // record nothing unseen, so it is counted.
        if (found.status().code() != StatusCode::kNotFound) ++stats_.write_failures;
        return nullptr;
    }
    if ((found.value()->flags & catalog::kPatternWaystone) == 0) return nullptr;
    return found.value();
}

StatusOr<std::pair<PageId, std::uint8_t>> TrailRecorder::EnsureDirectory(
    const catalog::PatternAccess& pattern) {
    // The directory `WAYSTONE ON` or the mount's renewal made. **None is
    // ever made here**: a run-time claim would put a logged catalog write
    // on the `SELECT` path, and two cores claiming one row raced and leaked
    // the loser's page. An opted row with no root waits for the next mount.
    if (!pattern.has_waystone_directory()) {
        return Status::NotFound("waystone: the opted pattern has no directory this run");
    }
    return std::make_pair(pattern.waystone_root, pattern.dir_depth);
}

void TrailRecorder::OnPatternResult(const InstanceKey& key, const exec::TrailCollector& trail) {
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

    // The threshold is asked **before** the catalog, which is the whole
    // saving: a shape on its first sighting - every one-shot query in the
    // database - answers from the sighting table alone and never touches a
    // catalog page. This read the pattern row first while a declared
    // pattern could lower the threshold to 1; withdrawing declared patterns
    // took that dependence away, and with it a lookup EnsurePattern() was
    // about to repeat.
    if (!WouldRecord(seen)) return;

    // Not opted: sighted, and nothing more. Not a failure - it is what
    // every shape is until an operator says otherwise. A row that could not
    // be read is counted inside EnsurePattern().
    const catalog::PatternAccess* pattern = EnsurePattern(key.fetch_id);
    if (pattern == nullptr) return;

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
}

}  // namespace kds::stats
