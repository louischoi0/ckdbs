#include "kds/base/contention.hpp"
#include "kds/server/expeditor.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/oid.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/server/load_placer.hpp"
#include "kds/server/transaction_placement.hpp"
#include "kds/wire/error_registry.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kds/wire/row_codec.hpp"
#include "view_text.hpp"

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
//   - **`cores = 1` arming the stream latch**: the volume's topology is
//     true of every database this build creates, so arming on it alone put
//     a mutex on every logged page mutation for a section no second thread
//     can reach. The core count is the whole predicate since AM-S4(d).
//     `AtOneCoreTheStreamsLatchIsNeverArmed` fails with the fix reverted,
//     naming what it costs.
//
// **The third is not reproduced here**: the assertion scan's floor, which
// stays green in every cell with its fix reverted. A cleanly stopped
// instance cannot reach the state that exposes it, and an `Expeditor` cell
// is the wrong shape for the state that can - `docs/inflight/known-gaps.md`
// carries the argument and names the harness that would.

namespace kds::server {
namespace {

// `Start()`'s status, printed when it fails (AZ-S4): a bare `.ok()` lost the
// cause of a one-off failure under `-j8` (`known-gaps.md`, Testing).
::testing::AssertionResult Started(const Status& s) {
    if (s.ok()) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "Start() failed: code " << static_cast<int>(s.code())
                                         << ": " << s.message();
}

// **Two free ports, held open together.** Hard-coding them races every other
// cell in a `ctest -j8` run and every other instance on the box (`kds_server`
// has been seen holding 15432 for hours). Taken as a pair rather than one at
// a time because the kernel may reissue an ephemeral port the moment it is
// closed, and two calls would then hand the same number to the main port and
// the debug one - a bind failure inside `Start()` that has nothing to say
// about the engine.
//
// The window between closing these and `Start()` binding them stays open and
// is not worth closing: at one core nothing passes `SO_REUSEPORT`, so losing
// that race is a clean `EADDRINUSE` out of `Start()` rather than a wrong
// answer. Above one core the default port is bound with it (AT-S8), and a
// clash with another such listener of the same user binds rather than
// failing.
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
        config.buffer_pool_frames = 4096;  // required (BE-R3)
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
    ASSERT_TRUE(Started(db.Start()));

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

    // **EV3's floor, installed or not** (AM-S2). Everything below
    // `kFirstUserPageId` is a fixed system structure and is never an
    // eviction candidate - a protection every *peer* got from the call that
    // installed its lease, and core 0 got nowhere, so it depended on
    // whether a lease was installed. Asserted here because its absence changes no
    // result: the catalog pages would simply be re-read after a sweep
    // reclaimed them, which is slower and never wrong.
    EXPECT_EQ(db.store().first_evictable_page_id(), kFirstUserPageId)
        << "core 0's store came up without the resident-by-class floor";

    // **One buffer pool for the instance** (AM-S2 step 3), which is the
    // stage's own definition of done: a page faulted on core 1 is served
    // from the frame core 0 loaded.
    //
    // **This one line is the whole claim, and two that stood beside it were
    // not.** A first version also compared `frame_budget()` and watched
    // `resident_pages()` across a peer dispatch; the review measured both as
    // vacuous - the budgets are equal under either arrangement, and the
    // count was core 0's own on both sides of a *peer* read, which two
    // tables do not move either. Naming the same object is not a weaker
    // statement than a behavioural one here, it is the same statement
    // earlier: one object is one frame table, and there is no second table
    // for a page to be missing from.
    EXPECT_EQ(&peer.store(), &db.store())
        << "the peer opened a frame table of its own over the shared device";

    // **The wake wiring, which fails silently or not at all** (AU-S1b).
    // `Kick` returns without a sound for a core outside the table, so a
    // table built smaller than the instance would disable every wake to the
    // high cores and change no result - every destination would wait out its
    // 10 ms block. This is the only place in the tree that would notice. It
    // compared against the ring transport's core count until AT-S10d retired
    // the transport.
    ASSERT_NE(db.wakers(), nullptr) << "a two-core instance built no waker table";
    EXPECT_EQ(db.wakers()->core_count(), config.cores)
        << "the waker table is not sized for every core the instance runs";
    // **And the lock table kicks through the same registry** (AO-S5,
    // AU-S2) - the third object with the same silent failure: a table
    // handed no registry flips a waiter's slot and kicks nobody, and the
    // waiter's reactor sleeps out its block. The same statement one level
    // down: the peer's manager releases into *this* table, or a decide on
    // core 1 wakes a waiter core 0 queued in some other one.
    ASSERT_NE(db.locks(), nullptr) << "a two-core instance built no lock table";
    EXPECT_EQ(db.locks()->wake_registry(), db.wakers())
        << "the lock table kicks through a registry that is not this instance's";
    EXPECT_EQ(db.locks()->partition_count(), 64u)
        << "the lock table's partition count is fixed at every core count (BP-S6)";
    // **And every dispatcher records its edges in it** (AO-S4b): a
    // dispatcher handed no table keeps the narrow admission and records
    // no edge, and a cross-core cycle through it is one the detector never
    // sees - ended by the fault net, eleven seconds later.
    EXPECT_EQ(db.dispatcher().locks(), db.locks())
        << "core 0's dispatcher records its edges in some other table, or none";
    EXPECT_EQ(peer.dispatcher().locks(), db.locks())
        << "the peer's dispatcher records its edges in some other table, or none";

    // **The superblock image.** A peer used to hold a default-constructed
    // copy, and zero is a legal value of most of its fields - so it reported
    // `wal_topology=per-core` on a single-stream volume and `version=0`
    // beside it (AL-S9). Read through `SHOW META`, which is where every
    // wrong answer reached a client.
    const std::string peer_meta = peer.dispatcher().Dispatch("SHOW META").response;
    EXPECT_EQ(testing_view::MetaField(peer_meta, "wal_topology"), "single") << peer_meta;
    EXPECT_EQ(testing_view::MetaField(peer_meta, "core"), "1") << peer_meta;
    EXPECT_NE(testing_view::MetaField(peer_meta, "version"), "0") << peer_meta;
    EXPECT_NE(testing_view::MetaField(peer_meta, "create_time"), "0") << peer_meta;

    // **The recovery report.** A fresh volume's log holds nothing, and under
    // one stream a peer runs no pass of its own in any case - core 0's is
    // the instance's. Both readings are "nothing was replayed here", and the
    // marker is what says which: a core that ran its own pass does not carry
    // it.
    EXPECT_EQ(peer.recovery().records, 0u);
    EXPECT_EQ(peer.recovery().redo_applied, 0u);
    EXPECT_EQ(testing_view::MetaField(peer_meta, "recovery_by"), "core0") << peer_meta;

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
    EXPECT_NE(testing_view::MetaField(core0_meta, "sched_iterations"), std::nullopt) << core0_meta;
    EXPECT_NE(testing_view::MetaField(core0_meta, "sched_foreground_polls"), std::nullopt) << core0_meta;
    EXPECT_EQ(testing_view::MetaField(core0_meta, "core"), "0") << core0_meta;

