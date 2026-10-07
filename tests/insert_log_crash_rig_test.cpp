#include "file_rig_crash.hpp"
#include "two_core_rig.hpp"

#include <array>
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
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
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
//
// **Most cells name their keys ascending**, the shape BB-S3 reshaped them
// to while a named key below the mark was refused: a split is an append at
// the rightmost leaf, and a second core that has to reach a shared page - an
// index leaf, the var-heap tail - without passing through the leaf the first
// holds gets there by updating rows of an earlier leaf. **Two cells name
// keys below the mark** and were restored at BD-S3
// (`instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`), where a
// leaf places a key where it sorts again: a divide under another core, and a
// mid-chain append split.
//
// **The catalog's new-page cell is the exception** (AY-S7): its seam is the
// catalog's DDL undo hook, which runs in the same gap of
// `Catalog::InsertRow`, and it mounts nothing - what it asserts is the
// page image the writeback put in the file.

namespace kds::server {
namespace {

namespace fs = std::filesystem;

constexpr auto kGive = std::chrono::milliseconds(500);

using crash_rig::Mount;
using crash_rig::Ok;
using crash_rig::OpenFileRig;
using crash_rig::StartsWith;
using crash_rig::TempDir;

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

// Where a single-row `INSERT` put its row, by the name its reply gives the
// field: `INSERTED oid=.. id=.. page=.. slot=..`.
std::uint64_t ReplyField(const std::string& reply, const std::string& name) {
    const std::size_t at = reply.find(" " + name + "=");
    EXPECT_NE(at, std::string::npos) << name << " in " << reply;
    return at == std::string::npos ? 0 : std::stoull(reply.substr(at + name.size() + 2));
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

// The rows a leaf of `t (id int64, v int64)` holds: a leaf's body from its
// slot directory's start to the next_page_id reservation (8140 bytes), over
// what a row costs there - its 16-byte payload (the Keystone word and `v`),
// a 20-byte MVCC header and a 5-byte slot.
constexpr std::size_t kRowsPerLeaf =
    (heap::kNextPageIdOffset - (heap::kHeapHeaderOffset + heap::kHeaderSize)) /
    (kKeystoneWordSize + sizeof(std::int64_t) + heap::kTupleHeaderOnDiskSize +
     heap::kSlotOnDiskSize);
static_assert(kRowsPerLeaf == 198);

// `rows` rows at ids 10, 20, ..., ascending, so every leaf but the rightmost
// is full: the default 400 fill two leaves, to 1980 and 3960, and put four
// rows in a third.
std::set<std::uint64_t> Fill(CommandDispatcher& d0, std::uint64_t rows = 400) {
    std::set<std::uint64_t> ids;
    for (std::uint64_t k = 1; k <= rows; ++k) {
        Ok(d0, "INSERT INTO t VALUES (" + std::to_string(k * 10) + ", " + std::to_string(k) + ")");
        ids.insert(k * 10);
    }
    return ids;
}

TEST(InsertLogCrashRigTest, ARowWhoseLeafAnotherCoreFilledAndSplitBeforeItWasLoggedRecoversOnce) {
    // **Window 1.** Core 0 places row 4020 in the rightmost leaf. Before its
    // record is appended, core 1 fills that leaf past its room and splits it
    // right: its rows take the slots after row 4020's, and the split logs
    // the old leaf's image with row 4020 already in it. Core 0's
    // `HEAP_INSERT(leaf, slot)` then lands after all of it, and redo meets
    // core 1's first record naming a slot past the page's next - a heap
    // page's slots are dense, so it refuses the mount.
    //
    // **The divide is withdrawn** (BB-R3, on BB-Q8 (b)). Until BB, core 0
    // named 15 into the full first leaf, which divided it, and core 1's keys
    // below the mark divided it again - a rebuild that renumbered the slot
    // core 0's record names, so redo put row 15 over another row. A named
    // key below the mark is refused now and no insert divides a leaf; an
    // append split moves no row, so what is left of the window is the order
    // of one leaf's records, which this keeps.
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
                // A leaf's worth above row 4020: more than its leaf has room
                // for, so it splits right.
                for (std::uint64_t id = 4021; id <= 4020 + kRowsPerLeaf; ++id) {
                    Ok(d1, "INSERT INTO t VALUES (" + std::to_string(id) + ", 0)");
                }
            });
            other->Wait();
        });
        const std::string placed = d0.Dispatch("INSERT INTO t VALUES (4020, 0)").response;
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_TRUE(StartsWith(placed, "INSERTED")) << placed;
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ids.insert(4020);
        for (std::uint64_t id = 4021; id <= 4020 + kRowsPerLeaf; ++id) ids.insert(id);
        {
            auto leaf = rig->store().GetForRead(static_cast<PageId>(ReplyField(placed, "page")));
            ASSERT_TRUE(leaf.ok()) << leaf.status().message();
            ASSERT_NE(heap::PageView(leaf.value().bytes()).next_page_id(), kInvalidPageId)
                << "core 1 never split row 4020's leaf; the cell tested nothing";
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

TEST(InsertLogCrashRigTest, ARowWhoseLeafAnotherCoreDividedBeforeItWasLoggedRecoversOnce) {
    // **Restored at BD-S3** (`instructions/v3.0.0/workorder-bd-sorted-leaf-
    // named-keys.md`): BB-S3 withdrew this shape when a named key below the
    // mark was refused; a leaf places it where it sorts again.
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
    // Core 0's 5950 finds the rightmost leaf full and splits it right: it
    // creates a leaf, puts a separator into the root and links the old leaf
    // to the new - and appends none of it until the row's own record. Core 1
    // writes the root back in that gap, and the crash is taken before the
    // log reaches the file. The root on disk then routes to a leaf no record
    // creates.
    //
    // **An append split since BB-R3** (on BB-Q8 (b)): core 0 named 15 into
    // the full first leaf, a divide, and a named key below the mark is
    // refused now. Either split puts a separator into the root, so the
    // window is the same one; the fill is three leaves' worth, which leaves
    // the rightmost full.
    TempDir snap;
    std::set<std::uint64_t> ids;
    {
        auto rig = OpenFileRig();
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d0 = rig->core(0).dispatcher();
        CurrentCoreGuard as(0);
        Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE");
        ids = Fill(d0, 3 * kRowsPerLeaf);  // to 5940
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
        const std::string placed = d0.Dispatch("INSERT INTO t VALUES (5950, 0)").response;
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_TRUE(StartsWith(placed, "INSERTED")) << placed;
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ASSERT_TRUE(snapped);
        // The first row on its page is the row that opened it.
        ASSERT_EQ(ReplyField(placed, "slot"), 0u)
            << "row 5950 did not open a leaf, so nothing split; the cell tested nothing";
    }

    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    Expeditor& db = *mounted.value();
    const auto got = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end()))
        << "the crash came before row 5950 was logged";
    ExpectTreeWhole(db);
}

