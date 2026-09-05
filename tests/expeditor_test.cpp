#include "kds/server/expeditor.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

// **The multi-core assembly, run rather than modelled** (AM-S0(a)).
//
// Every other server-level test hand-builds the shape `Expeditor::Serve`
// uses: a scheduler here, a ring there, a `CoreRuntime` opened with a config
// the test filled in itself. Those approximations can agree with each other
// while all of them disagree with the real thing, and until this file
// nothing in the tree constructed an `Expeditor` at all - production's
// `src/server/main.cpp` was the only caller of `Open`, and `Serve` blocked
// for the life of the process, so there was no moment at which a test could
// look at an assembled instance. `Start()` / `RunUntilStopped()` is that
// moment (`expeditor.hpp`), and this is what it is for.
//
// **What the gap cost is on the record.** M0's cutover shipped three defects
// on the default multi-core path, every one found by review rather than by a
// test (`instructions/v3.0.0/workorder-al-m0-single-wal.md` AL-7c). Two of
// them are reproduced here, each verified by reverting its fix:
//
//   - **the null `log_device_`**: a peer dereferenced it when resuming
//     assertions, and the cutover made it null on the attached branch. A
//     `cores > 1` volume holding one assertion segfaults at mount,
//     deterministically. `APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt`
//     dumps core with the fix reverted; the other three cells pass, because
//     the resume short-circuits on an *empty* assertion list, which is
//     exactly why nothing caught it.
//   - **`cores = 1` arming the stream latch**: `single_stream()` is true of
//     every database this build creates, so arming on it alone put a mutex
//     on every logged page mutation for a section no second thread can
//     reach. `AtOneCoreTheStreamsLatchIsNeverArmed` fails with the fix
//     reverted, naming what it costs.
//
// **The third is not reproduced here**: the assertion scan's floor, which
// stays green in every cell with its fix reverted. A cleanly stopped
// instance cannot reach the state that exposes it, and an `Expeditor` cell
// is the wrong shape for the state that can - `docs/inflight/known-gaps.md`
// carries the argument and names the harness that would.

namespace kds::server {
namespace {

// **Two free ports, held open together.** Hard-coding them races every other
// cell in a `ctest -j8` run and every other instance on the box (`kds_server`
// has been seen holding 15432 for hours). Taken as a pair rather than one at
// a time because the kernel may reissue an ephemeral port the moment it is
// closed, and two calls would then hand the same number to the main port and
// the debug one - a bind failure inside `Start()` that has nothing to say
// about the engine.
//
// The window between closing these and `Start()` binding them stays open and
// is not worth closing: nothing here passes `SO_REUSEPORT`, so losing that
// race is a clean `EADDRINUSE` out of `Start()` rather than a wrong answer.
struct FreePorts {
    std::uint16_t first = 0;
    std::uint16_t second = 0;
};

FreePorts TwoFreeLoopbackPorts() {
    FreePorts ports;
    int fds[2] = {-1, -1};
    std::uint16_t* out[2] = {&ports.first, &ports.second};
    for (int i = 0; i < 2; ++i) {
        fds[i] = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] < 0) break;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fds[i], reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) break;
        socklen_t len = sizeof(addr);
        if (::getsockname(fds[i], reinterpret_cast<sockaddr*>(&addr), &len) != 0) break;
        *out[i] = ntohs(addr.sin_port);
    }
    for (const int fd : fds) {
        if (fd >= 0) ::close(fd);
    }
    return ports;
}

// One attempt per socket: after a refused `connect()` the socket's state is
// unspecified, so retrying on the same fd is a Linux-only accident rather
// than a technique. The port is already bound and listening when this runs
// (`Start()` did that), so the backlog accepts whether or not core 0's
// reactor has been entered - which is why one attempt is enough and the
// caller connects *before* it spawns the reactor thread.
int ConnectToLoopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
    ::close(fd);
    return -1;
}

std::string SendLine(int fd, const std::string& line) {
    const std::string out = line + "\n";
    if (::write(fd, out.data(), out.size()) < 0) return "ERR write failed";
    std::string response;
    char buf[512];
    while (response.find('\n') == std::string::npos) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));
    }
    if (!response.empty() && response.back() == '\n') response.pop_back();
    return response;
}