    // And then it serves, and stops the way an operator stops it.
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run()) << "the debug text port never accepted";
    EXPECT_EQ(SendLine(running.client(), "PING"), "PONG");
    EXPECT_TRUE(running.Stop().ok());
    // The shutdown tail cleared them, so a caller that asks afterwards sees
    // an instance with no cores rather than dangling ones.
    EXPECT_TRUE(db.cores().empty());
}

// BP-S4 (BP-H11): core 0's `system` tick runs the delete-mark purge, so a
// drop's marks are retired on a serving instance with no further DDL.
TEST_F(ExpeditorTest, TheTickRetiresADropsMarksWithNoFurtherDdl) {
    Expeditor::Config config = ConfigAt(1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(db.Start().ok());
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run()) << "the debug text port never accepted";
    const int c = running.client();
    ASSERT_NE(SendLine(c, "CREATE TABLE gone (id int64, v int64)").rfind("ERR", 0), 0u);
    ASSERT_NE(SendLine(c, "DROP TABLE gone").rfind("ERR", 0), 0u);

    std::uint64_t purged = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (purged == 0 && std::chrono::steady_clock::now() < deadline) {
        const auto field = testing_view::MetaField(SendLine(c, "SHOW META"), "catalog_marks_purged");
        if (field.has_value()) purged = std::stoull(*field);
        if (purged == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GE(purged, 1u) << "no tick retired the drop's marks within 5 s";
    EXPECT_TRUE(running.Stop().ok());
}

// BP-R1's arming cells (BP-H1, H2, H3): each structure the instance shares
// latches or fences above one core and not at one, where its every caller
// runs on the reactor thread (the census is in each header).
TEST_F(ExpeditorTest, TheSharedStructuresAreArmedAboveOneCoreOnly) {
    for (const std::uint32_t cores : {1u, 2u}) {
        Expeditor::Config config = ConfigAt(cores);
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        const Expeditor& db = *opened.value();
        const bool shared = cores > 1;
        EXPECT_EQ(db.statement_epochs().shared(), shared) << "cores = " << cores;
        EXPECT_EQ(db.visibility().WindowLatchForTest() != nullptr, shared) << "cores = " << cores;
        ASSERT_NE(db.cabin_store(), nullptr);
        EXPECT_EQ(db.cabin_store()->shared(), shared) << "cores = " << cores;
    }
}

// BP-S1's census: a synchronous checkpoint's step counts the pages it wrote
// back, and its publish's data-file sync is counted at the syncer.
TEST_F(ExpeditorTest, ACheckpointsPagesAndDataSyncsAreCounted) {
    Expeditor::Config config = ConfigAt(1);
    config.checkpoint_interval_ns = 0;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    const std::string created =
        db.dispatcher().Dispatch("CREATE TABLE census_ck (id int64, v int64)").response;
    ASSERT_EQ(created.rfind("ERR", 0), std::string::npos) << created;

    const Contention::Snapshot before = Contention::Read();
    ASSERT_TRUE(db.Checkpoint().ok());
    const Contention::Snapshot after = Contention::Read();
    EXPECT_GT(after.of(Tally::kCheckpointPagesWritten), before.of(Tally::kCheckpointPagesWritten));
    EXPECT_GT(after.of(Tally::kDataSyncs), before.of(Tally::kDataSyncs));
}

// Every mount discards the Waystone it finds, a clean stop's included: a
// crash can leave a directory page on the device naming a child whose
// allocation bit is not, the cursor would hand that id to a logged page, and
// the next Waystone write would overwrite it with no record. A mount cannot
// tell the two stops apart (invariant 8 allows the discard).
TEST_F(ExpeditorTest, EveryMountDiscardsTheWaystoneItFinds) {
    Expeditor::Config config = ConfigAt(1);
    const auto directories = [](Expeditor& db) {
        auto patterns = db.catalog().ListPatterns();
        EXPECT_TRUE(patterns.ok());
        return patterns.ok() ? std::count_if(patterns.value().begin(), patterns.value().end(),
                                             catalog::HasWaystoneDirectory)
                             : std::ptrdiff_t{0};
    };
    {
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        for (const char* sql : {"CREATE TABLE ws (id int64, v int64)", "INSERT INTO ws VALUES (5)",
                                "SELECT * FROM ws WHERE id = 1", "SELECT * FROM ws WHERE id = 1"}) {
            const std::string reply = db.dispatcher().Dispatch(sql).response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << sql << ": " << reply;
        }
        ASSERT_GT(directories(db), 0) << "nothing was recorded; the cell proves nothing";
        ASSERT_TRUE(db.Checkpoint().ok());
    }

    auto reopened = Expeditor::Open(config, /*now_unix_seconds=*/2000);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_EQ(directories(*reopened.value()), 0);
}

TEST_F(ExpeditorTest, APoolBelowTheVolumesFloorIsRefusedAtMountNamingBothNumbers) {
    // BE-Q6: the floor is the volume's 128 system pages plus a working
    // minimum of 256 frames. A pool below it would refuse every statement, so
    // the mount refuses it while the operator is still at the config; a pool
    // at the floor mounts. And a server with no `buffer_pool_frames` at all -
    // the 0 a `Config` is built with - is refused before anything opens.
    const std::size_t floor = kFirstUserPageId + storage::DevicePageStore::kWorkingMinimumFrames;
    {
        Expeditor::Config config = ConfigAt(/*cores=*/1);
        config.buffer_pool_frames = floor - 1;
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_FALSE(opened.ok());
        EXPECT_EQ(opened.status().code(), StatusCode::kInvalidArgument);
        EXPECT_NE(opened.status().message().find(std::to_string(floor - 1)), std::string::npos)
            << opened.status().message();
        EXPECT_NE(opened.status().message().find(std::to_string(floor)), std::string::npos)
            << opened.status().message();
    }
    {
        Expeditor::Config config = ConfigAt(/*cores=*/1);
        config.buffer_pool_frames = 0;
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_FALSE(opened.ok());
        EXPECT_NE(opened.status().message().find("buffer_pool_frames"), std::string::npos)
            << opened.status().message();
    }
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    config.buffer_pool_frames = floor;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
}

TEST_F(ExpeditorTest, AtOneCoreTheDispatcherHoldsTheInstancesLockTable) {
    // AO-S4b, and the finding AO-S5(a) recorded on the way: no server had a
    // lock table before it - `CoreRuntime` built one at `core_count == 1`,
    // and a server's core 0 is the `Expeditor` - so AO-S4a's detector ran
    // in every fixture and in no instance. A single-core server builds
    // one now, latchless, with no wake registry to kick through (there is
    // no peer to kick), and its dispatcher records its edges in it: the
    // first time a server admits a holding waiter under AO-S4a's rule.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));

    ASSERT_NE(db.locks(), nullptr) << "a single-core server built no lock table";
    EXPECT_FALSE(db.locks()->latched()) << "a single-core table constructed latches";
    EXPECT_EQ(db.locks()->partition_count(), 64u);
    EXPECT_EQ(db.locks()->wake_registry(), nullptr) << "no peer exists to be kicked";
    EXPECT_EQ(db.dispatcher().locks(), db.locks())
        << "core 0's dispatcher records its edges in some other table, or none";
}

// ---- AT-S8: a session runs where it was accepted --------------------------
//
// Every core listens on the port, so what a peer is handed decides what a
// session accepted there gets. Two halves, both read at the one moment the
// assembly is whole and still: the peer holds a listener with no config key
// asking for it, and its dispatcher was handed core 0's statement limits
// rather than the dispatcher's own defaults.
TEST_F(ExpeditorTest, EveryPeerListensAndCarriesCoreZerosStatementLimits) {
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    config.indexes = false;
    config.max_insert_rows = 9;
    config.aggregate_max_groups = 11;
    config.aggregate_max_distinct = 13;
    config.sort_max_rows = 7;
    config.join_build_max_rows = 17;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));
    ASSERT_EQ(db.cores().size(), 1u);
    const CoreRuntime& peer = *db.cores().front();

    EXPECT_TRUE(peer.listening()) << "the peer accepts nothing on the instance's port";

    const StatementLimits& handed = peer.config().statement_limits;
    EXPECT_FALSE(handed.indexes);
    EXPECT_EQ(handed.max_insert_rows, 9u);
    EXPECT_EQ(handed.aggregate.max_groups, 11u);
    EXPECT_EQ(handed.aggregate.max_distinct, 13u);
    EXPECT_EQ(handed.sort_max_rows, 7u);
    EXPECT_EQ(handed.join_build_max_rows, 17u);

    // And the optimizer's switch is one flag (AT-S8, `OptimizerSurface`): a
    // flip on core 0 reads on the peer, and the peer holds the instance's
    // collector rather than none.
    const OptimizerSurface& surface = peer.config().optimizer;
    ASSERT_NE(surface.cabin_optimizer_on, nullptr) << "the peer was handed no switch";
    db.dispatcher().set_cabin_optimizer_enabled(true);
    EXPECT_TRUE(surface.cabin_optimizer_on->load()) << "core 0 and the peer hold two switches";
    EXPECT_NE(surface.signals, nullptr) << "the peer's reads feed no optimizer signal";
    EXPECT_NE(surface.view_latch, nullptr) << "two cores and no view latch";
}

