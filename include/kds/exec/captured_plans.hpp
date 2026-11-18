#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
// **Latch protocol** (`docs/rules/rules.md` §3): a partition's latch guards
// its map and is held for one lookup, one install or one erase, across no
// I/O. The queue latch guards the two queues the persisting task drains and
// is taken alone or **under** a partition's (an install queues its capture
// before it lets the partition go, so the queue never names an order the
// table does not hold); nothing is taken under it. The shape count and the
// counters are relaxed atomics; the ceiling is taken by an increment that
// is given back, under the partition's latch.
//
// **Bounded, never evicted** (CX-Q21 (a)): past `kMaxShapes` a new shape is
// refused and runs its written order. Evicting one and capturing it again
// would be the engine changing a plan on its own.
//
// **Persisted off the statement** (CX-R8, CX-S1a): an install queues the
// capture with its statement's shape text, and core 0's `system` task
// drains the queue into logged `sys.plans` rows (`plan_catalog.hpp`); a
// mount loads them back. A capture a crash finds still queued is lost and
// made again at the shape's next compile.

namespace kds::exec {

struct CapturedKey {
    std::uint64_t fetch_id = 0;
    std::uint8_t n = 0;
    std::array<catalog::Oid, kMaxPlannedRelations> oids{};

    bool operator==(const CapturedKey&) const = default;
    // Whether the shape binds `oid`.
    bool Names(catalog::Oid oid) const noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            if (oids[i] == oid) return true;
        }
        return false;
    }
};

// Written positions in executed order; the slots past the block's relation
// count are 0.
using CapturedOrder = std::array<std::uint8_t, kMaxPlannedRelations>;

// A capture as the persisting task takes it: the shape, its order and the
// capturing statement's shape text (`parser::ShapeText`).
struct CapturedPlan {
    CapturedKey key;
    CapturedOrder order{};
    std::string statement;
};

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
    // ceiling, where nothing is installed. An install is queued for the
    // persisting task with `statement`'s shape text.
    std::optional<CapturedOrder> Capture(const CapturedKey& key, const CapturedOrder& order,
                                         std::string_view statement = {});

    // A mount's: installs a persisted capture, queueing nothing. False where
    // the key already holds an order or the table is full.
    bool Load(const CapturedKey& key, const CapturedOrder& order);

    // `DROP PLAN`'s: forgets the shape, so its next compile under `fixed`
    // captures again (CX-Q17). False where the key held nothing.
    bool Erase(const CapturedKey& key);

    // A committed `DROP TABLE`'s (CX-Q24): forgets every shape naming `oid`,
    // queued ones included, and - where it forgot one - owes the persisting
    // task the removal of their rows. An oid is never reissued, so nothing
    // could match them again; they would only count against the ceiling.
    void ForgetRelation(catalog::Oid oid);

    // ---- The persisting task's (`plan_catalog.hpp`) ------------------------
    // Up to `max` queued captures, oldest first, removed from the queue.
    std::vector<CapturedPlan> TakePending(std::size_t max);
    // Puts back captures a step could not write, ahead of the queue, unless
    // their shape was forgotten meanwhile.
    void Requeue(std::vector<CapturedPlan> plans);
    // The relations dropped since the last call.
    std::vector<catalog::Oid> TakeForgotten();
    void CountPersisted(std::uint64_t rows) noexcept {
        persisted_.fetch_add(rows, std::memory_order_relaxed);
    }

    // Whether a new shape would be refused. A hint: a capture that races the
    // last free slot is answered by `Capture` itself.
    bool full() const noexcept { return shapes_.load(std::memory_order_relaxed) >= kMaxShapes; }

    // ---- Counters, for `sys.meta` ------------------------------------------
    std::uint64_t shapes() const noexcept { return shapes_.load(std::memory_order_relaxed); }
    // Compiles that found a captured order in force - by `Find`, or as the
    // answer to a `Capture` another core's install won - whether it then
    // ran or was declined (`declined`).
    std::uint64_t served() const noexcept { return served_.load(std::memory_order_relaxed); }
    // Searched blocks not captured: a relation of theirs had no published
    // prior yet (CX-Q19), or a DDL was open (a minted view).
    std::uint64_t deferred() const noexcept { return deferred_.load(std::memory_order_relaxed); }
    // Blocks no key fits - no fetch id, or more relations than a key holds
    // (CX-Q20) - which ran their written order.
    std::uint64_t unkeyed() const noexcept { return unkeyed_.load(std::memory_order_relaxed); }
    // New shapes past the ceiling, which ran their written order.
    std::uint64_t over_ceiling() const noexcept {
        return over_ceiling_.load(std::memory_order_relaxed);
    }
    // Captured orders declined for a statement, which ran its written order:
    // one that did not compile for it (CX-Q26), or one that adds a sort its
    // LIMIT does not bound (CC-Q10).
    std::uint64_t declined() const noexcept { return declined_.load(std::memory_order_relaxed); }
    // Captures waiting for the persisting task, rows it has written in this
    // run, and captures a mount loaded.
    std::uint64_t pending() const noexcept { return pending_count_.load(std::memory_order_relaxed); }
    std::uint64_t persisted() const noexcept { return persisted_.load(std::memory_order_relaxed); }
    std::uint64_t loaded() const noexcept { return loaded_.load(std::memory_order_relaxed); }

    void CountServed() noexcept { served_.fetch_add(1, std::memory_order_relaxed); }
    void CountDeferred() noexcept { deferred_.fetch_add(1, std::memory_order_relaxed); }
    void CountUnkeyed() noexcept { unkeyed_.fetch_add(1, std::memory_order_relaxed); }
    void CountOverCeiling() noexcept { over_ceiling_.fetch_add(1, std::memory_order_relaxed); }
    void CountDeclined() noexcept { declined_.fetch_add(1, std::memory_order_relaxed); }

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
    Latch queue_latch_{LatchKind::kStatsRing};
    std::vector<CapturedPlan> pending_;
    std::vector<catalog::Oid> forgotten_;
    std::atomic<std::uint64_t> pending_count_{0};
    std::atomic<std::uint64_t> persisted_{0};
    std::atomic<std::uint64_t> loaded_{0};
    std::atomic<std::uint64_t> shapes_{0};
    std::atomic<std::uint64_t> served_{0};
    std::atomic<std::uint64_t> deferred_{0};
    std::atomic<std::uint64_t> unkeyed_{0};
    std::atomic<std::uint64_t> over_ceiling_{0};
    std::atomic<std::uint64_t> declined_{0};
};

}  // namespace kds::exec
