#include "kds/server/priority.hpp"

#include <array>
#include <string>
#include <utility>

#include "kds/sched/core_load.hpp"

namespace kds::server {

namespace {

constexpr std::array<std::string_view, 4> kNames = {"transactional", "normal", "analytic",
                                                    "background"};

bool IEqualsAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (x != b[i]) return false;
    }
    return true;
}

}  // namespace

StatusOr<Priority> ParsePriority(std::string_view name) {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (IEqualsAscii(name, kNames[i])) return static_cast<Priority>(i);
    }
    return Status::InvalidArgument("unknown priority '" + std::string(name) +
                                   "'; expected transactional, normal, analytic or background");
}

std::string_view PriorityName(Priority p) noexcept {
    return kNames[static_cast<std::size_t>(p)];
}

PrioritySeat::PrioritySeat(PrioritySeat&& other) noexcept
    : level_(std::exchange(other.level_, Priority::kNormal)) {}

PrioritySeat& PrioritySeat::operator=(PrioritySeat&& other) noexcept {
    if (this != &other) {
        set(Priority::kNormal);
        level_ = std::exchange(other.level_, Priority::kNormal);
    }
    return *this;
}

void PrioritySeat::set(Priority p) noexcept {
    if (IsBatchPriority(p) != IsBatchPriority(level_)) {
        if (IsBatchPriority(p)) {
            sched::CoreLoad::BatchSessions().fetch_add(1, std::memory_order_relaxed);
        } else {
            sched::CoreLoad::BatchSessions().fetch_sub(1, std::memory_order_relaxed);
        }
    }
    level_ = p;
}

}  // namespace kds::server
