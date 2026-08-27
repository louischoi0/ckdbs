#pragma once

#include <cstdint>
#include <unordered_map>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/trail_collector.hpp"
#include "kds/sched/clock.hpp"
#include "kds/stats/instance_key.hpp"
#include "kds/storage/page_store.hpp"

// The trail recorder: what decides that a pattern instance is worth
// remembering, and writes its trail (docs/inflight/in-progress/waystone-workplan.md P09/P10).
//
// One call from the statement path, `OnPatternResult()`, after the
// statement has **succeeded**. Everything else - the sighting count, the
// catalog row, the directory, the page - is behind it.
//
// ---- Recording is not free, so it is not unconditional ------------------
//
// `n = 2` for an auto-observed pattern (spec section 9, decided
// 2026-08-01): the first execution of an instance only *counts*, the second
// records. Recording on first sight pays a page write for every one-shot
// query a client ever sends; waiting longer misses short-lived hot
// instances. Two is the smallest n that excludes the one-shot case.
//
// `n = 1` for a **user-declared** pattern
// (docs/spec/create-pattern-user-defined-patterns-v1.md section 7): a
// declaration *is* the evidence n=2 waits for. An operator who wrote
// `CREATE PATTERN` has already said this shape repeats, and making them
// prove it again with traffic is asking a question that was answered.
//
// ---- What this deliberately does not do ---------------------------------
//
// **Nothing reads a trail.** Replay is P11. Until it lands this class is
// pure write side: it cannot change a query's result, only spend some
// pages. That is what makes "results are identical with recording on and
// off" checkable now rather than asserted.
//
// **It never fails a statement.** Every path returns void. A trail that
// cannot be written is a trail that does not exist, which is the state
// every instance starts in - so a full disk, a corrupt directory or a
// registration race costs a replay and never an answer (invariant 8). The
// failures are counted (`Stats`) so "recording silently does nothing" is
// visible rather than assumed.
//
// Concurrency: core-local, no synchronization (rules.md #3). One recorder
// per core; the sighting table below is that core's own.

namespace kds::stats {

class TrailRecorder {
public:
    // Sightings held before the table is cleared.
    //
    // `[PROPOSED]` - spec section 9 leaves this open. 4096 instances at 24
    // bytes an entry is ~100 KB, which is small against a page cache and
    // large against the number of *hot* instances an application really
    // has; the population this must hold is "instances seen at least once
    // and not yet twice", which is bounded by burst width rather than by
    // total distinct instances.
    //
    // **Overflow clears the whole table**, which merely restarts counting -
    // an instance loses its first sighting and needs one more execution to
    // record. Spec section 9 is explicit that eviction here is a
    // performance event and never a correctness one, which is why the
    // crudest possible policy is the right one until something measures
    // otherwise.
    static constexpr std::size_t kMaxSightings = 4096;

    // Executions an auto-observed instance must reach before its trail is
    // written. A declared pattern's instances record at 1.
    static constexpr std::uint8_t kAutoRecordThreshold = 2;

    struct Stats {
        std::uint64_t sightings = 0;        // instances observed
        std::uint64_t trails_written = 0;   // trails actually recorded
        std::uint64_t patterns_registered = 0;  // auto-registrations performed
        std::uint64_t skipped_overflow = 0; // collector overflowed; nothing written
        std::uint64_t write_failures = 0;   // a write that could not be made
        std::uint64_t sighting_table_clears = 0;
    };

    // `clock` supplies the trail's `recorded_ts` and may be null, in which
    // case trails are stamped 0. Best-effort by contract (waystone.hpp),
    // and retention is the only consumer, so a missing clock costs ordering
    // among trails and nothing else. Taken as an injected interface rather
    // than read directly, per rules.md #4.
    TrailRecorder(catalog::Catalog& catalog, storage::PageStore& store,
                  const sched::Clock* clock = nullptr) noexcept
        : catalog_(catalog), store_(store), clock_(clock) {}

    // The one thing the statement path calls, and **only after the
    // statement succeeded**. A trail from a statement that errored
    // describes a state no reader should be pointed at (workplan P10).
    //
    // `key` is the instance the statement executed as; `trail` is what the
    // executor collected. `stmt_class` is the compiled chain's class as the
    // catalog stores it (`exec::StoredStatementClass`), needed only if this
    // shape has to be registered - an already-registered pattern keeps the
    // class it was registered with. Never fails: see the class comment.
    void OnPatternResult(const InstanceKey& key, const exec::TrailCollector& trail,
                         std::uint8_t stmt_class);

    const Stats& stats() const noexcept { return stats_; }

    // Whether an instance would record *right now*, without observing it.
    // Exposed for tests and for a future EXPLAIN-style surface; the policy
    // lives in one place and this is it.
    bool WouldRecord(std::uint8_t sightings, std::uint8_t origin) const noexcept;

private:
    // Bumps `key`'s sighting count and returns the new value, clearing the
    // table wholesale if it is full.
    std::uint8_t Observe(const InstanceKey& key);

    // The pattern's row, registering it if this is the first time the shape
    // has been seen. Returns nullopt when the row could not be obtained,
    // which is a reason not to record and never an error to report upward.
    const catalog::PatternAccess* EnsurePattern(std::uint64_t pattern_id,
                                                std::uint8_t stmt_class);

    // The pattern's directory pair, creating the directory on first use.
    // An auto-registered pattern starts with `dir_depth == 0`.
    StatusOr<std::pair<PageId, std::uint8_t>> EnsureDirectory(
        const catalog::PatternAccess& pattern);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    const sched::Clock* clock_;

    std::unordered_map<InstanceKey, std::uint8_t, InstanceKeyHash> sightings_;
    Stats stats_;
};

}  // namespace kds::stats
