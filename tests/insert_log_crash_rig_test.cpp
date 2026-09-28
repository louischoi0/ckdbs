#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/server/expeditor.hpp"

#include "tree_structure.hpp"

// **Every insert logged under the hold that placed it** (AT-S21, the
// AT-close order's §7.3; `docs/inflight/bugs/an-insert-is-logged-after-its-leaf-is-released.md`).
//
// An insert placed its row, let its pages go, and appended their records
// afterwards, on a single-cooperative-thread argument AT-S5 retired. Each
// cell here puts a second core in that gap - through the dispatcher's one
// test seam, `SetBeforeInsertLogForTest`, which runs after the row is placed
// and before its record is appended - then takes what a crash at a chosen
// instant would leave (`TwoCoreRig::Snapshot`: the log flushed, the pages
// only as far as the store wrote them back) and brings it up through
// production's mount (`Expeditor::Open`). What is asserted is after the
// mount: every row once, where a descent finds it, and the tree's structure
// (`tree_structure.hpp`) - not only the one row.
//
// **The second core is a thread given a bounded time.** With the fix its
// write needs a page the first core holds until its record is stamped, so
// it waits; the seam gives it `kGive` and lets the first core go on, which
// is what a real second core would see. Without the fix it finishes inside
// that time and the window is open.

namespace kds::server {
namespace {

namespace fs = std::filesystem;

constexpr auto kGive = std::chrono::milliseconds(500);

bool StartsWith(const std::string& s, std::string_view prefix) {
    return s.rfind(prefix, 0) == 0;
}

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
               ("kds_insert_log_crash_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter.fetch_add(1)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::unique_ptr<TwoCoreRig> OpenFileRig() {
    TwoCoreRig::Options options;
    options.file_backed = true;
    auto opened = TwoCoreRig::Open(options);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

// Production's mount over a snapshot: the recovery that runs is the one a
// restart runs.
StatusOr<std::unique_ptr<Expeditor>> Mount(const fs::path& snapshot) {
    Expeditor::Config config;
    config.data_file = (snapshot / "kds.db").string();
    config.wal_dir = (snapshot / "wal").string();
    config.log_file = {};
    config.cores = 2;
    config.debug_text_port = 0;
    return Expeditor::Open(config, /*now_unix_seconds=*/2000);
}

// The ids a `SELECT` answers, one per line after the header.
std::multiset<std::uint64_t> Ids(CommandDispatcher& d, const std::string& sql) {
    std::multiset<std::uint64_t> out;
    const std::string reply = d.Dispatch(sql).response;
    EXPECT_FALSE(StartsWith(reply, "ERR")) << sql << " -> " << reply;
    // The text reply escapes its row separator: `id\\n1\\n2`, header first.
    std::size_t at = reply.find("\\n");
    while (at != std::string::npos) {
        const std::size_t next = reply.find("\\n", at + 2);
        out.insert(std::stoull(reply.substr(
            at + 2, next == std::string::npos ? std::string::npos : next - at - 2)));
        at = next;
    }
    return out;
}

void Ok(CommandDispatcher& d, const std::string& sql) {
    const std::string reply = d.Dispatch(sql).response;
    ASSERT_FALSE(StartsWith(reply, "ERR")) << sql << " -> " << reply;
}

// `t`'s tree, as the mounted catalog names its root.
void ExpectTreeWhole(Expeditor& db) {
    auto oid = db.catalog().FindTableOidByName("t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto access = db.catalog().InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    testing_race::ExpectBtreeSeparatorsBoundTheirSubtrees(db.store(), access.value()->desc_page_id);
}

// Runs `fn` as core 1 on its own thread; `Wait()` gives it up to `kGive`.
class OtherCore {
public:
    template <typename Fn>
    explicit OtherCore(Fn fn)
        : thread_([this, fn] {
              CurrentCoreGuard as(1);
              fn();
              done_.store(true, std::memory_order_release);
          }) {}
    ~OtherCore() {
        if (thread_.joinable()) thread_.join();
    }
    bool Wait() {
        const auto until = std::chrono::steady_clock::now() + kGive;
        while (!done_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done_.load(std::memory_order_acquire);
    }
    void Join() {
        if (thread_.joinable()) thread_.join();
    }

private:
    std::atomic<bool> done_{false};
    std::thread thread_;
};

// 400 rows at ids 10, 20, ... 4000, in two leaves; the first is full.
std::set<std::uint64_t> Fill(CommandDispatcher& d0) {
    std::set<std::uint64_t> ids;
    for (std::uint64_t k = 1; k <= 400; ++k) {
        Ok(d0, "INSERT INTO t VALUES (" + std::to_string(k * 10) + ", " + std::to_string(k) + ")");
        ids.insert(k * 10);
    }
    return ids;
}

TEST(InsertLogCrashRigTest, ARowWhoseLeafAnotherCoreDividedBeforeItWasLoggedRecoversOnce) {
    // **Window 1.** Core 0 places row 15 in the first leaf, which divides
    // it. Before its record is appended, core 1 fills that leaf back up and
    // divides it again - a rebuild that renumbers the slot core 0's record
    // will name - and logs the divide. Core 0's `HEAP_INSERT(leaf, slot)`
    // then lands after that image, and redo re-inserts row 15 at a slot
    // another row now owns.
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CommandDispatcher& d1 = rig->core(1).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0);

        std::unique_ptr<OtherCore> other;
        // One-shot by `other`, not by resetting the hook from inside it:
        // that would destroy the function while it runs.
        d0.SetBeforeInsertLogForTest([&] {
            if (other != nullptr) return;
            other = std::make_unique<OtherCore>([&] {
                // Twice around the leaf's lower half: more than it has room
                // for, so it divides again.
                for (std::uint64_t id = 11; id < 1000; id += 10) {
                    Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id) + ", 0)");
                    Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id + 1) + ", 0)");
                }
            });
            other->Wait();
        });
        Ok(d0, "INSERT INTO t VALUES (15, 0)");
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ids.insert(15);
        for (std::uint64_t id = 11; id < 1000; id += 10) {
            ids.insert(id);
            ids.insert(id + 1);
        }
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }

    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    Expeditor& db = *mounted.value();
    const auto got = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end()))
        << "row 15 came back " << got.count(15) << " times";
    ExpectTreeWhole(db);
}

