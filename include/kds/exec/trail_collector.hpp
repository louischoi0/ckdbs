#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/common.hpp"

// What the executor tells Waystone about the tuples a statement found
// (docs/inflight/in-progress/waystone-workplan.md P09).
//
// ---- Why this type knows nothing about Waystone -------------------------
//
// `TouchedTuple` is six integers. It is not a `WaystoneEntry`, it carries
// no page format, and nothing here mentions a trail, a directory or a
// pattern. That is the seam spec section 1 asks for - "Waystone lives
// outside the executor" - and it is what keeps the *dependency arrow*
// pointing one way: `stats/` already depends on `exec/`
// (stats/pattern_defs.cpp reads rows through exec/row_codec.hpp), so an
// executor that included a Waystone header would close a cycle.
//
// The conversion to a `WaystoneEntry` happens in stats/trail_store.hpp,
// which is the layer that owns the format.
//
// ---- Collected as it goes, never re-derived -----------------------------
//
// P09's rule, and it is the whole point: the executor records where it
// found a tuple **at the moment it finds it**. Re-deriving the locations
// afterwards would mean looking each tuple up again - which is precisely
// the search a trail exists to avoid, so a recorder that did it would cost
// more than it could ever save.
//
// ---- Droppable by construction ------------------------------------------
//
// A full collector silently stops accepting. It does not grow, does not
// allocate on the statement path after `Reserve`, and does not fail: a
// statement must never error because Waystone ran out of room. The caller
// reads `overflowed()` afterwards and, per the one-page trail cap, records
// nothing at all rather than a partial trail - a truncated trail covers
// only its first rows and no reader can tell that from a complete one.

namespace kds::exec {

// One tuple an execution found, and where.
struct TouchedTuple {
    // A full 64-bit oid, for the reason waystone.hpp gives about the entry
    // field it becomes: oids are uint64 at the source, and a narrowed copy
    // that fits today aliases two relations the day it does not.
    std::uint64_t rel_oid = 0;

    // Zero-extended Keystone id (invariant 7: upper 24 bits are 0).
    std::uint64_t pk = 0;

    PageId page_id = kInvalidPageId;

    // The page's relayout epoch at the moment of access
    // (docs/spec/physical-optimizer.md R4, since PX04 - it was absent while
    // the engine had no epoch). Reported **verbatim** as the header's u64:
    // the executor states the fact, and each store that keeps it owns its
    // stored width - trail_store.hpp narrows to its entry format's u32 and
    // says why that is safe.
    std::uint64_t page_epoch = 0;

    std::uint16_t slot = 0;

    // Which step of the chain produced it - the join position. This is what
    // makes a cross-relation trail replayable rather than merely
    // descriptive: without it the trail is an unordered bag of tuples from
    // three tables (spec section 6).
    std::uint16_t step_id = 0;
};

class TrailCollector {
public:
    // `capacity` is how many tuples this will hold before it starts
    // dropping. The caller sizes it from the trail format's own limit
    // (stats/trail_store.hpp's kMaxTrailEntries), so a collector can never
    // gather more than a trail could store.
    explicit TrailCollector(std::size_t capacity) : capacity_(capacity) {
        entries_.reserve(capacity);
    }

    // Appends, or drops and remembers that it dropped. Never fails, never
    // grows past `capacity`.
    void Add(const TouchedTuple& tuple) {
        if (entries_.size() >= capacity_) {
            overflowed_ = true;
            return;
        }
        entries_.push_back(tuple);
    }

    // True when at least one tuple was dropped, which makes everything
    // here an incomplete account of the execution.
    bool overflowed() const noexcept { return overflowed_; }

    std::span<const TouchedTuple> entries() const noexcept { return entries_; }
    bool empty() const noexcept { return entries_.empty(); }

    // Between statements on a reused collector. Keeps the reservation, so
    // a second statement allocates nothing.
    void Clear() noexcept {
        entries_.clear();
        overflowed_ = false;
    }

private:
    std::vector<TouchedTuple> entries_;
    std::size_t capacity_;
    bool overflowed_ = false;
};

}  // namespace kds::exec
