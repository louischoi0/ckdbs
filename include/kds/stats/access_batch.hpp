#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/sched/ring_transport.hpp"

// CR7 — a peer's access statistics, accumulated locally and flushed to core 0.
//
// ---- Why a peer has nowhere to write ------------------------------------
//
// `sys.access_stats` is pinned at `kCatalogPageAccessStats = 11`, inside the
// reserved range, and CC11 makes every page in that range core-0-write-only.
// `Catalog::RecordAccess` runs **per statement**, not per DDL, so a peer
// could not record an access at all - and until CR7 it did not try:
// `CoreRuntime::Open` passes `access_statistics = false` for every core it
// opens. That is `crosscore.md` §6a's *"a peer that records nothing cannot
// feed the mover"*, in the source rather than in prose.
//
// ---- What this is, and what it deliberately is not ----------------------
//
// One message per statement is what CR7 rules out, so a peer folds its
// accesses into this buffer by shape and sends the fold. Two things follow
// that are worth stating because neither is obvious from the type:
//
//   - **A drop is a permitted outcome** (CR8). A full ring drops the batch
//     and counts it, which is the engine-wide *exception* to
//     `sched/send_retry.hpp`'s rule that a message is never dropped: this
//     is invariant 8's advisory class, priced as performance and never as a
//     result, and a retry loop on the statement path would trade a
//     statistic for latency.
//   - **The stored row has no `core_id`, and this buffer's does not reach
//     it.** `SysAccessStatRow` is 33 fixed bytes with no field for one
//     (`catalog/rows.hpp`), so core 0 folds a peer's counts into the
//     existing `(kind, rel_id, column_mask)` shape. The sender is carried on
//     the wire for `SHOW META` and for a log line, not for storage; putting
//     it in the row is an on-disk format change and is not CB's.

namespace kds::stats {

// One folded shape. POD under `ring_message.hpp`'s exception to the on-disk
// rules: it never leaves the process, so no shift/mask encoding is owed.
struct AccessBatchEntry {
    std::uint64_t rel_id = 0;
    std::uint64_t column_mask = 0;
    // Executions this core observed since the last flush. Saturating for
    // `SysAccessStatRow::use_count`'s reason - a wrapped count would make
    // the hottest shape look like the coldest.
    std::uint64_t count = 0;
    std::uint64_t last_seen = 0;
    std::uint8_t kind = 0;
    std::uint8_t reserved[7] = {};
};
static_assert(sizeof(AccessBatchEntry) == 40);

// The wire form: this core's id, then `count` entries.
struct AccessBatchPayload {
    std::uint32_t from_core = 0;
    std::uint32_t count = 0;
};
static_assert(sizeof(AccessBatchPayload) == 8);

// **Derived from the ring slot rather than chosen**, the rule
// `kShippedStatementTextMax` already follows: a batch is one message, so the
// buffer cannot hold more shapes than one message carries, and a slot resize
// moves this rather than silently truncating a flush.
inline constexpr std::size_t kAccessBatchCapacity =
    (sched::kCoreRingPayloadBytes - sizeof(AccessBatchPayload)) / sizeof(AccessBatchEntry);
static_assert(kAccessBatchCapacity >= 16,
              "a batch that holds fewer shapes than a statement touches would flush per "
              "statement, which is exactly what CR7 rules out");

// What `SHOW META` reports, on the core that did the thing being counted: a
// peer fills the first three, core 0 the last two.
struct AccessBatchCounters {
    std::uint64_t batches_sent = 0;
    std::uint64_t entries_sent = 0;
    // CR8's permitted outcome, counted so that it is visible rather than
    // silent. Non-zero means statistics were lost, never that a statement
    // was.
    std::uint64_t batches_dropped = 0;
    std::uint64_t batches_applied = 0;
    std::uint64_t entries_applied = 0;
};

// The accumulator. Core-local, no locks: it is touched by the statement path
// and by the flush, both on this core's reactor thread.
class AccessBatch {
public:
    // Folds one access into the buffer. A shape already present takes the
    // increment; a new one takes a slot while there is one.
    //
    // **Full is not an error and not a flush point.** The buffer is sized to
    // one message and the tick empties it; a shape that arrives with no slot
    // left is dropped under CR8's rule, counted in `overflow_drops()` rather
    // than forcing a send from the statement path.
    void Note(std::uint8_t kind, catalog::Oid rel_id, std::uint64_t column_mask,
              std::uint64_t now) noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            AccessBatchEntry& e = entries_[i];
            if (e.kind != kind || e.rel_id != rel_id || e.column_mask != column_mask) continue;
            if (e.count != UINT64_MAX) ++e.count;
            e.last_seen = now;
            return;
        }
        if (size_ == entries_.size()) {
            ++overflow_drops_;
            return;
        }
        AccessBatchEntry& e = entries_[size_++];
        e.rel_id = rel_id;
        e.column_mask = column_mask;
        e.count = 1;
        e.last_seen = now;
        e.kind = kind;
    }

    bool empty() const noexcept { return size_ == 0; }
    std::span<const AccessBatchEntry> entries() const noexcept {
        return {entries_.data(), size_};
    }
    void Clear() noexcept { size_ = 0; }

    std::uint64_t overflow_drops() const noexcept { return overflow_drops_; }
    AccessBatchCounters& counters() noexcept { return counters_; }
    const AccessBatchCounters& counters() const noexcept { return counters_; }

private:
    std::array<AccessBatchEntry, kAccessBatchCapacity> entries_{};
    std::size_t size_ = 0;
    std::uint64_t overflow_drops_ = 0;
    AccessBatchCounters counters_;
};

}  // namespace kds::stats
