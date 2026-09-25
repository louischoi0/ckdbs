#include "kds/server/connection_handoff.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

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

    const std::vector<int> taken = handoff.Take(1);
    ASSERT_EQ(taken, std::vector<int>{pipe_fds[0]});
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

}  // namespace
}  // namespace kds::server