// ---- D19's fallback (AT-S10c): core 0 accepts and hands off ------------
//
// Where the platform refuses `SO_REUSEPORT`, core 0 binds the port alone
// and hands each connection to the core it places it on, through the
// destination's inbox and a kick (`connection_handoff.hpp`). Linux never
// refuses, so `force_listener_handoff` takes the arm.
//
// Two raw connections, made before core 0's reactor runs so the backlog
// holds them in order: the first is core 0's, the second core 1's. Each
// sends eight bytes - a whole frame header - declaring a length past
// `kMaxFrame`, and a server that runs the
// connection answers with a fatal error frame. **The second answer can come
// only from core 1**: core 0 offered that socket and holds no connection
// for it, and an inbox nobody took keeps its socket open and silent until
// the instance stops.
//
// **The mutations**: make `TcpServer::Host` return before it submits its
// task and the second read times out; drop core 0's `set_handoff` and the
// placement count reads 0.
namespace {
// Bytes back within `ms`, or -1 on a timeout.
ssize_t ReadWithin(int fd, int ms) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, ms) <= 0) return -1;
    char buf[256];
    return ::read(fd, buf, sizeof(buf));
}
}  // namespace

TEST_F(ExpeditorTest, UnderTheHandoffFallbackAConnectionCoreZeroAcceptedIsServedByCoreOne) {
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    config.force_listener_handoff = true;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));
    ASSERT_NE(db.connection_handoff(), nullptr) << "the fallback was forced and not taken";
    ASSERT_EQ(db.cores().size(), 1u);
    EXPECT_TRUE(db.cores().front()->listening()) << "the peer hosts nothing";

    const int to_core0 = ConnectToLoopback(config.port);
    const int to_core1 = ConnectToLoopback(config.port);
    ASSERT_GE(to_core0, 0);
    ASSERT_GE(to_core1, 0);

    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run()) << "the debug text port never accepted";
    for (const int fd : {to_core0, to_core1}) {
        ASSERT_EQ(::write(fd, "PINGPING", 8), 8);
    }
    EXPECT_GT(ReadWithin(to_core0, 5000), 0) << "core 0 did not answer the connection it kept";
    EXPECT_GT(ReadWithin(to_core1, 5000), 0)
        << "nobody answered the connection core 0 handed to core 1";
    // And core 0 routed both: without the routing it would adopt both
    // itself and both reads above would still answer.
    EXPECT_EQ(db.connection_handoff()->placed(), 2u) << "core 0 placed no connection";
    ::close(to_core0);
    ::close(to_core1);
    EXPECT_TRUE(running.Stop().ok());
}

// ---- AT-0 item 7: the fault net's key sets the fault net ----------------
//
// `in_doubt_ceiling_ms` was re-scoped and renamed to
// `lock_wait_fault_net_ms` rather than deleted, which is only worth doing
// if the key reaches the wait. **The AT-S6 review found it did not**: the
// value was parsed, range-checked and threaded to every dispatcher while
// the four wait sites read the `constexpr`, so a file setting it changed
// nothing and three documents said otherwise. This cell is what makes the
// key's promise checkable from outside.
//
// **The mutation**: point a wait site back at `txn::kLockWaitFaultNetNs`
// and the second expectation reads 1000.
TEST_F(ExpeditorTest, TheFaultNetsKeyReachesTheDispatcherThatWaits) {
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    // Unset, every dispatcher carries the constant - which is what keeps a
    // fixture behaving as it did when the sites read it directly.
    {
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        ASSERT_TRUE(Started(opened.value()->Start()));
        EXPECT_EQ(opened.value()->dispatcher().LockWaitFaultNetNs(), txn::kLockWaitFaultNetNs);
    }
    // Set, the dispatcher waits by it. Milliseconds in, nanoseconds held.
    config.lock_wait_fault_net_ns = 250ULL * 1'000'000ULL;
    {
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        ASSERT_TRUE(Started(opened.value()->Start()));
        EXPECT_EQ(opened.value()->dispatcher().LockWaitFaultNetNs(), 250ULL * 1'000'000ULL)
            << "the key was threaded to the dispatcher and not read by the wait";
    }
    // **And 0 is a value, not an absence** - the other policy, which the
    // key's documentation promises: refuse at once instead of waiting.
    config.lock_wait_fault_net_ns = 0;
    {
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        ASSERT_TRUE(Started(opened.value()->Start()));
        EXPECT_EQ(opened.value()->dispatcher().LockWaitFaultNetNs(), 0u)
            << "0 was read as 'nobody configured one' and replaced by the default";
    }
}

