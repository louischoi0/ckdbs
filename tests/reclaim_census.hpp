#pragma once

// The page census BF's reclaim cells share (`drop_table_reclaim_test.cpp`,
// `in_run_reclaim_test.cpp`): fill a relation, read its oid and root from
// `sys.tables`, and list the pages stamped with an owner. And the mount
// fixture the cells that restart a volume share
// (`drop_table_reclaim_test.cpp`, `catalog_page_ceiling_test.cpp`): a
// production mount, a clean stop's checkpoint, the reclaim driven by hand,
// and a crash image.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/command_dispatcher.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/page_header.hpp"
#include "view_text.hpp"

namespace kds::reclaim_census {

// `rows` rows into `t (id int64, v varchar)`, the tuple key issued, 100 a statement.
inline void Fill(server::CommandDispatcher& d, const std::string& table, int rows) {
    for (int done = 0; done < rows;) {
        std::string sql = "INSERT INTO " + table + " VALUES ";
        for (int k = 0; k < 100 && done < rows; ++k, ++done) {
            if (k > 0) sql += ", ";
            sql += "('row" + std::to_string(done) + "')";
        }
        const std::string reply = d.Dispatch(sql).response;
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
}

// The relation's `sys.tables` column `key` (`oid`, `root_page_id`).
inline std::uint64_t DescribeField(server::CommandDispatcher& d, const std::string& table,
                                   const std::string& key) {
    const auto value = testing_view::RelationField(d, "tables", table, key);
    EXPECT_TRUE(value.has_value()) << table << "." << key;
    return value.has_value() ? std::strtoull(value->c_str(), nullptr, 10) : 0;
}

// Every allocated id at or above the first user page, with its header's
// `owner_oid` (0 for a headerless page).
inline std::vector<std::pair<PageId, std::uint64_t>> AllocatedPages(
    storage::DevicePageStore& store) {
    std::vector<std::pair<PageId, std::uint64_t>> out;
    const PageId end = server::kFirstUserPageId + 4 * store.allocated_pages() + 1024;
    for (PageId id = server::kFirstUserPageId; id < end; ++id) {
        if (!store.IsAllocated(id) || storage::IsMapPageId(id)) continue;
        if (store.IsHeaderless(id)) {
            out.emplace_back(id, 0);
            continue;
        }
        auto page = store.GetForRead(id);
        EXPECT_TRUE(page.ok()) << id << ": " << page.status().message();
        out.emplace_back(id, page.ok() ? storage::GetOwnerOid(page.value().bytes()) : 0);
    }
    return out;
}

// The ids of the pages stamped with `oid`.
inline std::set<PageId> PagesOf(storage::DevicePageStore& store, std::uint64_t oid) {
    std::set<PageId> out;
    for (const auto& [id, owner] : AllocatedPages(store)) {
        if (owner == oid) out.insert(id);
    }
    return out;
}

// A mount of the volume in `dir`, with no cadence checkpoint: the only
// checkpoints are the mount's completion checkpoint and the ones a cell
// asks for, so a cut lands where the cell put it.
inline StatusOr<std::unique_ptr<server::Expeditor>> Mount(
    const std::filesystem::path& dir, std::uint32_t cores = 1,
    wal::DurabilityClass durability = server::Expeditor::Config{}.durability,
    bool waystone_recording = server::Expeditor::Config{}.waystone_recording) {
    server::Expeditor::Config config;
    config.waystone_recording = waystone_recording;
    config.buffer_pool_frames = 4096;  // required (BE-R3)
    config.data_file = (dir / "kds.db").string();
    config.wal_dir = (dir / "wal").string();
    config.log_file = {};
    config.cores = cores;
    config.debug_text_port = 0;
    config.checkpoint_interval_ns = 0;
    config.durability = durability;
    return server::Expeditor::Open(config, /*now_unix_seconds=*/1000);
}

// What a clean stop does (`CoreRuntime::ShutdownCheckpoint`): the log, then
// pages, so the checkpoint's dirty table is empty and the anchor's redo
// start is the checkpoint itself. `Checkpoint()` alone starts redo at the
// oldest dirty page's recLSN, inside the run, so the next mount would
// replay the run and raise the allocation floor past every page it names.
inline void CleanShutdownCheckpoint(server::Expeditor& db) {
    EXPECT_TRUE(db.wal().Flush().ok());
    EXPECT_TRUE(db.store().Sync().ok());
    EXPECT_TRUE(db.Checkpoint().ok());
}

// **What a mount does with a pending reclaim before a cell looks**: core
// 0's reclaim tick (BF-R8), driven by hand until no tombstone is owed or one
// is refused. BF-S1 wrote this as a no-op and the cells red against it.
inline void Settle(server::Expeditor& db) {
    for (int step = 0; step < 100000; ++step) {
        if (db.reclaim_counters().pending.load() == 0 ||
            db.reclaim_counters().refused.load() != 0) {
            break;
        }
        ASSERT_TRUE(db.ReclaimStep().ok());
    }
}

// A crash at this instant: the data file and the log copied as they are.
// Nothing is synced first, so only what the store and the log have already
// written is in the copy.
inline void CrashImage(const std::filesystem::path& from, const std::filesystem::path& to) {
    namespace fs = std::filesystem;
    fs::create_directories(to);
    fs::copy_file(from / "kds.db", to / "kds.db", fs::copy_options::overwrite_existing);
    fs::copy(from / "wal", to / "wal",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

}  // namespace kds::reclaim_census
