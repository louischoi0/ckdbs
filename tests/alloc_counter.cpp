#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace kds::test_support {

thread_local std::size_t g_allocations = 0;
thread_local bool g_counting = false;
thread_local std::size_t g_fail_after = 0;
thread_local std::size_t g_fail_seen = 0;

}  // namespace kds::test_support

void* operator new(std::size_t size) {
    if (kds::test_support::g_counting) ++kds::test_support::g_allocations;
    if (kds::test_support::g_fail_after != 0 &&
        ++kds::test_support::g_fail_seen == kds::test_support::g_fail_after) {
        kds::test_support::g_fail_after = 0;  // once
        // As the standard's allocation loop does: the new-handler first, and
        // only without one is `std::bad_alloc` thrown.
        if (const std::new_handler handler = std::get_new_handler()) handler();
        throw std::bad_alloc();
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
