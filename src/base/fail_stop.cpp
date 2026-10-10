#include "kds/base/fail_stop.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace kds::base {

void FailStop(const char* what) noexcept {
    // `fprintf` to unbuffered stderr takes no allocation, which is what the
    // out-of-memory path needs.
    std::fprintf(stderr, "kds: %s; stopping\n", what);
    std::fflush(stderr);
    std::abort();
}

void InstallAllocationFailStop() noexcept {
    std::set_new_handler([] { FailStop("allocation failed"); });
}

}  // namespace kds::base
