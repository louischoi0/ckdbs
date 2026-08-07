#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace kds::test_support {

std::size_t g_allocations = 0;
bool g_counting = false;

}  // namespace kds::test_support

void* operator new(std::size_t size) {
    if (kds::test_support::g_counting) ++kds::test_support::g_allocations;
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
