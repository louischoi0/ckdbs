#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/exec/step_chain.hpp"

// The STEP_OPEN step descriptor (docs/crosscore.md §3, workplan P4a second
// half): a compiled `exec::Step` serialized with full fidelity so the
// owning core executes exactly the step the session core compiled - kind,
// relation oid, key operand, residual predicates, range hint, projection
// set. No statement text travels: shipping the SQL and re-compiling on the
// owner would be a second spelling of the plan, the interim form §4
// already refuses for batches.
//
// **What is refused at encode, by class rather than silently narrowed:**
//
//  - a step carrying **sub-chains** (a predicate-position subquery):
//    shipping one means shipping a whole nested chain, which is P4d's
//    multi-step work, not a field this codec can quietly flatten;
//  - **kCabinProbe / kIndexProbe / kIndexRange** steps: their aux structs
//    carry core-local state (byte-encoded index keys, cabin ids) that has
//    no meaning off the owning core - and selection is f(shape, catalog),
//    so the owner would re-derive them from the shipped shape once P4b
//    re-plans locally. Until it does, the honest answer is Unsupported;
//  - a **kParam literal**: a declared pattern's body never executes
//    (spec-create-pattern §3), so a param in a shipped step is a defect
//    upstream, not a value to encode.
//
// The encoding is explicit little-endian append/read - no struct memcpy,
// because this payload holds variable-length fields (string literals, the
// residual list) and invariant 6 forbids layout-defined encodings for
// anything with a format. A leading version byte makes evolution
// detectable instead of silent.

namespace kds::server {

inline constexpr std::uint8_t kStepDescriptorVersion = 1;

// Serializes `step`. Fails with Unsupported for the refused classes above
// and InvalidArgument for a step no compiler produces (an unknown kind).
StatusOr<std::vector<std::byte>> EncodeStepDescriptor(const exec::Step& step);

// Reconstructs the step. Refuses truncated or version-mismatched bytes
// with InvalidArgument; every enum is range-checked before the cast, so a
// corrupt byte is an error and never an out-of-range enum value.
StatusOr<exec::Step> DecodeStepDescriptor(std::span<const std::byte> bytes);

}  // namespace kds::server