TEST(InsertLogCrashRigTest, AParentWrittenBackBeforeItsDivideIsLoggedDoesNotRouteToNothing) {
    // **Restored at BD-S3** (`instructions/v3.0.0/workorder-bd-sorted-leaf-
    // named-keys.md`) under a new name, beside the variant BB-S3 reshaped
    // it into while a named key below the mark was refused.
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

TEST(InsertLogCrashRigTest, AnIndexRecordCarriesItsOwnRowsEntryAcrossAnotherCoresUpdates) {
    // **Window 3.** Core 0 inserts row 5000 with v = 500, whose index entry
    // goes into a leaf of `ix`. Before its records are appended, core 1 sets
    // v = 499 on rows of another clustered leaf - an `UPDATE` that moves an
    // indexed key adds an entry for the new one (`index.md` §2) - into the
    // same index leaf, ahead of core 0's entry, which shifts it. The index
    // record core 0 then appends names a slot its entry no longer holds, and
    // was taken by re-reading that slot: redo refuses the mount, or places
    // another row's entry and loses this one's.
    //
    // **Updates since BB-R3** (on BB-Q8 (b)). Core 1 inserted rows 11 to
    // 391, below the mark, which reached the first clustered leaf; a named
    // key below the mark is refused now, and one above it lands in the leaf
    // core 0 holds across the gap, where core 1 would wait and reach the
    // index only after core 0's record. An update reaches the index from a
    // leaf core 0 does not hold.
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
                for (std::uint64_t id = 10; id < 400; id += 10) {
                    Ok(d1, "UPDATE t SET v = 499 WHERE id = " + std::to_string(id));
                }
            });
            other->Wait();
        });
        Ok(d0, "INSERT INTO t VALUES (5000, 500)");
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ids.insert(5000);
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }

    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    Expeditor& db = *mounted.value();
    EXPECT_EQ(Ids(db.dispatcher(), "SELECT id FROM t WHERE v = 500"),
              std::multiset<std::uint64_t>({5000}))
        << "the index lost row 5000's entry";
    std::multiset<std::uint64_t> moved;
    for (std::uint64_t id = 10; id < 400; id += 10) moved.insert(id);
    EXPECT_EQ(Ids(db.dispatcher(), "SELECT id FROM t WHERE v = 499"), moved)
        << "the index lost an entry core 1's updates wrote";
    const auto got = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end()));
    ExpectTreeWhole(db);
}

