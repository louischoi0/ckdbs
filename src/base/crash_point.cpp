#include "kds/base/crash_point.hpp"

#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <string>

namespace kds::base {
namespace {

// The parsed arm, built once on the first hit and never rebuilt. `getenv`
// is called exactly once for the life of the process, so a later `setenv`
// from inside the process cannot arm a point that was not armed at start -
// which is the property the header promises.
const CrashPointArm& ArmOnce() noexcept {
    static const CrashPointArm arm = [] {
        const char* raw = std::getenv("KDS_CRASH_POINT");
        if (raw == nullptr) return CrashPointArm{};
        return ParseCrashPointArm(raw);
    }();
    return arm;
}

// One counter serves every point, because exactly one name can be armed:
// a hit of any other name never reaches it.
std::atomic<std::uint64_t> hits{0};

}  // namespace

CrashPointArm ParseCrashPointArm(std::string_view spec) noexcept {
    CrashPointArm arm;
    if (spec.empty()) return arm;
    const std::size_t colon = spec.rfind(':');
    if (colon != std::string_view::npos && colon + 1 < spec.size()) {
        // Parsed by hand rather than by `stoull`, which throws: this runs
        // from a `noexcept` function on a path that must not take the
        // process down over a malformed environment.
        std::uint64_t n = 0;
        bool digits = true;
        for (std::size_t i = colon + 1; i < spec.size(); ++i) {
            if (spec[i] < '0' || spec[i] > '9') {
                digits = false;
                break;
            }
            n = n * 10 + static_cast<std::uint64_t>(spec[i] - '0');
        }
        if (digits && n > 0) {
            arm.ordinal = n;
            arm.name = std::string(spec.substr(0, colon));
            return arm;
        }
    }
    arm.name = std::string(spec);
    return arm;
}

std::string_view ArmedCrashPoint() noexcept { return ArmOnce().name; }

void CrashPointHit(std::string_view name) noexcept {
    const CrashPointArm& arm = ArmOnce();
    if (arm.name.empty() || arm.name != name) return;
    if (hits.fetch_add(1, std::memory_order_relaxed) + 1 != arm.ordinal) return;

    // Announced on fd 2 before the kill, so a harness that finds a dead
    // process can say *which* point killed it rather than inferring it from
    // the arm it set. Written with `write` and not a stream: nothing here
    // may leave anything buffered, since the next line is the kill.
    //
    // The write is to the process's stderr and touches neither the WAL nor
    // the page device, so the durable state a restart meets is exactly the
    // state at the call site.
    const std::string line = "kds: crash point '" + std::string(name) + "' fired; SIGKILL\n";
    ssize_t ignored = ::write(2, line.data(), line.size());
    (void)ignored;

    // `kill(getpid())` and not `raise`: on a multi-threaded process `raise`
    // is defined to signal the calling *thread*, and while SIGKILL is not
    // catchable either way, the process-directed form is what an external
    // `kill -9` sends and so is the thing being simulated.
    ::kill(::getpid(), SIGKILL);

    // Unreachable. SIGKILL cannot be caught, blocked or ignored, so control
    // does not come back here; there is nothing to fall through to.
}

}  // namespace kds::base