// **A client that retries what the engine told it to retry.** A peer's very
// first write to a relation finds its row-id lease unfunded and is refused
// `TXN_CONFLICT retryable=1` until the refill grant lands (`row_id_lease.hpp`
// P5) - which is the documented contract and what every real client does, so
// a test that treated the first refusal as a failure would be testing a
// client nobody writes. Bounded, so a refusal that never clears still fails
// the cell rather than hanging it - and the last response is what comes
// back, so nothing is masked. The cost is on the *negative* use: a write
// that is meant to be refused by an assertion, on a peer that came up not
// enforcing it, spins the whole budget before the expectation fails.
std::string SendLineRetrying(int fd, const std::string& line) {
    std::string response;
    for (int attempt = 0; attempt < 200; ++attempt) {
        response = SendLine(fd, line);
        if (response.find("retryable=1") == std::string::npos) return response;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return response;
}

class ExpeditorTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("kds_expeditor_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    // The config production builds from a file, with the two things a test
    // must control: a data file of its own and a port nobody else holds.
    // The cadences are left at their defaults - a test that turned the
    // checkpoint and drain timers off would be asserting about an instance
    // no operator runs.
    Expeditor::Config ConfigAt(std::uint32_t cores) {
        Expeditor::Config config;
        config.data_file = (dir_ / "kds.db").string();
        config.wal_dir = (dir_ / "wal").string();
        config.log_file = {};  // no log file: the suite's output is the test's
        config.cores = cores;
        const FreePorts ports = TwoFreeLoopbackPorts();
        // Checked, because `debug_text_port = 0` means *no text listener*
        // rather than an error - a cell would then fail on a connect that
        // could never succeed, saying nothing about what it tests.
        EXPECT_NE(ports.first, 0);
        EXPECT_NE(ports.second, 0);
        config.port = ports.first;
        // The newline surface, because `STOP` is reachable only there
        // (`protocol.md` §12) and `STOP` is how this test stops an instance
        // the way an operator does rather than by tearing one down.
        config.debug_text_port = ports.second;
        // A relation per core: `kNamespace` fixes every relation in `public`
        // to one core, and what these cells need is one that lands on the
        // peer.
        config.placement = catalog::PlacementPolicy::kRotate;
        return config;
    }

    std::filesystem::path dir_;
    static inline int counter_ = 0;
};

// Runs an instance to completion on a thread and hands the caller the text
// port while it serves: `Start()` first, so the caller may look at the
// assembled instance *before* core 0's reactor is entered, then
// `RunUntilStopped()` until a `STOP` arrives.
class RunningInstance {
public:
    RunningInstance(Expeditor& db, std::uint16_t text_port) : db_(db), text_port_(text_port) {}

    // The safety net for a cell that asserts its way out before stopping:
    // an unjoined reactor thread would outlive the `Expeditor` it holds.
    // The status is dropped here on purpose - a cell that cares asserts on
    // `Stop()`'s own return.
    ~RunningInstance() { (void)Stop(); }

    // The reactor, on its own thread. Called after the caller has finished
    // asserting on what `Start()` produced.
    //
    // **The client connects first, and the thread is spawned only if it
    // did.** The other order hangs the suite instead of failing a cell: a
    // connect that never succeeds leaves a reactor nothing will ever send
    // `STOP` to, and the join below then waits forever - with the cell's own
    // `ASSERT` never reached to say why. Connecting first is also simply
    // correct, because `Start()` has already bound and listened, so the
    // backlog accepts before core 0's reactor is entered.
    bool Run() {
        client_ = ConnectToLoopback(text_port_);
        if (client_ < 0) return false;
        thread_ = std::thread([this] { status_ = db_.RunUntilStopped(); });
        return true;
    }

    int client() const noexcept { return client_; }

    Status Stop() {
        if (!thread_.joinable()) return status_;
        if (client_ >= 0) {
            (void)SendLine(client_, "STOP");
            ::close(client_);
            client_ = -1;
        }
        thread_.join();
        return status_;
    }

private:
    Expeditor& db_;
    std::uint16_t text_port_;
    std::thread thread_;
    int client_ = -1;
    Status status_ = Status::OK();
};

TEST_F(ExpeditorTest, TwoCoresComeUpOnOneLogAndEachHoldsTheVolumesOwnImage) {
    // The assembly, asserted at the one moment it is whole and still: every
    // core built, the peer's thread running, the ports bound, and core 0's
    // reactor not yet entered.
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();

    ASSERT_TRUE(db.cores().empty()) << "no core is built until Start()";
    ASSERT_TRUE(db.Start().ok());

    // **One peer, and core 0 is not in the list**: core 0's runtime is the
    // `Expeditor` itself, which is why `cores()` is one short of `cores`.
    ASSERT_EQ(db.cores().size(), 1u);
    CoreRuntime& peer = *db.cores().front();
    EXPECT_EQ(peer.core_id(), 1u);

    // **The log.** One stream for the instance (AR0 M0): the peer attached
    // to core 0's rather than opening a `wal-1-*` of its own, and the object
    // it holds is the same object.
    EXPECT_TRUE(peer.wal().attached()) << "the peer opened a stream of its own";
    EXPECT_EQ(peer.wal().stream(), db.wal().stream());
    EXPECT_EQ(peer.wal().writer(), db.wal().writer());
    // And the latch is armed, because this volume has a second thread that
    // can reach the stream. `AtOneCoreTheStreamsLatchIsNeverArmed` is the
    // other half.
    EXPECT_TRUE(db.wal().stream()->shared());
    // **The page latch arms on the same count** (AM-S1): core 0's store and
    // the peer's, each from the superblock's `core_count`, each naming its
    // own core in the word. `AtOneCoreThePageLatchIsNeverArmed` is the
    // other half.
    EXPECT_TRUE(db.store().latch_armed()) << "core 0's store did not arm its page latch";
    EXPECT_TRUE(peer.store().latch_armed()) << "the peer's store did not arm its page latch";

    // **The wake wiring, which fails silently or not at all** (AU-S1b). The
    // transport kicks through the instance's `WakerTable` rather than
    // through a copy of its own, so what has to be true is that it holds
    // *this* table - the same shape as the stream assertion above, and for
    // the same reason: a transport handed nothing still accepts every send,
    // and every destination just waits out its 10 ms block. Nothing fails,
    // nothing is wrong, and every cross-core message costs a block.
    ASSERT_NE(db.transport(), nullptr) << "a two-core instance built no transport";
    ASSERT_NE(db.wakers(), nullptr) << "a two-core instance built no waker table";
    EXPECT_EQ(db.transport()->wakers(), db.wakers())
        << "the transport kicks through a registry that is not this instance's";
    // **The same silence one size down.** `Kick` returns without a sound for
    // a core outside the table, so a table built smaller than the transport
    // would disable send-wakes for the high cores and change no result. This
    // is the only place in the tree that would notice.
    EXPECT_EQ(db.wakers()->core_count(), db.transport()->core_count())
        << "the waker table is not sized for every core the transport can address";

    // **The superblock image.** A peer used to hold a default-constructed
    // copy, and zero is a legal value of most of its fields - so it reported
    // `wal_topology=per-core` on a single-stream volume and `version=0`
    // beside it (AL-S9). Read through `SHOW META`, which is where every
    // wrong answer reached a client.
    const std::string peer_meta = peer.dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(peer_meta.find("wal_topology=single"), std::string::npos) << peer_meta;
    EXPECT_NE(peer_meta.find(" core=1 "), std::string::npos) << peer_meta;
    EXPECT_EQ(peer_meta.find("version=0 "), std::string::npos) << peer_meta;
    EXPECT_EQ(peer_meta.find("create_time=0 "), std::string::npos) << peer_meta;

    // **The recovery report.** A fresh volume's log holds nothing, and under
    // one stream a peer runs no pass of its own in any case - core 0's is
    // the instance's. Both readings are "nothing was replayed here", and the
    // marker is what says which: a core that ran its own pass does not carry
    // it.
    EXPECT_EQ(peer.recovery().records, 0u);
    EXPECT_EQ(peer.recovery().redo_applied, 0u);
    EXPECT_NE(peer_meta.find("recovery_by=core0"), std::string::npos) << peer_meta;

    // **The catalog cache.** The peer reads the same catalog through its own
    // cache, and a peer that came up with an empty one answers `NotFound`
    // for relations that exist.
    const std::string tables = peer.dispatcher().Dispatch("SHOW TABLES").response;
    EXPECT_EQ(tables.rfind("ERR", 0), std::string::npos) << tables;

    // **Core 0's own `SHOW META` carries the scheduler block**, which is
    // the borrow this file nearly lost. `set_scheduler_view` installs it
    // and the reactor-borrow guard withdraws it; a guard built as a
    // temporary - the shape `optional::emplace(T{...})` produces for an
    // aggregate with a destructor - copies in and then destroys the
    // temporary at the semicolon, withdrawing what the line above had just
    // installed. The block then goes missing for the life of every
    // instance, with no error anywhere, which is why it is asserted here
    // rather than trusted: `sched.md` §4 is one of the three blocks
    // `SHOW META` is required to carry.
    const std::string core0_meta = db.dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(core0_meta.find("sched_iterations="), std::string::npos) << core0_meta;
    EXPECT_NE(core0_meta.find("sched_foreground_polls="), std::string::npos) << core0_meta;
    EXPECT_NE(core0_meta.find(" core=0 "), std::string::npos) << core0_meta;

    // And then it serves, and stops the way an operator stops it.
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run()) << "the debug text port never accepted";
    EXPECT_EQ(SendLine(running.client(), "PING"), "PONG");
    EXPECT_TRUE(running.Stop().ok());
    // The shutdown tail cleared them, so a caller that asks afterwards sees
    // an instance with no cores rather than dangling ones.
    EXPECT_TRUE(db.cores().empty());
}

