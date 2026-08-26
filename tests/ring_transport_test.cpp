#include "kds/sched/ring_transport.hpp"

#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/sim_ring_transport.hpp"

// The transport seam (docs/inflight/in-progress/workplan-crosscore.md M9): two implementations,
// one interface, and the property that makes the simulated one usable as a
// stand-in - **at zero injected delay they deliver identically**.
//
// That equivalence is what lets every later cross-core test run under
// simulation without arguing about whether the simulation is the thing
// being tested.

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

MessageHeader Msg(std::uint32_t src, std::uint32_t dst, std::uint64_t request_id) {
    MessageHeader h{};
    h.request_id = request_id;
    h.src_core = src;
    h.dst_core = dst;
    h.session_core = src;
    h.step_id = 0;
    h.kind = static_cast<std::uint16_t>(RingMessageKind::kStepBatch);
    h.sched_group = static_cast<std::uint16_t>(SchedulingGroup::kForeground);
    return h;
}

// Drains everything deliverable to `dst`, in delivery order.
std::vector<std::string> DrainAll(RingTransport& t, std::uint32_t dst) {
    std::vector<std::string> got;
    MessageHeader header{};
    std::vector<std::byte> payload;
    while (t.TryReceive(dst, header, payload)) {
        got.push_back(std::to_string(header.request_id) + ":" + ToString(payload));
    }
    return got;
}

// ---- The seam's shared contract ---------------------------------------

TEST(RingTransportTest, AMessageArrivesAtItsDestinationAndNowhereElse) {
    auto t = RealRingTransport::Create(/*core_count=*/3, /*capacity_slots=*/8,
                                       /*max_payload=*/64);
    ASSERT_TRUE(t.ok()) << t.status().message();

    ASSERT_TRUE(t.value().TrySend(Msg(0, 2, 1), Bytes("hello")).ok());

    // Nothing for the cores it was not addressed to.
    EXPECT_TRUE(DrainAll(t.value(), 0).empty());
    EXPECT_TRUE(DrainAll(t.value(), 1).empty());
    EXPECT_EQ(DrainAll(t.value(), 2), (std::vector<std::string>{"1:hello"}));
}

TEST(RingTransportTest, ACoreMayMessageItself) {
    // The degenerate case of the same protocol. Excluding it would put a
    // special case at every call site that computes a destination.
    auto t = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(t.ok());

    ASSERT_TRUE(t.value().TrySend(Msg(1, 1, 5), Bytes("self")).ok());
    EXPECT_EQ(DrainAll(t.value(), 1), (std::vector<std::string>{"5:self"}));
}

TEST(RingTransportTest, AnOutOfRangeCoreIsRefused) {
    auto t = RealRingTransport::Create(2, 8, 64);
    ASSERT_TRUE(t.ok());

    EXPECT_EQ(t.value().TrySend(Msg(0, 5, 1), Bytes("x")).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(t.value().TrySend(Msg(5, 0, 1), Bytes("x")).code(), StatusCode::kInvalidArgument);
}

TEST(RingTransportTest, OneEdgeFillingUpDoesNotBlockAnother) {
    // Per-core-pair rings, not one shared queue: that is the whole reason
    // the matrix is N² (sched.md §5). A saturated 0->2 must leave 1->2
    // alone.
    auto t = RealRingTransport::Create(3, /*capacity_slots=*/2, 64);
    ASSERT_TRUE(t.ok());

    ASSERT_TRUE(t.value().TrySend(Msg(0, 2, 1), Bytes("a")).ok());
    ASSERT_TRUE(t.value().TrySend(Msg(0, 2, 2), Bytes("b")).ok());
    EXPECT_EQ(t.value().TrySend(Msg(0, 2, 3), Bytes("c")).code(), StatusCode::kResourceExhausted);

    EXPECT_TRUE(t.value().TrySend(Msg(1, 2, 4), Bytes("d")).ok());
}

TEST(RingTransportTest, NoPeerIsStarvedByABusierOne) {
    // The rotating sweep in TryReceive. Without it, a core 0 with something
    // to say on every drain would mean core 2 never hears from core 1.
    auto t = RealRingTransport::Create(3, 16, 64);
    ASSERT_TRUE(t.ok());

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(t.value().TrySend(Msg(0, 2, 100 + i), Bytes("from0")).ok());
        ASSERT_TRUE(t.value().TrySend(Msg(1, 2, 200 + i), Bytes("from1")).ok());
    }

    int from0 = 0;
    int from1 = 0;
    MessageHeader header{};
    std::vector<std::byte> payload;
    while (t.value().TryReceive(2, header, payload)) {
        if (header.src_core == 0) ++from0;
        if (header.src_core == 1) ++from1;
    }
    EXPECT_EQ(from0, 4);
    EXPECT_EQ(from1, 4);
}

