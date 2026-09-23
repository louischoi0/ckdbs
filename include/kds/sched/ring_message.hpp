#pragma once

#include <cstddef>
#include <cstdint>

#include "kds/sched/task.hpp"

// The cross-core message: what one core sends another, and the only thing
// it ever sends (docs/spec/sched.md §5, docs/inflight/in-progress/workplan-crosscore.md P1 and
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
// The step kinds are `docs/spec/crosscore.md` §3's six, declared now though
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
    // 17 was kExtentLease, struck at AT-S2b: the page-id lease refill went
    // with the per-core pool at AW-S1b. **The value is not reused.**
    kTrxIdLease = 18,         // -> core 0: request a transaction-id block
    // 19 was kCatalogInvalidate, struck at AT-S2b: a peer asks the schema
    // version word at its task boundaries (AT-S2a, `catalog.hpp`) and is no
    // longer told. **The value is not reused.**

    // 20 was kShutdown, struck at AU-S3: stopping a reactor is an atomic
    // flag plus a kick, not a message. **The value is not reused** - a
    // number a stale peer might still send must not come to mean something
    // else, and the gap is the record that it was spent.

    // 21, 23 and 24 were CC7's three relation grants - fault rights over a
    // relation's page range, write rights over its exact creation pages,
    // and an owner's request to have both re-delivered - struck at AT-S2b.
    // Every one answered "can this core reach that page" for a frame table
    // one core owned; one table serves every core since AM-S2 step 3 and
    // nothing sent or handled them after AW-S1b. **The values are not
    // reused**: a struck kind arriving from a stale peer is unknown, and an
    // unknown kind is dropped rather than read as something else.

    // peer <-> core 0: a block of Keystone row ids for one relation
    // (workplan-crosscore.md P5's shape; catalog/row_id_lease.hpp). The
    // request carries `server::RowIdLeaseRequestPayload` and the reply
    // `server::RowIdLeaseGrantPayload`, on this one kind both ways - the
    // page-id lease's arrangement. A zero-count grant means the relation's
    // id space is exhausted; the requester fails honestly, never waits.
    kRowIdLease = 22,

    // 25, 26 and 27 were the index build's request, reply and done - a
    // peer-owned relation's CREATE INDEX built by its owner behind a
    // write-refusal window (PW1c-6b) - struck at AT-S5e: `CREATE INDEX`
    // takes the relation `X` and builds where its session is, and every
    // writer's `IX` waits on it from whichever core. **The values are not
    // reused.**

    // 28 and 29 were kShippedStatementRequest / kShippedStatementReply,
    // struck at AT-S6: a statement no longer crosses to the core that owns
    // its relation. The write's half went at AT-S5 and the read's here,
    // and what replaced both is a walk of pages every core faults
    // (`docs/spec/crosscore.md` §6).


    // 30, 31 and 32 were the assertion build's request, reply and done -
    // a peer-owned relation's CREATE ASSERTION built by its owner (PW1c-6c)
    // - struck at AT-S5d: the assertion registry is the instance's, so the
    // build runs where the session is and adopts into the one directory
    // every core's writes check. **The values are not reused.**

    // 33 to 38 were the two-phase commit's three pairs -
    // kTxnPrepareRequest / Reply, kTxnDecideRequest / Reply and
    // kTxnResolveRequest / Reply - struck at AT-S6 with the protocol.
    // Nothing ships, so no transaction has a half on another core to
    // prepare, to decide, or to be in doubt about
    // (`docs/spec/cross-owner-txn.md`).


    // 39 was kAccessStatsBatch, CR7's one-way flush of a peer's folded
    // access statistics to core 0 - and the one kind on this enum that
    // could be **dropped** rather than retried (CR8), because
    // `sys.access_stats` is invariant 8's advisory class. Struck at AT-S7:
    // the relation sits in the reserved range, which is why a peer had to
    // send rather than write, and every core writes every page since
    // AT-S5. A statistic is recorded where the statement ran, so there is
    // nothing to fold and nothing to drop (`docs/spec/crosscore.md` CC13).
    // **The value is not reused.**

    // owner -> arrival core: the **result description** of a shipped read
    // answered in typed rows (XG1, `docs/spec/crosscore.md` §4a). Sent on
    // the answer edge ahead of the first `kStepBatch`, and **chunked**: the
    // engine has no column-count cap, so a description may exceed a ring
    // message and crosses as an ordered sequence reassembled before the
    // rows arrive.
    //
    // **Its own kind rather than a `kStepBatch` with a flag**, and that is
    // a correctness choice: `StepBatchHeader::seq` is per-edge and asserted
    // contiguous - a receiver that sees a gap has lost a batch - so folding
    // a differently-shaped payload into that sequence would either break
    // the assertion or force description chunks to be counted as batches.
    // Two kinds, two sequences, one tag.
    //
    // Control rather than data, like EOF and CREDIT: it carries no rows and
    // spends no credit.
    kShippedRowDesc = 40,

    // 41 and 42 were kFkProbeRequest / kFkProbeReply and 43 and 44
    // kFkReverseProbeRequest / kFkReverseProbeReply, struck at AT-S5f: the
    // foreign key's forward and reverse checks both run on the core the
    // statement runs on, through the one frame table AM-S2 step 3 made the
    // instance's, so neither direction crosses and there is nothing left
    // for a probe to ask. The reference intent the forward pair granted
    // went with them (`docs/spec/foreign-keys.md` §2a, §3a).
};

