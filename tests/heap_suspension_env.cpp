// SUS-1's test-only bypass, armed for the whole test binary.
//
// Heap relations are suspended
// (`instructions/v3.0.0/workorder-as-sus1-heap-suspended.md`): the parser
// refuses `CREATE TABLE ... HEAP` so no new heap relation is created. But
// nothing below the parser changed, and the order is explicit that the heap
// paths must **stay exercised** rather than rot behind the refusal — the
// chain walk, tail append, the below-the-mark `OutOfRange`, redo and undo
// are all still live code serving every volume that already holds a heap
// relation.
//
// So the test binary runs with the suspension lifted, and a cell that means
// to observe the refusal turns it back off with
// `parser::AllowHeapStorageForTest`'s counterpart — the guard restores
// whatever it found, so neither direction leaks between cells.
//
// **This is why the bypass is not `NDEBUG`-gated** (AS-R3): a release test
// build needs it too, or the gate and the nightly run would be different
// engines. It is reachable from no config key and no wire path; a static
// initializer in a test translation unit is the whole of its surface.

#include "kds/parser/parser.hpp"

namespace {

struct HeapSuspensionBypass {
    HeapSuspensionBypass() noexcept { kds::parser::SetHeapStorageAllowedForTest(true); }
};

const HeapSuspensionBypass g_bypass;

}  // namespace
