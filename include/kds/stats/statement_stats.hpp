#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/stats/per_core_ring.hpp"

// **`sys.statement_stats`' rows: what a statement shape costs**
// (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` §3.1,
// BO-S5). Memory-resident (BO-Q5 (b)): emptied by a restart and by a
// restore, never a catalog page. No switch (BO-Q8): always on.
//
// ---- The key ---------------------------------------------------------------
//
// `(fetch_id, kind)` (BO-Q6 (a) as revised). `kind` is the statement's first
// keyword; only `SELECT`, `INSERT`, `UPDATE` and `DELETE` carry a
// fingerprint (`parser/fingerprint.cpp`), so every other kind is one row at
// `fetch_id = 0`. A first word that is no statement verb is `OTHER`, so a
// client cannot mint a row per misspelling.
//
// ---- Where the rows live, and the latch protocol ---------------------------
//
// One part per core under a latch of its own (`LatchKind::kStatsRing`, the
// statistics stores' kind): the core a statement ran on is the only writer
// of its part, once per statement at its end; a reader on any core takes
// each part's latch in turn, copies it, and merges the parts by key - one
// row per shape for the instance. No cross-core write is added to a
// statement's path.
//
// **A write never fails, delays or reorders the statement it describes.** A
// new shape past `kShapesPerCore` is dropped and counted (a `sys.meta`
// field, SV3); a shape that cannot be allocated is dropped the same way.

namespace kds::stats {

struct StatementShape {
    std::uint64_t fetch_id = 0;
    std::string kind;
    std::uint64_t calls = 0;
    std::uint64_t refused = 0;
    std::uint64_t total_us = 0;
    std::uint64_t max_us = 0;
    std::int64_t first_seen_us = 0;  // UTC epoch microseconds (TIMESTAMP)
    std::int64_t last_seen_us = 0;
    std::string shape_text;          // the first statement seen, cut to kShapeTextBytes
    // **Where the shape's join order last came from** (CX-R11, CX-S1b):
    // `admin`, `captured`, `searched`, `deferred`, `unkeyed`, `over_ceiling`,
    // `declined`, `admin_declined` or `written`; empty for a shape with no
    // join block, or none compiled yet. `plan_label` is `<plan>/<variant>`
    // where an administrator's plan chose it. The latest compile's, on the
    // core that saw the shape last.
    std::string plan_source;
    std::string plan_label;
};

// The `kind` of a statement: its first whitespace-separated token,
// upper-cased, when it is a statement verb this engine routes - the token
// `DispatchInner` routes on - and `OTHER` otherwise. Static storage.
std::string_view StatementKindOf(std::string_view line) noexcept;

class StatementStats {
public:
    static constexpr std::size_t kShapesPerCore = 4096;  // BO-Q7 (a)
    static constexpr std::size_t kShapeTextBytes = 256;

    // One statement, at its end; its kind is `StatementKindOf(line)`.
    // `fetch_id` is 0 where the kind carries no fingerprint; `elapsed_us` is
    // the statement whole, re-runs included. `plan_source` is empty for a
    // statement that compiled no join block, and then the shape keeps what
    // it had.
    static void Record(std::uint32_t core, std::uint64_t fetch_id, std::string_view line,
                       bool refused, std::uint64_t elapsed_us, std::int64_t now_us,
                       std::string_view plan_source = {},
                       std::string_view plan_label = {}) noexcept;

    // Every shape, the cores' parts merged by key: calls, refusals and time
    // summed, the longest call the longest, the first statement the
    // earliest seen. Ordered by (fetch_id, kind).
    static std::vector<StatementShape> Snapshot();

    // New shapes no part could take, over every core.
    static std::uint64_t Dropped() noexcept;

    // Empties every part; a test's alone.
    static void ResetForTest() noexcept;

private:
    struct Key {
        std::uint64_t fetch_id;
        std::string_view kind;  // static storage, `StatementKindOf`'s
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept;
    };
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::unordered_map<Key, StatementShape, KeyHash> shapes;
        std::uint64_t dropped = 0;
    };
    static std::array<Part, kStatsParts>& parts() noexcept;
};

}  // namespace kds::stats
