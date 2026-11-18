#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/sched/wait_state.hpp"
#include "kds/stats/per_core_ring.hpp"
#include "kds/stats/wait_event.hpp"

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
//
// ---- What a shape costs (DF-S1) --------------------------------------------
//
// Beside how often a shape ran and how long
// (`instructions/v3.2.0/workorder-df-statistics-coverage.md` §2, part A): the
// rows it answered, what its chain examined, the log it appended, where its
// time went, how its latency is distributed and how often it lost a race.
// **A number the engine could not take for a call is left out of the sum
// and of the count beside it, and a shape with no such call reads NULL**
// (DF-R7): `executed_calls`, `compiled_calls` and `timed_calls` are those
// counts. No value of a statement enters a shape (DF-R6), and nothing in the
// engine reads one (DF-R3).

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

    // ---- DF-S1 --------------------------------------------------------------
    std::uint64_t conflicts = 0;  // the calls refused `TxnConflict`, within `refused`
    std::uint64_t rows = 0;       // rows answered by a SELECT, affected by a write
    // The calls whose own step chain ran to its end, and what it counted
    // (`exec::StepStats`): a native `SELECT`'s and an `ANALYZE`'s.
    std::uint64_t executed_calls = 0;
    std::uint64_t rows_examined = 0;
    std::uint64_t pages = 0;
    // The calls that compiled a chain of their own, and the time from the
    // run's head to it.
    std::uint64_t compiled_calls = 0;
    std::uint64_t compile_ns = 0;
    // The calls that ran inside a task of their own session - the only ones
    // whose time and log the session's statistics hold - and the sum of
    // each one's delta of those statistics.
    std::uint64_t timed_calls = 0;
    sched::SessionCostSample timed;
    // Every call's elapsed time, in `sys.wait_histogram`'s buckets
    // (`sched::WaitBucketOf`); the buckets sum to `calls`.
    std::array<std::uint64_t, sched::kWaitHistogramBuckets> latency{};
};

// What one call hands `Record` beyond its outcome and its elapsed time. A
// default one is a call the engine counted nothing for.
struct StatementCost {
    std::uint64_t rows = 0;
    bool conflict = false;
    bool executed = false;  // `rows_examined` and `pages` are a chain's
    std::uint64_t rows_examined = 0;
    std::uint64_t pages = 0;
    bool compiled = false;  // `compile_ns` is set
    std::uint64_t compile_ns = 0;
    // The session's statistics at the call's end less at its head; unset
    // for a call that ran outside a task of its own session.
    std::optional<sched::SessionCostSample> timed;
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
                       std::string_view plan_label = {},
                       const StatementCost& cost = {}) noexcept;

    // Every shape, the cores' parts merged by key: every count and sum
    // added, the longest call the longest, the first statement the
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
