#include "two_core_rig.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/storage/page_header.hpp"

#include "tree_structure.hpp"

// **Every insert logged under the hold that placed it** (AT-S21, the
// AT-close order's §7.3; the bug entry it closes, `an-insert-is-logged-after-its-leaf-is-
// released.md`, is deleted by it).
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

// The ids one set has and the other lacks, for a failure worth reading.
std::string Diff(const std::multiset<std::uint64_t>& got,
                 const std::set<std::uint64_t>& want) {
    std::ostringstream out;
    out << "missing:";
    for (std::uint64_t id : want) {
        if (got.count(id) == 0) out << ' ' << id;
    }
    out << "; extra or repeated:";
    for (std::uint64_t id : got) {
        if (want.count(id) == 0 || got.count(id) > 1) out << ' ' << id;
    }
    return out.str();
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

// **Core 1 carves its transaction-id window before the seam.** A core's
// first transaction carves a block of ids and persists page 0 through the
// store's `Sync()`, which waits out every held dirty frame - core 0's too,
// held by the fix until its record is stamped. Carved during the seam,
// core 1 would wait on that and never reach the window the cell is about,
// fixed or not; one write before the seam carves 4096 ids
// (`txn::kTrxIdBlockSize`), more than any cell here spends.
void CarveOnCoreOne(CommandDispatcher& d1, std::uint64_t id, std::set<std::uint64_t>& ids) {
    CurrentCoreGuard as(1);
    Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id) + ", 1)");
    ids.insert(id);
}

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
        CarveOnCoreOne(d1, 4010, ids);

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
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end())) << Diff(got, ids);
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
        CarveOnCoreOne(d1, 4010, ids);

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


TEST(InsertLogCrashRigTest, AMidChainAppendSplitKeepsItsRightLinkAcrossACrash) {
    // **Found by this stage's first window-1 run, and older than it.** One
    // core, no race. 15 divides the first leaf; the rows after it fill the
    // lower half back up and divide it again; the last, 992, sorts past
    // every row of the full leaf it lands in and opens a new leaf there - an
    // append-split **mid-chain**, whose right link names the leaf holding
    // 1000. That leaf was logged as a `PAGE_INIT`, which formats a page with
    // no link, and redo rebuilt it so: after the crash a scan ended at 992
    // while a point lookup still found 1000 and up.
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0);
        Ok(d0, "INSERT INTO t VALUES (15, 0)");
        ids.insert(15);
        for (std::uint64_t id = 11; id < 1000; id += 10) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(id) + ", 0)");
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(id + 1) + ", 0)");
            ids.insert(id);
            ids.insert(id + 1);
        }
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << mounted.status().message();
    const auto got = Ids(mounted.value()->dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end())) << Diff(got, ids);
    ExpectTreeWhole(*mounted.value());
}

TEST(InsertLogCrashRigTest, TwoCoresSpillingIntoOneVarHeapPageRecoverInTheOrderTheyWrote) {
    // **The spill window** (AT-S21's survey). Core 0's row spills a value
    // into the var-heap tail; before its `VARHEAP_APPEND` is appended, core
    // 1's rows spill into the same page after it. A var-heap record names a
    // slot and redo refuses one that is not the page's next, so core 0's
    // record, logged after core 1's, refused the mount.
    const std::string long_a(200, 'a');
    const std::string long_b(200, 'b');
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CommandDispatcher& d1 = rig->core(1).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, s varchar) BTREE");
        // Enough short rows for two clustered leaves, so core 1's rows below
        // land in a leaf core 0 does not hold: what they share is the
        // var-heap tail, and nothing else.
        for (std::uint64_t k = 1; k <= 400; ++k) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(k * 10) + ", 'x')");
            ids.insert(k * 10);
        }
        Ok(d0, "INSERT INTO t VALUES (15, '" + long_a + "')");
        ids.insert(15);
        {
            CurrentCoreGuard as(1);
            Ok(d1, "INSERT INTO t VALUES (5000, 'y')");  // core 1's carve (above)
            ids.insert(5000);
        }

        std::unique_ptr<OtherCore> other;
        d0.SetBeforeInsertLogForTest([&] {
            if (other != nullptr) return;
            other = std::make_unique<OtherCore>([&] {
                for (std::uint64_t id = 6000; id < 6010; ++id) {
                    Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id) + ", '" + long_b + "')");
                }
            });
            other->Wait();
        });
        Ok(d0, "INSERT INTO t VALUES (25, '" + long_a + "')");
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ids.insert(25);
        for (std::uint64_t id = 6000; id < 6010; ++id) ids.insert(id);
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    Expeditor& db = *mounted.value();
    const auto got = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end())) << Diff(got, ids);
    EXPECT_NE(db.dispatcher().Dispatch("SELECT s FROM t WHERE id = 25").response.find(long_a),
              std::string::npos)
        << "row 25's spilled value did not come back";
}

