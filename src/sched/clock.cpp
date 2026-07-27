#include "kds/sched/clock.hpp"

#include <chrono>

namespace kds::sched {

MonoTimeNs SystemClock::Now() const {
    return static_cast<MonoTimeNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace kds::sched