// ---- The single-core fast path (workplan guideline 2) -----------------

TEST(RingTransportTest, ASingleCoreTransportCarriesNothing) {
    // The `cores = 1` build must contribute zero messages. The transport
    // exists (it is constructed unconditionally), and nothing goes through
    // it, because there is no peer to send to.
    sched::ManualClock clock;
    auto sim = SimRingTransport::Create(/*core_count=*/1, clock);
    ASSERT_TRUE(sim.ok());

    EXPECT_EQ(sim.value().core_count(), 1u);
    EXPECT_EQ(sim.value().sent_count(), 0u);
    EXPECT_EQ(sim.value().in_flight(), 0u);

    MessageHeader header{};
    std::vector<std::byte> payload;
    EXPECT_FALSE(sim.value().TryReceive(0, header, payload));
}

// ---- Equivalence of the two implementations ---------------------------

// Drains everything deliverable to `dst`, splitting the result by source
// core - which is the granularity at which the two implementations are
// required to agree. See the test below for why it is not the whole stream.
std::map<std::uint32_t, std::vector<std::string>> DrainByPeer(RingTransport& t,
                                                              std::uint32_t dst) {
    std::map<std::uint32_t, std::vector<std::string>> got;
    MessageHeader header{};
    std::vector<std::byte> payload;
    while (t.TryReceive(dst, header, payload)) {
        got[header.src_core].push_back(std::to_string(header.request_id) + ":" +
                                        ToString(payload));
    }
    return got;
}

TEST(RingTransportTest, TheSimulatedTransportMatchesTheRealOnePerEdgeAtZeroDelay) {
    // M9's substitutability claim, stated as the comparison it actually is.
    //
    // **The agreement is per edge, not over the whole inbox**, and that is
    // a real difference rather than a test convenience. The real transport
    // sweeps its peers in rotation so no peer starves; the simulation
    // delivers by deadline. Two messages sent from *different* cores
    // therefore arrive in an order the two implementations do not have to
    // share - and neither does the network, which is exactly what the
    // reorder injection exists to prove. Nothing above this layer may
    // depend on the relative order of two peers' messages.
    //
    // What both do guarantee, and what every protocol above them is
    // entitled to: messages on one edge arrive in send order, and no
    // message is invented, lost, or duplicated.
    constexpr std::uint32_t kCores = 3;

    auto real = RealRingTransport::Create(kCores, 32, 64);
    ASSERT_TRUE(real.ok());
    sched::ManualClock clock;
    auto sim = SimRingTransport::Create(kCores, clock);  // zero delay by default
    ASSERT_TRUE(sim.ok());

    const std::vector<std::pair<std::uint32_t, std::uint32_t>> traffic = {
        {0, 1}, {0, 1}, {2, 1}, {0, 1}, {2, 1}, {1, 0}, {2, 0},
    };
    for (std::size_t i = 0; i < traffic.size(); ++i) {
        const auto header = Msg(traffic[i].first, traffic[i].second, i);
        const auto payload = Bytes("m" + std::to_string(i));
        ASSERT_TRUE(real.value().TrySend(header, payload).ok());
        ASSERT_TRUE(sim.value().TrySend(header, payload).ok());
    }

    for (std::uint32_t dst = 0; dst < kCores; ++dst) {
        EXPECT_EQ(DrainByPeer(real.value(), dst), DrainByPeer(sim.value(), dst))
            << "the transports disagreed at destination " << dst;
    }
}

TEST(RingTransportTest, BothTransportsKeepOneEdgeInSendOrder) {
    // The half of the contract above that anything may depend on, asserted
    // directly rather than only through the comparison.
    sched::ManualClock clock;
    auto sim = SimRingTransport::Create(2, clock);
    ASSERT_TRUE(sim.ok());
    auto real = RealRingTransport::Create(2, 32, 64);
    ASSERT_TRUE(real.ok());

    for (std::uint64_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(real.value().TrySend(Msg(0, 1, i), Bytes("p")).ok());
        ASSERT_TRUE(sim.value().TrySend(Msg(0, 1, i), Bytes("p")).ok());
    }

    const std::vector<std::string> expected = {"0:p", "1:p", "2:p", "3:p", "4:p", "5:p"};
    EXPECT_EQ(DrainAll(real.value(), 1), expected);
    EXPECT_EQ(DrainAll(sim.value(), 1), expected);
}

}  // namespace
}  // namespace kds::sched
