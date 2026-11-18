#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/exec/plan_rule.hpp"

// **The administrator's plans** (CX-S1b, instructions/v3.2.0/
// workorder-cx-managed-plans.md CX-R1, CX-R9): the plan a DBA or an agent
// attached to a statement shape with `CREATE PLAN`. An enabled one is the
// first source of a matching statement's join order under every value of
// `join_order` - it outranks the shape's captured order, the search and the
// written order - and nothing but `ALTER PLAN` or `DROP PLAN` changes it.
//
// **A plan is variants** (CX-R4): each a join order and, but for the last,
// a rule over the statement's arguments (`plan_rule.hpp`). The first
// variant whose rule holds is the statement's; the last has no rule
// (`ELSE`), so every statement of the shape has one. The choice is made at
// every compile from that statement's literals and is never remembered.
//
// **A variant holds an order and, per relation, at most one access pin**
// (CX-S3a). The order is all a captured plan holds (`captured_plans.hpp`,
// CX-Q12 (a)); each step's access arm and build annotation are the
// compile's, through the full ladder. **A pin restricts that ladder and
// never replaces it** (`ArmPin`): it names the one class of arm the step
// may take, or one it may not, and the ladder then chooses among what is
// left by its own rules - so a pinned step runs an arm the ladder could
// legally take for it, located rows are re-checked against the whole
// residual as under any arm, and a variant cannot force an arm a statement
// cannot use (CX-R2). A variant whose order does not compile for a
// statement, whose pin the statement's step cannot meet, or which adds a
// sort its LIMIT does not bound (CC-Q10), is **declined for that
// statement** and the written order runs with the ladder unrestricted,
// counted and named in `ANALYZE`; the plan stays as it is.
//
// **Exact matching** (CX-R3) `[quiet-wrong]-adjacent`: the captured plans'
// key - the fetch id, the relation count and every relation's oid in
// written order, compared whole.
//
// **One table for the instance**, loaded at a mount from `sys.plans`
// (`plan_catalog.hpp`) and written by the plan DDL alone. It holds every
// administrator's plan whose relations exist, enabled or not, so "does this
// shape have a plan" is asked of memory; only an enabled one is served.
//
// **Cost** (CX-R6): a compile asks `any()` - one relaxed load - and probes
// only while at least one plan is enabled, and only for a top-level block
// of two relations or more with a fetch id. A point read never reaches it.
// Unlike the captured table, a block of tuple key descents **is** probed
// while a plan is enabled: W4 makes an administrator's plan always
// possible, and a plan created and then silently not applied would be a
// spelling that enforces nothing.
//
// **Latch protocol** (`docs/rules/rules.md` §3): a partition's latch guards
// its map and is held for one lookup, install or erase, across no I/O;
// nothing is taken under it. A plan is immutable once installed and handed
// out by shared pointer, so a compile reads it with no latch held. **The
// DDL claim** is one flag, taken by `CREATE PLAN`, `ALTER PLAN` and `DROP
// PLAN` for their check and their writes and never waited for: a second
// plan DDL meanwhile is refused `TxnConflict`. It is what makes "one plan a
// shape" and an `ALTER`'s replace-then-remove one act across cores.

namespace kds::exec {

// The variants one plan may hold: a rule list a compile walks, so bounded.
inline constexpr std::size_t kMaxPlanVariants = 16;

// **One relation's access pin** (CX-S3a, CX-R2): the class of arm the
// ladder is held to for that relation's step, or - `forbid` - the one it
// is kept from. The classes are the ladder's own ranks
// (`step_compiler.cpp`, section 4): the tuple key (a lookup, a probe, a key
// set, a range), a secondary index (`index` naming which, or any), a Cabin
// (every Cabin arm), and the walk (a filter scan or a bare scan, with or
// without the statement-local build). `kLadder` is no pin.
//
// A pin is met when the step's arm is of its class - and, for a named
// index, is that index. `SCAN` cannot be forbidden: the walk is what every
// other arm falls back to.
struct ArmPin {
    enum class Arm : std::uint8_t { kLadder, kTupleKey, kIndex, kCabin, kScan };
    Arm arm = Arm::kLadder;
    bool forbid = false;
    std::string index;  // a pinned `kIndex`'s name; empty: any index