TEST_F(ExpeditorTest, AtOneCoreThereIsNoWakeRegistry) {
    // The other half of the two-core wiring cell, and G2 read literally: a
    // single-core instance has no peer to kick, so no registry is built.
    // Asserted rather than assumed because the cheap way to make the
    // two-core cell pass is to build it unconditionally. Renamed at AT-S10d,
    // which retired the ring transport the old name also asserted absent.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));

    EXPECT_EQ(db.wakers(), nullptr);
    // BL-R9: nor any inbox for a move, so no transaction is placed.
    EXPECT_EQ(db.connection_handoff(), nullptr);
    // And core 0's own reactor reports the counter honestly rather than
    // omitting it: `sched_wakes_sent` is 0 because nothing can be woken,
    // which is a different statement from "the block is missing".
    const std::string meta = db.dispatcher().Dispatch("SHOW META").response;
    EXPECT_EQ(testing_view::MetaField(meta, "sched_wakes_sent"), "0") << meta;
}

TEST_F(ExpeditorTest, AtOneCoreTheStreamsLatchIsNeverArmed) {
    // **AR0's G2, and it is a property of the code rather than of a build
    // flag** (`base/latch.hpp`). The volume's topology is the same on every
    // database this build creates, the single-core default included, so
    // arming on that alone put a mutex on every logged page mutation for a
    // section no second thread can reach. Caught by review at AL-7c and
    // invisible to every test in the tree, because the decision is
    // `Expeditor`'s and nothing constructed one. Since AM-S4(d) the count
    // is the only test left, which is what this cell now pins.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));

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
    ASSERT_TRUE(Started(db.Start()));

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

    ASSERT_TRUE(Started(db.Start()));
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
        ASSERT_TRUE(Started(db.Start()));
        ASSERT_EQ(db.cores().size(), 1u);

        RunningInstance running(db, config.debug_text_port);
        ASSERT_TRUE(running.Run());
        const int c = running.client();

        // Placed on core 1 by `kRotate` until AT-S9, which made the assertion
        // the peer's own; no core owns a relation since, and the registry is
        // the instance's (AT-S5d), so the cell now pins the mount of a volume
        // with one assertion on it, whichever core a statement arrives on.
        const std::string made = SendLine(c, "CREATE TABLE cap (id int64, v int64)");
        ASSERT_EQ(made.rfind("ERR", 0), std::string::npos) << made;
        const std::string declared =
            SendLine(c, "CREATE ASSERTION one ON cap GROUP BY (v) CHECK COUNT(*) <= 1");
        ASSERT_EQ(declared.rfind("ERR", 0), std::string::npos) << declared;
        const std::string seeded = SendLine(c, "INSERT INTO cap VALUES (7)");
        ASSERT_EQ(seeded.rfind("ERR", 0), std::string::npos) << seeded;
        EXPECT_TRUE(running.Stop().ok());
    }

    // The mount. Nothing about this call is special - it is what a restart
    // is - and that is the point: the defect was reachable from the ordinary
    // path and from no test.
    auto reopened = Expeditor::Open(config, /*now_unix_seconds=*/2000);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    Expeditor& db = *reopened.value();
    ASSERT_TRUE(Started(db.Start()));
    ASSERT_EQ(db.cores().size(), 1u);
    CoreRuntime& peer = *db.cores().front();

    // **Enforcing, not merely known.** A registry that knows of an assertion
    // it cannot enforce refuses the relation's writes (`assertion.md` §6.1),
    // so `assertions_unrecovered` moving is a relation that stops taking
    // writes rather than a cosmetic count. This is the reading that would
    // catch the floor defect if the anchor were arranged to expose it.
    //
    // **Core 0's count, and the peer's zero** (AT-S5d). The registry is the
    // instance's and core 0's mount resumes it, for every relation; the peer
    // is handed it and resumes nothing. The peer's own resume was what this
    // cell read until then, and it is also what the segfault above was in.
    EXPECT_EQ(db.recovery().assertions_enforcing, 1u);
    EXPECT_EQ(db.recovery().assertions_unrecovered, 0u);
    EXPECT_EQ(peer.recovery().assertions_enforcing, 0u);
    EXPECT_EQ(&peer.dispatcher().assertions(), &db.dispatcher().assertions())
        << "the peer checks a registry of its own";

    // And the enforcement itself, which is the reading that does not depend
    // on a counter being named correctly: the second row in group 7 is
    // refused by the assertion - **arriving on core 0's listener**, which
    // was D1 exactly while the relation was core 1's: until AT-S5d core 0's
    // registry held no directory for it and admitted the row.
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run());
    const std::string violating = SendLine(running.client(), "INSERT INTO cap VALUES (7)");
    EXPECT_EQ(violating.rfind("ERR ", 0), 0u) << violating;
    EXPECT_NE(violating.find("ASSERTION_VIOLATION"), std::string::npos) << violating;
    EXPECT_TRUE(running.Stop().ok());
}

// ---- BL-S2: a session placed per transaction ------------------------------
//
// `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`
// BL-R1, BL-R6. A listener asks its placer at each transaction's head, and
// a core other than its own takes the whole connection. The placer here is
// the test's: it names the core outright, so every cell decides the move
// rather than racing a load signal for it.
namespace {

// Every head on `target`, or where the session is while `target` is -1.
// Asked on both reactors, hence the atomics.
class ScriptedPlacer final : public TransactionPlacer {
public:
    std::uint32_t Place(std::uint32_t core, Session&) override {
        asked.fetch_add(1, std::memory_order_relaxed);
        const int t = target.load(std::memory_order_relaxed);
        return t < 0 ? core : static_cast<std::uint32_t>(t);
    }
    std::atomic<int> target{-1};
    std::atomic<int> asked{0};
};

// A KWP/1 session over a real socket: one statement in, its answer out as
// text - a result set's cells joined by spaces, one row per line; a
// completion's tag; `ERR` and the message for an error frame.
class KwpClient {
public:
    explicit KwpClient(std::uint16_t port) : fd_(ConnectToLoopback(port)) {}
    ~KwpClient() {
        if (fd_ >= 0) ::close(fd_);
    }
    KwpClient(const KwpClient&) = delete;
    KwpClient& operator=(const KwpClient&) = delete;

