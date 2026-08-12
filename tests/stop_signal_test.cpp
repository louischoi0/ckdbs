#include "kds/server/stop_signal.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>

#include <gtest/gtest.h>

// `SIGTERM`/`SIGINT` as an fd (`server/stop_signal.hpp`), which is what makes a
// process-manager stop reach the same shutdown path a client's `STOP` does.
//
// **These tests change the process's signal mask, and put it back.** Blocking
// SIGTERM is the mechanism under test, and a test binary that exits with it
// still blocked is a test binary `ctest` cannot time out - so every case here
// restores the mask, and `MaskRestored` is the case that checks the restoring
// rather than trusting it.

namespace kds::server {
namespace {

// Unblocks both signals again, whatever the test did. Not a fixture teardown by
// accident: it is the thing that keeps a failure in one case from making every
// later case in the binary unkillable.
void RestoreMask() {
    sigset_t mask;
    ASSERT_EQ(sigemptyset(&mask), 0);
    ASSERT_EQ(sigaddset(&mask, SIGTERM), 0);
    ASSERT_EQ(sigaddset(&mask, SIGINT), 0);
    ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &mask, nullptr), 0);
}

bool ReadableWithin(int fd, int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) == 1 && (pfd.revents & POLLIN) != 0;
}

TEST(StopSignalTest, ADeliveredSigtermBecomesAReadableFd) {
    auto stop = StopSignal::Install();
    ASSERT_TRUE(stop.ok()) << stop.status().message();
    ASSERT_TRUE(stop.value().installed());

    // Not readable before anything is delivered: a reactor registering this must
    // not be woken at once and stop a server nobody asked to stop.
    EXPECT_FALSE(ReadableWithin(stop.value().fd(), 0));

    ASSERT_EQ(::raise(SIGTERM), 0) << "blocked, so this must not terminate the test";
    EXPECT_TRUE(ReadableWithin(stop.value().fd(), 1000))
        << "the signal was delivered and the fd never became readable";
    EXPECT_EQ(stop.value().Consume(), static_cast<std::uint32_t>(SIGTERM));

    // Drained: without this a level-triggered epoll reports the same delivery
    // forever and the reactor spins instead of shutting down.
    EXPECT_FALSE(ReadableWithin(stop.value().fd(), 0)) << "Consume() left the fd readable";

    RestoreMask();
}

TEST(StopSignalTest, SigintIsTheSameDoorAndSaysWhichItWas) {
    auto stop = StopSignal::Install();
    ASSERT_TRUE(stop.ok()) << stop.status().message();

    ASSERT_EQ(::raise(SIGINT), 0);
    ASSERT_TRUE(ReadableWithin(stop.value().fd(), 1000));
    // The number is not needed to shut down - one readable event is the whole
    // message - but "stopped on SIGINT" and "stopped on SIGTERM" are different
    // sentences in an operator's log.
    EXPECT_EQ(stop.value().Consume(), static_cast<std::uint32_t>(SIGINT));

    RestoreMask();
}

TEST(StopSignalTest, ADefaultConstructedOneInstallsNothingAndIsSafeToIgnore) {
    // What every in-process caller and every test gets: no signal handling, so a
    // library user does not inherit a mask it never asked for.
    StopSignal none;
    EXPECT_FALSE(none.installed());
    EXPECT_EQ(none.fd(), -1);
    EXPECT_EQ(none.Consume(), 0u);  // no fd to read, and not a crash
}

TEST(StopSignalTest, TheDescriptorIsClosedWithTheObject) {
    int fd = -1;
    {
        auto stop = StopSignal::Install();
        ASSERT_TRUE(stop.ok()) << stop.status().message();
        fd = stop.value().fd();
        ASSERT_GE(fd, 0);
    }
    // Closed by the destructor: the server installs one per process, but a test
    // installs several, and a leak here would exhaust the descriptor table of
    // whatever runs enough of them.
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1) << "the descriptor outlived its owner";

    RestoreMask();
}

TEST(StopSignalTest, MaskRestored) {
    // The guard on the guard: if an earlier case left SIGTERM blocked, this
    // binary would not be killable by the harness that runs it, and that failure
    // would show up as an unrelated test hanging rather than as this one failing.
    //
    // **It only bites when the binary is run whole.** `gtest_discover_tests`
    // gives ctest one process per test, so under ctest this case starts with a
    // fresh mask and passes trivially. Kept because running the whole binary is
    // what a developer does, and that is the mode where a leak would be real.
    sigset_t mask;
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, nullptr, &mask), 0);
    EXPECT_EQ(sigismember(&mask, SIGTERM), 0) << "SIGTERM is still blocked";
    EXPECT_EQ(sigismember(&mask, SIGINT), 0) << "SIGINT is still blocked";
}

}  // namespace
}  // namespace kds::server
