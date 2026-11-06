#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/ash_sampler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/ash_record.hpp"
#include "kds/stats/wait_event.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "view_text.hpp"

// CJ's WE11: every query of `tools/ash_panel.sql` runs against the engine and
// answers what the panel reads from it.

#ifndef KDS_SOURCE_DIR
#error "KDS_SOURCE_DIR is set by tests/CMakeLists.txt"
#endif

namespace kds::server {
namespace {

// name -> SQL, from the file's `-- name:` blocks; other `--` lines are prose.
std::map<std::string, std::string> ReadQueries() {
    std::ifstream in(std::string(KDS_SOURCE_DIR) + "/tools/ash_panel.sql");
    EXPECT_TRUE(in.good());
    std::map<std::string, std::string> out;
    std::string line, name, sql;
    while (std::getline(in, line)) {
        if (line.rfind("-- name: ", 0) == 0) {
            name = line.substr(9);
            sql.clear();
        } else if (line.rfind("--", 0) == 0 || name.empty()) {
            continue;
        } else {
            sql += (sql.empty() ? "" : " ") + line;
            if (!line.empty() && line.back() == ';') {
                sql.pop_back();
                out[name] = sql;
                name.clear();
            }
        }
    }
    return out;
}

std::string Fill(std::string sql, const std::string& key, const std::string& value) {
    for (std::size_t at; (at = sql.find(key)) != std::string::npos;) sql.replace(at, key.size(), value);
    return sql;
}

TEST(AshPanelQueries, EveryQueryRunsAndReadsTheSamplesTheEngineHolds) {
    storage::InMemoryPageStore store{kFirstUserPageId};
    auto boot = bootstrap::BootstrapDatabase(store, 1000);
    ASSERT_TRUE(boot.ok());
    CommandDispatcher d(boot.value().superblock, boot.value().catalog, store);
    const auto run = [&](const std::string& sql) { return d.Dispatch(sql).response; };

    sched::AshSampler sampler;
    sampler.set_core_id(59);
    stats::AshRecord r;
    const auto record = [&](std::uint64_t ts, stats::WaitEvent e, std::uint64_t fp) {
        r.sample_ts_us = ts;
        r.core_id = 59;
        r.event_id = static_cast<std::uint16_t>(e);
        r.wait_class = static_cast<std::uint8_t>(stats::Def(e).wait_class);
        r.fingerprint = fp;
        r.state = 2;  // WAITING
        sampler.ring()->Record(r);
    };
    for (std::uint64_t t = 1; t <= 4; ++t) record(t, stats::WaitEvent::kCommitWalFlush, 0xABCDEF);
    record(5, stats::WaitEvent::kIoPageRead, 0xABCDEF);
    record(5, stats::WaitEvent::kIoPageRead, 0);

    const auto queries = ReadQueries();
    ASSERT_EQ(queries.size(), 3u);
    const auto go = [&](const std::string& name, const char* since = "0") {
        return testing_view::ReadViewText(run(Fill(queries.at(name), ":since_us", since)));
    };
    // DESC and LIMIT both hold: one row, the newest.
    EXPECT_EQ(go("newest_sample").Column("sample_ts_us"), std::vector<std::string>{"5"});
    // Six samples, in time order, each carrying class, event and fingerprint.
    const auto samples = go("samples");
    EXPECT_EQ(samples.columns, (std::vector<std::string>{"sample_ts_us", "class", "event", "fingerprint"}));
    EXPECT_EQ(samples.Column("sample_ts_us"), (std::vector<std::string>{"1", "2", "3", "4", "5", "5"}));
    ASSERT_FALSE(samples.rows.empty());
    EXPECT_EQ(samples.rows.front(), (std::vector<std::string>{"1", "Commit", "commit.wal_flush", "11259375"}));
    EXPECT_EQ(samples.Find("fingerprint", "0", "event"), "io.page_read");
    EXPECT_EQ(samples.Find("fingerprint", "0", "class"), "User I/O");
    // `:since_us` is the window's inclusive lower bound.
    EXPECT_EQ(go("samples", "4").Column("sample_ts_us"), (std::vector<std::string>{"4", "5", "5"}));
    // The all-core totals: one row per event (a per-core row would repeat
    // one), largest total first.
    const auto waits = go("cumulative_waits");
    EXPECT_TRUE(waits.Find("event", "cpu.on_cpu", "class").has_value());
    const auto events = waits.Column("event");
    EXPECT_EQ(std::set<std::string>(events.begin(), events.end()).size(), events.size());
    const auto totals = waits.Column("total_us");
    for (std::size_t i = 1; i < totals.size(); ++i) {
        EXPECT_GE(std::stoull(totals[i - 1]), std::stoull(totals[i])) << i;
    }
}

}  // namespace
}  // namespace kds::server
