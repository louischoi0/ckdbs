#include "kds/sched/sim_ring_transport.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"

// The simulated transport's own contract (docs/spec/sched.md §8, workplan M9):
// injected delay actually delays, injected variance actually reorders, and
// **the whole thing reproduces from the seed**. The last one is the reason
// the class exists - a cross-core failure that cannot be replayed is a
// failure nobody can fix.

namespace kds::sched {
namespace {

std::vector<std::byte> Bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

MessageHeader Msg(std::uint32_t src, std::uint32_t dst, std::uint64_t request_id) {
    MessageHeader h{};
    h.request_id = request_id;
    h.src_core = src;
    h.dst_core = dst;
    h.session_core = src;
    h.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    h.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kForeground);
    return h;
}

std::vector<std::uint64_t> DrainIds(RingTransport& t, std::uint32_t dst) {
    std::vector<std::uint64_t> ids;
    MessageHeader header{};
    std::vector<std::byte> payload;
    while (t.TryReceive(dst, header, payload)) ids.push_back(header.request_id);
    return ids;
}

// One run: send `count` messages on edge 0->1 under `config`, advancing the
// clock between sends so the deadlines actually spread, then drain
// everything. Returns the delivery order.
std::vector<std::uint64_t> RunOnce(SimTransportConfig config, int count) {
    ManualClock clock;
    auto t = SimRingTransport::Create(2, clock, config);
    EXPECT_TRUE(t.ok()) << t.status().message();
    for (int i = 0; i < count; ++i) {
        EXPECT_TRUE(t.value().TrySend(Msg(0, 1, i), Bytes("p")).ok());
        clock.Advance(1);
    }
    // Far past every possible deadline, so the order under test is the
    // deadline order and not "what happened to be ready".
    clock.Advance(1'000'000);
    return DrainIds(t.value(), 1);
}

TEST(SimRingTransportTest, ZeroDelayDeliversImmediatelyAndInSendOrder) {
    ManualClock clock;
    auto t = SimRingTransport::Create(2, clock);  // defaults: no delay
    ASSERT_TRUE(t.ok());

    for (std::uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(t.value().TrySend(Msg(0, 1, i), Bytes("p")).ok());
    }
    // No clock advance at all: the messages are deliverable the moment they
    // are sent.
    EXPECT_EQ(DrainIds(t.value(), 1), (std::vector<std::uint64_t>{0, 1, 2, 3}));
}

TEST(SimRingTransportTest, AMessageIsNotDeliverableBeforeItsDeadline) {
    ManualClock clock;
    SimTransportConfig config;
    config.min_delay_ns = 100;
    config.max_delay_ns = 100;  // fixed, so the deadline is exactly known
    auto t = SimRingTransport::Create(2, clock, config);
    ASSERT_TRUE(t.ok());

    ASSERT_TRUE(t.value().TrySend(Msg(0, 1, 7), Bytes("p")).ok());
    EXPECT_EQ(t.value().in_flight(), 1u);

    MessageHeader header{};
    std::vector<std::byte> payload;
    clock.SetNow(99);
    EXPECT_FALSE(t.value().TryReceive(1, header, payload)) << "delivered early";

    clock.SetNow(100);  // the deadline is inclusive, as timers' are
    ASSERT_TRUE(t.value().TryReceive(1, header, payload));
    EXPECT_EQ(header.request_id, 7u);
    EXPECT_EQ(t.value().in_flight(), 0u);
}

TEST(SimRingTransportTest, VariableDelayReordersMessagesOnOneEdge) {
    SimTransportConfig config;
    config.min_delay_ns = 0;
    config.max_delay_ns = 1000;
    config.seed = 12345;

    const auto order = RunOnce(config, 12);
    ASSERT_EQ(order.size(), 12u);

    // Every message arrives exactly once...
    std::vector<std::uint64_t> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    for (std::uint64_t i = 0; i < 12; ++i) EXPECT_EQ(sorted[i], i);

    // ...and not in send order, which is the injection doing its job. A
    // reorder that never happens would make every test using it a test of
    // the in-order case.
    std::vector<std::uint64_t> in_order(12);
    for (std::uint64_t i = 0; i < 12; ++i) in_order[i] = i;
    EXPECT_NE(order, in_order) << "variable delay produced no reordering at all";
}

TEST(SimRingTransportTest, TheSameSeedGivesTheSameDeliveryOrder) {
    // The property the whole class is for: a failure reproduces from
    // (seed, build) alone (sched.md §8).
    SimTransportConfig config;
    config.min_delay_ns = 0;
    config.max_delay_ns = 1000;
    config.seed = 0xABCD'EF01;

    const auto first = RunOnce(config, 16);
    const auto second = RunOnce(config, 16);
    EXPECT_EQ(first, second);
}

TEST(SimRingTransportTest, ADifferentSeedGivesADifferentDeliveryOrder) {
    // Not a correctness property, but the one that makes seed-sweeping in
    // CI worth doing: if every seed produced the same schedule, running
    // many of them would explore nothing.
    SimTransportConfig a;
    a.min_delay_ns = 0;
    a.max_delay_ns = 1000;
    a.seed = 1;
    SimTransportConfig b = a;
    b.seed = 2;

    EXPECT_NE(RunOnce(a, 16), RunOnce(b, 16));
}

TEST(SimRingTransportTest, AFullEdgeIsResourceExhaustedJustAsARingIs) {
    // Backpressure has to be reachable under simulation, or M7's retry path
    // ships untested.
    ManualClock clock;
    SimTransportConfig config;
    config.capacity_per_edge = 2;
    auto t = SimRingTransport::Create(2, clock, config);
    ASSERT_TRUE(t.ok());

    ASSERT_TRUE(t.value().TrySend(Msg(0, 1, 1), Bytes("a")).ok());
    ASSERT_TRUE(t.value().TrySend(Msg(0, 1, 2), Bytes("b")).ok());
    EXPECT_EQ(t.value().TrySend(Msg(0, 1, 3), Bytes("c")).code(),
              StatusCode::kResourceExhausted);

    // And it is per edge: a different pair is unaffected, matching the real
    // transport's per-core-pair rings.
    EXPECT_TRUE(t.value().TrySend(Msg(1, 0, 4), Bytes("d")).ok());
}

TEST(SimRingTransportTest, ABackwardsDelayRangeIsRefused) {
    ManualClock clock;
    SimTransportConfig config;
    config.min_delay_ns = 100;
    config.max_delay_ns = 10;
    EXPECT_EQ(SimRingTransport::Create(2, clock, config).status().code(),
              StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::sched