// ---- The spill's hold against a reader's (AT-S21's review, C1) ----------
//
// The fix's first shape held each spill's var-heap page until the row was
// logged - past the row's placement. An insert then held the var-heap tail
// and asked for the clustered leaf, while a reader holds the leaf and asks
// for the var-heap page to resolve the value: two cores stopped on page
// latches, which never time out. Reproduced by the review, 2 runs in 2, as
// "no progress for 10 s". A spill is noted and logged at its append now,
// and its hold drops before the row is placed.
//
// **A deadlock cannot be joined**, so these cells watch for progress and,
// finding none for 10 s, fail and end the process - one ctest cell, since
// each runs alone.
void RaceSpillingInsertsAgainst(const std::string& reader_sql) {
    auto opened = TwoCoreRig::Open();
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    CommandDispatcher& d1 = rig->core(1).dispatcher();
    const std::string big = "'" + std::string(1000, 'z') + "'";
    {
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, s varchar) BTREE");
        Ok(d0, "INSERT INTO t VALUES (1, " + big + ")");
    }
    {
        CurrentCoreGuard as(1);
        Ok(d1, "INSERT INTO t VALUES (2, " + big + ")");  // core 1's carve, outside the race
    }
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> read{0};
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        CurrentCoreGuard as(0);
        for (std::uint64_t id = 10; id < 1500 && !stop; ++id) {
            d0.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " + big + ")");
            ++written;
        }
        stop = true;
    });
    std::thread reader([&] {
        CurrentCoreGuard as(1);
        while (!stop) {
            d1.Dispatch(reader_sql);
            ++read;
        }
    });
    std::uint64_t seen_w = 0;
    std::uint64_t seen_r = 0;
    auto moved = std::chrono::steady_clock::now();
    while (!stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (written != seen_w || read != seen_r) {
            seen_w = written;
            seen_r = read;
            moved = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - moved > std::chrono::seconds(10)) {
            std::fprintf(stderr,
                         "DEADLOCK: no progress for 10 s; writer rows=%llu reader ops=%llu\n",
                         static_cast<unsigned long long>(seen_w),
                         static_cast<unsigned long long>(seen_r));
            std::fflush(stderr);
            std::_Exit(3);
        }
    }
    writer.join();
    reader.join();
}

TEST(InsertLogCrashRigTest, ASpillingInsertAndASpillReadingSelectBothFinish) {
    RaceSpillingInsertsAgainst("SELECT s FROM t");
}

TEST(InsertLogCrashRigTest, ASpillingInsertAndASpillingUpdateBothFinish) {
    RaceSpillingInsertsAgainst("UPDATE t SET s = '" + std::string(1000, 'x') + "'");
}

// ---- A new catalog page, logged under its hold (AY-S7, AY-R6) ---------

TEST(InsertLogCrashRigTest, AnotherCoresFlushOfANewCatalogPageWaitsForItsRecord) {
    // `Catalog::InsertRow`'s new-page arm places a row on the page
    // `AllocateCatalogPage` just created, fires the DDL undo hook, and then
    // appends the row's `HEAP_INSERT`, whose stamp is the page's first
    // `page_lsn`. Core 1 writes that page back from inside the hook - the
    // gap between the row and its record. With the page held from its
    // creation to the stamp, the writeback waits out the hold and what
    // reaches the file carries a stamp - that record's, or a later row's on
    // the same page, the `CREATE` going on writing `sys.columns` there. With
    // the hold gone at the allocation, it copies the row under `page_lsn` 0
    // ahead of the record, and the stamp that follows only re-dirties the
    // frame.
    TempDir snap;
    const PageId fresh = catalog::kCatalogOverflowFirst;
    bool flushed_in_the_gap = false;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CurrentCoreGuard as(0);
        catalog::Catalog& cat = rig->core(0).catalog();

        catalog::Schema schema;
        for (std::uint16_t pos = 0; pos < 2; ++pos) {
            catalog::SysColumnRow col{};
            col.pos = pos;
            catalog::SetName(col.name, pos == 0 ? "id" : "v");
            col.type_val = catalog::kTypeValInt64;
            col.len = 8;
            col.notnull = true;
            schema.columns.push_back(col);
        }

        std::unique_ptr<OtherCore> other;
        // The hook's first event on the overflow range is the new-page
        // arm's: nothing is on that page before it. One-shot by `other`.
        cat.SetDdlUndoHook([&](const catalog::Catalog::DdlUndoEvent& event) {
            if (other != nullptr || event.page_id != fresh) return Status::OK();
            other = std::make_unique<OtherCore>([&] {
                const PageId only[] = {fresh};
                (void)rig->store().WriteBack(only);
            });
            flushed_in_the_gap = other->Wait();
            return Status::OK();
        });
        for (int i = 0; i < 64 && other == nullptr; ++i) {
            auto created = cat.CreateTable(catalog::kNamespacePublic, "spill" + std::to_string(i),
                                           schema, catalog::ClusteredType::kBtree);
            ASSERT_TRUE(created.ok()) << created.status().message();
        }
        cat.SetDdlUndoHook(nullptr);
        ASSERT_NE(other, nullptr) << "the catalog never grew onto page " << fresh
                                  << "; the cell tested nothing";
        other->Join();
        // The file holds what the writeback put there and nothing since.
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    EXPECT_FALSE(flushed_in_the_gap)
        << "core 1 wrote the new page back before its record was appended";

    std::ifstream file(snap.path / "kds.db", std::ios::binary);
    std::array<std::byte, kPageSize> image{};
    file.seekg(static_cast<std::streamoff>(fresh) * kPageSize);
    file.read(reinterpret_cast<char*>(image.data()), kPageSize);
    ASSERT_TRUE(file.good());
    EXPECT_NE(storage::GetPageLsn(image), storage::kNoPageLsn)
        << "the page reached the file with a row and no record's stamp";
}

}  // namespace
}  // namespace kds::server
