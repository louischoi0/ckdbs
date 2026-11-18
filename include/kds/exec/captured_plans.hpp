#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "kds/base/latch.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/exec/step_chain.hpp"

// **The captured plans of `join_order = fixed`** (CX-S0, instructions/
// v3.2.0/workorder-cx-managed-plans.md CX-R7): the join order the search
// chose for a statement shape the first time it priced the shape from
// published statistics, kept for the life of the instance and run at every
// later compile of the shape, whatever the statistics do after.
//
// **What is kept is an order and nothing else** (CX-Q12 (a)): the written
// positions of a block's relations in executed order. Each step's access
// arm and build annotation are the compile's again at every compile - the
// captured order goes through the full ladder, as an order the memo
// remembers does (`plan_memo.hpp`) - so no chain, literal or page id is
// reused, and a `CREATE INDEX` moves a captured shape's arm as it moves
// any other.
//
// **Exact matching** (CX-R3) `[quiet-wrong]-adjacent`: the key is the fetch
// id, the relation count and every relation's oid in written order, compared
// whole. A fetch id hashes names, not oids (`fingerprint.hpp`), so a
// relation dropped and created again under its name misses on its oid. A
// collision of two shapes' fetch ids over the same oids would run one
// shape's order for the other - the same rows, as every order of an inner
// block gives.
//
// **First install wins, at once, for every core** (CX-R7): one table for the
// instance in `kPartitions` latched partitions, so a core that searched a
// shape another core captured meanwhile is answered the plan in force and
// runs it. This is CX-Q22's option (b), taken at the build against the
// adopted (a) - a published snapshot with per-core staging - because (a)
// lets two cores run two orders for one shape until the next install and
// CX-R7 says none does; the cost is one partition latch on a searched
// block's compile, never on a point read or a tuple key chain, which leave
// the compile before the table is asked (CX-R6).
//
// **Bounded, never evicted** (CX-Q21 (a)): past `kMaxShapes` a new shape is
// refused and runs its written order. Evicting one and capturing it again
// would be the engine changing a plan on its own.
//
// Memory only until CX-S1 persists it: a restart captures every shape again.

namespace kds::exec {

struct CapturedKey {
    std::uint64_t fetch_id = 0;
    std::uint8_t n = 0;
    std::array<catalog::Oid, kMaxPlannedRelations> oids{};

    bool operator==(const CapturedKey&) const = default;
};

// Written positions in executed order; the slots past the block's relation
// count are 0.
using CapturedOrder = std::array<std::uint8_t, kMaxPlannedRelations>;

class CapturedPlans {
public:
    static constexpr std::size_t kPartitions = 64;
    // `[provisional]`, CX-Q21: a client that generates shapes must not grow
    // the table - and, once CX-S1 persists it, a logged relation - without
    // limit. 65,536 shapes of 152 bytes are 10 MiB.
    static constexpr std::size_t kMaxShapes = 65536;

    // The order in force for `key`, or nullopt.
    std::optional<CapturedOrder> Find(const CapturedKey& key);

    // Installs `order` for `key` unless the key holds one, and answers the
    // order in force - this one, or the earlier install's. Nullopt past the
    // ceiling, where nothing is installed.
    std::optional<CapturedOrder> Capture(const CapturedKey& key, const CapturedOrder& order);

    // Whether a new shape would be refused. A hint: a capture that races the
    // last free slot is answered by `Capture` itself.
    bool full() const noexcept { return shapes_.load(std::memory_order_relaxed) >= kMaxShapes; }

    // ---- Counters, for `sys.meta` ------------------------------------------
    std::uint64_t shapes() const noexcept { return shapes_.load(std::memory_order_relaxed); }
    // Compiles that ran a captured order.
    std::uint64_t served() const noexcept { return served_.load(std::memory_order_relaxed); }
    // Searched blocks not captured: a relation of theirs had no published
    // prior yet (CX-Q19).
    std::uint64_t deferred() const noexcept { return deferred_.load(std::memory_order_relaxed); }
    // Blocks no key fits - no fetch id, or more relations than a key holds
    // (CX-Q20) - which ran their written order.
    std::uint64_t unkeyed() const noexcept { return unkeyed_.load(std::memory_order_relaxed); }
    // New shapes refused past the ceiling, which ran their written order.
    std::uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }
    // Captured orders that did not compile for a statement, which ran its
    // written order (CX-Q26).
    std::uint64_t uncompiled() const noexcept {
        return uncompiled_.load(std::memory_order_relaxed);
    }

    void CountDeferred() noexcept { deferred_.fetch_add(1, std::memory_order_relaxed); }
    void CountUnkeyed() noexcept { unkeyed_.fetch_add(1, std::memory_order_relaxed); }
    void CountRefused() noexcept { refused_.fetch_add(1, std::memory_order_relaxed); }
    void CountUncompiled() noexcept { uncompiled_.fetch_add(1, std::memory_order_relaxed); }

private:
    struct KeyHash {
        std::size_t operator()(const CapturedKey& key) const noexcept;
    };
    struct Partition {
        Latch latch{LatchKind::kStatsRing};
        std::unordered_map<CapturedKey, CapturedOrder, KeyHash> plans;
    };
    Partition& PartitionOf(const CapturedKey& key) noexcept;

    std::array<Partition, kPartitions> partitions_;
    std::atomic<std::uint64_t> shapes_{0};
    std::atomic<std::uint64_t> served_{0};
    std::atomic<std::uint64_t> deferred_{0};
    std::atomic<std::uint64_t> unkeyed_{0};
    std::atomic<std::uint64_t> refused_{0};
    std::atomic<std::uint64_t> uncompiled_{0};
};

}  // namespace kds::exec
