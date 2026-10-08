#pragma once

// The page census BF's reclaim cells share (`drop_table_reclaim_test.cpp`,
// `in_run_reclaim_test.cpp`): fill a relation, read its oid and root from
// `DESCRIBE`, and list the pages stamped with an owner.

#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/command_dispatcher.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::reclaim_census {

// `rows` rows into `t (id int64, v varchar)`, the pk issued, 100 a statement.
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

// The relation's `key=` field (`oid`, `root_page_id`), from `DESCRIBE`.
inline std::uint64_t DescribeField(server::CommandDispatcher& d, const std::string& table,
                                   const std::string& key) {
    const std::string shape = d.Dispatch("DESCRIBE " + table).response;
    const std::string field = key + "=";
    std::size_t at = shape.rfind(field, 0) == 0 ? 0 : shape.find(" " + field);
    EXPECT_NE(at, std::string::npos) << shape;
    if (at == std::string::npos) return 0;
    if (at != 0) ++at;
    return std::strtoull(shape.c_str() + at + field.size(), nullptr, 10);
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

}  // namespace kds::reclaim_census
