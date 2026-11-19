#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/stats/per_core_ring.hpp"

// **`sys.access_stats`' rows: what a statement's steps actually did, per
// access *shape*** (`docs/spec/heap-and-tuple.md` §7). Memory-resident since
// DA (`instructions/v3.2.0/workorder-da-access-stats-memory-resident.md`):
// emptied by a restart and by a restore, as the catalog relation it replaced
// was emptied at every mount. Read by `SHOW ACCESS` and CREATE CABIN's
// no-filter warning, and by nothing else (DA-R7).
//
// ---- The key: columns, not values ----------------------------------------
//
// `(kind, rel_id, column_mask)`. `WHERE flag = 1` and `WHERE flag = 2` are
// one shape, so the store is bounded by the schema rather than the data.
// Every access kind is recorded by the same call, with no branch on the
// kind: "how often is this relation looked up versus filter-scanned" is only
// answerable if both are counted the same way.
//
// ---- Where the rows live, and the latch protocol --------------------------
//
// **The instance owns the store** (DA-R6): a restart, a restore and a second
// instance in one process each start empty, with no reset call. The
// instance hands one object to every core's dispatcher; a dispatcher handed
// none uses one of its own.
//
// One part per core (`kStatsParts`) under a latch of its own
// (`LatchKind::kStatsRing`): the core a statement ran on is the only writer
// of its part (DA-R4); a reader takes each part's latch in turn, copies its
// live slots, and merges the copies with no latch held - `uses` summed and
// saturating, `last_seen` the latest. A forget takes each part's latch in
// turn (DA-R5).
//
// **A part is a flat table of fixed-size slots** (DA-Q8 (a)), sized by the
// boot key `access_stats_bytes_per_core`: `bytes / kSlotBytes` slots,
// linear-probed from the key's hash, **at most half of them live** - the
// load bound that keeps a miss on a part at its bound a short walk rather
// than a walk of every slot. The table is allocated whole, once, at the
// part's first record, and never grown: a core that records nothing costs
// nothing, and a dispatcher's own fallback store costs nothing once the
// instance's replaces it. A forget deletes by backward shift, so no
// tombstone counts against the bound.
//
// **A record never fails, delays or reorders the statement it describes**
// (DA-R3), and waits for at most one reader's copy of its own part. A new
// shape past the bound is dropped and counted; a part whose table cannot be
// allocated drops and counts every record (`sys.meta`'s
// `access_stats_dropped`).

namespace kds::stats {

// One shape, the cores' parts merged.
struct AccessShape {
    exec::AccessKind kind = exec::AccessKind::kLookup;
    catalog::Oid rel_id = 0;
    // A bit per `col_pos` the access keyed or filtered on; a column past 63
    // folds into no bit (`catalog::ColumnBit`), which makes the shape
    // coarser, never wrong.
    std::uint64_t column_mask = 0;
    // Executions observed, saturating: a count that wrapped would make the
    // hottest shape read as the coldest.
    std::uint64_t uses = 0;
    // The latest execution on the instance's monotonic clock, in ns; 0 with
    // no clock.
    std::uint64_t last_seen = 0;
};

class AccessStats {
public:
    // A slot's bytes: the unit `access_stats_bytes_per_core` is counted in.
    static constexpr std::size_t kSlotBytes = 40;
    // The key's default: what admits 4,096 shapes a core (DA-Q3 (a),
    // BO-Q7 (a)'s number) at the half-full load bound.
    static constexpr std::uint64_t kDefaultBytesPerCore = 2 * 4096 * kSlotBytes;
    // The least that admits one shape, and the most a core's part may take.
    static constexpr std::uint64_t kMinBytesPerCore = 2 * kSlotBytes;
    static constexpr std::uint64_t kMaxBytesPerCore = std::uint64_t{256} << 20;

    // `access_stats_bytes_per_core`'s range; a value that is not a whole
    // number of slots admits the slots that fit.
    static Status CheckBytesPerCore(std::uint64_t bytes);

    explicit AccessStats(std::uint64_t bytes_per_core = kDefaultBytesPerCore) noexcept;
    AccessStats(const AccessStats&) = delete;
    AccessStats& operator=(const AccessStats&) = delete;
    ~AccessStats();

    // One execution of a shape on `core`. Returns nothing (DA-R3).
    void Record(std::uint32_t core, exec::AccessKind kind, catalog::Oid rel_id,
                std::uint64_t column_mask, std::uint64_t now) noexcept;

    // Removes every shape of `rel_id` whose mask names `col_pos`, from every
    // part, before it returns (CD-OI-13 (a), DA-R5).
    void Forget(catalog::Oid rel_id, std::uint16_t col_pos) noexcept;

    // Every shape, the parts merged by key, in key order (DA-Q2 (a)):
    // `rel_id`, then the kind's name, then `column_mask`.
    std::vector<AccessShape> Snapshot() const;

    // New shapes no part could take, over every core.
    std::uint64_t Dropped() const noexcept;

    // The shapes one core's part admits: half its slots.
    std::uint64_t ShapesPerCore() const noexcept { return slots_ / 2; }

private:
    struct Slot {
        std::uint64_t rel_id;
        std::uint64_t column_mask;
        std::uint64_t uses;
        std::uint64_t last_seen;
        exec::AccessKind kind;
        bool used;

        bool Is(exec::AccessKind k, catalog::Oid rel, std::uint64_t mask) const noexcept {
            return used && kind == k && rel_id == rel && column_mask == mask;
        }
    };
    static_assert(sizeof(Slot) == kSlotBytes,
                  "the key counts bytes in slots, so a slot's size is the unit");
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::unique_ptr<Slot[]> table;  // `slots_` slots once allocated
        std::size_t live = 0;
        std::uint64_t dropped = 0;
        bool failed = false;  // the allocation failed: every record drops
    };
    const std::size_t slots_;
    mutable std::array<Part, kStatsParts> parts_;
};

// Records one execution of every step in `chain`, sub-chains and hoisted
// ones included, on `core`'s part. Called after a **successful** statement:
// one that errored part way ran some steps and not others, and a count
// including it would describe a workload nobody ran.
void RecordChainAccess(AccessStats& store, std::uint32_t core, const exec::StepChain& chain,
                       std::uint64_t now) noexcept;

// The column bitmap for one step's access shape.
std::uint64_t ColumnMaskOf(const exec::Step& step) noexcept;

}  // namespace kds::stats