TEST(InsertLogCrashRigTest, AParentWrittenBackBeforeItsSplitIsLoggedDoesNotRouteToNothing) {
    // **Window 2, where it bites.** On the row's own leaf a writeback in
    // the gap is survivable: the row's begin and undo records precede the
    // gap, the store's gate flushes the log ahead of any page, and recovery
    // rolls the loser back. What is not survivable is the **structure**.
    // Core 0's 15 divides the first leaf: it creates a leaf, rewrites the
    // old one and puts a separator into the root - and appends none of it
    // until the row's own record. Core 1 writes the root back in that gap,
    // and the crash is taken before the log reaches the file. The root on
    // disk then routes to a leaf no record creates.
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0);
        ASSERT_TRUE(rig->store().Sync().ok());  // everything so far on disk, logged
        PageId root = kInvalidPageId;
        {
            auto oid = rig->core(0).catalog().FindTableOidByName("t");
            ASSERT_TRUE(oid.ok());
            auto access = rig->core(0).catalog().InitTableAccess(oid.value());
            ASSERT_TRUE(access.ok());
            root = access.value()->desc_page_id;
        }

        std::unique_ptr<OtherCore> other;
        bool snapped = false;
        // One-shot by `other`, not by resetting the hook from inside it:
        // that would destroy the function while it runs.
        d0.SetBeforeInsertLogForTest([&] {
            if (other != nullptr) return;
            other = std::make_unique<OtherCore>([&] {
                const PageId only[] = {root};
                (void)rig->store().WriteBack(only);
            });
            other->Wait();
            // Not flushed: the crash comes before the log reaches the
            // file - what is there is what the writeback's gate put there.
            snapped = rig->Snapshot(snap.path, /*flush_log=*/false).ok();
        });
        Ok(d0, "INSERT INTO t VALUES (15, 0)");
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ASSERT_TRUE(snapped);
    }

    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    Expeditor& db = *mounted.value();
    const auto got = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end()))
        << "the crash came before row 15 was logged";
    ExpectTreeWhole(db);
}

TEST(InsertLogCrashRigTest, AnIndexRecordCarriesItsOwnRowsEntryAcrossAnotherCoresInserts) {
    // **Window 3.** Core 0 inserts row 5000 with v = 500, whose index entry
    // goes into a leaf of `ix`. Before its records are appended, core 1
    // inserts rows with v = 499 - into another clustered leaf, and into the
    // same index leaf, ahead of core 0's entry, which shifts it. The index
    // record core 0 then appends names a slot its entry no longer holds, and
    // was taken by re-reading that slot: redo refuses the mount, or places
    // another row's entry and loses this one's.
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CommandDispatcher& d1 = rig->core(1).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        Ok(d0, "CREATE INDEX ix ON t (v)");
        ids = Fill(d0);

        std::unique_ptr<OtherCore> other;
        // One-shot by `other`, not by resetting the hook from inside it:
        // that would destroy the function while it runs.
        d0.SetBeforeInsertLogForTest([&] {
            if (other != nullptr) return;
            other = std::make_unique<OtherCore>([&] {
                for (std::uint64_t id = 11; id < 400; id += 10) {
                    Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id) + ", 499)");
                }
            });
            other->Wait();
        });
        Ok(d0, "INSERT INTO t VALUES (5000, 500)");
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ids.insert(5000);
        for (std::uint64_t id = 11; id < 400; id += 10) ids.insert(id);
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }

    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    Expeditor& db = *mounted.value();
    EXPECT_EQ(Ids(db.dispatcher(), "SELECT id FROM t WHERE v = 500"),
              std::multiset<std::uint64_t>({5000}))
        << "the index lost row 5000's entry";
    const auto got = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end()));
    ExpectTreeWhole(db);
}

}  // namespace
}  // namespace kds::server