TEST_F(ExpeditorTest, AtOneCoreThereIsNoWakeRegistryAndNoTransportToAskIt) {
    // The other half of the two-core wiring cell, and guideline 2's
    // "zero messages, zero allocations" read literally: a single-core
    // instance has no peer to kick, so neither object is built. Asserted
    // rather than assumed because the cheap way to make the two-core cell
    // pass is to build both unconditionally, which would put a table and an
    // N=1 ring matrix in every single-core process.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(db.Start().ok());

    EXPECT_EQ(db.transport(), nullptr);
    EXPECT_EQ(db.wakers(), nullptr);
    // And core 0's own reactor reports the counter honestly rather than
    // omitting it: `sched_wakes_sent` is 0 because nothing can be woken,
    // which is a different statement from "the block is missing".
    const std::string meta = db.dispatcher().Dispatch("SHOW META").response;
    EXPECT_NE(meta.find("sched_wakes_sent=0"), std::string::npos) << meta;
}

TEST_F(ExpeditorTest, AtOneCoreTheStreamsLatchIsNeverArmed) {
    // **AR0's G2, and it is a property of the code rather than of a build
    // flag** (`base/latch.hpp`). `single_stream()` is true of every database
    // this build creates, the single-core default included, so arming on
    // that alone put a mutex on every logged page mutation for a section no
    // second thread can reach. Caught by review at AL-7c and invisible to
    // every test in the tree, because the decision is `Expeditor`'s and
    // nothing constructed one.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(db.Start().ok());

    EXPECT_FALSE(db.wal().stream()->shared())
        << "a one-core instance armed the stream latch, which is G2's overhead on every append";
    EXPECT_TRUE(db.cores().empty()) << "one core builds no peer and spawns no thread";

    // Dropped without ever entering the reactor, which `~Expeditor` has to
    // survive: it is the shape a failed start leaves behind, and the shape
    // this file's other cells use to look at an instance.
}