    bool pins() const noexcept { return arm != Arm::kLadder; }
    // Whether the ladder may try arms of class `tried`.
    bool Admits(Arm tried) const noexcept {
        return arm == Arm::kLadder || (forbid ? arm != tried : arm == tried);
    }
    bool operator==(const ArmPin&) const = default;
};

// A pin's word in the DDL and in `sys.plans`: `TUPLE_KEY`, `INDEX`,
// `CABIN`, `SCAN`.
std::string_view ArmName(ArmPin::Arm arm) noexcept;

struct PlanVariant {
    std::string name;
    PlanRule rule;          // empty: `ELSE`
    CapturedOrder order{};  // written positions in executed order
    // By written position; empty where the variant pins nothing, else one
    // entry a relation.
    std::vector<ArmPin> access;
};

struct AdminPlan {
    std::string name;
    CapturedKey key;
    // In rule order; the last is `ELSE`. Never empty.
    std::vector<PlanVariant> variants;

    // The variant `statement` runs: the first whose rule holds for its
    // arguments. The statement is lexed only where a rule must be asked.
    const PlanVariant& Choose(std::string_view statement) const;
};

class AdminPlans {
public:
    static constexpr std::size_t kPartitions = 16;

    // Whether any plan is enabled. A hint a compile gates its probe on: a
    // plan enabled this instant is served from the next compile.
    bool any() const noexcept { return enabled_.load(std::memory_order_relaxed) != 0; }

    // The enabled plan for `key`, or null.
    std::shared_ptr<const AdminPlan> Find(const CapturedKey& key);

    // The name of the plan holding `key`, enabled or not, or nullopt.
    std::optional<std::string> Holder(const CapturedKey& key);

    // Installs `plan` for its key, replacing the key's plan, served or not.
    void Install(std::shared_ptr<const AdminPlan> plan, bool enabled);

    // Forgets the key's plan. False where it held none.
    bool Erase(const CapturedKey& key);

    // The name of an enabled plan naming `oid`, or nullopt: what RESTRICTs
    // a `DROP TABLE` (CX-Q24).
    std::optional<std::string> EnabledNaming(catalog::Oid oid);

    // The name of an enabled plan a variant of which pins the index named
    // `index` on relation `oid`, or nullopt: what RESTRICTs a `DROP INDEX`
    // (CX-Q4 (a)).
    std::optional<std::string> EnabledPinningIndex(catalog::Oid oid, std::string_view index);

    // A committed `DROP TABLE`'s: forgets every plan naming `oid`. Their
    // rows stay, shown `invalid` - an oid is never reissued, so they can
    // match nothing again.
    void ForgetRelation(catalog::Oid oid);

    // ---- The DDL claim -----------------------------------------------------
    bool TryBeginDdl() noexcept { return !ddl_busy_.exchange(true, std::memory_order_acquire); }
    void EndDdl() noexcept { ddl_busy_.store(false, std::memory_order_release); }

    // ---- Counters, for `sys.meta` ------------------------------------------
    // Plans the table holds, and how many of them are enabled.
    std::uint64_t plans() const noexcept { return plans_.load(std::memory_order_relaxed); }
    std::uint64_t enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    // Compiles that found an enabled plan, whether its variant then ran or
    // was declined.
    std::uint64_t served() const noexcept { return served_.load(std::memory_order_relaxed); }
    // Variants declined for a statement, which ran its written order.
    std::uint64_t declined() const noexcept { return declined_.load(std::memory_order_relaxed); }
    void CountServed() noexcept { served_.fetch_add(1, std::memory_order_relaxed); }
    void CountDeclined() noexcept { declined_.fetch_add(1, std::memory_order_relaxed); }

private:
    struct Entry {
        std::shared_ptr<const AdminPlan> plan;
        bool enabled = false;
    };
    struct Partition {
        Latch latch{LatchKind::kStatsRing};
        std::unordered_map<CapturedKey, Entry, CapturedKeyHash> plans;
    };
    Partition& PartitionOf(const CapturedKey& key) noexcept;

    std::array<Partition, kPartitions> partitions_;
    std::atomic<std::uint64_t> plans_{0};
    std::atomic<std::uint64_t> enabled_{0};
    std::atomic<std::uint64_t> served_{0};
    std::atomic<std::uint64_t> declined_{0};
    std::atomic<bool> ddl_busy_{false};
};

// Holds the DDL claim for a scope; `held()` is false where another plan DDL
// has it.
class PlanDdlClaim {
public:
    explicit PlanDdlClaim(AdminPlans& plans) noexcept
        : plans_(plans), held_(plans.TryBeginDdl()) {}
    ~PlanDdlClaim() {
        if (held_) plans_.EndDdl();
    }
    PlanDdlClaim(const PlanDdlClaim&) = delete;
    PlanDdlClaim& operator=(const PlanDdlClaim&) = delete;
    bool held() const noexcept { return held_; }

private:
    AdminPlans& plans_;
    bool held_;
};

}  // namespace kds::exec