    bool Hello() {
        if (fd_ < 0) return false;
        wire::ClientHello hello;
        hello.client_name = "bl-s2";
        Send(wire::ClientFrameType::kHello, wire::EncodeClientHello(hello));
        auto s_hello = Read();
        auto ready = Read();
        return s_hello.has_value() && ready.has_value() &&
               ready->type == static_cast<std::uint8_t>(wire::ServerFrameType::kReady);
    }

    // PARSE, BIND and EXECUTE of the unnamed statement and portal, sent in
    // one write - a pipeline the server reads in one go.
    std::string Run(std::string_view sql) {
        SendRaw(Pipeline(sql));
        return Answer();
    }

    // PARSE alone, of statement `name`: no dispatch, so no head, and the
    // statement is session state from here on.
    bool Prepare(std::string_view name, std::string_view sql) {
        Send(wire::ClientFrameType::kParse, ParseOf(name, sql));
        auto f = Read();
        return f.has_value() && f->type == static_cast<std::uint8_t>(wire::ServerFrameType::kParseOk);
    }

    // BIND and EXECUTE of the prepared statement `name`.
    std::string Execute(std::string_view name) {
        std::vector<std::byte> out = Frame(wire::ClientFrameType::kBind, BindOf(name));
        const std::vector<std::byte> exec = Frame(wire::ClientFrameType::kExecute, ExecuteOf());
        out.insert(out.end(), exec.begin(), exec.end());
        SendRaw(out);
        return Answer(/*parsed=*/false);
    }

    // Two statements in one write: the second's frames wait in the
    // connection's decoder while the first runs - and travel with it.
    std::pair<std::string, std::string> RunTwo(std::string_view a, std::string_view b) {
        std::vector<std::byte> both = Pipeline(a);
        const std::vector<std::byte> second = Pipeline(b);
        both.insert(both.end(), second.begin(), second.end());
        SendRaw(both);
        std::string first = Answer();
        return {first, Answer()};
    }

    // `C_TXN_BEGIN` (session default durability), `C_TXN_COMMIT`.
    std::string Txn(wire::ClientFrameType type) {
        std::vector<std::byte> payload;
        if (type == wire::ClientFrameType::kTxnBegin) payload.push_back(std::byte{0});
        Send(type, payload);
        auto f = Read();
        if (!f.has_value()) return "ERR hung up";
        return f->type == static_cast<std::uint8_t>(wire::ServerFrameType::kTxnOk) ? "TXN_OK"
                                                                                   : "ERR txn";
    }

    // The core `sys.meta` says this session runs on, or -1.
    int Core() {
        const std::string core = Run("SELECT value FROM sys.meta WHERE field = 'core'");
        return core.empty() || core.rfind("ERR", 0) == 0 ? -1 : std::atoi(core.c_str());
    }

    // One `sys.meta` field over KWP, as its text; empty when absent.
    std::string Meta(std::string_view field) {
        std::string value =
            Run("SELECT value FROM sys.meta WHERE field = '" + std::string(field) + "'");
        // A row is its value and a separating space, then a newline.
        while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) value.pop_back();
        return value;
    }

private:
    static std::vector<std::byte> Frame(wire::ClientFrameType type,
                                        std::span<const std::byte> payload) {
        return wire::EncodeFrame(static_cast<std::uint8_t>(type), 0, payload);
    }
    static std::vector<std::byte> ParseOf(std::string_view name, std::string_view sql) {
        wire::PayloadWriter w;
        w.Str(name);
        w.Text(sql);
        return w.Take();
    }
    // The unnamed portal over statement `name`.
    static std::vector<std::byte> BindOf(std::string_view name) {
        wire::PayloadWriter w;
        w.Str("");
        w.Str(name);
        std::vector<std::byte> out = w.Take();
        (void)wire::EncodeBindParams({}, out);
        return out;
    }
    static std::vector<std::byte> ExecuteOf() {
        wire::PayloadWriter w;
        w.Str("");
        w.U32(0);
        return w.Take();
    }
    static std::vector<std::byte> Pipeline(std::string_view sql) {
        std::vector<std::byte> out;
        for (const auto& f : {Frame(wire::ClientFrameType::kParse, ParseOf("", sql)),
                              Frame(wire::ClientFrameType::kBind, BindOf("")),
                              Frame(wire::ClientFrameType::kExecute, ExecuteOf())}) {
            out.insert(out.end(), f.begin(), f.end());
        }
        return out;
    }
    void Send(wire::ClientFrameType type, std::span<const std::byte> payload) {
        SendRaw(Frame(type, payload));
    }
    void SendRaw(const std::vector<std::byte>& bytes) {
        ASSERT_EQ(::write(fd_, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
    }
    std::optional<wire::DecodedFrame> Read() {
        while (true) {
            if (auto frame = decoder_.PopFrame()) return frame;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 10'000) <= 0) return std::nullopt;
            std::byte buf[4096];
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n <= 0) return std::nullopt;
            if (!decoder_.Feed(std::span<const std::byte>(buf, static_cast<std::size_t>(n))).ok()) {
                return std::nullopt;
            }
        }
    }
    // One statement's frames, from `S_PARSE_OK` (when `parsed`) and
    // `S_BIND_OK` to its completion - both of which a move must deliver too,
    // since they were in the source's reply tail. An error frame is read
    // through to the `S_READY` its `C_SYNC` asks for, so the next call starts
    // clean (§5's skip-to-sync).
    std::string Answer(bool parsed = true) {
        std::string text;
        std::vector<wire::FieldDescription> fields;
        bool parse_ok = !parsed;
        bool bind_ok = false;
        while (true) {
            auto f = Read();
            if (!f.has_value()) return "ERR hung up";
            const auto type = static_cast<wire::ServerFrameType>(f->type);
            if (type == wire::ServerFrameType::kError) {
                auto e = wire::DecodeError(f->payload);
                Send(wire::ClientFrameType::kSync, {});
                for (auto r = Read(); r.has_value(); r = Read()) {
                    if (r->type == static_cast<std::uint8_t>(wire::ServerFrameType::kReady)) break;
                }
                return "ERR " + (e.ok() ? e.value().message : std::string("undecodable"));
            }
            if (type == wire::ServerFrameType::kParseOk) {
                parse_ok = true;
            } else if (type == wire::ServerFrameType::kBindOk) {
                bind_ok = true;
            } else if (type == wire::ServerFrameType::kRowDesc) {
                auto described = wire::DecodeRowDescription(f->payload);
                if (!described.ok()) return "ERR row description";
                fields = std::move(described.value());
            } else if (type == wire::ServerFrameType::kRowBatch) {
                auto rows = wire::DecodeRowBatch(f->payload, fields.size());
                if (!rows.ok()) return "ERR row batch";
                for (const auto& row : rows.value()) {
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        // A variable-width column is text; every other one
                        // these cells select is an integer.
                        if (fields[i].type_len == -1) {
                            text += wire::DecodeText(row[i].bytes);
                        } else if (auto v = wire::DecodeInt(row[i].bytes); v.ok()) {
                            text += std::to_string(v.value());
                        }
                        text += ' ';
                    }
                    text += '\n';
                }
            } else if (type == wire::ServerFrameType::kComplete) {
                if (!parse_ok || !bind_ok) return "ERR completed without its S_PARSE_OK/S_BIND_OK";
                // A one-line answer (a write's) is the tag
                // itself, carried in this frame's payload.
                if (text.empty()) {
                    wire::PayloadReader r(f->payload);
                    auto tag = r.Text();
                    if (tag.has_value() && tag->has_value()) text = **tag;
                }
                return text;
            }
        }
    }

