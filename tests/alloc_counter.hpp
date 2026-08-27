#pragma once

#include <cstddef>

// A global allocation counter for tests that assert a code path allocates
// nothing.
//
// It works by **replacing the global operators for the whole test binary**
// (alloc_counter.cpp), which is why it lives here rather than in whichever
// test needed it first: two files each defining `operator new` is a
// duplicate symbol, so there can only ever be one counter and every test
// that wants one has to share it. Counting is off unless a
// `CountAllocations` is alive, so every other test in the binary pays one
// predicate per allocation and nothing else.
//
// Two users today, asserting the same property in different places: the
// aggregate fold's zero-allocations-per-row rule (docs/spec/aggregate.md
// §5) and the row codec's int-only decode (docs/spec/types.md TY5).

namespace kds::test_support {

extern std::size_t g_allocations;
extern bool g_counting;

// Counts allocations for as long as it is alive. Not nestable - the inner
// scope's destructor stops counting for the outer one too, which no caller
// needs and which a flag rather than a depth makes obvious.
struct CountAllocations {
    CountAllocations() {
        g_allocations = 0;
        g_counting = true;
    }
    ~CountAllocations() { g_counting = false; }

    CountAllocations(const CountAllocations&) = delete;
    CountAllocations& operator=(const CountAllocations&) = delete;

    std::size_t count() const { return g_allocations; }
};

}  // namespace kds::test_support
