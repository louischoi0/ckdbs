#pragma once

// **A crash on a file-backed rig, and the mount that recovers it** - the
// helpers every crash cell over `TwoCoreRig` shares
// (`insert_log_crash_rig_test.cpp`, `sorted_leaf_crash_test.cpp`; the reply
// helpers also `sorted_leaf_rig_test.cpp`): a cell
// runs statements on a `file_backed` rig, takes what a crash at that instant
// leaves (`TwoCoreRig::Snapshot`) and brings it up through production's
// mount (`Expeditor::Open`).

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "two_core_rig.hpp"

#include "kds/server/expeditor.hpp"

namespace kds::server::crash_rig {

inline bool StartsWith(const std::string& s, std::string_view prefix) {
    return s.rfind(prefix, 0) == 0;
}

// A directory under the system temp dir, removed with its contents.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = std::filesystem::temp_directory_path() /
               ("kds_crash_rig_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline std::unique_ptr<TwoCoreRig> OpenFileRig() {
    TwoCoreRig::Options options;
    options.file_backed = true;
    auto opened = TwoCoreRig::Open(options);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

// Production's mount over a snapshot: the recovery that runs is the one a
// restart runs.
inline StatusOr<std::unique_ptr<Expeditor>> Mount(const std::filesystem::path& snapshot) {
    Expeditor::Config config;
    config.data_file = (snapshot / "kds.db").string();
    config.wal_dir = (snapshot / "wal").string();
    config.log_file = {};
    config.buffer_pool_frames = 4096;  // required (BE-R3)
    config.cores = 2;
    config.debug_text_port = 0;
    return Expeditor::Open(config, /*now_unix_seconds=*/2000);
}

inline void Ok(CommandDispatcher& d, const std::string& sql, Session* session = nullptr) {
    const std::string reply = d.Dispatch(sql, session).response;
    ASSERT_FALSE(StartsWith(reply, "ERR")) << sql << " -> " << reply;
}

// The ids a `SELECT` answers, in reply order: `id\n1\n2`, header first.
inline std::vector<std::uint64_t> Ids(CommandDispatcher& d, const std::string& sql) {
    std::vector<std::uint64_t> out;
    const std::string reply = d.Dispatch(sql).response;
    EXPECT_FALSE(StartsWith(reply, "ERR")) << sql << " -> " << reply;
    std::size_t at = reply.find("\\n");
    while (at != std::string::npos) {
        const std::size_t next = reply.find("\\n", at + 2);
        out.push_back(std::stoull(reply.substr(
            at + 2, next == std::string::npos ? std::string::npos : next - at - 2)));
        at = next;
    }
    return out;
}

// The leaf count `DESCRIBE` reports for `table`.
inline int Leaves(CommandDispatcher& d, const std::string& table) {
    const std::string shape = d.Dispatch("DESCRIBE " + table).response;
    const std::size_t at = shape.find("leaves=");
    EXPECT_NE(at, std::string::npos) << shape;
    return at == std::string::npos ? -1 : std::atoi(shape.c_str() + at + 7);
}

}  // namespace kds::server::crash_rig
