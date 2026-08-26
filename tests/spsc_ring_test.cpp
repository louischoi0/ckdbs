#include "kds/sched/spsc_ring.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// The single-producer/single-consumer ring (docs/spec/sched.md §5). Everything
// here runs on one thread: what is under test is the index protocol and the
// slot arithmetic, not the memory model, and a two-thread test would only
// make those two things harder to see. The ordering guarantees are asserted
// by construction in the source and are not something a test can observe.

namespace kds::sched {
namespace {

std::vector<std::byte> Bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string ToString(const std::vector<std::byte>& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

MessageHeader HeaderFor(std::uint64_t request_id) {
    MessageHeader h{};
    h.request_id = request_id;
    h.src_core = 1;
    h.dst_core = 2;
    h.session_core = 1;
    h.step_id = 7;
    h.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    h.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kForeground);
    return h;
}

TEST(SpscRingTest, ARoundTripPreservesTheHeaderAndThePayload) {
    auto ring = SpscRing::Create(4, 64);
    ASSERT_TRUE(ring.ok()) << ring.status().message();

    const auto payload = Bytes("some rows");
    ASSERT_TRUE(ring.value().TrySend(HeaderFor(42), payload).ok());

    MessageHeader got{};
    std::vector<std::byte> out;
    ASSERT_TRUE(ring.value().TryReceive(got, out));
    EXPECT_EQ(got.request_id, 42u);
    EXPECT_EQ(got.step_id, 7u);
    EXPECT_EQ(got.kind, static_cast<std::uint16_t>(RingMessageKind::kStepBatch));
    EXPECT_EQ(ToString(out), "some rows");

    // And the length announced is the length copied - TrySend overwrites
    // payload_len so a caller cannot make the two disagree.
    EXPECT_EQ(got.payload_len, payload.size());
}

TEST(SpscRingTest, AnEmptyRingAnswersFalseRatherThanFailing) {
    auto ring = SpscRing::Create(4, 64);
    ASSERT_TRUE(ring.ok());

    MessageHeader got{};
    std::vector<std::byte> out;
    EXPECT_FALSE(ring.value().TryReceive(got, out));
    EXPECT_TRUE(ring.value().empty());
}

TEST(SpscRingTest, AFullRingIsResourceExhaustedAndNotAnError) {
    auto ring = SpscRing::Create(4, 64);
    ASSERT_TRUE(ring.ok());
    ASSERT_EQ(ring.value().capacity(), 4u);

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.value().TrySend(HeaderFor(i), Bytes("x")).ok()) << "at " << i;
    }
    // The fifth is refused. This is backpressure, and the code it carries
    // is what send_retry.hpp keys its whole behaviour on - a different code
    // here would silently turn a stall into a failed statement.
    Status full = ring.value().TrySend(HeaderFor(4), Bytes("x"));
    EXPECT_EQ(full.code(), StatusCode::kResourceExhausted);

    // Draining one makes room for exactly one more.
    MessageHeader got{};
    std::vector<std::byte> out;
    ASSERT_TRUE(ring.value().TryReceive(got, out));
    EXPECT_TRUE(ring.value().TrySend(HeaderFor(4), Bytes("x")).ok());
}

TEST(SpscRingTest, IndicesWrapWithoutLosingOrCorruptingAMessage) {
    auto ring = SpscRing::Create(4, 64);
    ASSERT_TRUE(ring.ok());

    // Many times the capacity, one in and one out, so every send but the
    // first four reuses a slot a receive freed. FIFO order must hold across
    // every wrap.
    for (std::uint64_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(ring.value().TrySend(HeaderFor(i), Bytes("msg" + std::to_string(i))).ok());
        MessageHeader got{};
        std::vector<std::byte> out;
        ASSERT_TRUE(ring.value().TryReceive(got, out));
        EXPECT_EQ(got.request_id, i);
        EXPECT_EQ(ToString(out), "msg" + std::to_string(i));
    }
}

TEST(SpscRingTest, MessagesComeBackInSendOrder) {
    auto ring = SpscRing::Create(8, 64);
    ASSERT_TRUE(ring.ok());

    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring.value().TrySend(HeaderFor(i), Bytes("p")).ok());
    }
    EXPECT_EQ(ring.value().size(), 5u);

    for (std::uint64_t i = 0; i < 5; ++i) {
        MessageHeader got{};
        std::vector<std::byte> out;
        ASSERT_TRUE(ring.value().TryReceive(got, out));
        EXPECT_EQ(got.request_id, i);
    }
    EXPECT_TRUE(ring.value().empty());
}

TEST(SpscRingTest, APayloadLargerThanTheSlotIsRefusedAsAProgrammingError) {
    auto ring = SpscRing::Create(4, 8);
    ASSERT_TRUE(ring.ok());

    // InvalidArgument, deliberately not ResourceExhausted: retrying would
    // never help, so it must not be routed to the retry path.
    Status s = ring.value().TrySend(HeaderFor(1), Bytes("far too long for eight bytes"));
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST(SpscRingTest, AnEmptyPayloadIsALegalMessage) {
    auto ring = SpscRing::Create(4, 64);
    ASSERT_TRUE(ring.ok());

    // STEP_EOF and STEP_CANCEL carry nothing but their tag, so zero bytes
    // has to be a message rather than an absence.
    ASSERT_TRUE(ring.value().TrySend(HeaderFor(9), {}).ok());
    MessageHeader got{};
    std::vector<std::byte> out;
    ASSERT_TRUE(ring.value().TryReceive(got, out));
    EXPECT_EQ(got.request_id, 9u);
    EXPECT_EQ(got.payload_len, 0u);
    EXPECT_TRUE(out.empty());
}

TEST(SpscRingTest, CapacityRoundsUpToAPowerOfTwo) {
    // Rounding up rather than refusing: a caller asking for 100 slots wants
    // at least 100, and the mask arithmetic needs a power of two.
    auto ring = SpscRing::Create(100, 32);
    ASSERT_TRUE(ring.ok());
    EXPECT_EQ(ring.value().capacity(), 128u);
}

TEST(SpscRingTest, DegenerateSizesAreRefused) {
    EXPECT_EQ(SpscRing::Create(0, 32).status().code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(SpscRing::Create(4, 0).status().code(), StatusCode::kInvalidArgument);
}

TEST(SpscRingTest, SteadyStateSendAndReceiveDoNotAllocate) {
    auto ring = SpscRing::Create(8, 128);
    ASSERT_TRUE(ring.ok());

    // The reactor's phase-3 arrangement: one payload buffer reused across
    // every receive. After it has grown once, no further receive may need
    // to grow it - that is what "the loop body performs no allocation in
    // steady state" (sched.md §2) amounts to for this class, and capacity
    // is the observable proxy for it.
    std::vector<std::byte> scratch;
    ASSERT_TRUE(ring.value().TrySend(HeaderFor(1), Bytes(std::string(128, 'a'))).ok());
    MessageHeader got{};
    ASSERT_TRUE(ring.value().TryReceive(got, scratch));
    const std::size_t settled = scratch.capacity();
    ASSERT_GE(settled, 128u);

    for (std::uint64_t i = 0; i < 50; ++i) {
        ASSERT_TRUE(ring.value().TrySend(HeaderFor(i), Bytes(std::string(128, 'b'))).ok());
        ASSERT_TRUE(ring.value().TryReceive(got, scratch));
    }
    EXPECT_EQ(scratch.capacity(), settled) << "a steady-state receive reallocated";
}

}  // namespace
}  // namespace kds::sched