    int fd_;
    wire::FrameDecoder decoder_;
};

}  // namespace

// **Above one core the instance places every transaction with its balancer**
// (BL-S3), unless a test seam is set; at one core nothing is placed.
//
// **The mutation**: wire `placer_` to the seam alone and the two-core half
// reads no `LoadPlacer`.
TEST_F(ExpeditorTest, AboveOneCoreTheBalancerPlacesEveryTransaction) {
    for (const std::uint32_t cores : {2u, 1u}) {
        auto opened = Expeditor::Open(ConfigAt(cores), /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        ASSERT_TRUE(Started(opened.value()->Start()));
        const TransactionPlacer* placer = opened.value()->transaction_placer();
        if (cores == 1) {
            EXPECT_EQ(placer, nullptr) << "a placer at one core";
        } else {
            EXPECT_NE(dynamic_cast<const LoadPlacer*>(placer), nullptr)
                << "no balancer places the transactions";
        }
    }
}

// Where the port is shared the kernel places each accept, and the handoff
// carries only BL's moves (BL-S3): it exists above one core, and core 0
// routes no accepted socket through it.
TEST_F(ExpeditorTest, WhereThePortIsSharedTheHandoffRoutesNoAccept) {
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));
    ASSERT_NE(db.connection_handoff(), nullptr) << "no inbox for a move to cross";
    KwpClient client(config.port);
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run());
    ASSERT_TRUE(client.Hello());
    EXPECT_GE(client.Core(), 0);
    EXPECT_EQ(db.connection_handoff()->placed(), 0u) << "core 0 routed an accept it did not need to";
    EXPECT_TRUE(running.Stop().ok());
}

// The head of each autocommit statement is placed, so a session's
// statements run where the placer says, one move per head - and a row
// written on one core is read on the other, since a move carries nothing
// a core owns.
//
// **The mutation**: make `DrainFrames` skip the placer and every core read
// is the accepting one.
TEST_F(ExpeditorTest, EachTransactionsHeadRunsOnTheCoreThePlacerNames) {
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    ScriptedPlacer placer;
    config.transaction_placer = &placer;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));
    KwpClient client(config.port);
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run());
    ASSERT_TRUE(client.Hello());

    const int accepted = client.Core();
    ASSERT_TRUE(accepted == 0 || accepted == 1) << "SHOW META named no core";
    const int other = 1 - accepted;
    placer.target = other;
    EXPECT_EQ(client.Core(), other) << "the head did not move the session";
    EXPECT_EQ(client.Run("CREATE TABLE bl (id int64, v int64)").substr(0, 7), "CREATED");
    placer.target = accepted;
    EXPECT_EQ(client.Run("INSERT INTO bl VALUES (41)").substr(0, 8), "INSERTED");
    placer.target = other;
    EXPECT_NE(client.Run("SELECT v FROM bl WHERE id = 1").find("41"), std::string::npos)
        << "the row written on the other core is not read here";
    EXPECT_EQ(client.Core(), other);
    EXPECT_TRUE(running.Stop().ok());
}

// **A transaction stays on the core it began on** (BL-R1): the placer is
// asked at `BEGIN`, and not again until a statement finds no transaction
// open. The `[quiet-wrong]` clause - a move inside a transaction runs its
// next statement on a core whose in-flight table does not hold it.
//
// **The mutation**: drop the `in_explicit_txn()` test in `DrainFrames` and
// the statements inside the transaction follow the placer to the other core.
TEST_F(ExpeditorTest, ATransactionStaysOnTheCoreItBeganOn) {
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    ScriptedPlacer placer;
    config.transaction_placer = &placer;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));
    KwpClient client(config.port);
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run());
    ASSERT_TRUE(client.Hello());
    ASSERT_EQ(client.Run("CREATE TABLE bl (id int64, v int64)").substr(0, 7), "CREATED");

    // `BEGIN` is the head, so it moves the session - and the transaction
    // begins, and stays, on the core it moved to.
    const int accepted_on = client.Core();
    const int began_on = 1 - accepted_on;
    placer.target = began_on;
    ASSERT_EQ(client.Txn(wire::ClientFrameType::kTxnBegin), "TXN_OK");
    const int asked_at_begin = placer.asked.load();
    placer.target = accepted_on;  // every head from here would move
    EXPECT_EQ(client.Core(), began_on) << "a statement inside the transaction moved";
    EXPECT_EQ(client.Run("INSERT INTO bl VALUES (7)").substr(0, 8), "INSERTED");
    EXPECT_EQ(client.Core(), began_on);
    EXPECT_EQ(placer.asked.load(), asked_at_begin) << "the placer was asked inside a transaction";
    ASSERT_EQ(client.Txn(wire::ClientFrameType::kTxnCommit), "TXN_OK");
    // The transaction has ended: the next statement is a head.
    EXPECT_EQ(client.Core(), accepted_on);
    EXPECT_NE(client.Run("SELECT v FROM bl WHERE id = 1").find("7"), std::string::npos)
        << "the committed row is not read on the core the next head moved to";
    EXPECT_TRUE(running.Stop().ok());
}

