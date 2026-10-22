#include "kds/server/connection_handoff.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/priority.hpp"

// D19's fallback (AT-S10c, `connection_handoff.hpp`): core 0 accepts and
// places each connection in the destination's inbox, then kicks it. These
// cells pin the inbox itself; `ExpeditorTest` pins the instance running on
// it.

namespace kds::server {
namespace {

// A registry that records kicks instead of ending anyone's block.
class CountingWakers final : public sched::WakeRegistry {
public:
    void Register(std::uint32_t, const std::atomic<bool>*, const sched::Waker*) override {}
    void Kick(std::uint32_t core) const noexcept override {
        kicks_[core].fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t kicks() const noexcept override { return kicks_[0] + kicks_[1] + kicks_[2]; }
    std::uint64_t KicksTo(std::uint32_t core) const noexcept { return kicks_[core]; }

private:
    mutable std::atomic<std::uint64_t> kicks_[3] = {};
};

bool IsOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

TEST(ConnectionHandoffTest, NextCoreGoesRoundEveryCoreCoreZeroIncluded) {
    ConnectionHandoff handoff(3, nullptr);
    std::vector<std::uint32_t> seen;
    for (int i = 0; i < 6; ++i) seen.push_back(handoff.NextCore());
    EXPECT_EQ(seen, (std::vector<std::uint32_t>{0, 1, 2, 0, 1, 2}));
}

// **While the partition is active the round-robin passes over the batch
// cores** (BL-R5, BL-S4): an accepted session is `normal`, so it lands on an
// interactive core - at three cores, every core but core 0.
//
// **The mutation**: drop the `partitioned` test and core 0 is handed one.
TEST(ConnectionHandoffTest, WhileThePartitionIsActiveNoAcceptLandsOnABatchCore) {
    ConnectionHandoff handoff(3, nullptr);
    PrioritySeat batch;
    batch.set(Priority::kBackground);
    std::vector<std::uint32_t> seen;
    for (int i = 0; i < 4; ++i) seen.push_back(handoff.NextCore());
    EXPECT_EQ(seen, (std::vector<std::uint32_t>{1, 2, 1, 2}));
    batch.set(Priority::kNormal);
    seen.clear();
    for (int i = 0; i < 3; ++i) seen.push_back(handoff.NextCore());
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_NE(std::find(seen.begin(), seen.end(), 0u), seen.end())
        << "core 0 was still passed over with no batch session";
}

TEST(ConnectionHandoffTest, AnOfferIsPendingForItsCoreAloneAndKicksItAfterPublishing) {
    CountingWakers wakers;
    ConnectionHandoff handoff(3, &wakers);
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);

    EXPECT_FALSE(handoff.Pending(1));
    handoff.Offer(1, pipe_fds[0]);
    // AR0-6-R1: the state first, then the kick - and to its core only.
    EXPECT_TRUE(handoff.Pending(1));
    EXPECT_FALSE(handoff.Pending(2));
    EXPECT_EQ(wakers.KicksTo(1), 1u);
    EXPECT_EQ(wakers.KicksTo(2), 0u);

    const std::vector<ConnectionHandoff::Arrival> taken = handoff.Take(1);
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].fd, pipe_fds[0]);
    EXPECT_EQ(taken[0].moved, nullptr) << "an accepted socket arrived as a live connection";
    EXPECT_FALSE(handoff.Pending(1)) << "a taken inbox still reads as pending";
    EXPECT_TRUE(handoff.Take(1).empty()) << "a socket was handed out twice";

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
}

TEST(ConnectionHandoffTest, ASocketNobodyTookIsClosedWithTheHandoff) {
    // An instance stopped between core 0's accept and the peer's take: the
    // inbox owns the socket, so it must not leak.
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);
    {
        ConnectionHandoff handoff(2, nullptr);
        handoff.Offer(1, pipe_fds[0]);
        ASSERT_TRUE(IsOpen(pipe_fds[0]));
    }
    EXPECT_FALSE(IsOpen(pipe_fds[0])) << "an untaken socket outlived its inbox";
    ::close(pipe_fds[1]);
}

// BL-S2: a live connection in an inbox nobody took is torn down by its own
// `Transit`, and the inbox does not close the socket under it a second time.
TEST(ConnectionHandoffTest, ALiveConnectionNobodyTookIsTornDownByItsTransit) {
    struct Probe final : ConnectionHandoff::Transit {
        explicit Probe(int* destroyed) : destroyed_(destroyed) {}
        ~Probe() override { ++*destroyed_; }
        int* destroyed_;
    };
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);
    int destroyed = 0;
    {
        ConnectionHandoff handoff(2, nullptr);
        handoff.Offer(1, pipe_fds[0], std::make_unique<Probe>(&destroyed));
    }
    EXPECT_EQ(destroyed, 1) << "an untaken live connection was never torn down";
    EXPECT_TRUE(IsOpen(pipe_fds[0])) << "the inbox closed a socket its transit owns";
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
}

}  // namespace
}  // namespace kds::server
