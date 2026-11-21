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
// of two relations or more with a fetch id. Unlike the captured table, a
// block of tuple key descents **is** probed while a plan is enabled: W4
// makes an administrator's plan always possible, and a plan created and
// then silently not applied would be a spelling that enforces nothing.
//
// **A statement over one relation has a plan too** (CX-S3b): no order to
// state, but an arm to pin - which is how a controller-made Cabin, whose
// candidates are one-relation filters, is read through at all. Its compile
// asks `any_single()` - one relaxed load - and probes only while a
// one-relation plan is enabled; with none, a point read never reaches the
// table, and with one, every top-level one-relation `SELECT` with a fetch
// id pays the probe, a point read's included, for the reason above.
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
//
// **A pinned `kCabin` may name a Cabin, by the column it is on** (CX-S3b,
// CX-Q6 (a)): `CABIN (<column>)`. It is then met only by that column
// Cabin's arm, and it is **the one way a controller-made Cabin reaches the
// ladder** - a Cabin of `auto` origin is tried by no rank of an unpinned
// step and by no rank under `CABIN` unnamed, which admits the declared
// Cabins alone. The cabin optimizer builds; a plan that names what it built
// is what reads through it.
//
// **A relation's join method may be pinned too** (CX-S5): `BUILD <binding>
// ON` holds the step to the statement-local inner build
// (`join-inner-build.md`) - every keyed class stands aside, so the walk's
// ranks are what the ladder has left, and the pin is met only where the
// step ends with the build annotation; `OFF` takes the build's rank away,
// so the step walks per outer row, or takes what ranks below it. Neither
// forces a build: the build's own rule (`BuildKeyOf`) and the filter scan's
// rank above it are unchanged, and `join_build_max_rows`' run-time decline
// is a resource limit, not a plan change (CX-Q8 (a)).
struct ArmPin {
    enum class Arm : std::uint8_t { kLadder, kTupleKey, kIndex, kCabin, kScan };
    enum class Build : std::uint8_t { kLadder, kOn, kOff };
    Arm arm = Arm::kLadder;
    bool forbid = false;
    // What a pin names: a pinned `kIndex`'s index, or a pinned `kCabin`'s
    // column. Empty: any index, or any declared Cabin. Never set on another
    // arm, nor on a forbidden one.
    std::string name;
    Build build = Build::kLadder;

    // Whether the relation's step is pinned at all - its arm, its build, or
    // both.
    bool pins() const noexcept { return arm != Arm::kLadder || build != Build::kLadder; }
    // Whether the pin holds the step to a keyed class - the tuple key, an
    // index, a Cabin - which leaves `BUILD ON` nothing to take.
    bool keyed() const noexcept {
        return arm != Arm::kLadder && arm != Arm::kScan && !forbid;
    }
    // Whether the ladder may try arms of class `tried`. Under `BUILD ON` the
    // walk is the one class left.
    bool Admits(Arm tried) const noexcept {
        if (build == Build::kOn && tried != Arm::kScan) return false;
        return arm == Arm::kLadder || (forbid ? arm != tried : arm == tried);
    }
    bool operator==(const ArmPin&) const = default;
};

// A pin's word - `TUPLE_KEY`, `INDEX`, `CABIN`, `SCAN` - the one spelling
// the DDL reads, a `sys.plans` row stores and `ANALYZE` and the view print;
// and the arm a word names, case-insensitively, or nullopt.
std::string_view ArmName(ArmPin::Arm arm) noexcept;
std::optional<ArmPin::Arm> ArmFromName(std::string_view word) noexcept;
// A build pin's word, `ON` or `OFF` - the DDL's, the row's and the view's
// (CX-S5); empty for none.
std::string_view BuildName(ArmPin::Build build) noexcept;

// The class of arm a step's kind is: what a pin naming a class is met by.
// No `default`, so a new kind is judged here too.
constexpr ArmPin::Arm ArmClassOf(AccessKind kind) noexcept {
    switch (kind) {
        case AccessKind::kLookup:
        case AccessKind::kProbe:
        case AccessKind::kRange: return ArmPin::Arm::kTupleKey;
        case AccessKind::kIndexProbe:
        case AccessKind::kIndexRange: return ArmPin::Arm::kIndex;
        case AccessKind::kCabinProbe: return ArmPin::Arm::kCabin;
        case AccessKind::kFilterScan:
        case AccessKind::kScan: return ArmPin::Arm::kScan;
    }
    return ArmPin::Arm::kScan;
}

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
    struct Entry {
        std::shared_ptr<const AdminPlan> plan;
        bool enabled = false;
    };
    struct Partition {
        Latch latch{LatchKind::kPlanTable};
        std::unordered_map<CapturedKey, Entry, CapturedKeyHash> plans;
    };

public:
    static constexpr std::size_t kPartitions = 16;

    // Whether any plan is enabled. A hint a compile gates its probe on: a
    // plan enabled this instant is served from the next compile.
    bool any() const noexcept { return enabled_.load(std::memory_order_relaxed) != 0; }
    // Whether any enabled plan is for a statement over one relation
    // (CX-S3b): what a one-relation compile gates its probe on, so it asks
    // the table only while such a plan exists.
    bool any_single() const noexcept {
        return enabled_single_.load(std::memory_order_relaxed) != 0;
    }

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

    // The same for the Cabin on relation `oid`'s column named `column`
    // (CX-S3b): what RESTRICTs a `DROP CABIN` and the cabin optimizer's own
    // DROP of a Cabin a plan reads through.
    std::optional<std::string> EnabledPinningCabin(catalog::Oid oid, std::string_view column);

private:
    // The name of the first enabled plan `test` holds for, or nullopt.
    template <typename Test>
    std::optional<std::string> EnabledWhere(Test&& test) {
        for (Partition& partition : partitions_) {
            const LatchGuard hold(&partition.latch);
            for (const auto& [key, entry] : partition.plans) {
                if (entry.enabled && test(key, *entry.plan)) return entry.plan->name;
            }
        }
        return std::nullopt;
    }

public:

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
    Partition& PartitionOf(const CapturedKey& key) noexcept;
    // One enabled plan more, or one fewer, for `key`.
    void CountEnabled(const CapturedKey& key, bool enabled) noexcept;
    // The name of an enabled plan a variant of which pins, by name, the
    // index (`arm == kIndex`) or the column's Cabin (`kCabin`) on `oid`.
    std::optional<std::string> EnabledPinning(catalog::Oid oid, ArmPin::Arm arm,
                                              std::string_view name);

    std::array<Partition, kPartitions> partitions_;
    std::atomic<std::uint64_t> plans_{0};
    std::atomic<std::uint64_t> enabled_{0};
    std::atomic<std::uint64_t> enabled_single_{0};  // of `enabled_`, those with `key.n == 1`
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