// **Every core's load is read from any core, and nowhere at one core**
// (BL-S1, BL-R9, `sched/core_load.hpp`): above one core `SHOW META` prints a
// `load_c<k>_*` block for each core, the asking core's counting this
// session and its statements; at one core nothing writes one.
//
// **The mutation**: drop `SetCores` and the two-core half reads no block.
TEST_F(ExpeditorTest, EveryCoresLoadIsReadFromAnyCoreAndNoneAtOneCore) {
    for (const std::uint32_t cores : {2u, 1u}) {
        Expeditor::Config config = ConfigAt(cores);
        ScriptedPlacer stay;  // the session's core is read, so it must not move
        config.transaction_placer = &stay;
        auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        Expeditor& db = *opened.value();
        ASSERT_TRUE(Started(db.Start()));
        KwpClient client(config.port);
        RunningInstance running(db, config.debug_text_port);
        ASSERT_TRUE(running.Run());
        ASSERT_TRUE(client.Hello());
        const int core = client.Core();
        if (cores == 1) {
            const std::string load = client.Run("SELECT field FROM sys.meta WHERE section = 'load'");
            ASSERT_EQ(load.rfind("ERR", 0), std::string::npos) << load;
            EXPECT_EQ(load.find("load_c"), std::string::npos) << "a load word at one core";
        } else {
            const std::string mine = "load_c" + std::to_string(core) + "_";
            // Read first: this statement is the session's second head.
            const std::string heads = client.Meta(mine + "heads");
            ASSERT_FALSE(heads.empty());
            EXPECT_GE(std::atoi(heads.c_str()), 2)
                << "the session's two statements were not counted as heads";
            const std::string sessions = client.Meta(mine + "sessions");
            ASSERT_FALSE(sessions.empty());
            EXPECT_GE(std::atoi(sessions.c_str()), 1);
            for (int k = 0; k < 2; ++k) {
                EXPECT_FALSE(client.Meta("load_c" + std::to_string(k) + "_busy_ppm").empty())
                    << "no load block for core " << k;
            }
        }
        EXPECT_TRUE(running.Stop().ok());
    }
}

