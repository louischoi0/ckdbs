#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kds/sched/wait_state.hpp"
#include "kds/stats/per_core_ring.hpp"

// **`sys.refusal_stats`' rows: how statements were refused, by code**
// (`instructions/v3.2.0/workorder-df-statistics-coverage.md` §2 part A,
// DF-Q3 (a)). Memory-resident (DF-R1): emptied by a restart and by a
// restore. No switch (DF-R9).
//
// ---- The key ---------------------------------------------------------------
//
// `(core, code)`, the code a number of the wire's pinned error registry
// (`wire::ErrorCategory`, `protocol.md` §11) - the category the client was
// told. Bounded by that registry and keyed by nothing a client can mint: a
// code at or past `kRefusalCodes` is not recorded, and the registry's last
// entry is asserted below it where the view is built.
//
// ---- Where the counts live, and the protocol --------------------------------
//
// A fixed table per core, no latch and no allocation: the core a statement
// ran on is the only writer of its part, once per refused statement at its
// end, with relaxed single-writer words (`sched::RelaxedU64`); a reader on
// any core reads them unguarded, so a row's `count` and `last_seen` may
// differ by one refusal in flight. Nothing in the engine reads them (DF-R3).

namespace kds::stats {

struct RefusalCount {
    std::uint32_t core = 0;
    std::uint16_t code = 0;
    std::uint64_t count = 0;
    std::int64_t last_seen_us = 0;  // UTC epoch microseconds (TIMESTAMP)
};

class RefusalStats {
public:
    // Above the registry's 20 entries with room to append; 16 bytes a code.
    static constexpr std::size_t kRefusalCodes = 64;

    // One refused statement, at its end.
    static void Record(std::uint32_t core, std::uint16_t code, std::int64_t now_us) noexcept;

    // Every non-zero count, ordered by (core, code).
    static std::vector<RefusalCount> Snapshot();

    // Zeroes every part; a test's alone.
    static void ResetForTest() noexcept;

private:
    struct Slot {
        sched::RelaxedU64 count;
        sched::RelaxedU64 last_seen_us;
    };
    using Part = std::array<Slot, kRefusalCodes>;
    static std::array<Part, kStatsParts>& parts() noexcept;
};

}  // namespace kds::stats
