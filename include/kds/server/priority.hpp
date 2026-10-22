#pragma once

#include <cstdint>
#include <string_view>

#include "kds/base/status.hpp"

// **A session's priority: the work it declares it does** (BL-R2, BL-R8,
// `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`).
//
// Four context levels, declared by `SET PRIORITY <level>` after the
// handshake, `normal` until one is. v0 serves a level by where its
// transactions run and by its walk slice, nothing else (BL-R7 rung 2):
// - **interactive** levels, `transactional` and `normal`, run on the
//   interactive cores while the partition is active, and anywhere while it
//   is not;
// - **batch** levels, `analytic` and `background`, run on the batch cores
//   while the partition is active - which is exactly while one of them
//   lives on the instance (`load_placer.hpp`).
// `transactional` is `normal`'s placement in v0 and needs the `admin` role
// (BL-R8); the role matters from rung 3, which is not built. Buffer frames,
// the log and the lock table do not read a level (BL-R2's scope).

namespace kds::server {

enum class Priority : std::uint8_t { kTransactional, kNormal, kAnalytic, kBackground };

// The level names hash as identifiers (nothing is reserved), matched
// ignoring ASCII case; an unknown name is `InvalidArgument`.
StatusOr<Priority> ParsePriority(std::string_view name);
std::string_view PriorityName(Priority p) noexcept;

constexpr bool IsBatchPriority(Priority p) noexcept {
    return p == Priority::kAnalytic || p == Priority::kBackground;
}

// **The walk slice, per level** (BL-R2): `exec::SlicePolicy::pages_per_slice`
// re-scoped, not a second quantity. `analytic` yields four times as often,
// so its walks give way first to the batch sessions it shares a core with;
// above one core it protects no interactive level, which the partition
// does. A `SELECT`'s outermost walk only (`SelectRun`): a DDL build's walk
// keeps the default 64 whatever its session declared. `[provisional]` until
// BL-S5.
constexpr std::uint32_t PagesPerSlice(Priority p) noexcept {
    return p == Priority::kAnalytic ? 16 : 64;
}

// **A session's level, and its seat in the instance's batch count**: a batch
// level counts in `sched::CoreLoad::BatchSessions` (`rules.md` §3) for as long
// as this holds it, and is given back when it ends. Move-only: a moved
// session takes the seat and leaves `normal` behind, and a copy would count
// twice.
class PrioritySeat {
public:
    PrioritySeat() noexcept = default;
    PrioritySeat(const PrioritySeat&) = delete;
    PrioritySeat& operator=(const PrioritySeat&) = delete;
    PrioritySeat(PrioritySeat&& other) noexcept;
    PrioritySeat& operator=(PrioritySeat&& other) noexcept;
    ~PrioritySeat() { set(Priority::kNormal); }

    Priority level() const noexcept { return level_; }
    void set(Priority p) noexcept;

private:
    Priority level_ = Priority::kNormal;
};

}  // namespace kds::server
