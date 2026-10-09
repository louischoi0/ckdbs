// **A carve's ceiling survives a crash** (BA-S13, BA-R10), on a file-backed
// two-core rig.
//
// Since BA-S13 a transaction-id carve persists page 0 alone
// (`DevicePageStore::PersistPage`) instead of syncing the whole pool. The
// ceiling must still be durable before the carved window is used: a crash
// image taken right after the carve - nothing synced since - holds a
// superblock whose ceiling is at or above the window, so no id issued from
// it can be issued again after the restart. On alternate rounds another
// writeback holds page 0's claim, with an image older than the carve's,
// across the carve.
//
// The image copies the data file through the page cache, so a carve that
// wrote page 0 and skipped the `fdatasync` passes here: the cell pins the
// write and its order against the claim, not the sync.

#include "two_core_rig.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/server/superblock.hpp"

namespace kds::server {
namespace {

// The ceiling the crash image's page 0 records.
std::uint64_t CeilingIn(const std::filesystem::path& image) {
    std::array<std::byte, kPageSize> page{};
    std::ifstream in(image / "kds.db", std::ios::binary);
    in.read(reinterpret_cast<char*>(page.data()), static_cast<std::streamsize>(page.size()));
    EXPECT_TRUE(in.good()) << "the crash image's page 0 could not be read";
    auto decoded = SuperBlock::Decode(page);
    EXPECT_TRUE(decoded.ok()) << decoded.status().message();
    return decoded.ok() ? decoded.value().next_trx_id() : 0;
}

TEST(TrxIdCarveCrashRigTest, ACarvesCeilingIsDurableBeforeItsWindowIsUsed) {
    TwoCoreRig::Options options;
    options.file_backed = true;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_TRUE(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response.rfind("CREATED", 0) == 0);

    const std::filesystem::path image =
        std::filesystem::temp_directory_path() / ("kds_carve_crash_" + std::to_string(::getpid()));
    for (int round = 0; round < 20; ++round) {
        // Dirty, logged pages the carve must neither write nor wait for.
        ASSERT_TRUE(d0.Dispatch("INSERT INTO t VALUES (" + std::to_string(round) + ")")
                        .response.rfind("INSERTED", 0) == 0);
        // Even rounds carve alone, so page 0 can reach the disk only through
        // the carve's own persist. Odd rounds hold another writeback's claim
        // on page 0 across the carve, forced rather than left to the
        // scheduler: that writeback copies page 0 **before** the carve's
        // encode and stalls with the claim held, so a carve that skipped the
        // claimed frame (`kSkip`) would return with its ceiling unwritten.
        const bool race = round % 2 == 1;
        std::atomic<bool> copied{false};
        static thread_local bool racer = false;
        if (race) {
            rig->store().SetAfterWritebackCopyForTest([&](PageId page) {
                if (!racer || page != kSuperBlockPageId) return;
                copied.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            });
        }
        std::thread writeback([&] {
            if (!race) return;
            racer = true;
            const CurrentCoreGuard as(0);
            // Page 0 dirty with the pre-carve image: an exclusive fetch marks it.
            { auto page = rig->store().Get(kSuperBlockPageId); }
            const PageId ids[] = {kSuperBlockPageId};
            (void)rig->store().FlushPages(ids);
        });
        if (race) {
            while (!copied.load()) std::this_thread::yield();
        }
        const CurrentCoreGuard as(1);
        txn::TrxIdSequence& ids = rig->core(1).trx_ids();
        ASSERT_TRUE(ids.BurnWindow().ok());
        const std::uint64_t window_ceiling = ids.ceiling();
        writeback.join();
        rig->store().SetAfterWritebackCopyForTest(nullptr);
        // The crash, right after the carve returned: nothing synced since.
        std::filesystem::remove_all(image);
        ASSERT_TRUE(rig->Snapshot(image, /*flush_log=*/false).ok());
        EXPECT_GE(CeilingIn(image), window_ceiling)
            << "round " << round << ": the carved window's ceiling did not reach page 0 on disk";
    }
    std::filesystem::remove_all(image);
}

}  // namespace
}  // namespace kds::server