// **The census of every kind this build sends or handles, and the number
// AR0-6's D25 freezes** (`raft-marks-2026-09-05.md` §3; written at AT-S2b,
// and given this form by its review). The switch has **no `default`**, so
// under `-Wall` a new enumerator warns here until its case is added; and
// the count below is derived from the switch at compile time, so once the
// case is added the freeze **fails** until the frozen number is moved on
// purpose. AU-R4's rule is that a kind is only ever struck, never
// renumbered and never reused, so the number moves only downward and only
// here: strike the enumerator, drop its case, lower the assert in the same
// change. (An earlier form kept a hand-written `std::array<…, 29>` beside
// the switch; its explicit size let a dropped entry pad to `kUnset`, which
// made the freeze assert restate its own template argument and made
// `kUnset` a "known" kind - the review of AT-S2b is why it is a switch.)
constexpr bool IsKnownRingMessageKind(RingMessageKind kind) noexcept {
    switch (kind) {
        case RingMessageKind::kStepOpen:
        case RingMessageKind::kStepBatch:
        case RingMessageKind::kStepEof:
        case RingMessageKind::kStepCredit:
        case RingMessageKind::kStepCancel:
        case RingMessageKind::kStepError:
        case RingMessageKind::kAnchorWrite:
        case RingMessageKind::kTrxIdLease:
        case RingMessageKind::kRowIdLease:
        case RingMessageKind::kShippedRowDesc:
            return true;
        case RingMessageKind::kUnset:
            return false;
    }
    return false;  // a value no enumerator names: a struck kind from a stale peer
}

// The wire's form: callers hold a `uint16_t` off a `MessageHeader`, and use
// this in place of a raw cast, for the reason the enum's 0 exists.
constexpr bool IsKnownRingMessageKind(std::uint16_t kind) noexcept {
    return IsKnownRingMessageKind(static_cast<RingMessageKind>(kind));
}

// The number the switch above knows, computed from it. Every 16-bit value,
// because the wire field is 16 bits and a bound tied to the highest
// enumerator would be a second number to keep in step; 65,536 steps of a
// switch is far inside the constant-evaluation budget of the compilers
// this builds with.
constexpr std::size_t CountKnownRingMessageKinds() noexcept {
    std::size_t n = 0;
    for (std::uint32_t v = 0; v <= 0xFFFFu; ++v) {
        if (IsKnownRingMessageKind(static_cast<std::uint16_t>(v))) ++n;
    }
    return n;
}
static_assert(CountKnownRingMessageKinds() == 10,
              "AR0-6 D25: the ring's kind count is frozen and moves only by a strike - 34 at "
              "AU-S3, 29 at AT-S2b (17, 19, 21, 23, 24 struck), 26 at AT-S5d (30, 31, 32 "
              "struck), 23 at AT-S5e (25, 26, 27 struck), 19 at AT-S5f (41, 42, 43, 44 "
              "struck), 11 at AT-S6 (28, 29 and 33-38 struck), 10 at AT-S7 (39 struck)");

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