// **A prepared statement is session state, and travels with it** (BL §1.2):
// parsed on one core, bound and executed on the other, with the result it
// gives where it was parsed.
TEST_F(ExpeditorTest, AStatementPreparedOnOneCoreExecutesOnTheOther) {
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    ScriptedPlacer placer;
    config.transaction_placer = &placer;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));
    KwpClient client(config.port);
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run());
    ASSERT_TRUE(client.Hello());
    ASSERT_EQ(client.Run("CREATE TABLE bl (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(client.Run("INSERT INTO bl VALUES (5)").substr(0, 8), "INSERTED");

    const int parsed_on = client.Core();
    ASSERT_TRUE(client.Prepare("q", "SELECT v FROM bl WHERE id = 1"));
    const std::string here = client.Execute("q");
    EXPECT_NE(here.find('5'), std::string::npos) << here;
    placer.target = 1 - parsed_on;
    EXPECT_EQ(client.Execute("q"), here) << "the statement parsed on the other core answers otherwise";
    EXPECT_EQ(client.Core(), 1 - parsed_on);
    EXPECT_TRUE(running.Stop().ok());
}

// **Bytes the source already read travel with the connection**: two
// statements in one write, the first moving the session - its frames and
// the second statement's are in the source's decoder by then. Both are
// answered, in order, the second where the first moved it.
//
// **The mutation**: clear `decoder` in `MoveConnection` and the second
// answer never comes.
TEST_F(ExpeditorTest, APipelinedStatementTravelsWithTheConnectionThatMoved) {
    Expeditor::Config config = ConfigAt(/*cores=*/2);
    ScriptedPlacer placer;
    config.transaction_placer = &placer;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    Expeditor& db = *opened.value();
    ASSERT_TRUE(Started(db.Start()));
    KwpClient client(config.port);
    RunningInstance running(db, config.debug_text_port);
    ASSERT_TRUE(running.Run());
    ASSERT_TRUE(client.Hello());

    const int other = 1 - client.Core();
    placer.target = other;
    const std::string core = "SELECT value FROM sys.meta WHERE field = 'core'";
    const auto [first, second] = client.RunTwo(core, core);
    EXPECT_EQ(first, std::to_string(other) + " \n") << first;
    EXPECT_EQ(second, std::to_string(other) + " \n") << second;
    EXPECT_TRUE(running.Stop().ok());
}

// ---- AN-R14: the transaction lifetime ceiling, instance-wide ---------------
//
// `docs/spec/txn.md` §1. An explicit transaction older than
// `txn_lifetime_ceiling_ms`, counted from its `BEGIN`, is rolled back by
// the listener's sweep, and the client learns it at its next statement. Run
// on a real instance over the debug text port because the sweep is the
// listener's and the key is the Expeditor's: a dispatcher-level cell would
// test neither.
//
// **Two bounds, and the ceiling is sized for the tighter one.** Each cell's
// setup - at most three statement round trips after its `BEGIN`, in the
// in-flight cell - must finish inside one ceiling, or a setup assertion
// meets the refusal; 1 s is that budget on a loaded `-j8` Debug host. Every
// assertion after it is a lower bound the sweep cannot beat (it never fires
// before the ceiling) or an outcome three ceilings on, where the sweep has
// had eight ticks to act.

constexpr std::uint64_t kCellCeilingMs = 1000;
constexpr auto kWellPastTheCeiling = std::chrono::milliseconds(3 * kCellCeilingMs);

bool Refused(const std::string& reply) {
    return reply.rfind("ERR", 0) == 0 && reply.find("lifetime ceiling") != std::string::npos;
}

TEST_F(ExpeditorTest, AnIdleTransactionPastItsLifetimeIsRolledBackAndItsNextStatementIsTold) {
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    config.txn_lifetime_ceiling_ns = kCellCeilingMs * 1'000'000ULL;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    ASSERT_TRUE(Started(opened.value()->Start()));
    RunningInstance running(*opened.value(), config.debug_text_port);
    ASSERT_TRUE(running.Run());
    const int fd = running.client();

    ASSERT_EQ(SendLine(fd, "CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(SendLine(fd, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(SendLine(fd, "INSERT INTO t VALUES (1, 10)").rfind("INSERTED", 0), 0u);
    std::this_thread::sleep_for(kWellPastTheCeiling);

    // Every statement but the way out is refused, and says why.
    const std::string next = SendLine(fd, "SELECT * FROM t");
    EXPECT_TRUE(Refused(next)) << next;
    // **COMMIT above all**: answering it OK over writes the server already
    // undid would be a wrong answer, not a refusal.
    const std::string commit = SendLine(fd, "COMMIT");
    EXPECT_TRUE(Refused(commit)) << commit;
    EXPECT_EQ(SendLine(fd, "ROLLBACK").rfind("ROLLBACK trx_id=", 0), 0u);

    // The insert is gone, and the session is in autocommit again.
    const std::string gone = SendLine(fd, "SELECT * FROM t WHERE id = 1");
    ASSERT_EQ(gone.rfind("ERR", 0), std::string::npos) << gone;
    EXPECT_EQ(gone.find("10"), std::string::npos)
        << "the swept transaction's insert survived its rollback: " << gone;
    EXPECT_EQ(SendLine(fd, "INSERT INTO t VALUES (2, 20)").rfind("INSERTED", 0), 0u);
    EXPECT_TRUE(running.Stop().ok());
}

TEST_F(ExpeditorTest, ABusyTransactionIsRolledBackAtItsLifetimeNotItsIdleness) {
    // AN-R14's cost, stated in `txn.md` §1: a transaction that keeps
    // issuing statements is still ended at its lifetime. **The mutation**:
    // stamp the connection on every statement completion rather than once
    // per transaction, which is a bound on idleness, and this cell's
    // statements - 30 ms apart - are never refused.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    config.txn_lifetime_ceiling_ns = kCellCeilingMs * 1'000'000ULL;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    ASSERT_TRUE(Started(opened.value()->Start()));
    RunningInstance running(*opened.value(), config.debug_text_port);
    ASSERT_TRUE(running.Run());
    // A second connection for the transaction, closed without a ROLLBACK
    // once it is swept, so the close path meets an expired session; the
    // PING after it shows only that the instance survived, not that the
    // close was handled first.
    const int fd = ConnectToLoopback(config.debug_text_port);
    ASSERT_GE(fd, 0);

    ASSERT_EQ(SendLine(fd, "CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    const auto began = std::chrono::steady_clock::now();
    ASSERT_EQ(SendLine(fd, "BEGIN").rfind("BEGIN", 0), 0u);
    std::string reply;
    int statements = 0;
    while (std::chrono::steady_clock::now() - began < std::chrono::seconds(10)) {
        reply = SendLine(fd, "SELECT * FROM t");
        if (Refused(reply)) break;
        ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
        ++statements;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const auto refused_after = std::chrono::steady_clock::now() - began;
    ASSERT_TRUE(Refused(reply)) << "a transaction issuing a statement every 30 ms ran for 10 s "
                                   "under a " << kCellCeilingMs << " ms lifetime ceiling ("
                                << statements << " statements): " << reply;
    EXPECT_GE(refused_after, std::chrono::milliseconds(kCellCeilingMs))
        << "the sweep ended the transaction before its lifetime";
    ::close(fd);

    EXPECT_EQ(SendLine(running.client(), "PING"), "PONG");
    EXPECT_TRUE(running.Stop().ok());
}

TEST_F(ExpeditorTest, ADdlInsideAnExplicitTransactionIsRolledBackWithIt) {
    // The operator's ruling of 2026-10-12: AN-R14's DDL exemption does not
    // cover an explicit transaction. Such a transaction holds its relation
    // `X`, so left open it blocks every writer of it, which is the case the
    // ceiling exists for. The relation is gone after the sweep, and its
    // name is free.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    config.txn_lifetime_ceiling_ns = kCellCeilingMs * 1'000'000ULL;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    ASSERT_TRUE(Started(opened.value()->Start()));
    RunningInstance running(*opened.value(), config.debug_text_port);
    ASSERT_TRUE(running.Run());
    const int fd = running.client();

    ASSERT_EQ(SendLine(fd, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(SendLine(fd, "CREATE TABLE d (id int64, v int64)").rfind("CREATED", 0), 0u);
    std::this_thread::sleep_for(kWellPastTheCeiling);

    const std::string next = SendLine(fd, "INSERT INTO d VALUES (1, 1)");
    EXPECT_TRUE(Refused(next)) << next;
    EXPECT_EQ(SendLine(fd, "ROLLBACK").rfind("ROLLBACK trx_id=", 0), 0u);
    const std::string created = SendLine(fd, "CREATE TABLE d (id int64, v int64)");
    EXPECT_EQ(created.rfind("CREATED", 0), 0u)
        << "the swept transaction's CREATE TABLE still holds its name: " << created;
    EXPECT_TRUE(running.Stop().ok());
}

TEST_F(ExpeditorTest, AStatementInFlightPastTheCeilingRunsToItsEnd) {
    // The sweep defers a connection with a statement in flight; it does
    // not interrupt it. B begins first and parks on a row A holds, so B is
    // past its ceiling while in flight; A, past its own a moment later and
    // idle, is swept, which releases the row; B's UPDATE then completes
    // normally, and B is swept at the next tick after it. **The mutation**:
    // drop the `in_flight` skip, and the sweep rolls B back under its
    // parked statement.
    Expeditor::Config config = ConfigAt(/*cores=*/1);
    config.txn_lifetime_ceiling_ns = kCellCeilingMs * 1'000'000ULL;
    // Far past the ceiling, so B's wait ends at A's rollback and never at
    // the lock family's fault net.
    config.lock_wait_fault_net_ns = 30ULL * 1'000'000'000ULL;
    auto opened = Expeditor::Open(config, /*now_unix_seconds=*/1000);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    ASSERT_TRUE(Started(opened.value()->Start()));
    RunningInstance running(*opened.value(), config.debug_text_port);
    ASSERT_TRUE(running.Run());
    const int a = ConnectToLoopback(config.debug_text_port);
    const int b = ConnectToLoopback(config.debug_text_port);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);

    ASSERT_EQ(SendLine(a, "CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(SendLine(a, "INSERT INTO t VALUES (1, 10)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(SendLine(b, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(SendLine(a, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(SendLine(a, "UPDATE t SET v = 11 WHERE id = 1").rfind("UPDATED 1", 0), 0u);

    // Parks on A's row until A is swept, so its reply is the whole proof.
    const std::string parked = SendLine(b, "UPDATE t SET v = 12 WHERE id = 1");
    EXPECT_EQ(parked.rfind("UPDATED 1", 0), 0u)
        << "the sweep interrupted a statement in flight: " << parked;
    std::this_thread::sleep_for(kWellPastTheCeiling);
    const std::string after = SendLine(b, "SELECT * FROM t");
    EXPECT_TRUE(Refused(after)) << "B was not swept once its statement ended: " << after;
    EXPECT_TRUE(Refused(SendLine(a, "SELECT * FROM t")));
    EXPECT_EQ(SendLine(a, "ROLLBACK").rfind("ROLLBACK trx_id=", 0), 0u);
    EXPECT_EQ(SendLine(b, "ROLLBACK").rfind("ROLLBACK trx_id=", 0), 0u);
    // Both rolled back: the row is as it was before either.
    const std::string row = SendLine(a, "SELECT * FROM t WHERE id = 1");
    EXPECT_NE(row.find("10"), std::string::npos) << row;
    ::close(a);
    ::close(b);
    EXPECT_TRUE(running.Stop().ok());
}

}  // namespace
}  // namespace kds::server
