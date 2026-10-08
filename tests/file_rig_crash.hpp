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
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "two_core_rig.hpp"

#include "kds/server/expeditor.hpp"
#include "kds/wal/record.hpp"

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

// ---- The log, cut at a record boundary ------------------------------------
//
// Shared by the crash cells that cut one statement's log at each record it
// added (BD-S1's `sorted_leaf_crash_test.cpp`, BH-S2's
// `purge_key_crash_test.cpp`).

struct Segment {
    std::filesystem::path file;
    std::vector<std::uint64_t> starts;  // file offset of each record
    std::vector<wal::RecordType> types;
    std::vector<std::uint64_t> txn_ids;  // each record's envelope
    std::uint64_t end = 0;               // file offset just past the last
};

// The one segment file in `wal_dir`, and where each record in it starts.
inline std::optional<Segment> ReadSegment(const std::filesystem::path& wal_dir) {
    Segment seg;
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir)) {
        if (!entry.is_regular_file()) continue;
        seg.file = entry.path();
        ++files;
    }
    if (files != 1) {
        ADD_FAILURE() << wal_dir << " holds " << files << " files; a cell's log fits one segment";
        return std::nullopt;
    }
    std::ifstream in(seg.file, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() < wal::kSegmentHeaderSize) {
        ADD_FAILURE() << seg.file << " is shorter than a segment header";
        return std::nullopt;
    }
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()),
                                           raw.size());
    auto header = wal::DecodeSegmentHeader(bytes);
    if (!header.ok()) {
        ADD_FAILURE() << header.status().message();
        return std::nullopt;
    }
    // Each record carries its own stream offset, and the reader ends at one
    // that disagrees with where it sits: the base is the segment's own.
    const wal::Lsn base = header.value().start_lsn + wal::kSegmentHeaderSize;
    wal::RecordReader reader(bytes.subspan(wal::kSegmentHeaderSize), base);
    for (;;) {
        const std::uint64_t at = reader.end_lsn() - base;
        const std::optional<wal::DecodedRecord> record = reader.Next();
        if (!record) break;
        seg.starts.push_back(wal::kSegmentHeaderSize + at);
        seg.types.push_back(record->type());
        seg.txn_ids.push_back(record->header.txn_id);
    }
    seg.end = wal::kSegmentHeaderSize + (reader.end_lsn() - base);
    return seg;
}

// `before`'s data file beside `after`'s log, the log ending at file offset
// `cut`.
inline void CopyCut(const std::filesystem::path& before, const std::filesystem::path& after,
                    const std::filesystem::path& to, std::uint64_t cut) {
    std::filesystem::copy(after / "wal", to / "wal", std::filesystem::copy_options::recursive);
    std::filesystem::copy_file(before / "kds.db", to / "kds.db");
    const std::optional<Segment> seg = ReadSegment(to / "wal");
    ASSERT_TRUE(seg.has_value());
    std::fstream f(seg->file, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(cut));
    const std::vector<char> zeros(seg->end - cut, 0);
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
}

// Every boundary the records `after` adds over `before` - the statement's
// own records, cut after each, and the whole log - mounted and asked
// `check`, with whether the statement's commit survived the cut.
template <typename Check>
inline void CutEverywhere(const std::filesystem::path& before,
                          const std::filesystem::path& after, Check check) {
    const std::optional<Segment> head = ReadSegment(before / "wal");
    const std::optional<Segment> seg = ReadSegment(after / "wal");
    ASSERT_TRUE(head.has_value() && seg.has_value());
    const std::size_t first = head->starts.size();
    ASSERT_GT(seg->starts.size(), first + 1) << "the statement logged fewer than two records";
    bool committed = false;
    for (std::size_t k = first + 1; k <= seg->starts.size(); ++k) {
        committed = committed || seg->types[k - 1] == wal::RecordType::kTxnCommit;
        TempDir cut;
        CopyCut(before, after, cut.path, k < seg->starts.size() ? seg->starts[k] : seg->end);
        const std::string where = "log cut after record " + std::to_string(k - first) + " of " +
                                  std::to_string(seg->starts.size() - first) + " (" +
                                  wal::RecordTypeName(seg->types[k - 1]) + ")";
        auto mounted = Mount(cut.path);
        ASSERT_TRUE(mounted.ok()) << where << ": the mount refused: "
                                  << mounted.status().message();
        check(*mounted.value(), committed, where);
    }
}

}  // namespace kds::server::crash_rig
