#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/sched/wait_state.hpp"
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
// ---- Where the rows live, and the protocol --------------------------------
//
// **The instance owns the store** (DA-R6): a restart, a restore and a second
// instance in one process each start empty, with no reset call. The
// instance hands one object to every core's dispatcher; a dispatcher handed
// none uses one of its own.
//
// One part per core (`kStatsParts`), **and no latch** (`docs/rules/rules.md`
// §3a; DL-S2): the core a statement ran on is the only writer of its part
// (DA-R4) and takes nothing to record; a reader walks each part's slots and
// reads them as they stand, then merges the copies - `uses` summed and
// saturating, `last_seen` the latest; a forget marks slots and waits for
// nothing (DA-R5, DL-Q15).
//
// **A part is a flat table of fixed-size slots** (DA-Q8 (a)), sized by the
// boot key `access_stats_bytes_per_core`: `bytes / kSlotBytes` slots,
// linear-probed from the key's hash, **at most half of them live** - the
// load bound that keeps a miss on a part at its bound a short walk rather
// than a walk of every slot. The table is allocated whole, once, at the
// part's first record, published by a compare-exchange and never grown or
// moved: a core that records nothing costs nothing, and a dispatcher's own
// fallback store costs nothing once the instance's replaces it.
//
// **A slot's state is one atomic word**: two bits of state - empty, being
// claimed, live, dead - above a generation that moves every time the slot
// is given to a key. Every field of a slot is an atomic, so no read beside
// a write is a data race. The part's core claims an empty slot by a
// compare-exchange, writes its key and publishes it live. **A forget marks
// a matching slot dead where it stands** (a compare-exchange from live, by
// any thread) and waits for nothing: a reader skips a dead slot, and the
// part's core takes it again - for its own key, from zero, or for a new
// shape whose walk passes it - under a new generation, and gives a dead
// slot at the end of a run back to empty (`Record`'s note says why that
// breaks no key's run). **So a forgotten slot is free again, no tombstone
// counts against the bound, and none stays to lengthen every later miss**,
// as a backward shift left it before DL. A reader copies a live slot and reads
// its state again: a slot that changed hands under the copy is left out,
// so no row is ever two shapes'. One writer a part is the caller's premise
// and nothing depends on it for memory safety: two threads recording as
// one part can lose a count, or leave one shape two slots that a reader's
// merge adds - nothing else.
//
// **A record never fails, delays or reorders the statement it describes**
// (DA-R3) and waits for nothing. A new shape past the bound is dropped and
// counted; a part whose table cannot be allocated - tried once - drops and
// counts every record (`sys.meta`'s `access_stats_dropped`).

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

    // Forgets every shape of `rel_id` whose mask names `col_pos`, in every
    // part, before it returns (CD-OI-13 (a), DA-R5): no reader shows one
    // after it, and the shape's next execution counts from one. Waits for
    // nothing.
    void Forget(catalog::Oid rel_id, std::uint16_t col_pos) noexcept;

    // Every shape, the parts merged by key, in key order (DA-Q2 (a)):
    // `rel_id`, then the kind's name, then `column_mask`.
    std::vector<AccessShape> Snapshot() const;

    // New shapes no part could take, over every core.
    std::uint64_t Dropped() const noexcept;

    // The shapes one core's part admits: half its slots.
    std::uint64_t ShapesPerCore() const noexcept { return slots_ / 2; }

private:
    // A slot's state word: the state in the low two bits, the slot's
    // generation above them.
    enum SlotState : std::uint32_t { kEmpty = 0, kClaiming = 1, kLive = 2, kDead = 3 };
    static constexpr std::uint32_t kStateMask = 3;
    static constexpr std::uint32_t kGeneration = 4;  // one generation
    static std::uint32_t StateOf(std::uint32_t word) noexcept { return word & kStateMask; }
    // `word`'s slot under its next generation, in `state`.
    static std::uint32_t NextAs(std::uint32_t word, SlotState state) noexcept {
        return ((word & ~kStateMask) + kGeneration) | state;
    }
    struct Slot {
        // The key: written while `kClaiming`, by the part's core alone.
        sched::RelaxedU64 rel_id;
        sched::RelaxedU64 column_mask;
        sched::RelaxedU64 uses;
        sched::RelaxedU64 last_seen;
        std::atomic<std::uint8_t> kind;
        std::atomic<std::uint32_t> state;

        bool Is(exec::AccessKind k, catalog::Oid rel, std::uint64_t mask) const noexcept {
            return kind.load(std::memory_order_relaxed) == static_cast<std::uint8_t>(k) &&
                   rel_id.get() == rel && column_mask.get() == mask;
        }
    };
    static_assert(sizeof(Slot) == kSlotBytes,
                  "the key counts bytes in slots, so a slot's size is the unit");
    struct Part {
        std::atomic<Slot*> table{nullptr};  // `slots_` slots once allocated
        // Slots ever claimed, and those of them a forget left dead and no
        // key has taken again: the difference is the live shapes, which the
        // load bound counts.
        std::atomic<std::size_t> claimed{0};
        std::atomic<std::size_t> dead{0};
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<bool> failed{false};  // the allocation failed: every record drops
    };
    // Whether the part admits a shape it does not hold live: fewer than
    // half its slots are.
    bool RoomForANewShape(const Part& part) const noexcept;
    // Gives `slot`, which the part's core has just moved to `kClaiming`,
    // to a key with one execution, and publishes it live.
    static void Fill(Slot& slot, std::uint32_t claiming, exec::AccessKind kind,
                     catalog::Oid rel_id, std::uint64_t column_mask, std::uint64_t now) noexcept;
    // The part's table, made at its first record; null where it cannot be.
    Slot* TableOf(Part& part) noexcept;
    const std::size_t slots_;
    std::array<Part, kStatsParts> parts_;
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