TEST_F(ExpeditorTest, AtOneCoreThePageLatchIsNeverArmed) {
    // AM-S1's half of G2: the page latch word exists in every frame, and at
    // one core the store never reads or writes it (AM-R3's run-time branch,
    // `device_page_store.hpp` "The page latch"). Asserted on the assembly
    // rather than on a hand-built store, because the decision is
    // `Expeditor::Open`'s - a hand-built store is unarmed by default and
    // would pass this cell whatever the assembly did.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(db.Start().ok());

    if (std::getenv("KDS_TEST_PAGE_LATCH") != nullptr) {
        GTEST_SKIP() << "the census override arms every store and wins over the assembly's "
                        "decision, which is what this cell asserts";
    }
    EXPECT_FALSE(db.store().latch_armed())
        << "a one-core instance armed the page latch, which is a CAS on every pin";
}

TEST_F(ExpeditorTest, StartIsRefusedTwiceAndTheRunHalfIsRefusedWithoutIt) {
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();

    // The run half alone has no reactor to run, and answering "stopped
    // cleanly" for an instance that never started is the one wrong answer
    // available here.
    const Status without_start = db.RunUntilStopped();
    EXPECT_EQ(without_start.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(without_start.message().find("Start() has not run"), std::string::npos)
        << without_start.message();

    ASSERT_TRUE(db.Start().ok());
    const Status again = db.Start();
    EXPECT_EQ(again.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(again.message().find("already started"), std::string::npos) << again.message();
}

TEST_F(ExpeditorTest, APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt) {
    // **M0's segfault, reproduced through the assembly it lives in**
    // (AL-7c). The first run leaves a peer-owned relation with an assertion
    // on it; the second mounts that volume.
    //
    // Against the pre-fix engine the second `Start()` **dumps core**: the
    // peer's resume was handed `*log_device_`, which the cutover made null
    // on the attached branch, and it short-circuits only on an *empty*
    // assertion list - so a volume with one assertion is the reproduction
    // and a volume without one is not, which the other three cells passing
    // under the same revert is the demonstration of.
    //
    // The floor defect beside it in AL-7c does **not** reach here; the file
    // comment says what it would take.
    Expeditor::Config config = ConfigAt(/*cores=*/2);

    {
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        ASSERT_TRUE(db.Start().ok());
        ASSERT_EQ(db.cores().size(), 1u);

        RunningInstance running(db, config.debug_text_port);
        ASSERT_TRUE(running.Run());
        const int c = running.client();

        // `kRotate` at two cores places every relation on core 1, so this is
        // the peer's - which is what makes the assertion the peer's own, and
        // an assertion this core merely knows about would reproduce neither
        // defect the same way.
        const std::string made = SendLine(c, "CREATE TABLE cap (id int64, v int64)");
        ASSERT_EQ(made.rfind("ERR", 0), std::string::npos) << made;
        const std::string declared =
            SendLine(c, "CREATE ASSERTION one ON cap GROUP BY (v) CHECK COUNT(*) <= 1");
        ASSERT_EQ(declared.rfind("ERR", 0), std::string::npos) << declared;
        const std::string seeded = SendLineRetrying(c, "INSERT INTO cap VALUES (7)");
        ASSERT_EQ(seeded.rfind("ERR", 0), std::string::npos) << seeded;
        EXPECT_TRUE(running.Stop().ok());
    }

    // The mount. Nothing about this call is special - it is what a restart
    // is - and that is the point: the defect was reachable from the ordinary
    // path and from no test.
    auto reopened = Expeditor::Open(config, /*now_unix_seconds=*/2000);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    Expeditor& db = *reopened.value();
    ASSERT_TRUE(db.Start().ok());
    ASSERT_EQ(db.cores().size(), 1u);
    CoreRuntime& peer = *db.cores().front();

    // **Enforcing, not merely known.** A core that knows of an assertion it
    // cannot enforce refuses the relation's writes (`assertion.md` §6.1), so
    // `assertions_unrecovered` moving is a relation that stops taking writes
    // rather than a cosmetic count. This is the reading that would catch the
    // floor defect if the anchor were arranged to expose it.
    EXPECT_EQ(peer.recovery().assertions_enforcing, 1u);
    EXPECT_EQ(peer.recovery().assertions_unrecovered, 0u);

    // And the enforcement itself, which is the reading that does not depend
    // on a counter being named correctly: the second row in group 7 is
    // refused by the assertion.
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run());
    const std::string violating = SendLineRetrying(running.client(), "INSERT INTO cap VALUES (7)");
    EXPECT_EQ(violating.rfind("ERR ", 0), 0u) << violating;
    EXPECT_NE(violating.find("ASSERTION_VIOLATION"), std::string::npos) << violating;
    EXPECT_TRUE(running.Stop().ok());
}

}  // namespace
}  // namespace kds::server
