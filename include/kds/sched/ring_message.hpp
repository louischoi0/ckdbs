#pragma once

#include <cstddef>
#include <cstdint>

#include "kds/sched/task.hpp"

// The cross-core message: what one core sends another, and the only thing
// it ever sends (docs/sched.md §5, docs/workplan-crosscore.md P1 and
// guideline 1). Every kind the engine will ever send is enumerated here,
// centrally, so no subsystem invents a parallel numbering.
//
// ---- Why this struct is not under the on-disk rules ---------------------
//
// Every other fixed-layout struct in this codebase (heap_page.hpp,
// superblock.hpp, catalog/rows.hpp) carries named offset constants, a
// field-wise memcpy codec, and a ban on compiler bitfields, because it
// describes bytes that outlive the process and must be read by a build that
// is not this one.
//
// **A ring message is none of those things.** It is written and read by two
// threads of the same process, from the same build, through memory that is
// never persisted and never leaves the machine. Portability of the
// *encoding* is not a property it needs, so a plain struct with a
// static_asserted size is the honest representation and an offset table
// would be ceremony. Stated explicitly because the surrounding convention
// is the opposite one, and the next reader is entitled to know this
// exception was decided rather than overlooked.
//
// What it does still owe: fixed-width integer types only, no padding
// surprises (asserted below), and trivially copyable, since the ring copies
// it byte for byte.

namespace kds::sched {

// What a message asks the receiving core to do.
//
// **0 is `kUnset` and names nothing.** The same zero-collision rule
// `StoredAccessKind`, `kCabinOriginUnset` and `stmt_class` each had to be
// taught: a zeroed buffer must not decode as a real value.
//
// The step kinds are `docs/crosscore.md` §3's six, declared now though
// nothing sends them until workplan P4. Declaring them early costs a line
// each and is what keeps the pipeline from arriving with an enum of its
// own - P1's "kinds enumerated centrally" is a structural requirement, not
// a tidiness one. The system kinds are the four P1 names.
enum class RingMessageKind : std::uint16_t {
    kUnset = 0,

    // ---- Cross-core step pipeline (crosscore.md §3) --------------------
    // Not sent yet. P4 owns all six.
    kStepOpen = 1,    // session -> step core: the step descriptor
    kStepBatch = 2,   // step k -> step k+1 (or session): encoded rows
    kStepEof = 3,     // upstream -> downstream: no more batches
    kStepCredit = 4,  // downstream -> upstream: grants batch credits
    kStepCancel = 5,  // any -> any: stop producing, discard tagged state
    kStepError = 6,   // failing core -> downstream + session

    // ---- System services (workplan P1, owned by P5/P6) ------------------
    // Not sent yet either.
    kAnchorWrite = 16,        // -> core 0: publish a WAL checkpoint anchor
    kExtentLease = 17,        // -> core 0: request an extent for file growth
    kTrxIdLease = 18,         // -> core 0: request a transaction-id block
    kCatalogInvalidate = 19,  // core 0 -> all: DDL happened, drop caches
};

// Whether `kind` names something this build knows. Callers use it in place
// of a raw cast, for the reason the enum's 0 exists.
constexpr bool IsKnownRingMessageKind(std::uint16_t kind) noexcept {
    switch (static_cast<RingMessageKind>(kind)) {
        case RingMessageKind::kStepOpen:
        case RingMessageKind::kStepBatch:
        case RingMessageKind::kStepEof:
        case RingMessageKind::kStepCredit:
        case RingMessageKind::kStepCancel:
        case RingMessageKind::kStepError:
        case RingMessageKind::kAnchorWrite:
        case RingMessageKind::kExtentLease:
        case RingMessageKind::kTrxIdLease:
        case RingMessageKind::kCatalogInvalidate:
            return true;
        case RingMessageKind::kUnset:
            return false;
    }
    return false;
}

const char* RingMessageKindName(RingMessageKind kind) noexcept;

// The header every message carries.
//
// The `(session_core, request_id, step_id)` triple is `crosscore.md` §3's
// tag, and it is on **every** message rather than only on pipeline ones so
// that the discard rule has one shape: a core receiving a message whose tag
// matches no live state drops it silently, and that is normal operation
// rather than an error (workplan guideline 5). A message must therefore
// remain *processable* after the request that caused it is gone - which is
// why nothing here is a pointer.
struct MessageHeader {
    // `request_id` is allocated per statement by the session core and is
    // **sequential per core**, never pointer-derived (sched.md §8's
    // determinism rules). Zero means "no request" - a system message that
    // belongs to no statement.
    std::uint64_t request_id;

    std::uint32_t src_core;
    std::uint32_t dst_core;

    // The core that owns the statement, which is not necessarily either
    // endpoint: a batch flowing from step k to step k+1 crosses two cores
    // that are both downstream of a third.
    std::uint32_t session_core;

    std::uint32_t step_id;

    std::uint16_t kind;  // RingMessageKind

    // The scheduling group the receiving core should run this message's
    // task in - **designated by the sender** (sched.md §5). It travels in
    // the message rather than being derived from the kind because the same
    // kind can be foreground or maintenance work depending on what asked
    // for it; crosscore.md CC6 puts step traffic in `foreground` because
    // step chains are the OLTP path, and a future background sender of the
    // same kind must be able to say otherwise.
    std::uint16_t sched_group;  // sched::SchedulingGroup

    std::uint32_t payload_len;
};

// No padding, so a ring slot holds exactly what it looks like it holds.
static_assert(sizeof(MessageHeader) == 32);
static_assert(alignof(MessageHeader) == 8);

// The scheduling group a header names, or `kForeground` for a value this
// build does not know. Defaulting rather than failing is deliberate: an
// unrecognized group is a message from a build that disagrees with this one
// about the group list, and running its work at OLTP priority is the
// conservative reading - the alternative silently starves it.
inline SchedulingGroup GroupOf(const MessageHeader& header) noexcept {
    const int index = static_cast<int>(header.sched_group);
    if (index < 0 || index >= kNumSchedulingGroups) return SchedulingGroup::kForeground;
    return static_cast<SchedulingGroup>(index);
}

}  // namespace kds::sched