TEST(InsertLogCrashRigTest, AnIndexRecordCarriesItsOwnRowsEntryAcrossAnotherCoresInserts) {
    // **Restored at BD-S3** (`instructions/v3.0.0/workorder-bd-sorted-leaf-
    // named-keys.md`): BB-S3 reshaped it while a named key below the mark
    // was refused; a leaf places that key where it sorts again.
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
    // **Restored at BD-S3** (`instructions/v3.0.0/workorder-bd-sorted-leaf-
    // named-keys.md`): BB-S3 withdrew this shape when a named key below the
    // mark was refused; a leaf places it where it sorts again.
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
    // 1's updates spill into the same page after it. A var-heap record names
    // a slot and redo refuses one that is not the page's next, so core 0's
    // record, logged after core 1's, refused the mount.
    //
    // **Core 1 updates since BB-R3** (on BB-Q8 (b)). Core 0's row was 25, a
    // named key below the mark that landed in the first leaf, away from core
    // 1's inserts in the last; it is refused now. Above the mark core 0's row
    // lands in the rightmost leaf, which it holds across the gap, and an
    // insert from core 1 would land there too and wait for it - the spills'
    // order would then be the leaf hold's, not the spill's own record's. So
    // core 1 rewrites rows of the first leaf, which core 0 does not hold.
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
        // Enough short rows for several clustered leaves, so the rows core 1
        // updates below sit in a leaf core 0 does not hold: what they share
        // is the var-heap tail, and nothing else.
        for (std::uint64_t k = 1; k <= 400; ++k) {
            Ok(d0, "INSERT INTO t VALUES (" + std::to_string(k * 10) + ", 'x')");
            ids.insert(k * 10);
        }
        Ok(d0, "INSERT INTO t VALUES (4010, '" + long_a + "')");
        ids.insert(4010);
        {
            CurrentCoreGuard as(1);
            Ok(d1, "INSERT INTO t VALUES (5000, 'y')");  // core 1's carve (above)
            ids.insert(5000);
        }

        std::unique_ptr<OtherCore> other;
        d0.SetBeforeInsertLogForTest([&] {
            if (other != nullptr) return;
            other = std::make_unique<OtherCore>([&] {
                for (std::uint64_t id = 10; id <= 100; id += 10) {
                    Ok(d1, "UPDATE t SET s = '" + long_b + "' WHERE id = " + std::to_string(id));
                }
            });
            other->Wait();
        });
        Ok(d0, "INSERT INTO t VALUES (5010, '" + long_a + "')");
        d0.SetBeforeInsertLogForTest(nullptr);
        ASSERT_NE(other, nullptr) << "the seam never ran; the cell tested nothing";
        other->Join();
        ids.insert(5010);
        ASSERT_TRUE(rig->Snapshot(snap.path).ok());
    }
    auto mounted = Mount(snap.path);
    ASSERT_TRUE(mounted.ok()) << "the mount refused the crash: " << mounted.status().message();
    Expeditor& db = *mounted.value();
    const auto got = Ids(db.dispatcher(), "SELECT id FROM t");
    EXPECT_EQ(got, std::multiset<std::uint64_t>(ids.begin(), ids.end())) << Diff(got, ids);
    EXPECT_NE(db.dispatcher().Dispatch("SELECT s FROM t WHERE id = 5010").response.find(long_a),
              std::string::npos)
        << "row 5010's spilled value did not come back";
    for (std::uint64_t id = 10; id <= 100; id += 10) {
        EXPECT_NE(db.dispatcher()
                      .Dispatch("SELECT s FROM t WHERE id = " + std::to_string(id))
                      .response.find(long_b),
                  std::string::npos)
            << "row " << id << "'s spilled value did not come back";
    }
}

TEST(InsertLogCrashRigTest, TwoCoresInsertingSpillsIntoOneVarHeapPageRecoverInTheOrderTheyWrote) {
    // **Restored at BD-S3** (`instructions/v3.0.0/workorder-bd-sorted-leaf-
    // named-keys.md`) under a new name, beside the variant BB-S3 reshaped
    // it into while a named key below the mark was refused.
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

        // Unallocated before the loop, so the hook's first event on it is
        // the new-page arm's. Were it the tail arm's, which holds its page
        // across the hook, the cell would pass with or without the fix.
        ASSERT_FALSE(rig->store().IsAllocated(fresh));
        std::unique_ptr<OtherCore> other;
        // One-shot by `other`.
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
